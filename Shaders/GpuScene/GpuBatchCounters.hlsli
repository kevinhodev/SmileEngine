#ifndef SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI
#define SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI

// O que o PREFIXO publica e o SCATTER consome (Fase 5).
//
// Moraram dentro do PrefixBatchCounts.cs.hlsl ate o scatter precisar ler `PrefixOverflow`. Copiar
// o indice para o scatter seria um terceiro espelho — e um indice divergente ali faria o scatter
// ler o TOTAL no lugar da flag, e escrever por cima dos comandos de outro lote exatamente no frame
// em que devia se abster.

// Indices dos contadores, espelhando EGpuPrefixCounter.
#define SMILE_PREFIX_TOTAL    0u
#define SMILE_PREFIX_OVERFLOW 1u
#define SMILE_PREFIX_COUNT    2u

// Offset gravado em toda entrada que o prefixo nao conseguiu calcular. Espelha
// kGpuBatchOffsetPoison. Nao e zero: zero e o offset legitimo do primeiro lote.
#define SMILE_BATCH_OFFSET_POISON 0xFFFFFFFFu

#endif
