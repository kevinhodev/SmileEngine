#ifndef SMILE_GI_RTGEOMETRY_HLSLI
#define SMILE_GI_RTGEOMETRY_HLSLI

// Decodificacao de GEOMETRIA no ponto de hit (normal de face / de shading), extraida do
// HitShading.hlsli p/ passes que precisam da superficie mas nao da iluminacao — hoje o
// BvhDebug, que so quer sombrear o color-coding p/ a forma ficar legivel, e nao pode arrastar
// LightsCommon/BRDF/ReGIR junto.
//
// Contrato de bindings (declarados pelo shader que inclui): Instances
// (StructuredBuffer<InstanceGeo>) e root signature heap-directly-indexed — VB/IB vem bindless
// pelo InstanceGeo, via ResourceDescriptorHeap. Mesmo contrato do RTAlphaTest.hlsli.

// Normal de FACE no ponto de hit — usada pelo ReSTIR GI como n2 no Jacobiano de reconexao.
//
// Era a normal INTERPOLADA de vertice, que e uma normal de SHADING suave: em malha suavizada o
// fator cosDst/cosSrc do Jacobiano saia sistematicamente errado, porque o Jacobiano e definido
// sobre a area real do triangulo (Ouyang 2021 Eq. 11 / GRIS Eq. 52 pedem a geometrica). Agora
// vem do cross das POSICOES; o custo extra e so ler Position dos 3 vertices ja buscados.
//
// Transformacao: cross em espaco de objeto e depois mul(n, WorldToObject) com vetor-linha — que
// e a inversa-transposta de ObjectToWorld, a mesma convencao ja usada pela normal de vertice.
//
// Normal de FACE em espaco de mundo, SEM orientacao — o caller decide o lado. Retorna false em
// triangulo degenerado ou transform singular (o caller cai na normal de vertice).
//
// Teste de degenerado ADIMENSIONAL, feito em espaco de OBJETO e antes da transformacao:
// |cross|^2 / (|e1|^2 |e2|^2) = sin^2(angulo entre as arestas). Comparar o comprimento absoluto
// (ainda mais depois de multiplicar por worldToObject, que reescala por ~1/s) faria o limiar
// depender da escala da instancia e da unidade do asset — instancia muito ampliada cairia no
// fallback sem ser degenerada. sin <= 1e-6 = arestas colineares de verdade; aresta de comprimento
// zero da scale2 = 0 e tambem entra aqui.
bool HitFaceNormal(StructuredBuffer<DDGIVertex> Verts, uint i0, uint i1, uint i2,
                   float3x4 worldToObject, out float3 outFaceN) {
    float3 e1 = Verts[i1].Position - Verts[i0].Position;
    float3 e2 = Verts[i2].Position - Verts[i0].Position;
    float3 nObj = cross(e1, e2);

    float  nObjLen2 = dot(nObj, nObj);
    float  scale2   = dot(e1, e1) * dot(e2, e2);
    float3 nWrld    = mul(nObj, (float3x3)worldToObject);
    float  nLen     = length(nWrld);

    outFaceN = (nLen > 0.0f) ? (nWrld / nLen) : float3(0.0f, 0.0f, 1.0f);
    return (nObjLen2 > 1e-12f * scale2) && (nLen > 0.0f);
}

// Orientacao: dot(faceN, rayDir) em vez de CommittedTriangleFrontFace(). Com a normal de FACE o
// teste do produto escalar e exato (com a interpolada nao era, e por isso havia o gate TwoSided),
// e nao depende da convencao de winding nem da flag TRIANGLE_FRONT_COUNTERCLOCKWISE da instancia
// — o cross ja herda o winding, entao parear os dois arriscaria inverter o sinal duas vezes. O
// gate `geo.TwoSided == 0` saiu: material two-sided agora tambem tem a normal virada no verso,
// como o raster ja faz em GBuffer.ps.hlsl. (Hoje isso e inerte para o unico consumidor: o
// ReconnectionJacobian usa abs() nos dois cossenos. Fica correto para os proximos.)
float3 HitGeomNormal(uint instId, uint tri, float2 bary, float3x4 worldToObject, float3 rayDir) {
    InstanceGeo geo = Instances[instId];
    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];
    uint i0 = Idx[tri * 3 + 0];
    uint i1 = Idx[tri * 3 + 1];
    uint i2 = Idx[tri * 3 + 2];

    float3 faceN;
    if (!HitFaceNormal(Verts, i0, i1, i2, worldToObject, faceN)) {
        // Degenerado: cai na normal de vertice interpolada.
        float3 vObj = Verts[i0].Normal * (1.0f - bary.x - bary.y)
                    + Verts[i1].Normal * bary.x
                    + Verts[i2].Normal * bary.y;
        float3 vWrld = mul(vObj, (float3x3)worldToObject);
        float  vLen  = length(vWrld);
        faceN = (vLen > 1e-5f) ? (vWrld / vLen) : normalize(-rayDir);
    }
    return (dot(faceN, rayDir) > 0.0f) ? -faceN : faceN;
}

#endif
