#include "Smile/Graphics/RainWetness.h"
#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>

namespace Smile {

    void FRainWetness::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                  u32 _Width, u32 _Height) {
        if (IsInitialized()) return;
        BuildRootSignature(_Device);
        BuildPSO(_Device);
        CreateConstantBuffer(_Device);
        CreateScratch(_Device, _SRVHeap, _Width, _Height);
        LogInfo("Chuva deferred (wetness no G-buffer) inicializada");
    }

    void FRainWetness::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                              u32 _Width, u32 _Height) {
        if (!IsInitialized()) return;
        CreateScratch(_Device, _SRVHeap, _Width, _Height);
    }

    void FRainWetness::Recreate(ID3D12Device* _Device) {
        if (!IsInitialized()) return;
        PSO.Reset();
        BuildPSO(_Device);
    }

    void FRainWetness::BuildRootSignature(ID3D12Device* _Device) {
        // [0] CBV b0 | [1] tabela [t0,t1] = scratch A/B (contiguos) | [2] tabela t2 = depth
        D3D12_DESCRIPTOR_RANGE ScratchRange{};
        ScratchRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ScratchRange.NumDescriptors                    = 2;
        ScratchRange.BaseShaderRegister                = 0;
        ScratchRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE DepthRange = ScratchRange;
        DepthRange.NumDescriptors     = 1;
        DepthRange.BaseShaderRegister = 2;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &ScratchRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &DepthRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters = _countof(RootParams);
        Desc.pParameters   = RootParams;
        Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("RainWetness root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FRainWetness::BuildPSO(ID3D12Device* _Device) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso"); // fullscreen tri com uv
        auto PS = LoadShaderBytecode("RainWetness.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        // Sem blend: o PS reescreve A/B por inteiro; pixel seco/ceu da discard e preserva
        // o conteudo original do geometry pass.
        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        Blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 2;
        PSODesc.RTVFormats[0]         = FGBuffer::kFormatA;
        PSODesc.RTVFormats[1]         = FGBuffer::kFormatB;
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FRainWetness::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                                sizeof(RainWetnessConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Ptr));
        MappedBase = reinterpret_cast<u8*>(Ptr);
        RainWetnessConstants Zero{};
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedBase + static_cast<size_t>(i) * sizeof(RainWetnessConstants),
                        &Zero, sizeof(RainWetnessConstants));
    }

    void FRainWetness::CreateScratch(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _Width, u32 _Height) {
        if (_Width == 0 || _Height == 0) return;
        if (_Width == ScratchW && _Height == ScratchH && ScratchA) return;
        ScratchW = _Width;
        ScratchH = _Height;

        if (ScratchSRVBase == 0xFFFFFFFFu)
            ScratchSRVBase = _SRVHeap.Allocate(2); // [A,B] contiguos = uma tabela

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto Make = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& _Out, DXGI_FORMAT _Fmt, u32 _Slot) {
            _Out.Reset();

            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = _Width;
            Desc.Height           = _Height;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = _Fmt;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            // Ciclo de estados deterministico por Execute: COPY_DEST -> PSR -> COPY_DEST.
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&_Out)));

            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = _Fmt;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Texture2D.MipLevels     = 1;
            _SRVHeap.CreateSRV(_Device, _Out.Get(), SRVDesc, _Slot);
        };

        Make(ScratchA, FGBuffer::kFormatA, ScratchSRVBase);
        Make(ScratchB, FGBuffer::kFormatB, ScratchSRVBase + 1);
    }

    void FRainWetness::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProjFull,
                                      const Vec3& _CameraWorldPos, f32 _TimeSec,
                                      const FWeather& _Weather) {
        FrameSlot = _FrameSlot;
        if (!MappedBase) return;

        RainWetnessConstants c{};
        c.InvViewProj    = _InvViewProjFull;
        c.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, _TimeSec };
        c.RainParams0    = { _Weather.RainAmount, _Weather.PuddleAmount,
                             _Weather.RippleStrength, _Weather.WetDarkening };
        const f32 Scale  = _Weather.PuddleScale > 1e-3f ? _Weather.PuddleScale : 1.0f;
        c.RainParams1    = { 1.0f / Scale, 0.0f, 0.0f, 0.0f };

        std::memcpy(MappedBase + static_cast<size_t>(FrameSlot) * sizeof(RainWetnessConstants),
                    &c, sizeof(RainWetnessConstants));
    }

    void FRainWetness::Execute(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                               FGBuffer& _GBuffer, ID3D12Resource* _DepthBuffer, u32 _DepthSRVSlot,
                               u32 _Width, u32 _Height) {
        if (!IsInitialized() || !ScratchA) return;

        auto Barrier = [&](ID3D12Resource* _R, D3D12_RESOURCE_STATES _Before,
                           D3D12_RESOURCE_STATES _After) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = _R;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = _Before;
            B.Transition.StateAfter  = _After;
            _Cmd->ResourceBarrier(1, &B);
        };

        // 1) Copia A/B pro scratch (Cry copia os RTs pelo mesmo motivo: nao da pra ler e
        //    escrever o mesmo RT no fullscreen pass).
        _GBuffer.TransitionToRead(_Cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        _Cmd->CopyResource(ScratchA.Get(), _GBuffer.Resource(0));
        _Cmd->CopyResource(ScratchB.Get(), _GBuffer.Resource(1));
        _GBuffer.TransitionToWrite(_Cmd);

        Barrier(ScratchA.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(ScratchB.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(_DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 2) Fullscreen: reescreve GBufferA/B com a versao molhada.
        D3D12_CPU_DESCRIPTOR_HANDLE RTVs[2] = { _GBuffer.RTVHandle(0), _GBuffer.RTVHandle(1) };
        _Cmd->OMSetRenderTargets(2, RTVs, FALSE, nullptr);

        D3D12_VIEWPORT Viewport{ 0.0f, 0.0f,
                                 static_cast<f32>(_Width), static_cast<f32>(_Height),
                                 0.0f, 1.0f };
        D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(_Width), static_cast<LONG>(_Height) };
        _Cmd->RSSetViewports(1, &Viewport);
        _Cmd->RSSetScissorRects(1, &Scissor);

        _Cmd->SetGraphicsRootSignature(RootSig.Get());
        _Cmd->SetPipelineState(PSO.Get());
        _Cmd->SetGraphicsRootConstantBufferView(0, CBAddr());
        _Cmd->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(ScratchSRVBase));
        _Cmd->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _Cmd->IASetVertexBuffers(0, 0, nullptr);
        _Cmd->IASetIndexBuffer(nullptr);
        _Cmd->DrawInstanced(3, 1, 0, 0);

        // 3) Restaura as pre-condicoes (depth de volta a DEPTH_WRITE, scratch pro proximo frame).
        Barrier(_DepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
        Barrier(ScratchA.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(ScratchB.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
    }
}
