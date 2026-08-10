#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

// Estado por-frame do RenderFrame, resolvido ANTES da gravacao e depois so lido.
//
// O RenderFrame tem ~115 locais de nivel 0 e ~2500 linhas. Nao e a quantidade de passes que
// impede extrair fases dele: e que o estado compartilhado por todas elas mora em variavel
// LOCAL. Enquanto for local, nenhuma fase pode virar metodo — nao ha o que passar. Tornar esse
// estado explicito e o pre-requisito, nao a arrumacao final.
//
// Este header nasce com a primeira das tres partes previstas:
//   FFrameModes    — quais passes rodam neste frame              (FEITO)
//   FFrameView     — camera, matrizes, jitter                    (FEITO)
//   FFrameLighting — sol, lua, chuva e a luz-chave                (FEITO)
//   FFrameAmbient  — ambiente hemisferico do ceu                  (FEITO)
namespace Smile {

    // Ambiente hemisferico do ceu, ja com o dim de chuva aplicado. Sai do mesmo integral que
    // publica a SH-L1 no constant buffer.
    //
    // Era o ultimo pedaco do estado compartilhado que continuava LOCAL do RenderFrame — a §13
    // do Docs/ARCHITECTURE.md o nomeava como o bloqueio restante ("soldado ao bloco que publica
    // a SH"). Enquanto fosse local, o bloco de publicacao nao podia virar metodo: as nuvens
    // volumetricas, o froxel, a chuva e a agua leem estes dois vetores DEPOIS, e nao havia o
    // que devolver.
    struct FFrameAmbient {
        Vec3 Sky{};    // hemisferio superior (zenite)
        Vec3 Ground{}; // hemisferio inferior (nadir)
    };

    // Sol, lua, chuva e a resolucao da LUZ-CHAVE do frame. Puramente calculo: as publicacoes
    // (MappedCB->..., Atmosphere.SetNightParams/SetMoonSkyLight/SetStarRotation) ficam no
    // RenderFrame e passam a ler daqui.
    //
    // O ambiente hemisferico (SkyAmbient/GroundAmbient) NAO entrou: ele e calculado dentro do
    // mesmo bloco que publica a SH no constant buffer, e separar os dois e uma cirurgia de
    // outra natureza. Fica para uma proxima.
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

    // Camera e matrizes do frame. Tudo derivado da camera + do jitter do upscaler; nada aqui
    // toca a GPU. Sao ~20 locais que quase toda fase do RenderFrame le — a maior razao pela
    // qual nenhuma delas podia virar metodo.
    //
    // Fica FORA daqui o que tem efeito colateral e continua no RenderFrame: a publicacao no
    // constant buffer (MappedCB->...), o LastViewProj (lido pelo editor entre frames) e o
    // PrevVPNoTrans, que so e atualizado no fim do frame.
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

        // Frustum da camera, derivado da ViewProjection (planos NAO normalizados — para o teste
        // de lado o sinal basta, e normalizar seria trabalho jogado fora). Ordem: esq, dir,
        // baixo, cima, perto, longe.
        //
        // Morava como `Vec4 Planes[6]` local do RenderFrame, com o teste ao lado numa lambda. Sai
        // de la porque e a MESMA matriz que o resto do FFrameView ja carrega, e porque o teste
        // atravessa fases: quem monta a lista de visiveis e quem culla luz local sao blocos
        // diferentes e distantes.
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

    // Resolucao de "que passes rodam neste frame". Cada campo e a conjuncao do TOGGLE do
    // usuario com a PRONTIDAO do subsistema — um passe pode estar ligado e mesmo assim nao
    // rodar porque a cena ainda nao deu SRV a ele.
    //
    // Ficavam espalhados como `const bool XActive = ...` ao longo do RenderFrame, alguns a 900
    // linhas de distancia de onde eram decididos os irmaos. Juntos, a forma do frame fica
    // legivel num lugar so — e passavel para uma fase extraida.
    //
    // NAO entram aqui os que dependem de trabalho do proprio frame (ReGIROn precisa da
    // contagem de luzes ja empacotada; RRPoisoned precisa saber se o debug desenhou no HDR).
    // Aqueles continuam onde estao, porque nao sao resolviveis de antemao.
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
        bool GeometricNormalWillRun   = false;

        // --- Agua -----------------------------------------------------------------------
        bool WaterReflectionDebug       = false;
        bool DedicatedWaterReflections  = false;
        // As copias de scene color/depth existem. A superficie da agua refrata lendo a cor da
        // cena e por isso nao pode rodar sem elas. Era um `const bool WaterHasDepth` local,
        // decidido no bloco de update e lido ~1200 linhas abaixo, na hora de gravar a agua.
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
