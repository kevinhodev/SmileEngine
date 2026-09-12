#ifndef SMILE_SCENE_GPUSCENETABLES_HLSLI
#define SMILE_SCENE_GPUSCENETABLES_HLSLI

// ================================================================================================
// Espelho HLSL das tabelas canonicas da GPU scene (Fase 2 do GPU-DRIVEN-PLAN).
//
// Espelha, byte a byte:
//   GpuMaterialRecord <- Smile::FGpuMaterialRecord  (Graphics/Scene/GpuMaterialRecord.h, 116 B)
//   GpuMeshRecord     <- Smile::FGpuMeshRecord      (Graphics/Scene/GpuSceneTables.h, 68 B)
//   GpuInstanceMeta   <- Smile::FGpuSceneInstanceMeta (20 B)
//   GpuInstanceBounds <- Smile::FGpuSceneInstanceBounds (32 B)
//   GpuInstanceTransform <- Smile::FGpuSceneInstanceTransform (64 B)
//   InstanceGeo       <- Smile::FGpuInstanceGeo     (Graphics/Scene/GpuSceneTables.h, 20 B)
//
// A ABI e verificada em WARP por Tests/GpuSceneTablesWarpTests.cpp, que compila a sonda
// Shaders/Scene/GpuSceneTablesProbe.cs.hlsl com o DXC de producao e le os campos de volta.
// Reordenar um campo aqui sem mexer no C++ faz esse teste falhar; sem ele, faria todo hit de RT
// ler material trocado em silencio.
//
// ATENCAO: este header e PURO. Nada de ResourceDescriptorHeap aqui — shaders de RASTER
// (DebugView.ps, DeferredLighting.ps) o alcancam pelo DDGICommon.hlsli, e o DXC valida o corpo de
// toda funcao mesmo sem chamada. O acessor bindless mora em GpuSceneTablesAccess.hlsli. Mesma
// regra ja escrita no RTTriangleAccess.hlsli, e pelo mesmo incidente.
// ================================================================================================

// Bits de GpuMaterialRecord::Flags. Os oito primeiros herdam o valor numerico dos antigos
// INSTGEO_FLAG_*, que viviam no snapshot por instancia; os dois ultimos eram campos uint inteiros
// (HasAlbedo, TwoSidedRT).
#define SMILE_MAT_FLAG_ALPHATEST   1u   // clip por albedo.a * BaseColor.a vs AlphaCutoff (folhagem)
#define SMILE_MAT_FLAG_EMISSIVEMAP 2u   // tem mapa emissivo
#define SMILE_MAT_FLAG_FOLIAGE     4u   // ShadingModel Foliage (two-sided + transmissao no PT)
#define SMILE_MAT_FLAG_MRMAP       8u   // tem mapa metallic-roughness
#define SMILE_MAT_FLAG_SPECPACK    16u  // o mapa MR e "Specular" (R=AO, G=rough, B=metal)
#define SMILE_MAT_FLAG_METALMAP    32u  // tem mapa Metalness separado (R=metal)
#define SMILE_MAT_FLAG_ROUGHMAP    64u  // tem mapa Roughness separado (R=rough)
#define SMILE_MAT_FLAG_TRANSLUCENT 128u // material Blend; usado pelo BvhDebug
#define SMILE_MAT_FLAG_ALBEDOMAP   256u // tem mapa de albedo (era o uint HasAlbedo)
#define SMILE_MAT_FLAG_TWOSIDED    512u // FMaterial::IsTwoSidedForRT (era o uint TwoSidedRT)

// Bits do BLOCO DE RASTER (Fase 3): os `uint` do MaterialCB que so o raster consulta e que
// chegavam ao pixel shader por um CBV amarrado POR DRAW.
#define SMILE_MAT_FLAG_NORMALMAP       1024u  // HasNormalMap
#define SMILE_MAT_FLAG_AOMAP           2048u  // HasAOMap
#define SMILE_MAT_FLAG_HEIGHTMAP       4096u  // HasHeightMap (parallax)
#define SMILE_MAT_FLAG_NORMALFLIPY     8192u  // normal map DirectX (Y invertido)
#define SMILE_MAT_FLAG_NORMALRECONZ    16384u // normal BC5 (so RG): reconstruir Z
#define SMILE_MAT_FLAG_PARALLAXREFINE  32768u // busca binaria no hit do POM

