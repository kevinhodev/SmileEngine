#pragma once

#include <d3d12.h>
#include <vector>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

namespace Smile {
    // Relacao da primitiva com o depth da CENA. Modos EXCLUSIVOS — por isso enum e nao bitmask:
    // o e_DepthTestOn/e_DrawInFront do Cry sao bits porque dividem o campo com cull e blend;
    // aqui e so esta escolha, e um bitmask deixaria "Foreground|Scene" representavel sem
    // significar nada.
    enum class EDebugDepthMode : u8 {
        Foreground = 0, // sempre visivel, por cima da cena (gizmo, markers)
        Scene,          // some atras da geometria (teste manual no PS contra o depth da cena)
        // Solido na frente, TRANSLUCIDO atras. Precisa do alpha blend que chega na F1b; ate la
        // cai no Scene (parte oculta some) e avisa uma vez — nao degrada em silencio.
        XRay,
    };

    class FDebugDraw {
    public:
        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        bool IsInitialized() const { return Initialized; }

        // A API guarda COMANDOS, nao vertices ja achatados: a expansao de linha grossa da F1b
        // precisa da camera e do viewport, que so existem dentro do Render(). Por ora o alpha da
        // cor e o Thickness (em PIXELS) sao GUARDADOS E IGNORADOS — o render ainda e LINELIST de
        // 1px opaco. E de proposito: a API e os callers ja nascem no formato final, e esta fase
        // fica provadamente sem mudanca de pixel.
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
        void WarnXRayOnce();

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

        struct DDVertex { f32 Pos[3]; f32 Color[3]; };
        // Stride derivado do tipo. Era sizeof(f32)*6 solto no .cpp — o mesmo numero escrito duas
        // vezes, e mudar o vertice (alpha, thickness) exigiria lembrar do segundo.
        static constexpr u32 kVBStride = sizeof(DDVertex);

        // Scratch do achatamento comando -> vertice, por bucket de PSO. Membros (e nao locais)
        // p/ o vector reaproveitar a alocacao entre frames.
        std::vector<DDVertex> SceneLineVerts, FgLineVerts, SceneTriVerts, FgTriVerts;

        // Vertice do icone billboard: centro + cor + canto do quad (-1..1) + misc
        // (x = meia-largura mundo, y = tipo, z = selecionada). 6 vertices por icone.
        struct IconVertex { f32 Pos[3]; f32 Color[3]; f32 Corner[2]; f32 Misc[3]; };
        std::vector<IconVertex> IconVerts;

        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> LinePSO;      // Foreground: sem teste de depth
        ComPtr<ID3D12PipelineState> LineScenePSO; // Scene/XRay: teste manual no PS
        ComPtr<ID3D12PipelineState> TriPSO;
        ComPtr<ID3D12PipelineState> TriScenePSO;
        ComPtr<ID3D12PipelineState> IconPSO;
        ComPtr<ID3D12Resource>      CB;      u8* MappedCB     = nullptr;
        ComPtr<ID3D12Resource>      VB;      u8* MappedVB     = nullptr;
        ComPtr<ID3D12Resource>      IconVB;  u8* MappedIconVB = nullptr;
        bool                        Initialized = false;
        // Arma/desarma o aviso de estouro do orcamento: loga na ENTRADA do episodio, nao a cada
        // frame (a 200fps um log por frame vira spam que esconde o resto).
        bool                        OverflowLogged = false;
        bool                        XRayWarned     = false;

        static constexpr u32 kMaxVerts = 4096;
        static constexpr u32 kMaxIconVerts = 256 * 6; // 256 icones/frame
    };
}
