#ifndef SMILE_LIGHTS_COMMON
#define SMILE_LIGHTS_COMMON

// Luz puntual COMPACTA pro mundo indireto (espelha o FGPULightGI do Renderer.h — versao sem
// a matriz de shadow map do deferred: a visibilidade nos hits de RT e por shadow ray inline).
struct FPunctualLight {
    float4 PosInvRadius;      // xyz = posicao, w = 1/raio de atenuacao
    float4 ColorSourceRadius; // rgb = cor*intensidade, w = bulbo (distancia minima)
    float4 DirCosOuter;       // xyz = eixo do spot, w = cos(outer); -2 = point (sem cone)
    float4 SpotParams;        // x = 1/(cosInner - cosOuter)
};

// Radiancia incidente da luz num ponto — MESMA atenuacao do deferred lighting: inverse-square
// com janela de raio (1-(d/r)^4)^2 (Unreal), piso de bulbo (Cry) e mascara de cone quadratica.
// Devolve cor*atenuacao; L (ponto->luz, unitario) e dist saem por out.
float3 PunctualLightIncoming(FPunctualLight Lt, float3 pos, out float3 L, out float dist) {
    float3 toL     = Lt.PosInvRadius.xyz - pos;
    float  distSqr = dot(toL, toL);
    dist = sqrt(distSqr);
    L    = toL / max(dist, 1e-4f);

    float win = distSqr * Lt.PosInvRadius.w * Lt.PosInvRadius.w; // (d/r)^2
    win = saturate(1.0f - win * win);                            // 1-(d/r)^4
    win *= win;

    float dc    = max(dist, Lt.ColorSourceRadius.w);
    float atten = win / (dc * dc);

    if (Lt.DirCosOuter.w > -1.5f) {
        float sm = saturate((dot(-L, Lt.DirCosOuter.xyz) - Lt.DirCosOuter.w) * Lt.SpotParams.x);
        atten *= sm * sm;
    }
    return Lt.ColorSourceRadius.rgb * atten;
}

#endif
