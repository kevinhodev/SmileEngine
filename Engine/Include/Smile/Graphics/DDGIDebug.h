#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>

namespace Smile {
    class FTextureSRVHeap;
    class FDDGI;

    struct FDDGIPointProbeDiagnostic {
        u32  ProbeIndex = 0xffffffffu;
        bool Active = false;
        f32  DistanceToPoint = 0.0f;
        f32  MeanDistance = 0.0f;
        f32  DistanceDeviation = 0.0f;
        f32  TrilinearWeight = 0.0f;
        f32  Visibility = 0.0f;
        f32  RawWeight = 0.0f;
        f32  NormalizedWeight = 0.0f;
        Vec3 Irradiance{};
        f32  LeakRisk = 0.0f;
    };

    struct FDDGIPointDiagnostic {
        bool Valid = false;
        Vec3 WorldPosition{};
        Vec3 WorldNormal{};
        f32  TotalWeight = 0.0f;
        std::array<FDDGIPointProbeDiagnostic, 8> Probes{};
        i32 DominantSlot = -1;
        i32 RiskSlot = -1;
        // Peso do volume de sondas no ponto: 1 = dentro, <1 = mistura com o ambiente de fora,
        // 0 = so ambiente. Os pesos por probe nao dependem dele — por isso e reportado a parte.
        f32 VolumeWeight = 1.0f;
    };

