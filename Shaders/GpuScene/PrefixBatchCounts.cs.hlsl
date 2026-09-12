// Prefixo exclusivo das contagens por lote (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// `BatchOffsets[b]` = onde os comandos do lote b comecam em `IndirectArgs`. Consome o `BatchCounts`
// que o ClassifyCandidates acabou de escrever.
//
// Na GPU porque a entrada e da GPU: soma-la na CPU exigiria readback no caminho do frame, que a
// regra 7 proibe. Nao e a situacao do `BuildRangeOutputBases` da Fase 4, que prefixa dados da CPU.
//
// UMA THREAD, em serie, e isso e deliberado. O array tem `kGpuPipelineClassCount * PoolCount`
// entradas — quatro vezes o numero de chunks de geometria, ou seja, unidades a poucas centenas. Um
// scan paralelo em groupshared aqui seria cerimonia: mais codigo, mais para errar, e o custo real
// e o do lancamento do dispatch, nao o dos oito ADDs. Se um dia o numero de lotes crescer uma ordem
// de grandeza (multi-submesh, virtual geometry), este e o lugar de trocar por um scan em blocos —
// e o teste WARP que compara com a referencia CPU continua servindo.
//
// ⚠️ NAO e in-place. `BatchCounts` e tambem o COUNT BUFFER do `ExecuteIndirect` (ele le um uint32 em
// `CountBufferOffset`, entao o lote b usa `b * 4`); acumular por cima destruiria as contagens de que
// os draws dependem.

// Indices dos contadores, espelhando EGpuPrefixCounter.
#define SMILE_PREFIX_TOTAL    0u
#define SMILE_PREFIX_OVERFLOW 1u
#define SMILE_PREFIX_COUNT    2u

// Offset gravado em toda entrada que o prefixo nao conseguiu calcular. Espelha
// kGpuBatchOffsetPoison. Nao e zero: zero e o offset legitimo do primeiro lote, e um consumidor que
// ignorasse a flag escreveria os comandos de um lote por cima dos do lote 0.
#define SMILE_BATCH_OFFSET_POISON 0xFFFFFFFFu

cbuffer PrefixCB : register(b0) {
    uint4 PrefixParams; // x = slots de BatchCounts; yzw reservados
};

StructuredBuffer<uint> BatchCounts : register(t0);

RWStructuredBuffer<uint> BatchOffsets : register(u0);
RWStructuredBuffer<uint> Counters     : register(u1);
// O count buffer do ExecuteIndirect. Ver a nota do DrawCounts no GpuClassify.h: um `return` no
// scatter nao suprime draw nenhum, porque o command processor le ESTE buffer e nao o shader.
RWStructuredBuffer<uint> DrawCounts   : register(u2);

[numthreads(1, 1, 1)]
void main() {
    const uint Slots = PrefixParams.x;

    uint Acc = 0u;
    uint Estourou = 0u;
    for (uint I = 0u; I < Slots; ++I) {
        // A checagem vem ANTES da escrita: assim a entrada que falhou recebe o poison junto das
        // seguintes, e nao um inicio valido para um lote que nao cabe inteiro.
        //
        // Escrita como subtracao no lado seguro. Um `BatchCounts` vindo do classify nao consegue
        // estourar — cada candidato contribui no maximo 1, e candidatos sao contados em u32 —, mas
        // este shader nao sabe quem escreveu o buffer que esta somando, e um prefixo que da a volta
        // aponta comandos para dentro dos comandos de outro lote.
        if (Acc > 0xFFFFFFFFu - BatchCounts[I]) {
            Estourou = 1u;
            // Poison em TODAS as entradas restantes, e nao so a interrupcao das escritas. Parar de
            // escrever deixaria o conteudo ANTERIOR do recurso — no frame seguinte, os offsets do
            // frame passado, que sao plausiveis e errados. O valor tem de ser o mesmo da referencia
            // CPU para que o teste possa comparar elemento a elemento tambem neste caso.
            for (uint J = I; J < Slots; ++J) BatchOffsets[J] = SMILE_BATCH_OFFSET_POISON;
            break;
        }
        BatchOffsets[I] = Acc;
        Acc += BatchCounts[I];
    }


    // O COUNT BUFFER, sempre percorrendo TODOS os slots — inclusive depois de um `break`.
    //
    // Zerado por inteiro quando houve estouro: e assim, e nao com um early-out no scatter, que o
    // draw e suprimido. O `ExecuteIndirect` le este buffer direto; com contagem nao zero ele
    // executaria comandos velhos que o scatter deixou intactos.
    //
    // Laco proprio, e nao uma escrita dentro do laco do prefixo, porque `Estourou` so e conhecido
    // quando aquele termina: uma entrada escrita antes do estouro ja teria publicado contagem.
    for (uint K = 0u; K < Slots; ++K) DrawCounts[K] = Estourou ? 0u : BatchCounts[K];
    Counters[SMILE_PREFIX_TOTAL]    = Acc;
    Counters[SMILE_PREFIX_OVERFLOW] = Estourou;
}
