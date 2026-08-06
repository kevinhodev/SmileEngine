#include "../Shadow/CSMCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

// Sun shafts — inscatter direcional VOLUMÉTRICO (meia-res): raymarch da câmera até o
// depth amostrando o CSM por passo (tap único — penumbra não é legível em volume) com
// fase Henyey-Greenstein e densidade acoplada ao height fog exponencial. Substitui o
// DirectionalInscattering analítico do fog, que atravessa parede; aqui raio só existe
// onde o sol realmente alcança — janelas, frestas entre prédios, copas de árvore.
// O ruído IGN por passo é integrado pelo passe TEMPORAL (SunShaftsTemporal) — este
// shader NÃO faz blur espacial (a tentativa anterior virava mancha).

cbuffer SunShaftsVolCB : register(b0) {
    float4 SunDirPhase;    // xyz = dir PARA a key light, w = g da fase HG
    float4 SunColorInt;    // rgb = cor da key light x intensidade da luz, w = intensidade do efeito
    float4 FogDensityP;    // x = densidade1 colapsada na câmera (exp2), y = falloff1,
                           // z = densidade2 colapsada, w = falloff2
    float4 MarchParams;    // x = passos, y = dist máxima (m), z = frame do ruído IGN,
                           // w = "poeira" = multiplicador artistico do espalhamento solar
    float4 MediumParams;   // x = extinction scale do froxel, yzw unused
    float4 ScreenParams;   // xy = dims do RT meia-res, zw = 1/dims
    float4 CloudShadowParams;  // xy = centro XZ (km), z = 1/extent, w = força (0 = off)
    float4 CloudShadowParams2; // x = km/unidade, y = altura da base (km), zw = keyDir.xz/y
    row_major float4x4 InvViewProj;
    float4 CameraWorldPos; // xyz = câmera em mundo, w = início da marcha (m)
};

Texture2D<float> SceneDepth     : register(t0);
Texture2D<float> CloudShadowMap : register(t1); // transmitância das nuvens p/ a key light
SamplerState     LinearClamp    : register(s0);
SamplerState     PointClamp     : register(s1);

// Visibilidade barata p/ pontos no ar: 1 tap por cascata e crossfade no band comum.
// Sem normal-offset (não há superfície); a segunda leitura ocorre só na transição.
float VisVolumetric(float3 worldPos) {
    return SampleCSMVolumetric(worldPos);
}

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    float2 uv  = svpos.xy * ScreenParams.zw;
    float  d   = SceneDepth.SampleLevel(PointClamp, uv, 0.0f);

    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 dir;
    float  dist = MarchParams.y; // céu: marcha só o alcance máximo (feixe é efeito de perto)
    if (!SmileIsSky(d)) {
        float4 wH = mul(float4(ndc, d, 1.0f), InvViewProj);
        float3 c2r = wH.xyz / wH.w - CameraWorldPos.xyz;
        dist = min(length(c2r), MarchParams.y);
        dir  = normalize(c2r);
    } else {
        // direção do raio independe da profundidade
        float4 pH = mul(float4(ndc, 0.5f, 1.0f), InvViewProj);
        dir = normalize(pH.xyz / pH.w - CameraWorldPos.xyz);
    }

    // fase Henyey-Greenstein (lobo forward: shaft brilha olhando contra a luz)
    float g   = SunDirPhase.w;
    float mu  = dot(dir, SunDirPhase.xyz);
    float ph  = (1.0f - g * g) /
                (12.566371f * pow(abs(1.0f + g * g - 2.0f * g * mu), 1.5f));

    const int steps = (int)MarchParams.x;
    float jitter = CSM_IGN(svpos.xy + 5.588238f * MarchParams.z);

    // Distribuicao QUADRATICA dos passos (t = start + f^2 * range): os feixes vivem
    // nos primeiros metros do trecho coberto — denso perto, esparso longe, mesmo
    // espirito do slicing exponencial do froxel fog da UE. dt = largura do segmento.
    // start normalmente e zero: quando o froxel esta ativo, seu termo solar e
    // desligado e este passe fornece a versao com oclusao fina no mesmo intervalo.
    float  start = min(CameraWorldPos.w, dist);
    float  range = dist - start;
    float  T       = 1.0f;
    float  accum   = 0.0f;
    float  wSum    = 0.0f;  // contribuicao total (p/ distancia media da reprojecao)
    float  dSum    = 0.0f;
    float  prevEnd = start;
    float  fadeWidth = min(max(MarchParams.y * 0.15f, 4.0f), 24.0f);
    float  fadeStart = max(MarchParams.y - fadeWidth, start);
    [loop] for (int i = 0; i < steps; ++i) {
        float  fj = ((float)i + jitter) / (float)steps;
        float  t  = start + fj * fj * range;
        float  fe = (float)(i + 1) / (float)steps;
        float  segEnd = start + fe * fe * range;
        float  dt = segEnd - prevEnd;
        prevEnd = segEnd;
        float3 wp = CameraWorldPos.xyz + dir * t;

        // densidade do height fog na altura do passo (mesma distribuição 2-exponencial
        // do FogCommon, colapsada na câmera; x0.6931 converte exp2 -> unidades naturais).
        // A extincao e EXATAMENTE a do froxel. "Poeira" aumenta apenas o espalhamento
        // direcional artistico; assim o passe substitui o sol do volume sem alterar duas
        // vezes a transmitancia do mesmo meio.
        float dh = wp.y - CameraWorldPos.y;
        float sigma = (FogDensityP.x * exp2(-FogDensityP.y * dh) +
                       FogDensityP.z * exp2(-FogDensityP.w * dh)) * 0.6931472f *
                      MediumParams.x;

        float vis = VisVolumetric(wp);

        // Sombra das nuvens POR PASSO (mesma projeção do deferred lighting): nuvem
        // corta o feixe no meio do ar — a borda da sombra vira uma parede de luz.
        // De brinde, dia nublado apaga os shafts sozinho.
        if (CloudShadowParams.w > 0.0f) {
            float  kKm   = CloudShadowParams2.x;
            float2 hitKm = wp.xz * kKm + CloudShadowParams2.zw *
                           max(CloudShadowParams2.y - wp.y * kKm, 0.0f);
            float2 suv   = (hitKm - CloudShadowParams.xy) * CloudShadowParams.z + 0.5f;
            if (all(suv > 0.0f) && all(suv < 1.0f)) {
                float Tc = CloudShadowMap.SampleLevel(LinearClamp, suv, 0.0f).r;
                vis *= lerp(1.0f, Tc, CloudShadowParams.w);
            }
        }

        // Crossfade com o termo solar do froxel no fim do alcance. Alem de
        // preservar os shafts proximos, evita uma casca quando Phase/Dust artisticos
        // diferem dos parametros fisicos do volume.
        float shaftRangeWeight = 1.0f - smoothstep(fadeStart, MarchParams.y, t);
        float contrib = vis * sigma * MarchParams.w * shaftRangeWeight * T * dt;
        accum += contrib;
        wSum  += contrib;
        dSum  += contrib * t;
        T     *= exp(-sigma * dt);
    }

    float3 result = accum * ph * SunColorInt.rgb * SunColorInt.w;

    // alpha = distância média ponderada pela contribuição: o passe temporal reprojeta
    // o pixel por este ponto representativo (fallback: meio do raio, contribuição ~0)
    float meanDist = (wSum > 1e-6f) ? dSum / wSum : dist * 0.5f;
    return float4(result, meanDist);
}
