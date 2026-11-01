#ifndef SMILE_RESTIR_GI_VISIBILITY_HLSLI
#define SMILE_RESTIR_GI_VISIBILITY_HLSLI
// Requires Scene, Instances, RTAlphaTest and RayOffset. Never changes reservoir history.
// Tambem requer EnvironmentSky no ReSTIRCB do includer: o .w e a politica de OMM do passe
// (SMILE_OmmRayFlags). Trace e Spatial leem o MESMO CB, entao a visibilidade dos dois concorda
// com o gather sobre a folhagem.
bool ReSTIRGI_Visible(float3 receiver, float3 normal, float3 samplePosition) {
    const float3 dir = SafeRayDir(samplePosition - receiver, normal);
    const float3 origin = OffsetRayGBuffer(receiver, normal, dir,
                                           length(CameraPos.xyz - receiver));
    const float3 segment = samplePosition - origin;
    const float distance = length(segment);
    if (distance <= RayEpsB.y + RayEpsB.z + 0.01f) return true;
    RayDesc ray;
    ray.Origin = RT_WorldToTlas(origin, CameraPos.xyz);
    ray.Direction = segment / distance;
    ray.TMin = RayEpsB.y;
    ray.TMax = distance - RayEpsB.z;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> query;
    query.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                                    SMILE_OmmRayFlags(EnvironmentSky.w),
                          SMILE_RT_MASK_GATHER, ray);
    SMILE_RT_PROCEED(query)
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}
#endif
