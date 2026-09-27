#ifndef SMILE_OCTAHEDRAL_HLSLI
#define SMILE_OCTAHEDRAL_HLSLI
// Signed octahedral coordinates. Zero chooses +Z; the fold needs sign-not-zero.
float2 Smile_OctEncode(float3 n) {
    const float l1 = dot(abs(n), 1.0f);
    if (l1 <= 0.0f) return 0.0f;
    n /= l1;
    const float2 signs = float2(n.x >= 0.0f ? 1.0f : -1.0f,
                                n.y >= 0.0f ? 1.0f : -1.0f);
    return n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * signs;
}
#endif
