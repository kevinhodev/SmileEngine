#include "Smile/Graphics/DDGI.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/RTMasks.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kAtlasFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* _Device, u32 _W, u32 _H,
                                           DXGI_FORMAT _Fmt, const char* _Label = nullptr) {
            return GpuResources::CreateTex2D(_Device, _W, _H, _Fmt,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI,
                                             nullptr, 1, 1, _Label);
        }

        struct DDGIInstanceGeo {
            Vec4 BaseColor;
            u32  VertexSrv   = 0; 
            u32  IndexSrv    = 0; 
            u32  AlbedoIndex = 0;
            u32  HasAlbedo   = 0;
            u32  TwoSidedRT  = 0; // = FMaterial::IsTwoSidedForRT (inclui AlphaTest), nao a flag crua
            u32  Flags       = 0;
            f32  AlphaCutoff = 0.5f;
            f32  RoughnessFactor = 0.5f;
            Vec4 EmissiveFactor{ 0.0f, 0.0f, 0.0f, 0.0f }; 
            u32  EmissiveMapIndex = 0;
            u32  MrMapIndex       = 0;
            u32  MetalMapIndex    = 0; // mapa Metalness separado (slot +6)
            u32  RoughMapIndex    = 0; // mapa Roughness separado (slot +7)
        };
        static_assert(sizeof(DDGIInstanceGeo) == 80, "DDGIInstanceGeo deve casar com o HLSL (80B)");

        ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* _Device, UINT64 _Size,
                                                   D3D12_RESOURCE_STATES _State,
                                                   D3D12_RESOURCE_FLAGS _Flags = D3D12_RESOURCE_FLAG_NONE) {
            return GpuResources::CreateBuffer(_Device, _Size, _Flags, _State, EVramCategory::GI,
                                              "DDGI · buffers");
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* _Device, UINT64 _Size,
                                                  u8** _MappedOut) {
            // Sem alinhamento de CB: os chamadores passam o tamanho TOTAL ja multiplicado por
            // kFramesInFlight e fatiam o mapeado na mao pelo FrameSlot.
            GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(_Device, _Size, 1, /*ForConstantBuffer*/ false);
            if (_MappedOut) *_MappedOut = Upload.Mapped;
            return Upload.Resource;
        }
    }

    void FDDGI::Initialize(ID3D12Device* _Device) {
        CreatePipelines(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FDDGI::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(DDGIConstants);
        CB = CreateUploadBuffer(_Device, Size, &MappedCB);
    }

    void FDDGI::ReleaseSceneResources(FTextureSRVHeap& _SRVHeap) {
        auto FreeSlot = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        FreeSlot(AtlasSRVSlot, 1);
        FreeSlot(AtlasUAVSlot, 1);
        FreeSlot(DistSRVSlot, 1);
        FreeSlot(DistUAVSlot, 1);
        FreeSlot(ProbesTraceSRVSlot, 1);
        FreeSlot(ProbesTraceUAVSlot, 1);
        FreeSlot(InstanceSRVSlot, 1);
        if (MeshGeoSlotBase != kInvalidSlot) {
            _SRVHeap.Free(MeshGeoSlotBase, MeshGeoSlotCount);
            MeshGeoSlotBase  = kInvalidSlot;
            MeshGeoSlotCount = 0;
        }
        FreeSlot(ProbeDataSRVSlot, 1);
        FreeSlot(ProbeRayCountSRVSlot, 1);
        FreeSlot(ProbeDataUAVSlot, 2); 
        ProbeRayCountUAVSlot = kInvalidSlot;
        for (u32 i = 0; i < kTraceTables; ++i) FreeSlot(TraceTable[i], 9);
        FreeSlot(SceneGITableStart_, 3);
        FreeSlot(UpdateTableStart, 2);
        IrradAtlas.Reset();
        DistAtlas.Reset();
        ProbesTrace.Reset();
        InstanceGeoBuf.Reset();
        ProbeDataBuf.Reset();
        ProbeRayCountBuf.Reset();
        AtlasState         = D3D12_RESOURCE_STATE_COMMON;
        DistState          = D3D12_RESOURCE_STATE_COMMON;
        ProbesState        = D3D12_RESOURCE_STATE_COMMON;
        ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    // Preenche o snapshot de materiais/geometria que TODO o RT le (DDGI, ReSTIR, reflexoes).
    // Extraido do SetupForScene p/ poder ser REFEITO: editar AlphaTest/TwoSided/emissivo de um
    // material no editor mudava so o constant buffer do material e deixava este snapshot velho.
    void FDDGI::FillInstanceGeo(const FScene& _Scene, u8* _Mapped, u32 _Count) const {
        for (u32 i = 0; i < _Count; ++i) {
            const FRenderable& R = _Scene.Renderables()[i];
            DDGIInstanceGeo g{};
            g.BaseColor = { 0.7f, 0.7f, 0.7f, 1.0f };
            if (R.Material) {
                const MaterialConstants& MC = R.Material->Constants;
                g.BaseColor = MC.BaseColorFactor;
                // Mesmo criterio da flag de culling da TLAS (ver FMaterial::IsTwoSidedForRT):
                // este campo e o que decide, no shader, se um hit pelo verso significa "dentro de
                // solido" — tem que concordar com quem o raio consegue enxergar pelo verso.
                g.TwoSidedRT = R.Material->IsTwoSidedForRT() ? 1u : 0u;
                if (R.Material->IsFinalized() && R.Material->HasAlbedoTexture()) {
                    g.AlbedoIndex = R.Material->AlbedoDescriptorIndex();
                    g.HasAlbedo   = 1;
                }

                g.AlphaCutoff     = MC.AlphaCutoff;
                g.RoughnessFactor = MC.RoughnessFactor;
                // O RTEmissiveScale entra SO aqui: este InstanceGeo e o que os hits de RT leem
                // (DDGI, ReSTIR GI e reflexoes). O raster segue com o EmissiveStrength puro, entao
                // baixar a escala tira a malha de iluminar o ambiente sem apagar o brilho dela na
                // tela. Ver MaterialConstants::RTEmissiveScale.
                const f32 EmiRT = MC.EmissiveStrength * MC.RTEmissiveScale;
                g.EmissiveFactor  = { MC.EmissiveFactor.X * EmiRT,
                                      MC.EmissiveFactor.Y * EmiRT,
                                      MC.EmissiveFactor.Z * EmiRT,
                                      MC.MetallicFactor };
                if (MC.AlphaTest)        g.Flags |= 1u;
                if (MC.ShadingModel == 1) g.Flags |= 4u; // Foliage
                // Categoria da instancia na TLAS (kRTMaskTranslucent). Espelhada aqui porque a
                // mask e propriedade do RAIO: um shader nao consegue perguntar a TLAS com que
                // mask a instancia entrou. Consumida so pelo BvhDebug — os outros passes
                // distinguem translucido pela mask que eles proprios tracam.
                if (R.Material->Blend)   g.Flags |= 128u;
                if (R.Material->IsFinalized()) {
                    // Slots do material: 0=albedo, 1=normal, 2=metallic-roughness, 3=AO, 4=emissive.
                    if (MC.HasEmissiveMap) {
                        g.EmissiveMapIndex = R.Material->AlbedoDescriptorIndex() + 4;
                        g.Flags |= 2u;
                    }
                    if (MC.HasMetallicRoughnessMap) {
                        g.MrMapIndex = R.Material->AlbedoDescriptorIndex() + 2;
                        g.Flags |= 8u;
                        // Empacotamento do mapa: "Specular" poe metal em B, glTF poe em R. Sem
                        // este bit o RT leria o canal errado e um material viraria metal (ou
                        // dielétrico) por engano no bounce.
                        if (MC.SpecularPacking) g.Flags |= 16u;
                    }
                    // Mapa Metalness SEPARADO (slot +6). Precisa existir aqui porque o loader
                    // deixa MetallicFactor = 1 quando ha mapa — sem enxergar o mapa, o RT leria
                    // "metal puro" e zeraria o difuso de um material que e quase todo dieletrico.
                    if (MC.HasMetalnessMap) {
                        g.MetalMapIndex = R.Material->AlbedoDescriptorIndex() + 6;
                        g.Flags |= 32u;
                    }
                    if (MC.HasRoughnessMap) {
                        g.RoughMapIndex = R.Material->AlbedoDescriptorIndex() + 7;
                        g.Flags |= 64u;
                    }
                }
            }
            auto It = R.Mesh ? MeshGeoSlot.find(R.Mesh) : MeshGeoSlot.end();
            if (It != MeshGeoSlot.end()) { g.VertexSrv = It->second; g.IndexSrv = It->second + 1; }
            std::memcpy(_Mapped + i * sizeof(DDGIInstanceGeo), &g, sizeof(DDGIInstanceGeo));
        }
    }

    // Re-upload do snapshot. O chamador (Renderer::NotifyMaterialRTStateChanged) e responsavel
    // por garantir que a GPU nao esteja lendo o buffer — e um upload heap unico, sem versao por
    // frame em voo, entao escrever com frames em voo corromperia o que eles estao lendo.
    void FDDGI::InvalidateRegion(const Vec3& _Min, const Vec3& _Max, EGIRegionChange _Change) {
        // Uniao com o que ja estava pendente: duas edicoes dentro da mesma janela de frames nao
        // podem fazer a segunda cancelar a primeira. A uniao e conservadora (pega sondas a mais),
        // que e o lado seguro — o errado seria deixar de fora uma sonda que precisava atualizar.
        // Une enquanto QUALQUER uma das duas janelas estiver aberta: a caixa e compartilhada, e
        // a do atlas de distancia e a mais longa. Zerar a caixa por causa da janela curta ter
        // fechado deixaria o dist atlas com uma caixa que ja nao descreve o que ele ainda vai
        // misturar.
        if (InvalidateFramesLeft_ > 0 || InvalidateDistFramesLeft_ > 0) {
            InvalidateMin_ = { std::min(InvalidateMin_.X, _Min.X),
                               std::min(InvalidateMin_.Y, _Min.Y),
                               std::min(InvalidateMin_.Z, _Min.Z) };
            InvalidateMax_ = { std::max(InvalidateMax_.X, _Max.X),
                               std::max(InvalidateMax_.Y, _Max.Y),
                               std::max(InvalidateMax_.Z, _Max.Z) };
        } else {
            InvalidateMin_ = _Min;
            InvalidateMax_ = _Max;
        }
        // A caixa guardada aqui e CRUA: o espacamento de folga entra so na hora de mandar para o
        // shader (UpdatePerFrame). Padding aqui dentro se ACUMULA — a caixa pendente ja vinha
        // padded, a chamada seguinte unia com ela e padava de novo, entao a regiao crescia um
        // spacing por lado A CADA CHAMADA. Com uma edicao isolada isso passava despercebido; com
        // arraste de gizmo, que invalida por FRAME, a caixa engoliria o volume em ~1 segundo.
        InvalidateFramesLeft_     = kInvalidateFrames;
        InvalidateDistFramesLeft_ = kInvalidateDistFrames;
        // So GEOMETRIA envelhece a classificacao: ela mede onde a sonda esta em relacao as
        // superficies, e luz nenhuma mexe nisso (ver EGIRegionChange). O flag e OU-acumulado
        // porque a janela e compartilhada — uma edicao de geometria seguida de tres de luz dentro
        // da mesma janela ainda precisa da reclassificacao no fim.
        if (_Change == EGIRegionChange::Geometry) ReclassifyPending_ = true;
    }

    void FDDGI::RefreshInstanceGeo(const FScene& _Scene) {
        if (!InstanceGeoBuf || InstanceGeoCount == 0) return;
        const u32 Count = std::min(InstanceGeoCount,
                                   static_cast<u32>(_Scene.Renderables().size()));
        u8*         Mapped = nullptr;
        D3D12_RANGE NoRead{ 0, 0 };
        if (FAILED(InstanceGeoBuf->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped)))) return;
        FillInstanceGeo(_Scene, Mapped, Count);
        InstanceGeoBuf->Unmap(0, nullptr);
    }

    void FDDGI::SetupForScene(ID3D12Device* _Device, FCommandQueue& _Queue,
                              FTextureSRVHeap& _SRVHeap, const FScene& _Scene,
                              const Vec3& _AABBMin, const Vec3& _AABBMax,
                              u32 _TlasSRVSlot, u32 _SkyViewSRVSlot) {
        if (!Initialized) return;
        ReleaseSceneResources(_SRVHeap);

        const u32 NumRenderables = static_cast<u32>(_Scene.Renderables().size());
        if (NumRenderables == 0 || _TlasSRVSlot == kInvalidSlot) {
            LogDebug("[GI] - DDGI: Cena sem Geometria/TLAS; Volume nao Criado");
            return;
        }

        Vec3 ext = { std::max(_AABBMax.X - _AABBMin.X, 0.1f),
                     std::max(_AABBMax.Y - _AABBMin.Y, 0.1f),
                     std::max(_AABBMax.Z - _AABBMin.Z, 0.1f) };
        f32 maxExt = std::max(ext.X, std::max(ext.Y, ext.Z));
        // kTargetMax = densidade do grid. E o unico parafuso do espacamento: spacing sai de
        // maxExt/(kTargetMax-1), e dele saem as tres contagens. No Bistro, 24 da 8,018 m.
        //
        // O teto sao limites de DIMENSAO de Texture2D (16384) nos tres recursos que escalam com o
        // grid. Com o empacotamento 2D (ver DDGI_TileOrigin e DDGI_TraceTexel) nenhum deles depende
        // mais de um PAR de eixos: os atlas viraram uma grade de tilesPerRow x tileRows e o
        // ProbesTrace uma grade de kTraceProbesPerRow sondas por linha. O que limita agora e o
        // NUMERO DE SONDAS, e o teto e ~1 milhao — antes eram 1024 colunas de sonda no atlas de
        // distancia (o Bistro ja usava 552, e uma segunda cascata nao caberia).
        //
        // A checagem continua em RUNTIME e sobre os valores REAIS: em tempo de compilacao so daria
        // para supor cena cubica, e o pior caso reprovaria grid que cabe folgado. A resposta a nao
        // caber e abrir o espacamento ate caber — perder resolucao de sonda e ruim, criar textura
        // invalida e pior, e era isso que acontecia em silencio.
        constexpr int kTargetMax  = 24;
        constexpr int kMaxPerAxis = 128; // rede secundaria; quem decide e o gridFits abaixo
        constexpr u64 kTexMax     = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        SpacingV = std::max(maxExt / (kTargetMax - 1), 0.5f);
        auto axisCount = [&](f32 e) {
            int n = static_cast<int>(std::ceil(e / SpacingV)) + 1;
            return std::clamp(n, 2, kMaxPerAxis);
        };
        // Layout dos atlas: o plano (x,z) nas colunas e o y nas linhas — a vizinhanca do layout
        // historico —, com o plano enrolado em BANDAS quando nao cabe numa linha. Ver
        // DDGI_TileOrigin: e a forma que preserva o bloco 2x2 de tiles adjacentes que o gather
        // de 8 sondas percorre.
        //
        // A largura alvo e a raiz do numero de sondas (atlas quase quadrado, as duas dimensoes
        // crescendo juntas). Sobre ela, uma preferencia: se houver um DIVISOR de Cols perto do
        // alvo, ele e melhor — banda exata nao deixa coluna sobrando, e o desperdicio cai a zero.
        // No Bistro o alvo e 67 e o divisor e 69 (552 = 69*8), entao a grade fica exata.
        //
        // O shader nao reproduz nada disto: ele le tilesPerRow da LARGURA do atlas. Por isso a
        // heuristica pode ser o que for sem risco de divergir — e por isso ela pode melhorar
        // depois sem tocar em shader nenhum.
        auto tilesPerRowFor = [](u32 Probes, u32 Cols) {
            u32 Target = 1;
            while (Target * Target < Probes) ++Target;
            Target = std::min(Target, std::max(Cols, 1u));
            const u32 Limit = std::min(Cols, Target * 2u);
            for (u32 C = Target; C <= Limit; ++C)
                if (Cols % C == 0) return C;
            return Target;
        };
        auto gridFits = [&] {
            const u64 Probes = static_cast<u64>(CountX) * CountY * CountZ;
            const u64 Cols   = static_cast<u64>(CountX) * CountZ;
            const u64 PerRow = tilesPerRowFor(static_cast<u32>(Probes), static_cast<u32>(Cols));
            const u64 Bands  = (Cols + PerRow - 1) / PerRow;
            const u64 Rows   = Bands * CountY;
            // Maior dimensao de cada recurso. O atlas de distancia (stride 16) e o mais apertado
            // dos dois; o ProbesTrace paga a largura em raios.
            const u64 TraceRow = std::min<u64>(Probes, kTraceProbesPerRow);
            // E o DISPATCH, que nao e recurso e por isso quase ficou de fora: os tres passes
            // pesados sao uma sonda por grupo, e o D3D12 para em 65535 grupos por dimensao. Sem
            // esta linha o gridFits aprovaria, as texturas seriam criadas, e so o Dispatch
            // falharia — em runtime, longe daqui.
            constexpr u64 kMaxGroups = 65535;
            return PerRow * (kDistTileSize + 2) <= kTexMax &&
                   Rows   * (kDistTileSize + 2) <= kTexMax &&
                   TraceRow * kRaysPerProbe <= kTexMax &&
                   (Probes + TraceRow - 1) / TraceRow <= kTexMax &&
                   DispatchGroupsX(static_cast<u32>(Probes)) <= kMaxGroups &&
                   DispatchGroupsY(static_cast<u32>(Probes)) <= kMaxGroups;
        };
        CountX = axisCount(ext.X); CountY = axisCount(ext.Y); CountZ = axisCount(ext.Z);
        if (!gridFits()) {
            const f32 Requested = SpacingV;
            // Converge sempre: o espacamento so cresce, e com ele as contagens caem ate o piso
            // de 2 por eixo (8 sondas), que cabe em qualquer um dos limites.
            while (!gridFits()) {
                SpacingV *= 1.05f;
                CountX = axisCount(ext.X); CountY = axisCount(ext.Y); CountZ = axisCount(ext.Z);
            }
            LogWarning("[GI] - DDGI: espacamento de " + std::to_string(Requested) +
                       " m nao cabe nos atlas (limite de dimensao de textura); aberto para " +
                       std::to_string(SpacingV) + " m");
        }
        GridMinV = { _AABBMin.X - 0.5f * SpacingV, _AABBMin.Y - 0.5f * SpacingV,
                     _AABBMin.Z - 0.5f * SpacingV };
        NumProbes       = static_cast<u32>(CountX) * CountY * CountZ;

        // Grade 2D de tiles. O shader NAO recebe tilesPerRow por cbuffer: ele o recupera da
        // LARGURA (DDGI_TilesPerRow), e as duas contas so batem porque a largura e construida aqui
        // como tilesPerRow*stride. Mexer numa sem a outra desalinha o atlas inteiro em silencio —
        // por isso as quatro dimensoes saem das MESMAS duas variaveis, aqui, e nao em quatro
        // expressoes independentes como antes.
        const u32 Cols  = static_cast<u32>(CountX) * CountZ;
        TilesPerRow     = tilesPerRowFor(NumProbes, Cols);
        TileBands       = (Cols + TilesPerRow - 1) / TilesPerRow;
        const u32 Rows  = TileBands * static_cast<u32>(CountY);
        AtlasWidth      = TilesPerRow * (kTileSize + 2);
        AtlasHeight     = Rows        * (kTileSize + 2);
        DistAtlasWidth  = TilesPerRow * (kDistTileSize + 2);
        DistAtlasHeight = Rows        * (kDistTileSize + 2);
        // Idem para o ProbesTrace: kTraceProbesPerRow sondas por linha, cada uma ocupando
        // kRaysPerProbe texels. O espelho em HLSL e DDGI_TRACE_PROBES_PER_ROW.
        const u32 TraceProbesPerRow = std::min<u32>(NumProbes, kTraceProbesPerRow);
        const u32 TraceRows         = (NumProbes + TraceProbesPerRow - 1) / TraceProbesPerRow;
        MaxRayDist      = std::sqrt(ext.X * ext.X + ext.Y * ext.Y + ext.Z * ext.Z) * 1.5f;

        IrradAtlas  = CreateTex2D(_Device, AtlasWidth, AtlasHeight, kAtlasFormat,
                                  "DDGI · atlas irradiancia");
        DistAtlas   = CreateTex2D(_Device, DistAtlasWidth, DistAtlasHeight, DXGI_FORMAT_R16G16_FLOAT,
                                  "DDGI · atlas distancia");
        ProbesTrace = CreateTex2D(_Device, TraceProbesPerRow * kRaysPerProbe, TraceRows,
                                  kAtlasFormat, "DDGI · raios por sonda");

        MeshGeoSlot.clear(); // membro: sobrevive p/ o RefreshInstanceGeo
        std::vector<const FGpuMesh*> UniqueMeshes;
        for (u32 i = 0; i < NumRenderables; ++i) {
            const FGpuMesh* M = _Scene.Renderables()[i].Mesh;
            if (!M || !M->IsValid() || MeshGeoSlot.count(M)) continue;
            MeshGeoSlot[M] = 0;
            UniqueMeshes.push_back(M);
        }
        MeshGeoSlotCount = static_cast<u32>(UniqueMeshes.size()) * 2;
        MeshGeoSlotBase  = _SRVHeap.Allocate(MeshGeoSlotCount);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC GeoSrv{};
            GeoSrv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            GeoSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            for (u32 i = 0; i < static_cast<u32>(UniqueMeshes.size()); ++i) {
                const FGpuMesh* M      = UniqueMeshes[i];
                const u32       VbSlot = MeshGeoSlotBase + i * 2;

                GeoSrv.Format                     = DXGI_FORMAT_UNKNOWN;
                GeoSrv.Buffer.FirstElement        = M->VertexFirstElement();
                GeoSrv.Buffer.NumElements         = M->VertexCount();
                GeoSrv.Buffer.StructureByteStride = sizeof(Vertex);
                _SRVHeap.CreateSRV(_Device, M->VertexResource(), GeoSrv, VbSlot);
                GeoSrv.Format                     = DXGI_FORMAT_R32_UINT;
                GeoSrv.Buffer.FirstElement        = M->IndexFirstElement();
                GeoSrv.Buffer.NumElements         = M->GetIndexCount();
                GeoSrv.Buffer.StructureByteStride = 0;
                _SRVHeap.CreateSRV(_Device, M->IndexResource(), GeoSrv, VbSlot + 1);
                MeshGeoSlot[M] = VbSlot;
            }
        }
        ProbeDataBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(Vec4),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ProbeRayCountBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(u32),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        // Snapshot com folga (SceneCapacityFor): objeto criado no editor cabe sem realocar
        // descriptor nem refazer o SetupForScene. So os [0, NumRenderables) sao preenchidos —
        // FillInstanceGeo le a lista da cena, e ir alem dela seria leitura fora do vetor.
        // A cauda vai a zero: nenhuma instancia da TLAS aponta para la (a TLAS so tem os
        // reais), mas zero e um estado legivel se algum dia um indice errado chegar ate aqui.
        const u32 GeoCapacity = SceneCapacityFor(NumRenderables);
        u8* GeoMapped = nullptr;
        InstanceGeoBuf = CreateUploadBuffer(_Device,
            static_cast<UINT64>(GeoCapacity) * sizeof(DDGIInstanceGeo), &GeoMapped);
        InstanceGeoCount = GeoCapacity;
        FillInstanceGeo(_Scene, GeoMapped, NumRenderables);
        std::memset(GeoMapped + static_cast<size_t>(NumRenderables) * sizeof(DDGIInstanceGeo),
                    0, static_cast<size_t>(GeoCapacity - NumRenderables) * sizeof(DDGIInstanceGeo));
        InstanceGeoBuf->Unmap(0, nullptr);

        AtlasSRVSlot       = _SRVHeap.Allocate(1);
        AtlasUAVSlot       = _SRVHeap.Allocate(1);
        DistSRVSlot        = _SRVHeap.Allocate(1);
        DistUAVSlot        = _SRVHeap.Allocate(1);
        ProbesTraceSRVSlot = _SRVHeap.Allocate(1);
        ProbesTraceUAVSlot = _SRVHeap.Allocate(1);
        InstanceSRVSlot    = _SRVHeap.Allocate(1);
        ProbeDataSRVSlot     = _SRVHeap.Allocate(1);
        ProbeRayCountSRVSlot = _SRVHeap.Allocate(1);

        const u32 RelocUAVBase = _SRVHeap.Allocate(2);
        ProbeDataUAVSlot     = RelocUAVBase;
        ProbeRayCountUAVSlot = RelocUAVBase + 1;

        D3D12_SHADER_RESOURCE_VIEW_DESC Tex2DSrv{};
        Tex2DSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Tex2DSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Tex2DSrv.Format                  = kAtlasFormat;
        Tex2DSrv.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, IrradAtlas.Get(),  Tex2DSrv, AtlasSRVSlot);
        _SRVHeap.CreateSRV(_Device, ProbesTrace.Get(), Tex2DSrv, ProbesTraceSRVSlot);
        Tex2DSrv.Format = DXGI_FORMAT_R16G16_FLOAT;
        _SRVHeap.CreateSRV(_Device, DistAtlas.Get(), Tex2DSrv, DistSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Tex2DUav{};
        Tex2DUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Tex2DUav.Format        = kAtlasFormat;
        _SRVHeap.CreateUAV(_Device, IrradAtlas.Get(),  Tex2DUav, AtlasUAVSlot);
        _SRVHeap.CreateUAV(_Device, ProbesTrace.Get(), Tex2DUav, ProbesTraceUAVSlot);
        Tex2DUav.Format = DXGI_FORMAT_R16G16_FLOAT;
        _SRVHeap.CreateUAV(_Device, DistAtlas.Get(), Tex2DUav, DistUAVSlot);

        D3D12_SHADER_RESOURCE_VIEW_DESC BufSrv{};
        BufSrv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        BufSrv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        BufSrv.Format                     = DXGI_FORMAT_UNKNOWN;
        BufSrv.Buffer.FirstElement        = 0;
        BufSrv.Buffer.NumElements         = GeoCapacity; // = tamanho do buffer, nao da cena
        BufSrv.Buffer.StructureByteStride = sizeof(DDGIInstanceGeo);
        _SRVHeap.CreateSRV(_Device, InstanceGeoBuf.Get(), BufSrv, InstanceSRVSlot);

        BufSrv.Format                     = DXGI_FORMAT_R32G32B32A32_FLOAT;
        BufSrv.Buffer.NumElements         = NumProbes;
        BufSrv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeDataBuf.Get(), BufSrv, ProbeDataSRVSlot);

        BufSrv.Format                     = DXGI_FORMAT_R32_UINT;
        BufSrv.Buffer.NumElements         = NumProbes;
        BufSrv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeRayCountBuf.Get(), BufSrv, ProbeRayCountSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC BufUav{};
        BufUav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        BufUav.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;
        BufUav.Buffer.FirstElement = 0;
        BufUav.Buffer.NumElements  = NumProbes;
        _SRVHeap.CreateUAV(_Device, ProbeDataBuf.Get(), BufUav, ProbeDataUAVSlot);
        BufUav.Format              = DXGI_FORMAT_R32_UINT;
        _SRVHeap.CreateUAV(_Device, ProbeRayCountBuf.Get(), BufUav, ProbeRayCountUAVSlot);

        D3D12_CPU_DESCRIPTOR_HANDLE Src[8] = {
            _SRVHeap.CpuHandleStaging(_TlasSRVSlot),
            _SRVHeap.CpuHandleStaging(_SkyViewSRVSlot),
            _SRVHeap.CpuHandleStaging(InstanceSRVSlot),
            _SRVHeap.CpuHandleStaging(AtlasSRVSlot),
            // t4 = atlas de distancia (gather completo no 2o bounce). t5 segue filler: aqui o
            // ProbeData ja esta em t6, usado tambem pela origem do raio.
            _SRVHeap.CpuHandleStaging(DistSRVSlot),
            _SRVHeap.CpuHandleStaging(InstanceSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeRayCountSRVSlot),
        };
        UINT DstCount = 8; UINT SrcCounts[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
        for (u32 i = 0; i < kTraceTables; ++i) {
            TraceTable[i] = _SRVHeap.Allocate(9);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[i]);
            _Device->CopyDescriptors(1, &Dst, &DstCount, 8, Src, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        SceneGITableStart_ = _SRVHeap.Allocate(3);
        D3D12_CPU_DESCRIPTOR_HANDLE GDst = _SRVHeap.CpuHandle(SceneGITableStart_);
        D3D12_CPU_DESCRIPTOR_HANDLE GSrc[3] = {
            _SRVHeap.CpuHandleStaging(AtlasSRVSlot),
            _SRVHeap.CpuHandleStaging(DistSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
        };
        UINT GDstCount = 3; UINT GSrcCounts[3] = { 1, 1, 1 };
        _Device->CopyDescriptors(1, &GDst, &GDstCount, 3, GSrc, GSrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        UpdateTableStart = _SRVHeap.Allocate(2);
        D3D12_CPU_DESCRIPTOR_HANDLE UDst = _SRVHeap.CpuHandle(UpdateTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE USrc[2] = {
            _SRVHeap.CpuHandleStaging(ProbesTraceSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
        };
        UINT UDstCount = 2; UINT USrcCounts[2] = { 1, 1 };
        _Device->CopyDescriptors(1, &UDst, &UDstCount, 2, USrc, USrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CPU.GridMinSpacing  = { GridMinV.X, GridMinV.Y, GridMinV.Z, SpacingV };
        CPU.GridCountRays   = { (f32)CountX, (f32)CountY, (f32)CountZ, (f32)kRaysPerProbe };
        CPU.AtlasParams     = { (f32)kTileSize, (f32)AtlasWidth, (f32)AtlasHeight, (f32)NumProbes };
        CPU.DistAtlasParams = { (f32)kDistTileSize, (f32)DistAtlasWidth, (f32)DistAtlasHeight, 0.0f };

        _Queue.ResetForRecording();
        ID3D12GraphicsCommandList* CL = _Queue.List();
        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        Transition(CL, IrradAtlas.Get(), AtlasState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, DistAtlas.Get(),  DistState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float Zero[4] = { 0, 0, 0, 0 };
        CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(AtlasUAVSlot),
                                          _SRVHeap.CpuHandleStaging(AtlasUAVSlot),
                                          IrradAtlas.Get(), Zero, 0, nullptr);
        CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(DistUAVSlot),
                                          _SRVHeap.CpuHandleStaging(DistUAVSlot),
                                          DistAtlas.Get(), Zero, 0, nullptr);

        Transition(CL, ProbeDataBuf.Get(), ProbeDataState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(ProbeDataUAVSlot),
                                          _SRVHeap.CpuHandleStaging(ProbeDataUAVSlot),
                                          ProbeDataBuf.Get(), Zero, 0, nullptr);

        Transition(CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT RayCountInit[4] = { 64, 64, 64, 64 };
        CL->ClearUnorderedAccessViewUint(_SRVHeap.GpuHandle(ProbeRayCountUAVSlot),
                                         _SRVHeap.CpuHandleStaging(ProbeRayCountUAVSlot),
                                         ProbeRayCountBuf.Get(), RayCountInit, 0, nullptr);
        Transition(CL, IrradAtlas.Get(),   AtlasState,     kAtlasRead);
        Transition(CL, DistAtlas.Get(),    DistState,      kAtlasRead);
        Transition(CL, ProbeDataBuf.Get(), ProbeDataState, kAtlasRead); 
        Transition(CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        _Queue.ExecuteAndSync(Lists, 1);

        Ready = true;
        // Os tres recursos acabaram de ser ZERADOS ali em cima, entao o primeiro update tem que
        // SUBSTITUIR, nao misturar. Sem isto ele mistura 1% da estimativa nova com 99% de preto
        // e o volume inteiro converge a 0.99^n: ~300 updates, 5 s a 60 fps, com a cena nascendo
        // sem indireto e clareando sozinha.
        //
        // A relocacao NAO cobre este caso, apesar de marcar sondas com hysteresis 0: ela so
        // marca `wasInactive || bigJump` (DDGIRelocate.cs.hlsl), e pos-setup o ProbeData esta
        // zerado (w = 0, nao e "inativo") e a sonda em ar livre nao se move — logo nao e
        // marcada. So as encostadas em parede escapavam.
        HysteresisResetPending = true;
        RelocateFramesLeft = Relocation ? kRelocateConvergeFrames : 0;
        LogDebug("[GI] - DDGI volume: " + std::to_string(CountX) + "x" + std::to_string(CountY) +
                "x" + std::to_string(CountZ) + " probes (" + std::to_string(NumProbes) +
                "), spacing " + std::to_string(SpacingV) + ", atlas " +
                std::to_string(AtlasWidth) + "x" + std::to_string(AtlasHeight));
    }

    void FDDGI::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _StagingSlot, u32 _FrameSlot) {
        static_assert(kTraceTables == FCommandQueue::kFramesInFlight,
                      "tabela de trace versionada por frame em voo");
        if (!Ready) return;
        // Escreve so na tabela DESTE FrameSlot: a outra pertence ao frame em voo.
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[_FrameSlot] + 8);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FDDGI::UpdatePerFrame(u32 _FrameSlot, const Vec3& _DirToSun, f32 _SunIntensity,
                               const Vec3& _SunColor, u32 _FrameIndex, u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.SunDirIntensity = { _DirToSun.X, _DirToSun.Y, _DirToSun.Z, _SunIntensity };
        // Histerese 0 enquanto houver reset pendente: o update SUBSTITUI o atlas em vez de
        // misturar 99% do conteudo velho (ver ResetHistoryOnce). O flag so e consumido quando o
        // passe realmente roda, no RecordUpdate.
        CPU.SunColorHyst    = { _SunColor.X, _SunColor.Y, _SunColor.Z,
                                HysteresisResetPending ? 0.0f : Hysteresis };
        // O atlas de DISTANCIA tem histerese propria (ver kDistHysteresis) — x/y/z do
        // DistAtlasParams vem do SetupForScene e nao mudam por frame, entao so o w e reescrito.
        // O reset one-shot vale para os dois: apos o SetupForScene os momentos sao ZERO, e com
        // media/2o momento em zero o Chebyshev prende todo tap no piso de 0,05.
        CPU.DistAtlasParams.W = HysteresisResetPending ? 0.0f : kDistHysteresis;
        // Caixa de invalidacao espacial. Como o reset global, so e CONSUMIDA quando o passe roda
        // (o decremento mora no RecordUpdate) — senao um frame em que o DDGI nem atualiza gastaria
        // a janela. w do Min = "ha invalidacao ativa"; w do Max = a hysteresis de dentro dela.
        const bool Invalidating = InvalidateFramesLeft_ > 0;
        // A folga entra AQUI, uma vez, sobre a uniao crua (ver InvalidateRegion): uma sonda
        // ILUMINA o objeto e e iluminada por ele de fora da AABB dele, entao a caixa cresce um
        // espacamento de grid por lado p/ pegar a camada de sondas em volta, que e onde o color
        // bleed aparece. Como e derivada e nao acumulada, N chamadas custam o mesmo pad que uma.
        const f32 Pad = SpacingV;
        CPU.InvalidateMin     = { InvalidateMin_.X - Pad, InvalidateMin_.Y - Pad,
                                  InvalidateMin_.Z - Pad, Invalidating ? 1.0f : 0.0f };
        CPU.InvalidateMaxHyst = { InvalidateMax_.X + Pad, InvalidateMax_.Y + Pad,
                                  InvalidateMax_.Z + Pad, kInvalidateHysteresis };
        // Mesma caixa, janela e histerese proprias (ver kInvalidateDistHysteresis).
        // z: o RecordUpdate so decrementa RelocateFramesLeft na hora de despachar, entao aqui
        // "> 0" ja significa "o passe de classificacao roda neste frame" — o trace usa isso para
        // mandar os 64 raios e nao alimentar o classificador com a propria decimacao.
        CPU.MiscParams3       = { kInvalidateDistHysteresis,
                                  InvalidateDistFramesLeft_ > 0 ? 1.0f : 0.0f,
                                  RelocateFramesLeft > 0 ? 1.0f : 0.0f, 0.0f };
        CPU.TraceParams     = { (f32)_FrameIndex, MaxRayDist, SkyIntensity,
                                RayEps.HitShadowRayBias };
        CPU.RayEpsA         = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                                RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB         = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                                FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.GIDistParams    = { GIHit.DistTile, GIHit.DistAtlasW, GIHit.DistAtlasH,
                                GIHit.SkipModePacked() };
        // .w = piso de roughness do hit: a sonda integra o hit num cosseno e serve o hemisferio
        // inteiro — cache NAO-direcional, mesmo caso do reservoir do ReSTIR GI (ver
        // FGIHitSampling::SecondaryRoughnessMin).
        CPU.GIBiasParams    = { GIHit.BiasScale, GIHit.BiasMax, GIHit.FadeProbes,
                                GIHit.SecondaryRoughnessMin };
        CPU.ReGIRGridMinSlots     = ReGIRParams.GridMinSlots;
        CPU.ReGIRInvCellEnabled   = ReGIRParams.InvCellSizeEnabled;
        CPU.ReGIRGridCountSamples = ReGIRParams.GridCountSamples;
        CPU.ReGIRResources        = ReGIRParams.Resources;
        CPU.RadianceCacheCamCell     = RadianceCacheParams.CameraPosCell;
        CPU.RadianceCacheLodCapFlags = RadianceCacheParams.LodCapacityFlags;
        CPU.RadianceCacheResources   = RadianceCacheParams.Resources;
        CPU.SkyParams             = SkyLutParams;

        // Detector desligado = piso e teto no MESMO kRaysPerProbe, ou seja o DDGI_DesiredRays
        // clampa em 64 e a classificacao vira identidade. Era 64.0f literal nos dois — o vinculo
        // com a largura do grupo do trace so existia no comentario.
        const f32 EffMax = AdaptiveRays ? (f32)MaxRays : (f32)kRaysPerProbe;
        // min(Min, Max): os dois setters sao independentes de proposito (baixar o teto nao deve
        // mexer no piso pelas costas de quem esta ajustando), entao a ordem so pode ser garantida
        // aqui. Invertidos, o `clamp(desired, min, max)` do HLSL devolve o MAX em silencio.
        const f32 EffMin = AdaptiveRays ? (f32)std::min(MinRays, MaxRays) : (f32)kRaysPerProbe;
        CPU.MiscParams      = { Relocation ? 1.0f : 0.0f, DeactivationThreshold, EffMax, EffMin };
        // Marca de "recem-ativado" so quando o Relocate ainda tem >=1 frame agendado DEPOIS
        // deste (a marca precisa do proximo Relocate p/ o auto-demote; orfa = hyst 0 eterno).
        // z = ShadowRayMask (ver ReSTIRGI.cpp: translucido fora dos dois casos).
        // w = detector de mudanca por sonda (rede da invalidacao por evento; so a irradiancia
        // o consome — ver SetAdaptiveHysteresis).
        CPU.MiscParams2     = { (Relocation && RelocateFramesLeft > 1) ? 1.0f : 0.0f,
                                static_cast<f32>(_PunctualLightCount),
                                static_cast<f32>(FoliageShadows ? kRTMaskShadowFull
                                                                : kRTMaskShadowFast),
                                AdaptiveHysteresis ? 1.0f : 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(DDGIConstants),
                    &CPU, sizeof(DDGIConstants));
    }

    void FDDGI::Transition(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _Res,
                           D3D12_RESOURCE_STATES& _State, D3D12_RESOURCE_STATES _After) {
        if (_State == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _Res;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = _State;
        B.Transition.StateAfter  = _After;
        _CL->ResourceBarrier(1, &B);
        _State = _After;
    }

    void FDDGI::TransitionForUpdate(ID3D12GraphicsCommandList* _CL) {
        if (!Ready) return;
        // Sai de kAtlasRead (contem PIXEL) -> so em fila direta. ProbeData fica de fora: o trace
        // ainda LE ele como SRV (offsets de relocation) antes do relocate.
        //
        // Os ATLASES vao para NON_PIXEL, e NAO para UAV: o proximo passe e o TRACE, que os le
        // como SRV (t3 = irradiancia e t4 = distancia) no gather do 2o bounce. Estado de escrita
        // nao se combina com estado de leitura no D3D12, entao le-los em UAV era comportamento
        // indefinido — e isso ja valia para a irradiancia desde que o bounce existe; o atlas de
        // distancia so herdou o problema quando o bounce ganhou o Chebyshev. A promocao para UAV
        // acontece DEPOIS do trace, dentro do RecordUpdate, onde e compute-legal.
        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, IrradAtlas.Get(),  AtlasState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, DistAtlas.Get(),   DistState,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void FDDGI::TransitionForRead(ID3D12GraphicsCommandList* _CL) {
        if (!Ready) return;
        Transition(_CL, IrradAtlas.Get(),   AtlasState,     kAtlasRead);
        Transition(_CL, DistAtlas.Get(),    DistState,      kAtlasRead);
        Transition(_CL, ProbeDataBuf.Get(), ProbeDataState, kAtlasRead);
    }

    void FDDGI::RecordUpdate(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;
        // Daqui em diante o update roda de fato, entao o reset one-shot foi consumido — o CB
        // deste frame ja foi escrito com histerese 0 pelo UpdatePerFrame.
        HysteresisResetPending = false;
        // Idem para a janela da invalidacao espacial: conta FRAMES DE UPDATE, nao frames de
        // aplicacao. Se o DDGI ficar sem atualizar (async, toggle, cena sem TLAS), a janela
        // espera em vez de escoar sem ter misturado nada.
        if (InvalidateFramesLeft_ > 0)     --InvalidateFramesLeft_;
        if (InvalidateDistFramesLeft_ > 0) --InvalidateDistFramesLeft_;

        // Grade 2D de grupos, uma sonda por grupo. Os tres passes abaixo compartilham a MESMA
        // grade, e o shader recalcula X pela mesma formula em vez de recebe-la (ver
        // DDGI_ProbeFromGroup). Com 1D o teto era 65535 sondas — abaixo do que os atlas
        // comportam, e sem sintoma ate o Dispatch falhar.
        const u32 GroupsX = DispatchGroupsX(NumProbes);
        const u32 GroupsY = DispatchGroupsY(NumProbes);

        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbesTraceUAVSlot));
        _CL->Dispatch(GroupsX, GroupsY, 1);

        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, IrradAtlas.Get(),  AtlasState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        UpdatePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpdateTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AtlasUAVSlot));
        _CL->Dispatch(GroupsX, GroupsY, 1);

        Transition(_CL, DistAtlas.Get(), DistState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UpdateDistPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpdateTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DistUAVSlot));
        _CL->Dispatch(GroupsX, GroupsY, 1);

        if (RelocateFramesLeft > 0) {
            --RelocateFramesLeft;
            Transition(_CL, ProbeDataBuf.Get(),     ProbeDataState,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            RelocatePSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ProbesTraceSRVSlot));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbeDataUAVSlot)); 
            _CL->Dispatch((NumProbes + 63) / 64, 1, 1);
            // ProbeData -> kAtlasRead fica no TransitionForRead (estado com PIXEL; e o
            // relocate so roda no caminho sincrono — ver CanRunAsync).
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Reclassificacao pos-edicao, agendada quando a janela de invalidacao FECHA (ver
        // ReclassifyPending_). Fica no fim da funcao de proposito: agendar antes do bloco acima
        // faria o relocate despachar JA, e este RecordUpdate pode estar sendo gravado na fila de
        // compute — onde a transicao do ProbeData a partir de um estado com PIXEL e ilegal. Aqui,
        // o agendamento so vale a partir do proximo frame, que ja e avaliado pelo CanRunAsync.
        //
        // A guarda de trabalho util: sem relocacao e sem raios adaptativos o passe so reescreve
        // zeros e 64, e custaria 6 frames de caminho sincrono para nada.
        if (ReclassifyPending_ && InvalidateFramesLeft_ == 0 && InvalidateDistFramesLeft_ == 0) {
            ReclassifyPending_ = false;
            if (Relocation || AdaptiveRays) TriggerReclassify();
        }
    }

    void FDDGI::CreatePipelines(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "DDGITrace.cs_6_6.cso", 9, 1, true); 
        UpdatePSO.Initialize(_Device, "DDGIUpdate.cs_6_0.cso", 2, 1);
        UpdateDistPSO.Initialize(_Device, "DDGIUpdateDist.cs_6_0.cso", 2, 1);
        RelocatePSO.Initialize(_Device, "DDGIRelocate.cs_6_0.cso", 1, 2);
    }


    FPassShaderStems FDDGI::ShaderStems() const {
        static const char* const kStems[] = { "DDGITrace.cs", "DDGIUpdate.cs", "DDGIUpdateDist.cs", "DDGIRelocate.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FDDGI::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
