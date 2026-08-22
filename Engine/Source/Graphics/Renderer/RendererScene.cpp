#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RendererCaptureState.h"
#include "Smile/Graphics/Renderer/RendererFrameState.h"
#include "Smile/Graphics/Renderer/RendererSceneState.h"
#include "Smile/Graphics/Backend/RenderBackend.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Graphics/Water/OceanSpectrum.h"
#include "Smile/Graphics/RayTracing/RTMasks.h" // kRTMaskShadowFull: mascara dos shadow rays de direta local
#include "Smile/Graphics/Backend/D3D12/Barriers.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/Renderer/DepthConfig.h"
#include "Smile/Graphics/RayTracing/RayEpsilons.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <exception>
#include <functional>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <type_traits>

namespace Smile {
    FScene& Renderer::GetScene() { return SceneState->Scene; }

    u32 Renderer::GetDrawCount() const {
        return static_cast<u32>(SceneState->Scene.Renderables().size());
    }

    void Renderer::SetDraggingRenderable(u64 _Id) {
        SceneState->DraggingRenderableId = _Id;
    }

    u64 Renderer::GetDraggingRenderable() const {
        return SceneState->DraggingRenderableId;
    }

    void Renderer::RebuildGIVolume() {
        SetupGIForScene(SceneState->BoundsMin, SceneState->BoundsMax);
    }

    Vec3 Renderer::GetCameraPos() const { return SceneState->Camera.GetPosition(); }
    f32 Renderer::GetPitch() const { return SceneState->Camera.GetPitch(); }
    f32 Renderer::GetYaw() const { return SceneState->Camera.GetYaw(); }

    Vec3 Renderer::GetCameraRight() const {
        const Mat44 View = SceneState->Camera.GetViewMatrix();
        return Vec3{ View.M[0][0], View.M[1][0], View.M[2][0] };
    }

    bool Renderer::LoadTerrain(const FTerrainDesc& _Desc) {
        return Terrain.Load(Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap, _Desc);
    }

    void Renderer::UpdateMaterialTextureSlot(FMaterial& _Material, u32 _LocalSlot,
                                             FTexture* _Texture) {
        if (!Initialized || !_Texture || !_Texture->IsValid() || !_Material.IsFinalized()) return;
        _Material.UpdateTextureSlot(Backend->Device.Native(), Backend->SRVHeap, _LocalSlot, _Texture);
    }

    // O build libera recursos ainda usados por ambas as filas; drene direta antes da compute
    // porque o trabalho async espera um fence da direta. Depois de reconstruir uma cena viva,
    // SetupGIForScene deve remontar as tabelas que copiam descriptors do snapshot.
    void Renderer::BuildRaytracingScene() {
        if (!Backend->Device.RaytracingSupported()) return;
        Backend->DirectQueue.Flush();
        Backend->ComputeQueue.WaitIdle();
        RaytracingScene.Build(Backend->Device, Backend->DirectQueue, Backend->SRVHeap,
                              SceneState->Scene);
        SceneState->TlasTransformsVersion = SceneState->Scene.TransformsVersion();
    }

    void Renderer::SetCameraPose(const Vec3& _Pos, f32 _PitchDeg, f32 _YawDeg,
                                 bool _NotifyCameraCut) {
        // A pose permanece imutavel durante o aquecimento de uma captura deterministica.
        if (CaptureState->Session.Busy()) {
            LogWarning("Camera travada: ha uma captura deterministica em andamento");
            return;
        }
        SceneState->Camera.SetPose(_Pos, _PitchDeg, _YawDeg);
        // Nao existe vetor de movimento valido entre poses descontínuas. Em um percurso continuo
        // automatizado, entretanto, a camera anterior e a nova formam justamente o motion vector
        // que queremos medir; o chamador declara qual dos dois contratos esta usando.
        if (_NotifyCameraCut) Settings().NotifyCameraCut();
    }

    void Renderer::SetSelectedObject(int _Index) {
        if (_Index < 0 || _Index >= static_cast<int>(SceneState->Scene.Renderables().size())) {
            ClearSelection();
            return;
        }
        SceneState->Selection = {
            SceneState->Scene.IdAt(static_cast<u32>(_Index)),
            ESceneObject::Renderable,
            static_cast<u32>(_Index)
        };
    }

