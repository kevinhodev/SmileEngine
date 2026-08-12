#ifndef SMILE_DDGI_COMMON_HLSLI
#define SMILE_DDGI_COMMON_HLSLI

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979323846f
#endif

#ifndef DDGI_IRRADIANCE_GAMMA
#define DDGI_IRRADIANCE_GAMMA 1.5f
#endif

// Instance masks da TLAS (RaytracingScene.cpp seta por instancia; espelhado em
// Smile/Graphics/RTMasks.h). Cada instancia carrega UM bit de categoria, e cada PASSE escolhe
// quais categorias enxerga — o modelo do Lumen (RayTracingDefinitions.h), onde o bit OPAQUE e
// descrito como "used by reflection, shadow, AO and GI tracing passes".
#define SMILE_RT_MASK_OPAQUE      0x01u
#define SMILE_RT_MASK_ALPHATEST   0x02u
// Translucido (material Blend: vidro, vitrine). Sem este bit o vidro entrava na TLAS junto com os
// opacos e, como nenhum RayQuery implementa transmissao, virava PAREDE para o GI — interior de
// ambiente envidracado ficava escuro demais. O raster ja o exclui do G-buffer (passe forward).
#define SMILE_RT_MASK_TRANSLUCENT 0x04u
#define SMILE_RT_MASK_ALL         0xFFu

// Gather difuso e visibilidade do GI: opaco + alpha-test, SEM translucido. Equivale ao
// RAY_TRACING_MASK_OPAQUE do Lumen, que traca o gather com ele sozinho
// (LumenHardwareRayTracingCommon.ush:195). Aproxima transmissao TOTAL pelo vidro — grosseiro, mas
// muito mais perto da fisica que bloquear 100%. Refracao/Fresnel/absorcao ficam p/ depois.
#define SMILE_RT_MASK_GATHER      (SMILE_RT_MASK_OPAQUE | SMILE_RT_MASK_ALPHATEST)

// Flags do InstanceGeo (bitmask em Flags).
#define INSTGEO_FLAG_ALPHATEST 1u  // clip por albedo.a vs AlphaCutoff (folhagem; FORCE_NON_OPAQUE na TLAS)
#define INSTGEO_FLAG_EMISSIVE  2u  // tem mapa emissivo (EmissiveMapIndex valido)
#define INSTGEO_FLAG_FOLIAGE   4u  // ShadingModel Foliage (two-sided + transmissao no PT)
#define INSTGEO_FLAG_MRMAP     8u  // tem mapa metallic-roughness (MrMapIndex valido)
#define INSTGEO_FLAG_SPECPACK  16u // o mapa MR e "Specular" (R=AO, G=rough, B=metal); senao R=metal
#define INSTGEO_FLAG_METALMAP  32u // tem mapa Metalness separado (MetalMapIndex valido; R=metal)
#define INSTGEO_FLAG_ROUGHMAP  64u // tem mapa Roughness separado (RoughMapIndex valido; R=rough)
#define INSTGEO_FLAG_TRANSLUCENT 128u // material Blend — kRTMaskTranslucent na TLAS. So o BvhDebug
                                      // usa: a categoria da instancia e a mask do raio, que o
                                      // shader nao consegue ler de volta da TLAS

// 80 bytes — casa campo-a-campo com DDGIInstanceGeo (DDGI.cpp). Campos alem do BaseColor/geometria
// alimentam o ReSTIR PT (emissivo, alpha-test, metal/rough); os shaders antigos ignoram os novos.
// VertexSrv/IndexSrv = indices bindless (ResourceDescriptorHeap) do VB/IB originais do mesh,
// 0-based por mesh — substituem os merged buffers (t4/t5 aposentados).
struct InstanceGeo {
    float4 BaseColor;
    uint   VertexSrv;
    uint   IndexSrv;
    uint   AlbedoIndex;
    uint   HasAlbedo;
    uint   TwoSidedRT;   // = FMaterial::IsTwoSidedForRT: TwoSided OU AlphaTest. NAO e a flag crua
                         // do material — cutout tem so um lado de geometria, entao p/ o RT ele e
                         // two-sided tanto no culling da TLAS quanto aqui.
    uint   Flags;
    float  AlphaCutoff;
    float  RoughnessFactor;
    float4 EmissiveFactor; // rgb = EmissiveFactor * EmissiveStrength; w = MetallicFactor
    uint   EmissiveMapIndex;
    uint   MrMapIndex;
    uint   MetalMapIndex; // mapa Metalness separado (slot +6); valido sob INSTGEO_FLAG_METALMAP
    uint   RoughMapIndex; // mapa Roughness separado (slot +7); valido sob INSTGEO_FLAG_ROUGHMAP
};

struct DDGIVertex {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
};

float3 DDGI_SphericalFibonacci(float i, float n) {
    const float PHI = 1.61803398875f;
    float phi = 2.0f * SMILE_PI * frac(i * (PHI - 1.0f));
    float cosTheta = 1.0f - (2.0f * i + 1.0f) / n;
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float DDGI_Hash(uint x) {
    x = (x ^ 61u) ^ (x >> 16); x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15;
    return (float)(x & 0x00FFFFFFu) / (float)0x01000000;
}

float3x3 DDGI_RandomRotation(uint frame) {
    float a = DDGI_Hash(frame * 3u + 0u) * 6.2831853f;
    float b = DDGI_Hash(frame * 3u + 1u) * 6.2831853f;
    float c = DDGI_Hash(frame * 3u + 2u) * 6.2831853f;
    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b), cc = cos(c), sc = sin(c);
    float3x3 rz = float3x3(ca, -sa, 0, sa, ca, 0, 0, 0, 1);
    float3x3 ry = float3x3(cb, 0, sb, 0, 1, 0, -sb, 0, cb);
    float3x3 rx = float3x3(1, 0, 0, 0, cc, -sc, 0, sc, cc);
    return mul(rz, mul(ry, rx));
}