// Classe de PIPELINE do material (Fase 5). Espelha EGpuPipelineClass (GpuMaterialRecord.h).
//
// VALOR, nao bit: sao quatro classes mutuamente exclusivas, resolvidas por precedencia
// (Blend > AlphaTest > TwoSided > OpaqueOneSided) no lado C++. Renumerar uma delas aqui mantem o
// layout certo e troca o LOTE em que a malha cai — por isso a sonda devolve as quatro constantes
// para o teste conferir, como ja faz com os SMILE_MAT_FLAG_*.
#define SMILE_PIPELINE_CLASS_OPAQUE_ONESIDED 0u
#define SMILE_PIPELINE_CLASS_OPAQUE_TWOSIDED 1u
#define SMILE_PIPELINE_CLASS_MASKED          2u
#define SMILE_PIPELINE_CLASS_BLEND           3u
#define SMILE_PIPELINE_CLASS_COUNT           4u

// Deslocamentos na tabela contigua de descritores do material (FMaterial reserva oito).
#define SMILE_MAT_TEX_ALBEDO   0u
#define SMILE_MAT_TEX_NORMAL   1u
#define SMILE_MAT_TEX_MR       2u
#define SMILE_MAT_TEX_AO       3u
#define SMILE_MAT_TEX_EMISSIVE 4u
#define SMILE_MAT_TEX_HEIGHT   5u
#define SMILE_MAT_TEX_METAL    6u
#define SMILE_MAT_TEX_ROUGH    7u

// Uma linha por MATERIAL, e nao por instancia. Editar um material toca esta linha; as instancias
// que o usam nao sao tocadas.
struct GpuMaterialRecord {
    // --- 64 B lidos pelo RAY TRACING. Offsets congelados: mexer aqui troca o significado de um
    //     byte em todo hit. ---
    float4 BaseColor;       // rgb tint + a opacidade (o alpha-test dos raios usa o .a)
    float4 EmissiveFactor;  // rgb = fator * forca * RTEmissiveScale; w = MetallicFactor
    float4 SubsurfaceColor; // tint rgb + intensidade a
    float  RoughnessFactor;
    float  AlphaCutoff;
    uint   Flags;           // SMILE_MAT_FLAG_*
    uint   TextureBase;     // primeiro descritor do material; 0xFFFFFFFF = sem tabela

    // --- Bloco de RASTER (Fase 3), 48 B. O que o MaterialCB entregava por um CBV por draw e os
    //     64 B acima nao carregam. `ParallaxSelfShadow`/`ParallaxShadowSteps` ficam de fora: os
    //     dois existem no CBV e nenhum shader os le. ---
    float4 RasterEmissive;  // rgb = EmissiveFactor * EmissiveStrength (SEM RTEmissiveScale);
                            // w   = AOStrength
    float  NormalStrength;
    float  HeightScale;
    float  ParallaxMinSteps;
    float  ParallaxMaxSteps;
    float  ParallaxFadeStart;
    float  ParallaxFadeRange;
    uint   ParallaxRefineSteps;
    uint   ShadingModel;    // SMILE_SHADINGMODEL_*; VALOR, nao bit (sao quatro modelos)

    // --- Classe de PIPELINE (Fase 5), 4 B. Derivada dos Flags acima no lado C++ e guardada aqui;
    //     o classify a le por candidato. SMILE_PIPELINE_CLASS_*. ---
    uint   PipelineClass;
};

