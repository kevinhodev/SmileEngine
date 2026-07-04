#include "Smile/Graphics/RaytracingScene.h"
#include "Smile/Graphics/D3D12Device.h"
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

        ComPtr<ID3D12Resource> CreateUAVBuffer(ID3D12Device* _Device, UINT64 _Size,
                                               D3D12_RESOURCE_STATES _State) {
            D3D12_HEAP_PROPERTIES HeapProps{};
            HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = _Size;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                     _State, nullptr, IID_PPV_ARGS(&Buffer)));
            return Buffer;
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* _Device, const void* _Src,
                                                  UINT64 _Size) {
            D3D12_HEAP_PROPERTIES HeapProps{};
            HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = _Size;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;
            ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffer)));
            if (_Src && _Size) {
                void* Mapped = nullptr; D3D12_RANGE NoRead{ 0, 0 };
                SMILE_HR(Buffer->Map(0, &NoRead, &Mapped));
                std::memcpy(Mapped, _Src, _Size);
                Buffer->Unmap(0, nullptr);
            }
            return Buffer;
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
    }

    void FRaytracingScene::Release(FTextureSRVHeap& _SRVHeap) {
        if (TlasSRVSlot_ != kInvalidSlot) { _SRVHeap.Free(TlasSRVSlot_, 1); TlasSRVSlot_ = kInvalidSlot; }
        BlasResults.clear();
        BlasByMesh.clear();
        Tlas.Reset();
        Built         = false;
        InstanceCount_ = 0;
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

        std::vector<D3D12_RESOURCE_BARRIER> ToRT, FromRT;
        for (const FGpuMesh* M : UniqueMeshes) {
            if (!M->IsDefaultHeap()) continue;
            PushTransition(ToRT, M->VertexResource(),
                           D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransition(ToRT, M->IndexResource(),
                           D3D12_RESOURCE_STATE_INDEX_BUFFER,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransition(FromRT, M->VertexResource(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            PushTransition(FromRT, M->IndexResource(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }
        if (!ToRT.empty()) CL->ResourceBarrier(static_cast<UINT>(ToRT.size()), ToRT.data());

        std::vector<ComPtr<ID3D12Resource>> ScratchKeepAlive;
        BlasResults.reserve(UniqueMeshes.size());
        for (const FGpuMesh* M : UniqueMeshes) {
            D3D12_RAYTRACING_GEOMETRY_DESC Geo{};
            Geo.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            Geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            Geo.Triangles.VertexBuffer.StartAddress  = M->VertexBufferGPUVA();
            Geo.Triangles.VertexBuffer.StrideInBytes = M->VertexStride();
            Geo.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
            Geo.Triangles.VertexCount                = M->VertexCount();
            Geo.Triangles.IndexBuffer                = M->IndexBufferGPUVA();
            Geo.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
            Geo.Triangles.IndexCount                 = M->GetIndexCount();
            Geo.Triangles.Transform3x4               = 0;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
            Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            Inputs.NumDescs       = 1;
            Inputs.pGeometryDescs = &Geo;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info);

            ComPtr<ID3D12Resource> Scratch = CreateUAVBuffer(Dev5,
                AlignAS(Info.ScratchDataSizeInBytes), D3D12_RESOURCE_STATE_COMMON);
            ComPtr<ID3D12Resource> Result = CreateUAVBuffer(Dev5,
                AlignAS(Info.ResultDataMaxSizeInBytes),
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build{};
            Build.Inputs                                  = Inputs;
            Build.ScratchAccelerationStructureData        = Scratch->GetGPUVirtualAddress();
            Build.DestAccelerationStructureData           = Result->GetGPUVirtualAddress();
            CL->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);

            BlasByMesh[M] = Result->GetGPUVirtualAddress();
            BlasResults.push_back(Result);
            ScratchKeepAlive.push_back(Scratch);
        }

        {
            D3D12_RESOURCE_BARRIER UAV{};
            UAV.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UAV.UAV.pResource = nullptr;
            CL->ResourceBarrier(1, &UAV);
        }

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
        Instances.reserve(_Scene.Renderables().size());
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
            Inst.InstanceMask                        = 0xFF;
            Inst.InstanceContributionToHitGroupIndex = 0;
            Inst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            // Folhagem/alpha-test: candidatos nao-opacos passam pelo AlphaTestPass no shader
            // (SMILE_RT_PROCEED em HitShading.hlsli) — sem isto os cards viram quads solidos.
            if (R.Material && R.Material->Constants.AlphaTest)
                Inst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
            Inst.AccelerationStructure               = It->second;
            Instances.push_back(Inst);
        }
        InstanceCount_ = static_cast<u32>(Instances.size());

        ComPtr<ID3D12Resource> InstanceBuffer, TScratch;
        if (!Instances.empty()) {
            InstanceBuffer = CreateUploadBuffer(Dev5, Instances.data(),
                Instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TInputs{};
            TInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            TInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            TInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            TInputs.NumDescs      = static_cast<UINT>(Instances.size());
            TInputs.InstanceDescs = InstanceBuffer->GetGPUVirtualAddress();

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TInfo{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&TInputs, &TInfo);
            TScratch = CreateUAVBuffer(Dev5, AlignAS(TInfo.ScratchDataSizeInBytes),
                                       D3D12_RESOURCE_STATE_COMMON);
            Tlas = CreateUAVBuffer(Dev5, AlignAS(TInfo.ResultDataMaxSizeInBytes),
                                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TBuild{};
            TBuild.Inputs                           = TInputs;
            TBuild.ScratchAccelerationStructureData = TScratch->GetGPUVirtualAddress();
            TBuild.DestAccelerationStructureData    = Tlas->GetGPUVirtualAddress();
            CL->BuildRaytracingAccelerationStructure(&TBuild, 0, nullptr);
        }

        if (!FromRT.empty()) CL->ResourceBarrier(static_cast<UINT>(FromRT.size()), FromRT.data());

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL.Get() };
        _Queue.ExecuteAndSync(Lists, 1);

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
        LogInfo("[GI] - TLAS construida: " + std::to_string(InstanceCount_) +
                " instancias / " + std::to_string(BlasResults.size()) + " BLAS");
    }
}
