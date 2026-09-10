#pragma once

// O par temporal por instancia, e de onde ele passa a vir (Fase 4 do GPU-DRIVEN-PLAN).
//
// O `FTemporalMotionVectors` mantem hoje uma tabela propria de 128 B por instancia, em ping-pong de
// dois frames — 256 B por instancia residentes, ~256 MB em 1M —, reescrita INTEIRA por frame na
// CPU, com DUAS inversas 4x4 por entrada.
//
// Ela nao carrega dado novo: e uma VISAO DERIVADA de `Transform` e `PrevTransform`, que ja moram na
// tabela canonica de instancias — e as duas matrizes de entrada saem exatamente de la (ver
// `FGpuSceneInstanceTable::TransformsFor`). Derivar na LEITURA troca 256 B residentes por instancia
// e uma varredura da cena por frame por ~200 ALU sem trafego, no momento em que o consumidor
// precisa.
//
// Este header e o contrato dessa troca, separado do passe pelo mesmo motivo pratico do
// GpuInstanceTransform.h: o teste de paridade que compara a derivacao do HLSL com a do C++ nao pode
// arrastar o renderer inteiro para dentro do executavel de teste.

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

namespace Smile {
    // Dados compartilhados pelos vetores temporais especificos do cap. 25 de Ray Tracing Gems II.
    // Espelhado em HLSL como `FTemporalInstanceTransform`
    // (Shaders/Temporal/TemporalMotionCommon.hlsli).
    struct FTemporalInstanceTransformGPU {
        Mat44 CurrentToPrevious;
        Mat44 PreviousToCurrent;
    };
    static_assert(sizeof(FTemporalInstanceTransformGPU) == 128);
    static_assert(offsetof(FTemporalInstanceTransformGPU, CurrentToPrevious) ==  0);
    static_assert(offsetof(FTemporalInstanceTransformGPU, PreviousToCurrent) == 64);

    // A composicao do par, num lugar so.
    //
    // O erro classico e a ORDEM DO PRODUTO: `Current.Inverse() * Previous` e `Previous *
    // Current.Inverse()` compilam os dois e reprojetam para lugares diferentes — e a diferenca so
    // aparece EM MOVIMENTO, onde o suspeito natural seria o denoiser, que nao tem nada a ver com
    // isso. O teste de paridade em WARP compara ESTA funcao com a do shader, inclusive no ramo
    // singular (escala zero: `Mat44::Inverse` devolve identidade em vez de NaN).
    inline FTemporalInstanceTransformGPU ComposeTemporalInstanceTransform(const Mat44& Current,
                                                                          const Mat44& Previous) {
        FTemporalInstanceTransformGPU Out;
        Out.CurrentToPrevious = Current.Inverse() * Previous;
        Out.PreviousToCurrent = Previous.Inverse() * Current;
        return Out;
    }
}
