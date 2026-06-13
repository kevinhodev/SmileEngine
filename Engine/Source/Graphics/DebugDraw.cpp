#include "Smile/Graphics/DebugDraw.h"
#include "Smile/Graphics/CommandQueue.h" 
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <algorithm>

namespace Smile {
    static constexpr u32 kFIF      = FCommandQueue::kFramesInFlight;
    static constexpr u32 kVBStride = sizeof(f32) * 6; 

    void FDebugDraw::Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat) {
        if (Initialized) return;
        BuildRootSignature(Device);
        BuildPSOs(Device, RTFormat);
        CreateBuffers(Device);
        Initialized = true;
        LogInfo("FDebugDraw (desenho 3D imediato p/ tooling do editor) inicializado");
    }

    void FDebugDraw::Line(const Vec3& A, const Vec3& B, const Vec3& C) {
        LineVerts.push_back({ { A.X, A.Y, A.Z }, { C.X, C.Y, C.Z } });
        LineVerts.push_back({ { B.X, B.Y, B.Z }, { C.X, C.Y, C.Z } });
    }
    void FDebugDraw::Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec3& Col) {
        TriVerts.push_back({ { A.X, A.Y, A.Z }, { Col.X, Col.Y, Col.Z } });
        TriVerts.push_back({ { B.X, B.Y, B.Z }, { Col.X, Col.Y, Col.Z } });
        TriVerts.push_back({ { C.X, C.Y, C.Z }, { Col.X, Col.Y, Col.Z } });
    }

    void FDebugDraw::BuildRootSignature(ID3D12Device* Device) {
        D3D12_ROOT_PARAMETER P{};
        P.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        P.Descriptor.ShaderRegister = 0; 
        P.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters = 1;
        Desc.pParameters   = &P;
        Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> Blob, Err;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
        if (FAILED(Hr)) {
            if (Err) LogError(std::string("DebugDraw Root Sig: ") + static_cast<const char*>(Err->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                 IID_PPV_ARGS(&RootSig)));
    }

    void FDebugDraw::BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat) {
        auto VS = LoadShaderBytecode("DebugDraw.vs_6_0.cso");
        auto PS = LoadShaderBytecode("DebugDraw.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = FALSE; 
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = RTFormat;
        PSODesc.SampleDesc            = { 1, 0 };

        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&LinePSO)));

        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TriPSO)));
    }

    void FDebugDraw::CreateBuffers(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES UploadHeap{}; UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE NoRead{ 0, 0 };

        Desc.Width = 256ull * kFIF; 
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));

        Desc.Width = static_cast<u64>(kMaxVerts) * kVBStride * kFIF; 
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VB)));
        SMILE_HR(VB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedVB)));
    }

    void FDebugDraw::Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                            D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height) {
        if (!Initialized || Empty()) return;
        const u32 Slot = FrameSlot % kFIF;

        u32 numLine = static_cast<u32>(LineVerts.size());
        u32 numTri  = static_cast<u32>(TriVerts.size());
        if (numLine + numTri > kMaxVerts) { 
            if (numLine > kMaxVerts) numLine = kMaxVerts;
            numTri = std::min(numTri, kMaxVerts - numLine);
        }

        u8* VBSlot = MappedVB + static_cast<size_t>(Slot) * kMaxVerts * kVBStride;
        if (numLine) std::memcpy(VBSlot, LineVerts.data(), numLine * kVBStride);
        if (numTri)  std::memcpy(VBSlot + numLine * kVBStride, TriVerts.data(), numTri * kVBStride);
        std::memcpy(MappedCB + static_cast<size_t>(Slot) * 256, &ViewProj, sizeof(Mat44));

        CmdList->OMSetRenderTargets(1, &BackbufferRTV, FALSE, nullptr);
        D3D12_VIEWPORT VP{}; VP.Width = static_cast<FLOAT>(Width); VP.Height = static_cast<FLOAT>(Height);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sci{}; Sci.right = static_cast<LONG>(Width); Sci.bottom = static_cast<LONG>(Height);
        CmdList->RSSetViewports(1, &VP);
        CmdList->RSSetScissorRects(1, &Sci);

        CmdList->SetGraphicsRootSignature(RootSig.Get());
        CmdList->SetGraphicsRootConstantBufferView(0, CB->GetGPUVirtualAddress() + static_cast<u64>(Slot) * 256);

        D3D12_VERTEX_BUFFER_VIEW VBV{};
        VBV.BufferLocation = VB->GetGPUVirtualAddress() + static_cast<u64>(Slot) * kMaxVerts * kVBStride;
        VBV.StrideInBytes  = kVBStride;
        VBV.SizeInBytes    = (numLine + numTri) * kVBStride;
        CmdList->IASetVertexBuffers(0, 1, &VBV);

        if (numLine) {
            CmdList->SetPipelineState(LinePSO.Get());
            CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            CmdList->DrawInstanced(numLine, 1, 0, 0);
        }
        if (numTri) {
            CmdList->SetPipelineState(TriPSO.Get());
            CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            CmdList->DrawInstanced(numTri, 1, numLine, 0); 
        }
    }
}