    int Renderer::GetSelectedObject() const {
        return SceneState->Selection.IsRenderable() ? static_cast<int>(SceneState->Selection.Index) : -1;
    }

    u64 Renderer::GetSelectedObjectId() const {
        return SceneState->Selection.IsRenderable() ? SceneState->Selection.Id : 0ull;
    }

    // Escopadas por tipo: limpar "a selecao de mesh" quando ha uma LUZ selecionada e no-op, nao
    // limpa a luz. Preserva o significado que os call sites do editor ja tinham quando os dois
    // campos eram separados.
    void Renderer::ClearSelection() {
        if (SceneState->Selection.IsRenderable()) SceneState->Selection = {};
    }

    void Renderer::ClearLightSelection() {
        if (SceneState->Selection.IsLight()) SceneState->Selection = {};
    }

    void Renderer::SetSelectedLight(int _Index) {
        if (_Index < 0 || _Index >= static_cast<int>(SceneState->Scene.Lights().size())) {
            ClearLightSelection();
            return;
        }
        // A luz pode nao ter identidade ainda: o editor faz push_back direto e quem atribui e o
        // RenderFrame, no proximo frame. Selecionar e um bom momento para adiantar — sem isso a
        // selecao ficaria com Id 0, ou seja, invalida, ate um frame passar.
        FLight& L = SceneState->Scene.Lights()[static_cast<size_t>(_Index)];
        if (L.Id == 0) L.Id = SceneState->Scene.AllocObjectId();
        SceneState->Selection = { L.Id, ESceneObject::Light, static_cast<u32>(_Index) };
    }

    int Renderer::GetSelectedLight() const {
        return SceneState->Selection.IsLight() ? static_cast<int>(SceneState->Selection.Index) : -1;
    }

    bool Renderer::RemoveRenderable(u64 _Id) {
        // AABB capturada ANTES da remocao: depois dela o objeto nao existe mais e nao ha de onde
        // tirar a regiao que o GI precisa reavaliar.
        const FRenderable* Doomed = SceneState->Scene.FindRenderable(_Id);
        if (!Doomed) return false;
        const Vec3 Min = Doomed->AABBMin, Max = Doomed->AABBMax;
        if (!SceneState->Scene.RemoveRenderable(_Id)) return false;
        OnSceneStructureChanged(&Min, &Max);
        return true;
    }

    u64 Renderer::DuplicateRenderable(u64 _Id) {
        const FRenderable* Added = SceneState->Scene.DuplicateRenderable(_Id);
        if (!Added) return 0;
        // Le ANTES do re-setup, que pode realocar a lista.
        const u64  NewId = Added->Id;
        const Vec3 Min = Added->AABBMin, Max = Added->AABBMax;
        OnSceneStructureChanged(&Min, &Max);
        return NewId;
    }

    void Renderer::NotifyGIRegionChanged(const Vec3& _Min, const Vec3& _Max,
                                         EGIRegionChange _Change) {
        DDGI.InvalidateRegion(_Min, _Max, _Change);
    }