// Rotacao por frame E por probe (Flax randomiza por-probe via quaternion): com rotacao unica
// por frame todos os probes amostram as MESMAS direcoes e o alias fica sincronizado no grid
// inteiro (flicker estruturado). Trace/Update/UpdateDist/Relocate do mesmo frame tem que passar
// o MESMO (frame, probeIdx) p/ reconstruir as direcoes tracadas.
float3 DDGI_RayDirection(int rayIdx, int numRays, uint frame, uint probeIdx) {
    float3x3 rot = DDGI_RandomRotation(frame + probeIdx * 2654435761u);
    return normalize(mul(rot, DDGI_SphericalFibonacci((float)rayIdx, (float)numRays)));
}

float2 DDGI_OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 o = (n.z >= 0.0f) ? n.xy : ((1.0f - abs(n.yx)) * sign(n.xy));
    return o; 
}

float3 DDGI_OctDecode(float2 f) {
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    return normalize(n);
}

int  DDGI_ProbeLinear(int3 c, int3 count) { return c.x + c.y * count.x + c.z * count.x * count.y; }

// Endereco no ProbesTrace (radiancia + distancia assinada de cada raio de cada sonda). Era uma
// linha por sonda, ou seja NumProbes virava a ALTURA de uma Texture2D e o grid parava em 16384
// sondas. Enrolado em blocos de kProbesPerRow sondas por linha, o teto vira
// kProbesPerRow * 16384 e deixa de ser o limite que morde.
//
// kProbesPerRow e CONSTANTE (nao parametro) para nao duplicar estado, mas E um knob: a primeira
// versao disto dizia que 256 "e o maior que cabe, entao nao ha o que calibrar" — e isso confunde
// CAPACIDADE com desempenho. 256*64 = 16384 enche a largura maxima, mas uma textura de 16384 de
// largura nao e necessariamente a melhor para cache/swizzle. Valores uteis: 64 (largura 4096),
// 128 (8192), 256 (16384). Espelhado em FDDGI::kTraceProbesPerRow — mudar aqui exige mudar la.
#define DDGI_TRACE_PROBES_PER_ROW 256

// Uma sonda por GRUPO nos tres passes pesados (trace, update, updateDist), e o D3D12 para em
// 65535 grupos por dimensao. Com o dispatch 1D o teto de sondas era esse, muito abaixo do que os
// atlas passaram a comportar depois do reempacotamento: os recursos seriam criados, o gridFits
// aprovaria, e o Dispatch e que ficaria invalido em runtime. Dispatch 2D, com o indice da sonda
// linearizado a partir de Gid.xy.
//
// A largura da grade de grupos NAO e parametro: sai de numProbes com aritmetica inteira, do mesmo
// jeito e pelo mesmo motivo que o tilesPerRow sai da largura do atlas — CPU e GPU calculam o
// MESMO numero em vez de trocar um numero que poderia dessincronizar. E escolhida para dividir
// numProbes o mais exato possivel (sobra < numero de linhas), entao nao se paga uma faixa de
// grupos que so testa e retorna.
#define DDGI_DISPATCH_GROUPS_X 1024

int DDGI_DispatchGroupsX(int numProbes) {
    int rows = max((numProbes + DDGI_DISPATCH_GROUPS_X - 1) / DDGI_DISPATCH_GROUPS_X, 1);
    return max((numProbes + rows - 1) / rows, 1);
}

int DDGI_ProbeFromGroup(uint2 gid, int numProbes) {
    return (int)gid.x + (int)gid.y * DDGI_DispatchGroupsX(numProbes);
}

int2 DDGI_TraceTexel(int probeIdx, int rayIdx, int numProbes, int raysPerProbe) {
    // Cena pequena nao paga a linha inteira: a largura alocada e min(numProbes, 256)*raios.
    int perRow = min(numProbes, DDGI_TRACE_PROBES_PER_ROW);
    perRow     = max(perRow, 1);
    return int2((probeIdx % perRow) * raysPerProbe + rayIdx, probeIdx / perRow);
}

int3 DDGI_ProbeCoord(int idx, int3 count) {
    int xy = count.x * count.y;
    int z  = idx / xy;
    int r  = idx - z * xy;
    int y  = r / count.x;
    int x  = r - y * count.x;
    return int3(x, y, z);
}

// Quantos tiles cabem numa LINHA do atlas. Nao e campo proprio de cbuffer de proposito: sai da
// LARGURA, que todo consumidor do gather ja carrega ao lado do tamanho do tile, e assim nao existe
// o estado duplicado que poderia dessincronizar. A largura E tilesPerRow*(tile+2) por construcao
// no SetupForScene, entao esta divisao inteira e exata nos dois lados.
//
// Os dois atlas (irradiancia, tile 6+2, e distancia, tile 14+2) usam o MESMO tilesPerRow — o
// indice do tile e o da SONDA, e a sonda e a mesma. Derivar de qualquer um dos dois da o mesmo
// numero, o que e a propriedade que torna seguro derivar em vez de passar.
int DDGI_TilesPerRow(float atlasW, int tile) {
    return max((int)atlasW / (tile + 2), 1);
}

