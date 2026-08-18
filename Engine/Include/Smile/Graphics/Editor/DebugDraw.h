#pragma once

#include <d3d12.h>
#include <array>
#include <vector>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Renderer/RenderPass.h"

namespace Smile {
    // Relacao da primitiva com o depth da CENA. Modos EXCLUSIVOS — por isso enum e nao bitmask:
    // o e_DepthTestOn/e_DrawInFront do Cry sao bits porque dividem o campo com cull e blend;
    // aqui e so esta escolha, e um bitmask deixaria "Foreground|Scene" representavel sem
    // significar nada.
    enum class EDebugDepthMode : u8 {
        Foreground = 0, // sempre visivel, por cima da cena (gizmo, markers)
        Scene,          // some atras da geometria (teste manual no PS contra o depth da cena)
        XRay,           // solido na frente, TRANSLUCIDO atras (a parte oculta continua legivel)
    };

    class FDebugDraw : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Debug draw (gizmo/ícones)"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        bool IsInitialized() const override { return Initialized; }
        // Hot reload de shader. Reusa o FORMATO GUARDADO no Initialize de proposito: o debug
        // desenha no BACKBUFFER, nao no RT HDR da cena, entao o formato que o RecreateAllPSOs
        // passa pros outros passes (R16G16B16A16) criaria estes PSOs errados.
        void RecreatePSOs(ID3D12Device* Device);

