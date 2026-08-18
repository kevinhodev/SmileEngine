#include "Smile/Graphics/RayTracing/RaytracingScene.h"
#include "Smile/Graphics/RHI/GpuResources.h"
#include "Smile/Graphics/RHI/D3D12Device.h"
#include "Smile/Graphics/Debug/VramTracker.h"
#include "Smile/Graphics/RHI/CommandQueue.h"
#include "Smile/Graphics/RHI/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h" // sizeof(Vertex): stride do SRV bindless de VB
#include "Smile/Graphics/RayTracing/RTMasks.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
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
            return GpuResources::CreateBuffer(_Device, _Size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              _State, EVramCategory::RaytracingAS);
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

        // Espelho C++ do `InstanceGeo` do HLSL (ver RTGeometry.hlsli / HitShading.hlsli). O
        // static_assert e o contrato: qualquer campo acrescentado aqui desloca o resto e faz todo
        // hit de RT ler material trocado, em silencio.
        struct FRTInstanceGeo {
            Vec4 BaseColor;
            // VB/IB continuam aqui: o MeshLightExtract ainda percorre a malha por vertice, e o
            // raster/BLAS seguem consumindo os mesmos buffers. Quem saiu deles foi o caminho de
            // HIT (PT_LoadHitSurface, HitFaceNormal, HitIsBackface, HitGeomNormal, AlphaTestPass),
            // que agora le o TriangleSrv.
            u32  VertexSrv   = 0;
            u32  IndexSrv    = 0;
            // Payload pre-cozido por triangulo (FRTTriangle, 32 B), indexado por PrimitiveIndex.
            // Compartilhado por TODAS as instancias da mesma FGpuMesh — ele descreve geometria em
            // espaco local, entao duas instancias da mesma malha leem o mesmo buffer.
            u32  TriangleSrv = 0;
            u32  AlbedoIndex = 0;
            u32  HasAlbedo   = 0;
            u32  TwoSidedRT  = 0; // = FMaterial::IsTwoSidedForRT (inclui AlphaTest), nao a flag crua
            u32  Flags       = 0;
            f32  AlphaCutoff = 0.5f;
            f32  RoughnessFactor = 0.5f;
            Vec4 EmissiveFactor{ 0.0f, 0.0f, 0.0f, 0.0f };
            u32  EmissiveMapIndex = 0;
            u32  MrMapIndex       = 0;
            u32  MetalMapIndex    = 0; // mapa Metalness separado (slot +6)
            u32  RoughMapIndex    = 0; // mapa Roughness separado (slot +7)
        };
        // 84 B na v8 (+TriangleSrv). Vec4 aqui e alinhado em 4, entao o struct nao ganha padding
        // de cauda e o stride do StructuredBuffer casa byte a byte com o `InstanceGeo` do HLSL —
        // que e o unico contrato que importa (StructuredBuffer nao exige stride multiplo de 16).
        static_assert(sizeof(FRTInstanceGeo) == 84, "FRTInstanceGeo deve casar com o HLSL (84B)");
    }

    static_assert(FRaytracingScene::kInstanceSlots == FCommandQueue::kFramesInFlight,
                  "InstanceUpload versionado por frame em voo");

    void FRaytracingScene::Release(FTextureSRVHeap& _SRVHeap) {
        if (TlasSRVSlot_ != kInvalidSlot) { _SRVHeap.Free(TlasSRVSlot_, 1); TlasSRVSlot_ = kInvalidSlot; }
        if (InstanceGeoSRVSlot_ != kInvalidSlot) {
            _SRVHeap.Free(InstanceGeoSRVSlot_, 1);
            InstanceGeoSRVSlot_ = kInvalidSlot;
        }
        if (MeshGeoSlotBase != kInvalidSlot) {
            _SRVHeap.Free(MeshGeoSlotBase, MeshGeoSlotCount);
            MeshGeoSlotBase  = kInvalidSlot;
            MeshGeoSlotCount = 0;
        }
        InstanceGeoBuf.Reset();
        InstanceGeoCount = 0;
        MeshGeoSlot.clear();
        BlasPool.Reset();
        BlasByMesh.clear();
        Tlas.Reset();
        TlasScratch.Reset();
        for (u32 s = 0; s < kInstanceSlots; ++s) {
            InstanceUpload[s].Reset();
            InstanceMapped[s] = nullptr;
        }
        InstanceCapacity_ = 0;
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
            // Categoria da instancia (kRTMask* em RTMasks.h). UM bit por instancia; quem escolhe
            // o que enxerga e o PASSE, pela mask do raio — modelo do Lumen.
            // Folhagem/alpha-test: candidatos nao-opacos passam pelo AlphaTestPass no shader
            // (SMILE_RT_PROCEED em HitShading.hlsli) — sem isto os cards viram quads solidos.
            // Translucido (Blend) fica numa categoria propria: o gather do GI nao o inclui, entao
            // o vidro deixa de ser parede opaca para a iluminacao indireta. Ele CONTINUA na TLAS —
            // as reflexoes ainda o enxergam, que era a razao de nao simplesmente removE-lo.
            const bool AlphaTest = R.Material && R.Material->Constants.AlphaTest;
            const bool Blend     = R.Material && R.Material->Blend;
            Inst.InstanceMask = Blend ? kRTMaskTranslucent
                                      : (AlphaTest ? kRTMaskAlphaTest : kRTMaskOpaque);
            if (AlphaTest)
                Inst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;

            // Culling. A instance flag VENCE a ray flag na spec do DXR, entao enquanto isto saia
            // incondicionalmente TODO RAY_FLAG_CULL_BACK_FACING_TRIANGLES da engine era no-op — a
            // cena inteira era double-sided no RT enquanto o raster cullava. A flag agora descreve
            // so a GEOMETRIA; quem decide cullar e cada passe, pela ray flag.
            //
            // Criterio = FMaterial::IsTwoSidedForRT, que espelha o que o RASTER desenha two-sided:
            // o G-buffer escolhe PSOGBufferTwoSided por `TwoSided || AlphaTest`, e o passe forward
            // dos translucidos e cull NONE para todos. So assim as duas visoes da cena concordam
            // sobre o que e visivel pelo verso. No cozido atual `TwoSided` e `AlphaTest` saem da
            // mesma condicao (cutout, em Cooker/main.cpp) — na pratica isto isola
            // folhagem/cutouts/vidro e deixa arquitetura one-sided.
            //
            // DDGI nao e afetado: ele traca com RAY_FLAG_NONE de proposito, porque a deteccao de
            // "probe dentro de geometria" depende de ENXERGAR o backface (distancia assinada em
            // DDGITrace). Ray flag e por raio, instance flag e por instancia — por isso os dois
            // regimes convivem na mesma TLAS.
            if (R.Material && R.Material->IsTwoSidedForRT())
                Inst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            Inst.AccelerationStructure               = It->second;
            _Out.push_back(Inst);
        }
    }

    // Irma do CollectInstances: as duas percorrem a MESMA lista na MESMA ordem, e e essa ordem que
    // liga as duas — o InstanceID que a TLAS carrega e o indice deste vetor, e e por ele que o hit
    // de RT indexa o snapshot. Preencher aqui, e nao no dono do volume DDGI, e o que garante que
    // as duas nunca sejam construidas em momentos diferentes.
    void FRaytracingScene::FillInstanceGeo(const FScene& _Scene, u8* _Mapped, u32 _Count) const {
        for (u32 i = 0; i < _Count; ++i) {
            const FRenderable& R = _Scene.Renderables()[i];
            FRTInstanceGeo g{};
            g.BaseColor = { 0.7f, 0.7f, 0.7f, 1.0f };
            if (R.Material) {
                const MaterialConstants& MC = R.Material->Constants;
                g.BaseColor = MC.BaseColorFactor;
                // Mesmo criterio da flag de culling da TLAS (ver FMaterial::IsTwoSidedForRT, e o
                // CollectInstances logo acima): este campo e o que decide, no shader, se um hit
                // pelo verso significa "dentro de solido" — tem que concordar com quem o raio
                // consegue enxergar pelo verso.
                g.TwoSidedRT = R.Material->IsTwoSidedForRT() ? 1u : 0u;
                if (R.Material->IsFinalized() && R.Material->HasAlbedoTexture()) {
                    g.AlbedoIndex = R.Material->AlbedoDescriptorIndex();
                    g.HasAlbedo   = 1;
                }

                g.AlphaCutoff     = MC.AlphaCutoff;
                g.RoughnessFactor = MC.RoughnessFactor;
                // O RTEmissiveScale entra SO aqui: este InstanceGeo e o que os hits de RT leem
                // (DDGI, ReSTIR GI e reflexoes). O raster segue com o EmissiveStrength puro, entao
                // baixar a escala tira a malha de iluminar o ambiente sem apagar o brilho dela na
                // tela. Ver MaterialConstants::RTEmissiveScale.
                const f32 EmiRT = MC.EmissiveStrength * MC.RTEmissiveScale;
                g.EmissiveFactor  = { MC.EmissiveFactor.X * EmiRT,
                                      MC.EmissiveFactor.Y * EmiRT,
                                      MC.EmissiveFactor.Z * EmiRT,
                                      MC.MetallicFactor };
                if (MC.AlphaTest)        g.Flags |= 1u;
                if (MC.ShadingModel == 1) g.Flags |= 4u; // Foliage
                // Categoria da instancia na TLAS (kRTMaskTranslucent). Espelhada aqui porque a
                // mask e propriedade do RAIO: um shader nao consegue perguntar a TLAS com que
                // mask a instancia entrou. Consumida so pelo BvhDebug — os outros passes
                // distinguem translucido pela mask que eles proprios tracam.
                if (R.Material->Blend)   g.Flags |= 128u;
                if (R.Material->IsFinalized()) {
                    // Slots do material: 0=albedo, 1=normal, 2=metallic-roughness, 3=AO, 4=emissive.
                    if (MC.HasEmissiveMap) {
                        g.EmissiveMapIndex = R.Material->AlbedoDescriptorIndex() + 4;
                        g.Flags |= 2u;
                    }
                    if (MC.HasMetallicRoughnessMap) {
                        g.MrMapIndex = R.Material->AlbedoDescriptorIndex() + 2;
                        g.Flags |= 8u;
                        // Empacotamento do mapa: "Specular" poe metal em B, glTF poe em R. Sem
                        // este bit o RT leria o canal errado e um material viraria metal (ou
                        // dielétrico) por engano no bounce.
                        if (MC.SpecularPacking) g.Flags |= 16u;
                    }
                    // Mapa Metalness SEPARADO (slot +6). Precisa existir aqui porque o loader
                    // deixa MetallicFactor = 1 quando ha mapa — sem enxergar o mapa, o RT leria
                    // "metal puro" e zeraria o difuso de um material que e quase todo dieletrico.
                    if (MC.HasMetalnessMap) {
                        g.MetalMapIndex = R.Material->AlbedoDescriptorIndex() + 6;
                        g.Flags |= 32u;
                    }
                    if (MC.HasRoughnessMap) {
                        g.RoughMapIndex = R.Material->AlbedoDescriptorIndex() + 7;
                        g.Flags |= 64u;
                    }
                }
            }
            // Tres slots CONSECUTIVOS por malha unica: [VB][IB][RTTri]. O base vem do
            // MeshGeoSlot, que e indexado pela FGpuMesh — entao N instancias da mesma malha
            // recebem o MESMO TriangleSrv, que e o que torna o payload por malha e nao por
            // instancia (a conta de VRAM segue triangulos unicos).
            auto It = R.Mesh ? MeshGeoSlot.find(R.Mesh) : MeshGeoSlot.end();
            if (It != MeshGeoSlot.end()) {
                g.VertexSrv   = It->second;
                g.IndexSrv    = It->second + 1;
                g.TriangleSrv = It->second + 2;
            }
            std::memcpy(_Mapped + i * sizeof(FRTInstanceGeo), &g, sizeof(FRTInstanceGeo));
        }
    }

    void FRaytracingScene::RefreshInstanceGeo(const FScene& _Scene) {
        if (!InstanceGeoBuf || InstanceGeoCount == 0) return;
        const u32 Count = std::min(InstanceGeoCount,
                                   static_cast<u32>(_Scene.Renderables().size()));
        u8*         Mapped = nullptr;
        D3D12_RANGE NoRead{ 0, 0 };
        if (FAILED(InstanceGeoBuf->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped)))) return;
        FillInstanceGeo(_Scene, Mapped, Count);
        InstanceGeoBuf->Unmap(0, nullptr);
    }

    bool FRaytracingScene::RecordTlasRebuild(ID3D12GraphicsCommandList4* _CL, const FScene& _Scene,
                                             u32 _FrameSlot) {
        if (!Built || !Tlas || !TlasScratch || !_CL) return false;

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
        CollectInstances(_Scene, Instances);
        if (Instances.empty() || Instances.size() > InstanceCapacity_) return false;

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
            LogDebug("[GI] - Cena sem geometria; AS nao construida");
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
        ComPtr<ID3D12Resource> ReadbackBuf = GpuResources::CreateReadbackBuffer(
            Dev5, static_cast<u64>(NumBlas) * sizeof(UINT64));

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
            // Capacidade COM folga (SceneCapacityFor): sem ela, o primeiro objeto criado no
            // editor ja nao cabia e o RecordTlasRebuild recusava, forcando um Build inteiro.
            InstanceCapacity_ =
                SceneCapacityFor(static_cast<u32>(_Scene.Renderables().size()));
            const UINT64 UploadSize =
                static_cast<UINT64>(InstanceCapacity_) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
            for (u32 s = 0; s < kInstanceSlots; ++s) {
                const GpuResources::FUploadBuffer Upload =
                    GpuResources::CreateUploadBuffer(Dev5, UploadSize, 1, false);
                InstanceUpload[s] = Upload.Resource;
                InstanceMapped[s] = Upload.Mapped;
            }
            std::memcpy(InstanceMapped[0], Instances.data(),
                        Instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TInputs{};
            TInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            TInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            TInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            TInputs.NumDescs      = InstanceCapacity_; // prebuild no pior caso (rebuilds reusam)
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
            LogDebug("[GI] - Nenhuma instancia visivel; TLAS nao construida");
            return;
        }

        TlasSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
        SRV.ViewDimension                           = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        SRV.Shader4ComponentMapping                 = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRV.RaytracingAccelerationStructure.Location = Tlas->GetGPUVirtualAddress();
        _SRVHeap.CreateSRV(_Device.Native(), nullptr, SRV, TlasSRVSlot_);

        // === Snapshot InstanceGeo ==========================================================
        // Depois da TLAS, e nao antes, porque o snapshot so tem sentido acompanhado dela: quem o
        // le chega pelo InstanceID de um hit. Sem TLAS o Build ja retornou acima, e ai o
        // InstanceGeoSRV segue invalido — o mesmo criterio que o FDDGI aplicava quando era dono
        // disto (ele desistia com TlasSRVSlot == kInvalidSlot).
        //
        // Os SRVs bindless de VB/IB saem do MESMO UniqueMeshes que dimensionou os BLAS. O DDGI
        // reconstruia essa lista com um criterio identico, palavra por palavra; uma so agora.
        // TRES por malha desde a v8: [VB][IB][RTTri]. O terceiro e o payload pre-cozido por
        // triangulo, que o caminho de hit passou a ler no lugar da cadeia IB -> 3 vertices.
        MeshGeoSlotCount = NumBlas * 3;
        MeshGeoSlotBase  = _SRVHeap.Allocate(MeshGeoSlotCount);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC GeoSrv{};
            GeoSrv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            GeoSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            for (u32 i = 0; i < NumBlas; ++i) {
                const FGpuMesh* M      = UniqueMeshes[i];
                const u32       VbSlot = MeshGeoSlotBase + i * 3;

                GeoSrv.Format                     = DXGI_FORMAT_UNKNOWN;
                GeoSrv.Buffer.FirstElement        = M->VertexFirstElement();
                GeoSrv.Buffer.NumElements         = M->VertexCount();
                GeoSrv.Buffer.StructureByteStride = sizeof(Vertex);
                _SRVHeap.CreateSRV(_Device.Native(), M->VertexResource(), GeoSrv, VbSlot);
                GeoSrv.Format                     = DXGI_FORMAT_R32_UINT;
                GeoSrv.Buffer.FirstElement        = M->IndexFirstElement();
                GeoSrv.Buffer.NumElements         = M->GetIndexCount();
                GeoSrv.Buffer.StructureByteStride = 0;
                _SRVHeap.CreateSRV(_Device.Native(), M->IndexResource(), GeoSrv, VbSlot + 1);

                // Malha sem payload nao deveria existir (o loader recusa cozido sem a regiao, e o
                // caminho procedural gera no upload), mas um descritor NULO tipado e melhor que
                // um slot nao escrito: o shader le zeros e cai no fallback de normal invalida,
                // em vez de amostrar lixo de um descritor herdado.
                GeoSrv.Format                     = DXGI_FORMAT_UNKNOWN;
                GeoSrv.Buffer.FirstElement        = M->RTTriangleFirstElement();
                GeoSrv.Buffer.NumElements         = M->RTTriangleCount();
                GeoSrv.Buffer.StructureByteStride = sizeof(FRTTriangle);
                _SRVHeap.CreateSRV(_Device.Native(),
                                   M->RTTriangleCount() > 0 ? M->RTTriangleResource() : nullptr,
                                   GeoSrv, VbSlot + 2);
                MeshGeoSlot[M] = VbSlot;
            }
        }

        // Com folga (SceneCapacityFor): objeto criado no editor cabe sem realocar descritor nem
        // refazer o Build. So os [0, NumRenderables) sao preenchidos — FillInstanceGeo le a lista
        // da cena, e ir alem dela seria leitura fora do vetor. A cauda vai a zero: nenhuma
        // instancia da TLAS aponta para la (a TLAS so tem as reais), mas zero e um estado legivel
        // se algum dia um indice errado chegar ate aqui.
        const u32 NumRenderables = static_cast<u32>(_Scene.Renderables().size());
        const u32 GeoCapacity    = SceneCapacityFor(NumRenderables);
        const GpuResources::FUploadBuffer GeoUpload = GpuResources::CreateUploadBuffer(
            Dev5, static_cast<UINT64>(GeoCapacity) * sizeof(FRTInstanceGeo), 1,
            /*ForConstantBuffer*/ false);
        InstanceGeoBuf   = GeoUpload.Resource;
        InstanceGeoCount = GeoCapacity;
        FillInstanceGeo(_Scene, GeoUpload.Mapped, NumRenderables);
        std::memset(GeoUpload.Mapped + static_cast<size_t>(NumRenderables) * sizeof(FRTInstanceGeo),
                    0, static_cast<size_t>(GeoCapacity - NumRenderables) * sizeof(FRTInstanceGeo));
        InstanceGeoBuf->Unmap(0, nullptr);

        InstanceGeoSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC GeoBufSrv{};
        GeoBufSrv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        GeoBufSrv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GeoBufSrv.Format                     = DXGI_FORMAT_UNKNOWN;
        GeoBufSrv.Buffer.FirstElement        = 0;
        GeoBufSrv.Buffer.NumElements         = GeoCapacity; // = tamanho do buffer, nao da cena
        GeoBufSrv.Buffer.StructureByteStride = sizeof(FRTInstanceGeo);
        _SRVHeap.CreateSRV(_Device.Native(), InstanceGeoBuf.Get(), GeoBufSrv, InstanceGeoSRVSlot_);

        Built = true;
        const double BuildMB   = static_cast<double>(TotalBuild)   / (1024.0 * 1024.0);
        const double FinalMB   = static_cast<double>(Compact ? TotalCompact : TotalBuild) / (1024.0 * 1024.0);
        LogDebug("[GI] - TLAS construida: " + std::to_string(InstanceCount_) +
                " instancias / " + std::to_string(NumBlas) + " BLAS (pool " +
                std::to_string(FinalMB).substr(0, 6) + " MB, build " +
                std::to_string(BuildMB).substr(0, 6) + " MB, compaction " +
                (Compact ? "ON" : "OFF") + ")");
    }
}
