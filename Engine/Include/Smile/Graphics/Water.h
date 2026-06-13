#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Mat44.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FWaterRenderer {
    public:
        enum class EDebugMode : u32 {
            Off          = 0,
            Wireframe    = 1,
            Tiles        = 2,
            Displacement = 3,
            Normal       = 4,
            Fresnel      = 5,
            Body         = 6,
            Reflection   = 7,
            Foam         = 8,
            Depth        = 9,
        };

        void Initialize(ID3D12Device* Device, u32 SampleCount,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Resize(ID3D12Device* Device, u32 Width, u32 Height); 

        void UpdatePerFrame(u32 FrameSlot, const Mat44& ViewProj, const Mat44& Projection, const Mat44& InvViewProj,
                            const Vec3& CameraPos, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, f32 ElapsedTime,
                            bool IBLEnabled, f32 IBLIntensity,
                            u32 ScreenW, u32 ScreenH, f32 NearZ, f32 FarZ, bool HasSceneCopies,
                            bool UseAtmosphereSky);

        void RenderSurface(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                           u32 SpecularCubeSRVSlot, u32 FFTDisplacementSRVSlot,
                           u32 FFTNormalSRVSlot, u32 SceneCopyTableStart,
                           u32 AtmosphereSkyViewSRVSlot);

        bool IsInitialized() const { return PSO != nullptr; }
        void SetDebugMode(EDebugMode Mode) { DebugMode = Mode; }
        EDebugMode GetDebugMode() const    { return DebugMode; }
        void SetGpuFrustumCull(bool V)     { UseGpuFrustumCull = V; }
        bool GetGpuFrustumCull() const     { return UseGpuFrustumCull; }
        void SetTileBaseSize(f32 V)        { TileBaseSize = V < 1.0f ? 1.0f : V; }
        f32  GetTileBaseSize() const       { return TileBaseSize; }
        void SetTileMaxDepth(u32 V)        { TileMaxDepth = V > 10u ? 10u : V; }
        u32  GetTileMaxDepth() const       { return TileMaxDepth; }
        void SetGpuRingRadius(u32 V) {
            if (V < 2u) V = 2u;
            if (V > 32u) V = 32u;
            GpuTileBuildRequestedRingRadius = V;
        }
        u32 GetGpuRingRadius() const { return GpuTileBuildRequestedRingRadius; }

        struct WaterDebugStats {
            bool GpuDrawPath  = false;
            bool GpuTileBuild = false;
            u32 CandidateCount = 0;
            u32 ValidTileCount = 0;
            u32 CountedTileCount = 0;
            u32 ScatteredTileCount = 0;
            u32 DrawCommandCount = 0;
            u32 FrustumCulledCount = 0;
            u32 CoveredByFinerCount = 0;
            u32 OutOfBoundsCount = 0;
            u32 BucketCount = 0;
            u32 LevelCount = 0;
            u32 RingRadius = 0;
            u32 Depth = 0;
            f32 RootX = 0.0f;
            f32 RootZ = 0.0f;
            f32 BaseSize = 0.0f;
            f32 RootSize = 0.0f;
        };
        const WaterDebugStats& GetDebugStats() const { return DebugStats; }

        void SetWaterLevel(f32 V)        { WaterLevel = V; }
        void SetWindDirection(f32 Rad)   { WindDir = Rad; }
        f32  GetWindDirection() const    { return WindDir; }
        void SetWindSpeed(f32 V)         { WindSpeed = V; }
        void SetWavesAmount(f32 V)       { WavesAmount = V; }
        void SetWavesSize(f32 V)         { WavesSize = V; }
        f32  GetWindSpeed() const        { return WindSpeed; }
        f32  GetWavesAmount() const      { return WavesAmount; }
        f32  GetWavesSize() const        { return WavesSize; }
        void SetDeepColor(const Vec3& C) { DeepColor = C; }
        void SetReflectionScale(f32 V)   { ReflectionScale = V; }
        void SetReflectionBumpScale(f32 V) { ReflectionBumpScale = V; }
        void SetFresnelGloss(f32 V)      { FresnelGloss = V; }
        f32  GetReflectionScale() const  { return ReflectionScale; }
        f32  GetReflectionBumpScale() const { return ReflectionBumpScale; }
        f32  GetFresnelGloss() const     { return FresnelGloss; }
        f32  GetWaterLevel() const       { return WaterLevel; }

        void SetUseFFT(bool V)           { UseFFT = V; }
        void SetFFTDisplacementScale(f32 V) { FFTDispScale = V; }
        void SetFFTChoppyScale(f32 V)    { FFTChoppyScale = V; }
        void SetFFTNormalUp(f32 V)       { FFTNormalUp = V; }
        void SetFFTFade(f32 Start, f32 Range) { FFTFadeStart = Start; FFTFadeRange = Range; }
        f32  GetFFTDisplacementScale() const { return FFTDispScale; }
        f32  GetFFTChoppyScale() const   { return FFTChoppyScale; }
        f32  GetFFTNormalUp() const      { return FFTNormalUp; }

        void SetUseBump(bool V)          { UseBump = V; }
        void SetBumpTiling(f32 V)        { BumpTilling = V; }
        void SetBumpDetailTiling(f32 V)  { BumpDetailTilling = V; }
        void SetBumpStrength(f32 V)      { BumpStrength = V; }
        void SetParallaxHeight(f32 V)    { ParallaxHeight = V; }
        void SetBumpFadeDist(f32 V)      { BumpFadeDist = V; }
        f32  GetBumpStrength() const     { return BumpStrength; }

        void SetRefractionBumpScale(f32 V) { RefractionBumpScale = V; }
        void SetSoftIntersection(f32 V)    { SoftIntersectionFactor = V; }
        void SetFogDensity(f32 V)          { FogDensity = V; }
        f32  GetRefractionBumpScale() const { return RefractionBumpScale; }
        f32  GetFogDensity() const          { return FogDensity; }

        void SetWaterClarity(f32 V)        { WaterClarity = V < 0.1f ? 0.1f : V; }
        f32  GetWaterClarity() const       { return WaterClarity; }

        void SetInScatterColor(const Vec3& C) { InScatterColor = C; }
        void SetInScatterDensity(f32 V)       { InScatterDensity = V; }
        void SetAbsorption(const Vec3& C)     { AbsorptionColor = C; }
        void SetSunSpecClamp(f32 V)           { SunSpecClamp = V; }
        f32  GetInScatterDensity() const      { return InScatterDensity; }
        f32  GetSunSpecClamp() const          { return SunSpecClamp; }

        void SetUseFoam(bool V)            { UseFoam = V; }
        void SetFoamCoverage(f32 V)        { FoamCoverage = V; }
        void SetFoamSharpness(f32 V)       { FoamSharpness = V; }
        void SetFoamIntensity(f32 V)       { FoamIntensity = V; }
        void SetFoamFadeDist(f32 V)        { FoamFadeDist = V; }
        void SetFoamColor(const Vec3& C)   { FoamColor = C; }
        void SetFoamSpecSuppress(f32 V)    { FoamSpecSuppress = V; }
        bool GetUseFoam() const            { return UseFoam; }
        f32  GetFoamCoverage() const       { return FoamCoverage; }
        f32  GetFoamIntensity() const      { return FoamIntensity; }

        void SetSSSStrength(f32 V)         { SSSStrength = V; }
        void SetSSSPower(f32 V)            { SSSPower = V; }
        void SetSSSHeightScale(f32 V)      { SSSHeightScale = V; }
        f32  GetSSSStrength() const        { return SSSStrength; }

        void SetShoreFoamWidth(f32 V)      { ShoreFoamWidth = V; }
        void SetShoreFoamIntensity(f32 V)  { ShoreFoamIntensity = V; }
        f32  GetShoreFoamWidth() const     { return ShoreFoamWidth; }
        f32  GetShoreFoamIntensity() const { return ShoreFoamIntensity; }


    private:
        struct alignas(256) WaterConstants {
            Mat44 ViewProj;        // 64
            Mat44 InvViewProj;     // 64
            Vec4  CameraPos;       // 16  xyz, w=unused
            Vec4  SunDirection;    // 16  xyz=dir TO sun (norm), w=intensity
            Vec4  SunColor;        // 16  rgb, w=waterClarity(m)
            Vec4  OceanParams0;    // 16  x=windDir(rad) y=windSpeed z=shoreFoamIntensity w=wavesAmount
            Vec4  OceanParams1;    // 16  x=wavesSize y=FlowDir.x(cos) z=FlowDir.y(sin) w=waterLevel
            Vec4  DeepColorDensity;// 16  rgb=cor de fundo, w=fogDensity (futuro)
            Vec4  Misc;            // 16  x=time y=iblEnabled z=iblIntensity w=specMaxMip
            Vec4  WaterFXParams;   // 16  x=sssStrength y=sssPower z=sssHeightScale w=shoreFoamWidth(m)
            Vec4  ShadeParams;     // 16  x=FresnelGloss y=ReflectionScale z=SunShininess w=SunSpecScale
            Vec4  OceanFFT;        // 16  x=useFFT y=dispScale z=choppyScale w=normalUp
            Vec4  OceanFade;       // 16  x=fadeStart(m) y=fadeRange(m) — achata deslocamento/normal ao longe
            Vec4  BumpParams;      // 16  x=Tilling y=DetailTilling z=NormalsScale w=DetailNormalsScale
            Vec4  BumpParams2;     // 16  x=bumpStrength y=parallaxHeight z=useBump w=bumpFadeDist
            Vec4  ScreenParams;    // 16  x=w y=h z=1/w w=1/h
            Vec4  DepthParams;     // 16  x=near y=far z=hasSceneCopies w=useAtmosphereSky
            Vec4  RefractionParams;// 16  x=RefractionBumpScale y=SoftIntersectionFactor z=fogDensity w=ReflectionBumpScale
            Vec4  DebugParams;     // 16  x=debug mode (0 off/1 wire/2 tiles)
            Vec4  InScatterColor;  // 16  rgb=cor turquesa do in-scatter, w=densidade do in-scatter
            Vec4  AbsorptionColor; // 16  rgb=extincao por canal (Beer-Lambert), w=clamp do sun-spec
            Vec4  FoamParams;      // 16  x=coverage(limiar J) y=sharpness z=intensidade(0=off) w=fadeDist(m)
            Vec4  FoamColor;       // 16  rgb=tint da espuma, w=supressao do sun-spec
            Vec4  QuadTreeParams;  // 16  x=rootX y=rootZ z=leafSize w=rootSize
        };
        static_assert(sizeof(WaterConstants) % 256 == 0, "WaterConstants deve ser multiplo de 256");

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void BuildGenerateDrawsPipeline(ID3D12Device* Device);
        void BuildGrid(ID3D12Device* Device);
        void BuildInstanceBuffer(ID3D12Device* Device);
        void PrepareGpuTileSources(const Mat44& ViewProj, const Vec3& CameraPos);
        void DispatchGenerateDraws(ID3D12GraphicsCommandList* CommandList);
        void ReadGpuDebugCounters(u32 FrameSlot);

        static constexpr u32 kGridPoints        = 33;  // pontos por eixo (32*32*2 tris/tile no LOD 0)
        static constexpr u32 kMaxTileInstances  = 8192;
        static constexpr u32 kSubsetPatternCount = 81; // 3^4: left/right/bottom/top
        static constexpr u32 kSubsetLODCount     = 3;  // levelsize 32, 16, 8 como no corte util do Asylum
        static constexpr u32 kSubsetRangeCount   = kSubsetPatternCount * kSubsetLODCount;
        static constexpr u32 kGpuDebugCounterCount = 16;

        static constexpr f32 kSunShininess   = 256.0f; // expoente do lobo estreito do sun-spec
        static constexpr f32 kSunSpecScale   = 1.0f;   // escala global do sun-spec
        static constexpr f32 kSpecularMaxMip = 6.0f;   // mip mais alto do cubemap especular

        enum EGpuDebugCounter : u32 {
            GpuCounterCandidateThreads = 0,
            GpuCounterValidTiles       = 1,
            GpuCounterOutOfBounds      = 2,
            GpuCounterCoveredByFiner   = 3,
            GpuCounterFrustumCulled    = 4,
            GpuCounterCountedTiles     = 5,
            GpuCounterDrawCommands     = 6,
            GpuCounterScatteredTiles   = 7,
            GpuCounterTileCount        = 8,
            GpuCounterLevelCount       = 9,
            GpuCounterRingRadius       = 10,
            GpuCounterDepth            = 11,
            GpuCounterCameraCellX      = 12,
            GpuCounterCameraCellZ      = 13,
        };

        struct TileInstance {
            u32 Data0 = 0;
            u32 Data1 = 0;
            u32 Data2 = 0;
        };
        static_assert(sizeof(TileInstance) == sizeof(u32) * 3, "TileInstance must stay tightly packed");
        struct TileSource {
            u32 Data0 = 0;
            u32 Data1 = 0;
            u32 Data2 = 0;
            u32 Pad   = 0;
        };
        struct DrawBucketSource {
            u32 IndexStart    = 0;
            u32 IndexCount    = 0;
            u32 InstanceStart = 0;
            u32 InstanceCount = 0;
        };
        struct IndexRange {
            u32 Start = 0;
            u32 Count = 0;
        };

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> WireframePSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        u8*             MappedCBVBase = nullptr;
        WaterConstants* MappedCBV     = nullptr;
        u32             FrameSlot     = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VBView{};
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_INDEX_BUFFER_VIEW IBView{};
        u32 InstanceCount = 0;
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> IndirectCommandSignature;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> GenerateDrawsRootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateDrawsPSO;
        FDescriptorHeap GenerateDrawsHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> DrawBucketSourceBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuTileSourceBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuInstanceBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuIndirectArgsBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuDrawBucketScratchBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugCounterBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugReadbackBuffer;
        D3D12_RESOURCE_STATES GpuTileSourceBufferState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES GpuInstanceBufferState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES GpuIndirectArgsBufferState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES GpuDrawBucketScratchState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES GpuDebugCounterState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_VERTEX_BUFFER_VIEW GpuInstanceVBView{};
        u8* MappedDrawBucketsBase = nullptr;
        u32* MappedGpuDebugCountersBase = nullptr;
        DrawBucketSource* MappedDrawBuckets = nullptr;
        WaterDebugStats DebugStats{};
        u32 GpuTileSourceCandidateCount = 0;
        u32 GpuTileBuildLevelCount = 0;
        u32 GpuTileBuildDepth = 0;
        u32 GpuTileBuildRequestedRingRadius = 8;
        u32 GpuTileBuildRingRadius = 8;
        u32 GpuTileBuildCameraCellX = 0;
        u32 GpuTileBuildCameraCellZ = 0;
        f32 GpuTileBuildRootX = 0.0f;
        f32 GpuTileBuildRootZ = 0.0f;
        f32 GpuTileBuildBaseSize = 64.0f;
        f32 GpuTileBuildBoundsPad = 128.0f;
        Mat44 GpuTileBuildViewProj = Mat44::Identity();
        IndexRange PatternRanges[kSubsetRangeCount]{};
        EDebugMode DebugMode = EDebugMode::Off;

        f32  WaterLevel      = 0.0f;
        f32  WindDir         = 1.0f;   
        f32  WindSpeed       = 4.0f;
        f32  WavesAmount     = 1.5f;
        f32  WavesSize       = 0.75f;
        Vec3 DeepColor       = { 0.02f, 0.08f, 0.12f };
        f32  ReflectionScale = 1.0f;
        f32  FresnelGloss    = 0.9f;

        bool UseFFT          = true;
        f32  FFTDispScale    = 1.0f;   
        f32  FFTChoppyScale  = 1.5f;  
        f32  FFTNormalUp     = 8.0f;   
        f32  FFTFadeStart    = 450.0f;  
        f32  FFTFadeRange    = 2000.0f; 

        bool UseBump           = true;
        f32  BumpTilling       = 10.0f; 
        f32  BumpDetailTilling = 2.5f;  
        f32  BumpNormalsScale  = 1.25f; 
        f32  BumpDetailScale   = 0.5f; 
        f32  BumpStrength      = 0.5f;  
        f32  ParallaxHeight    = 0.0f;  
        f32  BumpFadeDist      = 250.0f;

        f32  ReflectionBumpScale    = 0.18f; 
        f32  RefractionBumpScale    = 0.1f;  
        f32  SoftIntersectionFactor = 1.0f;  
        f32  FogDensity             = 0.1f;  
        f32  WaterClarity           = 8.0f;  

        Vec3 InScatterColor   = { 0.06f, 0.30f, 0.36f };
        f32  InScatterDensity = 1.5f;
        Vec3 AbsorptionColor  = { 0.45f, 0.15f, 0.10f };
        f32  SunSpecClamp     = 2.0f;  

        bool UseFoam          = true;
        f32  FoamCoverage     = 0.62f; 
        f32  FoamSharpness    = 0.5f;  
        f32  FoamIntensity    = 1.0f;  
        f32  FoamFadeDist     = 600.0f;
        Vec3 FoamColor        = { 0.82f, 0.86f, 0.90f }; 
        f32  FoamSpecSuppress = 0.85f; 

        f32  SSSStrength      = 0.6f;  
        f32  SSSPower         = 4.0f;  
        f32  SSSHeightScale   = 0.5f;  

        f32  ShoreFoamWidth     = 6.0f; 
        f32  ShoreFoamIntensity = 1.0f;

        bool UseGpuFrustumCull     = true;
        f32 TileBaseSize           = 64.0f;
        u32 TileMaxDepth           = 9;
    };
}
