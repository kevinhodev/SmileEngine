#include "OceanFFTCommon.hlsli"

Texture2D<float4>   H0Tex : register(t0);
RWTexture2D<float2>  SpecH : register(u0);
RWTexture2D<float2>  SpecD : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DISP_MAP_SIZE || id.y >= DISP_MAP_SIZE) return;

    int2 loc1 = int2(id.xy);
    int2 loc2 = (-loc1) & (int(DISP_MAP_SIZE) - 1);

    float4 hk    = H0Tex.Load(int3(loc1, 0)); 
    float2 h0_k  = hk.xy;
    float  w_k   = hk.z;
    float2 h0_mk = H0Tex.Load(int3(loc2, 0)).xy;

    float cos_wt = cos(w_k * Time);
    float sin_wt = sin(w_k * Time);

    float2 h_tk;
    h_tk.x = cos_wt * (h0_k.x + h0_mk.x) - sin_wt * (h0_k.y + h0_mk.y);
    h_tk.y = cos_wt * (h0_k.y - h0_mk.y) + sin_wt * (h0_k.x - h0_mk.x);

    float2 k  = float2(float(int(DISP_MAP_SIZE) / 2) - float(loc1.x),
                       float(int(DISP_MAP_SIZE) / 2) - float(loc1.y));
    float  kn2 = dot(k, k);
    float2 nk  = (kn2 > 1e-12f) ? normalize(k) : float2(0.0f, 0.0f);

    float2 Dt_x  = float2(h_tk.y * nk.x, -h_tk.x * nk.x);
    float2 iDt_z = float2(h_tk.x * nk.y,  h_tk.y * nk.y);

    SpecH[loc1] = h_tk;
    SpecD[loc1] = Dt_x + iDt_z;
}
