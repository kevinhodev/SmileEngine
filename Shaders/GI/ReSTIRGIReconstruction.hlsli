#ifndef SMILE_RESTIR_GI_RECONSTRUCTION_HLSLI
#define SMILE_RESTIR_GI_RECONSTRUCTION_HLSLI
// A moving surface can miss every 2x2 phase. Geometry-valid history still needs
// current lighting: after 16 frames at most (3/4)^16 = 1.01% of stale light remains.
float4 ReSTIRGI_UpdateReconstruction(float4 previous, float4 current) {
    return lerp(previous, current, 0.25f);
}
#endif
