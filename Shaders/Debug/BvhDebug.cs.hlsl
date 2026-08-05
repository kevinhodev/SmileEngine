// Visualizacao de debug da BVH — porte da secao 7.3.3 do GPU Zen 3 ("Debug Visualization", cap.
// 7, pipeline de luz do Cyberpunk 2077). Traca UM raio primario por pixel direto na TLAS e
// desenha o que a traversal encontrou. Existe porque num renderer hibrido os raios primarios
// NAO sao tracados no frame normal: o que a TLAS contem, com que categoria, e onde ela custa
// caro sao coisas que nada na tela mostra.
//
// Nao substitui o FShaderTimer (Debug/ShaderTimer.hlsli), que mede CICLOS nos passes reais de
// RT e depende da NVAPI. Este aqui e portatil, roda no seu proprio dispatch e responde
// "o que esta na TLAS" em vez de "quanto o passe custou".
//
// Modos (BvhDebugParams.x — espelha FBvhDebugView::EMode):
//   0 Categoria    — cor por categoria da instancia (opaco / alpha-test / translucido).
//                    E a Figura 7.14 do capitulo, com o eixo que a Smile tem: a cena e estatica,
//                    entao estatico-vs-dinamico nao diria nada; a mask da TLAS diz.
//   1 Instancia    — cor por hash do InstanceID. Acha instancia duplicada e geometria
//                    sobreposta, que o capitulo cita como causa direta de traversal lenta.
//   2 Complexidade — quantos TRIANGULOS a traversal testou ao longo do raio, em falsa-cor.
//                    Ver o comentario do modo, que explica por que o raio e outro.

#include "../GI/DDGICommon.hlsli"

cbuffer BvhDebugCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;        // xyz = camera em mundo
    float4 ScreenParams;     // xy = largura/altura, zw = 1/largura, 1/altura
    float4 BvhDebugParams;   // x = modo, y = alcance do raio, z = teto do heatmap (triangulos),
                             // w = -
};

RaytracingAccelerationStructure Scene     : register(t0);
StructuredBuffer<InstanceGeo>   Instances : register(t1);

RWTexture2D<float4> Output : register(u0);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/RTAlphaTest.hlsli" // alpha-test dos candidatos: folhagem nao vira quad solido
#include "../GI/RTGeometry.hlsli"  // HitGeomNormal, p/ o color-coding ter forma legivel

// Teto de candidatos do modo Complexidade. Sem ele, um raio rasante numa regiao densa varre a
// cena inteira sem nunca comitar (ver o modo) e o dispatch de debug pode estourar o watchdog do
// driver. Quem satura ja esta vermelho no heatmap de qualquer jeito.
#define SMILE_BVHDEBUG_MAX_CANDIDATES 512u

// Rampa de falsa-cor. Mesma do HeatForView (DebugView.ps.hlsl) — replicada porque aquela vive no
// passe de VISUALIZACAO e este e um passe de compute, e porque aqui ela precisa sair em espaco
// LINEAR (ver ToViewerSpace).
float3 HeatRamp(float t) {
    t = saturate(t);
    float3 c = 0.0f;
    c += float3(0.05f, 0.15f, 1.00f) * saturate(1.0f - abs((t - 0.00f) * 4.0f));
    c += float3(0.00f, 0.90f, 1.00f) * saturate(1.0f - abs((t - 0.25f) * 4.0f));
    c += float3(0.10f, 1.00f, 0.20f) * saturate(1.0f - abs((t - 0.50f) * 4.0f));
    c += float3(1.00f, 0.90f, 0.05f) * saturate(1.0f - abs((t - 0.75f) * 4.0f));
    c += float3(1.00f, 0.08f, 0.02f) * saturate(1.0f - abs((t - 1.00f) * 4.0f));
    return c;
}

// O alvo e registrado com EDebugDecode::Raw, e o visualizador aplica pow(1/2.2) em tudo que nao
// e display-referred. As cores daqui sao AUTORADAS (paleta de categoria, rampa de falsa-cor),
// entao saem pre-elevadas p/ chegarem na tela como foram escolhidas. Sem isto a paleta inteira
// lava e as faixas do heatmap deixam de ser distinguiveis.
float3 ToViewerSpace(float3 c) { return pow(saturate(c), 2.2f); }

// Sombreamento so p/ dar FORMA ao color-coding: cor chapada por instancia vira uma sopa onde nao
// da p/ ler silhueta nem profundidade. Nao pretende ser iluminacao — e um wrap lambert do angulo
// com o raio, que e a unica direcao que este passe conhece.
float ShapeTerm(float3 n, float3 rayDir) {
    return 0.55f + 0.45f * saturate(dot(n, -rayDir));
}

