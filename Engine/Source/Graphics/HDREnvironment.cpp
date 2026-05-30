#include "Smile/Graphics/HDREnvironment.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#include <stb/stb_image.h>

#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        // Helper: writes the per-pass root constants and dispatches a 2D grid
        // over the 6 cube faces.
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
        // Cubemaps (all start in UNORDERED_ACCESS).
        EnvCube.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                       kEnvCubeSize, kEnvCubeMips, /*AllowUAV=*/true);
        IrradianceCube.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              kIrradianceSize, 1, /*AllowUAV=*/true);
        SpecularCube.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            kSpecularSize, kSpecularMips, /*AllowUAV=*/true);

        // BRDF LUT 2D
        {
            D3D12_RESOURCE_DESC LutDesc{};
            LutDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            LutDesc.Width            = kBRDFLutSize;
            LutDesc.Height           = kBRDFLutSize;
            LutDesc.DepthOrArraySize = 1;
            LutDesc.MipLevels        = 1;
            LutDesc.Format           = DXGI_FORMAT_R16G16_FLOAT;
            LutDesc.SampleDesc       = { 1, 0 };
            LutDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            LutDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &LutDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&BRDFLutResource)));

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

        // Compute pipelines.
        EquirectToCubePSO.Initialize(_Device, "EquirectToCube.cs_6_0.cso",  /*SourceIsCube=*/false);
        MipGenPSO        .Initialize(_Device, "MipGen.cs_6_0.cso",                /*SourceIsCube=*/true);
        IrradiancePSO    .Initialize(_Device, "IrradianceConvolution.cs_6_0.cso", /*SourceIsCube=*/true);
        SpecularPSO      .Initialize(_Device, "SpecularPrefilter.cs_6_0.cso",     /*SourceIsCube=*/true);
        BRDFLutPSO       .Initialize(_Device, "BRDFIntegration.cs_6_0.cso",       /*SourceIsCube=*/false);

        // One-shot bake of BRDF LUT.
        GenerateBRDFLut(_Device, _CmdQueue, _SRVHeap);

        // Default neutral environment: 1x1 black equirect so the PS always has
        // valid SRVs even before the user picks an HDR file.
        const float Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        u32 EquirectSRV = UploadEquirect2D(_Device, _CmdQueue, _SRVHeap, Black, 1, 1);
        RunIBLChain(_Device, _CmdQueue, _SRVHeap, EquirectSRV);

        LogInfo("HDREnvironment inicializado (default env preto, BRDF LUT pronta)");
    }

    void FHDREnvironment::GenerateBRDFLut(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                           FTextureSRVHeap& _SRVHeap) {
        _CmdQueue.ResetForRecording();
        auto* CmdList = _CmdQueue.List();

        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CmdList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        // The BRDF CS declares Unused t0 — bind any valid SRV. We don't have one
        // yet (equirect uploaded after this), so temporarily bind the env cube
        // SRV: it's allocated, formatted as cube, but DXC's view-dim mismatch
        // only matters if the shader actually reads it, which it doesn't.
        const u32 Constants[2] = { kBRDFLutSize, 1024u /*NumSamples*/ };
        DispatchLutPass(CmdList, _SRVHeap, BRDFLutPSO,
                        Constants, _countof(Constants),
                        EnvCube.SRVSlot(), BRDFLutUAVSlot, kBRDFLutSize);

        // LUT UAV → SRV.
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
        // (Re)create the equirect resource at the requested size.
        Equirect2DResource.Reset();

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES DefaultHeap{}; DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&Equirect2DResource)));

        // Staging.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
        UINT NumRows = 0; UINT64 RowSize = 0; UINT64 TotalSize = 0;
        _Device->GetCopyableFootprints(&Desc, 0, 1, 0, &Layout, &NumRows, &RowSize, &TotalSize);

        D3D12_RESOURCE_DESC BufDesc{};
        BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufDesc.Width            = TotalSize;
        BufDesc.Height           = 1;
        BufDesc.DepthOrArraySize = 1;
        BufDesc.MipLevels        = 1;
        BufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        BufDesc.SampleDesc       = { 1, 0 };
        BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BufDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES UploadHeap{}; UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> Staging;
        SMILE_HR(_Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Staging)));

        u8* Mapped = nullptr;
        SMILE_HR(Staging->Map(0, nullptr, reinterpret_cast<void**>(&Mapped)));
        const u32 SrcRowPitchBytes = _Width * 4 * sizeof(float);
        for (u32 Row = 0; Row < _Height; ++Row) {
            const u8* Src = reinterpret_cast<const u8*>(_Pixels) + Row * SrcRowPitchBytes;
            u8*       Dst = Mapped + Layout.Offset + static_cast<UINT64>(Row) * Layout.Footprint.RowPitch;
            memcpy(Dst, Src, SrcRowPitchBytes);
        }
        Staging->Unmap(0, nullptr);

        // Copy + transition to SHADER_RESOURCE.
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

        // Allocate the SRV slot lazily (kept across swaps so the PS bindings stay valid).
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

        // Bring all three cubes (every subresource) into UAV state. The
        // Transition helper tracks per-subresource state — no-op on the very
        // first run (already UAV from Create), barriers issued on subsequent
        // swaps when subresources are split across SRV/UAV (mip-gen ping-pong).
        EnvCube       .Transition(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        IrradianceCube.Transition(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpecularCube  .Transition(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // 1) Equirect → cube (writes env cube mip 0)
        {
            const u32 Constants[1] = { kEnvCubeSize };
            DispatchCubePass(CmdList, _SRVHeap, EquirectToCubePSO,
                             Constants, _countof(Constants),
                             _EquirectSRVSlot, EnvCube.UAVSlot(0), kEnvCubeSize);
        }

        // 2) Generate env cube mip chain. Each pass reads mip i-1 (SRV) and
        //    writes mip i (UAV) via per-mip subresource transitions. The cube
        //    SRV views the full chain — we sample at SrcMip via root constant.
        EnvCube.TransitionMip(CmdList, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (u32 Mip = 1; Mip < kEnvCubeMips; ++Mip) {
            const u32 MipSize = kEnvCubeSize >> Mip;
            const u32 Constants[2] = { MipSize, Mip - 1 /*SrcMip*/ };
            DispatchCubePass(CmdList, _SRVHeap, MipGenPSO,
                             Constants, _countof(Constants),
                             EnvCube.SRVSlot(), EnvCube.UAVSlot(Mip), MipSize);
            // Just-written mip → SRV so the next iteration (or specular pass) reads it.
            EnvCube.TransitionMip(CmdList, Mip, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        // Whole cube now in NON_PIXEL_SHADER_RESOURCE across all mips.

        // 3) Irradiance convolution
        {
            const u32 Constants[1] = { kIrradianceSize };
            DispatchCubePass(CmdList, _SRVHeap, IrradiancePSO,
                             Constants, _countof(Constants),
                             EnvCube.SRVSlot(), IrradianceCube.UAVSlot(0), kIrradianceSize);
        }
        IrradianceCube.Transition(CmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 4) Specular prefilter — now benefits from env mip chain via Karis
        //    solid-angle mip selection inside the shader.
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

        // env cube → PIXEL_SHADER_RESOURCE so the skybox PS can sample it.
        EnvCube.Transition(CmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        SMILE_HR(CmdList->Close());
        ID3D12CommandList* Lists[] = { CmdList };
        _CmdQueue.ExecuteAndSync(Lists, 1);
    }

    bool FHDREnvironment::LoadFromFile(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                        FTextureSRVHeap& _SRVHeap, const std::wstring& _Path) {
        // stb_image takes UTF-8 paths on Windows.
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
        LogInfo("IBL chain generated (env cube 512^2, irradiance 32^2, specular 128^2 x 7 mips)");
        return true;
    }
}
