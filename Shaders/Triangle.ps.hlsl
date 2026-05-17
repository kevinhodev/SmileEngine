cbuffer TransformCB : register(b0) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
};

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

// Constantes
static const float PI = 3.14159265359f;

// D - GGX Normal Distribution Function
float D_GGX(float a2, float NoH) {
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / (PI * d * d);
}

// G - Smith Joint Masking-Shadowing Function (Approximation by Heitz)
float Vis_SmithJointApprox(float a2, float NoV, float NoL) {
    float a = sqrt(a2);
    float Vis_SmithV = NoL * (NoV * (1.0f - a) + a);
    float Vis_SmithL = NoV * (NoL * (1.0f - a) + a);
    return 0.5f / (Vis_SmithV + Vis_SmithL + 1e-5f);
}

// F - Schlick Fresnel
float3 F_Schlick(float3 SpecularColor, float VoH) {
    float Fc = pow(1.0f - VoH, 5.0f);
    // 50 * SpecularColor.g é o mesmo truque da Unreal para calcular o F90
    float f90 = saturate(50.0f * SpecularColor.g); 
    return saturate(f90) * Fc + (1.0f - Fc) * SpecularColor;
}

float4 main(PSInput input) : SV_TARGET {
    // Vetores normalizados
    float3 N = normalize(input.worldNormal);
    
    // Luz Point
    float3 lightPos = LightDirection.xyz;
    float3 lightVec = lightPos - input.worldPos;
    float dist = length(lightVec);
    float3 L = lightVec / dist;
    
    // Atenuação da luz (Inverse square law simplificada + limitador)
    float attenuation = 1.0f / (dist * dist + 1.0f);
    
    float3 V = normalize(CameraPosition.xyz - input.worldPos);
    float3 H = normalize(L + V);

    // Produtos escalares (saturados para evitar valores negativos)
    float NoL = saturate(dot(N, L));
    float NoV = saturate(dot(N, V));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    // Material do cubo: Plástico vermelho brilhante
    // (Metais puros ficam pretos sem um Environment Map/Skybox, então vamos usar não-metal por enquanto)
    float3 BaseColor = float3(1.0f, 0.2f, 0.2f); // Vermelho vibrante
    float Metallic = 0.0f; // 0.0 = Plástico/Dielétrico
    float Roughness = 0.4f; // 0.4 = Brilhoso o suficiente para ter um highlight amplo

    // Derivar os parâmetros PBR
    float3 DiffuseColor = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);

    // Alpha quadrado para GGX
    float a = Roughness * Roughness;
    float a2 = a * a;

    // --- Iluminação Direta (Cook-Torrance BRDF) ---
    float3 Lighting = float3(0, 0, 0);

    if (NoL > 0.0f) {
        // Specular BRDF
        float D = D_GGX(a2, NoH);
        float Vis = Vis_SmithJointApprox(a2, NoV, NoL);
        float3 F = F_Schlick(SpecularColor, VoH);

        float3 Specular = (D * Vis) * F;
        
        // Diffuse BRDF (Lambert simples com conservação de energia)
        float3 Kd = (1.0f - F) * (1.0f - Metallic);
        float3 Diffuse = (Kd * DiffuseColor) / PI;

        // Radiância da luz
        float3 LightRadiance = LightColor.rgb * LightColor.w * attenuation;

        Lighting = (Diffuse + Specular) * LightRadiance * NoL;
    }

    // Luz Ambiente básica simulando o céu (hemisférica)
    float3 upDir = float3(0.0f, 1.0f, 0.0f);
    float skyOcclusion = dot(N, upDir) * 0.5f + 0.5f;
    float3 ambientLight = float3(0.05f, 0.06f, 0.08f) * BaseColor * skyOcclusion;
    
    // Tonemapping simples (Reinhard) e correção Gamma
    float3 finalColor = Lighting + ambientLight;
    finalColor = finalColor / (finalColor + 1.0f); // Tonemap
    finalColor = pow(finalColor, 1.0f / 2.2f);     // Gamma correction
    
    return float4(finalColor, 1.0f);
}
