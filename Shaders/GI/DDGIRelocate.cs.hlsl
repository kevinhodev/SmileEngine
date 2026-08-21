#include "DDGICommon.hlsli"

#define DDGI_RAYS      64 
#define RELOCATE_GROUP 64 

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;
    float4 GridCountRays;   
    float4 AtlasParams;  
    float4 SunDirIntensity;
    float4 SunColorHyst;
    float4 TraceParams;
    float4 DistAtlasParams;
    float4 MiscParams;
    float4 MiscParams2;     // x = canMarkActivated (relocacao tem +1 frame agendado)
    // Preenchimento ate o bloco de cascatas. Este passe so precisa do ESPACAMENTO da cascata da
    // sonda — os limiares de relocacao (minFront, maxOff) e a classificacao de raios sao todos
    // em multiplos dele, entao usar o da grossa moveria a sonda da fina quatro vezes demais.
    float4 RayEpsA;   float4 RayEpsB;
    float4 GIDistParams; float4 GIBiasParams;
    float4 ReGIRGridMinSlots; float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples; float4 ReGIRResources;
    float4 SkyParams;
    float4 InvalidateMin; float4 InvalidateMaxHyst;
    float4 RadianceCacheCamCell; float4 RadianceCacheLodCapFlags; float4 RadianceCacheResources;
    float4 MiscParams3;
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    // 6.2b-ii: scroll toroidal, em CELULAS, por cascata (xyz). Espelha o ScrollOffset do
    // FDDGICascadeConstants — o bloco e copiado campo-a-campo, entao a ORDEM e o contrato.
    float4 GICascadeScrollOffset[4];
    // Quanto cada cascata ROLOU desde o ultimo update que rodou, em celulas (xyz), w = rolou.
    // So os passes de update leem: e com ele que DDGI_NewlyExposed decide, em inteiro, se o slot
    // guarda outro ponto do mundo. Ver DDGIConstants::CascadeScrollDelta.
    float4 GICascadeScrollDelta[4];
};

Texture2D<float4> ProbesTrace  : register(t0);
RWBuffer<float4>  ProbeData     : register(u0); 
RWBuffer<uint>    ProbeRayCount : register(u1); 

uint DDGI_DesiredRays(float closestFront, float spacing, int minRays, int maxRays) {
    float ratio = closestFront / max(spacing, 1e-3f);
    int desired = (ratio < 0.5f) ? 256 : (ratio < 1.0f) ? 128 : (ratio < 2.0f) ? 64
                : (ratio < 4.0f) ? 32  : (ratio < 8.0f) ? 16  : 8;
    return (uint)clamp(desired, minRays, maxRays);
}