// Origem do INTERIOR do tile: cada tile tem 1px de borda octaedrica de cada lado (stride =
// tile+2, como paper/RTXGI/Flax) — a borda recebe copia com o fold do octaedro (tabelas nos
// passes de update) p/ o bilinear ser continuo na costura em vez do clamp achatar o anel externo.
//
// Layout: o plano (x,z) corre nas COLUNAS e o y nas LINHAS — a mesma vizinhanca do layout
// historico de fileira — so que o plano e ENROLADO em bandas de `tilesPerRow` colunas quando nao
// cabe numa linha so. Com uma banda, isto e bit a bit o layout antigo.
//
// Por que a fileira precisou morrer: a largura era `count.x*count.z*(tile+2)`, e no atlas de
// distancia (stride 16) isso estourava o limite de dimensao de Texture2D com 1024 colunas de
// sonda. O Bistro ja usava 552 — uma segunda cascata nao caberia. Com bandas o teto vira o
// PRODUTO (~1 milhao de sondas) em vez de um par de eixos.
//
// Por que NAO e o indice linear da sonda enrolado, que seria mais simples: o gather le OITO
// sondas vizinhas, e no plano-em-colunas as quatro de (x..x+1, y..y+1) caem num bloco 2x2 de
// tiles ADJACENTES — vizinhas na horizontal e na vertical. Enrolar o `DDGI_ProbeLinear` mantinha
// so o par em x junto e jogava y a `count.x` tiles de distancia; medido, custou ~0,4 ms de
// espera no Bistro com 9.920 sondas. A propriedade que o layout antigo tinha nao era acidente, e
// e ela que este formato preserva.
// As CASCATAS empilham em blocos de LINHAS, uma depois da outra. Escolha deliberada: cada cascata
// mantem a propria grade intacta, entao o bloco 2x2 de tiles adjacentes vale dentro de cada uma —
// intercalar cascatas na largura ou no indice destruiria justamente isso. E o mesmo lugar onde a
// Flax as empilha (probesCountTotalY = probesCountCascadeY * cascadesCount).
//
// zRowsPerBand e o numero de fileiras Z por banda; sai de tilesPerRow / count.x porque o
// SetupForScene garante que tilesPerRow e SEMPRE multiplo de count.x (ver atlasGridFor). Dai saem
// as bandas e as linhas por cascata, tudo em inteiro e sem campo novo de cbuffer.
int DDGI_TileRowsPerCascade(int3 count, int tilesPerRow) {
    int zRowsPerBand = max(tilesPerRow / max(count.x, 1), 1);
    int bands        = (count.z + zRowsPerBand - 1) / zRowsPerBand;
    return bands * count.y;
}

int2 DDGI_TileOrigin(int3 c, int3 count, int tile, int tilesPerRow, int cascade) {
    int plane  = c.x + c.z * count.x;
    int band   = plane / tilesPerRow;
    int col    = plane - band * tilesPerRow;
    int row    = c.y + band * count.y + cascade * DDGI_TileRowsPerCascade(count, tilesPerRow);
    int stride = tile + 2;
    return int2(col * stride, row * stride) + 1;
}

// Espaco de indice das sondas: cascata c ocupa [c*porCascata, (c+1)*porCascata). Os buffers
// (ProbeData, ProbeRayCount) e o ProbesTrace usam o indice GLOBAL; a geometria — posicao no
// mundo, gaiola, vizinhos — usa o LOCAL, com o (GridMin, Spacing) daquela cascata.
int  DDGI_ProbesPerCascade(int3 count) { return count.x * count.y * count.z; }
int  DDGI_CascadeOfProbe(int globalIdx, int3 count) {
    return globalIdx / max(DDGI_ProbesPerCascade(count), 1);
}
int  DDGI_LocalProbeIndex(int globalIdx, int3 count) {
    int per = max(DDGI_ProbesPerCascade(count), 1);
    return globalIdx - (globalIdx / per) * per;
}
int  DDGI_GlobalProbeIndex(int localIdx, int cascade, int3 count) {
    return localIdx + cascade * DDGI_ProbesPerCascade(count);
}

#define DDGI_MAX_CASCADES 4
// Largura do blend fino->grosso, em CELULAS medidas na cascata que esta saindo. A Flax usa 2,5
// sondas (DDGI_CASCADE_BLEND_SIZE); corte seco deixaria uma costura visivel exatamente onde as
// duas estimativas mais divergem, que e a borda da fina.
#define DDGI_CASCADE_BLEND_CELLS 2.5f

// Peso de permanencia na cascata: 1 no miolo, caindo a 0 nas ultimas DDGI_CASCADE_BLEND_CELLS
// celulas antes da borda da gaiola. Fora dela, 0.
//
// Medido na posicao CRUA, sem o bias de superficie. Isto e a "gaiola pelo ponto cru" que a
// auditoria exige que entre junto com a cascata, e aqui ela deixa de ser detalhe: o bias escala
// com o espacamento, entao numa cascata de 2 m ele vale ate 0,30 m — se a selecao usasse o ponto
// viesado, dois pixels vizinhos de uma parede rasante poderiam escolher cascatas DIFERENTES por
// causa de um deslocamento artificial. O Flax faz igual (baseProbeCoords sai de worldPosition, e
// o viesado so entra no alpha do trilinear).
float DDGI_CascadeWeight(float3 rawPos, float3 gridMin, float spacing, int3 count) {
    float3 gridMax = gridMin + (float3)(count - 1) * spacing;
    float  margin  = 0.5f * spacing; // a gaiola vai meia celula alem da ultima sonda
    float3 inside  = min(rawPos - (gridMin - margin), (gridMax + margin) - rawPos);
    float  d       = min(inside.x, min(inside.y, inside.z));
    return saturate(d / max(DDGI_CASCADE_BLEND_CELLS * spacing, 1e-4f));
}