    void Renderer::OnSceneStructureChanged(const Vec3* _ChangedMin, const Vec3* _ChangedMax) {
        const u32 Count = static_cast<u32>(SceneState->Scene.Renderables().size());

        // Regiao que o GI precisa reavaliar. Invalidacao ESPACIAL, no lugar de derrubar o atlas
        // inteiro: o dominio SceneStructure nao carrega mais DDGIAtlas justamente por isto.
        // Geometria por definicao: quem chega aqui criou, removeu ou trocou um renderavel.
        if (_ChangedMin && _ChangedMax)
            DDGI.InvalidateRegion(*_ChangedMin, *_ChangedMax, EGIRegionChange::Geometry);

        // Picks em voo carregam indices e nao sobrevivem a uma mudanca estrutural.
        ObjectPicker.CancelPending();
        SceneState->Selection = SceneState->Scene.FindObject(SceneState->Selection.Id);
        // PreviousModels e indexado pela ordem da cena, que acabou de mudar.
        SceneState->PreviousModels.clear();

        // Estruturas de GPU compartilham a folga de SceneCapacityFor; ao excede-la, todo o setup
        // de cena precisa ser refeito porque TLAS, InstanceGeo e SRVs possuem capacidade fixa.
        const bool NeedsResize = Count > MaxObjects
                              || Count > HiZ.Capacity()
                              || (RaytracingScene.IsBuilt() &&
                                  Count > RaytracingScene.InstanceCapacity())
                              || (TemporalMotion.InstanceCount() > 0 &&
                                  Count > TemporalMotion.InstanceCount())
                              || (RaytracingScene.InstanceGeoCapacity() > 0 &&
                                  Count > RaytracingScene.InstanceGeoCapacity());

        if (NeedsResize) {
            if (Count > MaxObjects) {
                MaxObjects = SceneCapacityFor(Count);
                RecreateObjectCB(); // ja da Flush na fila (e recria a tabela do HiZ)
            }
            HiZ.SetupObjects(Backend->Device.Native(), Backend->SRVHeap, MaxObjects);
            BuildRaytracingScene();
            SetupGIForScene(SceneState->BoundsMin, SceneState->BoundsMax);
        } else {
            // InstanceGeo nao e versionado por frame e pode estar sendo lido nas duas filas.
            Backend->DirectQueue.Flush();
            Backend->ComputeQueue.WaitIdle();
            RaytracingScene.RefreshInstanceGeo(SceneState->Scene);
            // As tasks de MeshLights guardam InstanceIndex e precisam ser reconstruidas.
            RebuildMeshLights();
        }

        Settings().NotifySceneStructureChanged();
    }

    void Renderer::RebuildMeshLights() {
        // SetupForScene substitui buffers lidos pelas filas direta e compute.
        Backend->DirectQueue.Flush();
        Backend->ComputeQueue.WaitIdle();
        MeshLights.SetupForScene(Backend->Device.Native(), Backend->SRVHeap, SceneState->Scene,
                                 RaytracingScene.InstanceGeoSRV());
        SceneState->MeshLightTransformsVersion = SceneState->Scene.TransformsVersion();
        SceneState->MeshLightEmissiveDirty = false;
        // ReSTIR DI copia estes descriptors e precisa acompanhar a troca dos buffers.
        ReSTIRDI.RefreshMeshLightDescriptors(Backend->Device.Native(), Backend->SRVHeap,
                                             MeshLights.LightSRVSlot(),
                                             MeshLights.AliasSRVSlot());
    }

    void Renderer::SetupGIForScene(const Vec3& _AABBMin, const Vec3& _AABBMax) {
        SceneState->BoundsMin = _AABBMin;
        SceneState->BoundsMax = _AABBMax;

        // Uma probe selecionada pertence ao volume anterior; nunca deixa o marcador apontar
        // para o mesmo indice numerico de uma grade recem-criada.
        SetDebugProbeIndex(-1);
        FrameState->PreviousDirectLightPositions.clear();

        if (!Backend->Device.RaytracingSupported() || !RaytracingScene.IsBuilt()) return;
        if (!Atmosphere.IsInitialized()) return;

        // O setup substitui recursos usados pelas duas filas. Drene direta antes da compute,
        // pois o update async do DDGI pode estar esperando um fence da fila direta.
        Backend->DirectQueue.Flush();
        Backend->ComputeQueue.WaitIdle();

        DDGI.SetupForScene(Backend->Device.Native(), Backend->DirectQueue, Backend->SRVHeap,
                           SceneState->Scene, _AABBMin, _AABBMax,
                           RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
                           RaytracingScene.InstanceGeoSRV());
        TemporalMotion.SetupForScene(Backend->Device.Native(), Backend->SRVHeap, SceneState->Scene,
                                     RaytracingScene.TlasSRVSlot(),
                                     RaytracingScene.InstanceGeoSRV());
        ReGIR.SetupForScene(Backend->Device.Native(), Backend->SRVHeap, _AABBMin, _AABBMax, GILightSRVSlot);
        // O cache usa hash de mundo e nao depende dos limites da cena.
        RadianceCache.SetupForScene(Backend->Device.Native(), Backend->SRVHeap);
        // A extracao usa InstanceGeo, portanto roda depois da construcao da cena de RT.
        MeshLights.SetupForScene(Backend->Device.Native(), Backend->SRVHeap, SceneState->Scene,
                                 RaytracingScene.InstanceGeoSRV());
        SceneState->MeshLightTransformsVersion = SceneState->Scene.TransformsVersion();

        SetupReflectionsForScene();

        DDGIDebugPass.SetupForScene(Backend->Device.Native(), Backend->SRVHeap, DDGI.NumProbesCount());
        DDGIDebugPass.SetupPointDiagnosticInputs(
            Backend->Device.Native(), Backend->SRVHeap, GBuffer.SRVTableStart(), DDGI);

        // DDGI/reflexoes so ganham SRV aqui (por cena), DEPOIS do registro feito em
        // RecreateInternalTargets — sem esta 2a passada eles nunca apareciam na lista.
        // Registrar de novo e barato e idempotente: a chave e o nome, entao sobrescreve.
        RegisterDebugTargets();
    }

