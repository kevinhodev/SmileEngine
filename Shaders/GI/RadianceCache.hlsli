#ifndef SMILE_RADIANCE_CACHE_HLSLI
#define SMILE_RADIANCE_CACHE_HLSLI

// ============================================================================================
// World Radiance Cache — cache de RADIANCIA DE SAIDA em espaco de mundo, indexado por hash
// espacial multirresolucao.
//
// POR QUE EXISTE
// O atlas do DDGI guarda IRRADIANCIA (ja convoluida com o cosseno) numa grade de posicao fixa,
// e o `ShadeSurfaceHit` amostrava esse atlas no ponto de hit tratando o resultado como se fosse
// radiancia de saida. Sao coisas diferentes: irradiancia e o que CHEGA, radiancia de saida e o
// que PARTE. A aproximacao chapa o 2o bounce e, pior, o espacamento do grid vem da AABB da cena
// (`DDGI.cpp`, `SpacingV = maxExt/23` — ~8 m no Bistro), entao o hit le uma media de um volume
// enorme. Este cache guarda o que o `ShadeSurfaceHit` REALMENTE produziu naquele ponto, em
// celulas de 50 cm perto da camera no preset calibrado atual.
//
// O cache NAO substitui o DDGI. O atlas continua sendo a resposta suave e sempre disponivel que
// o deferred le por pixel; este aqui e o TERMINADOR DE RAIO — o que um raio secundario "ve"
// quando bate em algo. Nas tres fontes de 2025 os dois coexistem (idTech 8: cache alimenta os
// irradiance volumes; AC Shadows: radiance 12x12 + irradiance 7x7 por probe; Cyberpunk: SHaRC
// alimenta o path tracer sem substituir nada).
//
// FONTES
//  [Gautron2020]  "Real-Time Ray-Traced Ambient Occlusion of Complex Scenes using Spatial
//                 Hashing", SIGGRAPH Talks. E a base do esquema de hash: duas chaves 32 bits
//                 independentes (indice + CHECKSUM), sondagem linear na colisao, tamanho de
//                 celula adaptativo embutido na chave (eq. 3-4) e normal na chave (eq. 5).
//  [Sousa2025]    "Fast as Hell: idTech 8 Global Illumination", SIGGRAPH Advances. Cita o
//                 Gautron; celulas de 25 cm + LOD por distancia da camera; 14 MB no DOOM TDA.
//  [SHaRC]        NVIDIA RTXGI 2.0. Mesma familia; de la vem a separacao acumulacao/resolvido,
//                 o teto de amostras e a politica de eviccao por frames sem uso.
//
// POR QUE CHECKSUM DE 32 BITS E NAO CHAVE DE 64
// A SHaRC usa chave de 64 bits e exige `InterlockedCompareExchange64` (SM 6.6 + Int64ShaderOps
// no device). O Gautron resolve o mesmo problema com DUAS funcoes de hash de 32 bits sobre as
// mesmas coordenadas: uma da o indice, a outra o checksum guardado na celula. Colisao de indice
// que tambem colida no checksum e possivel mas raríssima, e o custo e um atomico de 32 bits, que
// nao tem gate de hardware nenhum. A engine nao consulta Int64ShaderOps hoje — este caminho
// evita ter que passar a consultar.
//
// DUAS TABELAS, NAO UMA (anti-realimentacao)
// A escrita vai para `Accum` (deste frame) e a leitura vem de `Resolved` (frames anteriores,
// produzido pelo passe de resolve). Se fosse uma tabela so, uma superficie leria no mesmo frame
// o valor que ela propria acabou de escrever e a energia entraria em laco — o mesmo modo de
// falha que reprovou a correcao de vies do temporal do ReSTIR GI no A/B de 2026-08-07.
// ============================================================================================

