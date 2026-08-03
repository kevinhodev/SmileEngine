// Piramide HDR da cena iluminada antes da agua. Cada nivel conserva a radiancia media
// (box 2x2); o trace SSR escolhe o nivel a partir do footprint do lobo GGX.
Texture2D<float4>   SrcMip : register(t0);
RWTexture2D<float4> DstMip : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint dstWidth, dstHeight;
    DstMip.GetDimensions(dstWidth, dstHeight);
    if (id.x >= dstWidth || id.y >= dstHeight) return;

    uint srcWidth, srcHeight;
    SrcMip.GetDimensions(srcWidth, srcHeight);
    const int2 base = int2(id.xy) * 2;
    float4 radiance = 0.0f;

    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            const int2 samplePos = min(base + int2(x, y),
                                       int2(int(srcWidth) - 1, int(srcHeight) - 1));
            radiance += SrcMip.Load(int3(samplePos, 0));
        }
    }

    DstMip[int2(id.xy)] = radiance * 0.25f;
}
