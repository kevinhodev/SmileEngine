#ifndef SMILE_TEMPORAL_MOTION_COMMON
#define SMILE_TEMPORAL_MOTION_COMMON

#include "../Scene/GpuSceneTables.hlsli"

struct FTemporalInstanceTransform {
    row_major float4x4 CurrentToPrevious;
    row_major float4x4 PreviousToCurrent;
};

// Inversa 4x4 GERAL, transcrita linha a linha do `Smile::TMat44::Inverse` (Mat44.h), inclusive o
// corte de determinante e o retorno IDENTIDADE para matriz singular. A transcricao usa um array
// plano com os MESMOS indices do C++ de proposito: e o unico jeito de conferir dezesseis linhas de
// cofator a olho, e um sinal trocado numa delas nao daria erro em lugar nenhum — daria reprojecao
// errada so em movimento.
//
// Geral, e nao a forma afim barata. As duas dao o mesmo resultado para TRS, mas a afim depende de
// uma garantia que nada aqui verifica; especializar e um passo de MEDICAO, com a paridade em WARP
// como rede. O custo nao e o de memoria que a fase persegue: sao ~200 ALU sem trafego nenhum.
float4x4 TemporalInvert4x4(row_major float4x4 M) {
    float m[16];
    m[ 0] = M[0][0]; m[ 1] = M[0][1]; m[ 2] = M[0][2]; m[ 3] = M[0][3];
    m[ 4] = M[1][0]; m[ 5] = M[1][1]; m[ 6] = M[1][2]; m[ 7] = M[1][3];
    m[ 8] = M[2][0]; m[ 9] = M[2][1]; m[10] = M[2][2]; m[11] = M[2][3];
    m[12] = M[3][0]; m[13] = M[3][1]; m[14] = M[3][2]; m[15] = M[3][3];

    float inv[16];
    inv[ 0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[ 4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[ 8] =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[ 9];
    inv[12] = -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[ 9];
    inv[ 1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[ 5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[ 9] = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[ 9];
    inv[13] =  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[ 9];
    inv[ 2] =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[ 6];
    inv[ 6] = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[ 6];
    inv[10] =  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[ 5];
    inv[14] = -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[ 5];
    inv[ 3] = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[ 9]*m[2]*m[ 7] + m[ 9]*m[3]*m[ 6];
    inv[ 7] =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[ 8]*m[2]*m[ 7] - m[ 8]*m[3]*m[ 6];
    inv[11] = -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[ 9] - m[ 8]*m[1]*m[ 7] + m[ 8]*m[3]*m[ 5];
    inv[15] =  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[ 9] + m[ 8]*m[1]*m[ 6] - m[ 8]*m[2]*m[ 5];

    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    // MESMO corte do C++ (1e-8). Identidade para singular nao e defesa cosmetica: escala zero e um
    // estado que o editor produz, e um NaN daqui contaminaria a reprojecao do frame inteiro.
    if (abs(det) < 1.0e-8f) {
        return float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 1.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, 1.0f);
    }
    const float invDet = 1.0f / det;

    float4x4 R;
    R[0] = float4(inv[ 0], inv[ 1], inv[ 2], inv[ 3]) * invDet;
    R[1] = float4(inv[ 4], inv[ 5], inv[ 6], inv[ 7]) * invDet;
    R[2] = float4(inv[ 8], inv[ 9], inv[10], inv[11]) * invDet;
    R[3] = float4(inv[12], inv[13], inv[14], inv[15]) * invDet;
    return R;
}

// O par derivado, a partir das DUAS colunas canonicas da tabela de instancias (Fase 4).
//
// Era uma tabela propria de 128 B por instancia, em ping-pong, reescrita inteira por frame na CPU
// -- com duas inversas 4x4 POR INSTANCIA POR FRAME. Os dados de entrada ja moravam na tabela
// canonica: o que existia aqui era uma VISAO derivada deles, e derivada no lugar errado.
//
// A ordem do produto e a mesma do C++: `A * B` do Mat44 e o produto linha-major, identico a
// `mul(A, B)` sobre `row_major`. Trocar compila, executa, e reprojeta para o lugar errado.
FTemporalInstanceTransform TemporalComposeInstanceTransform(row_major float4x4 current,
                                                            row_major float4x4 previous) {
    FTemporalInstanceTransform Out;
    Out.CurrentToPrevious = mul(TemporalInvert4x4(current), previous);
    Out.PreviousToCurrent = mul(TemporalInvert4x4(previous), current);
    return Out;
}

// Os dois sentidos SEPARADOS, e não é redundância com o par acima.
//
// Nenhum consumidor precisa dos dois no mesmo ponto: o `DualMotion` usa `CurrentToPrevious` para o
// receptor e `PreviousToCurrent` só no ramo de desoclusão; `ReflectionMotion` e `ReSTIRDISpatial`
// usam só o primeiro. Chamar o par e descartar metade custaria uma inversa a mais POR LEITURA — e
// contar com o compilador para eliminar a metade morta é apostar em DCE numa conta que o gate mede.
//
// O par continua existindo porque é ele que o C++ produz hoje, e é sobre ele que a paridade
// compara. Cada um destes é comparado com a metade correspondente no mesmo teste.
float4x4 TemporalCurrentToPrevious(row_major float4x4 current, row_major float4x4 previous) {
    return mul(TemporalInvert4x4(current), previous);
}

float4x4 TemporalPreviousToCurrent(row_major float4x4 current, row_major float4x4 previous) {
    return mul(TemporalInvert4x4(previous), current);
}

float4x4 TemporalIdentity4x4() {
    return float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);
}

// Maior id que sobrevive ao round-trip pelo canal float da superficie.
//
// O valor gravado e (id + 1) num float32, que representa inteiros exatamente ate 2^24 INCLUSIVE --
// entao o maior ID e 2^24 - 1 (0xFFFFFF), cujo encoded e exatamente 2^24. E o mesmo teto do
// InstanceID do DXR, que tem 24 bits.
//
// Quem constroi a TLAS recusa contagens acima de 2^24 (ver kMaxTemporalInstanceCount, no lado da
// CPU): sem essa recusa, o excesso nao daria erro -- daria o OBJETO VIZINHO, em silencio.
#define SMILE_TEMPORAL_MAX_INSTANCE_ID 16777215u

bool TemporalDecodeInstance(float encoded, uint instanceCount, out uint instanceId) {
    // Sem o "+ 0.5f" que estava aqui. Ele parecia arredondamento defensivo e na pratica
    // CORTAVA A FAIXA PELA METADE: a soma em float32 nao e representavel a partir de 2^23,
    // entao o id 8388608 decodificava como 8388609 -- o objeto vizinho, sem erro nenhum.
    // Medido nas duas bordas (2^23 e 2^24 - 2).
    //
    // A defesa nao era necessaria: o valor vem de um Load (sem filtragem) de um canal que
    // recebeu um inteiro exato, entao truncar ja e exato.
    const uint value = (uint)encoded;
    instanceId = value > 0u ? value - 1u : 0u;
    return value > 0u && instanceId < instanceCount;
}

// Entrada da tabela de identidades. MESMO layout do FGpuIdentityEntry no C++, onde ha
// static_assert de tamanho E de offset -- trocar a ordem aqui nao daria erro de compilacao,
// daria dados trocados.
struct FGpuIdentityEntry {
    uint Slot;
    uint Generation;
};

// Resolve o endereco canonico do objeto ATUAL e valida a generation antes de tocar nas matrizes.
// A leitura de Meta e inevitavel; ela tambem carrega o bit que evita duas matrizes e uma inversa
// para o caso estatico, que domina cenas de editor.
bool TemporalResolveCanonicalSlot(uint instanceIndex,
                                  StructuredBuffer<FGpuIdentityEntry> currentIdentity,
                                  StructuredBuffer<GpuInstanceMeta> instanceMeta,
                                  uint currentCount,
                                  uint instanceTableCapacity,
                                  out uint slot,
                                  out bool transformChanged) {
    slot = 0u;
    transformChanged = false;
    if (instanceIndex >= currentCount) return false;

    const FGpuIdentityEntry identity = currentIdentity[instanceIndex];
    if (identity.Generation == 0u || identity.Slot >= instanceTableCapacity) return false;
    const GpuInstanceMeta meta = instanceMeta[identity.Slot];
    if (meta.Generation != identity.Generation) return false;

    slot = identity.Slot;
    transformChanged = (meta.Flags & SMILE_INSTANCE_FLAG_TRANSFORM_CHANGED) != 0u;
    return true;
}

bool TemporalCanonicalCurrentToPrevious(
    uint instanceIndex,
    StructuredBuffer<FGpuIdentityEntry> currentIdentity,
    StructuredBuffer<GpuInstanceMeta> instanceMeta,
    StructuredBuffer<GpuInstanceTransform> currentTransforms,
    StructuredBuffer<GpuInstanceTransform> previousTransforms,
    uint currentCount,
    uint instanceTableCapacity,
    out row_major float4x4 result) {
    uint slot;
    bool changed;
    if (!TemporalResolveCanonicalSlot(instanceIndex, currentIdentity, instanceMeta,
                                      currentCount, instanceTableCapacity, slot, changed)) {
        result = TemporalIdentity4x4();
        return false;
    }
    if (!changed) {
        result = TemporalIdentity4x4();
        return true;
    }
    result = TemporalCurrentToPrevious(currentTransforms[slot].Value,
                                       previousTransforms[slot].Value);
    return true;
}

bool TemporalCanonicalPreviousToCurrent(
    uint instanceIndex,
    StructuredBuffer<FGpuIdentityEntry> currentIdentity,
    StructuredBuffer<GpuInstanceMeta> instanceMeta,
    StructuredBuffer<GpuInstanceTransform> currentTransforms,
    StructuredBuffer<GpuInstanceTransform> previousTransforms,
    uint currentCount,
    uint instanceTableCapacity,
    out row_major float4x4 result) {
    uint slot;
    bool changed;
    if (!TemporalResolveCanonicalSlot(instanceIndex, currentIdentity, instanceMeta,
                                      currentCount, instanceTableCapacity, slot, changed)) {
        result = TemporalIdentity4x4();
        return false;
    }
    if (!changed) {
        result = TemporalIdentity4x4();
        return true;
    }
    result = TemporalPreviousToCurrent(currentTransforms[slot].Value,
                                       previousTransforms[slot].Value);
    return true;
}

// Traduz um id gravado no frame ANTERIOR para o indice do MESMO objeto no frame ATUAL.
//
// Sem isto, um id antigo era reinterpretado pela lista de agora: com [A,B,C], remover A faz o
// id 1 (que era B) passar a designar C, e o resultado e o transform do objeto errado -- sem
// erro, so movimento errado.
//
// A cadeia inteira: id anterior -> tabela ANTERIOR -> {slot,generation} -> remap -> indice
// atual -> confirmacao contra a tabela ATUAL. O ultimo passo e o que separa "o slot existe" de
// "e o mesmo objeto": um slot reciclado devolve indice valido com OUTRO ocupante, e so a
// generation reprova isso.
bool TemporalResolvePreviousIndex(uint previousId,
                                  StructuredBuffer<FGpuIdentityEntry> previousTable,
                                  StructuredBuffer<uint>              slotToCurrentIndex,
                                  StructuredBuffer<FGpuIdentityEntry> currentTable,
                                  uint slotCapacity,
                                  uint currentCount,
                                  out uint currentIndex) {
    currentIndex = 0u;

    // Generation 0 nunca e alocada (ver GpuSceneHandle.cpp), entao entrada zerada = vazia. E o
    // que a tabela guarda para as posicoes alem da contagem viva.
    // A checagem de generation abaixo e REDUNDANTE com a comparacao de identidade la no fim:
    // uma entrada {slot, 0} cai no ocupante legitimo daquele slot, cuja generation difere, e a
    // identidade ja reprova. Ela fica por ser early-out barato (evita duas leituras) e por dizer
    // a intencao. Medido: removê-la sozinha nao muda nenhum caso do teste em WARP -- entao nao
    // procure um caso que ela salve, nao existe.
    const FGpuIdentityEntry was = previousTable[previousId];
    if (was.Generation == 0u || was.Slot >= slotCapacity) return false;

    const uint candidate = slotToCurrentIndex[was.Slot];
    if (candidate >= currentCount) return false;

    const FGpuIdentityEntry now = currentTable[candidate];
    if (now.Slot != was.Slot || now.Generation != was.Generation) return false;

    currentIndex = candidate;
    return true;
}

// Reprojecao de receptores: duas superficies de frames diferentes so casam depois do remap.
// Compartilhado pelos leitores reais e pela sonda WARP.
bool TemporalMatchPreviousSurface(float encoded, uint previousCount, uint receiverIndex,
                                   StructuredBuffer<FGpuIdentityEntry> previousTable,
                                   StructuredBuffer<uint> slotToCurrentIndex,
                                   StructuredBuffer<FGpuIdentityEntry> currentTable,
                                   uint slotCapacity, uint currentCount, out uint currentIndex) {
    currentIndex = 0u;
    uint previousId;
    if (!TemporalDecodeInstance(encoded, previousCount, previousId)) return false;
    return TemporalResolvePreviousIndex(previousId, previousTable, slotToCurrentIndex,
                                        currentTable, slotCapacity, currentCount, currentIndex) &&
           currentIndex == receiverIndex;
}

float3 TemporalTransformPoint(float3 p, row_major float4x4 m) {
    const float4 h = mul(float4(p, 1.0f), m);
    return h.xyz / max(abs(h.w), 1.0e-6f);
}

float3 TemporalTransformDirection(float3 v, row_major float4x4 m) {
    return normalize(mul(float4(v, 0.0f), m).xyz);
}

bool TemporalProjectUv(float3 p, row_major float4x4 vp, out float2 uv) {
    const float4 clip = mul(float4(p, 1.0f), vp);
    if (clip.w <= 1.0e-5f) { uv = 0.0f; return false; }
    const float2 ndc = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    return all(uv > 0.0f) && all(uv < 1.0f);
}

#endif
