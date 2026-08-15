// Visualizador generico de render targets. Substitui o GBufferDebug.ps: os 8 modos antigos
// viram DECODE_GBUFFER_FIELD + SubIndex, reusando o mesmo DecodeGBuffer() do GBuffer.hlsli
// (a logica de desempacotamento nao e duplicada — continua morando la).
//
// Canal e escala NAO tem modo proprio: sao resolvidos por ChannelWeight, no estilo do
// r_ShowRenderTarget da Cry. "So o alfa" e ChannelWeight=(0,0,0,1); "x2" e (2,2,2,2).
#include "GBuffer.hlsli"
#include "GI/DDGICommon.hlsli"

#define DECODE_RAW            0u
#define DECODE_GRAYSCALE      1u
#define DECODE_HDR            2u
#define DECODE_GBUFFER_FIELD  3u
#define DECODE_OCT_NORMAL     4u
#define DECODE_REVERSE_Z      5u
#define DECODE_VELOCITY       6u
#define DECODE_DDGI_IRRADIANCE 7u
#define DECODE_DDGI_DISTANCE   8u
#define DECODE_HEATMAP         9u
#define DECODE_ARRAY_SLICE    10u

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
    float2 ScreenUvOffset; // jitter atual em UV; aplicado so a targets screen-space
    uint   EncodeForDisplay;
    uint   LinearFilter;
};

Texture2D Target    : register(t0);  // alvo generico do tile
Texture2D GBufferA  : register(t1);
Texture2D GBufferB  : register(t2);
Texture2D GBufferC  : register(t3);
Texture2D Velocity  : register(t4);
// Mesmo descritor que t0, so que visto como array (ver DECODE_ARRAY_SLICE). O par de
// bindings existe porque a dimensao da view faz parte da DECLARACAO no HLSL: um SRV de
// Texture2DArray nao pode ser lido por uma declaracao Texture2D, e vice-versa. Cada tile
// acessa exatamente um dos dois, decidido pelo Decode.
Texture2DArray TargetArray : register(t5);
SamplerState LinearClamp : register(s0);
SamplerState PointClamp  : register(s1);

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

float3 HeatForView(float t) {
    t = saturate(t);
    float3 c = 0.0f;
    c += float3(0.05f, 0.15f, 1.00f) * saturate(1.0f - abs((t - 0.00f) * 4.0f));
    c += float3(0.00f, 0.90f, 1.00f) * saturate(1.0f - abs((t - 0.25f) * 4.0f));
    c += float3(0.10f, 1.00f, 0.20f) * saturate(1.0f - abs((t - 0.50f) * 4.0f));
    c += float3(1.00f, 0.90f, 0.05f) * saturate(1.0f - abs((t - 0.75f) * 4.0f));
    c += float3(1.00f, 0.08f, 0.02f) * saturate(1.0f - abs((t - 1.00f) * 4.0f));
    return c;
}

