#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/Barriers.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <exception>
#include <functional>
#include <cmath>

namespace Smile {
    namespace {
        f32 Halton(u32 i, u32 b) {
            f32 f = 1.0f, r = 0.0f;
            while (i > 0) { f /= static_cast<f32>(b); r += f * static_cast<f32>(i % b); i /= b; }
            return r;
        }
    }

    Renderer::Renderer() = default;
    Renderer::~Renderer() noexcept {
        try {
            Shutdown();
        } catch (const std::exception& Error) {
            LogError(std::string("Falha absorvida no shutdown do Renderer: ") + Error.what());
        } catch (...) {
            LogError("Falha desconhecida absorvida no shutdown do Renderer");
        }
    }

    void Renderer::Initialize(HWND _hWnd, u32 _Width, u32 _Height) {
        if (Initialized) return;

    #ifdef _DEBUG
        constexpr bool kDebugLayer = true;
    #else
        constexpr bool kDebugLayer = false;
    #endif

        // Streamline (DLSS) em manual hooking: inicializar ANTES de criar o device D3D12.
        FDlssPass::InitStreamline();
        Device.Initialize(kDebugLayer);
        FDlssPass::SetDevice(Device.Native());   // avisa o SL do device (manual hooking)
        CommandQueue.Initialize(Device.Native(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        UploadQueue.Initialize(Device.Native());
        ComputeQueue.Initialize(Device.Native());
        GpuProfiler.Initialize(Device.Native(), CommandQueue.Native(),
                               FCommandQueue::kFramesInFlight);
        GpuProfilerCompute.Initialize(Device.Native(), ComputeQueue.Native(),
                                      FAsyncComputeQueue::kSlots);
        SwapChain.Initialize(Device.GetFactory(),
                             CommandQueue.Native(),
                             Device.Native(),
                             _hWnd, _Width, _Height,
                             Device.TearingSupported());
        SRVHeap.Initialize(Device.Native());
        PipelineState.Initialize(Device.Native());

        CreateDepthBuffer();
        CreateNormalBuffer();
        GBuffer.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        GBuffer.WriteDepthSRV(Device.Native(), SRVHeap, DepthBuffer.Get()); 
        GBufferDebugPass.Initialize(Device.Native(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        CreateHDRBuffers();
        CreateVelocityBuffer();
        CreateSceneCopies();
        CreateConstantBuffer();
        CreateDefaultMaterial();
        BuildDefaultScene();

        HDREnv.Initialize(Device.Native(), CommandQueue, SRVHeap);
        CreateIBLDescriptorTable();

        Skybox.Initialize(Device.Native(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        Atmosphere.Initialize(Device.Native(), CommandQueue, UploadQueue, SRVHeap,
                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        CloudNoise.Initialize(Device.Native(), CommandQueue, SRVHeap);
        VolumetricClouds.Initialize(Device.Native(), SRVHeap, CloudNoise,
                                    Atmosphere.TransmittanceSRV(), Atmosphere.MultiScatterSRV(),
                                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
                                    SwapChain.GetWidth(), SwapChain.GetHeight());

        Ocean[0].ConfigureCascade(1337u, 1.0f,     2.0f, 129.0f);
        Ocean[1].ConfigureCascade(1338u, 0.4082f,  2.0f, 12.0f);
        Ocean[2].ConfigureCascade(1339u, 0.2041f,  2.0f, 8.0f);

        for (u32 c = 0; c < kOceanCascades; ++c)
            Ocean[c].Initialize(Device.Native(), SRVHeap);
        Water.Initialize(Device.Native(),
                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
                         DXGI_FORMAT_R16G16_FLOAT);

        Fog.Initialize(Device.Native(), DXGI_FORMAT_R16G16B16A16_FLOAT);

        VolumetricFog.Initialize(Device.Native(), SRVHeap);

        SunShafts.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        RainWetness.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        SunShadows.Initialize(Device.Native(), SRVHeap);
        LocalShadows.Initialize(Device.Native(), SRVHeap);

        Terrain.Initialize(Device.Native());

        PostProcessor.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        ObjectPicker.Initialize(Device.Native(), SwapChain.GetWidth(), SwapChain.GetHeight());

        SelectionOutline.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        DebugDraw.Initialize(Device.Native(), FSwapChain::kFormat);

        TemporalAA.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());
        TemporalAA.SetupInputs(Device.Native(), SRVHeap, HDRColorBuffer.Get(), DepthBuffer.Get(), VelocityBuffer.Get());

        Fsr.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                        SwapChain.GetWidth(), SwapChain.GetHeight());
        Dlss.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                        SwapChain.GetWidth(), SwapChain.GetHeight());

        Flicker.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        AO.Initialize(Device.Native());
        AO.SetupForResize(Device.Native(), SRVHeap, DepthSRVSlot, NormalSRVSlot,
                          SwapChain.GetWidth(), SwapChain.GetHeight());

        HiZ.Initialize(Device.Native());
        HiZ.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        if (Device.RaytracingSupported()) {
            DDGI.Initialize(Device.Native());
            ReSTIRGI.Initialize(Device.Native());
            Nrd.Initialize(Device.Native());
            Reflections.Initialize(Device.Native());
            DDGIDebugPass.Initialize(Device.Native(),
                                     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);
        }

        BuildRaytracingScene();

        Initialized = true;
        LogInfo("Renderer Inicializado");
    }

    void Renderer::BuildRaytracingScene() {
        if (!Device.RaytracingSupported()) return;
        RaytracingScene.Build(Device, CommandQueue, SRVHeap, Scene);
        TlasTransformsVersion = Scene.TransformsVersion();
    }

    void Renderer::SetupGIForScene(const Vec3& _AABBMin, const Vec3& _AABBMax) {
        if (!Device.RaytracingSupported() || !RaytracingScene.IsBuilt()) return;
        if (!Atmosphere.IsInitialized()) return; 
        DDGI.SetupForScene(Device.Native(), CommandQueue, SRVHeap, Scene, _AABBMin, _AABBMax,
                           RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV());

        SetupReflectionsForScene();

        DDGIDebugPass.SetupForScene(Device.Native(), SRVHeap, DDGI.NumProbesCount());
    }

    void Renderer::CreateIBLDescriptorTable() {
        IBLTableStart = SRVHeap.Allocate(3);

        D3D12_CPU_DESCRIPTOR_HANDLE DstStart = SRVHeap.CpuHandle(IBLTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Sources[3] = {
            SRVHeap.CpuHandleStaging(HDREnv.IrradianceSRV()),
            SRVHeap.CpuHandleStaging(HDREnv.SpecularSRV()),
            SRVHeap.CpuHandleStaging(HDREnv.BRDFLutSRV()),
        };
        UINT DstCount = 3;
        UINT SrcCounts[3] = { 1, 1, 1 };
        Device.Native()->CopyDescriptors(1, &DstStart, &DstCount,
                                          3, Sources, SrcCounts,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    bool Renderer::LoadHDREnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        CommandQueue.Flush();
        return HDREnv.LoadFromFile(Device.Native(), CommandQueue, SRVHeap, _Path);
    }

    void Renderer::CreateDefaultMaterial() {
        auto* Dev = Device.Native();

        TexDefaultWhite  = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::White);
        TexDefaultNormal = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::FlatNormal);
        TexDefaultORM    = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::ORM);
        TexDefaultBlack  = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::Black);

        DefaultMaterial.Albedo            = &TexDefaultWhite;
        DefaultMaterial.Normal            = &TexDefaultNormal;
        DefaultMaterial.MetallicRoughness = &TexDefaultORM;
        DefaultMaterial.AO                = &TexDefaultWhite;
        DefaultMaterial.Emissive          = &TexDefaultBlack;
        DefaultMaterial.Height            = &TexDefaultWhite; 
        DefaultMaterial.Metalness         = &TexDefaultWhite; 
        DefaultMaterial.Roughness         = &TexDefaultWhite; 

        DefaultMaterial.Constants.BaseColorFactor  = { 0.8f, 0.8f, 0.8f, 1.0f };
        DefaultMaterial.Constants.MetallicFactor   = 0.0f;
        DefaultMaterial.Constants.RoughnessFactor  = 0.5f;

        DefaultMaterial.Finalize(Dev, SRVHeap);
        ActiveMaterial = &DefaultMaterial;
    }

    void Renderer::SetMaterial(FMaterial* _Material) {
        ActiveMaterial = (_Material && _Material->IsFinalized()) ? _Material : &DefaultMaterial;
    }

    void Renderer::SetUseWater(bool _Use) {
        if (_Use && !UseWater)
            LogInfo("Oceano/agua ativados (FFT 256^2 + superficie)");
        UseWater = _Use;
    }

    void Renderer::BuildDefaultScene() {
        FGpuMesh* Sphere = Scene.AddMesh(Device.Native(), FMesh::CreateSphere());

        FRenderable Renderable;
        Renderable.Name     = "Sphere";
        Renderable.Mesh     = Sphere;
        Renderable.Material = nullptr;
        Scene.AddRenderable(Renderable);
    }

    void Renderer::CreateConstantBuffer() {
        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        ResourceDesc.Width            = sizeof(FrameConstants);
        ResourceDesc.Height           = 1;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        D3D12_RANGE NoReadRange{ 0, 0 };
        void* Ptr = nullptr;

        ResourceDesc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(FrameConstants);
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&ConstantBuffer)));
        SMILE_HR(ConstantBuffer->Map(0, &NoReadRange, &Ptr));
        MappedFrameBase = reinterpret_cast<u8*>(Ptr);

