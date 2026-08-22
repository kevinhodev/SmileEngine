#pragma once

#include <d3d12.h>
#include <cstddef>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h" // Vec3 + Mat44
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include "Smile/Graphics/Renderer/RenderPass.h"

namespace Smile {
    class FGpuMesh;
    class FTextureSRVHeap;

    // Outline de selecao no editor (estilo Flax/Cry): renderiza o(s) objeto(s) selecionado(s)
    // numa mascara R8 (silhueta) e faz um edge-detect fullscreen que pinta a borda direto no
    // backbuffer LDR (depois do tonemap, p/ a cor nao passar por bloom/exposicao). So sem MSAA.
    class FSelectionOutline : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Contorno da seleção"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        // MVP SEM jitter (o outline e desenhado pos-TAA; usar o MVP jittered do forward faria a
        // borda tremer ~1px/frame com o Halton do TAA). O subsistema grava esse MVP no seu CB.
        struct FDrawItem {
            const FGpuMesh* Mesh;
            Mat44           MVP; // Model * ViewProjUnjittered
        };
        static constexpr u32 kMaxOutlineObjects = 16; // teto de objetos selecionados por frame

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        bool IsInitialized() const override { return Initialized; }

        void SetColor(const Vec3& C)  { Color = C; }
        void SetThickness(f32 T)      { Thickness = T; }
        void SetIntensity(f32 I)      { Intensity = I; }
        void SetFillStrength(f32 F)   { FillStrength = F; } // lavagem sobre o objeto (estilo Cry); 0 = so borda
        Vec3 GetColor() const         { return Color; }
        f32  GetThickness() const     { return Thickness; }
        f32  GetIntensity() const     { return Intensity; }
        f32  GetFillStrength() const  { return FillStrength; }

        // 1) Limpa a mascara e desenha as silhuetas dos itens selecionados nela (sem depth).
        //    Usa as dims supersampled internas (MaskW/MaskH). FrameSlot indexa o CB double-buffered.
        void RecordMask(ID3D12GraphicsCommandList* CmdList,
                        const FDrawItem* Items, size_t Count, u32 FrameSlot);

        // 2) Edge-detect fullscreen da mascara -> blend da borda no RTV de destino.
        //    Chamar apos RecordMask. Transiciona a mascara RT->PSR e de volta. FrameSlot p/ o CB.
        void RecordOutline(ID3D12GraphicsCommandList* CmdList, FTextureSRVHeap& SRVHeap,
                           D3D12_CPU_DESCRIPTOR_HANDLE TargetRTV, u32 Width, u32 Height, u32 FrameSlot);

    private:
        void CreateTargets(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void BuildRootSignatures(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffer(ID3D12Device* Device);

        // CB do edge-detect (b0). Layout casa com OutlineParams no SelectionOutline.ps.
        struct OutlineParams {
            f32 InvSizeX, InvSizeY;
            f32 Thickness, FillStrength;
            f32 ColorR, ColorG, ColorB, Alpha;
        };

        // A mascara e SUPERSAMPLED (kMaskSS x): a silhueta binaria sem AA fazia a borda RASTEJAR ao
        // mover a camera (cobertura virando em degraus de 1px/frame). Renderada a 2x e lida por
        // bilinear no centro do pixel = media exata 2x2 -> cobertura fracionaria -> borda estavel.
        static constexpr u32        kMaskSS = 2;
        ComPtr<ID3D12Resource>      MaskTarget; // R8_UNORM, (W*kMaskSS) x (H*kMaskSS)
        FDescriptorHeap             MaskRTVHeap;
        u32                         MaskSRVSlot = 0xFFFFFFFFu;
        u32                         MaskW = 0, MaskH = 0; // dims supersampled (p/ o viewport do mask pass)
        D3D12_RESOURCE_STATES       MaskState   = D3D12_RESOURCE_STATE_RENDER_TARGET;

        ComPtr<ID3D12RootSignature> MaskRootSig;
        ComPtr<ID3D12PipelineState> MaskPSO;
        ComPtr<ID3D12RootSignature> OutlineRootSig;
        ComPtr<ID3D12PipelineState> OutlinePSO;

        ComPtr<ID3D12Resource>      ParamsCB;
        u8*                         MappedParams = nullptr;

        // CB proprio com o MVP (sem jitter) de cada objeto selecionado — bindado em b2 no mask VS.
        // kMaxOutlineObjects slots de ObjectConstants (256B). Evita o jitter herdado do ObjectCB.
        ComPtr<ID3D12Resource>      ObjectCB;
        u8*                         MappedObjectCB = nullptr;

        Vec3 Color        = { 1.0f, 0.5f, 0.05f }; // laranja editor
        f32  Thickness    = 1.5f;                  // raio da borda em texels
        f32  Intensity    = 1.0f;                  // alpha do blend da borda
        f32  FillStrength = 0.25f;                 // lavagem de cor sobre o objeto (estilo Cry)
        bool Initialized = false;
    };
}
