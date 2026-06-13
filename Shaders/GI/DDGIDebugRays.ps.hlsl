// DDGI debug — PS do rótulo de nº de raios. Renderiza o número (info.x) em fonte 3x5 procedural.
// Fundo transparente (discard); texto branco. numDigits = info.y.
struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    nointerpolation float2 info : TEXCOORD1; // x = rayCount, y = numDigits
};

// Fonte 3x5 (bit = row*3 + col; col 0 = esquerda, row 0 = topo). Digitos 0-9.
static const uint kFont[10] = {
    31599u, 9362u, 29671u, 31207u, 18925u, 31183u, 31695u, 18727u, 31727u, 31215u
};

float4 main(VSOut i) : SV_Target {
    int num       = (int)(i.info.x + 0.5f);
    int numDigits = (int)(i.info.y + 0.5f);

    // Qual digito cobre este uv.x.
    float cellW = 1.0f / (float)numDigits;
    int   cell  = (int)(i.uv.x / cellW);
    cell = clamp(cell, 0, numDigits - 1);
    float lx    = (i.uv.x - (float)cell * cellW) / cellW; // [0,1] dentro da celula

    // Valor do digito desta celula (mais significativo a esquerda).
    int place = numDigits - 1 - cell;
    int p10   = (place == 2) ? 100 : (place == 1) ? 10 : 1;
    int digit = (num / p10) % 10;

    // Margem dentro da celula (separa os digitos).
    if (lx < 0.12f || lx > 0.88f) discard;
    float fx = (lx - 0.12f) / 0.76f;          // [0,1]
    int   col = clamp((int)(fx * 3.0f), 0, 2);
    int   row = clamp((int)((1.0f - i.uv.y) * 5.0f), 0, 4); // uv.y 0=baixo -> row 4

    uint mask = kFont[digit];
    bool lit  = ((mask >> (uint)(row * 3 + col)) & 1u) != 0u;
    if (!lit) discard;
    return float4(1.0f, 1.0f, 1.0f, 1.0f); // texto branco
}
