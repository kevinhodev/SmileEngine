#ifndef SMILE_PT_COMMON_HLSLI
#define SMILE_PT_COMMON_HLSLI

// ReSTIR PT — layout UNICO do constant buffer (b0), compartilhado por TODOS os passes do path
// tracer (PTInitial/PTSpatial/PTNrdPack/PTComposite). Campo faltante num cbuffer redeclarado
// desloca os offsets seguintes silenciosamente (bug real do prototipo anterior) — por isso o
// layout vive so aqui e casa campo-a-campo com ReSTIRPTConstants (ReSTIRPT.h).

cbuffer PTCB : register(b0) {
    row_major float4x4 InvViewProj;       // inversa FULL da view-proj (jittered, casa com o depth)
    row_major float4x4 View;              // world -> view (p/ o pack do NRD, F4)
    float4 CameraPos;                     // xyz = camera mundo
    float4 ScreenParams;                  // x=W, y=H, z=1/W, w=1/H
    float4 GridMinSpacing;                // DDGI: xyz = origem do grid, w = espacamento
    float4 GridCount;                     // DDGI: xyz = nº de probes por eixo
    float4 AtlasParams;                   // DDGI irradiancia: x = tile, y = W, z = H
    float4 SunDirIntensity;               // xyz = direcao P/ o sol (norm.), w = intensidade
    float4 SunColorCosRadius;             // rgb = cor do sol, w = cos(raio angular) p/ sombra suave
    float4 TraceParams;                   // x=frameIndex, y=maxRayDist, z=skyIntensity, w=normalBias
    float4 PathParams;                    // x=maxBounces, y=rrStartBounce, z=fireflyMax, w=albedoLOD
    float4 ReuseParams;                   // F1: x=MCap, y=posRejectScale, z=temporal(0/1), w=useNrd(0/1) F4
    float4 SpatialParams;                 // F3: x=sigma(px), y=count, z=spatial(0/1), w=normalReject
    float4 ShiftParams;                   // F2: x=kFootprint, y=reconnectRoughMin
    float4 NrdHitDistParams;              // F4: xyz = ReblurHitDistanceParameters {A,B,C}
    float4 DebugParams;                   // x=debugMode (0=off, 1=acumulacao progressiva), y=accumFrame
};

// Criterios de reconexao do Enhanced (Sec 4) — defaults; o CB pode sobrescrever via ShiftParams.
#define PT_FOOTPRINT_K          0.02f     // kappa do dual ray footprint threshold
#define PT_RECONNECT_ROUGH_MIN  0.2f      // roughness minima em x_{k-1} p/ reconectar

// Modos de debug (DebugParams.x).
#define PT_DEBUG_OFF     0
#define PT_DEBUG_ACCUM   1                // media progressiva com camera parada (ground truth)
#define PT_DEBUG_KMAP    2                // F2b: heatmap do vertice de reconexao (kb do candidato)
#define PT_DEBUG_CANARY  3                // F2b: residuo do auto-replay (deve ser preto)
#define PT_DEBUG_SPATIAL 4                // F3: r=vizinhos rejeitados, g=m_c, b=energia dos vizinhos

float PT_Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// Env-BRDF analitico (Karis/Lazarov) — fator de DE/REmodulacao do canal especular do NRD (F4).
// A precisao da aproximacao e irrelevante: pack divide e composite multiplica pelo MESMO fator
// (o erro cancela); so importa capturar a variacao espacial de F0/roughness p/ "braquear" o sinal.
float3 PT_EnvBRDF(float3 f0, float nov, float rough) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.04f, -0.04f);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * nov)) * r.x + r.y;
    float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

// Reconstroi a posicao de mundo do depth (mesma convencao do DeferredLighting).
float3 PT_WorldPosFromDepth(uint2 px, float deviceZ) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wH.xyz / wH.w;
}

#endif
