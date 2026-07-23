// Visualizador generico de render targets. Substitui o GBufferDebug.ps: os 8 modos antigos
// viram DECODE_GBUFFER_FIELD + SubIndex, reusando o mesmo DecodeGBuffer() do GBuffer.hlsli
// (a logica de desempacotamento nao e duplicada — continua morando la).
//
// Canal e escala NAO tem modo proprio: sao resolvidos por ChannelWeight, no estilo do
// r_ShowRenderTarget da Cry. "So o alfa" e ChannelWeight=(0,0,0,1); "x2" e (2,2,2,2).
#include "GBuffer.hlsli"

#define DECODE_RAW            0u
#define DECODE_HDR            1u
#define DECODE_GBUFFER_FIELD  2u
#define DECODE_OCT_NORMAL     3u
#define DECODE_REVERSE_Z      4u
#define DECODE_VELOCITY       5u

cbuffer DebugViewCB : register(b0) {
    uint   Decode;
    uint   SubIndex;      // campo do G-buffer quando DECODE_GBUFFER_FIELD
    uint   Mip;
    uint   _pad0;
    float4 ChannelWeight;
    float  Exposure;      // DECODE_HDR
    float3 _pad1;
};

Texture2D Target    : register(t0);  // alvo generico do tile
Texture2D GBufferA  : register(t1);
Texture2D GBufferB  : register(t2);
Texture2D GBufferC  : register(t3);
Texture2D Velocity  : register(t4);

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Reinhard simples: so p/ trazer radiancia HDR pra faixa visivel sem estourar branco. Nao
// precisa casar com o tonemap final — aqui o objetivo e ENXERGAR o sinal, nao reproduzi-lo.
float3 TonemapForView(float3 c, float exposure) {
    c = max(c * exposure, 0.0f);
    return c / (1.0f + c);
}

// Reverse-Z: perto = 1, longe = 0, e a distribuicao e fortemente nao-linear. Mostrar cru da
// uma tela quase branca. Aqui so invertemos e aplicamos uma curva p/ espalhar o range util.
float3 VisualizeReverseZ(float d) {
    float v = saturate(1.0f - d);
    return pow(v, 0.35f).xxx;
}

float4 main(VSOutput input) : SV_Target {
    // UV -> texel do PROPRIO alvo (cada tile pode ter resolucao diferente da tela).
    uint tw, th, tmips;
    Target.GetDimensions(0, tw, th, tmips);
    uint mip = min(Mip, tmips > 0 ? tmips - 1u : 0u);
    int2 tpx = int2(input.uv * float2(max(tw >> mip, 1u), max(th >> mip, 1u)));

    float3 outColor;

    if (Decode == DECODE_GBUFFER_FIELD) {
        // O G-buffer e sempre full-res da cena: indexa pela posicao de tela, nao pelo alvo.
        int2 px = int2(input.pos.xy);
        GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                      GBufferB.Load(int3(px, 0)),
                                      GBufferC.Load(int3(px, 0)));
        switch (SubIndex) {
            case 1:  outColor = g.BaseColor;                  break;
            case 2:  outColor = g.WorldNormal * 0.5f + 0.5f;  break;
            case 3:  outColor = g.Roughness.xxx;              break;
            case 4:  outColor = g.Metallic.xxx;               break;
            case 5:  outColor = g.Subsurface;                 break;
            case 6:  outColor = g.AO.xxx;                     break;
            case 7:
                outColor = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE)
                         ? float3(0.1f, 0.9f, 0.2f) : float3(0.5f, 0.5f, 0.5f);
                break;
            default: outColor = g.BaseColor;                  break;
        }
        return float4(outColor * ChannelWeight.rgb, 1.0f);
    }

    if (Decode == DECODE_VELOCITY) {
        uint vw, vh, vmips;
        Velocity.GetDimensions(0, vw, vh, vmips);
        int2 vpx = int2(input.uv * float2(vw, vh));
        float2 velPx = Velocity.Load(int3(vpx, 0)).rg * float2(vw, vh);
        outColor = float3(0.5f, 0.5f, 0.5f) + float3(velPx * 0.05f, 0.0f);
        return float4(outColor * ChannelWeight.rgb, 1.0f);
    }

    float4 s = Target.Load(int3(tpx, mip));

    if (Decode == DECODE_OCT_NORMAL) {
        outColor = GBuffer_OctDecode(s.rg) * 0.5f + 0.5f;
    } else if (Decode == DECODE_REVERSE_Z) {
        outColor = VisualizeReverseZ(s.r);
    } else if (Decode == DECODE_HDR) {
        outColor = TonemapForView(s.rgb * ChannelWeight.rgb, Exposure);
        return float4(outColor, 1.0f);
    } else {
        // RAW: o peso soma os canais escolhidos. Isolar o alfa (0,0,0,1) mostra o alfa em
        // cinza; isolar rg mantem as cores. E o comportamento do r_ShowRenderTarget.
        float4 w = s * ChannelWeight;
        outColor = (ChannelWeight.a > 0.0f && ChannelWeight.r == 0.0f &&
                    ChannelWeight.g == 0.0f && ChannelWeight.b == 0.0f)
                 ? w.a.xxx          // so alfa -> escala de cinza
                 : w.rgb;
        return float4(outColor, 1.0f);
    }

    return float4(outColor * ChannelWeight.rgb, 1.0f);
}
