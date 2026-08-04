#ifndef SMILE_SHADER_TIMER_HLSLI
#define SMILE_SHADER_TIMER_HLSLI

// Instrumentacao de timer por pixel nos passes de ray tracing. Tecnica do artigo da NVIDIA
// "Profiling DXR shaders with timer instrumentation": le o timer global da GPU antes e depois
// do trecho caro e grava o DELTA num alvo, que o visualizador mostra como heatmap. O que um
// query de timestamp nao da e exatamente isto — a distribuicao ESPACIAL do custo, que e o que
// aponta o pixel/geometria culpado (raios rasantes, BLAS ruim, alpha-test em folhagem).
//
// O que a medida e e o que NAO e:
//   - E ciclos do timer global da GPU gastos POR THREAD entre os dois pontos, incluindo stall de
//     memoria e espera de escalonamento. Serve p/ comparar pixel com pixel no MESMO frame.
//   - NAO e milissegundo, nao soma p/ dar o tempo do passe (as threads rodam em paralelo) e nao
//     e comparavel entre GPUs diferentes. Quem da o tempo do passe continua sendo o FGpuProfiler.
//
// So existe na PERMUTACAO instrumentada (-D SMILE_SHADER_TIMER=1). O passe normal nao paga
// registrador nem instrucao nenhuma por isso — e o motivo de ser permutacao e nao um if no CB.

#ifndef SMILE_SHADER_TIMER
#define SMILE_SHADER_TIMER 0
#endif

#if SMILE_SHADER_TIMER

// Slot falso da extensao HLSL da NVAPI: u999 em space0, mesma convencao do RTXDI. E um
// registrador que passe nenhum usa; o driver reconhece o padrao de escrita nesse UAV e troca
// pela instrucao real. PRECISA casar com FShaderTimer::kExtnSlot/kExtnSpace no C++ — quem
// concilia os dois lados e o NvAPI_D3D12_SetNvShaderExtnSlotSpace, chamado antes de criar a PSO.
#define NV_SHADER_EXTN_SLOT            u999
#define NV_SHADER_EXTN_REGISTER_SPACE  space0
#include "nvHLSLExtns.h"

// So os 32 bits baixos do timer. Juntar com o HI custaria registrador a mais p/ medir intervalo
// que nao chega perto de estourar 32 bits — o artigo faz a mesma escolha. A volta do contador e
// coberta pela aritmetica sem sinal: (fim - inicio) em uint da o delta certo mesmo com um wrap.
uint SmileTimerNow() { return NvGetSpecial(NV_SPECIALOP_GLOBAL_TIMER_LO); }

// Saida BINDLESS: as root sigs dos passes de trace ja sao HEAP_DIRECTLY_INDEXED, entao o alvo
// entra por indice no heap (um campo float do CB) e NENHUMA tabela de descritores muda por causa
// do debug. Sem isso, cada passe instrumentado precisaria de uma tabela UAV gemea.
// SlotF negativo = captura desligada (o CB e float; -1 e o sentinela, ver FShaderTimer).
void SmileTimerWrite(uint2 Px, float SlotF, uint Cycles) {
    if (SlotF < 0.0f) return;
    RWTexture2D<float> Dst = ResourceDescriptorHeap[(uint)SlotF];
    Dst[Px] = (float)Cycles;
}

#define SMILE_TIMER_BEGIN(Var)         uint Var = SmileTimerNow();
#define SMILE_TIMER_END(Var, Px, SlotF) SmileTimerWrite(Px, SlotF, SmileTimerNow() - Var);

#else

#define SMILE_TIMER_BEGIN(Var)
#define SMILE_TIMER_END(Var, Px, SlotF)

#endif // SMILE_SHADER_TIMER

#endif // SMILE_SHADER_TIMER_HLSLI
