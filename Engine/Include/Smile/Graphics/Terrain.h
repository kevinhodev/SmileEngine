#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
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
                            const Mat44& PrevViewProj, const Vec3& CameraPos, f32 FovYRadians);

        // Passes (cada um seta root signature/PSO proprios; o chamador re-liga os dele depois).
        void RenderDepthPrepass(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                                bool WithNormal);
        void RenderGBuffer(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap);
        // Uma cascata do CSM (chamada pelo callback do FSunShadows::RecordDepthPass); culling
        // proprio contra o frustum da cascata (5 planos, sem near — pancaking).
        void RenderShadowCascade(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                                 D3D12_GPU_VIRTUAL_ADDRESS CascadeCB, const Mat44& CascadeVP);

        void GetBounds(Vec3& OutMin, Vec3& OutMax) const { OutMin = BoundsMin; OutMax = BoundsMax; }
        u32  VisibleChunkCount() const { return static_cast<u32>(Visible.size()); }

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
            Vec4  Params2;     // x = debug LOD, y = cinza base, z = roughness, w = -
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
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSOShadow;

        FGrid Grids[kMaxLods];

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer; // kFramesInFlight slices
        u8* MappedCB = nullptr;
        u32 FrameSlot_ = 0;

        FTexture Heightmap;                // R16_UNORM + mips por decimacao
        u32  HeightmapSize = 0;            // texels mip0
        u32  ChunksPerSide = 0;
        u32  MaxLod        = 0;
        FTerrainDesc Desc_;

        std::vector<f32> ChunkMinH, ChunkMaxH; // altura normalizada [0,1], por chunk
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
