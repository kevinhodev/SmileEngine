// VS minimo compartilhado pelos passes de selecao (picking de ID + mascara de outline).
// Reusa o ObjectCB (b2) que o Renderer ja preenche por renderavel (mesmo MVP do forward),
// entao o clip-space Z bate bit-a-bit com o passe principal — essencial para o teste
// depth-EQUAL do passe de ID (picking pixel-exato sem re-renderizar profundidade).
cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
};

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

float4 main(VSInput input) : SV_POSITION {
    return mul(float4(input.pos, 1.0f), MVP);
}
