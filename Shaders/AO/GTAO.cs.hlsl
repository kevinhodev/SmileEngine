// GTAO (Ground-Truth Ambient Occlusion) — porte da linha da Unreal (busca de horizonte
// + integral interno cosseno-ponderado). Le o depth da cena (R32F) p/ a busca de horizonte e
// amostra a normal GEOMETRICA do RT do pre-pass (caminho USE_NORMALBUFFER da Unreal; fallback
// por derivada de depth sob GTAO_USE_NORMALBUFFER 0). Escreve a oclusao (R8) que o Triangle.ps
// multiplica no termo de ambiente/indireto (DDGI + atmo-ambient + IBL). Nunca toca no sol
// direto (que ja tem CSM). So no caminho sem MSAA.

#include "GTAOCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

// 1 = amostra a normal geometrica do RT do pre-pass (caminho USE_NORMALBUFFER da Unreal,
// mata os halos de silhueta); 0 = reconstroi a normal pela derivada de depth (fallback, p/ A/B).
#define GTAO_USE_NORMALBUFFER 1

// DEBUG (cospe valores como cinza no AO; 0 = AO normal):
//   1 = canal R cru do normal buffer (worldN.x*0.5+0.5). Estruturado=RT escrito; chapado 0.5=RT vazio.
//   2 = N.V em view (estrutura por orientacao = normal valida apos transform).
//   3 = comprimento cru de wN antes de normalizar (~1=ok; ~0=RT zerado/nao bindado).
//   4 = mascara de fundo: branco = GTAO ve fundo neste pixel (sem depth opaco aqui), preto = geo.
#define GTAO_DEBUG 0

Texture2D<float>   DepthTex  : register(t0);
Texture2D<float4>  NormalTex : register(t1); // world-normal encodada (*0.5+0.5); so no caminho normalbuffer
RWTexture2D<float> AOOut     : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint W = (uint)ScreenParams.x;
    const uint H = (uint)ScreenParams.y;
    if (tid.x >= W || tid.y >= H) return;

    const int2  ipx = int2(tid.xy);
    const float2 px = float2(tid.xy);
    const float rawD = DepthTex.Load(int3(ipx, 0));
#if GTAO_DEBUG == 4
    AOOut[tid.xy] = SmileIsSky(rawD) ? 1.0f : 0.0f; return; // branco = sem depth opaco aqui
#endif
    if (SmileIsSky(rawD)) { AOOut[tid.xy] = 1.0f; return; } // ceu/fundo: sem oclusao

#if GTAO_DEBUG == 1
    AOOut[tid.xy] = NormalTex.Load(int3(ipx, 0)).x; return; // R cru do normal buffer
#elif GTAO_DEBUG == 3
    AOOut[tid.xy] = saturate(length(NormalTex.Load(int3(ipx, 0)).xyz * 2.0f - 1.0f)); return;
#endif

    const float3 P = GTAO_ViewPos(px, rawD);
    const float3 V = normalize(-P); // do ponto p/ a camera (view space)

    // --- Normal em view space ---
#if GTAO_USE_NORMALBUFFER
    // Normal GEOMETRICA do RT do pre-pass (mundo, encodada *0.5+0.5) -> view. Sem ruido de
    // silhueta/derivada (= GetNormal com USE_NORMALBUFFER na Unreal). NaN-safe p/ pixels limpos.
    const float3 wN = NormalTex.Load(int3(ipx, 0)).xyz * 2.0f - 1.0f;
    float3 N = mul(wN, (float3x3)ViewMatrix);
    const float Nlen = length(N);
    N = (Nlen > 1e-4f) ? (N / Nlen) : V; // pixel sem normal valida: usa V (oclusao neutra)
#else
    // Fallback: normal por derivada de profundidade (melhor de diferenca p/ frente/tras).
    const float dR = DepthTex.Load(int3(min(ipx.x + 1, (int)W - 1), ipx.y, 0));
    const float dL = DepthTex.Load(int3(max(ipx.x - 1, 0),         ipx.y, 0));
    const float dD = DepthTex.Load(int3(ipx.x, min(ipx.y + 1, (int)H - 1), 0));
    const float dU = DepthTex.Load(int3(ipx.x, max(ipx.y - 1, 0),         0));
    const float3 Pr = GTAO_ViewPos(px + float2( 1, 0), dR);
    const float3 Pl = GTAO_ViewPos(px + float2(-1, 0), dL);
    const float3 Pd = GTAO_ViewPos(px + float2(0,  1), dD);
    const float3 Pu = GTAO_ViewPos(px + float2(0, -1), dU);
    float3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    float3 ddy = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? (Pd - P) : (P - Pu);
    float3 N = normalize(cross(ddx, ddy));
#endif
    if (dot(N, V) < 0.0f) N = -N;

#if GTAO_DEBUG == 2
    AOOut[tid.xy] = saturate(dot(N, V)); return; // N.V (estrutura = normal valida)
