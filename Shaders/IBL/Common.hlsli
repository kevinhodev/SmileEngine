// Common helpers for IBL precomputation shaders.

static const float PI       = 3.14159265359f;
static const float TwoPI    = 6.28318530718f;
static const float InvPI    = 0.31830988618f;

// Converts a texel coordinate (u,v) in [0,1] on cube face `face` to a world-
// space direction. Face order matches D3D11/12 cube layout: +X,-X,+Y,-Y,+Z,-Z.
float3 CubeFaceToDirection(uint face, float2 uv) {
    float2 t = uv * 2.0f - 1.0f;
    // Flip Y so the texture's V=0 is the cubemap's top edge.
    t.y = -t.y;
    float3 dir;
    switch (face) {
        case 0: dir = float3( 1.0f, t.y, -t.x); break; // +X
        case 1: dir = float3(-1.0f, t.y,  t.x); break; // -X
        case 2: dir = float3( t.x, 1.0f, -t.y); break; // +Y (top — t.y already inverted)
        case 3: dir = float3( t.x,-1.0f,  t.y); break; // -Y
        case 4: dir = float3( t.x, t.y,  1.0f); break; // +Z
        default: dir = float3(-t.x, t.y, -1.0f); break; // -Z
    }
    return normalize(dir);
}

// Hammersley low-discrepancy 2D sequence.
float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}
float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

// GGX importance sample: returns a half-vector H in world space given a normal.
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;
    float phi      = TwoPI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // Tangent basis from N.
    float3 up        = abs(N.z) < 0.999f ? float3(0,0,1) : float3(1,0,0);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// GGX NDF, used for solid-angle correction in specular prefilter.
float D_GGX(float NoH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NoH * NoH) * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d);
}

// Geometry-Schlick GGX for the BRDF LUT (IBL form: k = a^2 / 2).
float G_SchlickGGX_IBL(float NoV, float roughness) {
    float a = roughness;
    float k = (a * a) * 0.5f;
    return NoV / (NoV * (1.0f - k) + k);
}
float G_Smith_IBL(float NoV, float NoL, float roughness) {
    return G_SchlickGGX_IBL(NoV, roughness) * G_SchlickGGX_IBL(NoL, roughness);
}