// Qual cascata serve o ponto, e com quem ela se mistura na borda.
//   uma cascata      -> { 0, 0, 1 }
//   dentro de uma fina -> { fina, fina+1, peso }
//   fora das finas   -> { grossa, grossa, 1 }
// `Next == Primary` e o sinal de "nao ha blend": quem consome usa isso para pular o segundo
// gather inteiro, que e o custo dominante.
struct DDGICascadeChoice {
    int   Primary;
    int   Next;
    float PrimaryWeight;
};

// Cascata mais FINA que contem o ponto, com o peso de permanencia nela (1 = so ela; <1 = mistura
// com a proxima).
//
// So as cascatas ANTERIORES a grossa desvanecem. A grossa e fallback INCONDICIONAL: peso 1 em
// qualquer ponto, e quem cuida da borda externa dela continua sendo o DDGI_VolumeWeight — que e
// configuravel (VolumeFadeProbes), existe desde antes das cascatas e desvanece para o ambiente
// hemisferico ao longo de N celulas, nao 2,5. Aplicar o fade de cascata tambem nela substituiria
// aquele em silencio, e a equivalencia com o comportamento historico morreria exatamente quando o
// seletor fosse ligado — sem erro de compilacao em lugar nenhum.
//
// O contrato com quem chama: o gate de "esta dentro do volume da cena" ja foi feito la fora, pelo
// DDGI_VolumeWeight sobre a GROSSA. Aqui so se escolhe QUAL cascata serve o ponto.
DDGICascadeChoice DDGI_SelectCascade(float3 rawPos, float4 cascades[DDGI_MAX_CASCADES],
                                     int cascadeCount, int3 count) {
    DDGICascadeChoice ch;
    const int coarse = max(cascadeCount - 1, 0);
    for (int c = 0; c < coarse; ++c) {
        float w = DDGI_CascadeWeight(rawPos, cascades[c].xyz, cascades[c].w, count);
        if (w > 0.0f) {
            ch.Primary = c; ch.Next = c + 1; ch.PrimaryWeight = w;
            return ch;
        }
    }
    ch.Primary = coarse; ch.Next = coarse; ch.PrimaryWeight = 1.0f;
    return ch;
}

float3 DDGI_ProbeWorldPos(int3 c, float3 gridMin, float spacing) {
    return gridMin + (float3)c * spacing;
}

// Teste da caixa de invalidacao regional (FDDGI::InvalidateRegion), com a posicao RELOCADA da
// sonda. Compartilhado pelos DOIS atlas de proposito: se a irradiancia testasse a caixa com a
// posicao de grid e a distancia com a relocada (ou vice-versa), uma sonda perto de parede —
// justamente a que a relocacao move — poderia entrar em um e ficar de fora do outro, e o
// resultado seria iluminacao nova com visibilidade velha, que e o defeito que isto fecha.
//
// A hysteresis regional entra por parametro porque os dois atlas NAO usam a mesma: ver o bloco
// no DDGIUpdateDist sobre reconstrucao vs atraso.
float DDGI_RegionalHysteresis(int3 probeCoord, float3 probeOffset, float3 gridMin, float spacing,
                              float4 invMin, float3 invMax, float regionalHyst, float hyst) {
    if (invMin.w <= 0.5f) return hyst;
    float3 probePos = DDGI_ProbeWorldPos(probeCoord, gridMin, spacing) + probeOffset;
    if (all(probePos >= invMin.xyz) && all(probePos <= invMax))
        return min(hyst, regionalHyst);
    return hyst;
}

// === Histerese adaptativa, dirigida pela MUDANCA MEDIDA ==================================
// Invalidacao por evento e uma WHITELIST: falha em silencio quando alguem esquece de avisar.
// Mover objeto pelo gizmo, esconder no olho do outliner e editar cor/intensidade de luz
// puntual mudam a cena e NAO avisavam o DDGI — com histerese alta isso vira luz fantasma por
// segundos. Este detector e a rede: compara a estimativa nova com a guardada e derruba a
// histerese quando a sonda mudou de verdade, sem depender de ninguem avisar.
//
// NAO substitui a invalidacao por regiao (FDDGI::InvalidateRegion), que continua sendo a
// resposta barata e dirigida — o detector so paga o preco DEPOIS que a mudanca ja apareceu na
// estimativa. E rede, nao chao.
//
// Receita do "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production"
// (JCGT 2021); o paper de 2019 tem alfa fixo entre 0.85 e 0.98.

// Denominador SIMETRICO com piso, E banda morta no NUMERADOR. As duas coisas, nao so a
// primeira: com piso sozinho, 0 -> 0.02 (preto -> 0,003 de radiancia linear) ainda daria
// rel = 1.0 e derrubaria a histerese ao maximo. O detector amplificaria exatamente o ruido que
// existe para ignorar, e sonda escura em interior e justamente onde a exposicao mais levanta o
// sinal. Subtrair a mesma banda mata isso SEM degrau — um `if` cortando no piso faria a sonda
// trocar de regime ao cruza-lo.
//
// Efeito colateral, que e o desejado: a banda desloca `rel` em exatamente floorV/m, entao o
// detector perde sensibilidade conforme a sonda escurece e fica mudo no piso — "mudanca menor
// que o chao de ruido nao e mudanca". Para uma sonda de brilho normal (m ~ 1) o deslocamento e
// 0,02, dentro da folga dos limiares da curva de resposta.
//
// A medida continua em [0,1] POR CONSTRUCAO, que e o que garante que o acumulador de ponto
// fixo dos chamadores nao estoure.
float DDGI_RelChange3(float3 a, float3 b, float floorV) {
    float3 d = max(abs(a - b) - floorV, 0.0f);
    float3 m = max(max(abs(a), abs(b)), floorV);
    float3 r = d / m;
    return max(r.x, max(r.y, r.z)); // max por canal, nao luminancia: luz colorida acendendo
                                    // (so o R muda) some numa media ponderada
}

