// Linha que enxerga o depth da CENA. O quanto sobra da parte oculta vem do root constant
// OccludedAlpha: 0 = Scene (some atras da geometria), >0 = XRay (fica translucida).
#define SMILE_DD_DEPTH_TEST 1
#include "DebugDrawLine.hlsli"

float4 main(LineVSOutput In) : SV_TARGET { return SmileDebugLinePS(In); }
