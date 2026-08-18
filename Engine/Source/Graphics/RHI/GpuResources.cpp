#include "Smile/Graphics/RHI/GpuResources.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <D3D12MA/D3D12MemAlloc.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace Smile::GpuResources {
    namespace {
        D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE Type) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = Type;
            // Os demais campos (CPUPageProperty, MemoryPoolPreference, node masks) ficam em
            // UNKNOWN/0, que o D3D12 resolve a partir do Type. So adapter multi-node precisaria
            // mexer neles, e a engine e single-GPU.
            return Heap;
        }

        // Contadores atomicos e nao um mutex: sao tres somas independentes, e o caminho de
        // criacao ja e serial na pratica (o load decodifica textura em paralelo mas cria na
        // thread que grava o batch). relaxed basta — ninguem le isto para sincronizar nada,
        // so para imprimir no fim de uma fase.
        constexpr size_t kHeapClasses = static_cast<size_t>(EHeapClass::Count);
        std::atomic<u32> GCount[kHeapClasses];
        std::atomic<u64> GBytes[kHeapClasses];
        std::atomic<u64> GNanos[kHeapClasses];

        constexpr size_t kDefaultPaths = static_cast<size_t>(EDefaultCreationPath::Count);
        std::atomic<u32> GDefaultPathCount[kDefaultPaths];
        std::atomic<u64> GDefaultPathBytes[kDefaultPaths];
        std::atomic<u64> GDefaultPathNanos[kDefaultPaths];

        struct FDefaultAllocatorState {
            std::mutex                 Mutex;
            ComPtr<D3D12MA::Allocator> Allocator;
            ID3D12Device*              Device = nullptr; // identidade; Allocator segura a ref
            std::atomic<u32>           ActiveResources{ 0 };
            bool                       Accepting = false;
            bool                       ShutdownRequested = false;
            bool                       FallbackWarningLogged = false;
            bool                       NotifierSupported = true;
        };

        FDefaultAllocatorState GDefaultAllocator;

        struct FAllocationReleaseContext {
            D3D12MA::Allocation* Allocation = nullptr;
        };

        struct FCapturedDesc {
            D3D12_RESOURCE_DESC Desc;
            D3D12_HEAP_TYPE     HeapType;
        };
        std::mutex                 GCaptureMutex;
        std::vector<FCapturedDesc> GCaptured;
        std::atomic<bool>          GCaptureOn{ false };

        EHeapClass ClassOf(D3D12_HEAP_TYPE Type) {
            switch (Type) {
                case D3D12_HEAP_TYPE_UPLOAD:   return EHeapClass::Upload;
                case D3D12_HEAP_TYPE_READBACK: return EHeapClass::Readback;
                default:                       return EHeapClass::Default;
            }
        }

        void FinalizeAllocatorIfIdle() {
            if (GDefaultAllocator.ActiveResources.load(std::memory_order_acquire) != 0) return;

            std::lock_guard Lock(GDefaultAllocator.Mutex);
            if (!GDefaultAllocator.ShutdownRequested ||
                GDefaultAllocator.ActiveResources.load(std::memory_order_relaxed) != 0)
                return;

            GDefaultAllocator.Allocator.Reset();
            GDefaultAllocator.Device = nullptr;
            GDefaultAllocator.ShutdownRequested = false;
        }

        void CALLBACK ReleaseD3D12MAAllocation(void* _Data) {
            auto* Context = static_cast<FAllocationReleaseContext*>(_Data);
            D3D12MA::Allocation* Allocation = Context->Allocation;
            delete Context;
            Allocation->Release();

            const u32 Previous =
                GDefaultAllocator.ActiveResources.fetch_sub(1, std::memory_order_acq_rel);
            if (Previous == 1) FinalizeAllocatorIfIdle();
        }

        void DisablePlacedPath() {
            std::lock_guard Lock(GDefaultAllocator.Mutex);
            GDefaultAllocator.Accepting = false;
            GDefaultAllocator.NotifierSupported = false;
        }

        HRESULT TryCreatePlaced(ID3D12Device* _Device, const D3D12_RESOURCE_DESC& _Desc,
                                D3D12_RESOURCE_STATES _InitialState,
                                const D3D12_CLEAR_VALUE* _Clear,
                                ComPtr<ID3D12Resource>& _Out, bool& _Attempted) {
            ComPtr<D3D12MA::Allocator> Allocator;
            {
                std::lock_guard Lock(GDefaultAllocator.Mutex);
                if (!GDefaultAllocator.Accepting || !GDefaultAllocator.NotifierSupported ||
                    GDefaultAllocator.Device != _Device || !GDefaultAllocator.Allocator) {
                    _Attempted = false;
                    return E_NOINTERFACE;
                }
                Allocator = GDefaultAllocator.Allocator;
            }
            _Attempted = true;

            D3D12_RESOURCE_ALLOCATION_INFO Info =
                _Device->GetResourceAllocationInfo(0, 1, &_Desc);
            constexpr u64 kGranularity = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            Info.SizeInBytes = (Info.SizeInBytes + kGranularity - 1) & ~(kGranularity - 1);

            D3D12MA::ALLOCATION_DESC AllocationDesc{};
            AllocationDesc.Flags    = D3D12MA::ALLOCATION_FLAG_CAN_ALIAS;
            AllocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

            D3D12MA::Allocation* Allocation = nullptr;
            HRESULT Hr = Allocator->AllocateMemory(&AllocationDesc, &Info, &Allocation);
            if (FAILED(Hr)) return Hr;

            ComPtr<ID3D12Resource> Resource;
            Hr = Allocator->CreateAliasingResource(
                Allocation, 0, &_Desc, _InitialState, _Clear, IID_PPV_ARGS(&Resource));
            if (FAILED(Hr)) {
                Allocation->Release();
                return Hr;
            }

            auto* Context = new (std::nothrow) FAllocationReleaseContext{ Allocation };
            if (!Context) {
                Resource.Reset();
                Allocation->Release();
                return E_OUTOFMEMORY;
            }

            ComPtr<ID3DDestructionNotifier> Notifier;
            Hr = Resource.As(&Notifier);
            if (FAILED(Hr)) {
                delete Context;
                Resource.Reset();
                Allocation->Release();
                DisablePlacedPath();
                return Hr;
            }

            UINT Cookie = 0;
            Hr = Notifier->RegisterDestructionCallback(
                &ReleaseD3D12MAAllocation, Context, &Cookie);
            if (FAILED(Hr)) {
                delete Context;
                Notifier.Reset();
                Resource.Reset();
                Allocation->Release();
                DisablePlacedPath();
                return Hr;
            }

            GDefaultAllocator.ActiveResources.fetch_add(1, std::memory_order_release);
            _Out = std::move(Resource);
            return S_OK;
        }

        // Nucleo unico das duas familias. Cria num LOCAL e so entrega em sucesso — e o que
        // sustenta a promessa de "Out intacto em falha" das variantes Try.
        HRESULT TryCreateResource(ID3D12Device* Device, D3D12_HEAP_TYPE HeapType,
                                   const D3D12_RESOURCE_DESC& Desc,
                                   D3D12_RESOURCE_STATES InitialState,
                                   const D3D12_CLEAR_VALUE* Clear,
                                   ComPtr<ID3D12Resource>& Out) {
            ComPtr<ID3D12Resource> Resource;

            // O cronometro cobre o caminho de alocacao inteiro: D3D12MA + criacao placed +
            // notifier, ou CreateCommittedResource no fallback. Montar o desc e registrar no
            // VramTracker ficam de fora; assim o A/B mede exatamente o que a politica troca.
            const auto Start = std::chrono::steady_clock::now();
            bool AttemptedD3D12MA = false;
            bool UsedD3D12MA = false;
            HRESULT Hr = E_NOINTERFACE;
            // Placed RT/DS exige Clear, Discard ou Copy antes do primeiro uso. Nem todos os
            // passes antigos explicitam essa inicializacao porque committed preservava esse
            // detalhe. Mantemos esses recursos no caminho compatível nesta primeira versao;
            // buffers e texturas comuns, que concentram o load, seguem pelo pool.
            const bool ForceCommitted =
                (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 ||
                (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
            if (HeapType == D3D12_HEAP_TYPE_DEFAULT && !ForceCommitted) {
                Hr = TryCreatePlaced(Device, Desc, InitialState, Clear, Resource,
                                     AttemptedD3D12MA);
                UsedD3D12MA = SUCCEEDED(Hr);
            }
            if (!UsedD3D12MA) {
                if (AttemptedD3D12MA) {
                    bool ShouldLog = false;
                    {
                        std::lock_guard Lock(GDefaultAllocator.Mutex);
                        ShouldLog = !GDefaultAllocator.FallbackWarningLogged;
                        GDefaultAllocator.FallbackWarningLogged = true;
                    }
                    if (ShouldLog)
                        LogWarning("[D3D12MA] alocacao placed falhou; usando committed como fallback");
                }

                const D3D12_HEAP_PROPERTIES Heap = HeapProps(HeapType);
                Hr = Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                     InitialState, Clear,
                                                     IID_PPV_ARGS(&Resource));
            }
            const auto Elapsed = std::chrono::steady_clock::now() - Start;
            const u64 ElapsedNanos = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Elapsed).count());

            if (GCaptureOn.load(std::memory_order_relaxed)) {
                std::lock_guard Lock(GCaptureMutex);
                if (GCaptureOn.load(std::memory_order_relaxed))
                    GCaptured.push_back({ Desc, HeapType });
            }

            const size_t Klass = static_cast<size_t>(ClassOf(HeapType));
            GCount[Klass].fetch_add(1, std::memory_order_relaxed);
            GNanos[Klass].fetch_add(ElapsedNanos, std::memory_order_relaxed);
            const size_t Path = static_cast<size_t>(UsedD3D12MA
                ? EDefaultCreationPath::D3D12MA
                : EDefaultCreationPath::Committed);
            if (HeapType == D3D12_HEAP_TYPE_DEFAULT) {
                GDefaultPathCount[Path].fetch_add(1, std::memory_order_relaxed);
                GDefaultPathNanos[Path].fetch_add(ElapsedNanos, std::memory_order_relaxed);
            }
            if (SUCCEEDED(Hr)) {
                // Tamanho REAL cobrado (alinhamento incluso), mesma medida do VramTracker —
                // senao os dois numeros nao se comparam.
                const u64 Bytes = Device->GetResourceAllocationInfo(0, 1, &Desc).SizeInBytes;
                GBytes[Klass].fetch_add(Bytes, std::memory_order_relaxed);
                if (HeapType == D3D12_HEAP_TYPE_DEFAULT)
                    GDefaultPathBytes[Path].fetch_add(Bytes, std::memory_order_relaxed);
                Out = std::move(Resource);
            }
            return Hr;
        }

        ComPtr<ID3D12Resource> CreateResource(ID3D12Device* Device, D3D12_HEAP_TYPE HeapType,
                                               const D3D12_RESOURCE_DESC& Desc,
                                               D3D12_RESOURCE_STATES InitialState,
                                               const D3D12_CLEAR_VALUE* Clear) {
            ComPtr<ID3D12Resource> Resource;
            SMILE_HR(TryCreateResource(Device, HeapType, Desc, InitialState, Clear, Resource));
            return Resource;
        }
    }

    bool InitializeDefaultAllocator(ID3D12Device* _Device, IDXGIAdapter* _Adapter,
                                    D3D12_RESOURCE_HEAP_TIER _HeapTier) {
        char Disabled[8]{};
        const DWORD DisabledLength = GetEnvironmentVariableA(
            "SMILE_DISABLE_D3D12MA", Disabled, _countof(Disabled));
        if (DisabledLength != 0 && !(DisabledLength == 1 && Disabled[0] == '0')) {
            LogInfo("[D3D12MA] desabilitado por SMILE_DISABLE_D3D12MA; usando committed");
            return false;
        }
        if (!_Device || !_Adapter || _HeapTier < D3D12_RESOURCE_HEAP_TIER_2) {
            LogWarning("[D3D12MA] Heap Tier 2 indisponivel; usando committed");
            return false;
        }

        std::lock_guard Lock(GDefaultAllocator.Mutex);
        if (GDefaultAllocator.Allocator && GDefaultAllocator.Device == _Device) {
            GDefaultAllocator.Accepting = true;
            GDefaultAllocator.ShutdownRequested = false;
            return true;
        }
        if (GDefaultAllocator.ActiveResources.load(std::memory_order_acquire) != 0) {
            LogWarning("[D3D12MA] troca de device recusada com recursos placed ativos");
            return false;
        }

        D3D12MA::ALLOCATOR_DESC Desc{};
        Desc.Flags = D3D12MA_RECOMMENDED_ALLOCATOR_FLAGS;
        Desc.pDevice = _Device;
        Desc.pAdapter = _Adapter;

        ComPtr<D3D12MA::Allocator> Allocator;
        const HRESULT Hr = D3D12MA::CreateAllocator(&Desc, &Allocator);
        if (FAILED(Hr)) {
            LogWarning("[D3D12MA] CreateAllocator falhou; usando committed");
            return false;
        }

        GDefaultAllocator.Allocator = std::move(Allocator);
        GDefaultAllocator.Device = _Device;
        GDefaultAllocator.Accepting = true;
        GDefaultAllocator.ShutdownRequested = false;
        GDefaultAllocator.FallbackWarningLogged = false;
        GDefaultAllocator.NotifierSupported = true;
        LogInfo("[D3D12MA] pool DEFAULT ativo (Heap Tier 2)");
        return true;
    }

    void ShutdownDefaultAllocator(ID3D12Device* _Device) {
        std::lock_guard Lock(GDefaultAllocator.Mutex);
        if (!GDefaultAllocator.Allocator || GDefaultAllocator.Device != _Device) return;

        GDefaultAllocator.Accepting = false;
        GDefaultAllocator.ShutdownRequested = true;
        if (GDefaultAllocator.ActiveResources.load(std::memory_order_acquire) == 0) {
            GDefaultAllocator.Allocator.Reset();
            GDefaultAllocator.Device = nullptr;
            GDefaultAllocator.ShutdownRequested = false;
        } else {
            LogWarning("[D3D12MA] shutdown adiado ate os recursos placed ativos serem liberados");
        }
    }

    FMemoryAllocatorStats MemoryAllocatorStats() {
        ComPtr<D3D12MA::Allocator> Allocator;
        FMemoryAllocatorStats Out{};
        {
            std::lock_guard Lock(GDefaultAllocator.Mutex);
            Allocator = GDefaultAllocator.Allocator;
            Out.Enabled = GDefaultAllocator.Accepting;
            Out.ActiveResources =
                GDefaultAllocator.ActiveResources.load(std::memory_order_relaxed);
        }
        if (Allocator) {
            D3D12MA::Budget Local{};
            Allocator->GetBudget(&Local, nullptr);
            Out.BlockBytes = Local.Stats.BlockBytes;
            Out.AllocationBytes = Local.Stats.AllocationBytes;
        }
        return Out;
    }

    D3D12_RESOURCE_DESC Tex2DDesc(u32 _Width, u32 _Height, DXGI_FORMAT _Format,
                                  D3D12_RESOURCE_FLAGS _Flags, u32 _MipLevels, u32 _ArraySize) {
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = static_cast<UINT16>(_ArraySize);
        Desc.MipLevels        = static_cast<UINT16>(_MipLevels);
        Desc.Format           = _Format;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = _Flags;
        return Desc;
    }

    D3D12_RESOURCE_DESC Tex3DDesc(u32 _Width, u32 _Height, u32 _Depth, DXGI_FORMAT _Format,
                                  D3D12_RESOURCE_FLAGS _Flags, u32 _MipLevels) {
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = static_cast<UINT16>(_Depth);
        Desc.MipLevels        = static_cast<UINT16>(_MipLevels);
        Desc.Format           = _Format;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = _Flags;
        return Desc;
    }

    D3D12_RESOURCE_DESC BufferDesc(u64 _Bytes, D3D12_RESOURCE_FLAGS _Flags) {
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = _Bytes;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        // ROW_MAJOR e obrigatorio em buffer (o D3D12 rejeita UNKNOWN aqui).
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags            = _Flags;
        return Desc;
    }

    ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* _Device, u32 _Width, u32 _Height,
                                       DXGI_FORMAT _Format, D3D12_RESOURCE_FLAGS _Flags,
                                       D3D12_RESOURCE_STATES _InitialState,
                                       EVramCategory _Category,
                                       const D3D12_CLEAR_VALUE* _Clear,
                                       u32 _MipLevels, u32 _ArraySize, const char* _Label) {
        const D3D12_RESOURCE_DESC Desc =
            Tex2DDesc(_Width, _Height, _Format, _Flags, _MipLevels, _ArraySize);
        ComPtr<ID3D12Resource> Texture =
            CreateResource(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc, _InitialState, _Clear);
        VramTracker::Register(Texture.Get(), _Category, _Label);
        return Texture;
    }

    ComPtr<ID3D12Resource> CreateTex3D(ID3D12Device* _Device, u32 _Width, u32 _Height, u32 _Depth,
                                       DXGI_FORMAT _Format, D3D12_RESOURCE_FLAGS _Flags,
                                       D3D12_RESOURCE_STATES _InitialState,
                                       EVramCategory _Category, u32 _MipLevels) {
        const D3D12_RESOURCE_DESC Desc =
            Tex3DDesc(_Width, _Height, _Depth, _Format, _Flags, _MipLevels);
        ComPtr<ID3D12Resource> Texture =
            CreateResource(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc, _InitialState, nullptr);
        VramTracker::Register(Texture.Get(), _Category);
        return Texture;
    }

    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _Device, u64 _Bytes,
                                        D3D12_RESOURCE_FLAGS _Flags,
                                        D3D12_RESOURCE_STATES _InitialState,
                                        EVramCategory _Category, const char* _Label) {
        const D3D12_RESOURCE_DESC Desc = BufferDesc(_Bytes, _Flags);
        ComPtr<ID3D12Resource> Buffer =
            CreateResource(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc, _InitialState, nullptr);
        VramTracker::Register(Buffer.Get(), _Category, _Label);
        return Buffer;
    }

    HRESULT TryCreateTex2D(ID3D12Device* _Device, ComPtr<ID3D12Resource>& _Out,
                           u32 _Width, u32 _Height, DXGI_FORMAT _Format,
                           D3D12_RESOURCE_FLAGS _Flags, D3D12_RESOURCE_STATES _InitialState,
                           EVramCategory _Category, const D3D12_CLEAR_VALUE* _Clear,
                           u32 _MipLevels, u32 _ArraySize, const char* _Label) {
        const D3D12_RESOURCE_DESC Desc =
            Tex2DDesc(_Width, _Height, _Format, _Flags, _MipLevels, _ArraySize);
        const HRESULT Hr = TryCreateResource(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc,
                                              _InitialState, _Clear, _Out);
        if (SUCCEEDED(Hr)) VramTracker::Register(_Out.Get(), _Category, _Label);
        return Hr;
    }

    HRESULT TryCreateBuffer(ID3D12Device* _Device, ComPtr<ID3D12Resource>& _Out, u64 _Bytes,
                            D3D12_RESOURCE_FLAGS _Flags, D3D12_RESOURCE_STATES _InitialState,
                            EVramCategory _Category, const char* _Label) {
        const D3D12_RESOURCE_DESC Desc = BufferDesc(_Bytes, _Flags);
        const HRESULT Hr = TryCreateResource(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc,
                                              _InitialState, nullptr, _Out);
        if (SUCCEEDED(Hr)) VramTracker::Register(_Out.Get(), _Category, _Label);
        return Hr;
    }

    FUploadBuffer CreateUploadBuffer(ID3D12Device* _Device, u64 _SliceBytes, u32 _SliceCount,
                                     bool _ForConstantBuffer) {
        FUploadBuffer Out{};
        if (_SliceBytes == 0 || _SliceCount == 0) return Out;

        Out.SliceBytes = _ForConstantBuffer
            ? (_SliceBytes + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
              ~static_cast<u64>(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)
            : _SliceBytes;
        Out.SliceCount = _SliceCount;

        const D3D12_RESOURCE_DESC Desc = BufferDesc(Out.SliceBytes * _SliceCount);
        Out.Resource = CreateResource(_Device, D3D12_HEAP_TYPE_UPLOAD, Desc,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);

        // Range de leitura vazio: a CPU so escreve aqui. Sem Unmap — o mapeamento vive o
        // tempo todo do recurso, que e o ponto do upload heap persistente.
        const D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(Out.Resource->Map(0, &NoRead, reinterpret_cast<void**>(&Out.Mapped)));
        return Out;
    }

    ComPtr<ID3D12Resource> CreateReadbackBuffer(ID3D12Device* _Device, u64 _Bytes) {
        const D3D12_RESOURCE_DESC Desc = BufferDesc(_Bytes);
        return CreateResource(_Device, D3D12_HEAP_TYPE_READBACK, Desc,
                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    }

    FCreationStats CreationStats() {
        FCreationStats Out{};
        for (size_t i = 0; i < kHeapClasses; ++i) {
            Out.Count[i] = GCount[i].load(std::memory_order_relaxed);
            Out.Bytes[i] = GBytes[i].load(std::memory_order_relaxed);
            Out.Nanos[i] = GNanos[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kDefaultPaths; ++i) {
            Out.DefaultPathCount[i] = GDefaultPathCount[i].load(std::memory_order_relaxed);
            Out.DefaultPathBytes[i] = GDefaultPathBytes[i].load(std::memory_order_relaxed);
            Out.DefaultPathNanos[i] = GDefaultPathNanos[i].load(std::memory_order_relaxed);
        }
        return Out;
    }

    void ResetCreationStats() {
        for (size_t i = 0; i < kHeapClasses; ++i) {
            GCount[i].store(0, std::memory_order_relaxed);
            GBytes[i].store(0, std::memory_order_relaxed);
            GNanos[i].store(0, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kDefaultPaths; ++i) {
            GDefaultPathCount[i].store(0, std::memory_order_relaxed);
            GDefaultPathBytes[i].store(0, std::memory_order_relaxed);
            GDefaultPathNanos[i].store(0, std::memory_order_relaxed);
        }
    }

    namespace {
        void LogStatsBlock(const char* _Label, const FCreationStats& _S, bool _Verbose);
    }

    bool SetDescCapture(bool _Enabled) {
        std::lock_guard Lock(GCaptureMutex);
        if (_Enabled) {
            if (GCaptureOn.load(std::memory_order_relaxed)) {
                LogWarning("[Criacao] captura de descritores ja estava ligada; a janela "
                           "interna foi ignorada para nao corromper a externa");
                return false;
            }
            GCaptured.clear();
            GCaptureOn.store(true, std::memory_order_relaxed);
            return true;
        }
        GCaptureOn.store(false, std::memory_order_relaxed);
        return true;
    }

    bool DumpCapturedDescs(const char* _Path) {
        std::lock_guard Lock(GCaptureMutex);
        if (GCaptured.empty()) return false;

        std::ofstream File(_Path, std::ios::trunc);
        if (!File) return false;

        // Texto e nao binario: o arquivo e lido pelo AllocBench mas tambem por um humano
        // conferindo se o conjunto bate com o que o log disse.
        // v2 grava o D3D12_RESOURCE_DESC INTEIRO. A v1 omitia Alignment, SampleDesc e
        // Layout porque hoje os tres estao no default em todo recurso da engine — mas o dia
        // em que entrar MSAA (SampleDesc.Count > 1) ou um layout especial, o benchmark
        // reproduziria descritores DIFERENTES dos reais sem nada acusar. Um campo omitido
        // por ser constante hoje e uma armadilha para amanha.
        File << "# smile desc capture v2\n"
             << "# dim alignment width height deptharray mips format "
                "samplecount samplequality layout flags heaptype\n";
        for (const FCapturedDesc& C : GCaptured) {
            File << static_cast<int>(C.Desc.Dimension) << ' ' << C.Desc.Alignment << ' '
                 << C.Desc.Width << ' ' << C.Desc.Height << ' '
                 << C.Desc.DepthOrArraySize << ' ' << C.Desc.MipLevels << ' '
                 << static_cast<int>(C.Desc.Format) << ' '
                 << C.Desc.SampleDesc.Count << ' ' << C.Desc.SampleDesc.Quality << ' '
                 << static_cast<int>(C.Desc.Layout) << ' '
                 << static_cast<int>(C.Desc.Flags) << ' '
                 << static_cast<int>(C.HeapType) << '\n';
        }
        // Caminho ABSOLUTO no log: o editor e lancado por atalho ou pela IDE, e o cwd dele
        // nao e adivinhavel de fora. Sem isto sobra "o arquivo foi escrito onde?".
        std::error_code Ec;
        const std::filesystem::path Abs = std::filesystem::absolute(_Path, Ec);
        LogInfo("[Criacao] " + std::to_string(GCaptured.size()) + " descritores capturados em " +
                (Ec ? std::string(_Path) : Abs.string()));
        return true;
    }

    FDescCaptureSession::FDescCaptureSession(const char* _Path)
        : Path(_Path ? _Path : ""), Owns(SetDescCapture(true)) {}

    FDescCaptureSession::~FDescCaptureSession() noexcept {
        if (Owns) SetDescCapture(false);
    }

    bool FDescCaptureSession::Complete() {
        if (!Owns) return false;
        SetDescCapture(false);
        Owns = false;
        return !Path.empty() && DumpCapturedDescs(Path.c_str());
    }

    FCreationStats CreationDelta(const FCreationStats& _Since) {
        const FCreationStats Now = CreationStats();
        FCreationStats Delta{};
        for (size_t i = 0; i < kHeapClasses; ++i) {
            Delta.Count[i] = Now.Count[i] - _Since.Count[i];
            Delta.Bytes[i] = Now.Bytes[i] - _Since.Bytes[i];
            Delta.Nanos[i] = Now.Nanos[i] - _Since.Nanos[i];
        }
        for (size_t i = 0; i < kDefaultPaths; ++i) {
            Delta.DefaultPathCount[i] =
                Now.DefaultPathCount[i] - _Since.DefaultPathCount[i];
            Delta.DefaultPathBytes[i] =
                Now.DefaultPathBytes[i] - _Since.DefaultPathBytes[i];
            Delta.DefaultPathNanos[i] =
                Now.DefaultPathNanos[i] - _Since.DefaultPathNanos[i];
        }
        return Delta;
    }

    void LogCreationDelta(const char* _Label, const FCreationStats& _Since) {
        // Fase e ruido para quem nao esta medindo: vai em DEBUG, e o total da janela
        // continua em INFO pelo LogCreationStats.
        LogStatsBlock(_Label, CreationDelta(_Since), false);
    }

    void AccumulatePhase(FCreationStats& _InOutSum, const char* _Label,
                         const FCreationStats& _Since) {
        const FCreationStats Delta = CreationDelta(_Since);
        for (size_t i = 0; i < kHeapClasses; ++i) {
            _InOutSum.Count[i] += Delta.Count[i];
            _InOutSum.Bytes[i] += Delta.Bytes[i];
            _InOutSum.Nanos[i] += Delta.Nanos[i];
        }
        for (size_t i = 0; i < kDefaultPaths; ++i) {
            _InOutSum.DefaultPathCount[i] += Delta.DefaultPathCount[i];
            _InOutSum.DefaultPathBytes[i] += Delta.DefaultPathBytes[i];
            _InOutSum.DefaultPathNanos[i] += Delta.DefaultPathNanos[i];
        }
        LogStatsBlock(_Label, Delta, false);
    }

    void LogCreationUnattributed(const char* _Label, const FCreationStats& _SumOfPhases) {
        const FCreationStats Total = CreationStats();
        FCreationStats Rest{};
        for (size_t i = 0; i < kHeapClasses; ++i) {
            // Subtracao saturada: se alguem somar fases sobrepostas, a linha some em vez de
            // estourar por baixo num u64 e imprimir um numero absurdo.
            Rest.Count[i] = Total.Count[i] > _SumOfPhases.Count[i]
                          ? Total.Count[i] - _SumOfPhases.Count[i] : 0;
            Rest.Bytes[i] = Total.Bytes[i] > _SumOfPhases.Bytes[i]
                          ? Total.Bytes[i] - _SumOfPhases.Bytes[i] : 0;
            Rest.Nanos[i] = Total.Nanos[i] > _SumOfPhases.Nanos[i]
                          ? Total.Nanos[i] - _SumOfPhases.Nanos[i] : 0;
        }
        for (size_t i = 0; i < kDefaultPaths; ++i) {
            Rest.DefaultPathCount[i] =
                Total.DefaultPathCount[i] > _SumOfPhases.DefaultPathCount[i]
                ? Total.DefaultPathCount[i] - _SumOfPhases.DefaultPathCount[i] : 0;
            Rest.DefaultPathBytes[i] =
                Total.DefaultPathBytes[i] > _SumOfPhases.DefaultPathBytes[i]
                ? Total.DefaultPathBytes[i] - _SumOfPhases.DefaultPathBytes[i] : 0;
            Rest.DefaultPathNanos[i] =
                Total.DefaultPathNanos[i] > _SumOfPhases.DefaultPathNanos[i]
                ? Total.DefaultPathNanos[i] - _SumOfPhases.DefaultPathNanos[i] : 0;
        }
        LogStatsBlock(_Label, Rest, false);
    }

    namespace {
        void LogStatsBlock(const char* _Label, const FCreationStats& _S, bool _Verbose) {
            u32 TotalCount = 0;
            u64 TotalNanos = 0;
            for (size_t i = 0; i < kHeapClasses; ++i) {
                TotalCount += _S.Count[i];
                TotalNanos += _S.Nanos[i];
            }
            if (TotalCount == 0) return;

            // Mesma casa decimal do breakdown de VRAM, para os dois logs se lerem juntos.
            auto Ms = [](u64 Nanos) {
                const u64 Tenths = (Nanos + 50'000ull) / 100'000ull; // ns -> 0,1 ms
                return std::to_string(Tenths / 10u) + "," + std::to_string(Tenths % 10u) + " ms";
            };
            auto Mb = [](u64 Bytes) {
                const u64 Tenths = (Bytes * 10u + (1u << 19)) >> 20;
                return std::to_string(Tenths / 10u) + "," + std::to_string(Tenths % 10u) + " MB";
            };

            static const char* const kNames[kHeapClasses] = { "DEFAULT", "UPLOAD", "READBACK" };

            std::string Line = std::string("[Criacao] ") + _Label + ": " +
                               std::to_string(TotalCount) + " recursos em " + Ms(TotalNanos);

            // A quebra por classe vai nos DOIS, e nao so no total da janela. Sem ela a fase
            // de textura agrega os 406 recursos DEFAULT com os chunks UPLOAD que o ring cria
            // no meio — e aplicar a esse agregado uma constante medida so em DEFAULT (que foi
            // o que aconteceu na primeira leitura) mistura duas populacoes de custo bem
            // diferentes: UPLOAD custa ~2x por MB e vem em blocos de 64 MB.
            for (size_t i = 0; i < kHeapClasses; ++i) {
                if (_S.Count[i] == 0) continue;
                Line += _Verbose ? "\n         " : "  |  ";
                Line += std::string(kNames[i]) + ": " + std::to_string(_S.Count[i]) + " x " +
                        Mb(_S.Bytes[i]) + " em " + Ms(_S.Nanos[i]);
            }

            static const char* const kPathNames[kDefaultPaths] = {
                "committed", "D3D12MA"
            };
            for (size_t i = 0; i < kDefaultPaths; ++i) {
                if (_S.DefaultPathCount[i] == 0) continue;
                Line += _Verbose ? "\n         " : "  |  ";
                Line += std::string("DEFAULT/") + kPathNames[i] + ": " +
                        std::to_string(_S.DefaultPathCount[i]) + " x " +
                        Mb(_S.DefaultPathBytes[i]) + " em " + Ms(_S.DefaultPathNanos[i]);
            }

            if (_Verbose) {
                const FMemoryAllocatorStats Allocator = MemoryAllocatorStats();
                if (Allocator.Enabled || Allocator.ActiveResources != 0) {
                    Line += "\n         allocator: " + Mb(Allocator.BlockBytes) +
                            " em heaps, " + Mb(Allocator.AllocationBytes) + " ocupados, " +
                            std::to_string(Allocator.ActiveResources) + " recursos ativos";
                }
            }

            if (_Verbose) LogInfo(Line);
            else          LogDebug(Line);
        }
    }

    void LogCreationStats(const char* _Label) {
        LogStatsBlock(_Label, CreationStats(), true);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvTex2D(DXGI_FORMAT _Format, u32 _MipLevels,
                                             u32 _MostDetailedMip) {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                    = _Format;
        Srv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels       = _MipLevels;
        Srv.Texture2D.MostDetailedMip = _MostDetailedMip;
        return Srv;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC UavTex2D(DXGI_FORMAT _Format, u32 _MipSlice) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format             = _Format;
        Uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Texture2D.MipSlice = _MipSlice;
        return Uav;
    }

    D3D12_RENDER_TARGET_VIEW_DESC RtvTex2D(DXGI_FORMAT _Format, u32 _MipSlice) {
        D3D12_RENDER_TARGET_VIEW_DESC Rtv{};
        Rtv.Format             = _Format;
        Rtv.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
        Rtv.Texture2D.MipSlice = _MipSlice;
        return Rtv;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvStructuredBuffer(u32 _NumElements, u32 _StrideBytes,
                                                        u64 _FirstElement) {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                     = DXGI_FORMAT_UNKNOWN;
        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Buffer.FirstElement        = _FirstElement;
        Srv.Buffer.NumElements         = _NumElements;
        Srv.Buffer.StructureByteStride = _StrideBytes;
        Srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        return Srv;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC UavStructuredBuffer(u32 _NumElements, u32 _StrideBytes,
                                                         u64 _FirstElement) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                     = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.FirstElement        = _FirstElement;
        Uav.Buffer.NumElements         = _NumElements;
        Uav.Buffer.StructureByteStride = _StrideBytes;
        Uav.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;
        return Uav;
    }
}
