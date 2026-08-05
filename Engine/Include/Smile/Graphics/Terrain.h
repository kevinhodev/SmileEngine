#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace Smile {
    class FUploadQueue;

    // Descricao do terreno (sidecar <cena>.terrain.json, chaves planas).
    struct FTerrainDesc {
        std::wstring HeightmapPath;          // .r16: RAW u16 little-endian, quadrado, pot2
        u32          HeightmapSize  = 0;     // texels por lado (0 = infere do tamanho do arquivo)
        f32          UnitsPerTexel  = 1.0f;  // metros de mundo por texel
        f32          HeightScale    = 100.0f;// extensao Y em mundo do range 0..65535
        Vec3         Origin         = { 0.0f, 0.0f, 0.0f }; // mundo do texel (0,0)

        // F2: camadas de material (0 = grama, 1 = terra, 2 = rocha, 3 = alta). Pesos
        // procedurais por declive/ruido/altitude — sem splatmap pintada por enquanto.
        static constexpr u32 kLayers = 4;
        std::wstring LayerAlbedo[kLayers];   // vazio = camada com fallback branco/flat
        std::wstring LayerNormal[kLayers];
        f32          LayerTile[kLayers]  = { 4.0f, 4.0f, 6.0f, 3.0f }; // metros por tile
        f32          LayerRough[kLayers] = { 0.90f, 0.95f, 0.85f, 0.92f };
        f32          RockSlopeStart = 0.45f; // em (1 - N.y)
        f32          RockSlopeEnd   = 0.70f;
        f32          DirtScale      = 0.02f; // freq. do ruido dos patches de terra (1/m)
        f32          DirtAmount     = 0.6f;  // 0..1
        f32          HighStart      = 18.0f; // altitude de entrada da camada alta (mundo)
        f32          HighEnd        = 30.0f;
        f32          BlendContrast  = 4.0f;  // pow dos pesos (transicao mais curta)
        f32          MacroAmount    = 0.18f; // tinte de macro variation (0 desliga)
    };

    // Terreno F1 (renderizacao apenas — sem sculpt/paint no editor):
    //  - heightmap R16_UNORM com mips por DECIMACAO (mip N = texel 2N do mip 0), pra altura
    //    do morph bater exatamente com a que o proximo LOD renderiza (sem crack vertical);
    //  - grade de chunks (128 quads no LOD0) com um pool de grids COMPARTILHADOS por LOD
    //    (vertice = so UV + pesos de morph; a altura vem da heightmap no VS) — estilo Flax;
    //  - LOD continuo por screen-size (estilo UE) + costura por morph geometrico no VS
    //    (estilo Flax/CDLOD): sem skirts, sem permutacao de indices;
    //  - passes: z-prepass (depth ou depth+normal p/ GTAO), G-buffer (+velocity) e CSM
    //    (mesmo LOD da vista). RT/TLAS fica pra F3.
    class FTerrain {
    public:
        static constexpr u32 kChunkQuads = 128; // quads por lado do chunk no LOD0
        static constexpr u32 kMaxLods    = 8;   // LOD 0..7 (chunk de 1 quad no 7)

        void Initialize(ID3D12Device* Device);
        void RecreatePSOs(ID3D12Device* Device); // hot-reload dos shaders do terreno

        bool Load(ID3D12Device* Device, FUploadQueue& UploadQueue, FTextureSRVHeap& SRVHeap,
                  const FTerrainDesc& Desc);
        void Unload(FTextureSRVHeap& SRVHeap);
        bool IsLoaded() const { return ChunksPerSide > 0; }

        // Seleciona LOD por chunk (screen-size), resolve LOD dos vizinhos e faz o frustum
        // cull da vista. Escreve o CB do slot do frame.
        void UpdatePerFrame(u32 FrameSlot, const Mat44& ViewProj, const Mat44& ViewProjNoJitter,
                            const Mat44& PrevViewProj, const Vec3& CameraPos, f32 FovYRadians,
                            f32 MipBias);

        // Passes (cada um seta root signature/PSO proprios; o chamador re-liga os dele depois).
        void RenderDepthPrepass(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                                bool WithNormal);
        void RenderGBuffer(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap);
        // Uma cascata do CSM (chamada pelo callback do FSunShadows::RecordDepthPass); culling
        // proprio contra o frustum da cascata (5 planos, sem near — pancaking).
        // Depth do terreno numa view de sombra qualquer (cascata do CSM, slice de spot ou face
        // de cubo de point). PerspectiveView escolhe as duas coisas que separam os dois casos:
        // PSO com depth clip ligado (contra o pancaking grampear o chunk no near e virar
        // oclusor falso colado na luz) e o plano near no culling. O CSM e ortho e quer o
        // oposto nas duas — caster atras do near dele ainda projeta sombra valida.
        // LightRadius > 0 liga a broad phase esferica (sombras locais): o chunk precisa estar
        // dentro do alcance da luz, nao so do frustum dela. Ver FLocalShadows::FExtraLocalDraw.
        void RenderShadowCascade(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                                 D3D12_GPU_VIRTUAL_ADDRESS CascadeCB, const Mat44& CascadeVP,
                                 bool PerspectiveView = false,
                                 const Vec3& LightPos = Vec3{}, f32 LightRadius = 0.0f);

        void GetBounds(Vec3& OutMin, Vec3& OutMax) const { OutMin = BoundsMin; OutMax = BoundsMax; }
        u32  VisibleChunkCount() const { return static_cast<u32>(Visible.size()); }

        // F3: malha proxy do heightfield (decimada, world-space, transform identidade) pro
        // BLAS da cena — DDGI/ReSTIR/reflexoes passam a ver o chao. So valida apos Load.
        bool BuildProxyMesh(FMesh& Out) const;

        // Albedo BAKEADO do proxy de RT (padrao "multilayer proxy" do Red Engine 4, GPU Zen 3
        // 7.3.2): o blend procedural de 4 camadas so existe no pixel shader, entao o hit shading
        // do RT nao tem como avalia-lo e o proxy entrava na TLAS com UMA cor chapada — todo o
        // color bleed do terreno no GI saia com o tom do vale, mesmo em encosta de rocha.
        //
        // O bake sai pronto do Load (aqui e onde a heightmap mip 0 e as CPU data das camadas
        // ainda estao vivas) e e MOVIDO daqui pelo dono do material, que o sobe como textura e
        // controla o lifetime — o mesmo dono do FMaterial do proxy. Depois de tomado, o membro
        // fica vazio (sao ~5 MB de CPU que nao precisam sobreviver ao load).
        bool TakeProxyAlbedoCPU(FTextureCPUData& Out);

        void SetDebugLodColors(bool V) { DebugLodColors = V; }
        bool GetDebugLodColors() const { return DebugLodColors; }
        void SetLod0ScreenSize(f32 V)  { Lod0ScreenSize = V < 0.01f ? 0.01f : V; }
        f32  GetLod0ScreenSize() const { return Lod0ScreenSize; }
        void SetLodBias(i32 V)         { LodBias = V; }
        i32  GetLodBias() const        { return LodBias; }
        void SetBaseGray(f32 V)        { BaseGray = V; }
        void SetRoughness(f32 V)       { Roughness = V; }

    private:
        struct alignas(256) TerrainConstants {
            Mat44 ViewProj;
            Mat44 ViewProjNoJitter;
            Mat44 PrevViewProj;
            Vec4  OriginUnits; // xyz = origem, w = unidades por texel
            Vec4  Params;      // x = heightScale, y = texels mip0, z = quads LOD0, w = maxLod
            Vec4  Params2;     // x = debug LOD, y = cinza base, z = roughness, w = tem camadas
            Vec4  Params3;     // x = mip bias, y = contraste blend, z = escala ruido terra, w = qtd terra
            Vec4  Params4;     // xy = slope start/end rocha, zw = altura start/end camada alta
            Vec4  LayerTiling; // 1/metros-por-tile por camada
            Vec4  LayerRough;  // roughness por camada
            Vec4  CamPosMacro; // xyz = camera (mundo), w = intensidade da macro variation
        };
        struct ChunkConstants { // root constants (8 dwords) — espelha ChunkCB do hlsl
            u32 ChunkX, ChunkZ, Lod, Pad;
            f32 NeighborLod[4];
        };
        struct FGrid {
            Microsoft::WRL::ComPtr<ID3D12Resource> VB, IB;
            D3D12_VERTEX_BUFFER_VIEW VBV{};
            D3D12_INDEX_BUFFER_VIEW  IBV{};
            u32 IndexCount = 0;
        };

        // Bake do albedo do proxy de RT. Precisa da heightmap mip 0 (declive por diferencas
        // centrais na resolucao NATIVA — o proxy decimado 8x suavizaria a encosta e perderia a
        // rocha) e das cores medias das camadas, por isso roda dentro do Load.
        void BakeProxyAlbedo(const std::vector<u16>& Mip0, u32 Size);

        void BuildRootSignature(ID3D12Device* Device);
        void BuildGrids(ID3D12Device* Device);
        void CreateConstantBuffer(ID3D12Device* Device);
        void DrawChunks(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                        ID3D12PipelineState* PSO, const std::vector<u32>& List,
                        D3D12_GPU_VIRTUAL_ADDRESS CascadeCB = 0);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSODepthOnly;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSODepthNormal;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSOGBuffer;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSOShadow;      // CSM: ortho + pancaking
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSOShadowLocal; // spot/point: depth clip
        // Lista de chunks visiveis de UMA view de sombra. Membro so pra nao alocar por view:
        // com sombras locais sao ate 8 slices + 24 faces de cubo por frame.
        std::vector<u32>                            ShadowCullScratch;
        // Chunks dentro da esfera da luz corrente. O FLocalShadows chama as 6 faces de um point
        // em sequencia com a MESMA esfera, entao a broad phase roda uma vez por luz e as faces
        // so refinam por frustum — que e o contrato "broad phase por luz + frustum por face".
        // Memoizar aqui evita partir o callback num par PrepareLight/DrawView.
        std::vector<u32>                            ShadowSphereSubset;
        Vec3                                        ShadowSphereCenter = { 0.0f, 0.0f, 0.0f };
        f32                                         ShadowSphereRadius = -1.0f; // <0 = sem cache

        FGrid Grids[kMaxLods];

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer; // kFramesInFlight slices
        u8* MappedCB = nullptr;
        u32 FrameSlot_ = 0;

        FTexture Heightmap;                // R16_UNORM + mips por decimacao
        // F2: texturas das camadas (4 albedo + 4 normal; faltantes viram fallback 1x1) e a
        // tabela CONTIGUA t1-t8 (padrao FMaterial: SRVs re-criados nos slots da tabela).
        FTexture LayerTex[2 * FTerrainDesc::kLayers];
        u32  LayerTableStart = 0xFFFFFFFFu;
        bool HasLayers       = false;
        u32  HeightmapSize = 0;            // texels mip0
        u32  ChunksPerSide = 0;
        u32  MaxLod        = 0;
        FTerrainDesc Desc_;

        std::vector<f32> ChunkMinH, ChunkMaxH; // altura normalizada [0,1], por chunk
        // F3: copia CPU decimada (1 amostra a cada kProxyStep texels) p/ a malha proxy do RT
        static constexpr u32 kProxyStep = 8;
        std::vector<f32> ProxyHeights;         // (ProxyVerts)^2, altura normalizada
        u32              ProxyVerts = 0;
        // Teto da resolucao do bake de albedo do proxy (clampado ao tamanho da heightmap). 1024
        // num terreno de 2048 m da 2 m por texel — 4x mais fino que o quad do proxy (8 m) e ja
        // MUITO abaixo da frequencia do tiling das camadas (4-7 m por tile), que e justamente o
        // que autoriza usar a cor media de cada camada no lugar de amostrar a textura.
        static constexpr u32 kProxyAlbedoMaxSize = 1024;
        Vec3            LayerMeanColor[FTerrainDesc::kLayers]{}; // media LINEAR do albedo da camada
        FTextureCPUData ProxyAlbedoCPU;                          // movido no TakeProxyAlbedoCPU
        std::vector<u8>  ChunkLods;            // LOD selecionado no frame (por chunk)
        std::vector<u32> Visible;              // indices dos chunks visiveis na vista

        Vec3 BoundsMin{}, BoundsMax{};

        bool Initialized    = false;
        bool DebugLodColors = false;
        f32  Lod0ScreenSize = 0.25f; // fracao da altura da tela p/ manter LOD0 (UE-style)
        i32  LodBias        = 0;
        f32  BaseGray       = 0.35f;
        f32  Roughness      = 0.95f;
    };
}