    class FDDGIDebug : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "DDGI (debug de sondas)"; }
        bool IsInitialized() const override { return ProbePSO != nullptr; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        static constexpr u32 kPointProbeCount = 8;

        enum class EMode : u32 {
            Irradiance     = 0,
            Distance       = 1, 
            Relocation     = 2, 
            Classification = 3, 
            Stability      = 4, 
            Selected       = 5,
        };

        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 NumProbes);
        void SetupPointDiagnosticInputs(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                        u32 GBufferTableStart, const FDDGI& DDGI);

        void Render(u32 FrameSlot, ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                    FDDGI& DDGI, const Mat44& ViewProj, const Vec3& CameraPos, u32 FrameIndex);
        bool RequestPointDiagnostic(u32 X, u32 Y);
        // Re-executa o ultimo ponto (ver .cpp): knob que muda o peso das probes tem que
        // reexecutar o diagnostico, senao o painel mostra o snapshot do estado anterior.
        bool RepeatPointDiagnostic();
        void CancelPointDiagnostic();
        void RecordPointDiagnostic(u32 FrameSlot, ID3D12GraphicsCommandList* CommandList,
                                   FTextureSRVHeap& SRVHeap, const FDDGI& DDGI,
                                   const Mat44& InvViewProj, const Vec3& CameraPos,
                                   u32 Width, u32 Height, u32 GIFlags);
        void CollectPointDiagnostic(u32 FrameSlot);
        bool ConsumePointDiagnostic(FDDGIPointDiagnostic& OutDiagnostic);

        void  SetEnabled(bool V)     { Enabled = V; }
        bool  GetEnabled() const     { return Enabled; }
        void  SetMode(EMode M)       { Mode = M; }
        EMode GetMode() const        { return Mode; }
        void  SetProbeRadius(f32 V)  { ProbeRadius = V; }
        f32   GetProbeRadius() const { return ProbeRadius; }
        void  SetShowVolume(bool V)  { ShowVolume = V; }
        bool  GetShowVolume() const  { return ShowVolume; }
        void  SetShowRays(bool V)    { ShowRays = V; }
        bool  GetShowRays() const    { return ShowRays; }
        void  SetRayRadius(f32 V)    { RayRadius = V; } 
        f32   GetRayRadius() const   { return RayRadius; }
        void  SetSelectedProbe(i32 V);
        void  SetSelectedProbes(const u32* ProbeIndices, const f32* Weights,
                                u32 Count, i32 RiskSlot);
        i32   GetSelectedProbe() const {
            return SelectedProbeCount > 0 ? SelectedProbes[0] : -1;
        }

    private:
        struct alignas(256) DDGIDebugConstants {
            Mat44 ViewProj;        // 64
            Vec4  GridMinSpacing;  // xyz = origem do grid, w = espacamento
            Vec4  GridCount;       // xyz = probes por eixo, w = numProbes
            Vec4  AtlasParams;     // x = tile, y = atlasW, z = atlasH, w = -
            Vec4  DistAtlasParams; // x = distTile, y = distW, z = distH, w = maxRayDist
            Vec4  DebugParams;     // x = mode, y = probeRadius, z = relocMaxOffset, w = deactivThreshold
            Vec4  CameraPos;       // xyz = camera, w = probe focada
            Vec4  RayParams;       // x = frameIndex, y = rayRadius (world), z = -, w = -
            Vec4  SelectedIndices0;
            Vec4  SelectedIndices1;
            Vec4  SelectedWeights0;
            Vec4  SelectedWeights1;
            u8    _Tail[256 - 64 - 11 * 16] = {};
        };
        static_assert(sizeof(DDGIDebugConstants) == 256, "DDGIDebugConstants must be 256 bytes");

        struct alignas(256) PointDiagnosticConstants {
            Mat44 InvViewProj;
            Vec4  GridMinSpacing;
            Vec4  GridCount;
            Vec4  AtlasParams;
            Vec4  DistAtlasParams;
            Vec4  CameraPositionFlags;
            Vec4  PixelParams;
            Vec4  BiasParams; // x = escala do self-shadow bias, y = teto em metros (0 = sem teto)
            u8    _Tail[256 - 64 - 7 * 16] = {};
        };
        static_assert(sizeof(PointDiagnosticConstants) == 256,
                      "PointDiagnosticConstants must be 256 bytes");

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void CreatePointDiagnosticResources(ID3D12Device* Device);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbePSO;  
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VolumePSO; 
        Microsoft::WRL::ComPtr<ID3D12PipelineState> RaysPSO;   
        FVolumetricPipeline StatsPSO; 
        FVolumetricPipeline PointDiagnosticPSO;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeStatsBuf;
        u32 ProbeStatsSRVSlot = kInvalidSlot;
        u32 ProbeStatsUAVSlot = kInvalidSlot;
        u32 NumProbes = 0;
        D3D12_RESOURCE_STATES ProbeStatsState = D3D12_RESOURCE_STATE_COMMON;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCBBase = nullptr;

        // +1 no fim = peso do volume; espelha DDGI_POINT_ROWS no DDGIDebugPoint.cs.hlsl.
        static constexpr u32 kPointOutputRows    = 3 + kPointProbeCount * 3;
        static constexpr u32 kPointRowVolumeIdx  = 2 + kPointProbeCount * 3;
        Microsoft::WRL::ComPtr<ID3D12Resource> PointDiagnosticCB;
        u8* PointDiagnosticMappedCB = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> PointDiagnosticOutput;
        Microsoft::WRL::ComPtr<ID3D12Resource> PointDiagnosticReadback[2];
        bool PointReadbackPending[2] = {};
        u64  PointReadbackVersion[2] = {};
        D3D12_RESOURCE_STATES PointOutputState = D3D12_RESOURCE_STATE_COMMON;
        u32 PointInputTableStart = kInvalidSlot;
        u32 PointOutputUAVSlot = kInvalidSlot;
        bool PointInputsReady = false;
        bool PointRequestPending = false;
        bool PointHasLastRequest = false; // ja houve um clique: habilita o RepeatPointDiagnostic
        u32 PointRequestX = 0;
        u32 PointRequestY = 0;
        u64 PointRequestVersion = 1;
        bool PointResultReady = false;
        FDDGIPointDiagnostic PointResult;

        bool  Enabled     = false;
        EMode Mode        = EMode::Irradiance;
        f32   ProbeRadius = 0.10f; 
        bool  ShowVolume  = true;
        bool  ShowRays    = false; 
        f32   RayRadius   = 6.0f;  
        std::array<i32, kPointProbeCount> SelectedProbes{
            -1, -1, -1, -1, -1, -1, -1, -1
        };
        std::array<f32, kPointProbeCount> SelectedWeights{};
        u32 SelectedProbeCount = 0;
        i32 SelectedRiskSlot = -1;
        i32 FocusedProbe = -1;
    };
}
