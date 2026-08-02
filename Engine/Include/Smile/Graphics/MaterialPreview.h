#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/HDREnvironment.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace Smile {
    class FCommandQueue;
    class FTextureSRVHeap;
    class FMaterial;

    // Preview offscreen do Editor de Materiais (ate 1024x1024): primitiva unica com o FMaterial
    // selecionado, forward PBR isolado (sol fixo + IBL) e fundo do env cube. Tem um
    // FHDREnvironment PROPRIO — carregar HDRI aqui NAO liga IBL na cena principal (o
    // deferred usa Renderer::HDREnv; IBLParams.w segue HasHDRLoaded() de la).
    //
    // Render sincrono (ResetForRecording + ExecuteAndSync, mesmo caminho das cargas
    // avulsas) + readback RGBA8 — o editor vira QImage direto. Tudo lazy: nada e criado
    // ate o primeiro Render/LoadEnvironment.
    class FMaterialPreview {
    public:
        // 1024: o painel do preview passa de 512 na janela default — 512 upscalado ficava
        // borrado/serrilhado vs o viewport. A 1024 o QML downscala (efeito SSAA de graca).
        static constexpr u32 kSize          = 1024;
        // Browser exibe a thumb em 96px. Renderizar a 256 preserva supersampling suficiente
        // sem pagar o raster/readback de 1024 do preview principal.
        static constexpr u32 kThumbnailSize = 256;

        enum EPrimitive { PrimSphere = 0, PrimCube = 1, PrimPlane = 2, PrimCylinder = 3,
                          PrimSceneMesh = 4 };

        struct FParams {
            i32 Primitive    = PrimSphere;
            f32 Yaw          = 0.7f;   // rad, orbita em torno da origem
            f32 Pitch        = 0.32f;  // rad, +olha de cima
            f32 Dist         = 1.9f;
            f32 EnvRotation  = 0.0f;   // rad
            f32 SunIntensity = 2.5f;
            u32 RenderSize   = kSize;  // lado do output, limitado a 1..kSize
            // Thumbnails estilo UE: sem o fundo do HDRI, clear com alpha 0 (o IBL segue
            // iluminando/refletindo — so nao desenha o ceu).
            bool TransparentBackground = false;
        };

        bool LoadEnvironment(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap, const std::wstring& Path);
        bool HasEnvironment() const { return Env.HasHDRLoaded(); }

        // Renderiza o material na primitiva e devolve RGBA8 RenderSize*RenderSize*4 em Out.
        // false se material invalido ou infra indisponivel (shaders .cso ausentes).
        // Primitive == PrimSceneMesh: desenha SceneMesh com SceneModel (matriz que centra e
        // normaliza a mesh pra caber no enquadramento da esfera — o Renderer monta a partir
        // do AABB do renderable); SceneMesh nulo cai pra esfera.
        bool Render(ID3D12Device* Device, FCommandQueue& CmdQueue, FTextureSRVHeap& SRVHeap,
                    FMaterial& Material, const FParams& Params, std::vector<u8>& Out,
                    const FGpuMesh* SceneMesh = nullptr,
                    const Mat44& SceneModel = Mat44::Identity());

    private:
        bool EnsureInitialized(ID3D12Device* Device, FCommandQueue& CmdQueue,
                               FTextureSRVHeap& SRVHeap);
        void BuildRootSignatures(ID3D12Device* Device);
        bool BuildPSOs(ID3D12Device* Device);
        void CreateTargets(ID3D12Device* Device);
        void CreateMeshes(ID3D12Device* Device);

        // CB unico (upload, mapeado): slot 0 = mesh (VS+PS), slot 1 = sky.
        struct alignas(256) FMeshCB {
            Mat44 MVP;
            Mat44 Model;
            Vec4  CameraPos;
            Vec4  SunDirIntensity;
            Vec4  SunColor;
            Vec4  IBLParams;
        };
        struct alignas(256) FSkyCB {
            Vec4 CamRight;   // xyz + tan(fovX/2)
            Vec4 CamUp;      // xyz + tan(fovY/2)
            Vec4 CamForward; // xyz + rotacao do env
            Vec4 Params;     // x = mip, y = intensidade
        };

        ComPtr<ID3D12Resource>      ColorTarget;
        ComPtr<ID3D12Resource>      DepthTarget;
        ComPtr<ID3D12Resource>      Readback;
        FDescriptorHeap             RTVHeap;
        FDescriptorHeap             DSVHeap;

        ComPtr<ID3D12RootSignature> MeshRootSig;
        ComPtr<ID3D12PipelineState> MeshPSO;
        ComPtr<ID3D12RootSignature> SkyRootSig;
        ComPtr<ID3D12PipelineState> SkyPSO;

        ComPtr<ID3D12Resource>      ConstantBuffer;
        u8*                         MappedCB = nullptr;

        FGpuMesh Meshes[4];              // esfera/cubo/plano/cilindro
        FHDREnvironment Env;             // HDRI do preview (independente da cena)
        u32  IBLTableStart = 0xFFFFFFFFu; // 3 slots contiguos: irradiance/specular/BRDF LUT
        bool IBLTableWritten = false;
        bool Initialized = false;
        bool InitFailed  = false;        // .cso ausente etc. — nao re-tenta todo frame
    };
}
