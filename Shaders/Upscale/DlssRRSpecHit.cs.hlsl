// Extrai o hit distance especular (Resolved.a, distancia world-space do raio de reflexao) para um
// alvo scalar R16F que o DLSS-RR taga como kBufferTypeSpecularHitDistance (§4.1.9). Combinado com
// as matrizes world<->view (DLSSDOptions), o RR deriva os specular motion vectors internamente e
// melhora a reflexao em movimento. So roda com reflexoes ativas; senao o alvo fica zerado.
Texture2D<float4>  Resolved  : register(t0); // rgb = radiancia especular, a = hitDist (mundo)
RWTexture2D<float> OutSpecHit : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint W, H;
    OutSpecHit.GetDimensions(W, H);
    if (dtid.x >= W || dtid.y >= H) return;
    OutSpecHit[dtid.xy] = max(Resolved.Load(int3(dtid.xy, 0)).a, 0.0f);
}
