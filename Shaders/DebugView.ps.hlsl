// Visualizador generico de render targets. Substitui o GBufferDebug.ps: os 8 modos antigos
// viram DECODE_GBUFFER_FIELD + SubIndex, reusando o mesmo DecodeGBuffer() do GBuffer.hlsli
// (a logica de desempacotamento nao e duplicada — continua morando la).
//
// Canal e escala NAO tem modo proprio: sao resolvidos por ChannelWeight, no estilo do
// r_ShowRenderTarget da Cry. "So o alfa" e ChannelWeight=(0,0,0,1); "x2" e (2,2,2,2).
#include "GBuffer.hlsli"

#define DECODE_RAW            0u
#define DECODE_GRAYSCALE      1u
#define DECODE_HDR            2u
#define DECODE_GBUFFER_FIELD  3u
#define DECODE_OCT_NORMAL     4u
#define DECODE_REVERSE_Z      5u
#define DECODE_VELOCITY       6u

cbuffer DebugViewCB : register(b0) {
    uint   Decode;
    uint   SubIndex;      // campo do G-buffer quando DECODE_GBUFFER_FIELD
    uint   Mip;
    uint   AtlasTilePx;   // > 0: alvo e atlas de tiles NxN dessa largura -> reempacota em grade
    float4 ChannelWeight;
    float  Exposure;      // DECODE_HDR
    float  NearZ;         // DECODE_REVERSE_Z
    float  FarZ;
    float  TileAspect;    // largura/altura do RETANGULO onde o tile esta sendo desenhado
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

// Reverse-Z: perto = 1, longe = 0, e a distribuicao concentra quase todo o [0,1] no que esta
// perto (num cenario de 0.1..4000, o fundo cai em d ~= 1e-5). Qualquer gama pura sobre d e
// compromisso ruim: ou lava tudo de branco (gama pequena) ou afunda a cena inteira no preto
// (gama grande). O jeito certo e LINEARIZAR de volta p/ distancia de view e usar rampa
// logaritmica, que e o que distribui bem uma faixa de 4 ordens de grandeza.
float3 VisualizeReverseZ(float d, float nearZ, float farZ) {
    // Inversa da projecao reverse-Z: d=1 -> nearZ, d=0 -> farZ.
    float linearZ = (nearZ * farZ) / max(nearZ + d * (farZ - nearZ), 1e-6f);
    float t = log2(max(linearZ / nearZ, 1.0f)) / log2(max(farZ / nearZ, 2.0f));
    return saturate(1.0f - t).xxx;   // perto = claro, longe = escuro
}

float4 main(VSOutput input) : SV_Target {
    // UV -> texel do PROPRIO alvo (cada tile pode ter resolucao diferente da tela).
    uint tw, th, tmips;
    Target.GetDimensions(0, tw, th, tmips);
    uint mip = min(Mip, tmips > 0 ? tmips - 1u : 0u);

    // Preserva o aspecto do alvo (letterbox). Sem isto, alvo que nao e screen-space — os
    // atlas do DDGI sao grades de tiles de probe, nao imagem de tela — sai esticado a ponto
    // de virar listra ilegivel. Alvo screen-space tem aspecto ~igual ao do tile, entao
    // scale ~= 1 e nada muda: da p/ aplicar sempre, sem flag.
    float2 uv = input.uv;
    if (AtlasTilePx > 0u) {
        // Atlas de probe (DDGI): a largura e CountX*CountZ*tile e a altura so CountY*tile —
        // num grid comum isso da algo como 8192x64, proporcao ~128:1. Esticar vira listra e
        // o letterbox vira um fio de 1 pixel. O util e REEMPACOTAR: os tiles viram uma grade
        // aproximadamente quadrada, cada probe legivel.
        uint tilesX = max(tw / AtlasTilePx, 1u);
        uint tilesY = max(th / AtlasTilePx, 1u);
        uint total  = tilesX * tilesY;
        uint cols   = max((uint)ceil(sqrt((float)total * max(TileAspect, 1e-4f))), 1u);
        uint rows   = (total + cols - 1u) / cols;

        uint cx = min((uint)(uv.x * cols), cols - 1u);
        uint cy = min((uint)(uv.y * rows), rows - 1u);
        uint idx = cy * cols + cx;
        if (idx >= total) return float4(0.0f, 0.0f, 0.0f, 1.0f);

        float2 inCell = frac(float2(uv.x * cols, uv.y * rows));
        float2 srcTile = float2(idx % tilesX, idx / tilesX);
        uv = (srcTile + inCell) * float(AtlasTilePx) / float2(max(tw,1u), max(th,1u));
    } else {
        // Preserva o aspecto do alvo (letterbox). Alvo screen-space tem aspecto ~igual ao do
        // tile, entao scale ~= 1 e nada muda: da p/ aplicar sempre, sem flag.
        float scale = (float(tw) / max(float(th), 1.0f)) / max(TileAspect, 1e-4f);
        if (scale > 1.0f) uv.y = (uv.y - 0.5f) * scale + 0.5f;
        else              uv.x = (uv.x - 0.5f) / scale + 0.5f;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    int2 tpx = int2(uv * float2(max(tw >> mip, 1u), max(th >> mip, 1u)));

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

    if (Decode == DECODE_GRAYSCALE) {
        // Alvo de 1 canal (GTAO, mascaras): sem replicar, r vira vermelho puro. Exposure aqui
        // e FATOR DE ESCALA, nao tonemap: o atlas de distancia do DDGI guarda distancia em
        // unidades de mundo, entao qualquer valor > 1 satura em branco sem normalizar por
        // MaxRayDistance.
        outColor = saturate(s.r * ChannelWeight.r * Exposure).xxx;
        return float4(outColor, 1.0f);
    } else if (Decode == DECODE_OCT_NORMAL) {
        outColor = GBuffer_OctDecode(s.rg) * 0.5f + 0.5f;
    } else if (Decode == DECODE_REVERSE_Z) {
        outColor = VisualizeReverseZ(s.r, NearZ, FarZ);
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
