// DepthNormal.ps.hlsl — pre-pass de profundidade que TAMBEM escreve a normal geometrica
// (espaco-mundo) num RT, p/ o GTAO amostrar. Porte do caminho USE_NORMALBUFFER do GTAO da
// Unreal (PostProcessAmbientOcclusion.usf): GetNormal le a WorldNormal do G-buffer e o GTAO
// a transforma p/ view. Diferenca forward+: aqui a normal e GEOMETRICA (de vertice, sem
// normal-map), pois roda no depth pre-pass (antes do material) — suficiente p/ matar os
// halos de silhueta da reconstrucao por derivada de depth.
//
// Layout do VSOutput tem que casar com Triangle.vs (mesma VS do pre-pass).
struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target0 {
    float3 n = normalize(input.worldNormal);
    return float4(n * 0.5f + 0.5f, 1.0f); // encode [-1,1] -> [0,1] (R10G10B10A2_UNORM)
}