// Indice bindless de um mapa. Sem tabela, devolve a propria base invalida em vez de um vizinho
// plausivel — um bit de flag errado tem de aparecer como descritor absurdo, nao como textura de
// outro material.
uint MaterialTextureIndex(GpuMaterialRecord _M, uint _Offset) {
    return (_M.TextureBase == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (_M.TextureBase + _Offset);
}

// Uma linha por MALHA UNICA. Espelha Smile::FGpuMeshRecord (68 B).
struct GpuMeshRecord {
    float3 LocalAABBMin; // espaco do OBJETO, como o cozido v7+ grava
    float3 LocalAABBMax;
    uint   VertexSrv;    // StructuredBuffer<DDGIVertex>
    uint   IndexSrv;     // Buffer<uint>
    uint   TriangleSrv;  // StructuredBuffer<RTTriangle>, payload por triangulo (cozido v8)
    uint   VertexCount;
    uint   IndexCount;
    uint   TriangleCount;
    uint   Flags;             // ver SMILE_MESH_FLAG_*; o resto continua reservado
    uint   VirtualGeometryId; // RESERVADO: 0 hoje, ate a trilha de virtual geometry existir
    // Endereco dentro do POOL de geometria (Fase 5). Alimentam `BaseVertexLocation` e
    // `StartIndexLocation` do argumento indireto, e por isso o primeiro e INT como la.
    int    VertexOffset;
    uint   IndexOffset;
    uint   GeometryPoolId; // um ExecuteIndirect amarra UM par VB/IB: lote nao cruza pool
};

// A malha nao pode ir pelo caminho INDIRETO (offset fora do contrato do argumento). Ela continua
// valida para todo o resto; quem a desenha e o fallback CPU.
#define SMILE_MESH_FLAG_OFFSETS_UNREPRESENTABLE 1u

// As duas portas do caminho indireto, espelhando GpuPipelineClassIndirectEligible e
// GpuIndirectEligible (GpuMaterialRecord.h / GpuSceneTables.h).
//
// Existem aqui porque o classify roda na GPU e a resposta tem de ser a MESMA dos dois lados: se o
// espelho esquecesse o termo da flag, a malha irrepresentavel receberia comando com offset poison e
// o command processor leria bytes de vertice como indices. A sonda de ABI chama as duas para que
// isso seja um teste, e nao um comentario.
bool GpuPipelineClassIndirectEligible(uint _PipelineClass) {
    return _PipelineClass == SMILE_PIPELINE_CLASS_OPAQUE_ONESIDED;
}

bool GpuIndirectEligible(uint _PipelineClass, uint _MeshFlags) {
    return GpuPipelineClassIndirectEligible(_PipelineClass) &&
           (_MeshFlags & SMILE_MESH_FLAG_OFFSETS_UNREPRESENTABLE) == 0u;
}

#define SMILE_INSTANCE_FLAG_VISIBLE        1u
#define SMILE_INSTANCE_FLAG_RT_ONLY        2u
#define SMILE_INSTANCE_FLAG_DYNAMIC        4u
#define SMILE_INSTANCE_FLAG_TRANSFORM_CHANGED 8u

// Colunas SoA da tabela canonica de instancias, indexadas por slot de residencia. Generation 0
// marca linha vazia; todo leitor de dado persistente valida a generation capturada.
struct GpuInstanceMeta {
    uint MeshId;
    uint MaterialId;
    uint CellId;
    uint Flags;
    uint Generation;
};

struct GpuInstanceBounds {
    float4 Center;
    float4 Extent;
};

struct GpuInstanceTransform {
    row_major float4x4 Value;
};

// Snapshot por INSTANCIA, indexado pelo InstanceID da TLAS. Depois da Fase 2 ele carrega apenas
// endereco: o par que localiza a linha da malha, o par que localiza a linha do material, e o
// payload por triangulo desnormalizado.
//
// Antes ele tinha 100 B e carregava treze campos de material COPIADOS por instancia. Era isso que
// obrigava toda edicao de material a reescrever a cena inteira, num upload heap sem versao por
// frame, com as duas filas drenadas.
struct InstanceGeo {
    // DESNORMALIZADO de proposito, e e a unica excecao: o AlphaTestPass roda por CANDIDATO de
    // travessia, nao por hit comitado, entao em folhagem ele e chamado varias vezes por raio.
    // Passar por instancia -> mesh -> payload acrescentaria uma leitura dependente exatamente ali.
    uint TriangleSrv;
    uint MeshSrv;     // StructuredBuffer<GpuMeshRecord>: a tabela inteira da cena
    uint MeshId;      // linha dentro dela; 0 = malha ausente
    uint MaterialSrv; // StructuredBuffer<GpuMaterialRecord>: idem
    uint MaterialId;  // linha dentro dela; 0 = material default (renderavel sem material)
};

#endif