#define RC_INVALID_CHECKSUM   0u
// Slot que ja foi ocupado e depois despejado. Nao pode voltar a 0: em enderecamento aberto,
// `vazio` encerra a busca e uma remocao no meio da cadeia esconderia todas as colisoes que vieram
// depois. O tombstone preserva a cadeia e continua sendo reutilizavel pela insercao.
#define RC_TOMBSTONE_CHECKSUM 0xFFFFFFFFu
// Sondagem linear a partir do slot: [Gautron2020] "a linear search finds the next free space".
// 32 e o BUCKET_SIZE da SHaRC; acima disso o custo da busca supera o ganho de nao descartar.
#define RC_BUCKET_SIZE      32u
// Ponto fixo para o InterlockedAdd. 1e4 e a RADIANCE_SCALE do idTech 8.
#define RC_RADIANCE_SCALE   1e4f
// Teto por amostra ANTES do ponto fixo. Com RC_SAMPLE_NUM_MAX amostras de RC_RADIANCE_MAX, o
// acumulador chega a 64 * 6.5e3 * 1e4 = 4.16e9, logo abaixo do estouro de uint32 (4.29e9).
// Sem este teto um unico firefly transborda o acumulador e a celula volta com lixo.
#define RC_RADIANCE_MAX     6.5e3f
#define RC_SAMPLE_NUM_MAX   64u
#define RC_STALE_FRAME_MAX  64u
// Idade (em frames sem realimentacao) a partir da qual a proxima consulta REFRESCA a celula em vez
// de aproveita-la: ela devolve "erro" de proposito, o chamador sombreia inteiro e o RC_Update
// zera a idade.
//
// Existe porque o acerto e um RETORNO ANTECIPADO no ShadeSurfaceHit, antes do RC_Update — logo a
// celula que passa a acertar para de ser alimentada, envelhece ate RC_STALE_FRAME_MAX e e
// DESPEJADA. O cache matava exatamente as celulas que estavam funcionando (medido no Bistro
// 2026-08-08: amostras/celula caiu 41,8 -> 16,8 e 2466 despejos/frame ao ligar a leitura).
//
// E o "reaproveitadas por N frames" do [Sousa2025], nao um sorteio: a idade ja esta em
// Resolved[].w, entao a decisao nao custa leitura nenhuma e cada celula refresca em intervalo
// conhecido — que e o que devolve resposta a mudanca de iluminacao (TOD).
// TEM de ser bem menor que RC_STALE_FRAME_MAX, senao a corrida contra o despejo e perdida.
// POTENCIA DE 2: o escalonamento abaixo usa mascara.
#define RC_REFRESH_FRAME_MAX 8u

// Contadores do painel. Os quatro primeiros sao produzidos pelo RESOLVE (varredura da tabela);
// os dois ultimos pelos TRACES, dentro do RC_Query. Ver RadianceCacheResolve.cs.hlsl.
#define RC_STAT_OCCUPIED 0u  // celulas com chave valida
#define RC_STAT_VALID    1u  // dessas, quantas ja tem amostra (radiancia utilizavel)
#define RC_STAT_SAMPLES  2u  // soma de N — dividido por VALID da a convergencia media
#define RC_STAT_EVICTED  3u
#define RC_STAT_QUERIES  4u
#define RC_STAT_HITS     5u
#define RC_STAT_COUNT    6u

// Publicado no CB de cada consumidor do HitShading.hlsli, igual ao FReGIRShaderParams.
//
// TUDO por UAV, inclusive o que so e lido. O `ShadeSurfaceHit` consulta (le Entries e Resolved) e
// atualiza (atomico em Entries e Accum) na MESMA invocacao — se a leitura fosse por SRV o recurso
// teria que estar em NON_PIXEL_SHADER_RESOURCE e em UNORDERED_ACCESS ao mesmo tempo, o que nao
// existe. Ler um RWStructuredBuffer e legal; o preco e nao passar pelo caminho de cache
// somente-leitura. A SHaRC faz igual pelo mesmo motivo.
struct FRadianceCacheParams {
    float3 CameraPos;    float BaseCellSize;  // aresta da celula no nivel 0, em metros
    float  LodDistance;  uint  Capacity;      // potencia de 2 (mascara = Capacity - 1)
    uint   EntriesUAV;   uint  AccumUAV;
    uint   ResolvedUAV;  uint  StatsUAV;
    uint   Flags;                             // bit0 = query, bit1 = update, bit2 = stats
};