// Curva de resposta: dois limiares, e so REDUZ (o `min` no fim). Aumentar a histerese aqui
// deixaria o detector SEGURANDO luz velha, que e o oposto do que ele existe para fazer.
//
// A medida chega no dominio GAMMA do atlas (o update guarda pow(L, 1/DDGI_IRRADIANCE_GAMMA)),
// entao um fator r de radiancia LINEAR aparece aqui como r' = r^(1/1.5), e a medida simetrica
// vale rel = 1 - 1/r'. A calibracao, convertida:
//   rel 0.50  <=>  2,8x de luz linear -> 0.90 (~29 frames; mesmo valor da invalidacao regional)
//   rel 0.80  <=>   11x de luz linear -> 0.50 (~4 frames)
// Entre os dois a queda e continua: um degrau apareceria como pop na borda do efeito, que e
// exatamente o artefato que a histerese existe para evitar.
//
// A calibracao ORIGINAL era 0.20/0.50 e foi REPROVADA em A/B (2026-08-11): flicker espalhado
// com o detector ligado. A causa nao e o valor em si, e a comparacao — `result` e uma
// estimativa Monte Carlo de ~32 raios com peso positivo, e o ruido relativo dela neste dominio
// e ~0,15-0,25. O limiar de 0,20 estava DENTRO do ruido, entao a sonda de alta variancia
// disparava o detector todo frame e ficava presa em histerese reduzida — o detector produzindo
// exatamente o ruido que existe para filtrar.
//
// 0.50 poe o gatilho a ~2-3 sigma do ruido do estimador. O que o detector perde com isso e
// sensibilidade a mudanca sutil, e isso deixou de importar: depois que movimento, visibilidade
// e edicao de luz passaram a invalidar por evento, o que sobra para a rede pegar sao mudancas
// de ORDEM DE GRANDEZA (a coisa que ninguem ligou), nao de 40%.
//
// O que ISTO ainda nao resolve, e por que o knob segue default-OFF: uma sonda com variancia
// muito alta (emissivo pequeno no campo de visao) continua podendo cruzar 0.50 por ruido. A
// correcao definitiva e exigir PERSISTENCIA — a mesma mudanca em N frames seguidos, sendo que
// ruido troca de sinal e mudanca real nao —, e isso precisa de estado por sonda, que nao
// existe hoje.
float DDGI_AdaptiveHysteresis(float rel, float hyst) {
    const float kRelModerate  = 0.50f;
    const float kRelBig       = 0.80f;
    const float kHystModerate = 0.90f;
    const float kHystBig      = 0.50f;
    if (rel <= kRelModerate) return hyst;
    float t = saturate((rel - kRelModerate) / (kRelBig - kRelModerate));
    return min(hyst, lerp(kHystModerate, kHystBig, t));
}

float3 DDGI_SampleProbe(Texture2D<float4> atlas, SamplerState samp, int2 tileOriginPx,
                        int tile, float2 atlasInvSize, float3 dir) {
    float2 oct = DDGI_OctEncode(dir) * 0.5f + 0.5f;
    float2 px  = (float2)tileOriginPx + oct * tile; // sem clamp: bilinear cruza p/ a borda copiada
    float3 s   = atlas.SampleLevel(samp, px * atlasInvSize, 0.0f).rgb;
    return pow(max(s, 0.0f), DDGI_IRRADIANCE_GAMMA);
}

float3 SampleDDGIIrradiance(Texture2D<float4> atlas, SamplerState samp, float3 worldPos,
                            float3 N, float3 gridMin, float spacing, int3 count,
                            int tile, float2 atlasInvSize, int tilesPerRow, int cascade) {
    float3 g    = (worldPos - gridMin) / spacing;
    int3   base = (int3)floor(g);
    float3 frac = saturate(g - (float3)base);

    float3 sum  = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);

        float3 tw = lerp(1.0f - frac, frac, (float3)off);
        float  w  = tw.x * tw.y * tw.z;
        if (w <= 0.0f) continue;

        int2   tileOrigin = DDGI_TileOrigin(c, count, tile, tilesPerRow, cascade);
        float3 irr = DDGI_SampleProbe(atlas, samp, tileOrigin, tile, atlasInvSize, N);
        sum  += irr * w;
        wsum += w;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

float2 DDGI_SampleProbeRG(Texture2D<float4> distAtlas, SamplerState samp, int2 tileOriginPx,
                          int tile, float2 atlasInvSize, float3 dir) {
    float2 oct = DDGI_OctEncode(dir) * 0.5f + 0.5f;
    float2 px  = (float2)tileOriginPx + oct * tile; // sem clamp: bilinear cruza p/ a borda copiada
    return distAtlas.SampleLevel(samp, px * atlasInvSize, 0.0f).rg;
}

