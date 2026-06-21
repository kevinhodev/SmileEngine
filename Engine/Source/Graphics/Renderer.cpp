#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <vector>
#include <algorithm>
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
    Renderer::~Renderer() { Shutdown(); }

    void Renderer::Initialize(HWND _hWnd, u32 _Width, u32 _Height) {
        if (Initialized) return;

    #ifdef _DEBUG
        constexpr bool kDebugLayer = true;
    #else
        constexpr bool kDebugLayer = false;
    #endif

        Device.Initialize(kDebugLayer);
        CommandQueue.Initialize(Device.Native(), D3D12_COMMAND_LIST_TYPE_DIRECT);
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
        GBuffer.WriteDepthSRV(Device.Native(), SRVHeap, DepthBuffer.Get()); // 4o slot contiguo = depth
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

        Atmosphere.Initialize(Device.Native(), CommandQueue, SRVHeap,
                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        CloudNoise.Initialize(Device.Native(), CommandQueue, SRVHeap);
        VolumetricClouds.Initialize(Device.Native(), SRVHeap, CloudNoise,
                                    Atmosphere.TransmittanceSRV(), Atmosphere.MultiScatterSRV(),
                                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
                                    SwapChain.GetWidth(), SwapChain.GetHeight());

        Ocean.Initialize(Device.Native(), SRVHeap);
        Water.Initialize(Device.Native(),
                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        Fog.Initialize(Device.Native(), DXGI_FORMAT_R16G16B16A16_FLOAT);

        SunShadows.Initialize(Device.Native(), SRVHeap);

        PostProcessor.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        ObjectPicker.Initialize(Device.Native(), SwapChain.GetWidth(), SwapChain.GetHeight());

        SelectionOutline.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        // Servico de DebugDraw (overlay 3D pos-tonemap; tooling do editor desenha por aqui).
        DebugDraw.Initialize(Device.Native(), FSwapChain::kFormat);

        TemporalAA.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());
        TemporalAA.SetupInputs(Device.Native(), SRVHeap, HDRColorBuffer.Get(), DepthBuffer.Get(), VelocityBuffer.Get());

        // FSR2: cria contexto + textura de output. Render res = RenderWidth/Height; display = swapchain.
        // No-op em Debug (stub). Ativado por UseFsr2 no render loop (substitui o TAA).
        Fsr2.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                        SwapChain.GetWidth(), SwapChain.GetHeight());

        Flicker.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        AO.Initialize(Device.Native());
        AO.SetupForResize(Device.Native(), SRVHeap, DepthSRVSlot, NormalSRVSlot,
                          SwapChain.GetWidth(), SwapChain.GetHeight());

        if (Device.RaytracingSupported()) {
            DDGI.Initialize(Device.Native());
            ReSTIRGI.Initialize(Device.Native());
            Nrd.Initialize(Device.Native()); // B1: cria instancia RELAX_DIFFUSE + loga InstanceDesc
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

        TexDefaultWhite  = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::White);
        TexDefaultNormal = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::FlatNormal);
        TexDefaultORM    = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::ORM);
        TexDefaultBlack  = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::Black);

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
        // GBufferB (slot 1) = OctNormal+Roughness+Metallic, identico ao antigo ReflectionGBuffer.
        Reflections.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            DDGI.InstanceSRV(), DDGI.IrradianceAtlasSRV(), DDGI.VertexSRV(), DDGI.IndexSRV(),
            DepthSRVSlot, GBuffer.SRVSlot(1), HDREnv.BRDFLutSRV());

        // ReSTIR GI: GITexture full-res. Re-setup junto com as reflexoes (mesmo lifecycle: depth/
        // gbuffer recriados no resize invalidam a tabela do trace). Reusa os mesmos slots do DDGI.
        ReSTIRGI.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                             DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        ReSTIRGI.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            DDGI.InstanceSRV(), DDGI.IrradianceAtlasSRV(), DDGI.VertexSRV(), DDGI.IndexSRV(),
            DepthSRVSlot, GBuffer.SRVSlot(1), VelocitySRVSlot);

        // NRD: pool/IO textures em GI-res (= render res; ReSTIR e full-res). Independente do SRVHeap
        // da engine (heaps proprios — isolamento/blindagem).
        Nrd.SetupForResize(Device.Native(), RenderWidth(), RenderHeight());

        // Fase C: pack pipeline + UAVs das IN do NRD + SRV da OUT, no SRVHeap da engine.
        // REBLUR_DIFFUSE_SPECULAR: o pack do GI escreve o sinal DIFUSO + os inputs comuns; o pack da
        // reflexao (SetupNrdSpec) escreve o sinal ESPECULAR. Uma instancia NRD denoisa os dois.
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

        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f }; // velocidade zero = parado
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&VelocityBuffer)));
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
        SceneColorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        ResourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneDepthCopy)));
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
        Water.Recreate(Device.Native(), RT, DS);
        if (Device.RaytracingSupported())
            DDGIDebugPass.Recreate(Device.Native(), RT, DS);
    }

    bool Renderer::ReloadShaders(const std::string& _ChangedStem) {
        if (!Initialized) return false;
        try {
            CommandQueue.Flush();

            // Tabela stem -> recriacao. Cada entrada lista os .cso (sem perfil/extensao)
            // que aquele PSO consome e como reconstrui-lo. Grupo A: subsistemas que ja
            // expoem um ponto de recriacao isolado do PSO. Para estender ao Grupo B,
            // adicione aqui uma entrada apontando para o novo ReloadShaders do subsistema.
            constexpr DXGI_FORMAT RT = DXGI_FORMAT_R16G16B16A16_FLOAT;
            constexpr DXGI_FORMAT DS = DXGI_FORMAT_D32_FLOAT;
            ID3D12Device* Dev = Device.Native();

            struct ShaderReloadEntry {
                std::vector<std::string> Stems;
                std::function<void()>    Recreate;
            };
            const std::vector<ShaderReloadEntry> Table = {
                { { "Triangle.vs", "GBuffer.ps", "DeferredLighting.ps", "DepthNormal.ps" },
                  [&] { PipelineState.RecreatePSO(Dev); } },
                { { "Skybox.vs", "Skybox.ps" },
                  [&] { Skybox.Recreate(Dev, RT, DS); } },
                { { "SkyAtmosphere.vs", "SkyAtmosphere.ps" },
                  [&] { Atmosphere.RecreateSky(Dev, RT, DS); } },
                { { "CloudComposite.vs", "CloudComposite.ps" },
                  [&] { VolumetricClouds.RecreateComposite(Dev, RT, DS); } },
                { { "WaterSurface.vs", "WaterSurface.ps" },
                  [&] { Water.Recreate(Dev, RT, DS); } },
                { { "DDGIDebugProbes.vs", "DDGIDebugProbes.ps", "DDGIDebugVolume.vs",
                    "DDGIDebugVolume.ps", "DDGIDebugRays.vs", "DDGIDebugRays.ps" },
                  [&] { if (Device.RaytracingSupported())
                            DDGIDebugPass.Recreate(Dev, RT, DS); } },
            };

            // Sem stem (ex.: .hlsli incluido por varios shaders) => reload completo.
            if (_ChangedStem.empty()) {
                RecreateAllPSOs();
                LogInfo("Shaders recarregados (reload completo)");
                return true;
            }

            // Procura a entrada que consome esse .cso e recria so ela.
            for (const auto& Entry : Table) {
                if (std::find(Entry.Stems.begin(), Entry.Stems.end(), _ChangedStem)
                        != Entry.Stems.end()) {
                    Entry.Recreate();
                    LogInfo("Shader recarregado: " + _ChangedStem);
                    return true;
                }
            }

            // Stem nao mapeado (Grupo B ainda nao coberto) => fallback de reload completo.
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
        // <1.0 = render menor que o display (FSR2 faz o upscale); >1.0 = SSAA (downsample).
        _Scale = _Scale < 0.33f ? 0.33f : (_Scale > 2.0f ? 2.0f : _Scale);
        if (_Scale == RenderScale) return;
        RenderScale = _Scale;
        if (!Initialized || SwapChain.GetWidth() == 0) return;
        CommandQueue.Flush();
        RecreateInternalTargets();
    }

    // Recria os RTs de CENA na resolucao interna (swapchain * RenderScale). Backbuffer e modulos
    // de saida (PostProcessor, SelectionOutline) ficam na res NATIVA — o PostProcessor amostra o
    // HDR interno via UV e escreve no backbuffer nativo, fazendo o downsample (SSAA).
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
        CreateSceneCopies();

        PostProcessor.Resize(Device.Native(), SRVHeap, SW, SH);    // NATIVO (downsample)
        ObjectPicker.Resize(Device.Native(), RW, RH);
        SelectionOutline.Resize(Device.Native(), SRVHeap, SW, SH); // NATIVO (desenha no backbuffer)

        TemporalAA.Resize(Device.Native(), SRVHeap, RW, RH);
        TemporalAA.SetupInputs(Device.Native(), SRVHeap, HDRColorBuffer.Get(), DepthBuffer.Get(), VelocityBuffer.Get());
        TAARanLastFrame = false;

        // Contexto + output FSR2 dependem das resolucoes -> recria no resize/render-scale (idempotente).
        Fsr2.Initialize(Device.Native(), SRVHeap, RW, RH, SW, SH);
        Flicker.Resize(Device.Native(), SRVHeap, RW, RH);
        FlickerResetPending = true;

        AO.SetupForResize(Device.Native(), SRVHeap, DepthSRVSlot, NormalSRVSlot, RW, RH);

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
        Atmosphere.LoadMoonTexture(Device.Native(), CommandQueue, SRVHeap, _Path);
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

        ObjectPicker.Tick();

        f32 Aspect = SwapChain.GetWidth() > 0 && SwapChain.GetHeight() > 0
                     ? static_cast<f32>(SwapChain.GetWidth()) / static_cast<f32>(SwapChain.GetHeight())
                     : 1.0f;

        Mat44 View       = Camera.GetViewMatrix();

        const f32 NearZ = 0.1f;
        const f32 FarZ  = UseWater ? 20000.0f : 4000.0f;
        const Mat44 ProjUnjittered = kReverseZ
            ? Mat44::PerspectiveFovReverseZLH(60.0f * ToRad, Aspect, NearZ, FarZ)
            : Mat44::PerspectiveFovLH(60.0f * ToRad, Aspect, NearZ, FarZ);

        Mat44 Projection = ProjUnjittered;
        // FSR2 e mutuamente exclusivo com o TAA custom: ligado, ele cuida do AA temporal.
        const bool Fsr2Active = UseFsr2 && Fsr2.IsInitialized();
        const bool TAAActive  = UseTAA && !Fsr2Active && TemporalAA.IsInitialized();
        // Jitter aplicado a projecao; o MESMO offset (pixels) vai depois pro dispatch do FSR2.
        f32 JitterPxX = 0.0f, JitterPxY = 0.0f;
        f32 ProjJitterYSign = 1.0f; // sinal do termo Y do jitter na projecao
        if (Fsr2Active) {
            Fsr2.GetJitter(FrameIndex, JitterPxX, JitterPxY); // sequencia/fase proprias do FSR2
            // Convencao do FSR2: projecao recebe +2jx/w e -2jy/h (Y NEGADO); o dispatch recebe
            // (jx,jy) cru. Sem o sinal a reconstrucao nunca converge em Y -> shimmer global.
            ProjJitterYSign = -1.0f;
        } else if (TAAActive) {
            const u32 kJitterPhases = 8;
            const u32 Idx = (FrameIndex % kJitterPhases) + 1;
            JitterPxX = Halton(Idx, 2) - 0.5f;
            JitterPxY = Halton(Idx, 3) - 0.5f;
        }
        if (Fsr2Active || TAAActive) {
            Projection.M[2][0] += JitterPxX * 2.0f / static_cast<f32>(RenderWidth());
            Projection.M[2][1] += ProjJitterYSign * JitterPxY * 2.0f / static_cast<f32>(RenderHeight());
        }
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

        Vec3 EffectiveSunColor = SunColorRGB;
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            const Vec3 T = Atmosphere.SunTransmittance(SunN);
            EffectiveSunColor = { SunColorRGB.X * T.X, SunColorRGB.Y * T.Y, SunColorRGB.Z * T.Z };
        }
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
        const Vec3 MoonLightCol = { MoonTint.X * MoonTrans.X, MoonTint.Y * MoonTrans.Y, MoonTint.Z * MoonTrans.Z };
        const f32  MoonW        = MoonOn ? (TimeOfDay.MoonIntensity * MoonIllum * NightFactor * MoonUp) : 0.0f;
        MappedCB->MoonDirection = { MoonN.X, MoonN.Y, MoonN.Z, MoonW };
        MappedCB->MoonColor     = { MoonLightCol.X, MoonLightCol.Y, MoonLightCol.Z, 0.0f };

        const f32 MoonHalfAngleRad = 0.5f * ToRad * TimeOfDay.MoonDiskSize;
        const f32 CosMoonRadius    = std::cos(MoonHalfAngleRad);
        const f32 MoonDiskBright = MoonOn ? (Atmosphere.HasMoonTexture() ? 2.5f : 5.0f) : 0.0f;
        Atmosphere.SetNightParams(MoonN, CosMoonRadius, MoonDiskBright,
                                  TimeOfDay.StarIntensity, NightFactor, ElapsedTime);

        const bool KeyIsMoon  = (SunN.Y <= 0.0f);
        const Vec3 KeyDir     = KeyIsMoon ? MoonN : SunN;
        const Vec3 KeyColor   = KeyIsMoon ? MoonLightCol : EffectiveSunColor; 
        const f32  KeyInt     = KeyIsMoon ? MoonW : SunIntensity;
  
        const f32  CloudDim   = KeyIsMoon ? (MoonW / std::max(SunIntensity, 1e-3f)) : 1.0f;
        const Vec3 KeyCloudCol = { KeyColor.X * CloudDim, KeyColor.Y * CloudDim, KeyColor.Z * CloudDim };

        {
            auto Sat = [](f32 X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); };
            const f32 SunY   = SunN.Y;
            const f32 Day    = Sat(SunY * 4.0f + 0.2f);   
            const f32 LowSun = Sat(1.0f - SunY * 2.5f);   
            const Vec3 Zenith  = { 0.18f, 0.30f, 0.55f }; 
            const Vec3 Horizon = { 0.60f, 0.40f, 0.26f }; 
            const Vec3 Sky    = (Zenith + (Horizon - Zenith) * LowSun) * Day;
            const Vec3 Ground = Sky * 0.35f;
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
        ReSTIRGI.SetUseNrd(NrdMode); // afeta o estado final da GITexture no RecordTrace + a tabela t16
        Reflections.SetUseNrd(NrdMode); // NRD denoisa o especular junto; off = denoiser caseiro (fallback)
        // ReflectionParams.w = flag ReSTIR GI: 0=off, 1=on (raw), 2=on + NRD (saida REBLUR em YCoCg ->
        // o DeferredLighting desempacota). ReflectionParams.z = reflexoes ativas (mantido).
        MappedCB->ReflectionParams = { Reflections.GetMaxRoughness(), Reflections.GetRoughnessFade(),
                                       ReflectionsActive ? 1.0f : 0.0f,
                                       ReSTIRGIActive ? (NrdMode ? 2.0f : 1.0f) : 0.0f };
        ++FrameIndex;

        Mat44 ViewNoTrans = View;
        ViewNoTrans.M[3][0] = 0.0f;
        ViewNoTrans.M[3][1] = 0.0f;
        ViewNoTrans.M[3][2] = 0.0f;
        const Mat44 InvVPNoTrans = (ViewNoTrans * Projection).Inverse();
        const Mat44 InvViewProjFull = ViewProjection.Inverse();
        const Mat44 InvViewProjUnjit = ViewProjUnjittered.Inverse();
        MappedCB->InvViewProj = InvViewProjFull; // deferred lighting reconstroi worldPos do depth

        Atmosphere.UpdatePerFrame(FrameSlot, SunN, InvVPNoTrans,
                                  InvViewProjFull, CameraPosition, kKmPerWorldUnit);
        Fog.UpdatePerFrame(FrameSlot, InvViewProjFull, CameraPosition, kKmPerWorldUnit, KeyDir,
                           NearZ, FarZ, RenderWidth(), RenderHeight(),
                           UseAerialPerspective, UseHeightFog, Atmosphere.AerialDepthKm());
        const f32 CloudViewHeight = 6360.0f + FAtmosphere::kGroundAltitudeKm;
        VolumetricClouds.UpdatePerFrame(FrameSlot, InvVPNoTrans, CloudViewHeight, KeyDir, KeyCloudCol,
                                        ElapsedTime, RenderWidth(), RenderHeight());

        const Mat44 WaterViewProj    = ViewProjection;
        const Mat44 WaterInvViewProj = WaterViewProj.Inverse();
        const bool WaterHasDepth = SceneColorCopy && SceneDepthCopy;
        if (UseWater && Water.IsInitialized()) {
            Water.UpdatePerFrame(FrameSlot, WaterViewProj, Projection, WaterInvViewProj, CameraPosition, KeyDir,
                                 KeyInt, KeyColor, ElapsedTime,
                                 HDREnv.HasHDRLoaded(), IBLIntensity,
                                 RenderWidth(), RenderHeight(), NearZ, FarZ,
                                 WaterHasDepth, UseAtmosphereSky);
            if (Ocean.IsInitialized()) {
                Ocean.SetTime(ElapsedTime);
                Ocean.SetWindDirection(Water.GetWindDirection());
                Ocean.SetWindSpeed(Water.GetWindSpeed());
                Ocean.SetAmplitude(Water.GetWavesAmount());
            }
        }

        auto* CommandList = CommandQueue.List();

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        auto DSV = DSVHeap.CpuHandle(0);

        {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Transition.pResource   = HDRColorBuffer.Get();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            CommandList->ResourceBarrier(1, &ResourceBarrier);

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

        if (UseWater && Ocean.IsInitialized()) {
            Ocean.RecordCompute(FrameSlot, CommandList, SRVHeap);
        }

        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            Atmosphere.RecordSkyViewBake(CommandList); 
            Atmosphere.RenderSky(CommandList, SRVHeap);
        } else if (ShowSkybox && HDREnv.HasHDRLoaded()) {
            Skybox.Render(FrameSlot, CommandList, SRVHeap, HDREnv.EnvCubeSRV(),
                          InvVPNoTrans, IBLIntensity, IBLRotation);
        }

        if (Atmosphere.IsInitialized()) {
            Atmosphere.RecordAerialPerspectiveBake(CommandList);
        }

        if (UseGI && DDGI.IsReady()) {
            DDGI.UpdatePerFrame(FrameSlot, KeyDir, KeyInt, KeyColor, FrameIndex);
            DDGI.RecordUpdate(CommandList, SRVHeap);
        }

        if (ReflectionsActive) {
            Reflections.UpdatePerFrame(FrameSlot, InvViewProjFull, PrevViewProj, CameraPosition,
                                       RenderWidth(), RenderHeight(), KeyDir, KeyInt,
                                       KeyColor, FrameIndex, 1.0f, 0.2f, Reflections.GetRealHitShading(),
                                       View);
        }

        if (ReSTIRGIActive) {
            ReSTIRGI.UpdatePerFrame(FrameSlot, InvViewProjFull, CameraPosition,
                                    RenderWidth(), RenderHeight(), KeyDir, KeyInt, KeyColor,
                                    FrameIndex, 1.0f, 0.2f, View);
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
            const size_t PrevCount = PrevModels.size(); // tamanho antes do resize: distingue objeto novo
            PrevModels.resize(RList.size(), Mat44::Identity());
            for (size_t si = 0; si < RList.size(); ++si) {
                const FRenderable& R = RList[si];
                if (!R.Visible || !R.Mesh || !R.Mesh->IsValid()) continue;
                if (AllItems.size() >= MaxObjects) break;
                FMaterial* Mat = (R.Material && R.Material->IsFinalized()) ? R.Material : ActiveMaterial;
                const u32 Slot = FrameObjectBase + static_cast<u32>(AllItems.size());
                const Mat44 Model = R.Transform.Matrix();
                // Motion vector: matrizes SEM jitter. PrevModel do frame anterior (estatico => igual
                // ao atual => so o termo de camera). Objeto novo (si >= PrevCount) -> PrevModel = Model
                // (velocidade zero, sem spike no 1o frame). PrevViewProj ja e a VP unjittered anterior.
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
                PrevModels[si] = Model; // vira o PrevModel do proximo frame
            }
        }

        struct VisItem { const FRenderable* R; FMaterial* Mat; f32 Dist; u32 Slot; u32 SceneIndex; };
        std::vector<VisItem> VisibleScratch;
        VisibleScratch.reserve(AllItems.size());
        for (const AllItem& A : AllItems) {
            if (UseFrustumCulling && AABBOutsideFrustum(A.R->AABBMin, A.R->AABBMax)) continue;
            const f32 cx = (A.R->AABBMin.X + A.R->AABBMax.X) * 0.5f - CamPos.X;
            const f32 cy = (A.R->AABBMin.Y + A.R->AABBMax.Y) * 0.5f - CamPos.Y;
            const f32 cz = (A.R->AABBMin.Z + A.R->AABBMax.Z) * 0.5f - CamPos.Z;
            VisibleScratch.push_back({ A.R, A.Mat, cx*cx + cy*cy + cz*cz, A.Slot, A.SceneIndex });
        }
        std::sort(VisibleScratch.begin(), VisibleScratch.end(),
                  [](const VisItem& a, const VisItem& b) { return a.Dist < b.Dist; });
        LastVisibleCount = static_cast<u32>(VisibleScratch.size());

        {
            const f32 FovY = 60.0f * ToRad; 
            SunShadows.UpdatePerFrame(FrameSlot, UseSunShadows, View, CameraPosition, FovY, Aspect, KeyDir, NearZ);
            if (UseSunShadows) {
                std::vector<FSunShadows::FShadowDrawItem> Casters;
                Casters.reserve(AllItems.size());
                for (const AllItem& A : AllItems)
                    Casters.push_back({ A.R->Mesh, A.Mat,
                                        ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                        A.R->AABBMin, A.R->AABBMax });
                SunShadows.RecordDepthPass(CommandList, SRVHeap, Casters.data(), Casters.size());

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

        const bool AOWillRun = UseAO && AO.IsReady();
        // Deferred: o prepass roda sempre — estabelece o depth opaco que o geometry pass usa em
        // depth EQUAL. Com AO ligado escreve tambem a NormalBuffer (PSODepthNormal).
        const bool DoPrepass = true;
        if (DoPrepass) {
            if (AOWillRun) {
                if (NormalBufferState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    D3D12_RESOURCE_BARRIER NB{};
                    NB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    NB.Transition.pResource   = NormalBuffer.Get();
                    NB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    NB.Transition.StateBefore = NormalBufferState;
                    NB.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    CommandList->ResourceBarrier(1, &NB);
                    NormalBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
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
        }

        if (AO.IsReady()) {
            if (AOWillRun) {
                CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                D3D12_RESOURCE_BARRIER DB{};
                DB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                DB.Transition.pResource   = DepthBuffer.Get();
                DB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                DB.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                DB.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                CommandList->ResourceBarrier(1, &DB);

                const f32 TanHalf = std::tan(0.5f * 60.0f * ToRad);
                const f32 M11 = 1.0f / TanHalf;
                const f32 M00 = M11 / Aspect;
                const f32 M22 = Projection.M[2][2];
                const f32 M32 = Projection.M[3][2];
                AO.UpdatePerFrame(FrameSlot, M00, M11, M22, M32, View,
                                  RenderWidth(), RenderHeight(), FrameIndex);

                {
                    D3D12_RESOURCE_BARRIER NB{};
                    NB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    NB.Transition.pResource   = NormalBuffer.Get();
                    NB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    NB.Transition.StateBefore = NormalBufferState;
                    NB.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    CommandList->ResourceBarrier(1, &NB);
                    NormalBufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
                AO.Execute(CommandList, SRVHeap, DepthSRVSlot);

                DB.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                DB.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                CommandList->ResourceBarrier(1, &DB);

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

        // === Deferred: geometry pass — preenche o G-buffer (opaco depth-EQUAL, folhagem depth-write) ===
        {
            GBuffer.TransitionToWrite(CommandList); // RENDER_TARGET
            if (VelocityState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER VB{};
                VB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                VB.Transition.pResource   = VelocityBuffer.Get();
                VB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                VB.Transition.StateBefore = VelocityState;
                VB.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                CommandList->ResourceBarrier(1, &VB);
                VelocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            // 4 RTs: [A, B, C, Velocity]. SV_Target3 = motion vector.
            D3D12_CPU_DESCRIPTOR_HANDLE GBufRTVs[FGBuffer::kTargetCount + 1] = {
                GBuffer.RTVHandle(0), GBuffer.RTVHandle(1), GBuffer.RTVHandle(2),
                VelocityRTVHeap.CpuHandle(0) };
            CommandList->OMSetRenderTargets(FGBuffer::kTargetCount + 1, GBufRTVs, FALSE, &DSV);
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
                if (Mat->Blend) continue; // translucido -> passe forward (alpha-blend), nao no GBuffer
                const bool TwoSided = Mat->TwoSided || Mat->Constants.AlphaTest;
                ID3D12PipelineState* Want = TwoSided ? PipelineState.PSOGBufferTwoSided()
                                                     : PipelineState.PSOGBuffer();
                if (Want != CurGeomPSO) { CommandList->SetPipelineState(Want); CurGeomPSO = Want; }
                CommandList->SetGraphicsRootConstantBufferView(
                    4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                Mat->Bind(CommandList, SRVHeap);
                V.R->Mesh->Draw(CommandList);
            }
            // Velocity -> leitura (PSR): permanece assim o resto do frame (debug / TAA leem dela).
            D3D12_RESOURCE_BARRIER VB{};
            VB.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            VB.Transition.pResource   = VelocityBuffer.Get();
            VB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            VB.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            VB.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &VB);
            VelocityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // === ReSTIR GI: trace (compute) -> GITexture, ANTES do deferred lighting ================
        // Depth/GBuffer (DEPTH_WRITE/RENDER_TARGET apos o geometry pass) -> NON_PIXEL p/ o compute
        // ler; restaura depois, pois o deferred lighting faz as suas proprias transicoes p/ PIXEL.
        if (ReSTIRGIActive) {
            D3D12_RESOURCE_BARRIER DBar{};
            DBar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            DBar.Transition.pResource   = DepthBuffer.Get();
            DBar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            DBar.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            DBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &DBar);
            GBuffer.TransitionToRead(CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // Velocity (PIXEL apos o geometry pass) -> NON_PIXEL p/ o compute do reuso temporal.
            D3D12_RESOURCE_BARRIER VBar{};
            VBar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            VBar.Transition.pResource   = VelocityBuffer.Get();
            VBar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            VBar.Transition.StateBefore = VelocityState;
            VBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &VBar);
            VelocityState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            ReSTIRGI.RecordTrace(CommandList, SRVHeap);

            // NRD unificado (REBLUR_DIFFUSE_SPECULAR): packing dos 2 sinais -> 1 Denoise. depth/gbuffer/
            // velocity seguem em NON_PIXEL aqui (so restaurados abaixo). O NRD liga heap proprio.
            //   difuso = ReSTIR GI (RecordNrdPack); especular = reflexao (RecordTrace para no Resolved
            //   c/ UseNrd, e RecordNrdPack escreve a IN_SPEC). Os inputs comuns sao do pack do GI.
            if (NrdMode) {
                if (ReflectionsActive) Reflections.RecordTrace(CommandList, SRVHeap); // -> Resolved (NON_PIXEL)
                Nrd.TransitionInputsToWrite(CommandList);
                ReSTIRGI.RecordNrdPack(CommandList, SRVHeap);
                if (ReflectionsActive) Reflections.RecordNrdPack(CommandList, SRVHeap);
                Nrd.SetFrame(ProjUnjittered, NrdPrevProj, View, NrdPrevView,
                             Vec2{ 0.0f, 0.0f }, Vec2{ 0.0f, 0.0f }, FrameIndex);
                Nrd.Denoise(CommandList);
                Nrd.TransitionOutputToRead(CommandList);
                ID3D12DescriptorHeap* ReHeaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(ReHeaps), ReHeaps);
            }

            DBar.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            DBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            CommandList->ResourceBarrier(1, &DBar);
            GBuffer.TransitionToWrite(CommandList);
            // Restaura velocity p/ leitura no pixel shader (TAA/FSR2/debug leem depois).
            VBar.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            VBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &VBar);
            VelocityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // === Deferred: lighting fullscreen — le o G-buffer+depth e ilumina -> HDR ===============
        {
            GBuffer.TransitionToRead(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            D3D12_RESOURCE_BARRIER DepthBar{};
            DepthBar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            DepthBar.Transition.pResource   = DepthBuffer.Get();
            DepthBar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            DepthBar.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            DepthBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &DepthBar);

            auto SceneRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, nullptr); // sem depth no lighting
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<u64>(FrameSlot) * sizeof(FrameConstants));
            // Tabela de material (param 2) religada p/ [GBufferA, GBufferB, GBufferC, Depth] (t0-t3).
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
                // Param 9 (t16): GITexture do ReSTIR; fallback p/ tabela valida quando inativo.
                const u32 ReSTIRTable = ReSTIRGIActive ? ReSTIRGI.GITexSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(9, SRVHeap.GpuHandle(ReSTIRTable));
            }
            CommandList->SetPipelineState(PipelineState.PSODeferredLighting());
            CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            CommandList->IASetVertexBuffers(0, 0, nullptr);
            CommandList->IASetIndexBuffer(nullptr);
            CommandList->DrawInstanced(3, 1, 0, 0);

            // Restaura estados "resting": depth -> DEPTH_WRITE, G-buffer -> RENDER_TARGET.
            DepthBar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            DepthBar.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            CommandList->ResourceBarrier(1, &DepthBar);
            GBuffer.TransitionToWrite(CommandList);
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

            auto Barrier = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After) {
                D3D12_RESOURCE_BARRIER B{};
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.pResource   = R;
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = Before;
                B.Transition.StateAfter  = After;
                CommandList->ResourceBarrier(1, &B);
            };

            const D3D12_RESOURCE_STATES ReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Barrier(DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, ReadState);
            GBuffer.TransitionToRead(CommandList, ReadState); // GBufferB = normal+rough+metal das reflexoes

            // Com NRD: o trace+resolve+denoise ja rodaram no bloco do ReSTIR GI (1 Denoise unificado);
            // aqui so o composite (le a OUT_SPEC do NRD). Sem NRD: caminho caseiro completo.
            if (!NrdMode) Reflections.RecordTrace(CommandList, SRVHeap);
            Reflections.RecordComposite(CommandList, SRVHeap, HDRRTVHeap.CpuHandle(0),
                                        RenderWidth(), RenderHeight());

            Barrier(DepthBuffer.Get(), ReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.TransitionToWrite(CommandList);

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

        
        if (UseWater && Water.IsInitialized() && WaterHasDepth) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr); 

            auto Transition = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES& Cur,
                                  D3D12_RESOURCE_STATES To) {
                if (Cur == To) return;
                D3D12_RESOURCE_BARRIER B{};
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.pResource   = R;
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = Cur;
                B.Transition.StateAfter  = To;
                CommandList->ResourceBarrier(1, &B);
                Cur = To;
            };

            D3D12_RESOURCE_STATES HdrState   = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_RESOURCE_STATES DepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            Transition(HDRColorBuffer.Get(),  HdrState,   D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(DepthBuffer.Get(),     DepthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(SceneColorCopy.Get(),  SceneColorCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(SceneDepthCopy.Get(),  SceneDepthCopyState, D3D12_RESOURCE_STATE_COPY_DEST);

            CommandList->CopyResource(SceneColorCopy.Get(), HDRColorBuffer.Get());
            CommandList->CopyResource(SceneDepthCopy.Get(), DepthBuffer.Get());

            Transition(HDRColorBuffer.Get(),  HdrState,   D3D12_RESOURCE_STATE_RENDER_TARGET);
            Transition(DepthBuffer.Get(),     DepthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Transition(SceneColorCopy.Get(),  SceneColorCopyState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(SceneDepthCopy.Get(),  SceneDepthCopyState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            auto HDR_RTV_Rebind = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV_Rebind, FALSE, &DSV);
        }

        if (UseWater && Water.IsInitialized()) {
            Water.RenderSurface(CommandList, SRVHeap, HDREnv.SpecularSRV(), Ocean.SRVSlot(),
                                Ocean.NormalSRVSlot(), SceneCopyTableStart, Atmosphere.SkyViewSRV());
        }

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            VolumetricClouds.RecordRaymarch(CommandList, SRVHeap);
            VolumetricClouds.Composite(CommandList, SRVHeap);
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
            D3D12_RESOURCE_BARRIER DepthBarrier{};
            DepthBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            DepthBarrier.Transition.pResource   = DepthBuffer.Get();
            DepthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            DepthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            DepthBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &DepthBarrier);

            auto Fog_RTV = HDRRTVHeap.CpuHandle(0); 
            CommandList->OMSetRenderTargets(1, &Fog_RTV, FALSE, nullptr);
            Fog.Execute(CommandList, SRVHeap, DepthSRVSlot, Atmosphere.AerialVolumeSRV());

            DepthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            DepthBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            CommandList->ResourceBarrier(1, &DepthBarrier);
        }

        // === Deferred: visualizacao de debug do G-buffer (o geometry pass ja rodou) =============
        // Sobrescreve o HDR com o canal escolhido. So quando GBufferDebugMode > 0.
        if (GBufferDebugMode > 0 && GBuffer.IsInitialized() && GBufferDebugPass.IsInitialized()) {
            GBuffer.TransitionToRead(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto HDRDbgRTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDRDbgRTV, FALSE, nullptr);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            GBufferDebugPass.Execute(CommandList, SRVHeap, GBuffer.SRVTableStart(), VelocitySRVSlot, GBufferDebugMode);
            GBuffer.TransitionToWrite(CommandList); // volta p/ RENDER_TARGET p/ o proximo frame
        }

        {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Transition.pResource   = HDRColorBuffer.Get();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &ResourceBarrier);
        }

        ID3D12Resource* PostInput    = HDRColorBuffer.Get();
        u32             PostInputSRV = HDRSRVSlot;
        if (TAAActive) {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Transition.pResource   = DepthBuffer.Get();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &ResourceBarrier);

            TemporalAA.Execute(CommandList, SRVHeap, FrameSlot, InvViewProjUnjit, PrevViewProj,
                               TAAHistoryBlend, TAARanLastFrame, TAAVarianceGamma, TAASharpness,
                               TAAMotionBlend, TAAAntiFlicker, TAAStationaryMargin, CameraPosition,
                               NearZ, FarZ, TAADebugMode);

            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            CommandList->ResourceBarrier(1, &ResourceBarrier);

            // O history (OutputResource, SV_Target0) acumula a cor LIMPA sem sharpen; a tela usa
            // sempre o alvo de display (SV_Target1), que carrega o sharpen pos-resolve — ou, em
            // DebugMode > 0, a visualizacao de debug. Assim o sharpen sai do feedback do TAA.
            PostInput    = TemporalAA.DisplayOutputResource();
            PostInputSRV = TemporalAA.DisplayOutputSRVSlot();
            TAARanLastFrame = true;
        } else if (Fsr2Active) {
            // Inputs -> NON_PIXEL_SHADER_RESOURCE (= COMPUTE_READ que o FSR2 declara).
            // HDR ja esta em PSR (transicionado acima); Depth em DEPTH_WRITE; Velocity em PSR.
            D3D12_RESOURCE_BARRIER In[3]{};
            for (auto& B : In) {
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            In[0].Transition.pResource = HDRColorBuffer.Get(); In[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            In[1].Transition.pResource = DepthBuffer.Get();    In[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            In[2].Transition.pResource = VelocityBuffer.Get(); In[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(3, In);

            Fsr2.Dispatch(CommandList, HDRColorBuffer.Get(), DepthBuffer.Get(), VelocityBuffer.Get(),
                          JitterPxX, JitterPxY, NearZ, FarZ, 60.0f * ToRad, LastDeltaTime, false);

            // Volta aos estados de origem (HDR/Velocity -> PSR; Depth -> DEPTH_WRITE).
            D3D12_RESOURCE_BARRIER Out[3]{};
            for (auto& B : Out) {
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            Out[0].Transition.pResource = HDRColorBuffer.Get(); Out[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Out[1].Transition.pResource = DepthBuffer.Get();    Out[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            Out[2].Transition.pResource = VelocityBuffer.Get(); Out[2].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(3, Out);

            PostInput    = Fsr2.OutputResource();
            PostInputSRV = Fsr2.OutputSRVSlot();
            TAARanLastFrame = false;
        } else {
            TAARanLastFrame = false;
        }

        PrevViewProj = ViewProjUnjittered;
        NrdPrevProj  = ProjUnjittered; // prev NAO-jitteradas p/ a reprojecao do NRD no proximo frame
        NrdPrevView  = View;

        if (FlickerMode > 0 && Flicker.IsInitialized()) {
            Flicker.Execute(CommandList, SRVHeap, PostInputSRV, static_cast<f32>(FlickerMode),
                            FlickerScale, FlickerAlpha, FlickerResetPending,
                            RenderWidth(), RenderHeight());
            FlickerResetPending = false;
            PostInput    = Flicker.OutputResource();
            PostInputSRV = Flicker.OutputSRVSlot();
        }

        D3D12_RESOURCE_BARRIER BackBufferBarrier{};
        BackBufferBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        BackBufferBarrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
        BackBufferBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        BackBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        BackBufferBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        CommandList->ResourceBarrier(1, &BackBufferBarrier);

        PostProcessor.Execute(CommandList, SRVHeap, PostInput, SwapChain.CurrentRTV(),
                              PostInputSRV, FrameSlot, SwapChain.GetWidth(), SwapChain.GetHeight());

        if (SelectedIndex >= 0 && SelectedSlot != kInvalidSlot && SelectedMesh
            && SelectionOutline.IsInitialized()) {
            FSelectionOutline::FDrawItem Item{ SelectedMesh, SelectedModel * ViewProjUnjittered };
            SelectionOutline.RecordMask(CommandList, &Item, 1, FrameSlot);
            auto BackRTV = SwapChain.CurrentRTV();
            SelectionOutline.RecordOutline(CommandList, SRVHeap, BackRTV,
                                           SwapChain.GetWidth(), SwapChain.GetHeight(), FrameSlot);
        }

        if (DebugDraw.IsInitialized() && !DebugDraw.Empty()) {
            DebugDraw.Render(CommandList, FrameSlot, ViewProjUnjittered, SwapChain.CurrentRTV(),
                             SwapChain.GetWidth(), SwapChain.GetHeight());
        }
        DebugDraw.Clear(); 

        BackBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        BackBufferBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        CommandList->ResourceBarrier(1, &BackBufferBarrier);

        SMILE_HR(CommandList->Close());
        ID3D12CommandList* CommandLists[] = { CommandList };

        CommandQueue.EndFrame(CommandLists, 1);
        SwapChain.Present();
    }

    void Renderer::Shutdown() {
        if (!Initialized) return;
        CommandQueue.Flush();
        Nrd.Shutdown();  // destroi a instancia NRD antes do device cair
        Fsr2.Shutdown(); // libera as texturas internas do FSR2 antes do device cair
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
