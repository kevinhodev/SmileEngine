#pragma once

#include <d3d12.h>
#include <vector>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

namespace Smile {
    // Serviço de desenho 3D IMEDIATO (estilo DisplayContext da Cry / FPrimitiveDrawInterface da
    // Unreal): qualquer sistema — tipicamente o EDITOR — submete linhas/triângulos em world-space
    // por frame; a Engine batcha e desenha POS-tonemap no backbuffer LDR, SEM depth (sempre por
    // cima). Isso permite que a TOOLING do editor (gizmo etc.) viva no editor e só peça desenho à
    // Engine, em vez de embutir a lógica no Renderer. VB/CB double-buffered (sem race). O Render
    // projeta com a ViewProj do frame — a geometria submetida é world-space, então não há lag.
    class FDebugDraw {
    public:
        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        bool IsInitialized() const { return Initialized; }

        // --- Submissao (CPU, antes do Render). Cor RGB linear-ish 0..1. ---
        void Line(const Vec3& A, const Vec3& B, const Vec3& Color);
        void Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec3& Color);
        void Clear() { LineVerts.clear(); TriVerts.clear(); }
        bool Empty() const { return LineVerts.empty() && TriVerts.empty(); }

        // Desenha o acumulado no backbuffer (ja em RENDER_TARGET). NAO limpa (o caller chama Clear
        // apos consumir). FrameSlot indexa o CB/VB double-buffered.
        void Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                    D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height);

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        void CreateBuffers(ID3D12Device* Device);

        struct DDVertex { f32 Pos[3]; f32 Color[3]; }; // layout POSITION + COLOR (24 bytes)
        std::vector<DDVertex> LineVerts;
        std::vector<DDVertex> TriVerts;

        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> LinePSO; // line list
        ComPtr<ID3D12PipelineState> TriPSO;  // triangle list
        ComPtr<ID3D12Resource>      CB;      u8* MappedCB = nullptr; // Mat44 ViewProj, double-buffered
        ComPtr<ID3D12Resource>      VB;      u8* MappedVB = nullptr; // verts dinamicos, double-buffered
        bool                        Initialized = false;

        static constexpr u32 kMaxVerts = 4096; // por frame, folgado (gizmo + futuros overlays)
    };
}