        ResourceDesc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                             kMaxLights * sizeof(FGPULight);
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&LightBuffer)));
        SMILE_HR(LightBuffer->Map(0, &NoReadRange, &Ptr));
        MappedLightBase = reinterpret_cast<u8*>(Ptr);

        ResourceDesc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                             kMaxLights * sizeof(FGPULightGI);
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&GILightBuffer)));
        SMILE_HR(GILightBuffer->Map(0, &NoReadRange, &Ptr));
        MappedGILightBase = reinterpret_cast<u8*>(Ptr);

        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) {
            GILightSRVSlot[i] = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<UINT64>(i) * kMaxLights;
            Srv.Buffer.NumElements         = kMaxLights;
            Srv.Buffer.StructureByteStride = sizeof(FGPULightGI);
            SRVHeap.CreateSRV(Device.Native(), GILightBuffer.Get(), Srv, GILightSRVSlot[i]);
        }

        RecreateObjectCB();
    }

    void Renderer::CreateDepthBuffer() {
        UINT Width  = RenderWidth();
        UINT Height = RenderHeight();
        if (Width == 0 || Height == 0) return;

        if (!DSVHeap.Native())
            DSVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

        DepthBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format               = DXGI_FORMAT_D32_FLOAT;
        ClearValue.DepthStencil.Depth   = kClearDepth; 
        ClearValue.DepthStencil.Stencil = 0;

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &ClearValue,
            IID_PPV_ARGS(&DepthBuffer)));
        VramTracker::Register(DepthBuffer.Get(), EVramCategory::RenderTargets);

        D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        DSVDesc.Texture2D.MipSlice = 0;
        Device.Native()->CreateDepthStencilView(DepthBuffer.Get(), &DSVDesc, DSVHeap.CpuHandle(0));

        if (DepthSRVSlot == kInvalidSlot)
            DepthSRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Texture2D.MipLevels       = 1;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        SRVHeap.CreateSRV(Device.Native(), DepthBuffer.Get(), SRVDesc, DepthSRVSlot);
    }

    void Renderer::CreateNormalBuffer() {
        UINT Width  = RenderWidth();
        UINT Height = RenderHeight();
        if (Width == 0 || Height == 0) return;

        if (!NormalRTVHeap.Native())
            NormalRTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        NormalBuffer.Reset();

        const DXGI_FORMAT NormalFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = NormalFormat;
        ResourceDesc.SampleDesc       = { 1, 0 }; 
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format   = NormalFormat;
        ClearValue.Color[0] = 0.5f; ClearValue.Color[1] = 0.5f; 
        ClearValue.Color[2] = 0.5f; ClearValue.Color[3] = 0.0f;

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &ClearValue,
            IID_PPV_ARGS(&NormalBuffer)));
        VramTracker::Register(NormalBuffer.Get(), EVramCategory::RenderTargets);
        NormalBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = NormalFormat;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device.Native()->CreateRenderTargetView(NormalBuffer.Get(), &RTVDesc,
                                                NormalRTVHeap.CpuHandle(0));

        if (NormalSRVSlot == kInvalidSlot)
            NormalSRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = NormalFormat;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), NormalBuffer.Get(), SRVDesc, NormalSRVSlot);
    }

    void Renderer::SetupReflectionsForScene() {
        if (!Device.RaytracingSupported() || !DDGI.IsReady()) return;
        if (!GBuffer.IsInitialized() || DepthSRVSlot == kInvalidSlot) return;
        Reflections.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                                DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        Reflections.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            DDGI.InstanceSRV(), DDGI.IrradianceAtlasSRV(),
            DepthSRVSlot, GBuffer.SRVSlot(1), GBuffer.SRVSlot(2), HDREnv.BRDFLutSRV());

        ReSTIRGI.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                             DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        ReSTIRGI.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            DDGI.InstanceSRV(), DDGI.IrradianceAtlasSRV(),
            DepthSRVSlot, GBuffer.SRVSlot(1), VelocitySRVSlot);

        Nrd.SetupForResize(Device.Native(), RenderWidth(), RenderHeight());

        if (Nrd.IsReady()) {
            ReSTIRGI.SetupNrdPack(Device.Native(), SRVHeap,
                Nrd.IoResource(FNrdDenoiser::IO_VIEWZ),
                Nrd.IoResource(FNrdDenoiser::IO_NORMAL_ROUGHNESS),
                Nrd.IoResource(FNrdDenoiser::IO_MV),
                Nrd.IoResource(FNrdDenoiser::IO_DIFF_RADIANCE_HITDIST),
                Nrd.IoResource(FNrdDenoiser::IO_OUT_DIFF));
            Reflections.SetupNrdSpec(Device.Native(), SRVHeap,
                Nrd.IoResource(FNrdDenoiser::IO_SPEC_RADIANCE_HITDIST),
                Nrd.IoResource(FNrdDenoiser::IO_OUT_SPEC));
        }
    }

    void Renderer::CreateHDRBuffers() {
        UINT Width  = RenderWidth();
        UINT Height = RenderHeight();
        if (Width == 0 || Height == 0) return;

        HDRColorBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&HDRColorBuffer)));
        VramTracker::Register(HDRColorBuffer.Get(), EVramCategory::RenderTargets);

        if (!HDRRTVHeap.Native())
            HDRRTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device.Native()->CreateRenderTargetView(HDRColorBuffer.Get(), &RTVDesc, HDRRTVHeap.CpuHandle(0));

        if (HDRSRVSlot == kInvalidSlot)
            HDRSRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), HDRColorBuffer.Get(), SRVDesc, HDRSRVSlot);
    }

    void Renderer::CreateVelocityBuffer() {
        const u32 Width = RenderWidth(), Height = RenderHeight();
        if (Width == 0 || Height == 0) return;

        VelocityBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R16G16_FLOAT;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f }; 
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&VelocityBuffer)));
        VramTracker::Register(VelocityBuffer.Get(), EVramCategory::RenderTargets);
        VelocityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!VelocityRTVHeap.Native())
            VelocityRTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device.Native()->CreateRenderTargetView(VelocityBuffer.Get(), &RTVDesc, VelocityRTVHeap.CpuHandle(0));

        if (VelocitySRVSlot == kInvalidSlot)
            VelocitySRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), VelocityBuffer.Get(), SRVDesc, VelocitySRVSlot);
    }

    void Renderer::CreateSceneCopies() {
        UINT Width = RenderWidth(), Height = RenderHeight();
        if (Width == 0 || Height == 0) return;

        SceneColorCopy.Reset();
        SceneDepthCopy.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        ResourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneColorCopy)));
        VramTracker::Register(SceneColorCopy.Get(), EVramCategory::RenderTargets);
        SceneColorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        ResourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneDepthCopy)));
        VramTracker::Register(SceneDepthCopy.Get(), EVramCategory::RenderTargets);
        SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        if (SceneCopyTableStart == kInvalidSlot)
            SceneCopyTableStart = SRVHeap.Allocate(2);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), SceneColorCopy.Get(), SRVDesc, SceneCopyTableStart);

        D3D12_SHADER_RESOURCE_VIEW_DESC DSRV{};
        DSRV.Format                  = DXGI_FORMAT_R32_FLOAT;
        DSRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        DSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DSRV.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), SceneDepthCopy.Get(), DSRV, SceneCopyTableStart + 1);
    }

    void Renderer::RecreateAllPSOs() {
        constexpr DXGI_FORMAT RT = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT DS = DXGI_FORMAT_D32_FLOAT;
        PipelineState.RecreatePSO(Device.Native());
        Skybox.Recreate(Device.Native(), RT, DS);
        Atmosphere.RecreateSky(Device.Native(), RT, DS);
        VolumetricClouds.RecreateComposite(Device.Native(), RT, DS);
        Water.Recreate(Device.Native(), RT, DS, DXGI_FORMAT_R16G16_FLOAT);
        Terrain.RecreatePSOs(Device.Native());
        if (Device.RaytracingSupported())
            DDGIDebugPass.Recreate(Device.Native(), RT, DS);
    }

    bool Renderer::ReloadShaders(const std::string& _ChangedStem) {
        if (!Initialized) return false;
        try {
            CommandQueue.Flush();

            constexpr DXGI_FORMAT RT = DXGI_FORMAT_R16G16B16A16_FLOAT;
            constexpr DXGI_FORMAT DS = DXGI_FORMAT_D32_FLOAT;
            ID3D12Device* Dev = Device.Native();

            struct ShaderReloadEntry {
                std::vector<std::string> Stems;
                std::function<void()>    Recreate;
            };
            const std::vector<ShaderReloadEntry> Table = {
                { { "Triangle.vs", "GBuffer.ps", "DeferredLighting.ps", "DepthNormal.ps",
                    "ForwardBlend.ps" },
                  [&] { PipelineState.RecreatePSO(Dev); } },
                { { "Skybox.vs", "Skybox.ps" },
                  [&] { Skybox.Recreate(Dev, RT, DS); } },
                { { "SkyAtmosphere.vs", "SkyAtmosphere.ps" },
                  [&] { Atmosphere.RecreateSky(Dev, RT, DS); } },
                { { "CloudComposite.vs", "CloudComposite.ps" },
                  [&] { VolumetricClouds.RecreateComposite(Dev, RT, DS); } },
                { { "WaterSurface.vs", "WaterSurface.ps" },
                  [&] { Water.Recreate(Dev, RT, DS, DXGI_FORMAT_R16G16_FLOAT); } },
                { { "Terrain.vs", "TerrainShadow.vs", "TerrainGBuffer.ps",
                    "TerrainDepthNormal.ps" },
                  [&] { Terrain.RecreatePSOs(Dev); } },
                { { "RainWetness.ps", "RainCurtain.ps", "RainParticles.vs", "RainParticles.ps",
                    "RainSplash.vs", "RainSplash.ps" },
                  [&] { RainWetness.Recreate(Dev); } },
                { { "DDGIDebugProbes.vs", "DDGIDebugProbes.ps", "DDGIDebugVolume.vs",
                    "DDGIDebugVolume.ps", "DDGIDebugRays.vs", "DDGIDebugRays.ps" },
                  [&] { if (Device.RaytracingSupported())
                            DDGIDebugPass.Recreate(Dev, RT, DS); } },
            };

            if (_ChangedStem.empty()) {
                RecreateAllPSOs();
                LogInfo("Shaders recarregados (reload completo)");
                return true;
            }

            for (const auto& Entry : Table) {
                if (std::find(Entry.Stems.begin(), Entry.Stems.end(), _ChangedStem)
                        != Entry.Stems.end()) {
                    Entry.Recreate();
                    LogInfo("Shader recarregado: " + _ChangedStem);
                    return true;
                }
            }

            RecreateAllPSOs();
            LogInfo("Shader '" + _ChangedStem + "' nao mapeado; reload completo aplicado");
            return true;
        } catch (const std::exception& e) {
            LogError(std::string("Erro ao recarregar shaders: ") + e.what());
            return false;
        }
    }

    void Renderer::Resize(u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) return;
        CommandQueue.Flush();
        SwapChain.Resize(Device.Native(), _Width, _Height);
        RecreateInternalTargets();
    }

    void Renderer::SetRenderScale(f32 _Scale) {
        _Scale = _Scale < 0.33f ? 0.33f : (_Scale > 2.0f ? 2.0f : _Scale);
        if (_Scale == RenderScale) return;
        RenderScale = _Scale;
        if (!Initialized || SwapChain.GetWidth() == 0) return;
        CommandQueue.Flush();
        RecreateInternalTargets();
    }

    void Renderer::SetCloudsHalfRes(bool _HalfRes) {
        if (VolumetricClouds.GetHalfRes() == _HalfRes) return;
        VolumetricClouds.SetHalfRes(_HalfRes);
        if (!Initialized || !VolumetricClouds.IsInitialized()) return;
        CommandQueue.Flush();
        VolumetricClouds.Resize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
    }

    void Renderer::SetCloudWeatherSeed(u32 _Seed) {
        CloudNoise.SetSeed(_Seed);
        if (!Initialized || !CloudNoise.IsInitialized()) return;
        CommandQueue.Flush();
        CloudNoise.ClearWeatherOverride(SRVHeap); // mexer no procedural desativa a autorada
        CloudNoise.RebakeWeather(CommandQueue, SRVHeap);
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
    }

    void Renderer::SetCloudWeatherCells(u32 _Mult) {
        CloudNoise.SetCellMult(_Mult);
        if (!Initialized || !CloudNoise.IsInitialized()) return;
        CommandQueue.Flush();
        CloudNoise.ClearWeatherOverride(SRVHeap);
        CloudNoise.RebakeWeather(CommandQueue, SRVHeap);
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
    }

    bool Renderer::LoadCloudWeatherTexture(const std::wstring& _Path) {
        if (!Initialized || !CloudNoise.IsInitialized()) return false;
        CommandQueue.Flush();
        if (!CloudNoise.LoadWeatherOverride(Device.Native(), UploadQueue, SRVHeap, _Path))
            return false;
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
        return true;
    }

    void Renderer::ClearCloudWeatherTexture() {
        if (!Initialized || !CloudNoise.HasWeatherOverride()) return;
        CommandQueue.Flush();
        CloudNoise.ClearWeatherOverride(SRVHeap);
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
    }

    void Renderer::RecreateInternalTargets() {
        const u32 RW = RenderWidth(),        RH = RenderHeight();
        const u32 SW = SwapChain.GetWidth(), SH = SwapChain.GetHeight();

        CreateHDRBuffers();
        CreateDepthBuffer();
        CreateNormalBuffer();
        GBuffer.Resize(Device.Native(), SRVHeap, RW, RH);
        GBuffer.WriteDepthSRV(Device.Native(), SRVHeap, DepthBuffer.Get());
        CreateVelocityBuffer();

        VolumetricClouds.Resize(Device.Native(), SRVHeap, RW, RH);
        Water.Resize(Device.Native(), RW, RH);
        RainWetness.Resize(Device.Native(), SRVHeap, RW, RH);
        SunShafts.Resize(Device.Native(), SRVHeap, RW, RH);
        CreateSceneCopies();

        PostProcessor.Resize(Device.Native(), SRVHeap, SW, SH);    
        ObjectPicker.Resize(Device.Native(), RW, RH);
        SelectionOutline.Resize(Device.Native(), SRVHeap, SW, SH); 

        TemporalAA.Resize(Device.Native(), SRVHeap, RW, RH);
        TemporalAA.SetupInputs(Device.Native(), SRVHeap, HDRColorBuffer.Get(), DepthBuffer.Get(), VelocityBuffer.Get());
        TAARanLastFrame = false;

        Fsr.Initialize(Device.Native(), SRVHeap, RW, RH, SW, SH);
        Dlss.Initialize(Device.Native(), SRVHeap, RW, RH, SW, SH);
        Flicker.Resize(Device.Native(), SRVHeap, RW, RH);
        FlickerResetPending = true;

        AO.SetupForResize(Device.Native(), SRVHeap, DepthSRVSlot, NormalSRVSlot, RW, RH);

        HiZ.SetupForResize(Device.Native(), SRVHeap, RW, RH);

        SetupReflectionsForScene();
    }

    void Renderer::UpdateCamera(const CameraInput& _Input, f32 _DeltaTime) {
        Camera.Update(_Input, _DeltaTime);
        ElapsedTime  += _DeltaTime;
        LastDeltaTime = _DeltaTime;
    }

    void Renderer::SetSunDirection(const Vec3& _Direction) {
        SunDir = _Direction.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
    }

    void Renderer::SetSunAzimuthElevation(f32 _AzimuthDeg, f32 _ElevationDeg) {
        const f32 Az = _AzimuthDeg   * ToRad;
        const f32 El = _ElevationDeg * ToRad;
        const f32 CosEl = std::cos(El);
        SetSunDirection(Vec3{ CosEl * std::sin(Az), std::sin(El), CosEl * std::cos(Az) });
    }

    void Renderer::LoadMoonTexture(const std::wstring& _Path) {
        if (!Initialized || !Atmosphere.IsInitialized()) return;
        Atmosphere.LoadMoonTexture(Device.Native(), UploadQueue, SRVHeap, _Path);
    }

    FTexture* Renderer::ImportRuntimeTexture(const std::wstring& _Path, bool _IsNormalMap,
                                            bool _sRGB) {
        if (!Initialized) return nullptr;

        auto EndsWith = [&](const wchar_t* Ext) {
            const size_t N = wcslen(Ext);
            if (_Path.size() < N) return false;
            return _wcsnicmp(_Path.c_str() + _Path.size() - N, Ext, N) == 0;
        };

        FTexture Tex = EndsWith(L".dds")
            ? FTexture::LoadDDS(Device.Native(), UploadQueue, SRVHeap, _Path, _sRGB)
            : FTexture::CreateFromCPU(Device.Native(), UploadQueue, SRVHeap,
                                      FTexture::LoadCPU(_Path, _IsNormalMap, _sRGB));
        if (!Tex.IsValid()) return nullptr;

        ImportedTextures.push_back(std::make_unique<FTexture>(std::move(Tex)));
        return ImportedTextures.back().get();
    }

    bool Renderer::RenderMaterialPreview(FMaterial* _Material,
                                         const FMaterialPreview::FParams& _Params,
                                         std::vector<u8>& _Out) {
        if (!Initialized || !_Material) return false;

        const FGpuMesh* SceneMesh = nullptr;
        Mat44 SceneModel = Mat44::Identity();
        if (_Params.Primitive == FMaterialPreview::PrimSceneMesh) {
            const auto& Rnds = Scene.Renderables();
            const FRenderable* Pick = nullptr;
            if (SelectedIndex >= 0 && SelectedIndex < (int)Rnds.size() &&
                Rnds[SelectedIndex].Material == _Material && !Rnds[SelectedIndex].RaytracingOnly)
                Pick = &Rnds[SelectedIndex];
            if (!Pick) {
                for (const auto& R : Rnds) {
                    if (R.Material != _Material || R.RaytracingOnly || !R.Mesh) continue;
                    Pick = &R;
                    break;
                }
            }
            if (Pick && Pick->Mesh && Pick->Mesh->IsValid()) {
                SceneMesh = Pick->Mesh;
                const Vec3 Center = (Pick->AABBMin + Pick->AABBMax) * 0.5f;
                const Vec3 Ext    = (Pick->AABBMax - Pick->AABBMin) * 0.5f;
                f32 Radius = std::sqrt(Ext.X * Ext.X + Ext.Y * Ext.Y + Ext.Z * Ext.Z);
                if (Radius < 1e-3f || Radius > 1e8f) Radius = 0.5f; // AABB ausente/degenerado
                const f32 S = 0.5f / Radius;
                SceneModel = Pick->Transform.Matrix()
                           * Mat44::Translation(-Center)
                           * Mat44::Scale({ S, S, S });
            }
        }

        return MaterialPreview.Render(Device.Native(), CommandQueue, SRVHeap,
                                      *_Material, _Params, _Out, SceneMesh, SceneModel);
    }

    bool Renderer::LoadMaterialPreviewEnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        return MaterialPreview.LoadEnvironment(Device.Native(), CommandQueue, SRVHeap, _Path);
    }

    void Renderer::LoadStarCatalog(const std::wstring& _Path) {
        if (!Initialized || !Atmosphere.IsInitialized()) return;
        Atmosphere.LoadStarCatalog(Device.Native(), SRVHeap, _Path);
    }

    bool Renderer::WorldToScreen(const Vec3& _W, f32& _Sx, f32& _Sy) const {
        const Mat44& M = LastViewProj;
        const f32 cx = _W.X*M.M[0][0] + _W.Y*M.M[1][0] + _W.Z*M.M[2][0] + M.M[3][0];
        const f32 cy = _W.X*M.M[0][1] + _W.Y*M.M[1][1] + _W.Z*M.M[2][1] + M.M[3][1];
        const f32 cw = _W.X*M.M[0][3] + _W.Y*M.M[1][3] + _W.Z*M.M[2][3] + M.M[3][3];
        if (cw <= 1e-5f) return false;
        const f32 ndcx = cx / cw, ndcy = cy / cw;
        _Sx = (ndcx * 0.5f + 0.5f) * static_cast<f32>(SwapChain.GetWidth());
        _Sy = (0.5f - ndcy * 0.5f) * static_cast<f32>(SwapChain.GetHeight());
        return true;
    }

    bool Renderer::ScreenToRay(u32 _X, u32 _Y, Vec3& _O, Vec3& _D) const {
        const u32 Wd = SwapChain.GetWidth(), Ht = SwapChain.GetHeight();
        if (Wd == 0 || Ht == 0) return false;
        const f32 ndcx = ((static_cast<f32>(_X) + 0.5f) / Wd) * 2.0f - 1.0f;
        const f32 ndcy = 1.0f - ((static_cast<f32>(_Y) + 0.5f) / Ht) * 2.0f;
        const Mat44 Inv = LastViewProj.Inverse();
        const f32 v[4] = { ndcx, ndcy, 0.5f, 1.0f };
        f32 w[4];
        for (int j = 0; j < 4; ++j)
            w[j] = v[0]*Inv.M[0][j] + v[1]*Inv.M[1][j] + v[2]*Inv.M[2][j] + v[3]*Inv.M[3][j];
        if (std::fabs(w[3]) < 1e-9f) return false;
        const Vec3 WorldPt{ w[0]/w[3], w[1]/w[3], w[2]/w[3] };
        _O = Camera.GetPosition();
        _D = (WorldPt - _O).NormalizedSafe(Vec3::UnitZ());
        return true;
    }

    void Renderer::RenderFrame() {
        if (!Initialized) return;

        CommandQueue.BeginFrame();

        GpuProfiler.BeginFrame(CommandQueue.FrameIndex());
        GpuProfiler.Begin(CommandQueue.List(), "Frame (GPU)");

        ObjectPicker.Tick();

        f32 Aspect = SwapChain.GetWidth() > 0 && SwapChain.GetHeight() > 0
                     ? static_cast<f32>(SwapChain.GetWidth()) / static_cast<f32>(SwapChain.GetHeight())
                     : 1.0f;

        Mat44 View       = Camera.GetViewMatrix();

        const f32 NearZ = 0.1f;
        const f32 FarZ  = UseWater ? 20000.0f : 4000.0f;

        const f32 FovY  = 60.0f * ToRad;
        const Mat44 ProjUnjittered = kReverseZ
            ? Mat44::PerspectiveFovReverseZLH(FovY, Aspect, NearZ, FarZ)
            : Mat44::PerspectiveFovLH(FovY, Aspect, NearZ, FarZ);

        Mat44 Projection = ProjUnjittered;
        IUpscaler* ActiveUp = ActiveUpscaler();          // FSR ou DLSS-SR ativo (nullptr = None/indisponivel)
        const bool UpscaleActive = (ActiveUp != nullptr);
        const bool TAAActive  = UseTAA && !UpscaleActive && TemporalAA.IsInitialized();
        f32 JitterPxX = 0.0f, JitterPxY = 0.0f;
        f32 ProjJitterYSign = 1.0f;
        if (UpscaleActive) {
            ActiveUp->GetJitter(FrameIndex, JitterPxX, JitterPxY); // FSR: sequencia do SDK; DLSS: Halton
            ProjJitterYSign = -1.0f;
        } else if (TAAActive) {
            const u32 kJitterPhases = 8;
            const u32 Idx = (FrameIndex % kJitterPhases) + 1;
            JitterPxX = Halton(Idx, 2) - 0.5f;
            JitterPxY = Halton(Idx, 3) - 0.5f;
        }
        if (UpscaleActive || TAAActive) {
            Projection.M[2][0] += JitterPxX * 2.0f / static_cast<f32>(RenderWidth());
            Projection.M[2][1] += ProjJitterYSign * JitterPxY * 2.0f / static_cast<f32>(RenderHeight());
        }

        const Vec2 JitterUv{ JitterPxX / static_cast<f32>(RenderWidth()),
                             -ProjJitterYSign * JitterPxY / static_cast<f32>(RenderHeight()) };
        const Vec2 JitterPx{ JitterPxX, -ProjJitterYSign * JitterPxY };
        const Mat44 ViewProjection = View * Projection;
        const Mat44 ViewProjUnjittered = View * ProjUnjittered;
        LastViewProj = ViewProjUnjittered; 

        const u32 FrameSlot = CommandQueue.FrameIndex();
        FrameConstants* MappedCB = reinterpret_cast<FrameConstants*>(
            MappedFrameBase + static_cast<size_t>(FrameSlot) * sizeof(FrameConstants));

        Vec3 CameraPosition      = Camera.GetPosition();
        MappedCB->CameraPosition = { CameraPosition.X, CameraPosition.Y, CameraPosition.Z, 1.0f };

        const f32 IBLEnabled = HDREnv.HasHDRLoaded() ? 1.0f : 0.0f;
        MappedCB->IBLParams      = { IBLIntensity, IBLRotation,
                                     static_cast<f32>(FHDREnvironment::kSpecularMips - 1),
                                     IBLEnabled };
        MappedCB->Time           = { ElapsedTime, LastDeltaTime,
                                     static_cast<f32>(FrameIndex),
                                     AODebug ? 1.0f : 0.0f }; 

        if (TimeOfDay.Enabled) {
            TimeOfDay.Tick(LastDeltaTime);
            SetSunDirection(TimeOfDay.SunDirection());
        }
        const Vec3 SunN          = SunDir.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
        MappedCB->SunDirection   = { SunN.X, SunN.Y, SunN.Z, SunIntensity };

        {
            const f32 Target = Weather.RainAmount;
            const f32 Tau    = (Target > Weather.Wetness) ? 5.0f : 30.0f;
            Weather.Wetness += (Target - Weather.Wetness) *
                               (1.0f - std::exp(-std::max(LastDeltaTime, 0.0f) / Tau));
            if (Target <= 0.001f && Weather.Wetness < 0.005f) Weather.Wetness = 0.0f;
        }

        const f32 RainSky    = Weather.DriveSky ? Weather.RainAmount : 0.0f;
        const f32 RainKeyDim = 1.0f - RainSky * 0.75f;
        const f32 RainAmbDim = 1.0f - RainSky * 0.40f;

        Vec3 EffectiveSunColor = SunColorRGB;
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            const Vec3 T = Atmosphere.SunTransmittance(SunN);
            EffectiveSunColor = { SunColorRGB.X * T.X, SunColorRGB.Y * T.Y, SunColorRGB.Z * T.Z };
        }

        {
            const f32 hf = std::clamp(SunN.Y / 0.03f, 0.0f, 1.0f);
            const f32 HorizonFade = hf * hf * (3.0f - 2.0f * hf);
            EffectiveSunColor = { EffectiveSunColor.X * HorizonFade,
                                  EffectiveSunColor.Y * HorizonFade,
                                  EffectiveSunColor.Z * HorizonFade };
        }
        EffectiveSunColor = { EffectiveSunColor.X * RainKeyDim, EffectiveSunColor.Y * RainKeyDim,
                              EffectiveSunColor.Z * RainKeyDim }; // F4: nublado de chuva
        MappedCB->SunColor       = { EffectiveSunColor.X, EffectiveSunColor.Y, EffectiveSunColor.Z, 0.0f };

        const Vec3 MoonN = TimeOfDay.MoonDirection();

        const f32 nf          = std::clamp((0.0f - SunN.Y) / 0.15f, 0.0f, 1.0f);
        const f32 NightFactor = nf * nf * (3.0f - 2.0f * nf);
        const f32 MoonSunCos = MoonN.X * SunN.X + MoonN.Y * SunN.Y + MoonN.Z * SunN.Z;
        const f32 MoonIllum  = (1.0f - MoonSunCos) * 0.5f;
        const f32 MoonUp     = std::clamp(MoonN.Y * 8.0f, 0.0f, 1.0f); 
        const bool MoonOn    = TimeOfDay.MoonEnabled;

        const Vec3 MoonTrans = (UseAtmosphereSky && Atmosphere.IsInitialized())
                             ? Atmosphere.SunTransmittance(MoonN) : Vec3{ 1.0f, 1.0f, 1.0f };
        const Vec3 MoonTint     = { 0.6f, 0.7f, 1.0f };
        const Vec3 MoonLightCol = { MoonTint.X * MoonTrans.X * RainKeyDim,
                                    MoonTint.Y * MoonTrans.Y * RainKeyDim,
                                    MoonTint.Z * MoonTrans.Z * RainKeyDim }; // F4: nublado
        const f32  MoonW        = MoonOn ? (TimeOfDay.MoonIntensity * MoonIllum * NightFactor * MoonUp) : 0.0f;
        MappedCB->MoonDirection = { MoonN.X, MoonN.Y, MoonN.Z, MoonW };
        MappedCB->MoonColor     = { MoonLightCol.X, MoonLightCol.Y, MoonLightCol.Z, 0.0f };

        const f32 MoonHalfAngleRad = 0.5f * ToRad * TimeOfDay.MoonDiskSize;
        const f32 CosMoonRadius    = std::cos(MoonHalfAngleRad);
        // x2 sem textura: o disco procedural branco depende do brilho pra ter presenca.
        const f32 MoonDiskBright = MoonOn
            ? TimeOfDay.MoonDiskBrightness * (Atmosphere.HasMoonTexture() ? 1.0f : 2.0f)
            : 0.0f;
        Atmosphere.SetNightParams(MoonN, CosMoonRadius, MoonDiskBright,
                                  TimeOfDay.StarIntensity, NightFactor, ElapsedTime);

        const f32 MoonSkyScale = MoonOn ? (0.05f * TimeOfDay.MoonIntensity * MoonIllum) : 0.0f;
        Atmosphere.SetMoonSkyLight(MoonSkyScale, MoonOn ? MoonIllum : 0.0f);

        {
            const f32  LatR  = TimeOfDay.LatitudeDeg    * ToRad;
            const f32  NoR   = TimeOfDay.NorthOffsetDeg * ToRad;
            const Vec3 Pole  = { std::cos(LatR) * std::sin(NoR), std::sin(LatR),
                                 std::cos(LatR) * std::cos(NoR) };
            const f32  Angle = TimeOfDay.Enabled ? (TimeOfDay.TimeHours * 15.0f * ToRad)
                                                 : (ElapsedTime * 0.004f);
            Atmosphere.SetStarRotation(Pole, Angle);
        }

        const bool KeyIsMoon  = (SunN.Y <= 0.0f);
        const Vec3 KeyDir     = KeyIsMoon ? MoonN : SunN;
        const Vec3 KeyColor   = KeyIsMoon ? MoonLightCol : EffectiveSunColor; 
        const f32  KeyInt     = KeyIsMoon ? MoonW : SunIntensity;
  
        const f32  CloudDim   = KeyIsMoon ? (MoonW / std::max(SunIntensity, 1e-3f)) : 1.0f;
        const Vec3 KeyCloudCol = { KeyColor.X * CloudDim, KeyColor.Y * CloudDim, KeyColor.Z * CloudDim };

        Vec3 SkyAmbient, GroundAmbient; // tambem alimentam o ambient das nuvens volumetricas
        {
            Vec3& Sky    = SkyAmbient;
            Vec3& Ground = GroundAmbient;
            const bool Physical = UseAtmosphereSky && Atmosphere.IsInitialized() &&
                                  Atmosphere.GetSkyAmbient(FrameSlot, Sky, Ground);
            if (!Physical) {
                auto Sat = [](f32 X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); };
                const f32 SunY   = SunN.Y;
                const f32 Day    = Sat(SunY * 4.0f + 0.2f);
                const f32 LowSun = Sat(1.0f - SunY * 2.5f);
                const Vec3 Zenith  = { 0.18f, 0.30f, 0.55f };
                const Vec3 Horizon = { 0.60f, 0.40f, 0.26f };
                Sky    = (Zenith + (Horizon - Zenith) * LowSun) * Day;
                Ground = Sky * 0.35f;
            }

            Sky    = { Sky.X * RainAmbDim, Sky.Y * RainAmbDim, Sky.Z * RainAmbDim };
            Ground = { Ground.X * RainAmbDim, Ground.Y * RainAmbDim, Ground.Z * RainAmbDim };
            MappedCB->SkyAmbientColor    = { Sky.X, Sky.Y, Sky.Z,
                                             UseAtmosphereAmbient ? 1.0f : 0.0f };
            MappedCB->GroundAmbientColor = { Ground.X, Ground.Y, Ground.Z, AtmoAmbientIntensity };
        }

        if (UseGI && DDGI.IsReady()) {
            const Vec3 GMin = DDGI.GridMin();
            const Vec3 GCnt = DDGI.GridCount();
            MappedCB->DDGIGridMin   = { GMin.X, GMin.Y, GMin.Z, DDGI.Spacing() };

            MappedCB->DDGIGridCount = { GCnt.X, GCnt.Y, GCnt.Z, GIDebug ? 2.0f : 1.0f };
            MappedCB->DDGIParams    = { DDGI.GetIntensity(), DDGI.TileSizeF(),
                                        DDGI.AtlasW(), DDGI.AtlasH() };

            const f32 GIFlags = (GIChebyshev ? 1.0f : 0.0f) + (GISkipInactiveProbes ? 2.0f : 0.0f)
                              + (GISkipInactiveFallback ? 4.0f : 0.0f);
            MappedCB->DDGIDistParams = { DDGI.DistTileSizeF(), DDGI.DistAtlasW(),
                                         DDGI.DistAtlasH(), GIFlags };
        } else {
            MappedCB->DDGIGridMin    = { 0.0f, 0.0f, 0.0f, 1.0f };
            MappedCB->DDGIGridCount  = { 0.0f, 0.0f, 0.0f, 0.0f };
            MappedCB->DDGIParams     = { 0.0f, 6.0f, 1.0f, 1.0f };
            MappedCB->DDGIDistParams = { 14.0f, 1.0f, 1.0f, 0.0f };
        }

        const bool ReflectionsActive = UseReflections && Reflections.IsReady();
        const bool ReSTIRGIActive    = UseReSTIRGI && ReSTIRGI.IsReady();
        const bool NrdMode           = ReSTIRGIActive && Nrd.IsReady() && UseNrdDenoise;
        ReSTIRGI.SetUseNrd(NrdMode);
        Reflections.SetUseNrd(NrdMode);

        MappedCB->ReflectionParams = { Reflections.GetMaxRoughness(), Reflections.GetRoughnessFade(),
                                       ReflectionsActive ? 1.0f : 0.0f,
                                       ReSTIRGIActive ? (NrdMode ? 2.0f : 1.0f) : 0.0f };
        ++FrameIndex;

        Mat44 ViewNoTrans = View;
        ViewNoTrans.M[3][0] = 0.0f;
        ViewNoTrans.M[3][1] = 0.0f;
        ViewNoTrans.M[3][2] = 0.0f;
        const Mat44 VPNoTrans    = ViewNoTrans * Projection;
        const Mat44 InvVPNoTrans = VPNoTrans.Inverse();
        const Mat44 InvViewProjFull = ViewProjection.Inverse();
        const Mat44 InvViewProjUnjit = ViewProjUnjittered.Inverse();
        MappedCB->InvViewProj = InvViewProjFull;

        const f32 MipBias = (UpscaleActive && RenderWidth() < OutputWidth())
            ? std::log2(static_cast<f32>(RenderWidth()) / static_cast<f32>(OutputWidth())) - 1.0f
            : 0.0f;
        MappedCB->RenderParams = { MipBias, 0.0f, 0.0f, 0.0f };

        Atmosphere.UpdatePerFrame(FrameSlot, SunN, InvVPNoTrans, VPNoTrans,
                                  InvViewProjFull, CameraPosition, kKmPerWorldUnit,
                                  static_cast<f32>(RenderWidth()), static_cast<f32>(RenderHeight()));

        const bool VolShaftsActive = UseSunShafts && SunShafts.IsInitialized() && UseHeightFog;
        const f32 FogDensityBase = Fog.GetDensity();
        if (RainSky > 0.0f) Fog.SetDensity(FogDensityBase * (1.0f + RainSky * 1.5f));

        const Vec4 ShaftsFogCollapsed = Fog.CollapsedFogParams(CameraPosition.Y);

        Vec3 CamForwardW{ 0.0f, 0.0f, 1.0f };
        {
            const Mat44& IM = InvViewProjUnjit;
            const f32 v[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
            f32 w[4];
            for (int j = 0; j < 4; ++j)
                w[j] = v[2] * IM.M[2][j] + v[3] * IM.M[3][j];
            if (std::fabs(w[3]) > 1e-9f) {
                const Vec3 P{ w[0] / w[3], w[1] / w[3], w[2] / w[3] };
                CamForwardW = (P - CameraPosition).NormalizedSafe(Vec3{ 0.0f, 0.0f, 1.0f });
            }
        }

        const bool VolFogActive = UseVolumetricFog && UseHeightFog && VolumetricFog.IsInitialized();
        if (VolFogActive) {
            FVolumetricFogPass::FFrameParams VF{};
            VF.InvViewProjUnjit = InvViewProjUnjit;
            VF.ViewProjUnjit    = ViewProjUnjittered;
            VF.FrameIndex       = FrameIndex;
            VF.CameraPos        = CameraPosition;
            VF.CameraForward    = CamForwardW;
            VF.DirToSun         = KeyDir;
            VF.SunColorTimesIntensity = { KeyColor.X * KeyInt, KeyColor.Y * KeyInt,
                                          KeyColor.Z * KeyInt };
            VF.CollapsedFog     = ShaftsFogCollapsed;
            VF.SkyAmbient       = SkyAmbient;
            VF.NearZ            = NearZ;
            VF.RenderW          = RenderWidth();
            VF.RenderH          = RenderHeight();
            if (UseGI && DDGI.IsReady()) {
                const Vec3 GMin = DDGI.GridMin();
                const Vec3 GCnt = DDGI.GridCount();
                VF.DDGIGridMin   = { GMin.X, GMin.Y, GMin.Z, DDGI.Spacing() };
                VF.DDGIGridCount = { GCnt.X, GCnt.Y, GCnt.Z, 1.0f };
                VF.DDGIParams    = { DDGI.GetIntensity(), DDGI.TileSizeF(),
                                     DDGI.AtlasW(), DDGI.AtlasH() };
            }
            VolumetricFog.UpdatePerFrame(FrameSlot, VF);
        } else if (VolumetricFog.IsInitialized()) {
            VolumetricFog.ResetHistory(); // efeito dormiu: historia/PrevVP obsoletos
        }

        Fog.UpdatePerFrame(FrameSlot, InvViewProjFull, CameraPosition, kKmPerWorldUnit, KeyDir,
                           NearZ, FarZ, RenderWidth(), RenderHeight(),
                           UseAerialPerspective, UseHeightFog, Atmosphere.AerialDepthKm(),
                           VolShaftsActive, VolFogActive, VolumetricFog.GetMaxDistance(),
                           VolumetricFog.GridZParams(), CamForwardW);
        Fog.SetDensity(FogDensityBase);

        const f32 CloudGroundRadius = 6360.0f + FAtmosphere::kGroundAltitudeKm;
        const f32 CloudCovBase = VolumetricClouds.GetCoverage();
        if (RainSky > 0.0f) {
            const f32 RainCov = std::min(RainSky * 1.4f, 1.0f) * 0.92f;
            VolumetricClouds.SetCoverage(std::max(CloudCovBase, RainCov));
        }
        VolumetricClouds.UpdatePerFrame(FrameSlot, InvVPNoTrans, InvViewProjFull,
                                        ViewProjUnjittered, CameraPosition, kKmPerWorldUnit,
                                        CloudGroundRadius, KeyDir, KeyCloudCol,
                                        SkyAmbient, GroundAmbient, ElapsedTime, FrameIndex);
        VolumetricClouds.SetCoverage(CloudCovBase);

        Vec4 CloudShadowP{ 0.0f, 0.0f, 0.0f, 0.0f };
        Vec4 CloudShadowP2{ 0.0f, 0.0f, 0.0f, 0.0f };
        {
            const f32 KeyY = KeyDir.Y > 0.05f ? KeyDir.Y : 0.05f;
            const bool CloudShadowOn = UseClouds && VolumetricClouds.IsInitialized() &&
                                       VolumetricClouds.GetShadowsEnabled() && KeyDir.Y > 0.02f;
            CloudShadowP  = { VolumetricClouds.ShadowCenterX(),
                              VolumetricClouds.ShadowCenterZ(),
                              VolumetricClouds.ShadowInvExtent(),
                              CloudShadowOn ? VolumetricClouds.GetShadowStrength() : 0.0f };
            CloudShadowP2 = { kKmPerWorldUnit,
                              VolumetricClouds.GetBottomAltitude(),
                              KeyDir.X / KeyY, KeyDir.Z / KeyY };
            MappedCB->CloudShadowParams  = CloudShadowP;
            MappedCB->CloudShadowParams2 = CloudShadowP2;
            if (VolumetricClouds.IsInitialized()) {
                D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(GBuffer.SRVTableStart() + 4);
                D3D12_CPU_DESCRIPTOR_HANDLE Src =
                    SRVHeap.CpuHandleStaging(VolumetricClouds.ShadowSRV());
                UINT One = 1;
                Device.Native()->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }

        if (VolShaftsActive) {
            const Vec3 KeyColInt = { KeyColor.X * KeyInt, KeyColor.Y * KeyInt,
                                     KeyColor.Z * KeyInt };
            const f32 ShaftNoiseFrame =
                (TAAActive || UpscaleActive || SunShafts.GetVolTemporal())
                    ? static_cast<f32>(FrameIndex % 64u) : 0.0f;
            SunShafts.UpdateVolumetric(FrameSlot, KeyDir, KeyColInt, ShaftsFogCollapsed,
                                       ShaftNoiseFrame, InvViewProjFull, CameraPosition,
                                       ViewProjUnjittered, CloudShadowP, CloudShadowP2,
                                       0.0f);
        } else if (SunShafts.IsInitialized()) {
            SunShafts.ResetHistory();
        }

        if (VolFogActive) VolumetricFog.PatchCloudShadow(CloudShadowP, CloudShadowP2);

        const Mat44 WaterViewProj    = ViewProjection;
        const Mat44 WaterInvViewProj = WaterViewProj.Inverse();
        const bool WaterHasDepth = SceneColorCopy && SceneDepthCopy;
        if (UseWater && Water.IsInitialized()) {
            const bool WaterAtmoRefl = UseAtmosphereSky && Atmosphere.IsInitialized();
            const f32 WaterReflIntensity =
                WaterAtmoRefl ? (1.0f - RainSky * 0.65f) : IBLIntensity;
            Water.UpdatePerFrame(FrameSlot, WaterViewProj, Projection, WaterInvViewProj,
                                 ViewProjUnjittered, PrevViewProj, CameraPosition, KeyDir,
                                 KeyInt, KeyColor, SkyAmbient, ElapsedTime,
                                 WaterAtmoRefl || HDREnv.HasHDRLoaded(), WaterReflIntensity,
                                 RenderWidth(), RenderHeight(), NearZ, FarZ,
                                 WaterHasDepth, UseAtmosphereSky);
            for (u32 c = 0; c < kOceanCascades; ++c) {
                if (!Ocean[c].IsInitialized()) continue;
                Ocean[c].SetTime(ElapsedTime);
                Ocean[c].SetWindDirection(Water.GetWindDirection());
                Ocean[c].SetWindSpeed(Water.GetWindSpeed());
                Ocean[c].SetAmplitude(Water.GetWavesAmount());
                Ocean[c].SetChoppyFactors(Water.GetFFTChoppyScale() *
                                          Water.GetFFTDisplacementScale() *
                                          Water.GetWavesSize() * Water.GetWavesAmount());
            }
        }

        auto* CommandList = CommandQueue.List();

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        auto DSV = DSVHeap.CpuHandle(0);

        {
            FBarrierBatch Batch;
            Batch.Transition(HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            auto HDR_RTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(HDR_RTV, ClearColor, 0, nullptr);
            CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0, nullptr);
        }

        D3D12_VIEWPORT Viewport{};
        Viewport.Width    = static_cast<FLOAT>(RenderWidth());
        Viewport.Height   = static_cast<FLOAT>(RenderHeight());
        Viewport.MinDepth = 0.0f;
        Viewport.MaxDepth = 1.0f;

        D3D12_RECT ScissorRect{};
        ScissorRect.right  = static_cast<LONG>(RenderWidth());
        ScissorRect.bottom = static_cast<LONG>(RenderHeight());

        CommandList->RSSetViewports(1, &Viewport);
        CommandList->RSSetScissorRects(1, &ScissorRect);

        ID3D12DescriptorHeap* DescriptorHeaps[] = { SRVHeap.Native() };
        CommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);

        if (UseWater && Ocean[0].IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Água — FFT");
            for (u32 c = 0; c < kOceanCascades; ++c)
                Ocean[c].RecordCompute(FrameSlot, CommandList, SRVHeap);
        }

        GpuProfiler.Begin(CommandList, "Céu e atmosfera");
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            Atmosphere.RecordSkyViewBake(CommandList);
            Atmosphere.RecordSkyAmbientIntegration(CommandList);
            if (UseWater && Water.IsInitialized())
                Atmosphere.RecordSkyReflectionBake(CommandList);
            Atmosphere.RenderSky(CommandList, SRVHeap);
            if (NightFactor > 0.001f && TimeOfDay.StarIntensity > 0.0f)
                Atmosphere.RenderStars(CommandList, SRVHeap);
        } else if (ShowSkybox && HDREnv.HasHDRLoaded()) {
            Skybox.Render(FrameSlot, CommandList, SRVHeap, HDREnv.EnvCubeSRV(),
                          InvVPNoTrans, IBLIntensity, IBLRotation);
        }

        if (Atmosphere.IsInitialized()) {
            Atmosphere.RecordAerialPerspectiveBake(CommandList);
        }
        GpuProfiler.End(CommandList); // Céu e atmosfera

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Sombra das nuvens");
            VolumetricClouds.RecordShadowMap(CommandList, SRVHeap);
        }

        u32 GILightCount = 0;
        {
            FGPULightGI* Dst = reinterpret_cast<FGPULightGI*>(
                MappedGILightBase + static_cast<size_t>(FrameSlot) * kMaxLights * sizeof(FGPULightGI));
            for (const FLight& L : Scene.Lights()) {
                if (!L.Enabled || L.Intensity <= 0.0f || L.AttenuationRadius <= 0.0f) continue;
                if (GILightCount >= kMaxLights) break;

                FGPULightGI G;
                G.PosInvRadius      = { L.Position.X, L.Position.Y, L.Position.Z,
                                        1.0f / L.AttenuationRadius };
                G.ColorSourceRadius = { L.Color.X * L.Intensity, L.Color.Y * L.Intensity,
                                        L.Color.Z * L.Intensity,
                                        std::max(L.SourceRadius, 0.01f) };
                if (L.Type == ELightType::Spot) {
                    const Vec3 D = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                    const f32 OuterDeg = std::clamp(L.OuterConeDeg, 1.0f, 89.0f);
                    const f32 InnerDeg = std::clamp(L.InnerConeDeg, 0.0f, OuterDeg);
                    const f32 CosOuter = std::cos(OuterDeg * ToRad);
                    const f32 CosInner = std::cos(InnerDeg * ToRad);
                    G.DirCosOuter = { D.X, D.Y, D.Z, CosOuter };
                    G.SpotParams  = { 1.0f / std::max(CosInner - CosOuter, 1e-4f),
                                      0.0f, 0.0f, 0.0f };
                } else {
                    G.DirCosOuter = { 0.0f, -1.0f, 0.0f, -2.0f };
                    G.SpotParams  = { 0.0f, 0.0f, 0.0f, 0.0f };
                }
                Dst[GILightCount++] = G;
            }
        }

        if (RaytracingScene.IsBuilt() && Scene.TransformsVersion() != TlasTransformsVersion) {
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> TlasCL;
            if (SUCCEEDED(CommandList->QueryInterface(IID_PPV_ARGS(&TlasCL))) &&
                RaytracingScene.RecordTlasRebuild(TlasCL.Get(), Scene, FrameSlot)) {
                TlasTransformsVersion = Scene.TransformsVersion();
            }
        }

        u64 GIComputeFence = 0;
        if (UseGI && DDGI.IsReady()) {
            DDGI.SetPunctualLightsSRV(Device.Native(), SRVHeap, GILightSRVSlot[FrameSlot], FrameSlot);
            DDGI.UpdatePerFrame(FrameSlot, KeyDir, KeyInt, KeyColor, FrameIndex, GILightCount);
            if (UseAsyncCompute && DDGI.CanRunAsync()) {
                DDGI.TransitionForUpdate(CommandList);
                const u64 S1 = CommandQueue.SubmitSegmentAndContinue();

                ID3D12GraphicsCommandList* CCL = ComputeQueue.Begin();
                GpuProfilerCompute.BeginFrame(ComputeQueue.CurrentSlot());
                ID3D12DescriptorHeap* CHeaps[] = { SRVHeap.Native() };
                CCL->SetDescriptorHeaps(_countof(CHeaps), CHeaps);
                {
                    FGpuScope Scope(GpuProfilerCompute, CCL, "DDGI (async)");
                    DDGI.RecordUpdate(CCL, SRVHeap);
                }
                GpuProfilerCompute.Resolve(CCL);
                GIComputeFence = ComputeQueue.SubmitAfter(CommandQueue.NativeFence(), S1);

                ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
                auto SceneRTV = HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
            } else {
                FGpuScope Scope(GpuProfiler, CommandList, "DDGI");
                DDGI.TransitionForUpdate(CommandList);
                DDGI.RecordUpdate(CommandList, SRVHeap);
                DDGI.TransitionForRead(CommandList);
            }
        }

        if (ReflectionsActive) {
            Reflections.SetPunctualLightsSRV(Device.Native(), SRVHeap, GILightSRVSlot[FrameSlot], FrameSlot);
            const f32 ReflSkyIntensity = 1.0f - RainSky * 0.65f;
            Reflections.UpdatePerFrame(FrameSlot, InvViewProjFull, PrevViewProj, CameraPosition,
                                       RenderWidth(), RenderHeight(), KeyDir, KeyInt,
                                       KeyColor, FrameIndex, ReflSkyIntensity, 0.2f,
                                       Reflections.GetRealHitShading(), View, GILightCount);
        }

        if (ReSTIRGIActive) {
            ReSTIRGI.SetPunctualLightsSRV(Device.Native(), SRVHeap, GILightSRVSlot[FrameSlot]);
            ReSTIRGI.UpdatePerFrame(FrameSlot, InvViewProjFull, CameraPosition,
                                    RenderWidth(), RenderHeight(), KeyDir, KeyInt, KeyColor,
                                    FrameIndex, 1.0f, 0.2f, View, PrevJitterUv - JitterUv,
                                    GILightCount);
        }

        CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());

        CommandList->SetGraphicsRootConstantBufferView(
            0, ConstantBuffer->GetGPUVirtualAddress() +
               static_cast<u64>(FrameSlot) * sizeof(FrameConstants));

        Vec4 Planes[6];
        {
            const Mat44& V = ViewProjection;
            auto Col = [&](int j) { return Vec4{ V.M[0][j], V.M[1][j], V.M[2][j], V.M[3][j] }; };
            Vec4 c0 = Col(0), c1 = Col(1), c2 = Col(2), c3 = Col(3);
            Planes[0] = { c3.X+c0.X, c3.Y+c0.Y, c3.Z+c0.Z, c3.W+c0.W }; 
            Planes[1] = { c3.X-c0.X, c3.Y-c0.Y, c3.Z-c0.Z, c3.W-c0.W }; 
            Planes[2] = { c3.X+c1.X, c3.Y+c1.Y, c3.Z+c1.Z, c3.W+c1.W }; 
            Planes[3] = { c3.X-c1.X, c3.Y-c1.Y, c3.Z-c1.Z, c3.W-c1.W }; 
            Planes[4] = { c2.X, c2.Y, c2.Z, c2.W };                     
            Planes[5] = { c3.X-c2.X, c3.Y-c2.Y, c3.Z-c2.Z, c3.W-c2.W }; 
        }
        auto AABBOutsideFrustum = [&](const Vec3& Mn, const Vec3& Mx) -> bool {
            for (int i = 0; i < 6; ++i) {
                const Vec4& p = Planes[i];
                f32 px = (p.X >= 0.0f) ? Mx.X : Mn.X;
                f32 py = (p.Y >= 0.0f) ? Mx.Y : Mn.Y;
                f32 pz = (p.Z >= 0.0f) ? Mx.Z : Mn.Z;
                if (p.X*px + p.Y*py + p.Z*pz + p.W < 0.0f) return true;
            }
            return false;
        };

        std::vector<FLocalShadows::FShadowJob>     LocalShadowJobs;
        std::vector<FLocalShadows::FCubeShadowJob> LocalCubeJobs;
        {
            Vec4 NPlanes[6];
            for (int i = 0; i < 6; ++i) {
                const Vec4& p   = Planes[i];
                const f32   len = std::sqrt(p.X*p.X + p.Y*p.Y + p.Z*p.Z);
                const f32   inv = len > 1e-6f ? 1.0f / len : 0.0f;
                NPlanes[i] = { p.X*inv, p.Y*inv, p.Z*inv, p.W*inv };
            }

            FGPULight* DstLights = reinterpret_cast<FGPULight*>(
                MappedLightBase + static_cast<size_t>(FrameSlot) * kMaxLights * sizeof(FGPULight));
            u32 NumLights = 0;

            struct ShadowCand { u32 Gpu; u32 LightIdx; f32 Dist2; };
            std::vector<ShadowCand> ShadowCands;
            std::vector<ShadowCand> CubeCands; 

            const auto& SceneLights = Scene.Lights();
            for (u32 li = 0; li < static_cast<u32>(SceneLights.size()); ++li) {
                const FLight& L = SceneLights[li];
                if (!L.Enabled || L.Intensity <= 0.0f || L.AttenuationRadius <= 0.0f) continue;
                if (NumLights >= kMaxLights) break;

                bool Outside = false;
                for (int i = 0; i < 6 && !Outside; ++i) {
                    const Vec4& p = NPlanes[i];
                    Outside = (p.X*L.Position.X + p.Y*L.Position.Y + p.Z*L.Position.Z + p.W)
                              < -L.AttenuationRadius;
                }
                if (Outside) continue;

                FGPULight G;
                G.PosInvRadius      = { L.Position.X, L.Position.Y, L.Position.Z,
                                        1.0f / L.AttenuationRadius };
                G.ColorSourceRadius = { L.Color.X * L.Intensity, L.Color.Y * L.Intensity,
                                        L.Color.Z * L.Intensity,
                                        std::max(L.SourceRadius, 0.01f) };
                G.ShadowMatrix      = Mat44::Identity();
                if (L.Type == ELightType::Spot) {
                    const Vec3 D = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                    const f32 OuterDeg  = std::clamp(L.OuterConeDeg, 1.0f, 89.0f);
                    const f32 InnerDeg  = std::clamp(L.InnerConeDeg, 0.0f, OuterDeg);
                    const f32 CosOuter  = std::cos(OuterDeg * ToRad);
                    const f32 CosInner  = std::cos(InnerDeg * ToRad);
                    G.DirCosOuter = { D.X, D.Y, D.Z, CosOuter };
                    G.SpotParams  = { 1.0f / std::max(CosInner - CosOuter, 1e-4f),
                                      -1.0f, 0.0f, 0.0f };
                    if (L.CastShadows && LocalShadows.IsInitialized()) {
                        const Vec3 ToCam = L.Position - CameraPosition;
                        ShadowCands.push_back({ NumLights, li, ToCam.LengthSq() });
                    }
                } else {
                    G.DirCosOuter = { 0.0f, -1.0f, 0.0f, -2.0f }; // -2 = sem mascara de cone
                    G.SpotParams  = { 0.0f, -1.0f, 0.0f, 0.0f };
                    if (L.CastShadows && LocalShadows.IsInitialized()) {
                        const Vec3 ToCam = L.Position - CameraPosition;
                        CubeCands.push_back({ NumLights, li, ToCam.LengthSq() });
                    }
                }
                DstLights[NumLights++] = G;
            }

            std::sort(ShadowCands.begin(), ShadowCands.end(),
                      [](const ShadowCand& a, const ShadowCand& b) { return a.Dist2 < b.Dist2; });
            const u32 NumShadowed = std::min<u32>(static_cast<u32>(ShadowCands.size()),
                                                  FLocalShadows::kMaxShadows);
            for (u32 s = 0; s < NumShadowed; ++s) {
                const FLight& L = SceneLights[ShadowCands[s].LightIdx];
                const Vec3 D  = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                const Vec3 Up = std::fabs(D.Y) > 0.99f ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                       : Vec3{ 0.0f, 1.0f, 0.0f };
                const f32 OuterRad = std::clamp(L.OuterConeDeg, 1.0f, 89.0f) * ToRad;

                const f32 NearP    = FLocalShadows::kPointNear;
                const f32 FarP     = std::max(L.AttenuationRadius, NearP * 2.0f);
                const Mat44 LView = Mat44::LookAtLH(L.Position, L.Position + D, Up);
                const Mat44 LProj = Mat44::PerspectiveFovLH(2.0f * OuterRad, 1.0f, NearP, FarP);
                const Mat44 LVP   = LView * LProj;

                Mat44 BiasUV = Mat44::Identity();
                BiasUV.M[0][0] = 0.5f;  BiasUV.M[1][1] = -0.5f;
                BiasUV.M[3][0] = 0.5f;  BiasUV.M[3][1] = 0.5f;

                FGPULight& G  = DstLights[ShadowCands[s].Gpu];
                G.ShadowMatrix = LVP * BiasUV;
                G.SpotParams.Y = static_cast<f32>(s);
                LocalShadowJobs.push_back({ LVP, L.Position, FarP, s });
            }

            std::sort(CubeCands.begin(), CubeCands.end(),
                      [](const ShadowCand& a, const ShadowCand& b) { return a.Dist2 < b.Dist2; });
            const u32 NumCubes = std::min<u32>(static_cast<u32>(CubeCands.size()),
                                               FLocalShadows::kMaxCubeShadows);
            for (u32 c = 0; c < NumCubes; ++c) {
                const FLight& L = SceneLights[CubeCands[c].LightIdx];
                DstLights[CubeCands[c].Gpu].SpotParams.Y = static_cast<f32>(c);
                LocalCubeJobs.push_back({ L.Position, L.AttenuationRadius, c });
            }

            MappedCB->LightParams  = { static_cast<f32>(NumLights),
                                       1.0f / static_cast<f32>(FLocalShadows::kResolution),
                                       LocalShadows.GetDepthBias(), 0.0f };
            MappedCB->LightParams2 = { 1.0f / static_cast<f32>(FLocalShadows::kCubeResolution),
                                       FLocalShadows::kPointNear, 0.0f, 0.0f };

            if (VolFogActive)
                VolumetricFog.PatchLights(NumLights,
                                          1.0f / static_cast<f32>(FLocalShadows::kResolution),
                                          LocalShadows.GetDepthBias(),
                                          FLocalShadows::kPointNear);
        }


        const Vec3 CamPos = Camera.GetPosition();
        const D3D12_GPU_VIRTUAL_ADDRESS ObjectCBBase = ObjectCB->GetGPUVirtualAddress();
        const u32 FrameObjectBase = FrameSlot * MaxObjects;

        struct AllItem { const FRenderable* R; FMaterial* Mat; u32 Slot; u32 SceneIndex; };
        std::vector<AllItem> AllItems;

        u32             SelectedSlot  = kInvalidSlot;
        const FGpuMesh* SelectedMesh  = nullptr;
        Mat44           SelectedModel = Mat44::Identity();
        {
            const std::vector<FRenderable>& RList = Scene.Renderables();
            AllItems.reserve(RList.size());
            const size_t PrevCount = PrevModels.size();
            PrevModels.resize(RList.size(), Mat44::Identity());
            const bool WriteOcclusionBounds = UseOcclusionCulling && HiZ.ObjectsReady();
            for (size_t si = 0; si < RList.size(); ++si) {
                const FRenderable& R = RList[si];
                if (WriteOcclusionBounds)
                    HiZ.WriteBounds(FrameSlot, static_cast<u32>(si), R.AABBMin, R.AABBMax);
                if (!R.Visible || R.RaytracingOnly || !R.Mesh || !R.Mesh->IsValid()) continue;
                if (AllItems.size() >= MaxObjects) break;
                FMaterial* Mat = (R.Material && R.Material->IsFinalized()) ? R.Material : ActiveMaterial;
                const u32 Slot = FrameObjectBase + static_cast<u32>(AllItems.size());
                const Mat44 Model = R.Transform.Matrix();
                const Mat44 PrevModel = (si < PrevCount) ? PrevModels[si] : Model;
                ObjectConstants OC;
                OC.MVP            = Model * ViewProjection;
                OC.ModelMatrix    = Model;
                OC.CurMVPNoJitter = Model * ViewProjUnjittered;
                OC.PrevMVP        = PrevModel * PrevViewProj;
                std::memcpy(MappedObjectCB + static_cast<size_t>(Slot) * sizeof(ObjectConstants),
                            &OC, sizeof(ObjectConstants));
                if (static_cast<int>(si) == SelectedIndex) {
                    SelectedSlot = Slot; SelectedMesh = R.Mesh; SelectedModel = Model;
                }
                AllItems.push_back({ &R, Mat, Slot, static_cast<u32>(si) });
                PrevModels[si] = Model; 
            }
        }

        // Resultado do teste HZB gravado ha kFramesInFlight frames neste slot (a fence
        // ja foi esperada no BeginFrame). nullptr = sem teste valido -> tudo visivel.
        const u32* OcclusionVis = UseOcclusionCulling
            ? HiZ.ResolveResults(FrameSlot, static_cast<u32>(Scene.Renderables().size()))
            : nullptr;
        u32 OccludedCount = 0;

        struct VisItem { const FRenderable* R; FMaterial* Mat; f32 Dist; u32 Slot; u32 SceneIndex; };
        std::vector<VisItem> VisibleScratch;
        VisibleScratch.reserve(AllItems.size());
        for (const AllItem& A : AllItems) {
            if (UseFrustumCulling && AABBOutsideFrustum(A.R->AABBMin, A.R->AABBMax)) continue;
            // Objeto selecionado nunca e cullado (gizmo/drag move mais rapido que a
            // latencia do readback e o pop incomodaria bem aqui). O resultado so cobre
            // [0, Capacity); indices alem disso (ex.: proxy RT do terreno) ficam visiveis.
            if (OcclusionVis && A.SceneIndex < HiZ.Capacity() &&
                !OcclusionVis[A.SceneIndex] &&
                static_cast<int>(A.SceneIndex) != SelectedIndex) {
                ++OccludedCount;
                continue;
            }
            const f32 cx = (A.R->AABBMin.X + A.R->AABBMax.X) * 0.5f - CamPos.X;
            const f32 cy = (A.R->AABBMin.Y + A.R->AABBMax.Y) * 0.5f - CamPos.Y;
            const f32 cz = (A.R->AABBMin.Z + A.R->AABBMax.Z) * 0.5f - CamPos.Z;
            VisibleScratch.push_back({ A.R, A.Mat, cx*cx + cy*cy + cz*cz, A.Slot, A.SceneIndex });
        }
        std::sort(VisibleScratch.begin(), VisibleScratch.end(),
                  [](const VisItem& a, const VisItem& b) { return a.Dist < b.Dist; });
        LastVisibleCount  = static_cast<u32>(VisibleScratch.size());
        LastOccludedCount = OccludedCount;

        if (UseTerrain && Terrain.IsLoaded())
            Terrain.UpdatePerFrame(FrameSlot, ViewProjection, ViewProjUnjittered, PrevViewProj,
                                   CameraPosition, FovY, MipBias);

        {
            const f32 ShadowNoiseFrame = (TAAActive || UpscaleActive)
                ? static_cast<f32>(FrameIndex % 64u) : 0.0f;
            SunShadows.UpdatePerFrame(FrameSlot, UseSunShadows, View, CameraPosition, FovY, Aspect, KeyDir, NearZ, ShadowNoiseFrame);
            if (UseSunShadows) {
                std::vector<FSunShadows::FShadowDrawItem> Casters;
                Casters.reserve(AllItems.size());
                for (const AllItem& A : AllItems) {
                    // Translucido nao projeta sombra opaca (vidro deixa o sol entrar).
                    if (A.Mat && A.Mat->Blend) continue;
                    Casters.push_back({ A.R->Mesh, A.Mat,
                                        ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                        A.R->AABBMin, A.R->AABBMax });
                }
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "Sombras — sol (CSM)");
                    FSunShadows::FExtraCascadeDraw TerrainCasters;
                    if (UseTerrain && Terrain.IsLoaded())
                        TerrainCasters = [this](ID3D12GraphicsCommandList* Cmd, u32,
                                                D3D12_GPU_VIRTUAL_ADDRESS CascadeCB,
                                                const Mat44& CascadeVP) {
                            Terrain.RenderShadowCascade(Cmd, SRVHeap, CascadeCB, CascadeVP);
                        };
                    SunShadows.RecordDepthPass(CommandList, SRVHeap, Casters.data(), Casters.size(),
                                               TerrainCasters);
                }

                auto SceneRTV = HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, ConstantBuffer->GetGPUVirtualAddress() +
                       static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
            } else {
                SunShadows.EnsureReadable(CommandList);
            }

            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        }

        if (!LocalShadowJobs.empty() || !LocalCubeJobs.empty()) {
            std::vector<FLocalShadows::FShadowDrawItem> LocalCasters;
            LocalCasters.reserve(AllItems.size());
            for (const AllItem& A : AllItems) {
                if (A.Mat && A.Mat->Blend) continue; // vidro nao projeta sombra opaca
                LocalCasters.push_back({ A.R->Mesh, A.Mat,
                                         ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                         A.R->AABBMin, A.R->AABBMax });
            }
            {
                FGpuScope Scope(GpuProfiler, CommandList, "Sombras — locais");
                LocalShadows.RecordDepthPass(CommandList, SRVHeap, FrameSlot,
                                             LocalCasters.data(), LocalCasters.size(),
                                             LocalShadowJobs.data(),
                                             static_cast<u32>(LocalShadowJobs.size()),
                                             LocalCubeJobs.data(),
                                             static_cast<u32>(LocalCubeJobs.size()));
            }

            auto SceneRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        } else if (LocalShadows.IsInitialized()) {
            LocalShadows.EnsureReadable(CommandList);
        }

        const bool AOWillRun = UseAO && AO.IsReady();
        const bool DoPrepass = true;
        if (DoPrepass) {
            GpuProfiler.Begin(CommandList, "Z-prepass");
            if (AOWillRun) {
                FBarrierBatch Batch;
                Batch.TransitionTracked(NormalBuffer.Get(), NormalBufferState,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                Batch.Flush(CommandList);
                auto NormalRTV = NormalRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &NormalRTV, FALSE, &DSV);
                const FLOAT NeutralN[4] = { 0.5f, 0.5f, 0.5f, 0.0f }; 
                CommandList->ClearRenderTargetView(NormalRTV, NeutralN, 0, nullptr);
                CommandList->SetPipelineState(PipelineState.PSODepthNormal());
            } else {
                CommandList->SetPipelineState(PipelineState.PSODepthOnly());
            }
            for (const VisItem& V : VisibleScratch) {
                if (V.Mat->TwoSided || V.Mat->Constants.AlphaTest || V.Mat->Blend) continue;
                CommandList->SetGraphicsRootConstantBufferView(
                    4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                V.R->Mesh->Draw(CommandList);
            }

            CommandList->SetPipelineState(AOWillRun ? PipelineState.PSODepthNormalMasked()
                                                    : PipelineState.PSODepthOnlyMasked());
            for (const VisItem& V : VisibleScratch) {
                if (V.Mat->Blend) continue;
                if (!V.Mat->TwoSided && !V.Mat->Constants.AlphaTest) continue;
                CommandList->SetGraphicsRootConstantBufferView(
                    4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                V.Mat->Bind(CommandList, SRVHeap);
                V.R->Mesh->Draw(CommandList);
            }

            if (UseTerrain && Terrain.IsLoaded())
                Terrain.RenderDepthPrepass(CommandList, SRVHeap, AOWillRun);
            GpuProfiler.End(CommandList); // Z-prepass
        }

        // HZB do depth do prepass (min-reduce reverse-Z) + teste dos AABBs com a VP
        // sem jitter DESTE frame; o resultado volta pela readback ring e e consumido
        // kFramesInFlight frames depois no filtro do VisibleScratch (acima).
        if (HiZ.IsReady() && UseOcclusionCulling) {
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            {
                FGpuScope Scope(GpuProfiler, CommandList, "HZB");
                HiZ.RecordBuild(CommandList, SRVHeap, DepthSRVSlot);
                HiZ.RecordTest(CommandList, SRVHeap, FrameSlot,
                               static_cast<u32>(Scene.Renderables().size()),
                               ViewProjUnjittered);
            }
            Batch.Transition(DepthBuffer.Get(),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (AO.IsReady()) {
            if (AOWillRun) {
                CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

                const f32 TanHalf = std::tan(0.5f * FovY);
                const f32 M11 = 1.0f / TanHalf;
                const f32 M00 = M11 / Aspect;
                const f32 M22 = Projection.M[2][2];
                const f32 M32 = Projection.M[3][2];
                AO.UpdatePerFrame(FrameSlot, M00, M11, M22, M32, View,
                                  RenderWidth(), RenderHeight(), FrameIndex);

                FBarrierBatch Batch;
                Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Batch.TransitionTracked(NormalBuffer.Get(), NormalBufferState,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Batch.Flush(CommandList);
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "GTAO");
                    AO.Execute(CommandList, SRVHeap, DepthSRVSlot);
                }

                Batch.Transition(DepthBuffer.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
                Batch.Flush(CommandList);

                auto SceneRTV = HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, ConstantBuffer->GetGPUVirtualAddress() +
                       static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            } else {
                AO.ClearToWhite(CommandList, SRVHeap);
            }
        }

        {
            GpuProfiler.Begin(CommandList, "G-buffer (geometria)");
            FBarrierBatch Batch;
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(VelocityBuffer.Get(), VelocityState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);
            // 5o MRT = SceneColor HDR: o emissivo e escrito direto nele (dieta do G-buffer).
            // O ceu ja esta la (desenhado antes do prepass); a geometria opaca sobrescreve so
            // os proprios pixels e o deferred lighting depois SOMA a luz (blend aditivo).
            D3D12_CPU_DESCRIPTOR_HANDLE GBufRTVs[FGBuffer::kTargetCount + 2] = {
                GBuffer.RTVHandle(0), GBuffer.RTVHandle(1), GBuffer.RTVHandle(2),
                VelocityRTVHeap.CpuHandle(0), HDRRTVHeap.CpuHandle(0) };
            CommandList->OMSetRenderTargets(FGBuffer::kTargetCount + 2, GBufRTVs, FALSE, &DSV);
            GBuffer.Clear(CommandList);
            const FLOAT VelClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            CommandList->ClearRenderTargetView(VelocityRTVHeap.CpuHandle(0), VelClear, 0, nullptr);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<u64>(FrameSlot) * sizeof(FrameConstants));

            ID3D12PipelineState* CurGeomPSO = nullptr;
            for (const VisItem& V : VisibleScratch) {
                FMaterial* Mat = V.Mat;
                if (Mat->Blend) continue;
                const bool TwoSided = Mat->TwoSided || Mat->Constants.AlphaTest;
                ID3D12PipelineState* Want = TwoSided ? PipelineState.PSOGBufferTwoSided()
                                                     : PipelineState.PSOGBuffer();
                if (Want != CurGeomPSO) { CommandList->SetPipelineState(Want); CurGeomPSO = Want; }
                CommandList->SetGraphicsRootConstantBufferView(
                    4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                Mat->Bind(CommandList, SRVHeap);
                V.R->Mesh->Draw(CommandList);
            }

            if (UseTerrain && Terrain.IsLoaded()) {
                FGpuScope Scope(GpuProfiler, CommandList, "Terreno");
                Terrain.RenderGBuffer(CommandList, SRVHeap);
            }
            Batch.TransitionTracked(VelocityBuffer.Get(), VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            GpuProfiler.End(CommandList); // G-buffer (geometria)
        }

        if (Weather.Active() && RainWetness.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Chuva — wetness");
            if (Weather.RainOcclusion) {
                std::vector<FRainWetness::FOccluderItem> RainOccluders;
                RainOccluders.reserve(AllItems.size());
                for (const AllItem& A : AllItems)
                    RainOccluders.push_back({ A.R->Mesh, A.Mat,
                                              ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                              A.R->AABBMin, A.R->AABBMax });
                RainWetness.RecordOcclusionMap(CommandList, SRVHeap, FrameSlot, CameraPosition,
                                               RainOccluders.data(), RainOccluders.size());
            }
            const Vec3 KeyColorInt = { KeyColor.X * KeyInt, KeyColor.Y * KeyInt,
                                       KeyColor.Z * KeyInt };
            RainWetness.UpdatePerFrame(FrameSlot, InvViewProjFull, ViewProjection,
                                       CameraPosition, ElapsedTime, Weather, KeyDir,
                                       KeyColorInt, SkyAmbient);
            RainWetness.Execute(CommandList, SRVHeap, GBuffer, DepthBuffer.Get(), DepthSRVSlot,
                                RenderWidth(), RenderHeight());
        }

        if (GIComputeFence != 0) {
            CommandQueue.SubmitSegmentAndContinue();
            CommandQueue.GpuWait(ComputeQueue.NativeFence(), GIComputeFence);
            ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
            CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            DDGI.TransitionForRead(CommandList);
        }

        if (ReSTIRGIActive) {
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(VelocityBuffer.Get(), VelocityState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "ReSTIR GI");
                ReSTIRGI.RecordTrace(CommandList, SRVHeap);
            }

            if (NrdMode) {
                if (ReflectionsActive) {
                    FGpuScope Scope(GpuProfiler, CommandList, "Reflexos (trace)");
                    Reflections.RecordTrace(CommandList, SRVHeap);
                }
                GpuProfiler.Begin(CommandList, "NRD denoise");
                Nrd.TransitionInputsToWrite(CommandList);
                ReSTIRGI.RecordNrdPack(CommandList, SRVHeap);

                if (ReflectionsActive) Reflections.RecordNrdPack(CommandList, SRVHeap);
                else                   Reflections.RecordNrdSpecZero(CommandList, SRVHeap);
                Nrd.SetFrame(ProjUnjittered, NrdPrevProj, View, NrdPrevView,
                             JitterPx, PrevJitterPx, FrameIndex);
                Nrd.Denoise(CommandList);
                Nrd.TransitionOutputToRead(CommandList);
                GpuProfiler.End(CommandList); // NRD denoise
                ID3D12DescriptorHeap* ReHeaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(ReHeaps), ReHeaps);
            }

            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(VelocityBuffer.Get(), VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }

        {
            FBarrierBatch Batch;
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            auto SceneRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, nullptr); 
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
            CommandList->SetGraphicsRootDescriptorTable(2, SRVHeap.GpuHandle(GBuffer.SRVTableStart()));
            CommandList->SetGraphicsRootDescriptorTable(3, SRVHeap.GpuHandle(IBLTableStart));
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            {
                const u32 GITable = (UseGI && DDGI.IsReady()) ? DDGI.SceneGITableStart() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(7, SRVHeap.GpuHandle(GITable));
            }
            {
                const u32 AOTable = AO.IsReady() ? AO.AOSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(8, SRVHeap.GpuHandle(AOTable));
            }
            {
                const u32 ReSTIRTable = ReSTIRGIActive ? ReSTIRGI.GITexSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(9, SRVHeap.GpuHandle(ReSTIRTable));
            }
            CommandList->SetGraphicsRootShaderResourceView(
                10, LightBuffer->GetGPUVirtualAddress() +
                    static_cast<u64>(FrameSlot) * kMaxLights * sizeof(FGPULight));
            {
                const u32 LocalShadowTable = LocalShadows.IsInitialized()
                    ? LocalShadows.ShadowSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(11, SRVHeap.GpuHandle(LocalShadowTable));
            }
            GpuProfiler.Begin(CommandList, "Deferred lighting");
            // Aditivo (soma sobre o emissivo do geometry pass); nas views de debug SSAO/GI o
            // shader retorna a visualizacao inteira -> PSO opaco p/ substituir a tela.
            const bool DeferredDebugView = AODebug || (UseGI && DDGI.IsReady() && GIDebug);
            CommandList->SetPipelineState(DeferredDebugView
                ? PipelineState.PSODeferredLightingDebug()
                : PipelineState.PSODeferredLighting());
            CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            CommandList->IASetVertexBuffers(0, 0, nullptr);
            CommandList->IASetIndexBuffer(nullptr);
            CommandList->DrawInstanced(3, 1, 0, 0);
            GpuProfiler.End(CommandList); 

            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);
        }

        if (ObjectPicker.HasPendingRequest()) {
            {
                std::vector<FObjectPicker::FDrawItem> PickItems;
                PickItems.reserve(VisibleScratch.size());
                for (const VisItem& V : VisibleScratch)
                    PickItems.push_back({ V.R->Mesh,
                                          ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants),
                                          V.SceneIndex + 1 });
                ObjectPicker.RecordIDPass(CommandList, DSV, PickItems.data(), PickItems.size(),
                                          RenderWidth(), RenderHeight());

                auto SceneRTV = HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, ConstantBuffer->GetGPUVirtualAddress() +
                       static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            }
        }

        if (ReflectionsActive) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr); 

            const D3D12_RESOURCE_STATES ReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, ReadState);
            GBuffer.AppendTransitions(Batch, ReadState);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "Reflexos (composite)");
                if (!NrdMode) Reflections.RecordTrace(CommandList, SRVHeap);
                Reflections.RecordComposite(CommandList, SRVHeap, HDRRTVHeap.CpuHandle(0),
                                            RenderWidth(), RenderHeight());
            }

            Batch.Transition(DepthBuffer.Get(), ReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            auto SceneRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        }

        {
            bool AnyBlend = false;
            for (const VisItem& V : VisibleScratch)
                if (V.Mat->Blend) { AnyBlend = true; break; }
            if (AnyBlend) {
                FGpuScope Scope(GpuProfiler, CommandList, "Translúcidos");
                auto SceneRTV = HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, ConstantBuffer->GetGPUVirtualAddress() +
                       static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
                CommandList->SetGraphicsRootDescriptorTable(3, SRVHeap.GpuHandle(IBLTableStart));
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
                {
                    const u32 GITable = (UseGI && DDGI.IsReady()) ? DDGI.SceneGITableStart() : IBLTableStart;
                    CommandList->SetGraphicsRootDescriptorTable(7, SRVHeap.GpuHandle(GITable));
                }
                CommandList->SetPipelineState(PipelineState.PSOForwardBlend());
                for (auto It = VisibleScratch.rbegin(); It != VisibleScratch.rend(); ++It) {
                    if (!It->Mat->Blend) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(It->Slot) * sizeof(ObjectConstants));
                    It->Mat->Bind(CommandList, SRVHeap);
                    It->R->Mesh->Draw(CommandList);
                }
            }
        }

        if (UseWater && Water.IsInitialized() && WaterHasDepth) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr); 

            FBarrierBatch Batch;
            Batch.Transition(HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.TransitionTracked(SceneColorCopy.Get(), SceneColorCopyState,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
            Batch.TransitionTracked(SceneDepthCopy.Get(), SceneDepthCopyState,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
            Batch.Flush(CommandList);

            CommandList->CopyResource(SceneColorCopy.Get(), HDRColorBuffer.Get());
            CommandList->CopyResource(SceneDepthCopy.Get(), DepthBuffer.Get());

            Batch.Transition(HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.TransitionTracked(SceneColorCopy.Get(), SceneColorCopyState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(SceneDepthCopy.Get(), SceneDepthCopyState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            auto HDR_RTV_Rebind = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV_Rebind, FALSE, &DSV);
        }

        if (UseWater && Water.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Água — superfície");

            FBarrierBatch WaterBatch;
            WaterBatch.TransitionTracked(VelocityBuffer.Get(), VelocityState,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
            WaterBatch.Flush(CommandList);
            D3D12_CPU_DESCRIPTOR_HANDLE WaterRTVs[2] = {
                HDRRTVHeap.CpuHandle(0), VelocityRTVHeap.CpuHandle(0) };
            CommandList->OMSetRenderTargets(2, WaterRTVs, FALSE, &DSV);

            const u32 WaterReflCube =
                (UseAtmosphereSky && Atmosphere.IsInitialized())
                    ? Atmosphere.SkyReflectionSRV()
                    : HDREnv.SpecularSRV();
            const u32 OceanDispSlots[kOceanCascades] = {
                Ocean[0].SRVSlot(), Ocean[1].SRVSlot(), Ocean[2].SRVSlot() };
            const u32 OceanNormalSlots[kOceanCascades] = {
                Ocean[0].NormalSRVSlot(), Ocean[1].NormalSRVSlot(), Ocean[2].NormalSRVSlot() };
            Water.RenderSurface(CommandList, SRVHeap, WaterReflCube, OceanDispSlots,
                                OceanNormalSlots, SceneCopyTableStart, Atmosphere.SkyViewSRV(),
                                SunShadows.ConstantsAddress(), SunShadows.ShadowSRVSlot());

            WaterBatch.TransitionTracked(VelocityBuffer.Get(), VelocityState,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            WaterBatch.Flush(CommandList);
            auto PostWaterRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &PostWaterRTV, FALSE, &DSV);
        }

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Nuvens");

            const D3D12_RESOURCE_STATES CloudReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, CloudReadState);
            Batch.Flush(CommandList);

            VolumetricClouds.RecordRaymarch(CommandList, SRVHeap, DepthSRVSlot);

            auto CloudRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &CloudRTV, FALSE, nullptr); // sem DSV: depth e SRV
            VolumetricClouds.Composite(CommandList, SRVHeap, DepthSRVSlot);

            Batch.Transition(DepthBuffer.Get(), CloudReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (Device.RaytracingSupported() && DDGIDebugPass.GetEnabled() && DDGI.IsReady()) {
            auto SceneRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            DDGIDebugPass.Render(FrameSlot, CommandList, SRVHeap, DDGI, ViewProjection, CameraPosition,
                                 FrameIndex);
        }

        if ((UseHeightFog || UseAerialPerspective) && Fog.IsInitialized()) {
            const D3D12_RESOURCE_STATES FogDepthRead =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, FogDepthRead);
            Batch.Flush(CommandList);

            const bool VolFogOn = UseVolumetricFog && UseHeightFog && VolumetricFog.IsInitialized();
            if (VolFogOn) {
                FGpuScope Scope(GpuProfiler, CommandList, "Volumetric fog");
                SunShadows.EnsureReadableCompute(CommandList);
                LocalShadows.EnsureReadableCompute(CommandList);
                VolumetricFog.Execute(CommandList, SRVHeap, SunShadows.ConstantsAddress(),
                                      SunShadows.ShadowSRVSlot(),
                                      (UseGI && DDGI.IsReady()) ? DDGI.IrradianceAtlasSRV()
                                                                : DepthSRVSlot,
                                      LightBuffer->GetGPUVirtualAddress() +
                                          static_cast<u64>(FrameSlot) * kMaxLights * sizeof(FGPULight),
                                      LocalShadows.ShadowSRVSlot(),
                                      VolumetricClouds.IsInitialized()
                                          ? VolumetricClouds.ShadowSRV() : DepthSRVSlot,
                                      DepthSRVSlot);
            }

            const bool VolShaftsOn = UseSunShafts && SunShafts.IsInitialized() && UseHeightFog;
            if (VolShaftsOn) {
                FGpuScope Scope(GpuProfiler, CommandList, "Sun shafts");
                SunShadows.EnsureReadable(CommandList);
                SunShafts.RecordVolumetric(CommandList, SRVHeap, DepthSRVSlot,
                                           SunShadows.ConstantsAddress(),
                                           SunShadows.ShadowSRVSlot(),
                                           VolumetricClouds.IsInitialized()
                                               ? VolumetricClouds.ShadowSRV()
                                               : DepthSRVSlot);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
            }

            GpuProfiler.Begin(CommandList, "Fog");
            auto Fog_RTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &Fog_RTV, FALSE, nullptr);
            Fog.Execute(CommandList, SRVHeap, DepthSRVSlot, Atmosphere.AerialVolumeSRV(),
                        SunShafts.IsInitialized() ? SunShafts.VolumetricSRVSlot()
                                                  : DepthSRVSlot,
                        VolFogOn ? VolumetricFog.IntegratedSRVSlot() : DepthSRVSlot);
            GpuProfiler.End(CommandList); // Fog

            Batch.Transition(DepthBuffer.Get(), FogDepthRead,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (Weather.Raining() && RainWetness.IsInitialized() &&
            (Weather.CurtainAmount > 0.001f || Weather.RainParticles)) {
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            GpuProfiler.Begin(CommandList, "Chuva — cortina/gotas");
            auto RainRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &RainRTV, FALSE, nullptr);
            if (Weather.CurtainAmount > 0.001f)
                RainWetness.ExecuteCurtain(CommandList, SRVHeap, DepthSRVSlot,
                                           RenderWidth(), RenderHeight());
            if (Weather.RainParticles)
                RainWetness.ExecuteParticles(CommandList, SRVHeap, DepthSRVSlot,
                                             RenderWidth(), RenderHeight());
            GpuProfiler.End(CommandList); // Chuva — cortina/gotas

            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (GBufferDebugMode > 0 && GBuffer.IsInitialized() && GBufferDebugPass.IsInitialized()) {
            GBuffer.TransitionToRead(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto HDRDbgRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDRDbgRTV, FALSE, nullptr);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            GBufferDebugPass.Execute(CommandList, SRVHeap, GBuffer.SRVTableStart(), VelocitySRVSlot, GBufferDebugMode);
            GBuffer.TransitionToWrite(CommandList); 
        }

        {
            FBarrierBatch Batch;
            Batch.Transition(HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }

        ID3D12Resource* PostInput    = HDRColorBuffer.Get();
        u32             PostInputSRV = HDRSRVSlot;
        if (TAAActive) {
            FBarrierBatch Batch;
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "TAA");
                TemporalAA.Execute(CommandList, SRVHeap, FrameSlot, InvViewProjUnjit, PrevViewProj,
                                   TAAHistoryBlend, TAARanLastFrame, TAAVarianceGamma, TAASharpness,
                                   TAAMotionBlend, TAAAntiFlicker, TAAStationaryMargin, CameraPosition,
                                   NearZ, FarZ, TAADebugMode);
            }

            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);

            PostInput    = TemporalAA.DisplayOutputResource();
            PostInputSRV = TemporalAA.DisplayOutputSRVSlot();
            TAARanLastFrame = true;
        } else if (UpscaleActive) {
            FBarrierBatch Batch;
            Batch.Transition(HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Transition(VelocityBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            FUpscaleParams UpParams{};
            UpParams.Color        = HDRColorBuffer.Get();
            UpParams.Depth        = DepthBuffer.Get();
            UpParams.Velocity     = VelocityBuffer.Get();
            UpParams.JitterX      = JitterPxX;
            UpParams.JitterY      = JitterPxY;
            UpParams.NearZ        = NearZ;
            UpParams.FarZ         = FarZ;
            UpParams.FovYRadians  = FovY;
            UpParams.AspectRatio  = Aspect;
            UpParams.DeltaTimeSec = LastDeltaTime;
            UpParams.Quality      = UpscalerQuality;
            UpParams.Reset        = false;
            // Matrizes p/ o DLSS (o FSR ignora): projecao unjittered + reprojecao (clip atual -> anterior).
            // PrevViewProj ainda guarda o frame anterior aqui (so e atualizado logo abaixo).
            UpParams.ViewToClip     = ProjUnjittered;
            UpParams.ClipToPrevClip = ViewProjUnjittered.Inverse() * PrevViewProj;
            {
                const Mat44 InvView = View.Inverse();   // view->world: linhas = base da camera em mundo
                UpParams.CamRight = { InvView.M[0][0], InvView.M[0][1], InvView.M[0][2] };
                UpParams.CamUp    = { InvView.M[1][0], InvView.M[1][1], InvView.M[1][2] };
                UpParams.CamFwd   = { InvView.M[2][0], InvView.M[2][1], InvView.M[2][2] };
                UpParams.CamPos   = { InvView.M[3][0], InvView.M[3][1], InvView.M[3][2] };
            }

            {
                FGpuScope Scope(GpuProfiler, CommandList,
                                Upscaler == EUpscaler::DLSS ? "DLSS-SR" : "FSR");
                ActiveUp->Dispatch(CommandList, UpParams);
            }

            // Manual hooking (eDisableCLStateTracking): o SL pode ter mexido no estado do CL. Rebinda o
            // descriptor heap shader-visible antes do post chain (o FSR/ffx-api tolera o rebind redundante).
            {
                ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            }

            Batch.Transition(HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Transition(VelocityBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            PostInput    = ActiveUp->OutputResource();
            PostInputSRV = ActiveUp->OutputSRVSlot();
            TAARanLastFrame = false;
        } else {
            TAARanLastFrame = false;
        }

        PrevViewProj = ViewProjUnjittered;
        NrdPrevProj  = ProjUnjittered;
        NrdPrevView  = View;
        PrevJitterUv = JitterUv;
        PrevJitterPx = JitterPx;

        if (FlickerMode > 0 && Flicker.IsInitialized()) {
            Flicker.Execute(CommandList, SRVHeap, PostInputSRV, static_cast<f32>(FlickerMode),
                            FlickerScale, FlickerAlpha, FlickerResetPending,
                            RenderWidth(), RenderHeight());
            FlickerResetPending = false;
            PostInput    = Flicker.OutputResource();
            PostInputSRV = Flicker.OutputSRVSlot();
        }

        FBarrierBatch BackBatch;
        BackBatch.Transition(SwapChain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        BackBatch.Flush(CommandList);

        {
            FGpuScope Scope(GpuProfiler, CommandList, "Pós (bloom+tonemap)");
            PostProcessor.Execute(CommandList, SRVHeap, PostInput, SwapChain.CurrentRTV(),
                                  PostInputSRV, FrameSlot, SwapChain.GetWidth(), SwapChain.GetHeight());
        }

        if (SelectedIndex >= 0 && SelectedSlot != kInvalidSlot && SelectedMesh
            && SelectionOutline.IsInitialized()) {
            FSelectionOutline::FDrawItem Item{ SelectedMesh, SelectedModel * ViewProjUnjittered };
            SelectionOutline.RecordMask(CommandList, &Item, 1, FrameSlot);
            auto BackRTV = SwapChain.CurrentRTV();
            SelectionOutline.RecordOutline(CommandList, SRVHeap, BackRTV,
                                           SwapChain.GetWidth(), SwapChain.GetHeight(), FrameSlot);
        }

        if (DebugDraw.IsInitialized() && !DebugDraw.Empty()) {
            const bool WantDepth = DebugDraw.HasOccluded() && DepthSRVSlot != kInvalidSlot;
            FBarrierBatch Batch;
            if (WantDepth) {
                Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Batch.Flush(CommandList);
            }
            // Eixos da camera em mundo (colunas da view row-vector) p/ os billboards de icone.
            const Vec3 CamRight{ View.M[0][0], View.M[1][0], View.M[2][0] };
            const Vec3 CamUp   { View.M[0][1], View.M[1][1], View.M[2][1] };
            DebugDraw.Render(CommandList, FrameSlot, ViewProjUnjittered, SwapChain.CurrentRTV(),
                             SwapChain.GetWidth(), SwapChain.GetHeight(), CamRight, CamUp,
                             WantDepth ? SRVHeap.GpuHandle(DepthSRVSlot)
                                       : D3D12_GPU_DESCRIPTOR_HANDLE{});
            if (WantDepth) {
                Batch.Transition(DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
                Batch.Flush(CommandList);
            }
        }
        DebugDraw.Clear();

        BackBatch.Transition(SwapChain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT);
        BackBatch.Flush(CommandList);

        GpuProfiler.End(CommandList); 
        GpuProfiler.Resolve(CommandList);

        SMILE_HR(CommandList->Close());
        ID3D12CommandList* CommandLists[] = { CommandList };

        AsyncGIRanLastFrame = (GIComputeFence != 0);
        CommandQueue.EndFrame(CommandLists, 1);
        SwapChain.Present();
    }

    void Renderer::Shutdown() {
        if (!Initialized) return;
        CommandQueue.Flush();
        ComputeQueue.Shutdown();
        UploadQueue.Shutdown();
        Nrd.Shutdown();
        Fsr.Shutdown();
        Dlss.Shutdown();
        FDlssPass::ShutdownStreamline();   // desliga o Streamline apos liberar os recursos do DLSS
        if (ConstantBuffer && MappedFrameBase) {
            ConstantBuffer->Unmap(0, nullptr);
            MappedFrameBase = nullptr;
        }
        if (ObjectCB && MappedObjectCB) {
            ObjectCB->Unmap(0, nullptr);
            MappedObjectCB = nullptr;
        }
        Initialized = false;
        LogInfo("Renderer encerrado");
    }
} 
