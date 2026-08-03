// Linha Foreground: sem teste de depth, sempre visivel (gizmo, markers).
#define SMILE_DD_DEPTH_TEST 0
#include "DebugDrawLine.hlsli"

float4 main(LineVSOutput In) : SV_TARGET { return SmileDebugLinePS(In); }
