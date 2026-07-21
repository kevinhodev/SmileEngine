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

// Mesmo ACES fitted do FinalTonemap.ps (via MaterialPreview.ps) — ceu e mesh precisam
// da mesma curva.
static const float3x3 ACESInputMat = {
    0.59719f, 0.35458f, 0.04823f,
    0.07608f, 0.90834f, 0.01558f,
    0.02244f, 0.08207f, 0.89548f
};

static const float3x3 ACESOutputMat = {
    1.60475f, -0.53108f, -0.07367f,
    -0.10210f,  1.10813f, -0.00603f,
    -0.00327f, -0.07276f,  1.07602f
};

float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.432951f) + 0.238081f;
    return a / b;
}

float3 ACESFilm(float3 color) {
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
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
