#ifndef SMILE_GI_RTGEOMETRY_HLSLI
#define SMILE_GI_RTGEOMETRY_HLSLI

// RT_LoadTriangle. Separado do DDGICommon porque usa ResourceDescriptorHeap — ver a nota la.
#include "RTTriangleAccess.hlsli"

// Decodificacao de GEOMETRIA no ponto de hit (normal de face / de shading), extraida do
// HitShading.hlsli p/ passes que precisam da superficie mas nao da iluminacao — hoje o
// BvhDebug, que so quer sombrear o color-coding p/ a forma ficar legivel, e nao pode arrastar
// LightsCommon/BRDF/ReGIR junto.
//
// Contrato de bindings (declarados pelo shader que inclui): Instances
// (StructuredBuffer<InstanceGeo>) e root signature heap-directly-indexed — o payload por
// triangulo vem bindless pelo InstanceGeo.TriangleSrv. Mesmo contrato do RTAlphaTest.hlsli.
//
// ⚠️ Desde a v8 do cozido NENHUMA funcao daqui toca IndexSrv ou VertexSrv. O cross das posicoes
// e a interpolacao das normais foram para o cozimento (Smile::BuildRTTriangles), e o que sobrou
// em runtime e a decodificacao mais a transformacao — que dependem da INSTANCIA e por isso nao
// podiam ser cozidas.

// Normal de FACE no ponto de hit — usada pelo ReSTIR GI como n2 no Jacobiano de reconexao.
//
// Era a normal INTERPOLADA de vertice, que e uma normal de SHADING suave: em malha suavizada o
// fator cosDst/cosSrc do Jacobiano saia sistematicamente errado, porque o Jacobiano e definido
// sobre a area real do triangulo (Ouyang 2021 Eq. 11 / GRIS Eq. 52 pedem a geometrica). Ela vem
// do cross das POSICOES — que agora e feito UMA vez, no cozimento, e em precisao dupla.
//
// O criterio de degeneracao continua o mesmo e continua adimensional: |cross|^2 /
// (|e1|^2 |e2|^2) = sin^2(angulo entre as arestas), com sin <= 1e-6 = colineares de verdade.
// Ele so mudou de LUGAR — vive no bit RTTRI_FLAG_FACE_NORMAL_VALID. O segundo braco (transform
// singular) continua em runtime dentro do RT_FaceNormal, porque e por instancia.
//
// Mantida como encaminhador de nome estavel: os call sites falam de "normal de face do hit", e
// e isso que ela continua sendo.
bool HitFaceNormal(RTTriangle tri, float3x4 worldToObject, out float3 outFaceN) {
    return RT_FaceNormal(tri, worldToObject, outFaceN);
}

// Orientacao: dot(faceN, rayDir) em vez de CommittedTriangleFrontFace(). Com a normal de FACE o
// teste do produto escalar e exato (com a interpolada nao era, e por isso havia o gate TwoSided),
// e nao depende da convencao de winding nem da flag TRIANGLE_FRONT_COUNTERCLOCKWISE da instancia
// — o cross ja herda o winding, entao parear os dois arriscaria inverter o sinal duas vezes. O
// gate `geo.TwoSided == 0` saiu: material two-sided agora tambem tem a normal virada no verso,
// como o raster ja faz em GBuffer.ps.hlsl. (Hoje isso e inerte para o unico consumidor: o
// ReconnectionJacobian usa abs() nos dois cossenos. Fica correto para os proximos.)
// CLASSIFICACAO do hit, sem sombrear — a politica de auto-interseccao precisa decidir se
// re-traca ANTES de pagar o shading, e o caller e quem escolhe o que fazer (o ReSTIR mata o
// caminho, o DDGI usa o outSignedDist do ShadeSurfaceHit, as reflexoes tem politica propria).
//
// `outTwoSided` sai junto porque a politica trata os dois casos com distancias diferentes.
// Custo: UM registro de 32 B — era 3 indices + as 3 posicoes, em duas rodadas dependentes.
//
// Morava no HitShading.hlsli, ABAIXO do include do PathTracingCommon — o que impedia a politica
// de auto-interseccao de virar funcao compartilhada la. Mudou-se para ca porque e o que ela e:
// geometria do hit sem iluminacao, o mesmo criterio que trouxe o HitGeomNormal.
bool HitIsBackface(uint instId, uint tri, float3x4 worldToObject, float3 rayDir,
                   out bool outTwoSided) {
    InstanceGeo geo = Instances[instId];
    outTwoSided = (geo.TwoSidedRT != 0);

    float3 faceN;
    if (!HitFaceNormal(RT_LoadTriangle(geo, tri), worldToObject, faceN))
        return false; // degenerado: nao da p/ afirmar que e verso
    return dot(faceN, rayDir) > 0.0f;
}

float3 HitGeomNormal(uint instId, uint tri, float2 bary, float3x4 worldToObject, float3 rayDir) {
    InstanceGeo geo = Instances[instId];
    const RTTriangle T = RT_LoadTriangle(geo, tri);

    float3 faceN;
    if (!HitFaceNormal(T, worldToObject, faceN)) {
        // Degenerado: cai na normal de vertice interpolada — mesmo fallback de antes, so que a
        // normal ja vem decodificada do payload em vez de vir do VB.
        float3 vObj  = RT_InterpolatedNormalObj(T, bary);
        float3 vWrld = mul(vObj, (float3x3)worldToObject);
        float  vLen  = length(vWrld);
        faceN = (vLen > 1e-5f) ? (vWrld / vLen) : normalize(-rayDir);
    }
    return (dot(faceN, rayDir) > 0.0f) ? -faceN : faceN;
}

#endif