[numthreads(RELOCATE_GROUP, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int probeIdx  = (int)DTid.x;
    int numProbes = (int)AtlasParams.w;
    int updateProbes = clamp((int)GICascadeParams.z, 1, numProbes);
    if (probeIdx >= updateProbes) return;

    int  maxRays  = (int)MiscParams.z;
    int  minRays  = (int)MiscParams.w;
    // Duas responsabilidades moram neste passe por CONVENIENCIA (os dois querem a mesma varredura
    // dos 64 hits), nao por dependencia: relocar a sonda e classificar quantos raios ela merece.
    // Enquanto o `relocate` desligado saia por aqui, a classificacao ia junto e o ProbeRayCount
    // congelava no ultimo valor escrito — era o que deixava o toggle AdaptiveRays inerte.
    bool relocate = MiscParams.x >= 0.5f;

    // Espacamento da CASCATA desta sonda (indice global -> cascata). Todos os limiares abaixo sao
    // multiplos dele.
    int3   count    = (int3)GridCountRays.xyz;
    int    cascade  = DDGI_CascadeOfProbe(probeIdx, count);
    float  spacing  = GICascadeGridMinSpacing[cascade].w;
    // ARMAZENAMENTO -> GEOMETRIA, so para o teste de lamina: este passe nao usa a posicao de mundo
    // da sonda (todos os limiares dele sao relativos ao proprio hit), mas precisa saber se o slot
    // trocou de lugar no mundo.
    int3   pc       = DDGI_GeometricCoord(DDGI_ProbeCoord(DDGI_LocalProbeIndex(probeIdx, count),
                                                          count),
                                          (int3)GICascadeScrollOffset[cascade].xyz, count);
    const bool newlyExposed =
        DDGI_NewlyExposed(pc, (int3)GICascadeScrollDelta[cascade].xyz, count);

    uint   frame   = (uint)TraceParams.x;
    float4 prev    = ProbeData[probeIdx];
    // Sonda que o relocate moveu no update ANTERIOR (marca w >= 1, ver o bloco do `activated`).
    // Ela e o segundo tempo da lamina: os dois atlas acabaram de ser reintegrados a partir da
    // posicao nova (o update le a mesma marca e zera a histerese), e aqui a marca e demovida.
    const bool justRelocated = prev.w >= 1.0f;

    // Passada de SCROLL: o agendamento veio da rolagem e mais nada, entao so a lamina nova e as
    // sondas que ela acabou de mover tem o que ser reescrito. O early-return fica ANTES da escrita
    // do ProbeRayCount de proposito — reclassificar as sondas velhas aqui as remediria a partir do
    // trace DECIMADO delas, que e exatamente a catraca fechada na fase 4, reaberta uma celula por
    // vez enquanto a camera anda.
    //
    // `newlyExposed` vale independentemente disto: durante os 180 frames iniciais uma rolagem
    // chega com scrollOnly FALSO, e a lamina nova ainda assim nao pode herdar nada.
    if (MiscParams3.w > 0.5f && !newlyExposed && !justRelocated) return;
    // A sonda nova nao tem passado: nem offset (era de outro ponto do mundo) nem marca de inativa
    // (uma sonda enterrada 40 m atras faria o gather pular esta, que aparece como buraco). Zerar
    // `prev` aqui e o que garante que nenhum ramo abaixo herde qualquer um dos dois.
    if (newlyExposed) prev = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 offset  = (relocate && !newlyExposed) ? prev.xyz : float3(0.0f, 0.0f, 0.0f);

    int    backfaceCount   = 0;
    int    realCount       = 0;
    float  closestFront    = 1e27f, farthestFront = 0.0f;
    float3 closestFrontDir = float3(0.0f, 0.0f, 0.0f);
    float3 farthestFrontDir= float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float  d   = ProbesTrace[DDGI_TraceTexel(probeIdx, r, numProbes, DDGI_RAYS)].a;
        if (d < -1e8f) continue; 
        ++realCount;
        float3 dir = DDGI_RayDirection(r, DDGI_RAYS, frame, (uint)probeIdx);
        if (d < 0.0f) {
            backfaceCount++;
        } else {
            if (d < closestFront)  { closestFront  = d; closestFrontDir  = dir; }
            if (d > farthestFront) { farthestFront = d; farthestFrontDir = dir; }
        }
    }

    float backRatio = (realCount > 0) ? (float)backfaceCount / (float)realCount : 0.0f;

    // Contagem de raios: sai da mesma varredura e vale com ou sem relocacao — a sonda parada no
    // vertice do grid tem uma proximidade tao mensuravel quanto a relocada.
    float prox = (realCount > 0) ? closestFront : (spacing * 8.0f);
    ProbeRayCount[probeIdx] = DDGI_DesiredRays(prox, spacing, minRays, maxRays);

    if (!relocate) {
        ProbeData[probeIdx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float minFront  = spacing * 0.30f;
    float maxOff    = spacing * 0.45f;

    float3 target;
    if (backRatio > 0.25f) {
        target = offset + farthestFrontDir * (farthestFront * 0.5f);
    } else if (closestFront < minFront) {
        target = offset - closestFrontDir * (minFront - closestFront);
    } else {
        target = offset;
    }

    float L = length(target);
    if (L > maxOff) target *= (maxOff / L);

    // Tres regimes, e o do meio e o que fecha o buraco do one-shot.
    //
    // ESTREIA da lamina: ONE-SHOT, aplica o alvo inteiro em vez do quarto de passo. O lerp de 0,25
    // e amortecimento de um laco de realimentacao — a sonda move, o trace do frame seguinte sai da
    // posicao nova, o alvo se corrige. A sonda recem-exposta nao tem esse laco: o `delta` volta a
    // zero no proximo update e com ele a informacao de QUAIS sondas eram novas. Ou ela chega ao
    // lugar certo nesta passada, ou fica a um quarto do caminho para sempre. O clamp em `maxOff`
    // ja limitou o salto, entao o alvo inteiro e um passo limitado.
    //
    // FOLLOW-UP da lamina: NAO move. Este update existe para fechar a divergencia que a estreia
    // abriu — os atlas foram integrados no vertice e o offset foi publicado depois. Aqui o trace ja
    // saiu da posicao one-shot e os dois atlas estao sendo reintegrados dela (histerese 0 pela
    // marca), entao mover de novo recriaria exatamente a mesma divergencia, um update adiante e
    // sem nenhum frame agendado para fecha-la. Ele ainda varre o trace: a contagem de raios e a
    // classificacao ativo/inativo sao recalculadas, e so o deslocamento fica parado.
    //
    // Fora do scroll: o lerp de sempre.
    const bool scrollFollowUp = (MiscParams3.w > 0.5f) && justRelocated && !newlyExposed;
    float3 newOffset = scrollFollowUp ? offset
                     : (newlyExposed ? target : lerp(offset, target, 0.25f));

    float thresh = MiscParams.y;
    bool inactive = (backRatio > thresh) && (backfaceCount >= 6);

    // Marca "recem-ativado/relocado" com w em [1,2] (= 1 + backRatio): Update/UpdateDist zeram
    // a hysteresis por 1 frame e tomam a estimativa nova inteira (estilo ACTIVATED do Flax) —
    // senao um probe que sai de dentro da parede converge do preto a 0.99 (~4s de fade). O
    // sampler so testa w<0, entao probe marcado continua amostravel. Auto-demote no frame
    // seguinte: prevW>=1 nao e "inativo" e o passo do lerp ja decaiu abaixo do limiar. O CPU
    // zera MiscParams2.x no ULTIMO frame agendado da relocacao p/ a marca nunca ficar orfa
    // (hyst 0 permanente = flicker eterno naquele probe).
    bool wasInactive = prev.w < 0.0f;
    bool bigJump     = length(newOffset - offset) > spacing * 0.10f;
    bool canMark     = MiscParams2.x > 0.5f;
    bool activated   = !inactive && canMark && (wasInactive || bigJump);

    float w = inactive ? -1.0f : (activated ? 1.0f + backRatio : backRatio);
    ProbeData[probeIdx] = float4(newOffset, w);
}