    void Renderer::CreateIBLDescriptorTable() {
        IBLTableStart = Backend->SRVHeap.Allocate(3);

        D3D12_CPU_DESCRIPTOR_HANDLE DstStart = Backend->SRVHeap.CpuHandle(IBLTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Sources[3] = {
            Backend->SRVHeap.CpuHandleStaging(HDREnv.IrradianceSRV()),
            Backend->SRVHeap.CpuHandleStaging(HDREnv.SpecularSRV()),
            Backend->SRVHeap.CpuHandleStaging(HDREnv.BRDFLutSRV()),
        };
        UINT DstCount = 3;
        UINT SrcCounts[3] = { 1, 1, 1 };
        Backend->Device.Native()->CopyDescriptors(1, &DstStart, &DstCount,
                                          3, Sources, SrcCounts,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    bool Renderer::LoadHDREnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        Backend->DirectQueue.Flush();
        return HDREnv.LoadFromFile(Backend->Device.Native(), Backend->DirectQueue, Backend->SRVHeap, _Path);
    }

    void Renderer::CreateDefaultMaterial() {
        auto* Dev = Backend->Device.Native();

        TexDefaultWhite  = FTexture::CreateDefault(Dev, Backend->UploadQueue, Backend->SRVHeap, EDefaultTexture::White);
        TexDefaultNormal = FTexture::CreateDefault(Dev, Backend->UploadQueue, Backend->SRVHeap, EDefaultTexture::FlatNormal);
        TexDefaultORM    = FTexture::CreateDefault(Dev, Backend->UploadQueue, Backend->SRVHeap, EDefaultTexture::ORM);
        TexDefaultBlack  = FTexture::CreateDefault(Dev, Backend->UploadQueue, Backend->SRVHeap, EDefaultTexture::Black);

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

        DefaultMaterial.Finalize(Dev, Backend->SRVHeap);
        ActiveMaterial = &DefaultMaterial;
    }

    void Renderer::SetMaterial(FMaterial* _Material) {
        ActiveMaterial = (_Material && _Material->IsFinalized()) ? _Material : &DefaultMaterial;
    }

    void Renderer::SetUseWater(bool _Use) {
        if (_Use == UseWater) return;
        if (_Use)
            LogInfo("Oceano/agua ativados (FFT 256^2 + superficie)");
        UseWater = _Use;
        // FRenderSettings invalida OceanTemporal; filtros de tela tratam a disoclusao.
    }

    void Renderer::BuildDefaultScene() {
        FGpuMesh* Sphere = SceneState->Scene.AddMesh(Backend->Device.Native(), FMesh::CreateSphere());

        FRenderable Renderable;
        Renderable.Name     = "Sphere";
        Renderable.Mesh     = Sphere;
        Renderable.Material = nullptr;
        SceneState->Scene.AddRenderable(Renderable);
    }

    void Renderer::CreateConstantBuffer() {
        static_assert(sizeof(FrameConstants) % 256 == 0,
                      "o CB do frame e indexado por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Frame = GpuResources::CreateUploadBuffer(
            Backend->Device.Native(), sizeof(FrameConstants), FCommandQueue::kFramesInFlight);
        ConstantBuffer  = Frame.Resource;
        MappedFrameBase = Frame.Mapped;

        // As duas listas de luz sao lidas como StructuredBuffer (root SRV), nao como CB: o
        // passo e kMaxLights * sizeof(), e arredondar para 256 mudaria o offset por frame.
        const GpuResources::FUploadBuffer Lights = GpuResources::CreateUploadBuffer(
            Backend->Device.Native(), static_cast<u64>(kMaxLights) * sizeof(FGPULight),
            FCommandQueue::kFramesInFlight, false);
        LightBuffer     = Lights.Resource;
        MappedLightBase = Lights.Mapped;

        const GpuResources::FUploadBuffer GILights = GpuResources::CreateUploadBuffer(
            Backend->Device.Native(), static_cast<u64>(kMaxLights) * sizeof(FGPULightGI),
            FCommandQueue::kFramesInFlight, false);
        GILightBuffer     = GILights.Resource;
        MappedGILightBase = GILights.Mapped;

        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) {
            GILightSRVSlot[i] = Backend->SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<UINT64>(i) * kMaxLights;
            Srv.Buffer.NumElements         = kMaxLights;
            Srv.Buffer.StructureByteStride = sizeof(FGPULightGI);
            Backend->SRVHeap.CreateSRV(Backend->Device.Native(), GILightBuffer.Get(), Srv, GILightSRVSlot[i]);
        }

        // O deferred usa root SRV; ReSTIR DI usa sua propria tabela de descriptors.
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) {
            DirectLightSRVSlot[i] = Backend->SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<UINT64>(i) * kMaxLights;
            Srv.Buffer.NumElements         = kMaxLights;
            Srv.Buffer.StructureByteStride = sizeof(FGPULight);
            Backend->SRVHeap.CreateSRV(Backend->Device.Native(), LightBuffer.Get(), Srv, DirectLightSRVSlot[i]);
        }

        RecreateObjectCB();
    }