#define RC_FLAG_QUERY  1u
#define RC_FLAG_UPDATE 2u
// Instrumentacao de acerto/erro do query. Separada dos outros dois porque CUSTA: mesmo com a
// reducao por wave, sao dois atomicos disputados por todo o dispatch. Liga so com o painel aberto.
#define RC_FLAG_STATS  4u

// Contrato de CBUFFER, por NOME — igual ao RayEpsA/RayEpsB e ao GIDistParams. Todo consumidor do
// HitShading.hlsli declara estas tres linhas no b0 dele (anexadas no FIM da struct C++
// correspondente, para nao deslocar offset nenhum) e chama a macro abaixo.
//
// Macro e nao funcao porque o desempacotamento e identico nos cinco traces (DDGI, ReSTIR GI,
// reflexao, reflexao espelhada e agua) e escrever oito atribuicoes cinco vezes e exatamente o
// tipo de lista mantida a dedo que o RenderPass.h existe para extinguir: bastaria UMA delas
// esquecer o ResolvedUAV para aquele consumidor ler lixo, e o sintoma sairia como radiancia
// errada so em um dos caminhos.
#define RC_UNPACK_PARAMS(P)                                        \
    P.Cache.CameraPos    = RadianceCacheCamCell.xyz;               \
    P.Cache.BaseCellSize = RadianceCacheCamCell.w;                 \
    P.Cache.LodDistance  = RadianceCacheLodCapFlags.x;             \
    P.Cache.Capacity     = (uint)RadianceCacheLodCapFlags.y;       \
    P.Cache.Flags        = (uint)RadianceCacheLodCapFlags.z;       \
    P.Cache.EntriesUAV   = (uint)RadianceCacheResources.x;         \
    P.Cache.AccumUAV     = (uint)RadianceCacheResources.y;         \
    P.Cache.ResolvedUAV  = (uint)RadianceCacheResources.z;         \
    P.Cache.StatsUAV     = (uint)RadianceCacheResources.w;

