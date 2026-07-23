// HZB build — um nivel da cadeia hierarquica de depth (estilo UE "furthest HZB").
// Reverse-Z: far = 0, entao o "mais distante" de um bloco 2x2 e o MIN. Cada dispatch
// reduz uma fonte (depth full-res no primeiro passo, mip anterior nos seguintes) para
// o mip destino. Texel do destino cujo footprint cai INTEIRO fora da area valida da
// fonte vira far (0) — conservador: caixa testada ali nunca e dada como ocludida.
// Footprint parcial (borda impar) clampa na borda: duplica um pixel real, min correto.

cbuffer Constants : register(b0) {
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;
};

Texture2D<float>   Src : register(t0);
RWTexture2D<float> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= DstWidth || DTid.y >= DstHeight) return;

    const int2 Base = int2(DTid.xy) * 2;
    const int2 Max  = int2(SrcWidth - 1, SrcHeight - 1);

    if (Base.x > Max.x || Base.y > Max.y) {
        Dst[DTid.xy] = 0.0; // fora da area valida: far
        return;
    }

    float4 S;
    S.x = Src.Load(int3(min(Base + int2(0, 0), Max), 0));
    S.y = Src.Load(int3(min(Base + int2(1, 0), Max), 0));
    S.z = Src.Load(int3(min(Base + int2(0, 1), Max), 0));
    S.w = Src.Load(int3(min(Base + int2(1, 1), Max), 0));

    Dst[DTid.xy] = min(min(S.x, S.y), min(S.z, S.w));
}