// Cor de fundo (raio que escapou). Cinza bem escuro em vez de preto: preto se confunde com
// instancia de cor escura no modo Instancia.
static const float3 kMissColor = float3(0.02f, 0.025f, 0.03f);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const uint2 px = DTid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    // Raio primario da camera. deviceZ = 1 e o plano NEAR em reverse-Z (ver DepthConfig.h): a
    // direcao sai do ponto reconstruido no near, entao nao depende de haver depth escrito —
    // este passe roda independente do resto do frame, de proposito.
    const float2 uv  = (px + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wH  = mul(float4(ndc, 1.0f, 1.0f), InvViewProj);
    const float3 nearPos = wH.xyz / wH.w;

    RayDesc ray;
    ray.Origin    = CameraPos.xyz;
    ray.Direction = normalize(nearPos - CameraPos.xyz);
    ray.TMin      = 0.0f;
    ray.TMax      = BvhDebugParams.y;

    const uint mode = (uint)BvhDebugParams.x;
    float3 color;

    if (mode == 2u) {
        // COMPLEXIDADE. Traca com FORCE_NON_OPAQUE: assim NENHUM triangulo auto-comita e a
        // traversal reporta cada um como candidato, deixando o shader conta-los. E o unico jeito
        // de medir isto com RayQuery — o percurso dos nos da BVH acontece dentro do hardware e
        // nao e observavel. O que sai e "quantos triangulos foram testados ao longo do raio",
        // que e exatamente o sinal que o capitulo persegue na Figura 7.15: sobe onde ha malha
        // sobreposta, BLAS mal particionada e muita camada de folhagem.
        //
        // Nunca comitamos, entao o TMax nao encolhe e a conta cobre o raio INTEIRO ate o
        // alcance — nao so ate a primeira superficie. E de proposito: geometria escondida atras
        // da parede tambem custa traversal, e e justamente ela que passa despercebida.
        //
        // Consequencia: este raio e mais caro que o do frame real e nao mede o custo DELE. Mede
        // a densidade da estrutura, que e o que se conserta editando a cena.
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
        q.TraceRayInline(Scene, RAY_FLAG_FORCE_NON_OPAQUE, SMILE_RT_MASK_ALL, ray);
        uint tested = 0u;
        while (q.Proceed()) {
            ++tested;
            if (tested >= SMILE_BVHDEBUG_MAX_CANDIDATES) break;
        }
        const float t = (float)tested / max(BvhDebugParams.z, 1.0f);
        color = (tested == 0u) ? kMissColor : HeatRamp(t);
    } else {
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_ALL, ray);
        SMILE_RT_PROCEED(q)

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            color = kMissColor;
        } else {
            const uint instId = q.CommittedInstanceID();
            const InstanceGeo geo = Instances[instId];
            const float3 n = HitGeomNormal(instId, q.CommittedPrimitiveIndex(),
                                           q.CommittedTriangleBarycentrics(),
                                           q.CommittedWorldToObject3x4(), ray.Direction);

            float3 base;
            if (mode == 0u) {
                // CATEGORIA — a mesma que a instancia carrega na TLAS (RaytracingScene.cpp):
                // translucido tem prioridade porque a categoria e exclusiva (um bit por
                // instancia) e o material Blend nunca e alpha-test ao mesmo tempo.
                if ((geo.Flags & INSTGEO_FLAG_TRANSLUCENT) != 0u)
                    base = float3(0.25f, 0.55f, 1.00f);      // azul   — translucido
                else if ((geo.Flags & INSTGEO_FLAG_ALPHATEST) != 0u)
                    base = float3(1.00f, 0.85f, 0.15f);      // amarelo — alpha-test / folhagem
                else
                    base = float3(0.35f, 0.85f, 0.35f);      // verde  — opaco

                // Two-sided desaturado: e o eixo que decide se um passe pode cullar backface, e
                // ficou fora de sincronia com o raster mais de uma vez. Ver a nota sobre a
                // instance flag vencer a ray flag em RaytracingScene.cpp.
                if (geo.TwoSidedRT != 0u)
                    base = lerp(base, dot(base, float3(0.299f, 0.587f, 0.114f)).xxx, 0.55f);
            } else {
                // INSTANCIA — hash do id. DDGI_Hash com 3 sais diferentes; o piso de 0.15 evita
                // instancia quase preta, que some contra o fundo.
                base = float3(DDGI_Hash(instId * 3u + 0u),
                              DDGI_Hash(instId * 3u + 1u),
                              DDGI_Hash(instId * 3u + 2u)) * 0.85f + 0.15f;
            }
            color = base * ShapeTerm(n, ray.Direction);
        }
    }

    Output[px] = float4(ToViewerSpace(color), 1.0f);
}
