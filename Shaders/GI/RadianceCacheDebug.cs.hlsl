// Visualizador do World Radiance Cache. Reconstroi a posicao de mundo do pixel a partir do depth,
// calcula a MESMA chave que o ShadeSurfaceHit calcularia ali e pinta o resultado.
//
// Reusa RC_GridLevel/RC_CellSize/RC_HashKey/RC_Find do RadianceCache.hlsli de proposito: um
// visualizador que recalcula a chave por conta propria mente exatamente quando mais importa —
// quando a chave esta errada. Aqui, se o hash divergir, os dois divergem juntos e a imagem para
// de fazer sentido, que e o sintoma que se quer ver.
//
// O modo 0 e a "hash visualization" do [Sousa2025] (slide 18): a estrutura das celulas aparece
// direto no mundo, e da para ver de olho se o tamanho e o crescimento por distancia estao certos.

#include "RadianceCache.hlsli"

cbuffer RadianceCacheDebugCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPosMode;   // xyz = camera, w = modo
    float4 ScreenParams;    // x = W, y = H, zw = 1/W, 1/H
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
};

Texture2D<float>    Depth     : register(t0);
// NormalBuffer (R10G10B10A2 do depth pre-pass), nao o G-buffer: a chave do cache usa a normal
// GEOMETRICA (`cacheN` no ShadeSurfaceHit), nao a de sombreamento. Ler o normal map aqui pintaria
// celulas que o cache nao usa. Mesmo encoding da GTAO: xyz * 2 - 1.
Texture2D<float4>   NormalTex : register(t1);
RWTexture2D<float4> Output    : register(u0);

#define RC_DEBUG_CELLS     0
#define RC_DEBUG_HITMISS   1
#define RC_DEBUG_LOD       2
#define RC_DEBUG_CONVERGED 3

float3 RC_LevelColor(uint level) {
    // Rampa discreta: nivel e inteiro, entao interpolar so borraria a fronteira que interessa ver.
    const float3 kRamp[6] = {
        float3(0.15f, 0.35f, 1.00f), float3(0.10f, 0.85f, 0.85f),
        float3(0.20f, 0.90f, 0.25f), float3(0.95f, 0.90f, 0.15f),
        float3(1.00f, 0.50f, 0.10f), float3(1.00f, 0.15f, 0.15f),
    };
    return kRamp[min(level, 5u)];
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const uint2 px = DTid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    Output[px] = float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) return; // ceu: nao ha superficie para indexar

    const float2 uv  = (px + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 worldPos = wH.xyz / wH.w;

    const float3 nRaw = NormalTex.Load(int3(px, 0)).xyz * 2.0f - 1.0f;
    const float  nLen = length(nRaw);
    if (nLen < 1e-4f) return; // pixel sem normal geometrica (masked fora do pre-pass)
    const float3 N = nRaw / nLen;

    // Pela macro, e nao por uma copia a dedo das dez atribuicoes: um campo novo no contrato (o
    // MinSamples entrou assim) nao pode depender de alguem lembrar de acrescenta-lo aqui tambem —
    // o visualizador leria zero e pintaria como confiavel exatamente a celula que a consulta
    // recusa.
    FRadianceCacheParams P;
    RC_UNPACK_PARAMS_INTO(P);
    if (P.Capacity == 0u) return; // cena sem cache montado

    const uint  level    = RC_GridLevel(worldPos, P.CameraPos, P.LodDistance);
    const float cellSize = RC_CellSize(level, P.BaseCellSize);

    uint slot, checksum;
    RC_HashKey(worldPos, N, level, cellSize, P.Capacity, slot, checksum);

    RWStructuredBuffer<uint> entries =
        ResourceDescriptorHeap[NonUniformResourceIndex(P.EntriesUAV)];
    uint entryIndex;
    const bool found = RC_Find(entries, slot, checksum, P.Capacity, entryIndex);

    uint sampleNum = 0u;
    uint age       = 0u;
    if (found) {
        RWStructuredBuffer<uint4> resolved =
            ResourceDescriptorHeap[NonUniformResourceIndex(P.ResolvedUAV)];
        const uint packed = resolved[entryIndex].w;
        sampleNum = packed & 0xFFFFu;
        age       = packed >> 16u;
    }

    const uint mode = (uint)CameraPosMode.w;
    float3 color = float3(0.0f, 0.0f, 0.0f);

    if (mode == RC_DEBUG_CELLS) {
        // Cor derivada do CHECKSUM, nao do slot: o slot ja passou pelo modulo da capacidade e
        // duas celulas distantes que colidem no indice ficariam da mesma cor — justamente o caso
        // que se quer enxergar como DIFERENTE.
        const uint h = RC_XXHash32(uint4(checksum, 0x9E37u, 0x85EBu, 0xC2B2u));
        color = float3((h & 0xFFu) / 255.0f,
                       ((h >> 8) & 0xFFu) / 255.0f,
                       ((h >> 16) & 0xFFu) / 255.0f);
        // Celula sem dado sai escura: a estrutura continua legivel e a cobertura fica obvia.
        if (sampleNum == 0u) color *= 0.18f;
    } else if (mode == RC_DEBUG_HITMISS) {
        // Isto mostra COBERTURA da superficie visivel, nao promete o resultado de um raio real:
        // so o caller conhece segmentLength e pode aplicar a guarda segmento < tamanho da celula.
        //
        // O piso de confianca entra aqui pela mesma razao que a chave e recalculada em vez de
        // reimplementada: um visualizador que diverge da consulta mente exatamente quando mais
        // importa. Sem ele, a celula de uma amostra sairia VERDE ("posso encerrar um caminho
        // aqui") enquanto o raio de verdade a recusa, e o aquecimento pareceria instantaneo.
        const uint refreshThreshold = RC_REFRESH_FRAME_MAX +
                                      (checksum & (RC_REFRESH_FRAME_MAX - 1u));
        const bool confident = sampleNum >= max(P.MinSamples, 1u);
        color = (confident && age < refreshThreshold)
                                ? float3(0.15f, 0.85f, 0.25f)  // retorno antecipado disponivel
              : confident       ? float3(1.00f, 0.45f, 0.08f)  // vai pedir refresh
              : sampleNum > 0u  ? float3(0.20f, 0.55f, 0.95f)  // aquecendo: abaixo do piso
              : found           ? float3(0.95f, 0.80f, 0.15f)  // chave ainda sem amostra
                                : float3(0.90f, 0.15f, 0.15f); // nunca preenchida
    } else if (mode == RC_DEBUG_LOD) {
        color = RC_LevelColor(level);
    } else {
        const float t = saturate((float)sampleNum / (float)RC_SAMPLE_NUM_MAX);
        color = lerp(float3(0.75f, 0.10f, 0.10f), float3(0.15f, 0.85f, 0.30f), t);
        if (!found) color = float3(0.05f, 0.05f, 0.05f);
    }

    Output[px] = float4(color, 1.0f);
}