    void Renderer::SetupReflectionsForScene() {
        if (!Backend->Device.RaytracingSupported()) return;
        if (!GBuffer.IsInitialized() || Targets.DepthSRVSlot == kInvalidSlot) return;

        // A direta local usa o snapshot InstanceGeo (do FRaytracingScene), mas nao usa
        // atlas/probes. Se o snapshot ainda nao existe, o passe degenera para not-ready por conta
        // propria; assim uma troca de cena nunca deixa um alvo direto antigo aparentemente valido.
        TemporalMotion.SetupForResize(Backend->Device.Native(), Backend->SRVHeap, RenderWidth(), RenderHeight(),
                                      Targets.DepthSRVSlot, Targets.VelocitySRVSlot);
        const u32 ReliableVelocitySlot = TemporalMotion.IsReady()
            ? TemporalMotion.MotionSRV() : Targets.VelocitySRVSlot;
        const u32 TemporalTransformSlots[FCommandQueue::kFramesInFlight] = {
            TemporalMotion.TransformSRV(0), TemporalMotion.TransformSRV(1) };
        const u32 TemporalSurfaceSlots[FCommandQueue::kFramesInFlight] = {
            TemporalMotion.SurfaceSRV(0), TemporalMotion.SurfaceSRV(1) };

        ReSTIRDI.SetupForResize(Backend->Device.Native(), Backend->SRVHeap, RenderWidth(), RenderHeight(),
            GBuffer.SRVSlot(0), GBuffer.SRVSlot(1), GBuffer.SRVSlot(2), Targets.DepthSRVSlot,
            ReliableVelocitySlot, RaytracingScene.TlasSRVSlot(), RaytracingScene.InstanceGeoSRV(),
            MeshLights.LightSRVSlot(), MeshLights.AliasSRVSlot(),
            DirectLightSRVSlot, TemporalTransformSlots, TemporalSurfaceSlots);
        // A direta nao depende do volume DDGI. Ela ja vinha antes do early-out que existia aqui;
        // agora ninguem depende, e a ordem so reflete que o pack dela le o TemporalMotion acima.
        SetupNrdDirect();

        // Reflexoes e ReSTIR GI existem sem DDGI; o volume e apenas um fallback opcional.
        // As tabelas usam existencia fisica, enquanto FallbackAvailable controla a leitura.
        const FGIFallbackBindings GIFb = GIFallbackBindingsForSetup();

        Reflections.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                                DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        Reflections.SetupForResize(Backend->Device.Native(), Backend->SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            RaytracingScene.InstanceGeoSRV(), GIFb,
            Targets.DepthSRVSlot, GBuffer.SRVSlot(1), GBuffer.SRVSlot(2), HDREnv.BRDFLutSRV(),
            GBuffer.SRVSlot(0), // GBufferA = BaseColor (tint do metal no reflexo)
            Targets.VelocitySRVSlot,
            Targets.SceneCopyTableStart, Targets.SceneCopyTableStart + 1, Targets.SceneColorMipCount,
            Atmosphere.SkyReflectionSRV(), HDREnv.SpecularSRV(),
            TemporalTransformSlots, TemporalSurfaceSlots);

        ReSTIRGI.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                             DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        ReSTIRGI.SetupForResize(Backend->Device.Native(), Backend->SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            RaytracingScene.InstanceGeoSRV(), GIFb,
            Targets.DepthSRVSlot, GBuffer.SRVSlot(1), ReliableVelocitySlot);

        // O update do cache possui historico de mundo e nao consome velocity nem DDGI.
        RadianceCache.SetupUpdatePass(Backend->Device.Native(), Backend->SRVHeap,
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            RaytracingScene.InstanceGeoSRV(), GIFb,
            Targets.DepthSRVSlot, GBuffer.SRVSlot(1), RenderWidth(), RenderHeight());

        // Timers usam o domínio efetivo de cada dispatch.
        TimerGI.Initialize(Backend->Device.Native(), Backend->SRVHeap,
                           ReSTIRGI.TraceWidth(), ReSTIRGI.TraceHeight());
        TimerReflections.Initialize(Backend->Device.Native(), Backend->SRVHeap,
                                    Reflections.TraceWidth(), Reflections.TraceHeight());

        // Debug da BVH: alvo full-res + a tabela t0/t1 (TLAS + snapshot de instancias). Fica aqui
        // porque este ponto e o unico que roda nos DOIS eventos que invalidam a tabela — resize
        // da tela e reconstrucao da cena de RT (os slots mudam nos dois).
        BvhDebug.Resize(Backend->Device.Native(), Backend->SRVHeap, RenderWidth(), RenderHeight());
        BvhDebug.SetSceneSRVs(Backend->Device.Native(), Backend->SRVHeap,
                              RaytracingScene.TlasSRVSlot(), RaytracingScene.InstanceGeoSRV());

        SetupNrdIndirect();

        // Depois do NRD: o SRV do ReSTIR nasce acima e o da OUT do NRD so existe apos o
        // SetupNrdPack. Registrar antes deixava a saida do denoiser fora da lista p/ sempre.
        RegisterDebugTargets();
    }

