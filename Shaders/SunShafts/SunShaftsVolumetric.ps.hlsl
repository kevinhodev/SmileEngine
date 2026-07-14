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
                           // w = boost de "poeira" (só no espalhamento, não na extinção)
    float4 ScreenParams;   // xy = dims do RT meia-res, zw = 1/dims
    row_major float4x4 InvViewProj;
    float4 CameraWorldPos; // xyz = câmera em mundo, w unused
};

Texture2D<float> SceneDepth : register(t0);
SamplerState     PointClamp : register(s1);

// Visibilidade barata p/ pontos no ar: seleção de cascata + 1 tap comparativo.
// Sem normal-offset (não há superfície) e bias fixo — acne não existe em volume.
float VisVolumetric(float3 worldPos) {
    if (CSMParams.w < 0.5f) return 1.0f;
    int numC = (int)CSMParams.x;
    [loop] for (int i = 0; i < numC; ++i) {
        float3 uvz = mul(float4(worldPos, 1.0f), WorldToShadow[i]).xyz;
        if (!CSM_InBounds(uvz)) continue;
        float refZ = uvz.z - CSMParams.y * CSMBiasScale[i] * 2.0f;
        return SunShadowMap.SampleCmpLevelZero(ShadowCmp, float3(uvz.xy, (float)i), refZ);
    }
    return 1.0f; // fora do range do CSM = iluminado (igual às superfícies)
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

    // Distribuicao QUADRATICA dos passos (t = f^2 * dist): os feixes vivem nos
    // primeiros metros (copa de arvore, janela) — denso perto, esparso longe, mesmo
    // espirito do slicing exponencial do froxel fog da UE. dt = largura do segmento.
    float  T       = 1.0f;
    float  accum   = 0.0f;
    float  wSum    = 0.0f;  // contribuicao total (p/ distancia media da reprojecao)
    float  dSum    = 0.0f;
    float  prevEnd = 0.0f;
    [loop] for (int i = 0; i < steps; ++i) {
        float  fj = ((float)i + jitter) / (float)steps;
        float  t  = fj * fj * dist;
        float  fe = (float)(i + 1) / (float)steps;
        float  segEnd = fe * fe * dist;
        float  dt = segEnd - prevEnd;
        prevEnd = segEnd;
        float3 wp = CameraWorldPos.xyz + dir * t;

        // densidade do height fog na altura do passo (mesma distribuição 2-exponencial
        // do FogCommon, colapsada na câmera; x0.6931 converte exp2 -> unidades naturais)
        float dh = wp.y - CameraWorldPos.y;
        float sigma = (FogDensityP.x * exp2(-FogDensityP.y * dh) +
                       FogDensityP.z * exp2(-FogDensityP.w * dh)) * 0.6931472f;

        float vis = VisVolumetric(wp);
        float contrib = vis * sigma * T * dt;
        accum += contrib;
        wSum  += contrib;
        dSum  += contrib * t;
        T     *= exp(-sigma * dt);
    }

    // "Poeira": realça só o espalhamento (albedo artístico), extinção segue física —
    // deixa o feixe visível sem engrossar o fog global.
    float3 result = accum * MarchParams.w * ph * SunColorInt.rgb * SunColorInt.w;

    // alpha = distância média ponderada pela contribuição: o passe temporal reprojeta
    // o pixel por este ponto representativo (fallback: meio do raio, contribuição ~0)
    float meanDist = (wSum > 1e-6f) ? dSum / wSum : dist * 0.5f;
    return float4(result, meanDist);
}
