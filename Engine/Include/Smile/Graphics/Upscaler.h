#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace Smile {
    // Upscaler ativo do viewport. Os ints batem com o "upscalerMode" que o bridge/QML ja usam
    // (0 = nativo, 1 = FSR); DLSS entra como 2.
    enum class EUpscaler : u8 { None = 0, FSR = 1, DLSS = 2 };

    // Parametros de um dispatch de upscale. O FSR usa o subconjunto (cor/depth/vel/jitter/near/far/fov);
    // o DLSS (Streamline) exige tambem as matrizes de camera p/ o sl::Constants — o Renderer preenche
    // tudo sempre (custo desprezivel) e cada upscaler le o que precisa.
    struct FUpscaleParams {
        ID3D12Resource* Color    = nullptr;   // render-res, ja em COMPUTE_READ (NON_PIXEL_SHADER_RESOURCE)
        ID3D12Resource* Depth    = nullptr;
        ID3D12Resource* Velocity = nullptr;
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