// Self-shadow bias do paper (e do Flax, GetDDGISurfaceBias): desloca o ponto de amostragem na
// normal E na direcao da camera — o componente de view e o que evita dark banding/shadow leak
// em parede vista de raspao, onde bias so-normal nao tira o ponto da zona de auto-oclusao.
//
// `scale` = o `bias` do Flax (0.2 = legado). `maxMeters` = TETO ABSOLUTO em metros; 0 desliga o
// teto e reproduz o comportamento historico bit a bit.
//
// Por que o teto existe: a formula original escala com o espacamento do grid, e o grid daqui e
// dimensionado pela AABB da cena inteira (DDGI.cpp: spacing = maxExt/23). No Bistro isso da
// spacing = 8,02 m medido, ou seja 0.75*8.02*0.2 = 1,20 m de deslocamento — o ponto de
// amostragem atravessa parede e le a celula do outro lado. O Flax nao sofre disso porque a
// cascata mais fina tem spacing de ~1 m; o RTXGI resolveu tornando normalBias/viewBias
// absolutos (metros), independentes do grid. O teto e a versao barata dessa correcao: preserva
// o comportamento em cena pequena e corta o absurdo em cena grande.
float3 DDGI_SurfaceBias(float3 N, float3 V, float spacing, float scale, float maxMeters) {
    float s = 0.75f * spacing * scale;
    if (maxMeters > 0.0f) s = min(s, maxMeters);
    return (N * 0.2f + V * 0.8f) * s;
}

// Peso do volume: 1 em TODO ponto dentro do volume, caindo a 0 ao longo de `fadeProbes` celulas
// DEPOIS da borda, do lado de fora. `fadeProbes = 0` desliga (1 em todo lugar = historico).
//
// Por que existe: o gather CLAMPA as coordenadas do grid, entao um ponto fora do volume nao
// falha — ele le as probes da borda e as estende ao infinito. O terreno fica fora de proposito
// (SceneLoader: um terreno de km esticaria o grid), e o deferred desliga o IBL difuso quando o
// GI esta ligado, entao hoje o terreno inteiro recebe a irradiancia da ultima fileira de probes.
// O Flax trata isso com FallbackIrradiance e um peso de cascata que cai na borda
// (DDGI.hlsl:315-345); aqui, com um volume so, o fallback e o ambiente hemisferico da atmosfera,
// que e exatamente o que o deferred usaria se o GI estivesse desligado.
//
// O fade e para FORA, e nao para dentro como no Flax, porque os volumes sao diferentes: la ele e
// folgado e centrado na camera, entao desvanecer nas bordas internas nao custa nada. Aqui ele e
// justo (AABB da cena + meia celula), entao TODA a geometria fica perto de alguma face — o chao
// nasce a meia celula da face inferior. Fade para dentro lavaria o piso inteiro com ambiente
// hemisferico, que e pior que o problema original.
float DDGI_VolumeWeight(float3 worldPos, float3 gridMin, float spacing, int3 count,
                        float fadeProbes) {
    if (fadeProbes <= 0.0f) return 1.0f;
    // Meia celula de margem dos DOIS lados. Nao e folga arbitraria: o grid e ancorado em
    // AABBMin - 0.5*spacing e o numero de probes e ceil(extensao/spacing) + 1, entao a ultima
    // fileira cai em AABBMax - 0.5*spacing quando a extensao e multipla do espacamento — que e
    // exatamente o caso do eixo dominante (spacing = maxExt/23, count = 24). Sem a margem, os
    // ultimos ~4 m da PROPRIA cena entrariam como "fora" e a face AABBMax sairia com peso 0,5.
    // A mesma margem do lado negativo mantem o teste simetrico; la nao ha cena mesmo.
    float  margin  = 0.5f * spacing;
    float3 gridMax = gridMin + (float3)(count - 1) * spacing;
    float3 inside  = min(worldPos - (gridMin - margin), (gridMax + margin) - worldPos);
    float  d       = min(inside.x, min(inside.y, inside.z)); // <0 = fora daquela face
    return saturate(1.0f + min(d, 0.0f) / (fadeProbes * spacing));
}

// Um tap do gather com Chebyshev: tudo que decide o peso de UMA das 8 probes da celula.
// SampleDDGIIrradianceCheb consome so o Weight; o diagnostico pontual (DDGIDebugPoint.cs)
// publica os intermediarios. Os dois passam por DDGI_EvaluateTapCheb — mexer no peso sem
// mexer no diagnostico deixaria a ferramenta MENTINDO, que e pior que nao ter ferramenta.
struct DDGITapCheb {
    int3  Coord;        // pode diferir de base+off: o fallback procura vizinho ativo
    uint  Index;        // linear de Coord
    bool  Ignored;      // probe inativa e sem substituta -> nao contribui
    float DistToProbe;
    float Mean;         // 1o momento na direcao probe->ponto
    float Mean2;        // 2o momento (sigma sai daqui, so o debug usa)
    float Trilinear;    // ja com o piso de 0.001
    float Visibility;   // Chebyshev^3 com piso; 1.0 quando nao ha oclusao a testar
    float Weight;       // final: backface * visibilidade * crush * trilinear
};