// --------------------------------------------------------------------------------------------
// Hash
// --------------------------------------------------------------------------------------------
// xxHash32 [Jarzynski & Olano 2020], a MESMA familia que o GGXSample.hlsli usa para semear os
// sorteios. Reimplementado aqui em vez de incluir aquele header porque o RadianceCache e incluido
// pelo HitShading, que e incluido por DDGITrace/ReSTIRGITrace/ReflectionTrace — puxar
// GGXSample.hlsli para dentro dessa cadeia acoplaria o cache a um header de amostragem de GGX,
// que nao tem nada a ver. A duplicacao e de 8 linhas e as constantes sao as do paper.
uint RC_XXHash32(uint4 p) {
    const uint PRIME32_2 = 2246822519u, PRIME32_3 = 3266489917u;
    const uint PRIME32_4 = 668265263u,  PRIME32_5 = 374761393u;
    uint h32 = p.w + PRIME32_5 + p.x * PRIME32_3;
    h32 = PRIME32_4 * ((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.y * PRIME32_3;
    h32 = PRIME32_4 * ((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.z * PRIME32_3;
    h32 = PRIME32_4 * ((h32 << 17) | (h32 >> (32 - 17)));
    h32 = PRIME32_2 * (h32 ^ (h32 >> 15));
    h32 = PRIME32_3 * (h32 ^ (h32 >> 13));
    return h32 ^ (h32 >> 16);
}

// Nivel de detalhe pela distancia a camera [Sousa2025]: celulas crescem em potencias de 2 para
// que uma parede a 100 m nao consuma mil celulas de 25 cm. O `1.0 +` garante nivel 0 colado na
// camera. O log2/floor e o `s_wd = 2^floor(log2(s_w/s_min))` da eq. 3 do [Gautron2020].
uint RC_GridLevel(float3 samplePos, float3 cameraPos, float lodDistance) {
    const float d = length(cameraPos - samplePos);
    return (uint)max(floor(log2(1.0f + d / max(lodDistance, 1e-3f))), 0.0f);
}

float RC_CellSize(uint level, float baseCellSize) {
    return baseCellSize * exp2((float)level);
}

// Monta indice e checksum a partir de posicao quantizada + normal quantizada + nivel.
//
// A normal entra na chave porque sem ela os dois lados de uma parede fina caem na MESMA celula.
// O octante segue o Listing 7.6 do SHaRC: separa faces opostas sem fragmentar superficies curvas
// em dezenas de combinacoes instaveis, especialmente em folhagem.
void RC_HashKey(float3 samplePos, float3 sampleNormal, uint level, float cellSize,
                uint capacity, out uint slot, out uint checksum) {
    const int3  posInt = (int3)floor(samplePos / cellSize);
    // SHaRC Listing 7.6: keep only the normal octant. This separates opposing faces without
    // fragmenting curved surfaces into many unstable normal buckets.
    const uint normalBits = (sampleNormal.x >= 0.0f ? 1u : 0u)
                          | (sampleNormal.y >= 0.0f ? 2u : 0u)
                          | (sampleNormal.z >= 0.0f ? 4u : 0u);
    const uint4 key    = uint4((uint)posInt.x, (uint)posInt.y, (uint)posInt.z,
                               level | (normalBits << 8));
    slot = RC_XXHash32(key) & (capacity - 1u);
    // Segunda funcao sobre AS MESMAS coordenadas [Gautron2020]. O `key.w ^ 0x9E3779B9` (razao
    // aurea de Knuth) descorrelaciona os dois hashes; sem isso o checksum seria uma funcao
    // deterministica do indice e nao detectaria colisao nenhuma.
    checksum = RC_XXHash32(uint4(key.xyz, key.w ^ 0x9E3779B9u));
    // 0 marca slot que nunca foi usado e 0xFFFFFFFF marca tombstone. Nenhum checksum legitimo
    // pode assumir os sentinelas.
    checksum = (checksum == RC_INVALID_CHECKSUM || checksum == RC_TOMBSTONE_CHECKSUM)
             ? 1u : checksum;
}

// --------------------------------------------------------------------------------------------
// Busca (sem atomicos) — caminho de QUERY
// --------------------------------------------------------------------------------------------
bool RC_Find(RWStructuredBuffer<uint> entries, uint slot, uint checksum, uint capacity,
             out uint entryIndex) {
    entryIndex = 0u;
    for (uint i = 0u; i < RC_BUCKET_SIZE; ++i) {
        const uint idx = (slot + i) & (capacity - 1u);
        const uint stored = entries[idx];
        if (stored == checksum) { entryIndex = idx; return true; }
        // Vazio VERDADEIRO = a celula nunca foi inserida: a sondagem teria parado aqui. Tombstone
        // nao encerra a busca, porque pode haver uma colisao viva depois de uma entrada despejada.
        if (stored == RC_INVALID_CHECKSUM) return false;
    }
    return false;
}

// --------------------------------------------------------------------------------------------
// Insercao (atomica) — caminho de UPDATE
// --------------------------------------------------------------------------------------------
bool RC_Insert(RWStructuredBuffer<uint> entries, uint slot, uint checksum, uint capacity,
               out uint entryIndex) {
    entryIndex = 0u;
    // Primeiro PROCURA a chave ate o fim da cadeia, guardando o primeiro slot reutilizavel. Nao
    // podemos tomar o tombstone imediatamente: a mesma chave pode continuar viva mais adiante.
    // Depois tenta publicar no slot escolhido por CAS; se outra chave ganhar a corrida, recomeca.
    [loop] for (uint attempt = 0u; attempt < RC_BUCKET_SIZE; ++attempt) {
        uint reusable = 0xFFFFFFFFu;
        uint expected = RC_INVALID_CHECKSUM;

        for (uint i = 0u; i < RC_BUCKET_SIZE; ++i) {
            const uint idx    = (slot + i) & (capacity - 1u);
            const uint stored = entries[idx];
            if (stored == checksum) { entryIndex = idx; return true; }

            if (stored == RC_TOMBSTONE_CHECKSUM && reusable == 0xFFFFFFFFu) {
                reusable = idx;
                expected = RC_TOMBSTONE_CHECKSUM;
            }
            if (stored == RC_INVALID_CHECKSUM) {
                if (reusable == 0xFFFFFFFFu) {
                    reusable = idx;
                    expected = RC_INVALID_CHECKSUM;
                }
                break; // nada desta cadeia existe depois do primeiro vazio verdadeiro
            }
        }

        if (reusable == 0xFFFFFFFFu) return false; // balde cheio, sem tombstone

        uint prev;
        InterlockedCompareExchange(entries[reusable], expected, checksum, prev);
        if (prev == expected || prev == checksum) {
            entryIndex = reusable;
            return true;
        }
        // Outra chave tomou o candidato entre a leitura e o CAS. A nova varredura encontra tanto
        // ela quanto uma insercao concorrente da NOSSA chave e escolhe o proximo reutilizavel.
    }
    // Balde cheio: descarta a amostra. [Gautron2020] aceita o mesmo ("fingers crossed we fit,
    // else we have to drop some results") — perder uma amostra e melhor que despejar uma celula
    // viva de outra regiao.
    return false;
}

// --------------------------------------------------------------------------------------------
// API de alto nivel
// --------------------------------------------------------------------------------------------
// ONDE a consulta parou. Um bool respondia "aproveitei?" e perdia o resto — e o resto e o que a
// telemetria de miss da Fase 4 vai querer separar: miss por chave, por celula fria, por segmento
// curto e por cone estreito sao problemas DIFERENTES (capacidade, aquecimento, geometria do
// caminho, e um limite do proprio cache nao-direcional). Com um bool, os quatro chegam ao painel
// como o mesmo numero.
//
// Ordenado pelo ponto de desistencia, do mais cedo ao mais tarde.
#define RC_QUERY_DISABLED      0u // flag de query off: nem olhou a tabela
#define RC_QUERY_SHORT_SEGMENT 1u // segmento menor que a celula: seria auto-referencia
#define RC_QUERY_NARROW_CONE   2u // cone especular menor que a celula: o RGB apagaria a direcao
#define RC_QUERY_NO_ENTRY      3u // chave nao encontrada (ausente ou balde cheio)
#define RC_QUERY_NO_SAMPLES    4u // celula existe, ainda sem radiancia
#define RC_QUERY_STALE         5u // tem dado, mas velho: o chamador sombreia e o update refresca
#define RC_QUERY_HIT           6u // aproveitavel

struct FRCQueryResult {
    float3 Radiance;    // zerada em tudo que nao for HIT (ver a nota no RC_QueryEx)
    float  CellSize;    // aresta da celula no nivel resolvido; 0 se nem chegou a calcular
    uint   SampleCount; // N da celula (0 quando nao ha entrada)
    uint   Age;         // frames sem realimentacao
    uint   Status;      // RC_QUERY_*
};

bool RC_QueryHit(FRCQueryResult R) { return R.Status == RC_QUERY_HIT; }

// Le a radiancia resolvida (frames ANTERIORES) na posicao.
//
// `segmentLength` e o comprimento do segmento de raio que chegou aqui. Consultar o cache com
// segmento MENOR que a celula e auto-referencia: origem e hit caem na mesma celula e o valor
// devolvido descreve a propria superficie de origem. E a mesma guarda do `GetVoxelSize()` da
// SHaRC e do "continue tracing until the path segment length is bigger than the voxel size" do
// [Sousa2025].
FRCQueryResult RC_QueryInner(FRadianceCacheParams P, float3 samplePos, float3 sampleNormal,
                             float segmentLength, float rayRoughness) {
    FRCQueryResult R = (FRCQueryResult)0;

    const uint  level    = RC_GridLevel(samplePos, P.CameraPos, P.LodDistance);
    const float cellSize = RC_CellSize(level, P.BaseCellSize);
    R.CellSize = cellSize;
    if (segmentLength < cellSize) { R.Status = RC_QUERY_SHORT_SEGMENT; return R; }

    // SHaRC only reuses cached radiance for glossy rays once their cone covers a voxel. Before
    // that point an RGB cache would erase directional detail. Negative roughness means diffuse.
    if (rayRoughness >= 0.0f) {
        const float alpha  = saturate(rayRoughness) * saturate(rayRoughness);
        const float alpha2 = alpha * alpha;
        const float coneSpread = 2.0f * segmentLength
                               * sqrt(0.5f * alpha2 / max(1.0f - alpha2, 1e-6f));
        if (coneSpread < cellSize) { R.Status = RC_QUERY_NARROW_CONE; return R; }
    }

    uint slot, checksum;
    RC_HashKey(samplePos, sampleNormal, level, cellSize, P.Capacity, slot, checksum);

    RWStructuredBuffer<uint> entries =
        ResourceDescriptorHeap[NonUniformResourceIndex(P.EntriesUAV)];
    uint entryIndex;
    if (!RC_Find(entries, slot, checksum, P.Capacity, entryIndex)) {
        R.Status = RC_QUERY_NO_ENTRY;
        return R;
    }

    RWStructuredBuffer<uint4> resolved =
        ResourceDescriptorHeap[NonUniformResourceIndex(P.ResolvedUAV)];
    const uint4 packed    = resolved[entryIndex];
    const uint  sampleNum = packed.w & 0xFFFFu;
    R.SampleCount = sampleNum;
    if (sampleNum == 0u) { R.Status = RC_QUERY_NO_SAMPLES; return R; }

    // Idade em frames sem realimentacao — o MESMO campo que o resolve incrementa e usa para
    // despejar. Ja veio no `packed`: a decisao de refrescar nao custa leitura extra.
    //
    // O limiar e escalonado pelo checksum. Sem isso todas as celulas nascidas no mesmo frame
    // vencem no mesmo frame e o custo do refresh vira um pico periodico em vez de um piso
    // constante — o mesmo motivo pelo qual o secondary shadow map do AC Shadows e time-sliced.
    const uint age       = packed.w >> 16u;
    const uint threshold = RC_REFRESH_FRAME_MAX + (checksum & (RC_REFRESH_FRAME_MAX - 1u));
    R.Age      = age;
    R.Radiance = asfloat(packed.xyz);
    R.Status   = (age >= threshold) ? RC_QUERY_STALE : RC_QUERY_HIT;
    return R;
}

FRCQueryResult RC_QueryEx(FRadianceCacheParams P, float3 samplePos, float3 sampleNormal,
                          float segmentLength, float rayRoughness) {
    FRCQueryResult R = (FRCQueryResult)0;
    R.Status = RC_QUERY_DISABLED;
    if ((P.Flags & RC_FLAG_QUERY) == 0u) return R;

    R = RC_QueryInner(P, samplePos, sampleNormal, segmentLength, rayRoughness);
    const bool hit = RC_QueryHit(R);
    // A celula tem dado, mas quem pediu vai sombrear do zero: devolver a radiancia velha junto
    // seria um pe no caminho de quem ler isto depois. O SampleCount e a Age SOBREVIVEM ao STALE de
    // proposito — sao diagnostico, e "velha ha quantos frames" e justamente o que se quer saber.
    if (!hit) R.Radiance = float3(0.0f, 0.0f, 0.0f);

    // RC_STAT_HITS conta o RETORNO ANTECIPADO, nao a cobertura da tabela — e o numero que mapeia
    // para custo economizado, e o que mantem a serie comparavel com as medidas de antes do
    // refresh. Cobertura seria `Status >= RC_QUERY_STALE`, e hoje nao e reportada.
    //
    // A contagem de atomicos aqui e CONGELADA: um a mais mudaria o escalonamento das waves e, com
    // ele, quais threads vencem as insercoes do cache — medido, 73.218 celulas contra 73.195 entre
    // os regimes com e sem instrumentacao. Acrescentar contador por Status e mudanca de imagem no
    // regime instrumentado, nao adicao inocente de telemetria.
    [branch] if ((P.Flags & RC_FLAG_STATS) != 0u) {
        // UM atomico por WAVE, nao por lane: sao milhoes de consultas por frame e dois enderecos
        // so. WaveActiveCountBits conta as lanes ATIVAS, entao vale sob divergencia — que e a
        // regra aqui, ja que o ShadeSurfaceHit so roda em quem acertou geometria.
        const uint waveQueries = WaveActiveCountBits(true);
        const uint waveHits    = WaveActiveCountBits(hit);
        if (WaveIsFirstLane()) {
            RWStructuredBuffer<uint> stats =
                ResourceDescriptorHeap[NonUniformResourceIndex(P.StatsUAV)];
            uint ignored;
            InterlockedAdd(stats[RC_STAT_QUERIES], waveQueries, ignored);
            if (waveHits > 0u) InterlockedAdd(stats[RC_STAT_HITS], waveHits, ignored);
        }
    }
    return R;
}

// Forma booleana, mantida para quem so quer "aproveitei?" — hoje, todos os consumidores.
bool RC_Query(FRadianceCacheParams P, float3 samplePos, float3 sampleNormal,
              float segmentLength, float rayRoughness, out float3 radiance) {
    const FRCQueryResult R = RC_QueryEx(P, samplePos, sampleNormal, segmentLength, rayRoughness);
    radiance = R.Radiance;
    return RC_QueryHit(R);
}

// Acumula a radiancia de saida que o ShadeSurfaceHit produziu. Ponto fixo + InterlockedAdd
// porque varias threads escrevem a mesma celula no mesmo frame.
void RC_Update(FRadianceCacheParams P, float3 samplePos, float3 sampleNormal, float3 radiance) {
    if ((P.Flags & RC_FLAG_UPDATE) == 0u) return;

    const uint  level    = RC_GridLevel(samplePos, P.CameraPos, P.LodDistance);
    const float cellSize = RC_CellSize(level, P.BaseCellSize);

    uint slot, checksum;
    RC_HashKey(samplePos, sampleNormal, level, cellSize, P.Capacity, slot, checksum);

    RWStructuredBuffer<uint> entries =
        ResourceDescriptorHeap[NonUniformResourceIndex(P.EntriesUAV)];
    uint entryIndex;
    if (!RC_Insert(entries, slot, checksum, P.Capacity, entryIndex)) return;

    // NaN/Inf viram 0 aqui e nao no resolve: um asuint de NaN somado ao acumulador contamina a
    // celula para sempre (o resolve nao teria como distinguir depois).
    // Ternario ESCALAR por componente: `select` e o ternario com condicao VETORIAL so existem em
    // HLSL 2021, e o DXC aqui compila no modo anterior (a engine nao passa -HV 2021).
    float3 safe;
    safe.r = isfinite(radiance.r) ? radiance.r : 0.0f;
    safe.g = isfinite(radiance.g) ? radiance.g : 0.0f;
    safe.b = isfinite(radiance.b) ? radiance.b : 0.0f;
    safe = clamp(safe, 0.0f, RC_RADIANCE_MAX);

    RWStructuredBuffer<uint> accum = ResourceDescriptorHeap[NonUniformResourceIndex(P.AccumUAV)];
    const uint base = entryIndex * 4u;

    // O teto do Resolved nao limitava quantas threads podiam somar no Accum neste frame. No pior
    // caso, 67 amostras no clamp ja ultrapassavam uint32 (6500 * 1e4 * 67). Reserve uma das 64
    // vagas por CAS ANTES de tocar RGB; quem chega depois descarta a amostra sem contaminar a
    // media. A barreira UAV antes do resolve garante que contagem e canais terminaram.
    [loop] for (;;) {
        const uint count = accum[base + 3u];
        if (count >= RC_SAMPLE_NUM_MAX) return;
        uint prevCount;
        InterlockedCompareExchange(accum[base + 3u], count, count + 1u, prevCount);
        if (prevCount == count) break;
    }

    uint ignored;
    InterlockedAdd(accum[base + 0u], (uint)(safe.r * RC_RADIANCE_SCALE), ignored);
    InterlockedAdd(accum[base + 1u], (uint)(safe.g * RC_RADIANCE_SCALE), ignored);
    InterlockedAdd(accum[base + 2u], (uint)(safe.b * RC_RADIANCE_SCALE), ignored);
}

#endif
