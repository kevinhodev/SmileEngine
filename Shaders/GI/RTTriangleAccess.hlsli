#ifndef SMILE_GI_RTTRIANGLEACCESS_HLSLI
#define SMILE_GI_RTTRIANGLEACCESS_HLSLI

// ================================================================================================
// ACESSO BINDLESS ao payload por triangulo. UMA funcao — e ela mora separada por um motivo que
// custou uma build quebrada.
//
// O `struct RTTriangle` e os decodificadores (RT_OctDecode, RT_UnpackHalf2, RT_Interpolated*,
// RT_FaceNormal) sao MATEMATICA PURA e ficam no DDGICommon.hlsli, que meio mundo inclui —
// inclusive shaders de RASTER como o DebugView.ps.hlsl e o DeferredLighting.ps.hlsl.
//
// Este acessor NAO e puro: ele usa `ResourceDescriptorHeap`, que exige SM 6.6 e root signature
// heap-directly-indexed. Posto no DDGICommon, ele quebrava a compilacao de TODO shader de raster
// que o incluia, mesmo sem nunca chamar a funcao — o DXC valida o corpo de qualquer jeito. O
// primeiro a estourar foi o DebugView.ps.hlsl.
//
// ⚠️ REGRA: nada que toque ResourceDescriptorHeap pode entrar no DDGICommon.hlsli. Se precisar de
// outro acessor bindless de geometria, ele vem para ca — este header so e incluido pelos dois
// caminhos que ja declaram o contrato de heap-directly-indexed (RTAlphaTest.hlsli e
// RTGeometry.hlsli), e por isso e seguro.
//
// Contrato de bindings: `InstanceGeo` e `RTTriangle` (DDGICommon.hlsli, incluido antes) e root
// signature heap-directly-indexed.
// ================================================================================================

// Acesso unico ao payload. Todos os caminhos de hit passam por aqui — se um dia o stride ou o
// bindless mudarem, muda num lugar so.
RTTriangle RT_LoadTriangle(InstanceGeo _Geo, uint _Tri) {
    StructuredBuffer<RTTriangle> Tris = ResourceDescriptorHeap[_Geo.TriangleSrv];
    return Tris[_Tri];
}

#endif
