#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/HDREnvironment.h"
#include "Smile/Graphics/RenderPass.h"
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
    // Render assincrono na fila DIRECT + ring de readback RGBA8. Cada slot tem allocator/list
    // proprios e uma fence compartilhada; Submit nunca espera a GPU e ConsumeCompleted so mapeia
    // slots cuja fence ja terminou. Tudo lazy: nada e criado ate o primeiro Submit/LoadEnvironment.
    class FMaterialPreview : public FPipelineOwner {
    public:
        // --- Dono de pipeline (nao grava frame) (RenderPass.h) ---
        const char* Name() const override { return "Preview de material"; }
        bool IsInitialized() const override { return MeshPSO != nullptr; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        // 1024: o painel do preview passa de 512 na janela default — 512 upscalado ficava
        // borrado/serrilhado vs o viewport. A 1024 o QML downscala (efeito SSAA de graca).
        static constexpr u32 kSize          = 1024;
        // Browser exibe a thumb em 96px. Renderizar a 256 preserva supersampling suficiente
        // sem pagar o raster/readback de 1024 do preview principal.
        static constexpr u32 kThumbnailSize = 256;
        static constexpr u32 kReadbackSlots  = 3;

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

        enum class ESubmitResult : u8 { Submitted, Busy, Failed };

        struct FResult {
            u64 RequestId = 0;
            u32 Size      = 0;
            std::vector<u8> Pixels;
        };

        bool LoadEnvironment(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap, const std::wstring& Path);
        bool HasEnvironment() const { return Env.HasHDRLoaded(); }

        // Grava e submete o material sem esperar. Busy significa que os tres slots ainda estao
        // em voo/aguardando consumo; Failed indica material ou infraestrutura invalidos.
        // Primitive == PrimSceneMesh: desenha SceneMesh com SceneModel (matriz que centra e
        // normaliza a mesh pra caber no enquadramento da esfera — o Renderer monta a partir
        // do AABB do renderable); SceneMesh nulo cai pra esfera.
        ESubmitResult Submit(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap, FMaterial& Material,
                             const FParams& Params, u64 RequestId,
                             const FGpuMesh* SceneMesh = nullptr,
                             const Mat44& SceneModel = Mat44::Identity());

        // Retorna imediatamente false se nenhuma fence terminou. Resultados sao consumidos na
        // ordem de submissao; consumir libera o allocator/readback do slot para reuso.
        bool ConsumeCompleted(FResult& Out);

    private:
        bool EnsureInitialized(ID3D12Device* Device, FCommandQueue& CmdQueue,
                               FTextureSRVHeap& SRVHeap);
        void BuildRootSignatures(ID3D12Device* Device);
        bool BuildPSOs(ID3D12Device* Device);
        void CreateTargets(ID3D12Device* Device);
        void CreateMeshes(ID3D12Device* Device);

        // Upload mapeado e particionado por slot do ring: preview CB + sky CB + snapshot dos
        // MaterialConstants. O snapshot impede UpdateConstants() do editor de alterar uma
        // submissao que a GPU ainda nao consumiu.
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
        FDescriptorHeap             RTVHeap;
        FDescriptorHeap             DSVHeap;

        ComPtr<ID3D12RootSignature> MeshRootSig;
        ComPtr<ID3D12PipelineState> MeshPSO;
        ComPtr<ID3D12RootSignature> SkyRootSig;
        ComPtr<ID3D12PipelineState> SkyPSO;

        struct FReadbackSlot {
            ComPtr<ID3D12CommandAllocator>    Allocator;
            ComPtr<ID3D12GraphicsCommandList> CommandList;
            ComPtr<ID3D12Resource>            Readback;
            u64 RequestId = 0;
            u64 FenceValue = 0;
            u32 Size = 0;
            u32 RowPitch = 0;
            bool Pending = false;
        };

        ComPtr<ID3D12Resource> ConstantBuffer;
        u8*                    MappedCB = nullptr;
        FReadbackSlot          ReadbackSlots[kReadbackSlots];
        ComPtr<ID3D12Fence>    ReadbackFence;
        u64                    NextFenceValue = 0;
        u32                    NextSubmitSlot = 0;

        FGpuMesh Meshes[4];              // esfera/cubo/plano/cilindro
        FHDREnvironment Env;             // HDRI do preview (independente da cena)
        u32  IBLTableStart = 0xFFFFFFFFu; // 3 slots contiguos: irradiance/specular/BRDF LUT
        bool IBLTableWritten = false;
        bool Initialized = false;
        bool InitFailed  = false;        // .cso ausente etc. — nao re-tenta todo frame
    };
}