float4 DebugOutput(float3 color) {
    // O target offscreen e UNORM (nao sRGB) e chega ao QImage como bytes de display. Sem
    // codificar, qualquer RT linear fica artificialmente escuro. O viewport principal faz
    // a mesma curva no FinalTonemap.
    //
    // A excecao NAO e "ser atlas" (isso e layout, nao espaco de cor): e produzir valor que ja
    // esta em espaco de display. Hoje so a rampa de falsa-cor se encaixa — ela e autorada, e
    // encodar de novo lavaria as cores. A irradiancia do DDGI, apesar de tambem ser atlas, sai
    // LINEAR do pow+tonemap como qualquer outro alvo; com a regra antiga ela ficava
    // sistematicamente mais escura que os vizinhos na mesma grade.
    const bool AlreadyDisplayReferred = (Decode == DECODE_DDGI_DISTANCE ||
                                         Decode == DECODE_HEATMAP);
    if (EncodeForDisplay != 0u && !AlreadyDisplayReferred)
        color = pow(saturate(color), 1.0f / 2.2f);
    return float4(color, 1.0f);
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
    // Fatia de array tem caminho proprio, e ele vem ANTES de qualquer toque em `Target`:
    // naquele slot esta um SRV de array, que a declaracao Texture2D nao pode ler — nem para
    // um GetDimensions. Tudo o que o resto do shader faz com mip, atlas e jitter e
    // reproduzido aqui em miniatura porque nada disso se aplica: fatia de array nao e
    // screen-space (nao leva ScreenUvOffset) e nao e atlas de tiles.
    if (Decode == DECODE_ARRAY_SLICE) {
        uint aw, ah, aelems, amips;
        TargetArray.GetDimensions(0, aw, ah, aelems, amips);

        // Mesmo letterbox do caminho comum: preserva o aspecto do alvo dentro do tile.
        float2 auv = input.uv;
        float ascale = (float(aw) / max(float(ah), 1.0f)) / max(TileAspect, 1e-4f);
        if (ascale > 1.0f) auv.y = (auv.y - 0.5f) * ascale + 0.5f;
        else               auv.x = (auv.x - 0.5f) / ascale + 0.5f;
        if (auv.x < 0.0f || auv.x > 1.0f || auv.y < 0.0f || auv.y > 1.0f)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);

        uint amip = min(Mip, amips > 0 ? amips - 1u : 0u);
        uint mw   = max(aw >> amip, 1u);
        uint mh   = max(ah >> amip, 1u);
        int2 apx  = clamp(int2(auv * float2(mw, mh)),
                          int2(0, 0), int2(int(mw) - 1, int(mh) - 1));
        int  slice = int(min(SubIndex, aelems > 0u ? aelems - 1u : 0u));
        // Load pontual, nunca filtrado: o primeiro alvo de array e um shadow map, e
        // interpolar profundidade entre texels de casters diferentes inventa uma superficie
        // intermediaria que nao existe — exatamente o que se quer poder descartar ao olhar.
        float v = TargetArray.Load(int4(apx, slice, int(amip))).r;
        return DebugOutput(saturate(v * ChannelWeight.r * Exposure).xxx);
    }

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
        // Atlas de probe (DDGI): reempacota os tiles numa grade aproximadamente quadrada, cada
        // probe legivel, com a proporcao do painel. Nasceu porque o atlas era uma FILEIRA (largura
        // CountX*CountZ*tile, altura CountY*tile — algo como 8192x64, ~128:1) e esticar isso virava
        // listra. O atlas em si ja e quase quadrado desde o empacotamento 2D, entao hoje o reempa-
        // cotamento e quase identidade — continua aqui porque "quase" nao e "sempre", e porque e
        // ele que ajusta a grade a proporcao do painel, que varia.
        uint tilesX = max(tw / AtlasTilePx, 1u);
        uint tilesY = max(th / AtlasTilePx, 1u);
        uint total  = tilesX * tilesY;
        uint idx;
        float2 inCell;
        if (SubIndex > 0u) {
            // Inspecao individual: SubIndex = tile fisico + 1. Mostra so o interior; a
            // borda octaedrica de 1 px fica fora para nao dominar a leitura ampliada.
            idx = SubIndex - 1u;
            if (idx >= total) return float4(0.0f, 0.0f, 0.0f, 1.0f);
            float scale = 1.0f / max(TileAspect, 1e-4f); // origem quadrada
            if (scale > 1.0f) uv.y = (uv.y - 0.5f) * scale + 0.5f;
            else              uv.x = (uv.x - 0.5f) / scale + 0.5f;
            if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
                return float4(0.0f, 0.0f, 0.0f, 1.0f);
            inCell = uv;
        } else {
            uint cols = max((uint)ceil(sqrt((float)total * max(TileAspect, 1e-4f))), 1u);
            uint rows = (total + cols - 1u) / cols;
            uint cx = min((uint)(uv.x * cols), cols - 1u);
            uint cy = min((uint)(uv.y * rows), rows - 1u);
            idx = cy * cols + cx;
            if (idx >= total) return float4(0.0f, 0.0f, 0.0f, 1.0f);
            inCell = frac(float2(uv.x * cols, uv.y * rows));
        }
        float2 srcTile = float2(idx % tilesX, idx / tilesX);
        if (SubIndex > 0u) {
            float interior = float(max(AtlasTilePx - 2u, 1u));
            uv = (srcTile * float(AtlasTilePx) + 1.0f + inCell * interior)
               / float2(max(tw,1u), max(th,1u));
        } else {
            uv = (srcTile + inCell) * float(AtlasTilePx)
               / float2(max(tw,1u), max(th,1u));
        }
    } else {
        // Preserva o aspecto do alvo (letterbox). Alvo screen-space tem aspecto ~igual ao do
        // tile, entao scale ~= 1 e nada muda: da p/ aplicar sempre, sem flag.
        float scale = (float(tw) / max(float(th), 1.0f)) / max(TileAspect, 1e-4f);
        if (scale > 1.0f) uv.y = (uv.y - 0.5f) * scale + 0.5f;
        else              uv.x = (uv.x - 0.5f) / scale + 0.5f;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // A geometria e rasterizada com projecao jittered. Para inspecao, deslocamos a leitura
    // ate a posicao atual do sinal e o apresentamos em coordenadas sem jitter. Atlas de probe
    // nao depende da camera e por isso ignora este offset.
    float2 sourceUv = AtlasTilePx > 0u ? uv : saturate(uv + ScreenUvOffset);
    uint mw = max(tw >> mip, 1u);
    uint mh = max(th >> mip, 1u);
    int2 tpx = clamp(int2(sourceUv * float2(mw, mh)),
                     int2(0, 0), int2(int(mw) - 1, int(mh) - 1));

    float3 outColor;

    if (Decode == DECODE_GBUFFER_FIELD) {
        // A preview da janela tem resolucao propria, diferente do G-buffer. Indexar por UV
        // mantem o mesmo enquadramento tanto no viewport fullscreen quanto no target offscreen.
        float4 ga, gb, gc;
        if (SubIndex == 7u) {
            // Shading model e um ID discreto: interpolar inventaria classes inexistentes.
            ga = GBufferA.SampleLevel(PointClamp, sourceUv, 0.0f);
            gb = GBufferB.SampleLevel(PointClamp, sourceUv, 0.0f);
            gc = GBufferC.SampleLevel(PointClamp, sourceUv, 0.0f);
        } else {
            // Os demais campos sao continuos. Filtrar durante o resize evita a serrilha
            // adicional que vinha do Load pontual em um preview de resolucao diferente.
            ga = GBufferA.SampleLevel(LinearClamp, sourceUv, 0.0f);
            gb = GBufferB.SampleLevel(LinearClamp, sourceUv, 0.0f);
            gc = GBufferC.SampleLevel(LinearClamp, sourceUv, 0.0f);
        }
        GBufferData g = DecodeGBuffer(ga, gb, gc);
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
        return DebugOutput(outColor * ChannelWeight.rgb);
    }

    if (Decode == DECODE_VELOCITY) {
        uint vw, vh, vmips;
        Velocity.GetDimensions(0, vw, vh, vmips);
        float2 velPx = Velocity.SampleLevel(LinearClamp, sourceUv, 0.0f).rg *
                       float2(vw, vh);
        outColor = float3(0.5f, 0.5f, 0.5f) + float3(velPx * 0.05f, 0.0f);
        return DebugOutput(outColor * ChannelWeight.rgb);
    }

    // Depth, IDs e atlas precisam continuar exatos. Cores/atributos screen-space sao
    // continuos e devem ser reconstruidos linearmente quando o preview muda de resolucao.
    const bool exactSample = AtlasTilePx > 0u || Decode == DECODE_REVERSE_Z ||
                             LinearFilter == 0u;
    float4 s = exactSample
             ? Target.Load(int3(tpx, mip))
             : Target.SampleLevel(LinearClamp, sourceUv, float(mip));

    if (Decode == DECODE_GRAYSCALE) {
        // Alvo de 1 canal (GTAO, mascaras): sem replicar, r vira vermelho puro. Exposure aqui
        // e FATOR DE ESCALA, nao tonemap: o atlas de distancia do DDGI guarda distancia em
        // unidades de mundo, entao qualquer valor > 1 satura em branco sem normalizar por
        // MaxRayDistance.
        outColor = saturate(s.r * ChannelWeight.r * Exposure).xxx;
        return DebugOutput(outColor);
    } else if (Decode == DECODE_OCT_NORMAL) {
        outColor = GBuffer_OctDecode(s.rg) * 0.5f + 0.5f;
    } else if (Decode == DECODE_REVERSE_Z) {
        outColor = VisualizeReverseZ(s.r, NearZ, FarZ);
    } else if (Decode == DECODE_HDR) {
        outColor = TonemapForView(s.rgb * ChannelWeight.rgb, Exposure);
        return DebugOutput(outColor);
    } else if (Decode == DECODE_DDGI_IRRADIANCE) {
        // DDGIUpdate armazena pow(irradiance, 1/gamma) para ganhar precisao no escuro.
        // O consumidor real desfaz essa curva; o debug precisa fazer o mesmo antes do tonemap.
        float3 irradiance = pow(max(s.rgb, 0.0f), DDGI_IRRADIANCE_GAMMA);
        outColor = TonemapForView(irradiance * ChannelWeight.rgb, Exposure);
        return DebugOutput(outColor);
    } else if (Decode == DECODE_DDGI_DISTANCE) {
        outColor = HeatForView(s.r * ChannelWeight.r * Exposure);
        return DebugOutput(outColor);
    } else if (Decode == DECODE_HEATMAP) {
        // Mesmo mapeamento, mas p/ escalar de dominio livre: quem registra o alvo escolhe a
        // Exposure como 1/valor "quente". Sem normalizacao automatica de proposito — normalizar
        // pelo maximo do frame faria a mesma cena mudar de cor conforme a camera anda, e duas
        // capturas deixariam de ser comparaveis.
        const float v = s.r * ChannelWeight.r;
        // ZERO sai PRETO, nao no azul do piso da rampa. No timer de RT isso separa duas coisas
        // que a rampa confunde: "nao houve trabalho" (pixel que saiu cedo — ceu, rugosidade acima
        // do limite) e "houve trabalho e foi barato". Ler o segundo como o primeiro esconde
        // exatamente o custo residual que se quer enxergar.
        outColor = (v <= 0.0f) ? float3(0.0f, 0.0f, 0.0f) : HeatForView(v * Exposure);
        return DebugOutput(outColor);
    } else {
        // RAW: o peso soma os canais escolhidos. Isolar o alfa (0,0,0,1) mostra o alfa em
        // cinza; isolar rg mantem as cores. E o comportamento do r_ShowRenderTarget.
        float4 w = s * ChannelWeight;
        outColor = (ChannelWeight.a > 0.0f && ChannelWeight.r == 0.0f &&
                    ChannelWeight.g == 0.0f && ChannelWeight.b == 0.0f)
                 ? w.a.xxx          // so alfa -> escala de cinza
                 : w.rgb;
        return DebugOutput(outColor);
    }

    return DebugOutput(outColor * ChannelWeight.rgb);
}
