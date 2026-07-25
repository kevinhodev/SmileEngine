// Extrai o hit distance especular (Resolved.a, distancia world-space do raio de reflexao) para um
// alvo scalar R16F que o DLSS-RR taga como kBufferTypeSpecularHitDistance (§4.1.9). Combinado com
// as matrizes world<->view (DLSSDOptions), o RR deriva os specular motion vectors internamente e
// melhora a reflexao em movimento. So roda com reflexoes ativas; senao o alvo fica zerado.
Texture2D<float4>  Resolved  : register(t0); // rgb = radiancia especular, a = hitDist (mundo)
RWTexture2D<float> OutSpecHit : register(u0);

// O alvo e R16_FLOAT: satura em 65504, e o RR exige distancia FINITA. Um raio que escapa a cena (ou
// um hitDist propagado de vizinho invalido no resolve) pode chegar aqui como inf/NaN e virar lixo no
// guide. Teto com folga abaixo do limite do formato; NaN cai em 0 pelo max do clamp (o D3D devolve o
// operando nao-NaN). Nao ha semantica nova: distancia acima disso ja significa "nao acertou nada".
static const float kMaxSpecHitDist = 60000.0f;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint W, H;
    OutSpecHit.GetDimensions(W, H);
    if (dtid.x >= W || dtid.y >= H) return;
    OutSpecHit[dtid.xy] = clamp(Resolved.Load(int3(dtid.xy, 0)).a, 0.0f, kMaxSpecHitDist);
}
