// Preenche o motion vector do BACKGROUND (ceu/nuvens/fog compostos no HDR, sem geometria). O G-buffer so
// escreve velocity p/ geometria+terreno; o resto do frame fica ZERO (clear em Renderer.cpp). Zero = "parado"
// p/ DLSS/RR/TAA => ao girar a camera o historico do ceu e reprojetado do lugar errado = ghosting (pior
// subindo a camera). Aqui, p/ cada pixel de background (reverse-Z: deviceZ<=0) reprojetamos a DIRECAO do
// raio (ceu no infinito => SEM translacao) via SkyClipToPrevClip e escrevemos curUV-prevUV (mesma convencao
// do GBuffer.ps.hlsl). Pixels de geometria (deviceZ>0) sao PULADOS (mantem o velocity ja escrito).
Texture2D<float>    Depth    : register(t0); // reverse-Z (device)
RWTexture2D<float2> Velocity : register(u0); // RG16F, curUV-prevUV

cbuffer BgVelCB : register(b0) {
    row_major float4x4 SkyClipToPrevClip; // clip atual -> clip anterior, SEM translacao (ceu no infinito)
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint W, H;
    Velocity.GetDimensions(W, H);
    if (dtid.x >= W || dtid.y >= H) return;
    int2 px = int2(dtid.xy);

    // Reverse-Z: far plane = 0. Background (sem geometria) tem deviceZ<=0; geometria e > 0.
    if (Depth.Load(int3(px, 0)).r > 0.0f) return;

    float2 uv  = (float2(px) + 0.5f) / float2(W, H);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    // Ponto no far plane (deviceZ=0): a reprojecao rotacao-only leva o mesmo raio ao clip anterior.
    float4 prevClip = mul(float4(ndc, 0.0f, 1.0f), SkyClipToPrevClip);
    // w~0 = reprojecao degenerada (ex.: frame 0 sem PrevVPNoTrans) => velocity 0 (trata como parado).
    if (abs(prevClip.w) < 1e-6f) { Velocity[px] = float2(0.0f, 0.0f); return; }
    float2 prevNdc  = prevClip.xy / prevClip.w;
    float2 prevUv   = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
    Velocity[px] = uv - prevUv;
}
