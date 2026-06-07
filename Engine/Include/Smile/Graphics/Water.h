#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Mat44.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Renderiza a superficie do oceano — port fiel da CryEngine.
    //
    // Equivalente Smile do par COcean + CREWaterOcean + Water.cfx da Cry:
    //   - quadtree CPU/tiled LOD em world-space, inspirada no sistema do Asylum/UE,
    //     para evitar o anel de horizonte do projected grid;
    //   - shading Fresnel misturando cor de fundo <-> reflexao (Water.cfx:1000+).
    //
    // Etapa 0: agua PLANA refletindo o ceu (cubemap especular do FHDREnvironment +
    // fallback de ceu analitico) + sun specular. Saida em HDR linear (sem tonemap
    // por-shader — especificidade da Smile, ver DOCS/ARCHITECTURE). FFT/bump/refracao
    // entram nas etapas seguintes.
    //
    // Segue o contrato de subsistema grafico da engine (molde: FSkybox).
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

        // SampleCount/RTFormat/DSFormat devem casar com o RT de cena ativo (MSAA/HDR).
        void Initialize(ID3D12Device* Device, u32 SampleCount,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Resize(ID3D12Device* Device, u32 Width, u32 Height); // no-op ate a Etapa 3

        // Atualiza o constant buffer do frame. ViewProj = View*Projection (row-major,
        // convencao row-vector da engine). Projection alimenta o coverage da quadtree
        // no estilo Asylum; InvViewProj fica no CB por compatibilidade com shaders.
        void UpdatePerFrame(u32 FrameSlot, const Mat44& ViewProj, const Mat44& Projection, const Mat44& InvViewProj,
                            const Vec3& CameraPos, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, f32 ElapsedTime,
                            bool IBLEnabled, f32 IBLIntensity,
                            u32 ScreenW, u32 ScreenH, f32 NearZ, f32 FarZ, bool HasSceneCopies,
                            bool UseAtmosphereSky);

        // Desenha a superficie. O caller ja deve ter setado viewport/scissor/RT/DSV e os
        // descriptor heaps. Binda o cubemap especular (t0), o displacement do FFT (t1),
        // a tabela contigua [scene-color (t2), scene-depth (t3)] e o normal-map mipped
        // derivado do FFT (t4) e a SkyViewLUT da atmosfera fisica (t5).
        void RenderSurface(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                           u32 SpecularCubeSRVSlot, u32 FFTDisplacementSRVSlot,
                           u32 FFTNormalSRVSlot, u32 SceneCopyTableStart,
                           u32 AtmosphereSkyViewSRVSlot);

        bool IsInitialized() const { return PSO != nullptr; }
        void SetDebugMode(EDebugMode Mode) { DebugMode = Mode; }
        EDebugMode GetDebugMode() const    { return DebugMode; }

        // --- Setters do editor (Etapa 5). Defaults estilo Cry (3dEngineLoad.cpp:1369). ---
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
        // FFT (Etapa 1).
        void SetUseFFT(bool V)           { UseFFT = V; }
        void SetFFTDisplacementScale(f32 V) { FFTDispScale = V; }
        void SetFFTChoppyScale(f32 V)    { FFTChoppyScale = V; }
        void SetFFTNormalUp(f32 V)       { FFTNormalUp = V; }
        void SetFFTFade(f32 Start, f32 Range) { FFTFadeStart = Start; FFTFadeRange = Range; }
        f32  GetFFTDisplacementScale() const { return FFTDispScale; }
        f32  GetFFTChoppyScale() const   { return FFTChoppyScale; }
        f32  GetFFTNormalUp() const      { return FFTNormalUp; }
        // Bump de detalhe + scroll por vento + parallax (Etapa 2).
        void SetUseBump(bool V)          { UseBump = V; }
        void SetBumpTiling(f32 V)        { BumpTilling = V; }
        void SetBumpDetailTiling(f32 V)  { BumpDetailTilling = V; }
        void SetBumpStrength(f32 V)      { BumpStrength = V; }
        void SetParallaxHeight(f32 V)    { ParallaxHeight = V; }
        void SetBumpFadeDist(f32 V)      { BumpFadeDist = V; }
        f32  GetBumpStrength() const     { return BumpStrength; }
        // Refração / fog (Etapa 3).
        void SetRefractionBumpScale(f32 V) { RefractionBumpScale = V; }
        void SetSoftIntersection(f32 V)    { SoftIntersectionFactor = V; }
        void SetFogDensity(f32 V)          { FogDensity = V; }
        f32  GetRefractionBumpScale() const { return RefractionBumpScale; }
        f32  GetFogDensity() const          { return FogDensity; }
        // In-scattering (turquesa "dentro d'agua") + absorcao por canal (Etapa 3, estilo Cry/SLW).
        void SetInScatterColor(const Vec3& C) { InScatterColor = C; }
        void SetInScatterDensity(f32 V)       { InScatterDensity = V; }
        void SetAbsorption(const Vec3& C)     { AbsorptionColor = C; }
        void SetSunSpecClamp(f32 V)           { SunSpecClamp = V; }
        f32  GetInScatterDensity() const      { return InScatterDensity; }
        f32  GetSunSpecClamp() const          { return SunSpecClamp; }
        // Foam / whitecaps (Jacobiano da choppy do FFT, estilo Asylum/Cry).
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
        // Aerial perspective da superficie no horizonte (HDR, pos-water, estilo Cry).
        void SetUseAerialFog(bool V)       { UseAerialFog = V; }
        void SetAerialFog(f32 Start, f32 Range, f32 Density) {
            AerialFogStart = Start; AerialFogRange = Range; AerialFogDensity = Density;
        }
        // Blend estilo Asylum: FFT/choppy perto, swell procedural suave no medio-longe.
        void SetFarSwellBlend(f32 Start, f32 Range, f32 Height, f32 NormalStrength) {
            FarBlendStart = Start; FarBlendRange = Range;
            FarSwellHeight = Height; FarSwellNormalStrength = NormalStrength;
        }

    private:
        // Layout casa campo-a-campo com o cbuffer WaterCB (WaterCommon.hlsli).
        struct alignas(256) WaterConstants {
            Mat44 ViewProj;        // 64
            Mat44 InvViewProj;     // 64
            Vec4  CameraPos;       // 16  xyz, w=unused
            Vec4  SunDirection;    // 16  xyz=dir TO sun (norm), w=intensity
            Vec4  SunColor;        // 16  rgb, w=unused
            Vec4  OceanParams0;    // 16  x=windDir(rad) y=windSpeed z=0 w=wavesAmount  (Cry OceanParams0)
            Vec4  OceanParams1;    // 16  x=wavesSize y=FlowDir.x(cos) z=FlowDir.y(sin) w=waterLevel
            Vec4  DeepColorDensity;// 16  rgb=cor de fundo, w=fogDensity (futuro)
            Vec4  Misc;            // 16  x=time y=iblEnabled z=iblIntensity w=specMaxMip
            Vec4  ProjGrid;        // 16  x=maxDist y=overscan z=horizonBias w=nearClampEps
            Vec4  ShadeParams;     // 16  x=FresnelGloss y=ReflectionScale z=SunShininess w=SunSpecScale
            Vec4  OceanFFT;        // 16  x=useFFT y=dispScale z=choppyScale w=normalUp ("8" da Cry)
            Vec4  OceanFade;       // 16  x=fadeStart(m) y=fadeRange(m) — achata deslocamento/normal ao longe
            Vec4  BumpParams;      // 16  x=Tilling y=DetailTilling z=NormalsScale w=DetailNormalsScale
            Vec4  BumpParams2;     // 16  x=bumpStrength y=parallaxHeight z=useBump w=bumpFadeDist
            Vec4  ScreenParams;    // 16  x=w y=h z=1/w w=1/h
            Vec4  DepthParams;     // 16  x=near y=far z=hasSceneCopies w=unused
            Vec4  RefractionParams;// 16  x=RefractionBumpScale y=SoftIntersectionFactor z=fogDensity w=ReflectionBumpScale
            Vec4  AerialFogParams; // 16  x=start(m) y=range(m) z=density w=0 off/1 HDR/2 physical sky
            Vec4  FarBlendParams;  // 16  x=FFT->swell start y=range z=swell height w=normal strength
            Vec4  DebugParams;     // 16  x=debug mode (0 off/1 wire/2 tiles)
            Vec4  InScatterColor;  // 16  rgb=cor turquesa do in-scatter, w=densidade do in-scatter
            Vec4  AbsorptionColor; // 16  rgb=extincao por canal (Beer-Lambert), w=clamp do sun-spec
            Vec4  FoamParams;      // 16  x=coverage(limiar J) y=sharpness z=intensidade(0=off) w=fadeDist(m)
            Vec4  FoamColor;       // 16  rgb=tint da espuma, w=supressao do sun-spec
        };
        static_assert(sizeof(WaterConstants) % 256 == 0, "WaterConstants deve ser multiplo de 256");

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void BuildGrid(ID3D12Device* Device);
        void BuildInstanceBuffer(ID3D12Device* Device);
        void BuildTileInstances(const Mat44& ViewProj, const Mat44& Projection,
                                const Vec3& CameraPos, u32 ScreenW, u32 ScreenH);

        static constexpr u32 kGridPoints        = 33;  // pontos por eixo (32*32*2 tris/tile no LOD 0)
        static constexpr u32 kMaxTileInstances  = 8192;
        static constexpr u32 kSubsetPatternCount = 81; // 3^4: left/right/bottom/top
        static constexpr u32 kSubsetLODCount     = 3;  // levelsize 32, 16, 8 como no corte util do Asylum
        static constexpr u32 kSubsetRangeCount   = kSubsetPatternCount * kSubsetLODCount;

        struct TileInstance {
            Vec4 Data; // x=originX y=originZ z=size w=subset range (lod interno + pattern)
            Vec4 Morph; // x=geomorph 0..1 y=coverage z/w=debug/reserved
        };
        struct IndexRange {
            u32 Start = 0;
            u32 Count = 0;
        };

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> WireframePSO;

        // CB double-buffered: kFramesInFlight copias. MappedCBVBase eh a base mapeada;
        // MappedCBV/FrameSlot sao repontados por frame no UpdatePerFrame.
        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        u8*             MappedCBVBase = nullptr;
        WaterConstants* MappedCBV     = nullptr;
        u32             FrameSlot     = 0;

        // Grid local [0,1]^2, instanciado pelos tiles da quadtree.
        // z guarda edge Chebyshev para debug/futuros skirts.
        // O VS posiciona tudo em world-space.
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VBView{};
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_INDEX_BUFFER_VIEW IBView{};
        // Buffer de instancias tambem double-buffered (reescrito por frame em
        // BuildTileInstances). MappedInstances eh repontado p/ a regiao do frame.
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceBuffer;
        D3D12_VERTEX_BUFFER_VIEW InstanceVBView{};
        u8*           MappedInstancesBase = nullptr;
        TileInstance* MappedInstances     = nullptr;
        u32 InstanceCount = 0;
        IndexRange PatternRanges[kSubsetRangeCount]{};
        u32 PatternInstanceStarts[kSubsetRangeCount]{};
        u32 PatternInstanceCounts[kSubsetRangeCount]{};
        EDebugMode DebugMode = EDebugMode::Off;

        // Parametros (defaults Cry-like).
        f32  WaterLevel      = 0.0f;
        f32  WindDir         = 1.0f;   // rad (3dEngineLoad: WindDirection=1.0)
        f32  WindSpeed       = 4.0f;   // (WindSpeed=4.0)
        f32  WavesAmount     = 1.5f;   // (WavesAmount=1.5, clamp 0.4..3.0)
        f32  WavesSize       = 0.75f;  // (WavesSize=0.75, clamp 0..3.0)
        Vec3 DeepColor       = { 0.02f, 0.08f, 0.12f };
        f32  ReflectionScale = 1.0f;
        f32  FresnelGloss    = 0.9f;   // Water.cfx PM1.y default

        // FFT (Etapa 1) — knobs de tuning sobre o port verbatim do CWaterSim.
        bool UseFFT          = true;
        f32  FFTDispScale    = 1.0f;   // multiplicador mestre do deslocamento
        f32  FFTChoppyScale  = 1.5f;   // Cry: componentes horizontais x1.5
        f32  FFTNormalUp     = 8.0f;   // Cry: componente "up" do normal on-the-fly
        f32  FFTFadeStart    = 450.0f;  // ondas cheias ate aqui (m) — so a faixa do horizonte achata
        f32  FFTFadeRange    = 2000.0f; // achatam linearmente nessa faixa (anti-shimmer no horizonte)

        // Bump de detalhe (Etapa 2). Usa a textura FFT em freqs mais finas + scroll por vento.
        bool UseBump           = true;
        f32  BumpTilling       = 10.0f; // Water.cfx PM0.z (Tilling)
        f32  BumpDetailTilling = 2.5f;  // Water.cfx PM0.w (DetailTilling)
        f32  BumpNormalsScale  = 1.25f; // Water.cfx PM2.w (NormalsScale)
        f32  BumpDetailScale   = 0.5f;  // Water.cfx PM2.z (DetailNormalsScale)
        f32  BumpStrength      = 0.5f;  // peso global da perturbacao de detalhe
        f32  ParallaxHeight    = 0.0f;  // Water.cfx HeightScale (parallax) — 0 = desligado por ora
        f32  BumpFadeDist      = 250.0f;// ripple fino some ate aqui (anti-speckle no horizonte)

        // Refração / ocean fog (Etapa 3).
        f32  ReflectionBumpScale    = 0.18f; // Water.cfx PM2.x; deixa o reflexo ler a normal FFT
        f32  RefractionBumpScale    = 0.1f;  // Water.cfx PM2.y
        f32  SoftIntersectionFactor = 1.0f;  // Water.cfx PM0.x
        f32  FogDensity             = 0.1f;  // densidade da absorcao (Beer-Lambert)
        // In-scattering (turquesa) + absorcao por canal. Defaults validados (best-of-each Etapa 4):
        // extincao (0.45,0.15,0.10) = vermelho some primeiro -> corpo azul-esverdeado;
        // in-scatter turquesa lido mesmo em coluna fina (resolve a escala da esfera demo).
        Vec3 InScatterColor   = { 0.06f, 0.30f, 0.36f };
        f32  InScatterDensity = 1.5f;
        Vec3 AbsorptionColor  = { 0.45f, 0.15f, 0.10f };
        f32  SunSpecClamp     = 2.0f;  // limita o glint do sol espalhado nas ripples (manchas brancas)
        // Foam / whitecaps. Coverage e o limiar de J (espuma onde J < coverage); sharpness
        // e a faixa de J do fade; intensidade e a opacidade mestra; fade some a espuma ao longe.
        bool UseFoam          = true;
        f32  FoamCoverage     = 0.62f; // limiar do Jacobiano (J menor que isto vira espuma)
        f32  FoamSharpness    = 0.5f;  // faixa de J ate cobertura cheia (J = coverage - sharpness)
        f32  FoamIntensity    = 1.0f;  // opacidade mestra da espuma
        f32  FoamFadeDist     = 600.0f;// espuma some linearmente ate aqui (anti-aliasing no horizonte)
        Vec3 FoamColor        = { 0.82f, 0.86f, 0.90f }; // branco levemente azulado
        f32  FoamSpecSuppress = 0.85f; // quanto a espuma abafa o glint do sol (0..1)
        bool UseAerialFog           = false;
        f32  AerialFogStart         = 900.0f;
        f32  AerialFogRange         = 3800.0f;
        f32  AerialFogDensity       = 0.18f;
        f32  FarBlendStart          = 220.0f;
        f32  FarBlendRange          = 1600.0f;
        f32  FarSwellHeight         = 0.70f;
        f32  FarSwellNormalStrength = 1.15f;

        // Quadtree: cobertura 64m * 2^9 = 32768m, split por coverage de tela.
        f32 TileBaseSize           = 64.0f;
        f32 TileCoverageThreshold  = 256.0f; // area maxima aproximada, em pixels^2, por celula
        u32 TileMaxDepth           = 9;

    };
}