        // A API guarda COMANDOS, nao vertices ja achatados: a expansao da linha grossa acontece
        // no Render(), unico ponto com camera e viewport. Thickness em PIXELS (constante em tela,
        // como o Thickness/bScreenSpace do FBatchedElements da Unreal); alpha da cor e usado.
        void Line(const Vec3& A, const Vec3& B, const Vec4& Color,
                  EDebugDepthMode Mode = EDebugDepthMode::Foreground, f32 Thickness = 1.0f);
        void Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec4& Color,
                      EDebugDepthMode Mode = EDebugDepthMode::Foreground);
        // Icone billboard (estilo sprite de luz da UE / ViewportIcons do Flax): quad que encara
        // a camera, glifo por SDF no PS (Type 0 = lampada, 1 = spot), alpha-blend. Nao leva modo
        // de depth: icone de editor e Foreground por design. HalfSize em unidades de mundo (a
        // CPU escala por distancia p/ tamanho de tela constante); Selected desenha o anel branco.
        // Nao corta por orcamento aqui: quem clampa e relata e o Render(), num lugar so.
        void Icon(const Vec3& Center, f32 HalfSize, const Vec3& Color, u32 Type, bool Selected);

        void Clear() {
            LineCmds.clear(); TriCmds.clear(); IconVerts.clear(); SceneDepthPrims = 0;
        }
        bool Empty() const {
            return LineCmds.empty() && TriCmds.empty() && IconVerts.empty();
        }
        // Ha primitiva que precisa do depth da cena como SRV neste frame? O Renderer usa p/
        // decidir se vale a transicao de estado do depth buffer.
        bool NeedsSceneDepth() const { return SceneDepthPrims > 0; }

        // DepthSRV (shader-visible, res de render, estado PIXEL_SHADER_RESOURCE) habilita os
        // modos que testam depth; ptr 0 os descarta no frame (fallback seguro). CamRight/CamUp =
        // eixos da camera em mundo (expansao dos billboards).
        void Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                    D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height,
                    const Vec3& CamRight = Vec3::UnitX(), const Vec3& CamUp = Vec3::UnitY(),
                    D3D12_GPU_DESCRIPTOR_HANDLE DepthSRV = {});

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        void CreateBuffers(ID3D12Device* Device);
        // Depth SO DO OVERLAY (nao e o depth da cena): existe pra o debug se ordenar CONTRA SI
        // MESMO. Criado sob demanda no Render() a partir da largura/altura que ele ja recebe —
        // assim nao ha um caminho de resize pra alguem esquecer de chamar.
        void EnsureOverlayDepth(u32 Width, u32 Height);

        // Fracao do alpha que sobrevive atras da geometria, por modo. Vai como root constant:
        // Scene e XRay usam o MESMO PSO e diferem so nisto.
        static f32 OccludedAlphaFor(EDebugDepthMode Mode) {
            return Mode == EDebugDepthMode::XRay ? kXRayAlpha : 0.0f;
        }
        static constexpr f32 kXRayAlpha = 0.28f;

        struct FLineCmd {
            Vec3 A, B;
            Vec4 Color;
            f32  Thickness = 1.0f;
            EDebugDepthMode Mode = EDebugDepthMode::Foreground;
        };
        struct FTriCmd {
            Vec3 A, B, C;
            Vec4 Color;
            EDebugDepthMode Mode = EDebugDepthMode::Foreground;
        };
        std::vector<FLineCmd> LineCmds;
        std::vector<FTriCmd>  TriCmds;
        u32                   SceneDepthPrims = 0; // comandos submetidos em Scene/XRay

        // Cor empacotada em RGBA8: 16B por vertice no lugar de 24B, e o alpha entra de graca no
        // espaco que o float3 desperdicava. Ordem de byte do R8G8B8A8_UNORM (R no byte baixo).
        static u32 PackColor(const Vec4& C);

        // Um comando por SEGMENTO, nao 4 vertices: o VS gera os cantos do quad pelo SV_VertexID.
        struct FLineInstance { f32 A[3]; f32 B[3]; u32 Color; f32 Thickness; };
        struct DDVertex      { f32 Pos[3]; u32 Color; };
        static constexpr u32 kLineStride = sizeof(FLineInstance);
        static constexpr u32 kVBStride   = sizeof(DDVertex);

        // Scratch do achatamento comando -> GPU, um bucket por modo (indice = EDebugDepthMode).
        // Membros e nao locais p/ o vector reaproveitar a alocacao entre frames.
        static constexpr u32 kModeCount = 3;
        std::array<std::vector<FLineInstance>, kModeCount> LineBuckets;
        std::array<std::vector<DDVertex>, kModeCount>      TriBuckets;
        // Ordem de desenho: quem testa depth primeiro, Foreground por cima. Agrupar por MODO
        // (e nao por topologia) e o que sustenta essa regra — ver o Render().
        static constexpr EDebugDepthMode kDrawOrder[kModeCount] = {
            EDebugDepthMode::Scene, EDebugDepthMode::XRay, EDebugDepthMode::Foreground
        };

        // Vertice do icone billboard: centro + cor + canto do quad (-1..1) + misc
        // (x = meia-largura mundo, y = tipo, z = selecionada). 6 vertices por icone.
        struct IconVertex { f32 Pos[3]; f32 Color[3]; f32 Corner[2]; f32 Misc[3]; };
        std::vector<IconVertex> IconVerts;

        ComPtr<ID3D12RootSignature> RootSig;
        // "Depth" no nome do PSO = teste manual contra o depth da CENA (no PS). O depth do
        // OVERLAY e estado fixo do pipeline e vale pra todos eles menos o icone.
        ComPtr<ID3D12PipelineState> LinePSO;      // Foreground: sem teste contra a cena
        ComPtr<ID3D12PipelineState> LineDepthPSO; // Scene/XRay
        ComPtr<ID3D12PipelineState> TriPSO;
        ComPtr<ID3D12PipelineState> TriDepthPSO;
        ComPtr<ID3D12PipelineState> IconPSO;      // sem depth nenhum: camada de cima, por politica
        ComPtr<ID3D12Resource>      CB;      u8* MappedCB     = nullptr;
        ComPtr<ID3D12Resource>      LineVB;  u8* MappedLineVB = nullptr;
        ComPtr<ID3D12Resource>      TriVB;   u8* MappedTriVB  = nullptr;
        ComPtr<ID3D12Resource>      IconVB;  u8* MappedIconVB = nullptr;

        ComPtr<ID3D12Resource> OverlayDepth;
        FDescriptorHeap        OverlayDSVHeap;
        u32                    OverlayWidth  = 0;
        u32                    OverlayHeight = 0;
        static constexpr DXGI_FORMAT kOverlayDepthFormat = DXGI_FORMAT_D32_FLOAT;

        ID3D12Device*               Device_     = nullptr;         // p/ criar o depth no resize
        DXGI_FORMAT                 RTFormat_   = DXGI_FORMAT_UNKNOWN; // p/ o RecreatePSOs
        bool                        Initialized = false;
        // Arma/desarma o aviso de estouro do orcamento: loga na ENTRADA do episodio, nao a cada
        // frame (a 200fps um log por frame vira spam que esconde o resto).
        bool                        OverflowLogged = false;

        static constexpr u32 kMaxLines     = 2048;    // segmentos/frame (linha = 1 instancia)
        static constexpr u32 kMaxTriVerts  = 4096;
        static constexpr u32 kMaxIconVerts = 256 * 6; // 256 icones/frame
    };
}
