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
        void Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec3& Color);
        void Clear() { LineVerts.clear(); TriVerts.clear(); }
        bool Empty() const { return LineVerts.empty() && TriVerts.empty(); }

        void Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                    D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height);

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        void CreateBuffers(ID3D12Device* Device);

        struct DDVertex { f32 Pos[3]; f32 Color[3]; }; 
        std::vector<DDVertex> LineVerts;
        std::vector<DDVertex> TriVerts;

        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> LinePSO; 
        ComPtr<ID3D12PipelineState> TriPSO;  
        ComPtr<ID3D12Resource>      CB;      u8* MappedCB = nullptr; 
        ComPtr<ID3D12Resource>      VB;      u8* MappedVB = nullptr; 
        bool                        Initialized = false;

        static constexpr u32 kMaxVerts = 4096; 
    };
}
