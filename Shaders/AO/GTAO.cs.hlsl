#include "GTAOCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

#define GTAO_USE_NORMALBUFFER 1
#define GTAO_DEBUG 0

Texture2D<float>   DepthTex  : register(t0);
Texture2D<float4>  NormalTex : register(t1);
RWTexture2D<float> AOOut     : register(u0);

// Rotacao temporal (UE GetGTAOShaderParameters): ciclo embaralhado de 6 frames gira as
// direcoes dentro do setor PI/numDir e ciclo de 4 desloca o passo radial; periodo 12,
// o TAA acumula. Rots {60,300,180,240,120,0}*(PI/360) sobre o setor => fracoes do setor.
static const float kTemporalRot[6] = { 1.0f/6.0f, 5.0f/6.0f, 3.0f/6.0f, 4.0f/6.0f, 2.0f/6.0f, 0.0f };
static const float kTemporalOff[4] = { 0.1f, 0.6f, 0.35f, 0.85f };

// Thickness heuristic (UE r.GTAO.ThicknessBlend=0.5 => 1-0.5^2): horizonte decai atras de
// oclusores finos em vez de persistir como parede infinita.
static const float kThickness = 0.75f;

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
    if (SmileIsSky(rawD)) { AOOut[tid.xy] = 1.0f; return; } 

#if GTAO_DEBUG == 1
    AOOut[tid.xy] = NormalTex.Load(int3(ipx, 0)).x; return; // R cru do normal buffer
#elif GTAO_DEBUG == 3
    AOOut[tid.xy] = saturate(length(NormalTex.Load(int3(ipx, 0)).xyz * 2.0f - 1.0f)); return;
#endif

    const float3 P = GTAO_ViewPos(px, rawD);
    const float3 V = normalize(-P); 
    
#if GTAO_USE_NORMALBUFFER
    const float3 wN = NormalTex.Load(int3(ipx, 0)).xyz * 2.0f - 1.0f;
    float3 N = mul(wN, (float3x3)ViewMatrix);
    const float Nlen = length(N);
    N = (Nlen > 1e-4f) ? (N / Nlen) : V; 
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

    const float radiusWorld = Params.x;
    const float falloffEnd  = max(Params.w, 1e-3f);
    float radiusPix = radiusWorld * ProjA.y / max(P.z, 1e-3f) * 0.5f * ScreenParams.y;
    radiusPix = clamp(radiusPix, 2.0f, 256.0f);

    const int   numDir  = max((int)Params2.x, 1);
    const int   numStep = max((int)Params2.y, 1);
    const uint  frame   = (uint)Params2.z;
    const float ign      = GTAO_IGN(px);
    const float dirNoise = frac(ign + kTemporalRot[frame % 6u]);
    float       stepNoise = frac(ign + kTemporalOff[frame % 4u]);
    const float r2          = falloffEnd * falloffEnd;
    const float attenFactor = 2.0f / r2;

    float occlusion = 0.0f;

    [loop] for (int d = 0; d < numDir; ++d) {
        const float  phi = (float(d) + dirNoise) * (GTAO_PI / float(numDir));
        const float2 dir = float2(cos(phi), sin(phi));            
        const float3 sliceDirVS = normalize(float3(dir.x, -dir.y, 0.0f)); 

        float cosP = -1.0f;
        float cosN = -1.0f;
        [loop] for (int s = 1; s <= numStep; ++s) {
            const float  t   = (float(s) - stepNoise) / float(numStep);
            const float2 off = dir * t * radiusPix;

            int2 spP = clamp(ipx + int2(off), int2(0, 0), int2((int)W - 1, (int)H - 1));
            {
                float sd = DepthTex.Load(int3(spP, 0));
                if (!SmileIsSky(sd)) {
                    float3 D = GTAO_ViewPos(float2(spP), sd) - P;
                    float  dist2 = dot(D, D);
                    float  cosA  = dot(D * rsqrt(max(dist2, 1e-8f)), V);
                    cosA = lerp(cosA, cosP, saturate(dist2 * attenFactor));
                    cosP = (cosA > cosP) ? cosA : lerp(cosA, cosP, kThickness);
                }
            }
            int2 spN = clamp(ipx - int2(off), int2(0, 0), int2((int)W - 1, (int)H - 1));
            {
                float sd = DepthTex.Load(int3(spN, 0));
                if (!SmileIsSky(sd)) {
                    float3 D = GTAO_ViewPos(float2(spN), sd) - P;
                    float  dist2 = dot(D, D);
                    float  cosA  = dot(D * rsqrt(max(dist2, 1e-8f)), V);
                    cosA = lerp(cosA, cosN, saturate(dist2 * attenFactor));
                    cosN = (cosA > cosN) ? cosA : lerp(cosA, cosN, kThickness);
                }
            }
        }
        stepNoise = frac(stepNoise + 0.617f);

        float3 planeN = cross(V, sliceDirVS);
        float  planeLen = length(planeN);
        if (planeLen < 1e-5f) continue;
        planeN /= planeLen;
        float3 projN = N - planeN * dot(N, planeN);
        float  projLen = length(projN);
        if (projLen < 1e-4f) continue;
        projN /= projLen;

        float gamma = acos(clamp(dot(projN, V), -1.0f, 1.0f));
        gamma *= sign(dot(projN, sliceDirVS));

        float thetaPos =  acos(clamp(cosP, -1.0f, 1.0f));
        float thetaNeg = -acos(clamp(cosN, -1.0f, 1.0f));
        float h2 = gamma + min(thetaPos - gamma,  GTAO_HALFPI);
        float h1 = gamma + max(thetaNeg - gamma, -GTAO_HALFPI);

        float integ = 0.25f * (-cos(2.0f * h1 - gamma) + cos(gamma) + 2.0f * h1 * sin(gamma))
                    + 0.25f * (-cos(2.0f * h2 - gamma) + cos(gamma) + 2.0f * h2 * sin(gamma));
        occlusion += projLen * integ;
    }

    float vis = saturate(occlusion / float(numDir));
    vis = pow(vis, max(Params.z, 0.01f));           
    float ao = lerp(1.0f, vis, saturate(Params.y));  

    float fade = saturate((P.z - Params3.x) / max(Params3.y - Params3.x, 1e-3f));
    ao = lerp(ao, 1.0f, fade);

    AOOut[tid.xy] = ao;
}
