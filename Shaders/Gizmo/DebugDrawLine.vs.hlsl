// VS da linha grossa do DebugDraw (uma instancia por segmento). Corpo no .hlsli, compartilhado
// com as duas permutacoes de PS (com e sem teste de depth).
#include "DebugDrawLine.hlsli"

LineVSOutput main(LineVSInput In) { return SmileDebugLineVS(In); }
