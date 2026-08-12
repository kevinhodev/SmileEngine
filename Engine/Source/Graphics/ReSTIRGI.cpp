#include "Smile/Graphics/ReSTIRGI.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/RTMasks.h"
#include "Smile/Graphics/ShaderTimer.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>
#include <exception>
#include <vector>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kGIFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // Reservoir: x2 precisa da faixa inteira de fp32 (e posicao de mundo — a RTXDI tambem a
        // guarda assim); o resto vive empacotado em quatro uints. Ver ReSTIRReservoir.hlsli.
        constexpr DXGI_FORMAT kRes0Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constexpr DXGI_FORMAT kRes1Format = DXGI_FORMAT_R32G32B32A32_UINT;

        ComPtr<ID3D12Resource> CreateUAVTex2D(ID3D12Device* _Device, u32 _W, u32 _H,
                                              DXGI_FORMAT _Fmt, const char* _Label) {
            return GpuResources::CreateTex2D(
                _Device, _W, _H, _Fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI, nullptr, 1, 1, _Label);
        }

    }

    void FReSTIRGI::Initialize(ID3D12Device* _Device) {
        CreatePipelines(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FReSTIRGI::CreateConstantBuffer(ID3D12Device* _Device) {
        static_assert(sizeof(ReSTIRGIConstants) % 256 == 0,
                      "o CBAddr indexa por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(ReSTIRGIConstants), FCommandQueue::kFramesInFlight);
        CB       = Upload.Resource;
        MappedCB = Upload.Mapped;
    }

    void FReSTIRGI::SetGIParams(const Vec3& _GridMin, f32 _Spacing, const Vec3& _GridCount,
                                f32 _AtlasTile, f32 _AtlasW, f32 _AtlasH, f32 _MaxRayDist) {
        GIGridMinSpacing = { _GridMin.X, _GridMin.Y, _GridMin.Z, _Spacing };
        GIGridCount      = { _GridCount.X, _GridCount.Y, _GridCount.Z, 0.0f };
        GIAtlasParams    = { _AtlasTile, _AtlasW, _AtlasH, 0.0f };
        GIMaxRayDist     = _MaxRayDist;
    }

    void FReSTIRGI::ReleaseResize(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(GITexSRV, 1);
        Free(GITexUAV, 1);
        for (u32 i = 0; i < 2; ++i) {
            Free(Res0SRV[i], 1); Free(Res1SRV[i], 1);
            Free(Res0UAV[i], 1); Free(Res1UAV[i], 1);
            Free(TraceTable[i], 14); Free(TraceUAVTable[i], 3); Free(SpatialTable[i], 10);
            Res0[i].Reset(); Res1[i].Reset();
            Res0State[i] = Res1State[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        Free(PackSrvTable, 4); Free(PackUavTable, 4); Free(NrdOutSRV, 1);
        GITexture.Reset();
        GITextureState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FReSTIRGI::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                   u32 _Width, u32 _Height, u32 _TlasSlot, u32 _SkyViewSlot,
                                   u32 _InstanceSlot, u32 _IrradSlot, u32 _DepthSlot,
                                   u32 _GBufferSlot, u32 _VelocitySlot,
                                   u32 _DistSlot, u32 _ProbeDataSlot) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        // Valida os SETE slots, nao so tres: todos vao direto p/ CpuHandleStaging ao montar as
        // tabelas, e la kInvalidSlot (0xFFFFFFFF) vira base + 0xFFFFFFFF * HandleSize — ~128 GiB
        // fora do heap. O CopyDescriptors le desse endereco: access violation, ou pior, descriptor
        // corrompido em silencio. Hoje o Renderer garante os quatro que faltavam, mas isso e
        // invariante de call site, nao da classe.
        if (_Width == 0 || _Height == 0 || _TlasSlot == kInvalidSlot ||
            _SkyViewSlot == kInvalidSlot || _InstanceSlot == kInvalidSlot ||
            _IrradSlot == kInvalidSlot || _DepthSlot == kInvalidSlot ||
            _GBufferSlot == kInvalidSlot || _VelocitySlot == kInvalidSlot)
            return;

        Width = _Width; Height = _Height;
        DepthSlot = _DepthSlot; GBufferSlot = _GBufferSlot; VelocitySlot = _VelocitySlot;
        GITexture = CreateUAVTex2D(_Device, Width, Height, kGIFormat, "ReSTIR GI · saida");
        for (u32 i = 0; i < 2; ++i) {
            // Os dois lados do ping-pong compartilham o rotulo: o breakdown soma e mostra o custo
            // real do historico, que e o numero que interessa comparar entre builds.
            Res0[i] = CreateUAVTex2D(_Device, Width, Height, kRes0Format, "ReSTIR GI · reservoir");
            Res1[i] = CreateUAVTex2D(_Device, Width, Height, kRes1Format, "ReSTIR GI · reservoir");
        }
        GITextureState = D3D12_RESOURCE_STATE_COMMON;
        FrameParity = 0; NeedsClear = true;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        auto MakeSrvUav = [&](ID3D12Resource* Res, DXGI_FORMAT Fmt, u32& SrvSlot, u32& UavSlot) {
            SrvSlot = _SRVHeap.Allocate(1);
            UavSlot = _SRVHeap.Allocate(1);
            Srv.Format = Fmt; Uav.Format = Fmt;
            _SRVHeap.CreateSRV(_Device, Res, Srv, SrvSlot);
            _SRVHeap.CreateUAV(_Device, Res, Uav, UavSlot);
        };
        MakeSrvUav(GITexture.Get(), kGIFormat, GITexSRV, GITexUAV);
        for (u32 i = 0; i < 2; ++i) {
            MakeSrvUav(Res0[i].Get(), kRes0Format, Res0SRV[i], Res0UAV[i]);
            MakeSrvUav(Res1[i].Get(), kRes1Format, Res1SRV[i], Res1UAV[i]);
        }

        auto CopyTable = [&](u32 Dst, const D3D12_CPU_DESCRIPTOR_HANDLE* Src, UINT Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE D = _SRVHeap.CpuHandle(Dst);
            std::vector<UINT> Ones(Count, 1u);
            _Device->CopyDescriptors(1, &D, &Count, Count, Src, Ones.data(),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        for (u32 p = 0; p < 2; ++p) {
            const u32 prev = 1u - p;

            TraceTable[p] = _SRVHeap.Allocate(14);
            D3D12_CPU_DESCRIPTOR_HANDLE TSrc[13] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(_SkyViewSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_IrradSlot),
                _SRVHeap.CpuHandleStaging(_DistSlot),
                _SRVHeap.CpuHandleStaging(_ProbeDataSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_VelocitySlot),
                _SRVHeap.CpuHandleStaging(Res0SRV[prev]),
                _SRVHeap.CpuHandleStaging(Res1SRV[prev]),
                // t11/t12 continuam filler porque SetPunctualLightsSRV escreve as luzes no
                // offset 13 fixo.
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
            };
            CopyTable(TraceTable[p], TSrc, 13);

            TraceUAVTable[p] = _SRVHeap.Allocate(3);
            D3D12_CPU_DESCRIPTOR_HANDLE USrc[3] = {
                _SRVHeap.CpuHandleStaging(GITexUAV),
                _SRVHeap.CpuHandleStaging(Res0UAV[p]),
                _SRVHeap.CpuHandleStaging(Res1UAV[p]),
            };
            CopyTable(TraceUAVTable[p], USrc, 3);

            SpatialTable[p] = _SRVHeap.Allocate(10);
            D3D12_CPU_DESCRIPTOR_HANDLE SSrc[10] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(Res0SRV[p]),
                _SRVHeap.CpuHandleStaging(Res1SRV[p]),
                _SRVHeap.CpuHandleStaging(_InstanceSlot), // t3/t4: filler (ver acima)
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
            };
            CopyTable(SpatialTable[p], SSrc, 10);
        }

        Ready = true;
    }

    void FReSTIRGI::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                         u32 _StagingSlot) {
        // Este t13 e o UNICO descriptor reescrito por frame num heap shader-visible, com frames
        // em voo — o mesmo padrao que ja causou a "descriptor race t13" no projeto. O que torna
        // seguro e haver uma tabela por paridade e a paridade alternar a cada RecordTrace: ao
        // escrever a tabela p da CPU no frame N, o frame em voo mais antigo que ainda pode estar
        // lendo e o N-1, que usou a tabela 1-p. Com 3 frames em voo isso deixa de valer (o N-2
        // usou p), entao o assert quebra o build em vez de virar corrupcao intermitente.
        static_assert(kParityTables == FCommandQueue::kFramesInFlight,
                      "tabela de trace do ReSTIR versionada por paridade: com mais frames em voo "
                      "e preciso mais tabelas (ou desacoplar paridade do ping-pong do versionamento)");
        if (!Ready) return;

        // Indexado por FrameParity, NAO por FrameSlot (que e o padrao do FReflections): aqui a
        // tabela carrega qual conjunto de reservoirs e prev/curr, entao tem que ser exatamente a
        // que o RecordTrace vai bindar. Os dois indices desincronizam de vez assim que o ReSTIR
        // fica um frame desligado — FrameSlot avanca com o frame, FrameParity so dentro do
        // RecordTrace — e aí as luzes iriam parar na tabela errada.
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[FrameParity] + 13);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FReSTIRGI::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj, const Vec3& _CameraPos,
                                   u32 _Width, u32 _Height, const Vec3& _SunDir, f32 _SunIntensity,
                                   const Vec3& _SunColor, u32 _FrameIndex, f32 _SkyIntensity,
                                   const Mat44& _View, const Vec2& _JitterDeltaUv,
                                   u32 _PrevSurfaceSlot, u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.InvViewProj     = _InvViewProj;
        CPU.View            = _View;
        CPU.CameraPos       = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 1.0f };
        CPU.ScreenParams    = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.GridMinSpacing  = GIGridMinSpacing;
        CPU.GridCount       = GIGridCount;
        CPU.AtlasParams     = GIAtlasParams;
        CPU.SunDirIntensity = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        // w = ShadowRayMask. Nenhum dos dois valores inclui translucido: vidro que deixa a luz
        // passar no gather nao pode projetar sombra dura no mesmo frame.
        CPU.SunColor        = { _SunColor.X, _SunColor.Y, _SunColor.Z,
                                static_cast<f32>(FoliageShadows ? kRTMaskShadowFull
                                                                : kRTMaskShadowFast) };
        CPU.TraceParams     = { (f32)_FrameIndex, GIMaxRayDist, _SkyIntensity,
                                RayEps.HitShadowRayBias };
        // GI cru (RR/None) usa teto de firefly mais apertado: sem o NRD pra limpar o residuo, os
        // outliers viram sparkles que o RR nao remove bem. O caminho NRD mantem o teto original.
        CPU.ShadeParams     = { 0.0f, AlbedoLOD, // .x livre
                                UseNrd ? FireflyMax : FireflyMaxRaw, ValidateInterval };
        // Sem historico de superficie nao ha x1 anterior p/ reconstruir, entao o reuso temporal
        // deste frame cai — o passe volta a valer so pela amostra inicial, que e exatamente o que
        // ele ja fazia num frame de disoclusao. Nao invalida o reservoir: assim que o slot voltar,
        // o historico ainda esta la e o acumulo retoma sem piscar.
        const bool HasSurfaceHistory = _PrevSurfaceSlot != kInvalidSlot;
        CPU.ReuseParams     = { MCap, PosRejectScale, Visibility ? 1.0f : 0.0f,
                                (Temporal && HasSurfaceHistory) ? 1.0f : 0.0f };
        CPU.SpatialParams   = { SpatialRadius, SpatialCount, Spatial ? 1.0f : 0.0f, NormalReject };
        CPU.JitterParams    = { _JitterDeltaUv.X, _JitterDeltaUv.Y,
                                static_cast<f32>(_PunctualLightCount), RayEps.MaxAge }; // z = luzes (F5)
        CPU.PolicyParams    = { BackfacePolicy ? 1.0f : 0.0f, BoilingStrength,
                                TemporalBiasCorr ? 1.0f : 0.0f,
                                JacobianKillBackface ? 1.0f : 0.0f };
        CPU.RayEpsA         = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                                RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB         = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                                FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.GIDistParams    = { GIHit.DistTile, GIHit.DistAtlasW, GIHit.DistAtlasH,
                                GIHit.SkipModePacked() };
        // .w = piso de roughness do hit secundario: o reservoir e cache NAO-direcional (ver
        // FGIHitSampling::SecondaryRoughnessMin e o bloco no ShadeSurfaceHit).
        CPU.GIBiasParams    = { GIHit.BiasScale, GIHit.BiasMax, GIHit.FadeProbes,
                                GIHit.SecondaryRoughnessMin };
        CPU.ReGIRGridMinSlots     = ReGIRParams.GridMinSlots;
        CPU.ReGIRInvCellEnabled   = ReGIRParams.InvCellSizeEnabled;
        CPU.ReGIRGridCountSamples = ReGIRParams.GridCountSamples;
        CPU.ReGIRResources        = ReGIRParams.Resources;
        CPU.RadianceCacheCamCell     = RadianceCacheParams.CameraPosCell;
        CPU.RadianceCacheLodCapFlags = RadianceCacheParams.LodCapacityFlags;
        CPU.RadianceCacheResources   = RadianceCacheParams.Resources;
        CPU.SkyParams             = SkyLutParams;
        // O slot bindless viaja como float; -1 e o sentinela de "captura off" (o shader testa
        // < 0). Sem a permutacao instrumentada isto nunca e lido, mas fica coerente de todo jeito.
        CPU.DebugParams           = { (TraceTimed && TimerSlot != kInvalidSlot)
                                          ? static_cast<f32>(TimerSlot) : -1.0f,
                                      0.0f, 0.0f, 0.0f };
        CPU.HistoryParams         = { static_cast<f32>(HasSurfaceHistory ? _PrevSurfaceSlot : 0u),
                                      0.0f, 0.0f, 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReSTIRGIConstants),
                    &CPU, sizeof(ReSTIRGIConstants));
    }

    void FReSTIRGI::Transition(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _Res,
                               D3D12_RESOURCE_STATES& _State, D3D12_RESOURCE_STATES _After) {
        if (_State == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _Res;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = _State;
        B.Transition.StateAfter  = _After;
        _CL->ResourceBarrier(1, &B);
        _State = _After;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FReSTIRGI::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(ReSTIRGIConstants);
    }

    void FReSTIRGI::RecordTrace(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap,
                                 FGpuProfiler* _Profiler) {
        if (!Ready) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        const u32 p = FrameParity, prev = 1u - FrameParity;

        if (NeedsClear) {
            // Res1 e um formato INTEIRO: limpar com ClearUnorderedAccessViewFloat nele e undefined
            // behavior (a runtime le o clear value como float e o descriptor diz UINT). O que
            // importa zerar de fato e o canal de M — M == 0 e o unico sinal de "sem historico"
            // que o Pass A checa antes de tocar em qualquer outro campo.
            Transition(_CL, Res0[prev].Get(), Res0State[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const float ZeroF[4] = { 0, 0, 0, 0 };
            _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(Res0UAV[prev]),
                                               _SRVHeap.CpuHandleStaging(Res0UAV[prev]),
                                               Res0[prev].Get(), ZeroF, 0, nullptr);
            Transition(_CL, Res1[prev].Get(), Res1State[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const UINT ZeroU[4] = { 0, 0, 0, 0 };
            _CL->ClearUnorderedAccessViewUint(_SRVHeap.GpuHandle(Res1UAV[prev]),
                                              _SRVHeap.CpuHandleStaging(Res1UAV[prev]),
                                              Res1[prev].Get(), ZeroU, 0, nullptr);
            NeedsClear = false;
        }

        Transition(_CL, Res0[prev].Get(), Res0State[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Res1[prev].Get(), Res1State[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Res0[p].Get(), Res0State[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, Res1[p].Get(), Res1State[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, GITexture.Get(), GITextureState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (_Profiler) _Profiler->Begin(_CL, "Temporal + Trace Secundário");
        const bool Timed = TraceTimed && TimerSlot != kInvalidSlot;
        (Timed ? TracePSOTimed : TracePSO).Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[p]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable[p]));
        // O UAV falso da extensao: o driver troca o acesso, mas a tabela precisa estar setada.
        if (Timed)
            _CL->SetComputeRootDescriptorTable(FComputePipeline::kNvApiRootParam,
                                               FShaderTimer::ExtnTable(_SRVHeap));
        _CL->Dispatch(GX, GY, 1);
        if (_Profiler) _Profiler->End(_CL);


        if (Spatial) {
            if (_Profiler) _Profiler->Begin(_CL, "Reuso Espacial + Resolve");
            Transition(_CL, Res0[p].Get(), Res0State[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, Res1[p].Get(), Res1State[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_RESOURCE_BARRIER UB{};
            UB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UB.UAV.pResource = GITexture.Get();
            _CL->ResourceBarrier(1, &UB);

            SpatialPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpatialTable[p]));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(GITexUAV));
            _CL->Dispatch(GX, GY, 1);
            if (_Profiler) _Profiler->End(_CL);
        }

        // Estado combinado em vez de escolher por UseNrd. Sao TRES leitores: o deferred (t16,
        // passe grafico = PIXEL), o pack do NRD (compute = NON_PIXEL) e o visualizador de debug,
        // que le a GITexture CRUA — e este ultimo e registrado incondicionalmente pelo Renderer
        // (GITexRawSRVSlot) e o FDebugView nao emite barreira nenhuma. Terminar so em NON_PIXEL
        // com o NRD ligado deixava o alvo "ReSTIR GI" cru em estado invalido p/ um passe grafico:
        // erro da debug layer e leitura indefinida justamente no alvo mais util p/ debugar.
        // Custo: a mesma unica barreira de antes.
        Transition(_CL, GITexture.Get(), GITextureState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        FrameParity ^= 1u;
    }

    void FReSTIRGI::SetupNrdPack(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                 ID3D12Resource* _ViewZ, ID3D12Resource* _NormalRough,
                                 ID3D12Resource* _Mv, ID3D12Resource* _DiffRadHit, ID3D12Resource* _Out) {
        // Idempotente pelo mesmo motivo do ReSTIR DI: com o NRD alocado sob demanda
        // (Renderer::ReconcileNrdAllocation) isto e re-chamado a cada toggle do denoiser, e sem
        // devolver os slots cada troca vazaria 9 descritores.
        {
            auto FreeIf = [&](u32& Slot, u32 Count) {
                if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
            };
            FreeIf(PackUavTable, 4);
            FreeIf(PackSrvTable, 4);
            FreeIf(NrdOutSRV, 1);
        }
        // Os CINCO recursos, nao so dois: ponteiro nulo vira descriptor nulo (legal em D3D12), as
        // escritas do pack somem em silencio e o NRD denoisa lixo sem erro nenhum.
        if (!Ready || !_ViewZ || !_NormalRough || !_Mv || !_DiffRadHit || !_Out) return;

        PackUavTable = _SRVHeap.Allocate(4);
        auto MakeUav = [&](ID3D12Resource* R, DXGI_FORMAT F, u32 Slot) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; u.Format = F;
            _SRVHeap.CreateUAV(_Device, R, u, Slot);
        };
        MakeUav(_ViewZ,       DXGI_FORMAT_R32_FLOAT,          PackUavTable + 0);
        MakeUav(_NormalRough, DXGI_FORMAT_R10G10B10A2_UNORM,  PackUavTable + 1);
        MakeUav(_Mv,          DXGI_FORMAT_R16G16_FLOAT,       PackUavTable + 2);
        MakeUav(_DiffRadHit,  DXGI_FORMAT_R16G16B16A16_FLOAT, PackUavTable + 3);

        NrdOutSRV = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; s.Texture2D.MipLevels = 1;
        _SRVHeap.CreateSRV(_Device, _Out, s, NrdOutSRV);

        PackSrvTable = _SRVHeap.Allocate(4);
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(PackSrvTable);
        D3D12_CPU_DESCRIPTOR_HANDLE Src[4] = {
            _SRVHeap.CpuHandleStaging(GITexSRV),
            _SRVHeap.CpuHandleStaging(GBufferSlot),
            _SRVHeap.CpuHandleStaging(DepthSlot),
            _SRVHeap.CpuHandleStaging(VelocitySlot),
        };
        UINT N = 4; UINT Ones[4] = { 1, 1, 1, 1 };
        _Device->CopyDescriptors(1, &Dst, &N, 4, Src, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FReSTIRGI::RecordNrdPack(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || PackSrvTable == kInvalidSlot) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        NrdPackPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(PackSrvTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(PackUavTable));
        _CL->Dispatch(GX, GY, 1);
    }

    void FReSTIRGI::CreatePipelines(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "ReSTIRGITrace.cs_6_6.cso", 14, 3, true);
        SpatialPSO.Initialize(_Device, "ReSTIRGISpatial.cs_6_6.cso", 10, 1, true);
        NrdPackPSO.Initialize(_Device, "ReSTIRNrdPack.cs_6_6.cso", 4, 4, false);

        if (FShaderTimer::IsAvailable()) {
            try {
                TracePSOTimed.Initialize(_Device, "ReSTIRGITraceTimed.cs_6_6.cso", 14, 3, true, true);
                TraceTimed = true;
            } catch (const std::exception&) {
                LogWarning("ReSTIRGITraceTimed.cso Ausente — Timer do ReSTIR GI Indisponível.");
            }
        }
    }


    FPassShaderStems FReSTIRGI::ShaderStems() const {
        static const char* const kStems[] = { "ReSTIRGITrace.cs", "ReSTIRGITraceTimed.cs",
                                              "ReSTIRGISpatial.cs", "ReSTIRNrdPack.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FReSTIRGI::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