DDGITapCheb DDGI_EvaluateTapCheb(
        int i, int3 base, float3 frac, float3 biasPos, float3 rawPos, float3 N,
        float3 gridMin, float spacing, int3 count,
        Texture2D<float4> distAtlas, SamplerState samp, int distTile, float2 distInvSize,
        Buffer<float4> probeData, uint skipMode, int tilesPerRow, int cascade) {
    int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);

    DDGITapCheb tap;
    tap.Ignored    = false;
    tap.Visibility = 1.0f;

    // Indice GLOBAL para falar com o probeData: os buffers sao indexados por
    // `cascata*porCascata + local`, e a coordenada `c` aqui e LOCAL da cascata. Ler pelo local
    // pegaria a relocacao e a classificacao da cascata 0 em qualquer cascata acima dela — dormente
    // enquanto so existe uma, e silencioso quando deixar de existir.
    uint index = (uint)DDGI_GlobalProbeIndex(DDGI_ProbeLinear(c, count), cascade, count);
    if (skipMode != 0u && probeData[index].w < 0.0f) {
        if (skipMode >= 2u) {
            const int3 axisMask[3] = { int3(1,0,0), int3(0,1,0), int3(0,0,1) };
            bool found = false;
            [loop] for (int sd = 1; sd < 3 && !found; ++sd) {
                for (int ax = 0; ax < 3; ++ax) {
                    int  dir = (off[ax] != 0) ? 1 : -1;
                    int3 sc  = clamp(c + axisMask[ax] * (dir * sd), int3(0, 0, 0), count - 1);
                    // A busca por vizinho ativo nunca sai da cascata: `sc` e clampado no grid
                    // dela, entao o substituto tem de ser lido no bloco dela.
                    uint candidate = (uint)DDGI_GlobalProbeIndex(
                        DDGI_ProbeLinear(sc, count), cascade, count);
                    if (probeData[candidate].w >= 0.0f) {
                        c = sc; index = candidate; found = true; break;
                    }
                }
            }
            tap.Ignored = !found;
        } else {
            tap.Ignored = true;
        }
    }
    tap.Coord = c;
    tap.Index = index;

    // Posicao REAL do probe (grid + offset de relocacao): o trace dispara os raios e o
    // dist atlas mede distancias a partir do probe relocado — o Chebyshev e o peso de
    // backface tem que usar a mesma origem, senao o teste de visibilidade compara contra
    // a posicao errada exatamente nos probes que foram movidos por estar perto de parede.
    float3 probePos     = DDGI_ProbeWorldPos(c, gridMin, spacing) + probeData[index].xyz;
    float3 probeToPoint = biasPos - probePos;
    tap.DistToProbe     = length(probeToPoint);
    float3 dirPP        = probeToPoint / max(tap.DistToProbe, 1e-4f);

    // Direcao do teste de backface: da posicao SEM bias, como o Flax (DDGI.hlsl:210-215). O
    // teste pergunta de que lado da superficie REAL a probe esta, e o bias e um deslocamento
    // artificial que, quando grande, muda a resposta — com 1,2 m o ponto viesado ja podia estar
    // do outro lado da parede, e ai a probe de la parecia "de frente". A distancia/Chebyshev
    // continuam medidos do ponto viesado, que e a separacao que o Flax faz.
    float3 rawToProbe  = rawPos - probePos;
    float3 dirBackface = rawToProbe / max(length(rawToProbe), 1e-4f);

    // Pesos DEFENSIVOS com piso: backface e Chebyshev se auto-sabotam em geometria densa/fina
    // (miolo de sebe, cantos, frestas) — os pisos garantem que wsum nunca colapsa a zero, entao a
    // consulta nunca devolve preto absoluto, so escurece. Os pisos do Chebyshev (0.05), do peso
    // final (1e-6), do trilinear (0.001) e a curva de crush sao mesmo do Flax (DDGI.hlsl:214-240).
    //
    // O `+ 0.05` no termo de BACKFACE nao e: la o teste e `Square(dot(...) * 0.5 + 0.5)` puro, e
    // no paper tambem. O comentario anterior atribuia o somando ao Flax, e isso era FALSO. Ele
    // afrouxa o cull — a sonda que esta do lado de tras da superficie guarda 5% de peso em vez de
    // zero, o que injeta luz do outro lado da parede em canto fechado. Fica ate ter A/B proprio:
    // e o unico piso desta funcao que nao veio de lugar nenhum.
    float backface = dot(-dirBackface, N) * 0.5f + 0.5f;
    float w = backface * backface + 0.05f;

    int2   distOrigin = DDGI_TileOrigin(c, count, distTile, tilesPerRow, cascade);
    float2 md = DDGI_SampleProbeRG(distAtlas, samp, distOrigin, distTile, distInvSize, dirPP);
    tap.Mean  = md.x;
    tap.Mean2 = md.y;
    if (tap.DistToProbe > tap.Mean) {
        float variance = abs(tap.Mean * tap.Mean - tap.Mean2);
        float d        = tap.DistToProbe - tap.Mean;
        float cheb     = variance / max(variance + d * d, 1e-8f);
        tap.Visibility = max(cheb * cheb * cheb, 0.05f);
        w *= tap.Visibility;
    }
    w = max(w, 1e-6f);

    // Curva de crush suave p/ pesos baixos ("inject a small portion of light"): mantem a
    // penalizacao dos probes ocluidos sem a transicao dura do corte a zero.
    const float minWeightThreshold = 0.2f;
    if (w < minWeightThreshold)
        w *= (w * w) / (minWeightThreshold * minWeightThreshold);

    float3 trilin = lerp(1.0f - frac, frac, (float3)off);
    tap.Trilinear = max(trilin.x * trilin.y * trilin.z, 0.001f);
    w *= tap.Trilinear;

    tap.Weight = tap.Ignored ? 0.0f : w;
    return tap;
}

