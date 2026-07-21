// Fundo do preview de material: amostra o env cube do HDRI proprio do preview na direcao
// da camera orbital (fullscreen triangle do PostProcess.vs). Mip levemente borrado pro
// material ser o protagonista; mesmo ACES+gamma do MaterialPreview.ps.

cbuffer SkyCB : register(b0) {
    float4 CamRight;   // xyz = right,   w = tan(fovX/2)
    float4 CamUp;      // xyz = up,      w = tan(fovY/2)
    float4 CamForward; // xyz = forward, w = rotacao do env (rad)
    float4 Params;     // x = mip, y = intensidade, zw = -
};

TextureCube EnvCube        : register(t0);
SamplerState LinearSampler : register(s0);

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 RotateY(float3 v, float a) {
    float c = cos(a), s = sin(a);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 ACESFilm(float3 x) {
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

float4 main(VSOutput input) : SV_Target {
    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - 2.0f * input.uv.y);
    float3 dir = normalize(CamForward.xyz
                         + ndc.x * CamRight.w * CamRight.xyz
                         + ndc.y * CamUp.w   * CamUp.xyz);
    dir = RotateY(dir, CamForward.w);

    float3 c = EnvCube.SampleLevel(LinearSampler, dir, Params.x).rgb * Params.y;
    c = ACESFilm(c);
    c = pow(c, 1.0f / 2.2f);
    return float4(c, 1.0f);
}
