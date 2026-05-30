#ifndef PARALLAX_OCCLUSION_MAPPING_HLSL
#define PARALLAX_OCCLUSION_MAPPING_HLSL

// -----------------------------------------------------------------------------------------
// SmileEngine Parallax Occlusion Mapping (POM)
// Features: Ray Marching, Secant Refinement, Dithering, Silhouette Clipping, Self-Shadowing, PDO
// -----------------------------------------------------------------------------------------

// Helper for Dithering (Bayer Matrix 4x4)
// Returns a value between 0.0 and 1.0 based on screen pixel coordinates
float GetBayerDither(float2 ScreenPos) {
    int2 pos = int2(ScreenPos) % 4;
    const float dither[16] = {
        0.0f/16.0f,  8.0f/16.0f,  2.0f/16.0f, 10.0f/16.0f,
        12.0f/16.0f, 4.0f/16.0f, 14.0f/16.0f,  6.0f/16.0f,
        3.0f/16.0f, 11.0f/16.0f,  1.0f/16.0f,  9.0f/16.0f,
        15.0f/16.0f, 7.0f/16.0f, 13.0f/16.0f,  5.0f/16.0f
    };
    return dither[pos.y * 4 + pos.x];
}

// Struct to hold the result of the POM function
struct POMResult {
    float2 UV;
    float PixelDepthOffset;
    float ShadowFactor;
    bool bClipped;
};

// -----------------------------------------------------------------------------------------
// Main POM Function
// HeightMap: The heightmap texture (typically R channel)
// HeightSampler: Sampler for the heightmap
// UV: Original texture coordinates
// ViewDirTS: View direction vector in Tangent Space (normalized)
// LightDirTS: Light direction vector in Tangent Space (normalized) for self-shadowing
// MinSteps: Minimum number of ray march steps (e.g., 8)
// MaxSteps: Maximum number of ray march steps (e.g., 32)
// HeightScale: The scale of the displacement (e.g., 0.05)
// ScreenPos: SV_Position.xy for dithering
// EnableShadows: Whether to calculate self-shadowing
// EnableSilhouette: Whether to discard pixels outside the UV bounds (true 3D silhouette)
// -----------------------------------------------------------------------------------------
POMResult ParallaxOcclusionMapping(
    Texture2D HeightMap,
    SamplerState HeightSampler,
    float2 UV,
    float3 ViewDirTS,
    float3 LightDirTS,
    float MinSteps,
    float MaxSteps,
    float HeightScale,
    float2 ScreenPos,
    bool EnableShadows,
    bool EnableSilhouette)
{
    POMResult Result;
    Result.UV = UV;
    Result.PixelDepthOffset = 0.0f;
    Result.ShadowFactor = 1.0f;
    Result.bClipped = false;

    // View direction vector in texture space
    float3 vDir = normalize(ViewDirTS);
    vDir.xy /= (abs(vDir.z) + 0.01f); // Avoid divide by zero and negative Z
    vDir.xy *= HeightScale;

    // Determine number of steps based on viewing angle
    // Looking straight down = MinSteps, looking at glancing angle = MaxSteps
    float NumSteps = lerp(MaxSteps, MinSteps, saturate(ViewDirTS.z));
    float StepSize = 1.0f / NumSteps;

    // Calculate ddx and ddy for proper mipmap selection during the loop
    float2 dx = ddx(UV);
    float2 dy = ddy(UV);

    // Initial state
    float2 CurrentUV = UV;
    float CurrentRayZ = 1.0f; // Start at the top (1.0 = top of displacement, 0.0 = base)
    
    // Apply Dithering to the initial step to hide banding artifacts
    float DitherOffset = GetBayerDither(ScreenPos);
    CurrentRayZ -= StepSize * DitherOffset;
    CurrentUV -= vDir.xy * (StepSize * DitherOffset);

    float2 StepUV = vDir.xy * StepSize;
    
    // First sample
    float CurrentHeight = HeightMap.SampleGrad(HeightSampler, CurrentUV, dx, dy).r;

    // Ray Marching Loop
    int stepCount = 0;
    while (CurrentHeight < CurrentRayZ && stepCount < (int)MaxSteps)
    {
        CurrentRayZ -= StepSize;
        CurrentUV -= StepUV;
        CurrentHeight = HeightMap.SampleGrad(HeightSampler, CurrentUV, dx, dy).r;
        stepCount++;
    }

    // Silhouette Clipping
    if (EnableSilhouette)
    {
        if (CurrentUV.x < 0.0f || CurrentUV.x > 1.0f || CurrentUV.y < 0.0f || CurrentUV.y > 1.0f)
        {
            Result.bClipped = true;
            return Result; // We will call clip(-1) outside
        }
    }

    // Secant Method Refinement
    // Find the exact intersection point by interpolating between the last two steps
    float2 PrevUV = CurrentUV + StepUV;
    float PrevRayZ = CurrentRayZ + StepSize;
    float PrevHeight = HeightMap.SampleGrad(HeightSampler, PrevUV, dx, dy).r;

    float d1 = CurrentHeight - CurrentRayZ;
    float d2 = PrevRayZ - PrevHeight;
    float Weight = d1 / (d1 + d2 + 1e-5f);

    float2 FinalUV = lerp(CurrentUV, PrevUV, Weight);
    float FinalDepth = lerp(CurrentRayZ, PrevRayZ, Weight);

    Result.UV = FinalUV;

    // Calculate Pixel Depth Offset (PDO)
    // Map the [0..1] depth into world-space depth offset for SV_Depth
    Result.PixelDepthOffset = (1.0f - FinalDepth) * HeightScale * ViewDirTS.z;

    // Soft Self-Shadowing
    if (EnableShadows && LightDirTS.z > 0.0f)
    {
        float3 lDir = normalize(LightDirTS);
        lDir.xy /= (abs(lDir.z) + 0.01f);
        lDir.xy *= HeightScale;
        
        float ShadowStepSize = StepSize * 2.0f; // Can use larger steps for shadows
        float2 ShadowStepUV = lDir.xy * ShadowStepSize;
        
        float2 ShadowUV = FinalUV + ShadowStepUV;
        float ShadowRayZ = FinalDepth + ShadowStepSize;
        float ShadowMultiplier = 1.0f;
        
        int shadowStepCount = 0;
        // March towards the light
        while (ShadowRayZ < 1.0f && ShadowUV.x >= 0.0f && ShadowUV.x <= 1.0f && ShadowUV.y >= 0.0f && ShadowUV.y <= 1.0f && shadowStepCount < (int)(MaxSteps * 0.5f))
        {
            float ShadowHeight = HeightMap.SampleGrad(HeightSampler, ShadowUV, dx, dy).r;
            if (ShadowHeight > ShadowRayZ)
            {
                // Simple hard shadow
                ShadowMultiplier = 0.2f;
                break;
            }
            ShadowRayZ += ShadowStepSize;
            ShadowUV += ShadowStepUV;
            shadowStepCount++;
        }
        Result.ShadowFactor = ShadowMultiplier;
    }

    return Result;
}

#endif // PARALLAX_OCCLUSION_MAPPING_HLSL
