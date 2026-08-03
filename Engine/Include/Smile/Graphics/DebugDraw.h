#pragma once

#include <d3d12.h>
#include <vector>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

namespace Smile {
    class FDebugDraw {
    public:
        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        bool IsInitialized() const { return Initialized; }

        void Line(const Vec3& A, const Vec3& B, const Vec3& Color);
        // Linha depth-testada contra o depth da CENA (some atras da geometria) — teste manual
        // no PS via SRV, porque o debug desenha pos-tonemap sem DSV. Usada pros volumes de luz
        // do editor; gizmo/markers usam Line() normal (sempre visiveis).
        void LineOccluded(const Vec3& A, const Vec3& B, const Vec3& Color);
        void Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec3& Color);
        // Icone billboard (estilo sprite de luz da UE / ViewportIcons do Flax): quad que encara
        // a camera, glifo por SDF no PS (Type 0 = lampada, 1 = spot), alpha-blend, sempre
        // visivel. HalfSize em unidades de mundo (a CPU escala por distancia p/ tamanho de tela
        // constante); Selected desenha o anel branco.
        // Nao corta por orcamento aqui: quem clampa e relata e o Render(), num lugar so.
        void Icon(const Vec3& Center, f32 HalfSize, const Vec3& Color, u32 Type, bool Selected);
        void Clear() {
            LineVerts.clear(); TriVerts.clear(); LineOccVerts.clear(); IconVerts.clear();
        }
        bool Empty() const {
            return LineVerts.empty() && TriVerts.empty() && LineOccVerts.empty() &&
                   IconVerts.empty();
        }
        bool HasOccluded() const { return !LineOccVerts.empty(); }

        // DepthSRV (shader-visible, res de render, estado PIXEL_SHADER_RESOURCE) habilita as
        // linhas ocluiveis; ptr 0 as descarta no frame (fallback seguro). CamRight/CamUp =
        // eixos da camera em mundo (expansao dos billboards).
        void Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                    D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height,
                    const Vec3& CamRight = Vec3::UnitX(), const Vec3& CamUp = Vec3::UnitY(),
                    D3D12_GPU_DESCRIPTOR_HANDLE DepthSRV = {});

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        void CreateBuffers(ID3D12Device* Device);

        struct DDVertex { f32 Pos[3]; f32 Color[3]; };
        std::vector<DDVertex> LineVerts;
        std::vector<DDVertex> TriVerts;
        std::vector<DDVertex> LineOccVerts;

        // Vertice do icone billboard: centro + cor + canto do quad (-1..1) + misc
        // (x = meia-largura mundo, y = tipo, z = selecionada). 6 vertices por icone.
        struct IconVertex { f32 Pos[3]; f32 Color[3]; f32 Corner[2]; f32 Misc[3]; };
        std::vector<IconVertex> IconVerts;

        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> LinePSO;
        ComPtr<ID3D12PipelineState> TriPSO;
        ComPtr<ID3D12PipelineState> LineOccPSO;
        ComPtr<ID3D12PipelineState> IconPSO;
        ComPtr<ID3D12Resource>      CB;      u8* MappedCB     = nullptr;
        ComPtr<ID3D12Resource>      VB;      u8* MappedVB     = nullptr;
        ComPtr<ID3D12Resource>      IconVB;  u8* MappedIconVB = nullptr;
        bool                        Initialized = false;
        // Arma/desarma o aviso de estouro do orcamento: loga na ENTRADA do episodio, nao a cada
        // frame (a 200fps um log por frame vira spam que esconde o resto).
        bool                        OverflowLogged = false;

        static constexpr u32 kMaxVerts = 4096;
        static constexpr u32 kMaxIconVerts = 256 * 6; // 256 icones/frame
    };
}
