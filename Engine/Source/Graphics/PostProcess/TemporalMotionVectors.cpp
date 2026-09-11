#include "Smile/Graphics/PostProcess/TemporalMotionVectors.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Debug/GpuProfiler.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"
#include "Smile/Scene/Scene.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        ComPtr<ID3D12Resource> CreateTexture2D(ID3D12Device* Device, u32 Width, u32 Height,
                                               DXGI_FORMAT Format, const char* Label = nullptr) {
            return GpuResources::CreateTex2D(Device, Width, Height, Format,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI,
                                             nullptr, 1, 1, Label);
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* Device, UINT64 Size, u8** Mapped) {
            // Alinhamento de CB desligado: aqui o chamador ja passa o tamanho total e fatia
            // o mapeado na mao pelo FrameSlot.
            GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(Device, Size, 1, /*ForConstantBuffer*/ false);
            *Mapped = Upload.Mapped;
            return Upload.Resource;
        }
    }

    void FTemporalMotionVectors::Initialize(ID3D12Device* Device) {
        CreatePipelines(Device);
        CB = CreateUploadBuffer(Device,
            static_cast<UINT64>(kFrames) * sizeof(FTemporalMotionConstants), &MappedCB);
        Initialized = true;
    }

    void FTemporalMotionVectors::ReleaseResize(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        for (u32 i = 0; i < kFrames; ++i) {
            Free(SurfaceSRVSlot[i], 1);
            Free(SurfaceUAVSlot[i], 1);
            Free(SurfaceTable[i], 3);
            Free(DualTable[i], 10);
            Surface[i].Reset();
            SurfaceState[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        Free(ReliableMotionSRV, 1);
        Free(ReliableMotionUAV, 1);
        ReliableMotion.Reset();
        ReliableMotionState = D3D12_RESOURCE_STATE_COMMON;
        Width = Height = 0;
        Ready = false;
        HistoryValid = false;
    }

    void FTemporalMotionVectors::ReleaseScene(FTextureSRVHeap& SRVHeap) {
        ReleaseResize(SRVHeap);
        for (u32& Slot : TransformSRVSlot) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, 1); Slot = kInvalidSlot; }
        }
        TransformBuffer.Reset();
        MappedTransforms = nullptr;
        for (u32 f = 0; f < kFrames; ++f) {
            if (IdentitySRVSlot[f] != kInvalidSlot) {
                SRVHeap.Free(IdentitySRVSlot[f], 1);
                IdentitySRVSlot[f] = kInvalidSlot;
            }
            IdentityTable[f].Reset();
            IdentityState[f] = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        IdentityStaging = GpuResources::FUploadBuffer{};
        if (SlotRemapSRVSlot != kInvalidSlot) {
            SRVHeap.Free(SlotRemapSRVSlot, 1);
            SlotRemapSRVSlot = kInvalidSlot;
        }
        SlotRemap.Reset();
        SlotRemapStaging = GpuResources::FUploadBuffer{};
        SlotRemapState   = D3D12_RESOURCE_STATE_COPY_DEST;
        SlotCapacity_    = 0;
        PreviousCount_   = 0;
        InstanceCount_ = 0;
        TlasSRVSlot = InstanceGeoSRVSlot = kInvalidSlot;
        SceneReady = false;
    }

    void FTemporalMotionVectors::SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                                const FScene& Scene, u32 TlasSlot,
                                                u32 InstanceGeoSlot) {
        if (!Initialized) return;
        ReleaseScene(SRVHeap);
        const u32 Count = static_cast<u32>(Scene.Renderables().size());
        if (Count == 0 || TlasSlot == kInvalidSlot || InstanceGeoSlot == kInvalidSlot) return;

        // Capacidade com folga (SceneCapacityFor), nao o tamanho exato da cena: assim um objeto
        // criado no editor cabe sem refazer buffer e SRVs. InstanceCount_ passa a ser a
        // CAPACIDADE, e e ele que o UpdatePerFrame usa como stride do slice por frame.
        //
        // O historico CPU agora pertence a FGpuSceneInstanceTable; este passe guarda apenas a
        // visao DERIVADA CurrentToPrevious/PreviousToCurrent que evita inversao por pixel.
        const u32 Capacity = SceneCapacityFor(Count);
        InstanceCount_ = Capacity;
        TlasSRVSlot = TlasSlot;
        InstanceGeoSRVSlot = InstanceGeoSlot;
        TransformBuffer = CreateUploadBuffer(Device,
            static_cast<UINT64>(kFrames) * Capacity * sizeof(FTemporalInstanceTransformGPU),
            &MappedTransforms);

        // Tabela de identidades: uma por frame em voo, em DEFAULT, mais um staging UPLOAD com
        // um slice por frame. O custo e 8 B por instancia em cada uma -- na capacidade de 1M,
        // 8 MiB por tabela DEFAULT (16 MiB no total) e mais 16 MiB de staging.
        {
            const u64 TableBytes =
                static_cast<u64>(Capacity) * sizeof(FGpuIdentityEntry);
            for (u32 f = 0; f < kFrames; ++f) {
                IdentityTable[f] = GpuResources::CreateBuffer(Device, TableBytes,
                                                D3D12_RESOURCE_FLAG_NONE,
                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                EVramCategory::RenderTargets,
                                                "TemporalIdentityTable");
                IdentityState[f] = D3D12_RESOURCE_STATE_COPY_DEST;

                IdentitySRVSlot[f] = SRVHeap.Allocate(1);
                D3D12_SHADER_RESOURCE_VIEW_DESC S{};
                S.Format                     = DXGI_FORMAT_UNKNOWN;
                S.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                S.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                S.Buffer.FirstElement        = 0;
                S.Buffer.NumElements         = Capacity;
                S.Buffer.StructureByteStride = sizeof(FGpuIdentityEntry);
                SRVHeap.CreateSRV(Device, IdentityTable[f].Get(), S, IdentitySRVSlot[f]);
            }
            IdentityStaging = GpuResources::CreateUploadBuffer(Device, TableBytes, kFrames);
        }

        // Remap slot -> indice atual. Dimensionado pelo MAIOR entre a capacidade de slots e a
        // da cena: os slots incluem os APOSENTADOS, entao o pool pode passar da contagem viva
        // sem que a cena tenha crescido.
        //
        // Se ainda assim o pool crescer alem disto, o shader rejeita todo slot >= SlotCapacity_
        // e cai no fallback -- degrada para sem historico, que e o desfecho seguro, e nunca
        // para o objeto errado.
        {
            SlotCapacity_ = SlotRemapCapacityFor(Capacity, Scene.GpuSlots().Capacity());
            const u64 RemapBytes = static_cast<u64>(SlotCapacity_) * sizeof(u32);
            SlotRemap = GpuResources::CreateBuffer(Device, RemapBytes,
                                                  D3D12_RESOURCE_FLAG_NONE,
                                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                                  EVramCategory::RenderTargets,
                                                  "TemporalSlotRemap");
            SlotRemapState = D3D12_RESOURCE_STATE_COPY_DEST;

            SlotRemapSRVSlot = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format                     = DXGI_FORMAT_UNKNOWN;
            S.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            S.Buffer.FirstElement        = 0;
            S.Buffer.NumElements         = SlotCapacity_;
            S.Buffer.StructureByteStride = sizeof(u32);
            SRVHeap.CreateSRV(Device, SlotRemap.Get(), S, SlotRemapSRVSlot);

            SlotRemapStaging = GpuResources::CreateUploadBuffer(Device, RemapBytes, kFrames);
        }

        for (u32 f = 0; f < kFrames; ++f) {
            TransformSRVSlot[f] = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format                     = DXGI_FORMAT_UNKNOWN;
            S.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            S.Buffer.FirstElement        = static_cast<UINT64>(f) * Capacity;
            S.Buffer.NumElements         = Capacity;
            S.Buffer.StructureByteStride = sizeof(FTemporalInstanceTransformGPU);
            SRVHeap.CreateSRV(Device, TransformBuffer.Get(), S, TransformSRVSlot[f]);
        }

        SceneReady = true;
    }

    void FTemporalMotionVectors::SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                                 u32 NewWidth, u32 NewHeight,
                                                 u32 DepthSlot, u32 VelocitySlot,
                                                 u32 InstanceMetaSlot,
                                                 u32 InstanceTransformSlot,
                                                 u32 InstancePrevTransformSlot) {
        if (!Initialized) return;
        ReleaseResize(SRVHeap);
        if (!SceneReady || NewWidth == 0 || NewHeight == 0 ||
            DepthSlot == kInvalidSlot || VelocitySlot == kInvalidSlot ||
            InstanceMetaSlot == kInvalidSlot || InstanceTransformSlot == kInvalidSlot ||
            InstancePrevTransformSlot == kInvalidSlot) return;

        Width = NewWidth; Height = NewHeight;
        constexpr DXGI_FORMAT SurfaceFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constexpr DXGI_FORMAT MotionFormat  = DXGI_FORMAT_R16G16_FLOAT;
        for (u32 i = 0; i < kFrames; ++i)
            Surface[i] = CreateTexture2D(Device, Width, Height, SurfaceFormat,
                                         "Historico de superficie");
        ReliableMotion = CreateTexture2D(Device, Width, Height, MotionFormat,
                                         "Motion confiavel");

        auto MakeViews = [&](ID3D12Resource* Resource, DXGI_FORMAT Format, u32& SRV, u32& UAV) {
            SRV = SRVHeap.Allocate(1); UAV = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format = Format;
            S.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            S.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.Texture2D.MipLevels = 1;
            SRVHeap.CreateSRV(Device, Resource, S, SRV);
            D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
            U.Format = Format;
            U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            SRVHeap.CreateUAV(Device, Resource, U, UAV);
        };
        for (u32 i = 0; i < kFrames; ++i)
            MakeViews(Surface[i].Get(), SurfaceFormat, SurfaceSRVSlot[i], SurfaceUAVSlot[i]);
        MakeViews(ReliableMotion.Get(), MotionFormat, ReliableMotionSRV, ReliableMotionUAV);

        // O tamanho vem do PROPRIO array, e nao de um contador ao lado.
        //
        // Antes eram Src[4]/Counts[4] com o Count passado separado, e crescer um range de 4 para 7
        // escrevia fora da pilha sem nada reclamar -- os testes desta area sao de CPU pura e nao
        // criam device, entao nao havia quem pegasse. Com a referencia ao array, os dois nao tem
        // como divergir.
        auto CopyTable = [&](u32 DstSlot, const auto& Slots) {
            constexpr size_t N = std::tuple_size_v<std::remove_cvref_t<decltype(Slots)>>;
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[N]{};
            UINT Counts[N]{};
            for (size_t i = 0; i < N; ++i) {
                Src[i] = SRVHeap.CpuHandleStaging(Slots[i]);
                Counts[i] = 1;
            }
            UINT DstCount = static_cast<UINT>(N);
            Device->CopyDescriptors(1, &Dst, &DstCount, static_cast<UINT>(N), Src, Counts,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        for (u32 f = 0; f < kFrames; ++f) {
            SurfaceTable[f] = SRVHeap.Allocate(3);
            const std::array<u32, 3> SurfaceSlots{ DepthSlot, TlasSRVSlot, InstanceGeoSRVSlot };
            CopyTable(SurfaceTable[f], SurfaceSlots);

            // t4/t5 seguem a mesma alternancia das superficies: o frame le a tabela ANTERIOR
            // para traduzir o id antigo e a ATUAL para confirmar que a traducao aterrissou no
            // mesmo objeto.
            DualTable[f] = SRVHeap.Allocate(10);
            const std::array<u32, 10> DualSlots{
                VelocitySlot, SurfaceSRVSlot[f], SurfaceSRVSlot[1u - f], TransformSRVSlot[f],
                IdentitySRVSlot[1u - f], IdentitySRVSlot[f], SlotRemapSRVSlot,
                InstanceMetaSlot, InstanceTransformSlot, InstancePrevTransformSlot };
            CopyTable(DualTable[f], DualSlots);
        }

        Ready = true;
        HistoryValid = false;
    }

    void FTemporalMotionVectors::UpdatePerFrame(u32 NewFrameSlot, const FScene& Scene,
                                                 const FGpuSceneInstanceTable& Instances,
                                                 const Mat44& InvViewProj,
                                                 const Mat44& ViewProj,
                                                 const Mat44& PrevViewProj,
                                                 const Vec3& CameraPos) {
        FrameSlot = NewFrameSlot % kFrames;
        if (!Ready || !MappedTransforms || !MappedCB) return;

        const u32 Count = std::min(InstanceCount_, static_cast<u32>(Scene.Renderables().size()));
        auto* Dst = reinterpret_cast<FTemporalInstanceTransformGPU*>(MappedTransforms) +
                    static_cast<size_t>(FrameSlot) * InstanceCount_;
        // O que deixou de ser posicional aqui e o HISTORICO de CPU. O DESTINO continua indexado
        // pelo InstanceID da TLAS, que hoje e o indice na lista: o TemporalSurface.cs.hlsl
        // (linhas 52-53) grava CommittedInstanceID() + 1 na superficie, e o DualMotion le esse da
        // ANTERIOR para indexar InstanceTransforms do frame ATUAL.
        //
        // IDs antigos sao resolvidos pela tabela anterior + remap atual + generation,
        // compartilhados por DualMotion, ReflectionMotion e ReSTIRDI. A posicao abaixo
        // continua sendo so o endereco dos transforms DESTE frame.
        for (u32 i = 0; i < Count; ++i) {
            const FRenderable& R       = Scene.Renderables()[i];
            Mat44 Current = R.Transform.Matrix();
            Mat44 Previous = Current;
            if (!Scene.GpuSlots().IsAlive(R.RaytracingHandle())) {
                Dst[i].CurrentToPrevious = Mat44::Identity();
                Dst[i].PreviousToCurrent = Mat44::Identity();
                continue;
            }

            // Sem historico daquele objeto, Previous = Current, o que zera o movimento DO
            // OBJETO -- nao o motion vector de tela, que continua carregando o movimento da
            // camera. E o desfecho certo para quem acabou de nascer ou cujo slot foi
            // reciclado: herdar o transform do ocupante anterior inventaria um passado.
            if (HistoryValid) Instances.TransformsFor(R.GpuHandle, Current, Previous);

            // Pela funcao compartilhada: e ela que o teste de paridade compara com o espelho HLSL,
            // e uma copia solta aqui deixaria a que roda no frame divergir das duas.
            Dst[i] = ComposeTemporalInstanceTransform(Current, Previous);
        }

        // Tabela de identidades DESTE frame, no staging. A CPU escreve aqui e nunca na memoria
        // que os shaders consultam -- a copia para DEFAULT e ordenada na GPU (ver
        // RecordIdentityUpload).
        //
        // O indice e o mesmo InstanceID que a TLAS carrega, que hoje e a posicao na lista; o
        // conteudo e a residencia daquele objeto. E essa indirecao que deixa um id gravado no
        // frame passado ser resolvido pela tabela DAQUELE frame, em vez de reinterpretado pela
        // lista de agora.
        if (IdentityStaging) {
            auto* Table = reinterpret_cast<FGpuIdentityEntry*>(
                IdentityStaging.Slice(FrameSlot % kFrames));
            BuildIdentityTable(Table, InstanceCount_, Count, [&](u32 i) {
                const auto Handle = Scene.Renderables()[i].RaytracingHandle();
                return Scene.GpuSlots().IsAlive(Handle) ? Handle : FGpuSceneHandle{};
            });
        }

        // Remap do frame: slot -> indice na lista. kInvalidSlot marca slot sem ocupante na
        // lista de agora (removido, aposentado, ou nunca usado); o shader o reprova pela
        // comparacao com a contagem.
        if (SlotRemapStaging) {
            auto* Remap = reinterpret_cast<u32*>(
                SlotRemapStaging.Slice(FrameSlot % kFrames));
            BuildSlotRemap(Remap, SlotCapacity_, Count, [&](u32 i) {
                const auto Handle = Scene.Renderables()[i].RaytracingHandle();
                return Scene.GpuSlots().IsAlive(Handle) ? Handle : FGpuSceneHandle{};
            });
        }
        for (u32 i = Count; i < InstanceCount_; ++i) {
            Dst[i].CurrentToPrevious = Mat44::Identity();
            Dst[i].PreviousToCurrent = Mat44::Identity();
        }

        CPU.InvViewProj = InvViewProj;
        CPU.ViewProj = ViewProj;
        CPU.PrevViewProj = PrevViewProj;
        CPU.CameraPos = { CameraPos.X, CameraPos.Y, CameraPos.Z, 0.0f };
        CPU.ScreenParams = { static_cast<f32>(Width), static_cast<f32>(Height),
                             1.0f / static_cast<f32>(Width),
                             1.0f / static_cast<f32>(Height) };
        CPU.Params = { static_cast<f32>(FrameSlot), HistoryValid ? 1.0f : 0.0f,
                       PositionRejectScale, static_cast<f32>(Count) };
        // PreviousCount_ ainda e o do frame passado aqui -- ele so avanca depois do memcpy.
        CPU.IdentityParams = FTemporalIdentityParams{
            PreviousCount_, SlotCapacity_, Count, Instances.Capacity() };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(FTemporalMotionConstants),
                    &CPU, sizeof(CPU));
        PreviousCount_ = Count;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FTemporalMotionVectors::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(FTemporalMotionConstants);
    }

    void FTemporalMotionVectors::RecordIdentityUpload(ID3D12GraphicsCommandList* _CommandList,
                                                      u32 _FrameSlot) {
        if (!SceneReady || !IdentityStaging) return;
        const u32 f = _FrameSlot % kFrames;

        // Volta para COPY_DEST antes de escrever. A tabela pode estar em SRV desde o frame em
        // que foi consumida; a barreira aqui e o que ordena a sobrescrita DEPOIS das leituras
        // ja submetidas nesta fila.
        //
        // ATENCAO: isto so cobre a FILA DIRETA. Se um consumidor em outra fila passar a ler a
        // tabela, ele precisa de dependencia explicita -- a ordenacao daqui nao o alcanca.
        if (IdentityState[f] != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = IdentityTable[f].Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = IdentityState[f];
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            _CommandList->ResourceBarrier(1, &B);
            IdentityState[f] = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        _CommandList->CopyBufferRegion(
            IdentityTable[f].Get(), 0, IdentityStaging.Get(),
            static_cast<u64>(f) * IdentityStaging.SliceBytes,
            static_cast<u64>(InstanceCount_) * sizeof(FGpuIdentityEntry));

        // O remap acompanha a tabela: mesma fila, mesmo ponto, para o consumidor ver os dois
        // coerentes entre si.
        if (SlotRemapState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER R{};
            R.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            R.Transition.pResource   = SlotRemap.Get();
            R.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            R.Transition.StateBefore = SlotRemapState;
            R.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            _CommandList->ResourceBarrier(1, &R);
            SlotRemapState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        _CommandList->CopyBufferRegion(
            SlotRemap.Get(), 0, SlotRemapStaging.Get(),
            static_cast<u64>(f) * SlotRemapStaging.SliceBytes,
            static_cast<u64>(SlotCapacity_) * sizeof(u32));

        D3D12_RESOURCE_BARRIER B[2]{};
        for (u32 i = 0; i < 2; ++i) {
            B[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            B[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        B[0].Transition.pResource = IdentityTable[f].Get();
        B[1].Transition.pResource = SlotRemap.Get();
        _CommandList->ResourceBarrier(2, B);
        IdentityState[f] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        SlotRemapState   = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    void FTemporalMotionVectors::Transition(ID3D12GraphicsCommandList* CL,
                                              ID3D12Resource* Resource,
                                              D3D12_RESOURCE_STATES& State,
                                              D3D12_RESOURCE_STATES After) {
        if (!Resource || State == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource = Resource;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = State;
        B.Transition.StateAfter = After;
        CL->ResourceBarrier(1, &B);
        State = After;
    }

    void FTemporalMotionVectors::Record(ID3D12GraphicsCommandList* CL,
                                         FTextureSRVHeap& SRVHeap,
                                         FGpuProfiler* Profiler) {
        if (!Ready || !CL) return;
        const u32 Curr = FrameSlot;
        if (Profiler) Profiler->Begin(CL, "Captura de superficie");
        Transition(CL, Surface[Curr].Get(), SurfaceState[Curr],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SurfacePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(SurfaceTable[Curr]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SurfaceUAVSlot[Curr]));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        Transition(CL, Surface[Curr].Get(), SurfaceState[Curr],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Profiler) Profiler->End(CL);

        if (Profiler) Profiler->Begin(CL, "Dual motion");
        Transition(CL, ReliableMotion.Get(), ReliableMotionState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        (UseDerivedTransforms ? DualDerivedPSO : DualPSO).Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(DualTable[Curr]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(ReliableMotionUAV));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        Transition(CL, ReliableMotion.Get(), ReliableMotionState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Profiler) Profiler->End(CL);
        HistoryValid = true;
    }

    void FTemporalMotionVectors::CreatePipelines(ID3D12Device* _Device) {
        SurfacePSO.Initialize(_Device, "TemporalSurface.cs_6_9.cso", 3, 1, true);
        DualPSO.Initialize(_Device, "DualMotion.cs_6_0.cso", 10, 1, false);
        DualDerivedPSO.Initialize(_Device, "DualMotionDerived.cs_6_0.cso", 10, 1, false);
    }


    FPassShaderStems FTemporalMotionVectors::ShaderStems() const {
        static const char* const kStems[] = { "TemporalSurface.cs", "DualMotion.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FTemporalMotionVectors::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