    bool Renderer::WantNrdIndirect() const {
        // Alocacao segue o modo pedido, que muda apenas entre frames e dispara Reconcile.
        // O modo efetivo pode degradar sem setter e nao governa o lifetime dos pools.
        return Denoiser == EDenoiser::NRD && UseReSTIRGI &&
               IndirectPrimary == EIndirectPrimary::ReSTIR_SHaRC;
    }

    FGIFallbackBindings Renderer::GIFallbackBindingsForSetup() const {
        // Descriptors ficam latched no setup e seguem a existencia fisica do volume.
        // A habilitacao por frame pertence a FallbackAvailable e nao remonta tabelas.
        if (DDGI.IsReady()) {
            FGIFallbackBindings B{};
            B.IrradianceAtlasSRV = DDGI.IrradianceAtlasSRV();
            B.DistanceAtlasSRV   = DDGI.DistAtlasSRV();
            B.ProbeDataSRV       = DDGI.ProbeDataSRV();
            B.Available          = true;
            return B;
        }
        return GIFallback.Bindings();
    }

    bool Renderer::WantNrdDirect() const {
        return Denoiser == EDenoiser::NRD && UseReSTIRDI;
    }

    void Renderer::SetupNrdDirect() {
        if (!WantNrdDirect()) { NrdDirect.ReleaseResources(); return; }
        const u32 ReliableVelocitySlot = TemporalMotion.IsReady()
            ? TemporalMotion.MotionSRV() : Targets.VelocitySRVSlot;
        NrdDirect.SetupForResize(Backend->Device.Native(), RenderWidth(), RenderHeight());
        if (!NrdDirect.IsReady()) return;
        ReSTIRDI.SetupNrdPack(Backend->Device.Native(), Backend->SRVHeap,
            GBuffer.SRVSlot(0), GBuffer.SRVSlot(1), GBuffer.SRVSlot(2),
            Targets.DepthSRVSlot, ReliableVelocitySlot,
            NrdDirect.IoResource(FNrdDenoiser::IO_VIEWZ),
            NrdDirect.IoResource(FNrdDenoiser::IO_NORMAL_ROUGHNESS),
            NrdDirect.IoResource(FNrdDenoiser::IO_MV),
            NrdDirect.IoResource(FNrdDenoiser::IO_DIFF_RADIANCE_HITDIST),
            NrdDirect.IoResource(FNrdDenoiser::IO_SPEC_RADIANCE_HITDIST),
            NrdDirect.IoResource(FNrdDenoiser::IO_OUT_DIFF),
            NrdDirect.IoResource(FNrdDenoiser::IO_OUT_SPEC));
    }