#endif

    // --- Raio de busca em pixels (projeta o raio de mundo na vertical) ---
    const float radiusWorld = Params.x;
    const float falloffEnd  = max(Params.w, 1e-3f);
    float radiusPix = radiusWorld * ProjA.y / max(P.z, 1e-3f) * 0.5f * ScreenParams.y;
    radiusPix = clamp(radiusPix, 2.0f, 256.0f);

    const int   numDir  = max((int)Params2.x, 1);
    const int   numStep = max((int)Params2.y, 1);
    const float noise   = GTAO_IGN(px, Params2.z);
    const float r2      = falloffEnd * falloffEnd;

    float occlusion = 0.0f;

    [loop] for (int d = 0; d < numDir; ++d) {
        const float  phi = (float(d) + noise) * (GTAO_PI / float(numDir));
        const float2 dir = float2(cos(phi), sin(phi));            // direcao na tela (px)
        const float3 sliceDirVS = normalize(float3(dir.x, -dir.y, 0.0f)); // em view (y p/ cima)

        // Horizontes: maior cos do angulo entre (amostra-P) e V, em +dir e -dir.
        float cosP = -1.0f; // +dir
        float cosN = -1.0f; // -dir
        [loop] for (int s = 1; s <= numStep; ++s) {
            const float  t   = (float(s) - noise) / float(numStep); // (0,1]
            const float2 off = dir * t * radiusPix;

            // Clampa na borda da tela em vez de pular: amostra fora-da-tela pulada nao
            // atualizava o horizonte -> menos oclusao -> parede clareava abrupto ao chegar na
            // borda. Como o horizonte usa max(), reamostrar o pixel da borda e idempotente.
            int2 spP = clamp(ipx + int2(off), int2(0, 0), int2((int)W - 1, (int)H - 1));
            {
                float sd = DepthTex.Load(int3(spP, 0));
                if (!SmileIsSky(sd)) {
                    float3 D = GTAO_ViewPos(float2(spP), sd) - P;
                    float  dist2 = dot(D, D);
                    float  cosA  = dot(D * rsqrt(max(dist2, 1e-8f)), V);
                    float  fall  = saturate(1.0f - dist2 / r2);
                    cosP = max(cosP, lerp(-1.0f, cosA, fall));
                }
            }
            int2 spN = clamp(ipx - int2(off), int2(0, 0), int2((int)W - 1, (int)H - 1));
            {
                float sd = DepthTex.Load(int3(spN, 0));
                if (!SmileIsSky(sd)) {
                    float3 D = GTAO_ViewPos(float2(spN), sd) - P;
                    float  dist2 = dot(D, D);
                    float  cosA  = dot(D * rsqrt(max(dist2, 1e-8f)), V);
                    float  fall  = saturate(1.0f - dist2 / r2);
                    cosN = max(cosN, lerp(-1.0f, cosA, fall));
                }
            }
        }

        // Normal projetada no plano da fatia (gerado por V e sliceDirVS).
        float3 planeN = cross(V, sliceDirVS);
        float  planeLen = length(planeN);
        if (planeLen < 1e-5f) continue;
        planeN /= planeLen;
        float3 projN = N - planeN * dot(N, planeN);
        float  projLen = length(projN);
        if (projLen < 1e-4f) continue;
        projN /= projLen;

        // Angulo da normal projetada relativo a V (com sinal p/ o lado de +sliceDir).
        float gamma = acos(clamp(dot(projN, V), -1.0f, 1.0f));
        gamma *= sign(dot(projN, sliceDirVS));

        // Angulos de horizonte: +dir positivo, -dir negativo; clamp ao hemisferio de gamma.
        float thetaPos =  acos(clamp(cosP, -1.0f, 1.0f));
        float thetaNeg = -acos(clamp(cosN, -1.0f, 1.0f));
        float h2 = gamma + min(thetaPos - gamma,  GTAO_HALFPI);
        float h1 = gamma + max(thetaNeg - gamma, -GTAO_HALFPI);

        // Integral interno cosseno-ponderado (GTAO).
        float integ = 0.25f * (-cos(2.0f * h1 - gamma) + cos(gamma) + 2.0f * h1 * sin(gamma))
                    + 0.25f * (-cos(2.0f * h2 - gamma) + cos(gamma) + 2.0f * h2 * sin(gamma));
        occlusion += projLen * integ;
    }

    float vis = saturate(occlusion / float(numDir));
    vis = pow(vis, max(Params.z, 0.01f));            // power (contraste)
    float ao = lerp(1.0f, vis, saturate(Params.y));  // intensity

    // Distance fade: a normal reconstruida por derivada fica ruidosa a grande distancia
    // (1px de depth cobre muitos metros) -> manchas. Esmaece o AO p/ 1 (sem oclusao) ao longe.
    float fade = saturate((P.z - Params3.x) / max(Params3.y - Params3.x, 1e-3f));
    ao = lerp(ao, 1.0f, fade);

    AOOut[tid.xy] = ao;
}
