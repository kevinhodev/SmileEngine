#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace Smile {
    // Upscaler ativo do viewport. Os ints batem com o "upscalerMode" que o bridge/QML ja usam
    // (0 = nativo, 1 = FSR); DLSS entra como 2.
    enum class EUpscaler : u8 { None = 0, FSR = 1, DLSS = 2 };

    // Denoiser do GI/reflexao (eixo separado do upscaler). NRD RELAX = caseiro; DLSS_RR = Ray
    // Reconstruction (denoiser neural que SUBSTITUI o NRD E o passe de SR num eval so). Como o RR
    // acopla denoise+upscale, selecionar DLSS_RR forca o upscaler p/ DLSS (ver Renderer::SetDenoiser).
    enum class EDenoiser : u8 { None = 0, NRD = 1, DLSS_RR = 2 };

    // Parametros de um dispatch de upscale. O FSR usa o subconjunto (cor/depth/vel/jitter/near/far/fov);
    // o DLSS (Streamline) exige tambem as matrizes de camera p/ o sl::Constants — o Renderer preenche
    // tudo sempre (custo desprezivel) e cada upscaler le o que precisa.
    struct FUpscaleParams {
        ID3D12Resource* Color    = nullptr;   // render-res, ja em COMPUTE_READ (NON_PIXEL_SHADER_RESOURCE)
        ID3D12Resource* Depth    = nullptr;
        ID3D12Resource* Velocity = nullptr;
        // FSR 3.x: pixels que mudam sem seguir apenas o motion vector (refracao/alpha) e pixels
        // compostos depois da cena opaca. Outros upscalers ignoram estes recursos.
        ID3D12Resource* Reactive                    = nullptr;
        ID3D12Resource* TransparencyAndComposition = nullptr;
        f32  JitterX = 0.0f, JitterY = 0.0f;  // pixels (o MESMO jitter aplicado a projecao)
        f32  NearZ = 0.0f, FarZ = 0.0f, FovYRadians = 0.0f;
        f32  AspectRatio  = 1.0f;
        f32  DeltaTimeSec = 0.0f;
        int  Quality = 0;         // 0=Native..4=UltraPerf (DLSS mapeia p/ DLSSMode; FSR ignora)
        bool Reset = false;
        // Somente o DLSS usa (row-major, SEM jitter):
        Mat44 ViewToClip{};       // projecao unjittered
        Mat44 ClipToPrevClip{};   // reprojecao: clip atual -> clip do frame anterior
        Vec3  CamPos{}, CamRight{}, CamUp{}, CamFwd{};

        // Guides do DLSS Ray Reconstruction (somente o FDlssRRPass usa; FSR/DLSS-SR ignoram). Todos
        // render-res, lineares. A cor (Color acima) entra RUIDOSA no RR (GI+reflexao pre-denoise). Ver
        // Shaders/PostProcess/DlssRRGuides.cs.hlsl e ProgrammingGuideDLSS_RR.md §4.
        ID3D12Resource* DiffuseAlbedo   = nullptr; // kBufferTypeAlbedo         (BaseColor*(1-Metallic))
        ID3D12Resource* SpecularAlbedo  = nullptr; // kBufferTypeSpecularAlbedo (EnvBRDFApprox2)
        ID3D12Resource* NormalRoughness = nullptr; // kBufferTypeNormalRoughness (normal.xyz + rough linear no .a)
        ID3D12Resource* SpecHitDist     = nullptr; // kBufferTypeSpecularHitDistance (R16F, do Resolved.a)
        Mat44 WorldToView{};      // row-major SEM jitter — DLSSDOptions.worldToCameraView (specHitDist MV)
    };

    // Interface comum dos upscaladores "SR-class" (renderiza sub-nativo e reconstrui no fim do frame).
    // E a cara publica que o FFsrPass ja tinha; o FDlssPass implementa o mesmo contrato.
    class IUpscaler {
    public:
        virtual ~IUpscaler() = default;

        virtual bool Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) = 0;
        virtual void Shutdown() = 0;
        virtual bool IsInitialized() const = 0;
        virtual bool Available() const = 0;   // DLSS: true so em NVIDIA c/ suporte a feature

        virtual void GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const = 0;
        virtual void Dispatch(ID3D12GraphicsCommandList* Cmd, const FUpscaleParams& P) = 0;

        virtual ID3D12Resource* OutputResource() const = 0;
        virtual u32             OutputSRVSlot() const = 0;

        // Razao render/display recomendada p/ o modo de qualidade (0=Native..4=UltraPerf).
        // FSR = razoes fixas; DLSS = query do SDK (slDLSSGetOptimalSettings).
        virtual f32 RenderRatioForQuality(int Quality) const = 0;

        virtual u32 RenderW()  const = 0;
        virtual u32 RenderH()  const = 0;
        virtual u32 DisplayW() const = 0;
        virtual u32 DisplayH() const = 0;
    };
}
