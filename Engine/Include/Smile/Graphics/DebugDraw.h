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
        void Clear() { LineVerts.clear(); TriVerts.clear(); LineOccVerts.clear(); }
        bool Empty() const { return LineVerts.empty() && TriVerts.empty() && LineOccVerts.empty(); }
        bool HasOccluded() const { return !LineOccVerts.empty(); }

        // DepthSRV (shader-visible, res de render, estado PIXEL_SHADER_RESOURCE) habilita as
        // linhas ocluiveis; ptr 0 as descarta no frame (fallback seguro).
        void Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                    D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height,
                    D3D12_GPU_DESCRIPTOR_HANDLE DepthSRV = {});

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        void CreateBuffers(ID3D12Device* Device);

        struct DDVertex { f32 Pos[3]; f32 Color[3]; };
        std::vector<DDVertex> LineVerts;
        std::vector<DDVertex> TriVerts;
        std::vector<DDVertex> LineOccVerts;

        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> LinePSO;
        ComPtr<ID3D12PipelineState> TriPSO;
        ComPtr<ID3D12PipelineState> LineOccPSO;
        ComPtr<ID3D12Resource>      CB;      u8* MappedCB = nullptr;
        ComPtr<ID3D12Resource>      VB;      u8* MappedVB = nullptr;
        bool                        Initialized = false;

        static constexpr u32 kMaxVerts = 4096;
    };
}