    void Renderer::SetupNrdIndirect() {
        if (!WantNrdIndirect()) { Nrd.ReleaseResources(); return; }
        Nrd.SetupForResize(Backend->Device.Native(), RenderWidth(), RenderHeight());
        if (!Nrd.IsReady()) return;
        ReSTIRGI.SetupNrdPack(Backend->Device.Native(), Backend->SRVHeap,
            Nrd.IoResource(FNrdDenoiser::IO_VIEWZ),
            Nrd.IoResource(FNrdDenoiser::IO_NORMAL_ROUGHNESS),
            Nrd.IoResource(FNrdDenoiser::IO_MV),
            Nrd.IoResource(FNrdDenoiser::IO_DIFF_RADIANCE_HITDIST),
            Nrd.IoResource(FNrdDenoiser::IO_OUT_DIFF));
        Reflections.SetupNrdSpec(Backend->Device.Native(), Backend->SRVHeap,
            Nrd.IoResource(FNrdDenoiser::IO_SPEC_RADIANCE_HITDIST),
            Nrd.IoResource(FNrdDenoiser::IO_OUT_SPEC));
    }

    void Renderer::ReconcileNrdAllocation() {
        if (!Initialized || RenderWidth() == 0 || RenderHeight() == 0) return;
        if (WantNrdIndirect() == Nrd.IsReady() && WantNrdDirect() == NrdDirect.IsReady()) return;
        // Os packs guardam descriptors dos recursos NRD; eles nao podem mudar em voo.
        Backend->DirectQueue.Flush();
        SetupNrdDirect();
        SetupNrdIndirect();
        // A OUT do NRD entra e sai da lista do visualizador junto com a alocacao.
        RegisterDebugTargets();
    }


    void Renderer::UpdateCamera(const CameraInput& _Input, f32 _DeltaTime) {
        // Durante a captura, fase de animacao e camera congelam; processos de convergencia usam
        // um delta fixo para manter o aquecimento reproduzivel.
        if (CaptureState->Session.Busy()) {
            FrameState->LastDeltaTime = FRendererCaptureState::kDeltaSeconds;
            return;
        }
        SceneState->Camera.Update(_Input, _DeltaTime);
        FrameState->ElapsedTime  += _DeltaTime;
        FrameState->LastDeltaTime = _DeltaTime;
    }

    void Renderer::SetSunDirection(const Vec3& _Direction) {
        SunDir = _Direction.NormalizedSafe(DefaultSunDirection());
    }

    void Renderer::SetSunAzimuthElevation(f32 _AzimuthDeg, f32 _ElevationDeg) {
        const f32 Az = _AzimuthDeg   * ToRad;
        const f32 El = _ElevationDeg * ToRad;
        const f32 CosEl = std::cos(El);
        SetSunDirection(Vec3{ CosEl * std::sin(Az), std::sin(El), CosEl * std::cos(Az) });
    }

    void Renderer::LoadMoonTexture(const std::wstring& _Path) {
        if (!Initialized || !Atmosphere.IsInitialized()) return;
        Atmosphere.LoadMoonTexture(Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap, _Path);
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
            ? FTexture::LoadDDS(Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap, _Path, _sRGB)
            : FTexture::CreateFromCPU(Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap,
                                      FTexture::LoadCPU(_Path, _IsNormalMap, _sRGB));
        if (!Tex.IsValid()) return nullptr;

        ImportedTextures.push_back(std::make_unique<FTexture>(std::move(Tex)));
        return ImportedTextures.back().get();
    }

