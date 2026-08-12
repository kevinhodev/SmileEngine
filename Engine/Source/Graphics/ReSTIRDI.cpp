#include "Smile/Graphics/ReSTIRDI.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>
#include <algorithm>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // Só o W. O ponto visivel X1 morava aqui num RGBA32F inteiro e era redundante com o depth
        // (espacial) e com o historico de superficie (temporal) — ver ReSTIRDICommon.hlsli.
        constexpr DXGI_FORMAT kResWFormat   = DXGI_FORMAT_R32_FLOAT;
        // UINT, e nao mais RGBA16F: o LightIndex em fp16 era exato so ate 2048 e o M dividia canal
        // com a idade, preso em 63. Ver o layout em ReSTIRDICommon.hlsli.
        constexpr DXGI_FORMAT kResBFormat   = DXGI_FORMAT_R32G32B32A32_UINT;
        // O Pass A traca o raio de visibilidade do Alg. 5 (TLAS + Instances para o alpha-test da
        // folhagem, o que exige root signature bindless), le o pool de mesh lights (as candidatas
        // iniciais saem do pool COMBINADO) e, desde que o X1 saiu do reservoir, tambem o historico
        // de superficie do frame anterior (t12).
        constexpr u32 kInitialSRVs = 13;
        constexpr u32 kInitialUAVs = 2;
        constexpr u32 kSpatialSRVs = 13;
        constexpr u32 kSpatialUAVs = 4;

        // Posicao dos descritores de MESH LIGHT dentro de cada tabela. Existem com nome porque o
        // RefreshMeshLightDescriptors reescreve ESTES slots sem refazer a tabela inteira — e um
        // indice literal ali seria uma referencia invisivel aos arrays do SetupForResize: quem
        // inserisse um SRV antes deles deslocaria tudo e o refresh passaria a sobrescrever o
        // descritor errado, sem erro de compilacao e sem validation error. Mexer na ordem dos
        // arrays exige mexer aqui.
        constexpr u32 kInitialMeshLightIdx = 10; // t10 do InitialSlots
        constexpr u32 kInitialMeshAliasIdx = 11; // t11 do InitialSlots
        constexpr u32 kSpatialMeshLightIdx = 12; // t12 do SpatialSlots
        static_assert(kInitialMeshAliasIdx < kInitialSRVs && kSpatialMeshLightIdx < kSpatialSRVs,
                      "indice de mesh light fora da tabela");
        constexpr u32 kNrdPackSRVs = 8;
        constexpr u32 kNrdPackUAVs = 5;
        constexpr u32 kNrdCompositeSRVs = 6;

        ComPtr<ID3D12Resource> CreateUAVTexture(ID3D12Device* Device, u32 Width, u32 Height,
                                                DXGI_FORMAT Format) {
            return GpuResources::CreateTex2D(
                Device, Width, Height, Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, EVramCategory::RenderTargets, nullptr,
                1, 1, "ReSTIR DI");
        }
    }

    void FReSTIRDI::Initialize(ID3D12Device* Device) {
        InitialTemporalPSO.Initialize(Device, "ReSTIRDIInitialTemporal.cs_6_6.cso",
                                      kInitialSRVs, kInitialUAVs, true);
        SpatialPSO.Initialize(Device, "ReSTIRDISpatial.cs_6_6.cso",
                              kSpatialSRVs, kSpatialUAVs, true);
        NrdPackPSO.Initialize(Device, "ReSTIRDINrdPack.cs_6_6.cso",
                              kNrdPackSRVs, kNrdPackUAVs, false);
        NrdCompositePSO.Initialize(Device, "ReSTIRDINrdComposite.cs_6_6.cso",
                                   kNrdCompositeSRVs, 1, false);
        CreateConstantBuffer(Device);
        Initialized = true;
    }

    void FReSTIRDI::RecreatePSO(ID3D12Device* Device) {
        if (!Initialized) return;
        InitialTemporalPSO.Initialize(Device, "ReSTIRDIInitialTemporal.cs_6_6.cso",
                                      kInitialSRVs, kInitialUAVs, true);
        SpatialPSO.Initialize(Device, "ReSTIRDISpatial.cs_6_6.cso",
                              kSpatialSRVs, kSpatialUAVs, true);
        NrdPackPSO.Initialize(Device, "ReSTIRDINrdPack.cs_6_6.cso",
                              kNrdPackSRVs, kNrdPackUAVs, false);
        NrdCompositePSO.Initialize(Device, "ReSTIRDINrdComposite.cs_6_6.cso",
                                   kNrdCompositeSRVs, 1, false);
    }

    void FReSTIRDI::CreateConstantBuffer(ID3D12Device* Device) {
        // O sizeof ja e travado em 512 no header; aqui o que importa e o passo ser
        // 256-alinhado, que e o que o root CBV exige do endereco de cada slot.
        static_assert(sizeof(ReSTIRDIConstants) % 256 == 0,
                      "o CBAddr indexa por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            Device, sizeof(ReSTIRDIConstants), FCommandQueue::kFramesInFlight);
        CB       = Upload.Resource;
        MappedCB = Upload.Mapped;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FReSTIRDI::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<u64>(FrameSlot) * sizeof(ReSTIRDIConstants);
    }

    void FReSTIRDI::ReleaseResize(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(OutSRV, 1); Free(OutUAV, 1);
        Free(RawDiffuseSRV, 1); Free(RawDiffuseUAV, 1);
        Free(RawSpecularSRV, 1); Free(RawSpecularUAV, 1);
        Free(ShadowMotionSRV, 1); Free(ShadowMotionUAV, 1);
        Free(SpatialUAVTable, kSpatialUAVs);
        Free(NrdPackSrvTable, kNrdPackSRVs);
        Free(NrdPackUavTable, kNrdPackUAVs);
        Free(NrdCompositeSrvTable, kNrdCompositeSRVs);
        Free(NrdOutDiffuseSRV, 1); Free(NrdOutSpecularSRV, 1);
        for (u32 p = 0; p < kParityCount; ++p) {
            Free(ResWSRV[p], 1); Free(ResBSRV[p], 1);
            Free(ResWUAV[p], 1); Free(ResBUAV[p], 1);
            Free(InitialUAVTable[p], kInitialUAVs);
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                Free(InitialTable[p][f], kInitialSRVs);
                Free(SpatialTable[p][f], kSpatialSRVs);
            }
            ResW[p].Reset(); ResB[p].Reset();
            ResWState[p] = ResBState[p] = D3D12_RESOURCE_STATE_COMMON;
        }
        Output.Reset(); RawDiffuse.Reset(); RawSpecular.Reset(); ShadowMotion.Reset();
        OutputState = RawDiffuseState = RawSpecularState = ShadowMotionState =
            D3D12_RESOURCE_STATE_COMMON;
        NrdReady = false;
        Ready = false;
    }

    void FReSTIRDI::SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                   u32 NewWidth, u32 NewHeight,
                                   u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                                   u32 DepthSlot, u32 VelocitySlot, u32 TlasSlot, u32 InstanceSlot,
                                   u32 MeshLightSlot, u32 MeshAliasSlot,
                                   const u32 LightSlots[FCommandQueue::kFramesInFlight],
                                   const u32 TransformSlots[FCommandQueue::kFramesInFlight],
                                   const u32 SurfaceSlots[FCommandQueue::kFramesInFlight]) {
        if (!Initialized) return;
        ReleaseResize(SRVHeap);
        if (NewWidth == 0 || NewHeight == 0 ||
            GBufferASlot == kInvalidSlot || GBufferBSlot == kInvalidSlot ||
            GBufferCSlot == kInvalidSlot || DepthSlot == kInvalidSlot ||
            VelocitySlot == kInvalidSlot || TlasSlot == kInvalidSlot ||
            InstanceSlot == kInvalidSlot || MeshLightSlot == kInvalidSlot ||
            MeshAliasSlot == kInvalidSlot) return;
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f)
            if (LightSlots[f] == kInvalidSlot || TransformSlots[f] == kInvalidSlot ||
                SurfaceSlots[f] == kInvalidSlot) return;

        Width = NewWidth; Height = NewHeight;
        Output = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        RawDiffuse = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        RawSpecular = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        ShadowMotion = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        for (u32 p = 0; p < kParityCount; ++p) {
            ResW[p] = CreateUAVTexture(Device, Width, Height, kResWFormat);
            ResB[p] = CreateUAVTexture(Device, Width, Height, kResBFormat);
        }

        auto MakeViews = [&](ID3D12Resource* Resource, DXGI_FORMAT Format,
                             u32& SRV, u32& UAV) {
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
        MakeViews(Output.Get(), kOutputFormat, OutSRV, OutUAV);
        MakeViews(RawDiffuse.Get(), kOutputFormat, RawDiffuseSRV, RawDiffuseUAV);
        MakeViews(RawSpecular.Get(), kOutputFormat, RawSpecularSRV, RawSpecularUAV);
        MakeViews(ShadowMotion.Get(), kOutputFormat, ShadowMotionSRV, ShadowMotionUAV);
        for (u32 p = 0; p < kParityCount; ++p) {
            MakeViews(ResW[p].Get(), kResWFormat, ResWSRV[p], ResWUAV[p]);
            MakeViews(ResB[p].Get(), kResBFormat, ResBSRV[p], ResBUAV[p]);
        }

        auto CopyTable = [&](u32 DstSlot, const u32* Slots, u32 Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[16]{};
            UINT SrcCounts[16]{};
            for (u32 i = 0; i < Count; ++i) {
                Src[i] = SRVHeap.CpuHandleStaging(Slots[i]);
                SrcCounts[i] = 1;
            }
            UINT DstCount = Count;
            Device->CopyDescriptors(1, &Dst, &DstCount, Count, Src, SrcCounts,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        SpatialUAVTable = SRVHeap.Allocate(kSpatialUAVs);
        const u32 SpatialUAVSlots[kSpatialUAVs] = {
            OutUAV, RawDiffuseUAV, RawSpecularUAV, ShadowMotionUAV };
        CopyTable(SpatialUAVTable, SpatialUAVSlots, kSpatialUAVs);

        for (u32 p = 0; p < kParityCount; ++p) {
            const u32 Prev = 1u - p;
            InitialUAVTable[p] = SRVHeap.Allocate(kInitialUAVs);
            const u32 InitialUAVSlots[kInitialUAVs] = { ResWUAV[p], ResBUAV[p] };
            CopyTable(InitialUAVTable[p], InitialUAVSlots, kInitialUAVs);

            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                InitialTable[p][f] = SRVHeap.Allocate(kInitialSRVs);
                const u32 InitialSlots[kInitialSRVs] = {
                    GBufferASlot, GBufferBSlot, GBufferCSlot, DepthSlot, VelocitySlot,
                    ResWSRV[Prev], ResBSRV[Prev], LightSlots[f], TlasSlot, InstanceSlot,
                    // t10/t11: pool de mesh lights + alias table. Reescritos isoladamente pelo
                    // RefreshMeshLightDescriptors — ver kInitialMeshLightIdx/kInitialMeshAliasIdx.
                    MeshLightSlot, MeshAliasSlot,
                    // t12: superficie do frame ANTERIOR — de onde sai o X1 do reuso temporal.
                    SurfaceSlots[1u - f] };
                CopyTable(InitialTable[p][f], InitialSlots, kInitialSRVs);

                SpatialTable[p][f] = SRVHeap.Allocate(kSpatialSRVs);
                const u32 SpatialSlots[kSpatialSRVs] = {
                    GBufferASlot, GBufferBSlot, GBufferCSlot, DepthSlot, TlasSlot, InstanceSlot,
                    ResWSRV[p], ResBSRV[p], LightSlots[f], TransformSlots[f],
                    SurfaceSlots[f], SurfaceSlots[1u - f],
                    // t12: pool de mesh lights — ver kSpatialMeshLightIdx.
                    MeshLightSlot };
                CopyTable(SpatialTable[p][f], SpatialSlots, kSpatialSRVs);
            }
        }

        FrameParity = 0;
        NeedsClear = true;
        Ready = true;
    }

    // As tabelas guardam uma COPIA do descritor (CopyDescriptors), nao uma referencia ao slot de
    // origem. Quando o FMeshLights se reconstroi — Release + realocacao dos buffers e dos slots —
    // as copias daqui continuam descrevendo os buffers ANTIGOS, ja liberados. Como a alias table
    // e o pool sao lidos por TODA candidata inicial, o resultado nao e um pixel errado: sai peso
    // absurdo/NaN no reservoir e a tela inteira estoura em branco.
    //
    // Isto acontecia no caminho BARATO do OnSceneStructureChanged (duplicar objeto sem estourar a
    // folga), que reconstroi o FMeshLights e nao passa pelo SetupReflectionsForScene — o unico
    // lugar que reconstruia estas tabelas. Ligar/desligar o ReSTIR DI so limpa reservoir; nao
    // refaz descritor, e por isso o sintoma aparecia ao LIGAR o DI depois da duplicacao. Apagar os
    // duplicados costumava "consertar" porque a realocacao reaproveitava os mesmos enderecos —
    // coincidencia, nao correcao.
    //
    // Reescrever so os tres descritores em vez de chamar SetupForResize: aquele realoca todos os
    // alvos fullscreen e os reservoirs, que nao tem nada a ver com a mudanca. O CHAMADOR garante
    // as filas drenadas — sobrescrever descritor que a GPU esta lendo e o mesmo modo de falha.
    void FReSTIRDI::RefreshMeshLightDescriptors(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                                u32 MeshLightSlot, u32 MeshAliasSlot) {
        if (!Ready || MeshLightSlot == kInvalidSlot || MeshAliasSlot == kInvalidSlot) return;

        auto CopyOne = [&](u32 DstSlot, u32 SrcSlot) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src = SRVHeap.CpuHandleStaging(SrcSlot);
            UINT One = 1;
            Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        // As DUAS paridades e os DOIS frames em voo: o dispatch escolhe a tabela por
        // [paridade][FrameSlot], entao deixar qualquer combinacao para tras significa que o
        // defeito volta em um frame a cada dois — que e pior de diagnosticar que voltar sempre.
        for (u32 p = 0; p < kParityCount; ++p) {
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                if (InitialTable[p][f] != kInvalidSlot) {
                    CopyOne(InitialTable[p][f] + kInitialMeshLightIdx, MeshLightSlot);
                    CopyOne(InitialTable[p][f] + kInitialMeshAliasIdx, MeshAliasSlot);
                }
                if (SpatialTable[p][f] != kInvalidSlot)
                    CopyOne(SpatialTable[p][f] + kSpatialMeshLightIdx, MeshLightSlot);
            }
        }
    }

    void FReSTIRDI::SetupNrdPack(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                 u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                                 u32 DepthSlot, u32 VelocitySlot,
                                 ID3D12Resource* NrdInViewZ,
                                 ID3D12Resource* NrdInNormalRough,
                                 ID3D12Resource* NrdInMv,
                                 ID3D12Resource* NrdInDiffRadHit,
                                 ID3D12Resource* NrdInSpecRadHit,
                                 ID3D12Resource* NrdOutDiff,
                                 ID3D12Resource* NrdOutSpec) {
        NrdReady = false;
        // Idempotente: antes isto so era chamado depois de um ReleaseResize, que zerava tudo.
        // Com a alocacao do NRD sob demanda (ver Renderer::ReconcileNrdAllocation) ele passa a
        // ser chamado a cada liga/desliga do denoiser — sem devolver os slots, cada toggle
        // vazaria descritores do heap ate estourar.
        {
            auto FreeIf = [&](u32& Slot, u32 Count) {
                if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
            };
            FreeIf(NrdPackSrvTable, kNrdPackSRVs);
            FreeIf(NrdPackUavTable, kNrdPackUAVs);
            FreeIf(NrdCompositeSrvTable, kNrdCompositeSRVs);
            FreeIf(NrdOutDiffuseSRV, 1);
            FreeIf(NrdOutSpecularSRV, 1);
        }
        if (!Ready || !NrdInViewZ || !NrdInNormalRough || !NrdInMv ||
            !NrdInDiffRadHit || !NrdInSpecRadHit || !NrdOutDiff || !NrdOutSpec ||
            GBufferASlot == kInvalidSlot || GBufferBSlot == kInvalidSlot ||
            GBufferCSlot == kInvalidSlot || DepthSlot == kInvalidSlot ||
            VelocitySlot == kInvalidSlot) return;

        auto MakeUAV = [&](ID3D12Resource* Resource, DXGI_FORMAT Format, u32& Slot) {
            Slot = SRVHeap.Allocate(1);
            D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
            U.Format = Format;
            U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            SRVHeap.CreateUAV(Device, Resource, U, Slot);
        };
        auto MakeSRV = [&](ID3D12Resource* Resource, DXGI_FORMAT Format, u32& Slot) {
            Slot = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format = Format;
            S.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            S.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.Texture2D.MipLevels = 1;
            SRVHeap.CreateSRV(Device, Resource, S, Slot);
        };
        auto CopyTable = [&](u32 DstSlot, const u32* Slots, u32 Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[8]{};
            UINT SrcCounts[8]{};
            for (u32 i = 0; i < Count; ++i) {
                Src[i] = SRVHeap.CpuHandleStaging(Slots[i]);
                SrcCounts[i] = 1;
            }
            UINT DstCount = Count;
            Device->CopyDescriptors(1, &Dst, &DstCount, Count, Src, SrcCounts,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        u32 NrdInputUAV[kNrdPackUAVs]{};
        MakeUAV(NrdInViewZ,       DXGI_FORMAT_R32_FLOAT,             NrdInputUAV[0]);
        MakeUAV(NrdInNormalRough, DXGI_FORMAT_R10G10B10A2_UNORM,    NrdInputUAV[1]);
        MakeUAV(NrdInMv,          DXGI_FORMAT_R16G16_FLOAT,          NrdInputUAV[2]);
        MakeUAV(NrdInDiffRadHit,  DXGI_FORMAT_R16G16B16A16_FLOAT,   NrdInputUAV[3]);
        MakeUAV(NrdInSpecRadHit,  DXGI_FORMAT_R16G16B16A16_FLOAT,   NrdInputUAV[4]);
        MakeSRV(NrdOutDiff, DXGI_FORMAT_R16G16B16A16_FLOAT, NrdOutDiffuseSRV);
        MakeSRV(NrdOutSpec, DXGI_FORMAT_R16G16B16A16_FLOAT, NrdOutSpecularSRV);

        NrdPackSrvTable = SRVHeap.Allocate(kNrdPackSRVs);
        const u32 PackSRVs[kNrdPackSRVs] = {
            RawDiffuseSRV, RawSpecularSRV, GBufferASlot, GBufferBSlot, GBufferCSlot,
            DepthSlot, VelocitySlot, ShadowMotionSRV };
        CopyTable(NrdPackSrvTable, PackSRVs, kNrdPackSRVs);

        NrdPackUavTable = SRVHeap.Allocate(kNrdPackUAVs);
        CopyTable(NrdPackUavTable, NrdInputUAV, kNrdPackUAVs);
        for (u32& Slot : NrdInputUAV) SRVHeap.Free(Slot, 1);

        NrdCompositeSrvTable = SRVHeap.Allocate(kNrdCompositeSRVs);
        const u32 CompositeSRVs[kNrdCompositeSRVs] = {
            NrdOutDiffuseSRV, NrdOutSpecularSRV,
            GBufferASlot, GBufferBSlot, GBufferCSlot, DepthSlot };
        CopyTable(NrdCompositeSrvTable, CompositeSRVs, kNrdCompositeSRVs);
        NrdReady = true;
    }

    void FReSTIRDI::UpdatePerFrame(u32 NewFrameSlot, const Mat44& InvViewProj,
                                   const Mat44& View,
                                   const Mat44& PrevViewProj,
                                   const Vec3& CameraPos, u32 NewWidth, u32 NewHeight,
                                   u32 FrameIndex, u32 LightCount, u32 ShadowRayMask,
                                   bool EnableTemporalPermutation,
                                   u32 TemporalInstanceCount, bool MotionHistoryValid,
                                   u32 MeshLightCount) {
        FrameSlot = NewFrameSlot;
        if (!MappedCB || NewWidth == 0 || NewHeight == 0) return;
        CPU.InvViewProj = InvViewProj;
        CPU.CameraPos = { CameraPos.X, CameraPos.Y, CameraPos.Z,
                          static_cast<f32>(TemporalInstanceCount) };
        CPU.ScreenParams = { static_cast<f32>(NewWidth), static_cast<f32>(NewHeight),
                             1.0f / static_cast<f32>(NewWidth),
                             1.0f / static_cast<f32>(NewHeight) };
        CPU.Params = { static_cast<f32>(LightCount), static_cast<f32>(FrameIndex),
                       static_cast<f32>(ShadowRayMask), RayEps.VisRayEndMargin };
        // MCapRatio e RELATIVO, como o paper ("clamp do M do frame anterior a no maximo 20x o M do
        // reservoir do frame atual"). O shader continua tratando Sampling.y como teto ABSOLUTO, e
        // por isso a multiplicacao mora aqui: os dois passes leem o mesmo campo e nao ha como um
        // capar em 20 e o outro em 160. So passou a caber depois do ResB virar UINT (M em 16 bits).
        CPU.Sampling = { static_cast<f32>(InitialCandidates),
                         MCapRatio * static_cast<f32>(std::max(InitialCandidates, 1u)),
                         static_cast<f32>(SpatialCount), SpatialRadius };
        // MotionHistoryValid entrou na conta junto com a saida do X1 do reservoir: sem historico de
        // superficie do frame anterior nao ha como reconstruir o ponto visivel de la, e os testes
        // de posicao/plano rodariam contra uma pose velha. Perde-se um frame de acumulo, que e
        // exatamente o que ja acontecia numa disoclusao.
        CPU.Reuse = { (Temporal && !NeedsClear && MotionHistoryValid) ? 1.0f : 0.0f,
                      PosRejectScale, NormalReject, MaxAge };
        CPU.TemporalPolicy = { EnableTemporalPermutation ? 1.0f : 0.0f,
                               MotionHistoryValid ? 1.0f : 0.0f,
                               InitialVisibility ? 1.0f : 0.0f,
                               MeshLightsInPool ? static_cast<f32>(MeshLightCount) : 0.0f };
        CPU.RayEpsA = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                        RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB = { RayEps.ShadowRayTMin, RayEps.VisRayTMin,
                        RayEps.VisRayEndMargin, FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.View = View;
        CPU.PrevViewProj = PrevViewProj;
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReSTIRDIConstants),
                    &CPU, sizeof(CPU));
    }

    void FReSTIRDI::Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                               D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After) {
        if (State == After) return;
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = Resource;
        Barrier.Transition.StateBefore = State;
        Barrier.Transition.StateAfter = After;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CL->ResourceBarrier(1, &Barrier);
        State = After;
    }

    void FReSTIRDI::Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                            FGpuProfiler* Profiler) {
        if (!Ready) return;
        const u32 P = FrameParity;
        const u32 Prev = 1u - P;
        Transition(CL, ResW[Prev].Get(), ResWState[Prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ResB[Prev].Get(), ResBState[Prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ResW[P].Get(), ResWState[P], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, ResB[P].Get(), ResBState[P], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, RawDiffuse.Get(), RawDiffuseState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, RawSpecular.Get(), RawSpecularState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, ShadowMotion.Get(), ShadowMotionState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (Profiler) Profiler->Begin(CL, "Amostragem + temporal");
        InitialTemporalPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(InitialTable[P][FrameSlot]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(InitialUAVTable[P]));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        if (Profiler) Profiler->End(CL);

        D3D12_RESOURCE_BARRIER Uav[2]{};
        for (u32 i = 0; i < 2; ++i) Uav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Uav[0].UAV.pResource = ResW[P].Get();
        Uav[1].UAV.pResource = ResB[P].Get();
        CL->ResourceBarrier(2, Uav);
        Transition(CL, ResW[P].Get(), ResWState[P], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ResB[P].Get(), ResBState[P], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (Profiler) Profiler->Begin(CL, "Espacial + visibilidade");
        SpatialPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(SpatialTable[P][FrameSlot]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SpatialUAVTable));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        if (Profiler) Profiler->End(CL);

        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(CL, RawDiffuse.Get(), RawDiffuseState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, RawSpecular.Get(), RawSpecularState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ShadowMotion.Get(), ShadowMotionState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        NeedsClear = false;
        FrameParity = Prev;
    }

    void FReSTIRDI::RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!NrdReady) return;
        NrdPackPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(NrdPackSrvTable));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(NrdPackUavTable));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
    }

    void FReSTIRDI::RecordNrdComposite(ID3D12GraphicsCommandList* CL,
                                       FTextureSRVHeap& SRVHeap) {
        if (!NrdReady) return;
        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        NrdCompositePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(NrdCompositeSrvTable));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(OutUAV));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    FPassShaderStems FReSTIRDI::ShaderStems() const {
        static const char* const kStems[] = { "ReSTIRDIInitialTemporal.cs", "ReSTIRDISpatial.cs", "ReSTIRDINrdPack.cs", "ReSTIRDINrdComposite.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FReSTIRDI::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Ready) RecreatePSO(_Ctx.Device);
    }

}