float3 SampleDDGIIrradianceCheb(
        Texture2D<float4> irrAtlas, Texture2D<float4> distAtlas, SamplerState samp,
        float3 worldPos, float3 N, float3 gridMin, float spacing, int3 count,
        int irrTile, float2 irrInvSize, int distTile, float2 distInvSize, float3 biasVec,
        Buffer<float4> probeData, uint skipMode, int tilesPerRow, int cascade) {
    float3 biasPos = worldPos + biasVec;
    float3 g    = (biasPos - gridMin) / spacing;
    int3   base = (int3)floor(g);
    float3 frac = saturate(g - (float3)base);

    float3 sum  = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        DDGITapCheb tap = DDGI_EvaluateTapCheb(i, base, frac, biasPos, worldPos, N,
                                               gridMin, spacing, count, distAtlas, samp,
                                               distTile, distInvSize, probeData, skipMode,
                                               tilesPerRow, cascade);
        if (tap.Ignored) continue;

        int2   irrOrigin = DDGI_TileOrigin(tap.Coord, count, irrTile, tilesPerRow, cascade);
        float3 irr = DDGI_SampleProbe(irrAtlas, samp, irrOrigin, irrTile, irrInvSize, N);
        sum  += irr * tap.Weight;
        wsum += tap.Weight;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

// === Gather com cascatas ==================================================================
// Os DOIS wrappers abaixo sao o unico lugar onde selecao e blend acontecem. Cada consumidor que
// compusesse isso por conta propria seria mais uma copia para divergir — foi assim que a ordem do
// atlas e o clique do visualizador se separaram uma vez.
//
// Sao DOIS e nao um porque o fog volumetrico nao tem atlas de distancia nem ProbeData: obriga-lo
// a passar pelo caminho do Chebyshev acrescentaria bindings e acoplamento sem lhe dar nada.
//
// O que eles NAO fazem, de proposito: o peso do volume (DDGI_VolumeWeight) e o fallback de fora
// dele. Cada caller tem o seu — o deferred e o fog caem no ambiente hemisferico, o HitShading cai
// em PRETO (nao existe ambiente colorido no cbuffer de um passe de RT). Devolver "irradiancia do
// DDGI e mais nada" e o que mantem essa diferenca com o dono dela.
//
// A selecao usa SEMPRE a posicao CRUA. Ver DDGI_CascadeWeight.

float3 SampleDDGIIrradianceCascaded(
        Texture2D<float4> atlas, SamplerState samp, float3 rawPos, float3 N,
        float4 cascades[DDGI_MAX_CASCADES], int cascadeCount, int3 count,
        int tile, float2 atlasInvSize, int tilesPerRow) {
    DDGICascadeChoice ch = DDGI_SelectCascade(rawPos, cascades, cascadeCount, count);
    float3 primary = SampleDDGIIrradiance(atlas, samp, rawPos, N,
                                          cascades[ch.Primary].xyz, cascades[ch.Primary].w,
                                          count, tile, atlasInvSize, tilesPerRow, ch.Primary);
    if (ch.Next == ch.Primary || ch.PrimaryWeight >= 0.999f) return primary;

    float3 next = SampleDDGIIrradiance(atlas, samp, rawPos, N,
                                       cascades[ch.Next].xyz, cascades[ch.Next].w,
                                       count, tile, atlasInvSize, tilesPerRow, ch.Next);
    return lerp(next, primary, ch.PrimaryWeight);
}

// ⚠️ O BIAS E RECALCULADO POR CASCATA, e e o erro mais facil de cometer aqui. Ele nao entra
// pronto porque `DDGI_SurfaceBias` escala com o ESPACAMENTO: ele e um deslocamento de
// auto-sombra dimensionado para a densidade de sondas daquela cascata, entao usar o de outra e
// simplesmente usar o parametro errado.
//
// A magnitude, com os defaults (escala 0,2 e teto de 0,40 m): a fina de 2 m pede
// 0,75*2*0,2 = 0,30 m; a grossa de 8 m pediria 1,20 m e leva o teto, ficando em 0,40 m. A
// diferenca e 0,10 m — 0,05 celula da fina, nao uma celula inteira. Com o teto DESLIGADO
// (SurfaceBiasMax = 0, o comportamento historico) ela vai a 0,90 m, ou 0,45 celula.
//
// O risco nao e o tamanho do passo, e o que ele atravessa: 0,10 m ja e da ordem da espessura de
// parede do Bistro, e a razao de o teto existir e justamente a amostra nao cruzar para o outro
// lado. Errar isso reaparece como leak na faixa do blend, que e onde as duas estimativas ja
// divergem mais. Por isso a assinatura pede (V, escala, teto) e nao um biasVec pronto.
float3 SampleDDGIIrradianceChebCascaded(
        Texture2D<float4> irrAtlas, Texture2D<float4> distAtlas, SamplerState samp,
        float3 rawPos, float3 N, float3 V,
        float4 cascades[DDGI_MAX_CASCADES], int cascadeCount, int3 count,
        int irrTile, float2 irrInvSize, int distTile, float2 distInvSize,
        Buffer<float4> probeData, uint skipMode, int tilesPerRow,
        float biasScale, float biasMax) {
    DDGICascadeChoice ch = DDGI_SelectCascade(rawPos, cascades, cascadeCount, count);

    float3 primary = SampleDDGIIrradianceCheb(
        irrAtlas, distAtlas, samp, rawPos, N,
        cascades[ch.Primary].xyz, cascades[ch.Primary].w, count,
        irrTile, irrInvSize, distTile, distInvSize,
        DDGI_SurfaceBias(N, V, cascades[ch.Primary].w, biasScale, biasMax),
        probeData, skipMode, tilesPerRow, ch.Primary);
    if (ch.Next == ch.Primary || ch.PrimaryWeight >= 0.999f) return primary;

    float3 next = SampleDDGIIrradianceCheb(
        irrAtlas, distAtlas, samp, rawPos, N,
        cascades[ch.Next].xyz, cascades[ch.Next].w, count,
        irrTile, irrInvSize, distTile, distInvSize,
        DDGI_SurfaceBias(N, V, cascades[ch.Next].w, biasScale, biasMax),
        probeData, skipMode, tilesPerRow, ch.Next);
    return lerp(next, primary, ch.PrimaryWeight);
}

#endif