    FMaterialPreview::ESubmitResult Renderer::SubmitMaterialPreview(
        FMaterial* _Material, const FMaterialPreview::FParams& _Params, u64 _RequestId) {
        if (!Initialized || !_Material) return FMaterialPreview::ESubmitResult::Failed;

        const FGpuMesh* SceneMesh = nullptr;
        Mat44 SceneModel = Mat44::Identity();
        if (_Params.Primitive == FMaterialPreview::PrimSceneMesh) {
            const auto& Rnds = SceneState->Scene.Renderables();
            const FRenderable* Pick = nullptr;
            const int Sel = GetSelectedObject();
            if (Sel >= 0 && Sel < (int)Rnds.size() &&
                Rnds[Sel].Material == _Material && !Rnds[Sel].RaytracingOnly)
                Pick = &Rnds[Sel];
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

        return MaterialPreview.Submit(Backend->Device.Native(), Backend->DirectQueue, Backend->SRVHeap,
                                      *_Material, _Params, _RequestId, SceneMesh, SceneModel);
    }

    bool Renderer::LoadMaterialPreviewEnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        // O HDRI e seus descritores sao compartilhados por jobs de preview. Drenar aqui e raro
        // (browse explicito) e impede sobrescrever a tabela enquanto uma list assincrona a usa.
        Backend->DirectQueue.Flush();
        return MaterialPreview.LoadEnvironment(Backend->Device.Native(), Backend->DirectQueue, Backend->SRVHeap, _Path);
    }

    void Renderer::LoadStarCatalog(const std::wstring& _Path) {
        if (!Initialized || !Atmosphere.IsInitialized()) return;
        Atmosphere.LoadStarCatalog(Backend->Device.Native(), Backend->SRVHeap, _Path);
    }

    bool Renderer::WorldToScreen(const Vec3& _W, f32& _Sx, f32& _Sy) const {
        const Mat44& M = FrameState->LastViewProj;
        const f32 cx = _W.X*M.M[0][0] + _W.Y*M.M[1][0] + _W.Z*M.M[2][0] + M.M[3][0];
        const f32 cy = _W.X*M.M[0][1] + _W.Y*M.M[1][1] + _W.Z*M.M[2][1] + M.M[3][1];
        const f32 cw = _W.X*M.M[0][3] + _W.Y*M.M[1][3] + _W.Z*M.M[2][3] + M.M[3][3];
        if (cw <= 1e-5f) return false;
        const f32 ndcx = cx / cw, ndcy = cy / cw;
        _Sx = (ndcx * 0.5f + 0.5f) * static_cast<f32>(Backend->SwapChain.GetWidth());
        _Sy = (0.5f - ndcy * 0.5f) * static_cast<f32>(Backend->SwapChain.GetHeight());
        return true;
    }

    bool Renderer::ScreenToRay(u32 _X, u32 _Y, Vec3& _O, Vec3& _D) const {
        const u32 Wd = Backend->SwapChain.GetWidth(), Ht = Backend->SwapChain.GetHeight();
        if (Wd == 0 || Ht == 0) return false;
        const f32 ndcx = ((static_cast<f32>(_X) + 0.5f) / Wd) * 2.0f - 1.0f;
        const f32 ndcy = 1.0f - ((static_cast<f32>(_Y) + 0.5f) / Ht) * 2.0f;
        const Mat44 Inv = FrameState->LastViewProj.Inverse();
        const f32 v[4] = { ndcx, ndcy, 0.5f, 1.0f };
        f32 w[4];
        for (int j = 0; j < 4; ++j)
            w[j] = v[0]*Inv.M[0][j] + v[1]*Inv.M[1][j] + v[2]*Inv.M[2][j] + v[3]*Inv.M[3][j];
        if (std::fabs(w[3]) < 1e-9f) return false;
        const Vec3 WorldPt{ w[0]/w[3], w[1]/w[3], w[2]/w[3] };
        _O = SceneState->Camera.GetPosition();
        _D = (WorldPt - _O).NormalizedSafe(Vec3::UnitZ());
        return true;
    }

}
