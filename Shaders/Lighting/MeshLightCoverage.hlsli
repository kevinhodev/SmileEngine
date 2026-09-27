#ifndef SMILE_MESH_LIGHT_COVERAGE_HLSLI
#define SMILE_MESH_LIGHT_COVERAGE_HLSLI
#include "MeshLightCommon.hlsli"
// Tasks are sorted by InstanceIndex. Use ORIGINAL light indices: compaction only
// removes zero-support entries and must not change hit-to-light coverage.
bool MeshLight_CoversHit(StructuredBuffer<FMeshLightTask> tasks,
                         StructuredBuffer<FTriangleLightGPU> lights,
                         uint taskCount, uint instanceId, uint triangleIndex) {
    uint lo = 0, hi = taskCount;
    while (lo < hi) {
        const uint mid = lo + (hi - lo) / 2;
        const FMeshLightTask task = tasks[mid];
        if (task.InstanceIndex < instanceId) lo = mid + 1;
        else if (task.InstanceIndex > instanceId) hi = mid;
        else {
            if (triangleIndex >= task.TriangleCount) return false;
            const FTriangleLightGPU light = lights[task.LightOffset + triangleIndex];
            return isfinite(light.Flux) && light.Flux > 0.0f &&
                   any(MeshLight_UnpackRGB9E5(light.Radiance) > 0.0f) &&
                   length(cross(MeshLight_Edge0(light), MeshLight_Edge1(light))) >= 1e-12f;
        }
    }
    return false;
}
#endif
