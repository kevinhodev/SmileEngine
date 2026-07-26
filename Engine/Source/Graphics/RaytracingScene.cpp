#include "Smile/Graphics/RaytracingScene.h"
#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        UINT64 AlignAS(UINT64 _Value) {
            constexpr UINT64 A = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
            return (_Value + A - 1) & ~(A - 1);
        }

        ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _Device, UINT64 _Size,
                                            D3D12_HEAP_TYPE _Heap, D3D12_RESOURCE_STATES _State,
                                            D3D12_RESOURCE_FLAGS _Flags) {
            D3D12_HEAP_PROPERTIES HeapProps{};
            HeapProps.Type = _Heap;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = _Size;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = _Flags;
            ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                     _State, nullptr, IID_PPV_ARGS(&Buffer)));
            VramTracker::Register(Buffer.Get(), EVramCategory::RaytracingAS);
            return Buffer;
        }

        ComPtr<ID3D12Resource> CreateUAVBuffer(ID3D12Device* _Device, UINT64 _Size,
                                               D3D12_RESOURCE_STATES _State) {
            return CreateBuffer(_Device, _Size, D3D12_HEAP_TYPE_DEFAULT, _State,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }

        void PushTransition(std::vector<D3D12_RESOURCE_BARRIER>& _Out, ID3D12Resource* _Res,
                            D3D12_RESOURCE_STATES _Before, D3D12_RESOURCE_STATES _After) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = _Res;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = _Before;
            B.Transition.StateAfter  = _After;
            _Out.push_back(B);
        }

        void GlobalUAVBarrier(ID3D12GraphicsCommandList4* _CL) {
            D3D12_RESOURCE_BARRIER UAV{};
            UAV.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UAV.UAV.pResource = nullptr;
            _CL->ResourceBarrier(1, &UAV);
        }
    }

    static_assert(FRaytracingScene::kInstanceSlots == FCommandQueue::kFramesInFlight,
                  "InstanceUpload versionado por frame em voo");

    void FRaytracingScene::Release(FTextureSRVHeap& _SRVHeap) {
        if (TlasSRVSlot_ != kInvalidSlot) { _SRVHeap.Free(TlasSRVSlot_, 1); TlasSRVSlot_ = kInvalidSlot; }
        BlasPool.Reset();
        BlasByMesh.clear();
        Tlas.Reset();
        TlasScratch.Reset();
        for (u32 s = 0; s < kInstanceSlots; ++s) {
            InstanceUpload[s].Reset();
            InstanceMapped[s] = nullptr;
        }
        InstanceCapacity = 0;
        Built          = false;
        InstanceCount_ = 0;
        BlasCount_     = 0;
    }

    void FRaytracingScene::CollectInstances(const FScene& _Scene,
                                            std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& _Out) const {
        _Out.clear();
        _Out.reserve(_Scene.Renderables().size());
        u32 Idx = 0;
        for (const FRenderable& R : _Scene.Renderables()) {
            const u32 ThisIdx = Idx++;
            if (!R.Visible || !R.Mesh || !R.Mesh->IsValid()) continue;
            auto It = BlasByMesh.find(R.Mesh);
            if (It == BlasByMesh.end() || It->second == 0) continue;

            D3D12_RAYTRACING_INSTANCE_DESC Inst{};

            const Mat44 T = R.Transform.Matrix().GetTransposed();
            for (int Row = 0; Row < 3; ++Row)
                for (int Col = 0; Col < 4; ++Col)
                    Inst.Transform[Row][Col] = T.M[Row][Col];
            Inst.InstanceID                          = ThisIdx;
            Inst.InstanceContributionToHitGroupIndex = 0;
            Inst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            // Masks segmentadas (SMILE_RT_MASK_* em DDGICommon.hlsli): raios normais tracejam
            // com ALL (uniao dos bits = tudo visivel, comportamento identico ao 0xFF antigo);
            // shadow rays podem usar so OPAQUE p/ pular folhagem (toggle no editor).
            // Folhagem/alpha-test: candidatos nao-opacos passam pelo AlphaTestPass no shader
            // (SMILE_RT_PROCEED em HitShading.hlsli) — sem isto os cards viram quads solidos.
            const bool AlphaTest = R.Material && R.Material->Constants.AlphaTest;
            Inst.InstanceMask = AlphaTest ? 0x02u : 0x01u;
            if (AlphaTest)
                Inst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;

            // Culling. A instance flag VENCE a ray flag na spec do DXR, entao enquanto isto saia
            // incondicionalmente TODO RAY_FLAG_CULL_BACK_FACING_TRIANGLES da engine era no-op — a
            // cena inteira era double-sided no RT enquanto o raster cullava.
            //
            // Criterio = o MESMO do raster (Renderer.cpp escolhe PSOGBufferTwoSided por
            // `TwoSided || AlphaTest`): so assim as duas visoes da cena concordam sobre o que e
            // visivel pelo verso. No cozido atual `TwoSided` e `AlphaTest` saem da mesma condicao
            // (cutout, em Cooker/main.cpp), mais vidro translucido com TwoSided sozinho — na
            // pratica isto isola folhagem/cutouts/vidro e deixa arquitetura one-sided.
            //
            // DDGI nao e afetado: ele traca com RAY_FLAG_NONE de proposito, porque a deteccao de
            // "probe dentro de geometria" depende de ENXERGAR o backface (distancia assinada em
            // DDGITrace). Ray flag e por raio, instance flag e por instancia — por isso os dois
            // regimes convivem na mesma TLAS.
            const bool TwoSidedInstance = R.Material && R.Material->IsTwoSidedForRT();
            if (!SelectiveCulling || TwoSidedInstance)
                Inst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            Inst.AccelerationStructure               = It->second;
            _Out.push_back(Inst);
        }
    }

    bool FRaytracingScene::RecordTlasRebuild(ID3D12GraphicsCommandList4* _CL, const FScene& _Scene,
                                             u32 _FrameSlot) {
        if (!Built || !Tlas || !TlasScratch || !_CL) return false;

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
        CollectInstances(_Scene, Instances);
        if (Instances.empty() || Instances.size() > InstanceCapacity) return false;

        const u32 Slot = _FrameSlot % kInstanceSlots;
        std::memcpy(InstanceMapped[Slot], Instances.data(),
                    Instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TInputs{};
        TInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        TInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
        TInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        TInputs.NumDescs      = static_cast<UINT>(Instances.size());
        TInputs.InstanceDescs = InstanceUpload[Slot]->GetGPUVirtualAddress();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TBuild{};
        TBuild.Inputs                           = TInputs;
        TBuild.ScratchAccelerationStructureData = TlasScratch->GetGPUVirtualAddress();
        TBuild.DestAccelerationStructureData    = Tlas->GetGPUVirtualAddress();
        _CL->BuildRaytracingAccelerationStructure(&TBuild, 0, nullptr);
        // WAR/RAW: o proximo consumidor (trace) e o proximo rebuild (scratch) esperam o build.
        GlobalUAVBarrier(_CL);

        InstanceCount_ = static_cast<u32>(Instances.size());
        return true;
    }

    void FRaytracingScene::Build(FD3D12Device& _Device, FCommandQueue& _Queue,
                                 FTextureSRVHeap& _SRVHeap, const FScene& _Scene) {
        if (!_Device.RaytracingSupported() || !_Device.Device5()) return;

        Release(_SRVHeap);
        ID3D12Device5* Dev5 = _Device.Device5();

        _Queue.ResetForRecording();
        ComPtr<ID3D12GraphicsCommandList4> CL;
        if (FAILED(_Queue.List()->QueryInterface(IID_PPV_ARGS(&CL)))) {
            LogWarning("[GI] - ID3D12GraphicsCommandList4 indisponivel; AS nao construida");
            return;
        }

        std::vector<const FGpuMesh*> UniqueMeshes;
        for (const FRenderable& R : _Scene.Renderables()) {
            if (!R.Mesh || !R.Mesh->IsValid()) continue;
            if (BlasByMesh.find(R.Mesh) != BlasByMesh.end()) continue;
            BlasByMesh.emplace(R.Mesh, D3D12_GPU_VIRTUAL_ADDRESS{ 0 });
            UniqueMeshes.push_back(R.Mesh);
        }
        if (UniqueMeshes.empty()) {
            LogWarning("[GI] - Cena sem geometria; AS nao construida");
            return;
        }
        const u32 NumBlas = static_cast<u32>(UniqueMeshes.size());

        // VB/IB dos meshes ja vivem em estado combinado de leitura que inclui NON_PIXEL
        // (GpuMesh.cpp) — o build de BLAS le direto, sem transicao de ida e volta.

        // Passo 1: prebuild de todos os BLAS p/ dimensionar UM scratch e UM pool de build
        // suballocados por offset (alinhamento de AS = 256B) em vez de um committed resource
        // (heap >= 64KB) por BLAS — best practice NVIDIA/UE (RHIBuildAccelerationStructures).
        struct FBlasPlan {
            D3D12_RAYTRACING_GEOMETRY_DESC Geo{};
            UINT64 ScratchOffset = 0;
            UINT64 BuildOffset   = 0;
            UINT64 CompactOffset = 0;
        };
        std::vector<FBlasPlan> Plans(NumBlas);
        UINT64 TotalScratch = 0, TotalBuild = 0;
        for (u32 i = 0; i < NumBlas; ++i) {
            const FGpuMesh* M = UniqueMeshes[i];
            FBlasPlan& P = Plans[i];
            P.Geo.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            P.Geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            P.Geo.Triangles.VertexBuffer.StartAddress  = M->VertexBufferGPUVA();
            P.Geo.Triangles.VertexBuffer.StrideInBytes = M->VertexStride();
            P.Geo.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
            P.Geo.Triangles.VertexCount                = M->VertexCount();
            P.Geo.Triangles.IndexBuffer                = M->IndexBufferGPUVA();
            P.Geo.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
            P.Geo.Triangles.IndexCount                 = M->GetIndexCount();
            P.Geo.Triangles.Transform3x4               = 0;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
            Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                                  | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            Inputs.NumDescs       = 1;
            Inputs.pGeometryDescs = &P.Geo;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info);
            P.ScratchOffset = TotalScratch;
            P.BuildOffset   = TotalBuild;
            TotalScratch += AlignAS(Info.ScratchDataSizeInBytes);
            TotalBuild   += AlignAS(Info.ResultDataMaxSizeInBytes);
        }

        ComPtr<ID3D12Resource> Scratch = CreateUAVBuffer(Dev5, TotalScratch,
            D3D12_RESOURCE_STATE_COMMON);
        ComPtr<ID3D12Resource> BuildPool = CreateUAVBuffer(Dev5, TotalBuild,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        // Tamanhos compactados emitidos pelo build (postbuild info) + readback p/ CPU.
        ComPtr<ID3D12Resource> PostbuildBuf = CreateUAVBuffer(Dev5,
            static_cast<UINT64>(NumBlas) * sizeof(UINT64), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> ReadbackBuf = CreateBuffer(Dev5,
            static_cast<UINT64>(NumBlas) * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);

        // Passo 2: builds no pool + emissao do tamanho compactado de cada BLAS.
        for (u32 i = 0; i < NumBlas; ++i) {
            FBlasPlan& P = Plans[i];
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
            Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                                  | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            Inputs.NumDescs       = 1;
            Inputs.pGeometryDescs = &P.Geo;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build{};
            Build.Inputs                           = Inputs;
            Build.ScratchAccelerationStructureData = Scratch->GetGPUVirtualAddress() + P.ScratchOffset;
            Build.DestAccelerationStructureData    = BuildPool->GetGPUVirtualAddress() + P.BuildOffset;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC Post{};
            Post.InfoType   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
            Post.DestBuffer = PostbuildBuf->GetGPUVirtualAddress() + i * sizeof(UINT64);
            CL->BuildRaytracingAccelerationStructure(&Build, 1, &Post);
        }
        GlobalUAVBarrier(CL.Get());

        {
            std::vector<D3D12_RESOURCE_BARRIER> ToCopy;
            PushTransition(ToCopy, PostbuildBuf.Get(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            CL->ResourceBarrier(static_cast<UINT>(ToCopy.size()), ToCopy.data());
            CL->CopyResource(ReadbackBuf.Get(), PostbuildBuf.Get());
        }

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL.Get() };
        _Queue.ExecuteAndSync(Lists, 1);

        // Passo 3: readback dos tamanhos compactados e layout do pool final. Compaction de BLAS
        // estatico ~50% de VRAM sem custo de trace (NVIDIA); se algum tamanho vier 0 (readback
        // quebrado), cai pro pool de build sem compactar.
        UINT64 TotalCompact = 0;
        bool   Compact      = true;
        {
            void* Mapped = nullptr;
            D3D12_RANGE ReadRange{ 0, static_cast<SIZE_T>(NumBlas * sizeof(UINT64)) };
            SMILE_HR(ReadbackBuf->Map(0, &ReadRange, &Mapped));
            const UINT64* Sizes = static_cast<const UINT64*>(Mapped);
            for (u32 i = 0; i < NumBlas; ++i) {
                if (Sizes[i] == 0) { Compact = false; break; }
                Plans[i].CompactOffset = TotalCompact;
                TotalCompact += AlignAS(Sizes[i]);
            }
            D3D12_RANGE NoWrite{ 0, 0 };
            ReadbackBuf->Unmap(0, &NoWrite);
        }

        _Queue.ResetForRecording();

        if (Compact) {
            BlasPool = CreateUAVBuffer(Dev5, TotalCompact,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            for (u32 i = 0; i < NumBlas; ++i) {
                CL->CopyRaytracingAccelerationStructure(
                    BlasPool->GetGPUVirtualAddress() + Plans[i].CompactOffset,
                    BuildPool->GetGPUVirtualAddress() + Plans[i].BuildOffset,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            }
            GlobalUAVBarrier(CL.Get());
            for (u32 i = 0; i < NumBlas; ++i)
                BlasByMesh[UniqueMeshes[i]] = BlasPool->GetGPUVirtualAddress() + Plans[i].CompactOffset;
        } else {
            LogWarning("[GI] - Readback de compaction invalido; BLAS sem compactar");
            BlasPool = BuildPool;
            for (u32 i = 0; i < NumBlas; ++i)
                BlasByMesh[UniqueMeshes[i]] = BlasPool->GetGPUVirtualAddress() + Plans[i].BuildOffset;
        }
        BlasCount_ = NumBlas;

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
        CollectInstances(_Scene, Instances);
        InstanceCount_ = static_cast<u32>(Instances.size());

        if (!Instances.empty()) {
            // Infra persistente (rebuild de TLAS por frame no editor): uploads versionados
            // por frame em voo + TLAS/scratch dimensionados p/ a capacidade maxima — TODOS
            // os renderables, pois Visible pode ligar depois do load.
            InstanceCapacity = static_cast<u32>(_Scene.Renderables().size());
            const UINT64 UploadSize =
                static_cast<UINT64>(InstanceCapacity) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
            for (u32 s = 0; s < kInstanceSlots; ++s) {
                InstanceUpload[s] = CreateBuffer(Dev5, UploadSize, D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
                D3D12_RANGE NoRead{ 0, 0 };
                SMILE_HR(InstanceUpload[s]->Map(0, &NoRead,
                         reinterpret_cast<void**>(&InstanceMapped[s])));
            }
            std::memcpy(InstanceMapped[0], Instances.data(),
                        Instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TInputs{};
            TInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            TInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            TInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            TInputs.NumDescs      = InstanceCapacity; // prebuild no pior caso (rebuilds reusam)
            TInputs.InstanceDescs = InstanceUpload[0]->GetGPUVirtualAddress();

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TInfo{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&TInputs, &TInfo);
            TlasScratch = CreateUAVBuffer(Dev5, AlignAS(TInfo.ScratchDataSizeInBytes),
                                          D3D12_RESOURCE_STATE_COMMON);
            Tlas = CreateUAVBuffer(Dev5, AlignAS(TInfo.ResultDataMaxSizeInBytes),
                                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TBuild{};
            TBuild.Inputs                           = TInputs;
            TBuild.Inputs.NumDescs                  = static_cast<UINT>(Instances.size());
            TBuild.ScratchAccelerationStructureData = TlasScratch->GetGPUVirtualAddress();
            TBuild.DestAccelerationStructureData    = Tlas->GetGPUVirtualAddress();
            CL->BuildRaytracingAccelerationStructure(&TBuild, 0, nullptr);
        }

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists2[] = { CL.Get() };
        _Queue.ExecuteAndSync(Lists2, 1);
        // Pool de build, scratch e buffers de postbuild/readback morrem aqui (GPU ja sincronizada).

        if (Instances.empty()) {
            LogWarning("[GI] - Nenhuma instancia visivel; TLAS nao construida");
            return;
        }

        TlasSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
        SRV.ViewDimension                           = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        SRV.Shader4ComponentMapping                 = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRV.RaytracingAccelerationStructure.Location = Tlas->GetGPUVirtualAddress();
        _SRVHeap.CreateSRV(_Device.Native(), nullptr, SRV, TlasSRVSlot_);

        Built = true;
        const double BuildMB   = static_cast<double>(TotalBuild)   / (1024.0 * 1024.0);
        const double FinalMB   = static_cast<double>(Compact ? TotalCompact : TotalBuild) / (1024.0 * 1024.0);
        LogInfo("[GI] - TLAS construida: " + std::to_string(InstanceCount_) +
                " instancias / " + std::to_string(NumBlas) + " BLAS (pool " +
                std::to_string(FinalMB).substr(0, 6) + " MB, build " +
                std::to_string(BuildMB).substr(0, 6) + " MB, compaction " +
                (Compact ? "ON" : "OFF") + ")");
    }
}
