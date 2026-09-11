#include "TemporalMotionCommon.hlsli"

cbuffer TemporalMotionCB : register(b0) {
    row_major float4x4 InvViewProj;
    row_major float4x4 ViewProj;
    row_major float4x4 PrevViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params; // x=frame slot, y=history valid, z=position reject scale, w=instance count
    // uint4, e nao float4: sao CAPACIDADES comparadas com indices, e capacidade nao herda o
    // teto de 24 bits do id -- acima dele o float arredonda e o guard passa a mentir nos dois
    // sentidos (rejeita slot valido, ou aceita indice alem do remap).
    uint4 IdentityParams; // x=previous instance count, y=slot capacity
};

Texture2D<float2> RasterVelocity : register(t0);
Texture2D<float4> CurrentSurface : register(t1);
Texture2D<float4> PreviousSurface : register(t2);
StructuredBuffer<FTemporalInstanceTransform> InstanceTransforms : register(t3);
// A tabela ANTERIOR traduz o id gravado no frame passado; a ATUAL confirma que a traducao caiu
// no mesmo objeto; o remap diz onde ele esta na lista de agora.
StructuredBuffer<FGpuIdentityEntry> PreviousIdentity   : register(t4);
StructuredBuffer<FGpuIdentityEntry> CurrentIdentity    : register(t5);
StructuredBuffer<uint>              SlotToCurrentIndex : register(t6);
#if SMILE_TEMPORAL_DERIVED
StructuredBuffer<GpuInstanceMeta>      CanonicalMeta      : register(t7);
StructuredBuffer<GpuInstanceTransform> CanonicalCurrent   : register(t8);
StructuredBuffer<GpuInstanceTransform> CanonicalPrevious  : register(t9);
#endif
RWTexture2D<float2> ReliableMotion : register(u0);

bool InBounds(float2 uv) { return all(uv > 0.0f) && all(uv < 1.0f); }

float4 LoadSurfaceAtUv(Texture2D<float4> tex, float2 uv) {
    const int2 p = clamp(int2(uv * ScreenParams.xy), int2(0, 0), int2(ScreenParams.xy) - 1);
    return tex.Load(int3(p, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    const float2 baseMotion = RasterVelocity.Load(int3(px, 0));
    ReliableMotion[px] = baseMotion;
    if (Params.y < 0.5f) return;

    const float4 current = CurrentSurface.Load(int3(px, 0));
    uint currentId;
    if (!TemporalDecodeInstance(current.w, (uint)Params.w, currentId)) return;

    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 prevUv = uv - baseMotion;
    if (!InBounds(prevUv)) return;

    row_major float4x4 currentToPrevious;
#if SMILE_TEMPORAL_DERIVED
    if (!TemporalCanonicalCurrentToPrevious(
            currentId, CurrentIdentity, CanonicalMeta, CanonicalCurrent, CanonicalPrevious,
            IdentityParams.z, IdentityParams.w, currentToPrevious)) return;
#else
    currentToPrevious = InstanceTransforms[currentId].CurrentToPrevious;
#endif
    const float3 expectedPrevious = TemporalTransformPoint(current.xyz, currentToPrevious);
    const float4 previousAtBase = LoadSurfaceAtUv(PreviousSurface, prevUv);
    uint occluderId;
    // Contagem ANTERIOR, e nao a de agora: um id gravado quando a cena tinha mais objetos
    // passaria na contagem atual sem nunca ter sido valido nela.
    if (!TemporalDecodeInstance(previousAtBase.w, IdentityParams.x, occluderId)) return;

    const float reject = Params.z * max(length(current.xyz - CameraPos.xyz), 1.0f);
    const float baseError = length(previousAtBase.xyz - expectedPrevious);
    if (baseError <= reject) return; // correspondencia convencional ainda e valida

    // Eq. 25.6: Y e o fetch convencional no frame anterior; Z e o mesmo oclusor levado de
    // volta ao frame atual. A translacao relativa X-Z desloca Y para uma regiao de fundo.
    // O id do oclusor foi gravado pela lista do frame PASSADO, e InstanceTransforms e indexado
    // pela lista de AGORA -- usa-lo direto entregava o transform de outro objeto sempre que a
    // lista mudava. A resolucao abaixo o converte para o indice atual do MESMO objeto, ou
    // reprova (removido, slot reciclado, fora do remap) e cai no caminho convencional.
    uint occluderIndex;
    if (!TemporalResolvePreviousIndex(occluderId, PreviousIdentity, SlotToCurrentIndex,
                                      CurrentIdentity, IdentityParams.y, (uint)Params.w,
                                      occluderIndex)) return;

    row_major float4x4 previousToCurrent;
#if SMILE_TEMPORAL_DERIVED
    if (!TemporalCanonicalPreviousToCurrent(
            occluderIndex, CurrentIdentity, CanonicalMeta, CanonicalCurrent, CanonicalPrevious,
            IdentityParams.z, IdentityParams.w, previousToCurrent)) return;
#else
    previousToCurrent = InstanceTransforms[occluderIndex].PreviousToCurrent;
#endif
    const float3 occluderCurrent = TemporalTransformPoint(previousAtBase.xyz, previousToCurrent);
    float2 zUv;
    if (!TemporalProjectUv(occluderCurrent, ViewProj, zUv)) return;
    const float2 dualPrevUv = prevUv + (uv - zUv);
    if (!InBounds(dualPrevUv)) return;

    // A validacao no dominio de mundo evita trocar uma desoclusao por outro objeto qualquer.
    const float4 previousAtDual = LoadSurfaceAtUv(PreviousSurface, dualPrevUv);
    uint backgroundId;
    // Aqui basta a contagem ANTERIOR: o backgroundId e so um gate de existe superficie real
    // neste ponto, e nunca indexa transform -- entao nao precisa ser resolvido.
    if (!TemporalDecodeInstance(previousAtDual.w, IdentityParams.x, backgroundId)) return;
    const float dualError = length(previousAtDual.xyz - expectedPrevious);
    if (dualError >= min(baseError, reject * 4.0f)) return;

    ReliableMotion[px] = uv - dualPrevUv;
}
