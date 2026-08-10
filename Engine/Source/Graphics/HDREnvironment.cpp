#include "Smile/Graphics/HDREnvironment.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <iterator>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#include <stb/stb_image.h>

#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        void DispatchCubePass(ID3D12GraphicsCommandList* CmdList,
                              FTextureSRVHeap& SRVHeap,
                              const FComputePipeline& Pipeline,
                              const u32* Constants, u32 NumConstants,
                              u32 SourceSRVSlot, u32 DestUAVSlot,
                              u32 OutputSize) {
            Pipeline.Bind(CmdList);
            CmdList->SetComputeRoot32BitConstants(0, NumConstants, Constants, 0);
            CmdList->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(SourceSRVSlot));
            CmdList->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(DestUAVSlot));
            const u32 Groups = (OutputSize + 7) / 8;
            CmdList->Dispatch(Groups, Groups, 6);
        }

        void DispatchLutPass(ID3D12GraphicsCommandList* CmdList,
                             FTextureSRVHeap& SRVHeap,
                             const FComputePipeline& Pipeline,
                             const u32* Constants, u32 NumConstants,
                             u32 DummySRVSlot, u32 DestUAVSlot,
                             u32 OutputSize) {
            Pipeline.Bind(CmdList);
            CmdList->SetComputeRoot32BitConstants(0, NumConstants, Constants, 0);
            CmdList->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(DummySRVSlot));
            CmdList->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(DestUAVSlot));
            const u32 Groups = (OutputSize + 7) / 8;
            CmdList->Dispatch(Groups, Groups, 1);
        }
    }

    void FHDREnvironment::Initialize(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                      FTextureSRVHeap& _SRVHeap) {
        EnvCube.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                       kEnvCubeSize, kEnvCubeMips, true);
        IrradianceCube.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              kIrradianceSize, 1, true);
        SpecularCube.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            kSpecularSize, kSpecularMips, true);

        {
            BRDFLutResource = GpuResources::CreateTex2D(
                _Device, kBRDFLutSize, kBRDFLutSize, DXGI_FORMAT_R16G16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, EVramCategory::SceneTextures,
                nullptr, 1, 1, "LUT do BRDF");

            BRDFLutSRVSlot = _SRVHeap.Allocate(1);
            BRDFLutUAVSlot = _SRVHeap.Allocate(1);

            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                        = DXGI_FORMAT_R16G16_FLOAT;
            SRVDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.Texture2D.MipLevels           = 1;
            SRVDesc.Texture2D.MostDetailedMip     = 0;
            SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            _SRVHeap.CreateSRV(_Device, BRDFLutResource.Get(), SRVDesc, BRDFLutSRVSlot);

            D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
            UAVDesc.Format               = DXGI_FORMAT_R16G16_FLOAT;
            UAVDesc.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
            UAVDesc.Texture2D.MipSlice   = 0;
            UAVDesc.Texture2D.PlaneSlice = 0;
            _SRVHeap.CreateUAV(_Device, BRDFLutResource.Get(), UAVDesc, BRDFLutUAVSlot);
        }

        CreatePipelines(_Device);

        GenerateBRDFLut(_Device, _CmdQueue, _SRVHeap);

        const float Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        u32 EquirectSRV = UploadEquirect2D(_Device, _CmdQueue, _SRVHeap, Black, 1, 1);
        RunIBLChain(_Device, _CmdQueue, _SRVHeap, EquirectSRV);

        LogDebug("HDREnvironment inicializado (default env preto, BRDF LUT pronta)");
    }

    void FHDREnvironment::GenerateBRDFLut(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                           FTextureSRVHeap& _SRVHeap) {
        _CmdQueue.ResetForRecording();
        auto* CmdList = _CmdQueue.List();

        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CmdList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const u32 Constants[2] = { kBRDFLutSize, 1024u };
        DispatchLutPass(CmdList, _SRVHeap, BRDFLutPSO,
                        Constants, _countof(Constants),
                        EnvCube.SRVSlot(), BRDFLutUAVSlot, kBRDFLutSize);

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = BRDFLutResource.Get();
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CmdList->ResourceBarrier(1, &B);

        SMILE_HR(CmdList->Close());
        ID3D12CommandList* Lists[] = { CmdList };
        _CmdQueue.ExecuteAndSync(Lists, 1);
    }

    u32 FHDREnvironment::UploadEquirect2D(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                          FTextureSRVHeap& _SRVHeap,
                                          const float* _Pixels, u32 _Width, u32 _Height) {
        Equirect2DResource.Reset();

        // Desc guardado: o GetCopyableFootprints logo abaixo precisa dele p/ dimensionar o
        // staging do upload.
        const D3D12_RESOURCE_DESC Desc =
            GpuResources::Tex2DDesc(_Width, _Height, DXGI_FORMAT_R32G32B32A32_FLOAT);
        Equirect2DResource = GpuResources::CreateTex2D(
            _Device, _Width, _Height, DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
            EVramCategory::SceneTextures, nullptr, 1, 1, "HDRI equirretangular");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
        UINT NumRows = 0; UINT64 RowSize = 0; UINT64 TotalSize = 0;
        _Device->GetCopyableFootprints(&Desc, 0, 1, 0, &Layout, &NumRows, &RowSize, &TotalSize);

        // Staging do HDRI inteiro (um 4K equirect em RGBA32F passa de 100 MB). Fora do ring
        // pelo mesmo motivo do chunk de mesh: alocacao unica e grande, nao churn.
        const GpuResources::FUploadBuffer StagingBuffer =
            GpuResources::CreateUploadBuffer(_Device, TotalSize, 1, false);
        ComPtr<ID3D12Resource> Staging = StagingBuffer.Resource;
        u8* Mapped = StagingBuffer.Mapped;
        const u32 SrcRowPitchBytes = _Width * 4 * sizeof(float);
        for (u32 Row = 0; Row < _Height; ++Row) {
            const u8* Src = reinterpret_cast<const u8*>(_Pixels) + Row * SrcRowPitchBytes;
            u8*       Dst = Mapped + Layout.Offset + static_cast<UINT64>(Row) * Layout.Footprint.RowPitch;
            memcpy(Dst, Src, SrcRowPitchBytes);
        }
        Staging->Unmap(0, nullptr);

        _CmdQueue.ResetForRecording();
        auto* CmdList = _CmdQueue.List();

        D3D12_TEXTURE_COPY_LOCATION SrcLoc{};
        SrcLoc.pResource       = Staging.Get();
        SrcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        SrcLoc.PlacedFootprint = Layout;
        D3D12_TEXTURE_COPY_LOCATION DstLoc{};
        DstLoc.pResource        = Equirect2DResource.Get();
        DstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        DstLoc.SubresourceIndex = 0;
        CmdList->CopyTextureRegion(&DstLoc, 0, 0, 0, &SrcLoc, nullptr);

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Equirect2DResource.Get();
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CmdList->ResourceBarrier(1, &B);

        SMILE_HR(CmdList->Close());
        ID3D12CommandList* Lists[] = { CmdList };
        _CmdQueue.ExecuteAndSync(Lists, 1);

        if (Equirect2DSRVSlot == 0) {
            Equirect2DSRVSlot = _SRVHeap.Allocate(1);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        SRVDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels           = 1;
        SRVDesc.Texture2D.MostDetailedMip     = 0;
        SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, Equirect2DResource.Get(), SRVDesc, Equirect2DSRVSlot);

        return Equirect2DSRVSlot;
    }

    void FHDREnvironment::RunIBLChain(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                       FTextureSRVHeap& _SRVHeap, u32 _EquirectSRVSlot) {
        (void)_Device;
        _CmdQueue.ResetForRecording();
        auto* CmdList = _CmdQueue.List();

        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CmdList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        EnvCube       .Transition(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        IrradianceCube.Transition(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpecularCube  .Transition(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        {
            const u32 Constants[1] = { kEnvCubeSize };
            DispatchCubePass(CmdList, _SRVHeap, EquirectToCubePSO,
                             Constants, _countof(Constants),
                             _EquirectSRVSlot, EnvCube.UAVSlot(0), kEnvCubeSize);
        }

        EnvCube.TransitionMip(CmdList, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (u32 Mip = 1; Mip < kEnvCubeMips; ++Mip) {
            const u32 MipSize = kEnvCubeSize >> Mip;
            const u32 Constants[2] = { MipSize, Mip - 1 };
            DispatchCubePass(CmdList, _SRVHeap, MipGenPSO,
                             Constants, _countof(Constants),
                             EnvCube.SRVSlot(), EnvCube.UAVSlot(Mip), MipSize);
            EnvCube.TransitionMip(CmdList, Mip, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        {
            const u32 Constants[1] = { kIrradianceSize };
            DispatchCubePass(CmdList, _SRVHeap, IrradiancePSO,
                             Constants, _countof(Constants),
                             EnvCube.SRVSlot(), IrradianceCube.UAVSlot(0), kIrradianceSize);
        }
        IrradianceCube.Transition(CmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        for (u32 Mip = 0; Mip < kSpecularMips; ++Mip) {
            const u32 MipSize = kSpecularSize >> Mip;
            const float Roughness = (kSpecularMips == 1)
                                  ? 0.0f
                                  : static_cast<float>(Mip) / static_cast<float>(kSpecularMips - 1);

            u32 Constants[4];
            Constants[0] = MipSize;
            Constants[1] = kEnvCubeSize;
            std::memcpy(&Constants[2], &Roughness, sizeof(float));
            Constants[3] = kSpecularSampleCount;
            DispatchCubePass(CmdList, _SRVHeap, SpecularPSO,
                             Constants, _countof(Constants),
                             EnvCube.SRVSlot(), SpecularCube.UAVSlot(Mip), MipSize);
        }
        SpecularCube.Transition(CmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        EnvCube.Transition(CmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        SMILE_HR(CmdList->Close());
        ID3D12CommandList* Lists[] = { CmdList };
        _CmdQueue.ExecuteAndSync(Lists, 1);
    }

    bool FHDREnvironment::LoadFromFile(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                        FTextureSRVHeap& _SRVHeap, const std::wstring& _Path) {
        std::string UTF8;
        {
            int Size = WideCharToMultiByte(CP_UTF8, 0, _Path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            UTF8.resize(Size > 0 ? Size - 1 : 0);
            if (Size > 0) WideCharToMultiByte(CP_UTF8, 0, _Path.c_str(), -1, UTF8.data(), Size, nullptr, nullptr);
        }

        int Width = 0, Height = 0, Channels = 0;
        float* Pixels = stbi_loadf(UTF8.c_str(), &Width, &Height, &Channels, 4);
        if (!Pixels) {
            LogError(std::string("Falha ao carregar HDR: ") + UTF8 + " — " + stbi_failure_reason());
            return false;
        }
        LogInfo("HDR loaded: " + std::to_string(Width) + "x" + std::to_string(Height) +
                " R32G32B32A32_FLOAT");

        u32 EquirectSRV = UploadEquirect2D(_Device, _CmdQueue, _SRVHeap,
                                            Pixels, static_cast<u32>(Width), static_cast<u32>(Height));
        stbi_image_free(Pixels);

        RunIBLChain(_Device, _CmdQueue, _SRVHeap, EquirectSRV);
        HDRLoaded = true;
        LogDebug("IBL chain generated (env cube 512^2, irradiance 32^2, specular 128^2 x 7 mips)");
        return true;
    }

    void FHDREnvironment::CreatePipelines(ID3D12Device* _Device) {
        EquirectToCubePSO.Initialize(_Device, "EquirectToCube.cs_6_0.cso",        false);
        MipGenPSO        .Initialize(_Device, "MipGen.cs_6_0.cso",                true);
        IrradiancePSO    .Initialize(_Device, "IrradianceConvolution.cs_6_0.cso", true);
        SpecularPSO      .Initialize(_Device, "SpecularPrefilter.cs_6_0.cso",     true);
        BRDFLutPSO       .Initialize(_Device, "BRDFIntegration.cs_6_0.cso",       false);
    }


    FPassShaderStems FHDREnvironment::ShaderStems() const {
        static const char* const kStems[] = { "EquirectToCube.cs", "MipGen.cs", "IrradianceConvolution.cs", "SpecularPrefilter.cs", "BRDFIntegration.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FHDREnvironment::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
