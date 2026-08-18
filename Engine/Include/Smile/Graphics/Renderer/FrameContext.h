#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

// Estado calculado no inicio do frame e compartilhado pelas fases de gravacao.
namespace Smile {

    // Ambiente hemisferico do ceu, ja com o dim de chuva aplicado. Sai do mesmo integral que
    // publica a SH-L1 no constant buffer.
    struct FFrameAmbient {
        Vec3 Sky{};    // hemisferio superior (zenite)
        Vec3 Ground{}; // hemisferio inferior (nadir)
    };

    // Sol, lua, chuva e a resolucao da luz-chave do frame, sem efeitos colaterais de GPU.
    struct FFrameLighting {
        Vec3 SunN{};              // direcao normalizada PARA o sol

        // --- Chuva: tres dims distintos, nao um so -------------------------------------
        f32  RainSky    = 0.0f;   // RainAmount, ou 0 se DriveSky esta off
        f32  RainKeyDim = 1.0f;   // escurece a luz-chave (sol/lua)
        f32  RainAmbDim = 1.0f;   // escurece IRRADIANCIA (ambiente hemisferico)
        f32  RainSkyDim = 1.0f;   // escurece a RADIANCIA do ceu vista por um raio

        // Sol ja transmitido pela atmosfera, com HorizonFade e dim de chuva aplicados.
        Vec3 EffectiveSunColor{};

        // --- Lua -------------------------------------------------------------------------
        Vec3 MoonN{};
        bool MoonOn       = false;
        f32  NightFactor  = 0.0f; // 0 de dia, 1 com o sol bem abaixo do horizonte
        f32  MoonIllum    = 0.0f; // fase: 0 nova, 1 cheia
        Vec3 MoonTrans{ 1.0f, 1.0f, 1.0f };
        Vec3 MoonLightCol{};
        f32  MoonW          = 0.0f; // intensidade efetiva (fase x altura x noite)
        f32  CosMoonRadius  = 1.0f;
        f32  MoonDiskBright = 0.0f;
        f32  MoonSkyScale   = 0.0f;

        // --- Luz-chave: sol de dia, lua de noite -----------------------------------------
        bool KeyIsMoon = false;
        Vec3 KeyDir{};
        Vec3 KeyColor{};
        f32  KeyInt   = 0.0f;
        f32  CloudDim = 1.0f;     // razao lua/sol, p/ as nuvens nao estourarem a noite
        Vec3 KeyCloudCol{};
    };

    // Camera e matrizes derivadas da camera e do jitter; nao publica estado na GPU.
    struct FFrameView {
        f32   Aspect = 1.0f;
        f32   NearZ  = 0.1f;
        f32   FarZ   = 4000.0f; // 20000 com agua: o oceano estende o horizonte
        f32   FovY   = 0.0f;

        Mat44 View{};
        Mat44 ProjUnjittered{};
        Mat44 Projection{};     // ProjUnjittered + jitter do upscaler/TAA
        Mat44 ViewProjection{};
        Mat44 ViewProjUnjittered{};
        Vec3  CameraPosition{};

        // Jitter em pixels e o sinal de Y (o FSR/DLSS invertem em relacao ao TAA proprio).
        f32   JitterPxX = 0.0f;
        f32   JitterPxY = 0.0f;
        f32   ProjJitterYSign = 1.0f;
        Vec2  JitterUv{};       // ja com o y invertido p/ uv y-down
        Vec2  JitterPx{};

        // Variantes SEM translacao — ceu no infinito, que nao tem parallax.
        Mat44 ViewNoTrans{};
        Mat44 VPNoTrans{};
        Mat44 InvVPNoTrans{};
        Mat44 VPNoTransUnjit{};
        // Clip atual -> clip anterior, sem translacao e sem jitter. Espelha o ClipToPrevClip do
        // DLSS, mas com a translacao removida. Alimenta o velocity do background.
        Mat44 SkyClipToPrevClip{};

        Mat44 InvViewProjFull{};  // inversa da jittered — reconstrucao de worldPos do depth
        Mat44 InvViewProjUnjit{};

        // Mip bias global de textura no upscale (log2(render/display) - 1); 0 quando nativo.
        f32   MipBias = 0.0f;

        // Planos nao normalizados: esquerda, direita, baixo, cima, perto e longe.
        Vec4  FrustumPlanes[6]{};

        // true quando a AABB esta INTEIRAMENTE fora de algum plano. Conservador na direcao certa:
        // uma caixa que atravessa a fronteira conta como dentro.
        bool AABBOutsideFrustum(const Vec3& Mn, const Vec3& Mx) const {
            for (int i = 0; i < 6; ++i) {
                const Vec4& p = FrustumPlanes[i];
                const f32 px = (p.X >= 0.0f) ? Mx.X : Mn.X;
                const f32 py = (p.Y >= 0.0f) ? Mx.Y : Mn.Y;
                const f32 pz = (p.Z >= 0.0f) ? Mx.Z : Mn.Z;
                if (p.X * px + p.Y * py + p.Z * pz + p.W < 0.0f) return true;
            }
            return false;
        }
    };

    // Decisoes resolvidas antes da gravacao. Modos que dependem de trabalho do proprio frame
    // permanecem na fase que produz esse dado.
    struct FFrameModes {
        // --- Reconstrucao temporal ------------------------------------------------------
        bool UpscaleActive = false; // ha upscaler ativo (FSR/DLSS/RR) — nullptr = nativo
        bool TAAActive     = false; // TAA proprio; exclusivo com o upscaler
        bool RRMode        = false; // DLSS Ray Reconstruction: substitui NRD + SR num eval

        // --- Iluminacao indireta --------------------------------------------------------
        bool ReflectionsActive   = false;
        bool ReSTIRGIActive      = false;
        bool ReSTIRDIActiveFrame = false;
        bool NrdIndirectMode     = false; // NRD denoisando o ReSTIR GI
        bool NrdDirectMode       = false; // instancia dedicada do NRD p/ a direta
        bool AOWillRun           = false;
        // O visualizador do cache tambem consome a normal geometrica do prepass. Separado de AO
        // para nao executar GTAO apenas porque uma ferramenta de debug esta aberta.
        bool RadianceCacheDebugActive = false;
        // O produtor dedicado roda depois do G-buffer; os traces de render apenas consultam.
        bool RadianceCacheUpdateActive = false;
        bool GeometricNormalWillRun   = false;

        // --- Agua -----------------------------------------------------------------------
        bool WaterReflectionDebug       = false;
        bool DedicatedWaterReflections  = false;
        // A superficie da agua so pode refratar quando as copias de scene color/depth existem.
        bool WaterSceneCopiesReady      = false;

        // --- Vetores de movimento -------------------------------------------------------
        bool ReliableMotionActive        = false; // vetor dual (RT Gems II cap. 25)
        bool MotionHistoryValidThisFrame = false;

        // --- Atmosfera e volumetricos ---------------------------------------------------
        bool VolShaftsActive = false;
        bool VolFogActive    = false;
        bool FogSkyAnchor    = false; // ancora o height fog no sky-view LUT (so ceu procedural)
    };
}
