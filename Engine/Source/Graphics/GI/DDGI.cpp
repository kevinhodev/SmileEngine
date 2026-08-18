#include "Smile/Graphics/GI/DDGI.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/RayTracing/RTMasks.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Mesh.h"
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
        ProbeDataBuf.Reset();
        ProbeRayCountBuf.Reset();
        AtlasState         = D3D12_RESOURCE_STATE_COMMON;
        DistState          = D3D12_RESOURCE_STATE_COMMON;
        ProbesState        = D3D12_RESOURCE_STATE_COMMON;
        ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

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

    void FDDGI::SetupForScene(ID3D12Device* _Device, FCommandQueue& _Queue,
                              FTextureSRVHeap& _SRVHeap, const FScene& _Scene,
                              const Vec3& _AABBMin, const Vec3& _AABBMax,
                              u32 _TlasSRVSlot, u32 _SkyViewSRVSlot, u32 _InstanceGeoSRVSlot) {
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

        // Fixado ANTES do dimensionamento: o gridFits mede atlas, ProbesTrace e dispatch pelo
        // TOTAL de sondas, que e por cascata vezes esta contagem. Dimensionar pelo teto
        // (kMaxCascades) engrossaria o espacamento de quem usa uma so; dimensionar por 1 e depois
        // acender a segunda estouraria em silencio. Trocar a contagem exige refazer o
        // SetupForScene, que e o que realoca os recursos.
        CascadeCount_ = DesiredCascades < 1 ? 1u
                      : (DesiredCascades > kMaxCascades ? kMaxCascades : DesiredCascades);

        Cascades[0].Spacing = std::max(maxExt / (kTargetMax - 1), 0.5f);
        auto axisCount = [&](f32 e) {
            int n = static_cast<int>(std::ceil(e / Cascades[0].Spacing)) + 1;
            return std::clamp(n, 2, kMaxPerAxis);
        };
        // Layout dos atlas: o plano (x,z) nas colunas e o y nas linhas — a vizinhanca do layout
        // historico —, com o plano enrolado em BANDAS quando nao cabe numa linha. Ver
        // DDGI_TileOrigin: e a forma que preserva o bloco 2x2 de tiles adjacentes que o gather
        // de 8 sondas percorre.
        //
        // A banda tem de conter fileiras Z INTEIRAS: `TilesPerRow = CountX * zRowsPerBand`. Nao e
        // arredondamento, e a condicao da vizinhanca — o plano e `x + z*CountX`, entao uma largura
        // que nao seja multipla de CountX corta uma fileira X no meio e joga `x` e `x+1` em bandas
        // diferentes, que e exatamente a adjacencia pela qual este layout existe.
        //
        // A versao anterior procurava um DIVISOR de CountX*CountZ perto da raiz de NumProbes. No
        // Bistro caiu em 69 = 3*23 e funcionou — por sorte: 552 tem divisores como 92 e 276 que
        // nao sao multiplos de 23, e ali a vizinhanca se perderia de novo. Ela tambem podia
        // devolver mais de 1024 colunas (o teto do atlas de distancia) e fazer o gridFits recusar
        // um grid que caberia com outra largura, abrindo o espacamento sem necessidade.
        //
        // CountZ nao passa de algumas dezenas, entao enumerar todos os candidatos custa nada.
        // Criterio: minimizar a MAIOR dimensao — empurra para o quadrado e penaliza o desperdicio
        // da banda incompleta de uma vez so, alem de ser exatamente o que a checagem de limite
        // olha.
        //
        // O shader nao reproduz nada disto: ele le tilesPerRow da LARGURA do atlas. Por isso a
        // heuristica pode mudar sem risco de divergir, e sem tocar em shader nenhum.
        // `Cascades` entra no criterio porque a altura REAL do atlas e RowsPerCascade x cascatas.
        // Otimizar so o bloco de uma cascata escolheria um formato quadrado por cascata e alto
        // demais no total — e podia fazer o gridFits recusar um grid que outro empacotamento
        // acomodaria, que e o mesmo erro que o seletor de bandas ja tinha cometido uma vez.
        auto atlasGridFor = [](u32 CX, u32 CY, u32 CZ, u32 Cascades_, u32& OutPerRow,
                               u32& OutRows) {
            CX = std::max(CX, 1u); CY = std::max(CY, 1u); CZ = std::max(CZ, 1u);
            Cascades_ = std::max(Cascades_, 1u);
            OutPerRow = CX; OutRows = CY * CZ; // zRowsPerBand = 1
            u64 Best  = 0;
            for (u32 ZRows = 1; ZRows <= CZ; ++ZRows) {
                const u32 PerRow = CX * ZRows;
                const u32 Rows   = ((CZ + ZRows - 1) / ZRows) * CY;
                const u64 Score  = std::max<u64>(PerRow, static_cast<u64>(Rows) * Cascades_);
                if (Best == 0 || Score < Best) { Best = Score; OutPerRow = PerRow; OutRows = Rows; }
            }
        };
        auto gridFits = [&] {
            // TUDO que segue e por CASCATA ou TOTAL, e confundir os dois foi o jeito obvio de
            // errar: a grade de tiles e por cascata (a altura multiplica), mas atlas, ProbesTrace,
            // buffers e dispatch veem o TOTAL. Os nomes carregam a distincao.
            const u64 PerCascade = static_cast<u64>(CountX) * CountY * CountZ;
            const u64 Probes     = PerCascade * CascadeCount_;
            u32 PerRow32 = 1, RowsPerCascade32 = 1;
            atlasGridFor(static_cast<u32>(CountX), static_cast<u32>(CountY),
                         static_cast<u32>(CountZ), CascadeCount_, PerRow32, RowsPerCascade32);
            const u64 PerRow = PerRow32;
            const u64 Rows   = static_cast<u64>(RowsPerCascade32) * CascadeCount_;
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
            const f32 Requested = Cascades[0].Spacing;
            // Converge sempre: o espacamento so cresce, e com ele as contagens caem ate o piso
            // de 2 por eixo (8 sondas), que cabe em qualquer um dos limites.
            while (!gridFits()) {
                Cascades[0].Spacing *= 1.05f;
                CountX = axisCount(ext.X); CountY = axisCount(ext.Y); CountZ = axisCount(ext.Z);
            }
            LogWarning("[GI] - DDGI: espacamento de " + std::to_string(Requested) +
                       " m nao cabe nos atlas (limite de dimensao de textura); aberto para " +
                       std::to_string(Cascades[0].Spacing) + " m");
        }
        // A cascata que cobre a cena inteira e a ULTIMA. O SetupForScene dimensiona ESSA — as
        // finas saem dela por escala e seguem a CAMERA (6.2b-ii), nao a AABB. Aqui todas nascem
        // iguais a grossa; quem as reposiciona por frame e o PrepareCascadePlacement.
        const f32 CoarseSpacing = Cascades[0].Spacing;
        const Vec3 CoarseMin{ _AABBMin.X - 0.5f * CoarseSpacing,
                              _AABBMin.Y - 0.5f * CoarseSpacing,
                              _AABBMin.Z - 0.5f * CoarseSpacing };
        // Origem em celulas zerada nas duas pontas (atual e anterior): o volume nasce sem scroll
        // pendente. A grossa fica assim para sempre — ela nao rola, entao delta 0 e o endereco
        // dela e bit a bit o de antes do scrolling existir.
        for (u32 C = 0; C < kMaxCascades; ++C) {
            Cascades[C] = FCascade{};
            Cascades[C].GridMin = CoarseMin;
            Cascades[C].Spacing = CoarseSpacing;
        }

        ProbesPerCascade_ = static_cast<u32>(CountX) * CountY * CountZ;
        NumProbes         = ProbesPerCascade_ * CascadeCount_;

        // Grade 2D de tiles. O shader NAO recebe tilesPerRow por cbuffer: ele o recupera da
        // LARGURA (DDGI_TilesPerRow), e as duas contas so batem porque a largura e construida aqui
        // como tilesPerRow*stride. Mexer numa sem a outra desalinha o atlas inteiro em silencio —
        // por isso as quatro dimensoes saem das MESMAS duas variaveis, aqui, e nao em quatro
        // expressoes independentes como antes.
        //
        // As cascatas empilham em blocos de LINHAS (ver DDGI_TileOrigin): a grade de uma cascata
        // e sempre a mesma, e a altura total multiplica pela contagem.
        atlasGridFor(static_cast<u32>(CountX), static_cast<u32>(CountY),
                     static_cast<u32>(CountZ), CascadeCount_, TilesPerRow, TileRowsPerCascade);
        const u32 TileRows = TileRowsPerCascade * CascadeCount_;
        AtlasWidth      = TilesPerRow * (kTileSize + 2);
        AtlasHeight     = TileRows    * (kTileSize + 2);
        DistAtlasWidth  = TilesPerRow * (kDistTileSize + 2);
        DistAtlasHeight = TileRows    * (kDistTileSize + 2);
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

        ProbeDataBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(Vec4),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ProbeRayCountBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(u32),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        AtlasSRVSlot       = _SRVHeap.Allocate(1);
        AtlasUAVSlot       = _SRVHeap.Allocate(1);
        DistSRVSlot        = _SRVHeap.Allocate(1);
        DistUAVSlot        = _SRVHeap.Allocate(1);
        ProbesTraceSRVSlot = _SRVHeap.Allocate(1);
        ProbesTraceUAVSlot = _SRVHeap.Allocate(1);
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
        BufSrv.Buffer.FirstElement        = 0;
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
            _SRVHeap.CpuHandleStaging(_InstanceGeoSRVSlot),
            _SRVHeap.CpuHandleStaging(AtlasSRVSlot),
            // t4 = atlas de distancia (gather completo no 2o bounce). t5 segue filler: aqui o
            // ProbeData ja esta em t6, usado tambem pela origem do raio.
            _SRVHeap.CpuHandleStaging(DistSRVSlot),
            _SRVHeap.CpuHandleStaging(_InstanceGeoSRVSlot),
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

        // A GROSSA, nao a cascata 0: quem le este campo sem saber de cascata (fade de borda,
        // alcance, folga da invalidacao) quer dizer "o volume da cena". Ver FDDGI::GridMin.
        const FCascade& Coarse = Cascades[CoarseCascade()];
        CPU.GridMinSpacing  = { Coarse.GridMin.X, Coarse.GridMin.Y, Coarse.GridMin.Z,
                                Coarse.Spacing };
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
        // Com relocacao ligada os 180 frames ja classificam; sem ela, quem escreve o ProbeRayCount
        // e so este passe (ver TriggerReclassify), e o buffer acabou de nascer em 64 para todas as
        // sondas — o que e o valor CERTO com raios adaptativos desligados e o teto com eles
        // ligados. Os kReclassifyFrames existem para o segundo caso: sem eles a contagem ficaria
        // congelada no maximo e o knob nasceria inerte no volume novo.
        RelocateFramesLeft = Relocation ? kRelocateConvergeFrames
                                        : (AdaptiveRays ? kReclassifyFrames : 0);
        // O estado REGIONAL pertence ao volume que acabou de morrer. A caixa esta em coordenadas
        // de uma grade que nao existe mais, e os contadores mandariam o atlas RECEM-ZERADO passar
        // dezenas de frames com histerese reduzida numa regiao arbitraria da grade nova — os dois
        // decrementos contam frames de UPDATE, entao a janela atravessa o rebuild inteira.
        //
        // Nada se perde: o reset one-shot acima e estritamente mais forte que qualquer invalidacao
        // regional pendente (ele substitui o volume INTEIRO, nao so a caixa). Pelo mesmo motivo a
        // reclassificacao pendente cai junto — a linha acima ja agenda a classificacao global de
        // que o volume novo precisa, e o pedido velho descrevia uma edicao noutra grade.
        InvalidateFramesLeft_     = 0;
        InvalidateDistFramesLeft_ = 0;
        InvalidateMin_            = {};
        InvalidateMax_            = {};
        ReclassifyPending_        = false;
        // Mesmo motivo dos de cima: o follow-up pendente descrevia uma lamina de um volume que nao
        // existe mais. Atravessar o rebuild faria o primeiro update do volume novo rodar um
        // relocate restrito a sondas marcadas que ninguem marcou.
        ScrollFollowUpPending     = false;
        LogDebug("[GI] - DDGI volume: " + std::to_string(CountX) + "x" + std::to_string(CountY) +
                "x" + std::to_string(CountZ) + " probes (" + std::to_string(NumProbes) +
                "), spacing " + std::to_string(Cascades[0].Spacing) + ", atlas " +
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

    void FDDGI::PrepareCascadePlacement(const Vec3& _CameraPos) {
        if (!Ready || CascadeCount_ <= 1) return;

        // A GROSSA (indice CascadeCount_-1) e a que o SetupForScene dimensionou pela cena e nao se
        // mexe. As finas saem dela dividindo o espacamento por kCascadeSpacingRatio a cada degrau,
        // e sao CENTRADAS na cena.
        //
        // GridMin e o PRIMEIRO CENTRO de sonda, nao o canto da gaiola — mesma convencao da grossa,
        // onde ele vale AABBMin - 0.5*spacing justamente para a gaiola cobrir a AABB. Por isso a
        // conta aqui e so `centro - (count-1)*spacing/2` e nao ha meia celula a subtrair: os
        // centros cobrem (count-1)*spacing e a gaiola sobra meia celula de cada lado sozinha.
        const u32 Coarse = CoarseCascade();
        for (u32 C = 0; C < Coarse; ++C) {
            f32 Spacing = Cascades[Coarse].Spacing;
            for (u32 Step = Coarse; Step > C; --Step) Spacing /= kCascadeSpacingRatio;

            // 6.2b-ii: as finas seguem a CAMERA. Era o centro da cena na 6.2b-i, e a troca e
            // justamente o que torna o scrolling obrigatorio — sem ele, a cada celula andada todo
            // o conteudo guardado passaria a representar outro ponto do mundo de uma vez.
            const Vec3 Center = _CameraPos;
            // Snap ao proprio espacamento, e o resultado sai em CELULAS INTEIRAS: sem o snap a
            // grade "nada" sob a geometria e cada sonda troca de posicao no mundo por uma fracao
            // de celula — o pior caso para um cache temporal, e o scrolling nao teria como
            // compensar meia celula. Guardar a celula (e derivar o GridMin dela, nao o contrario)
            // e o que faz o scroll ser aritmetica inteira ponta a ponta.
            auto SnapCells = [&](f32 CenterAxis, int Count) {
                const f32 Extent = 0.5f * static_cast<f32>(Count - 1) * Spacing;
                return static_cast<int>(std::floor((CenterAxis - Extent) / Spacing));
            };
            const int Cells[3] = { SnapCells(Center.X, CountX),
                                   SnapCells(Center.Y, CountY),
                                   SnapCells(Center.Z, CountZ) };
            const int Counts[3] = { CountX, CountY, CountZ };

            Cascades[C].Spacing = Spacing;
            for (int A = 0; A < 3; ++A) {
                Cascades[C].OriginCells[A] = Cells[A];
                // Resto SEMPRE positivo: o `%` de C trunca em direcao a zero, e origem negativa
                // (cena com coordenadas negativas, que e o caso comum) daria slot negativo.
                const int N = Counts[A] > 0 ? Counts[A] : 1;
                Cascades[C].Scroll[A] = ((Cells[A] % N) + N) % N;
            }
            Cascades[C].GridMin = { static_cast<f32>(Cells[0]) * Spacing,
                                    static_cast<f32>(Cells[1]) * Spacing,
                                    static_cast<f32>(Cells[2]) * Spacing };
        }
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
        //
        // Espacamento da cascata GROSSA, e nao o da cascata 0 (que passa a ser a mais FINA). A
        // caixa e UMA so e vale para todas as cascatas, entao a folga tem de alcancar a camada de
        // sondas em volta na grade mais ESPARSA — uma folga de 2 m deixaria sondas da grade de
        // 8 m fora da invalidacao, preservando iluminacao e momentos de uma cena que ja mudou. A
        // folga maior e conservadora para a fina, que e o lado seguro do erro.
        const f32 Pad = Spacing();
        CPU.InvalidateMin     = { InvalidateMin_.X - Pad, InvalidateMin_.Y - Pad,
                                  InvalidateMin_.Z - Pad, Invalidating ? 1.0f : 0.0f };
        CPU.InvalidateMaxHyst = { InvalidateMax_.X + Pad, InvalidateMax_.Y + Pad,
                                  InvalidateMax_.Z + Pad, kInvalidateHysteresis };
        // Mesma caixa, janela e histerese proprias (ver kInvalidateDistHysteresis).
        // z: o RecordUpdate so decrementa RelocateFramesLeft na hora de despachar, entao aqui
        // "> 0" ja significa "o passe de classificacao roda neste frame" — o trace usa isso para
        // mandar os 64 raios e nao alimentar o classificador com a propria decimacao.
        // Cascatas: mesmo bloco que vai para os outros quatro cbuffers (ver CascadeConstants).
        CPU.Cascades = CascadeConstants();
        // Delta de scroll em CELULAS desde o ultimo update que rodou, so para os passes de update
        // (o gather nao limpa nada). Inteiro, e por isso identico nos quatro passes: se cada um
        // deduzisse a lamina comparando gaiolas em float, teriam tolerancias diferentes na borda
        // e discordariam sobre quais sondas limpar.
        const int Counts[3] = { CountX, CountY, CountZ };
        for (u32 i = 0; i < kMaxCascades; ++i) {
            const FCascade& Cs = Cascades[i < CascadeCount_ ? i : CoarseCascade()];
            f32  D[3]      = { 0.0f, 0.0f, 0.0f };
            bool Scrolled  = false;
            for (int A = 0; A < 3; ++A) {
                const int Raw   = Cs.OriginCells[A] - Cs.PrevOriginCells[A];
                // Alem de count+1 a resposta ja e "a cascata inteira e nova"; o clamp so mantem o
                // inteiro exato no float do cbuffer.
                const int Limit = Counts[A] + 1;
                D[A] = static_cast<f32>(std::clamp(Raw, -Limit, Limit));
                if (Raw != 0) Scrolled = true;
            }
            CPU.CascadeScrollDelta[i] = { D[0], D[1], D[2], Scrolled ? 1.0f : 0.0f };
        }
        // z = o passe de relocacao/classificacao roda neste frame; w = e uma passada SO de scroll.
        // O par existe porque o trace le os dois: com z e sem w ele manda os 64 raios em TODAS as
        // sondas (a catraca do classificador da fase 4 — medir proximidade a partir de uma sonda
        // ja decimada a derruba mais um degrau); com w, so nas recem-expostas, que sao as unicas
        // que o passe vai reclassificar. Sem essa distincao, andar reclassificaria o grid inteiro
        // a cada celula cruzada e reabriria a catraca pela porta do scroll.
        // DOIS predicados, e a separacao e o conserto: `ScrollWork` diz que o passe tem trabalho de
        // scroll (estreia OU follow-up) e `ScrollDebut` diz que ha uma lamina NOVA neste update.
        // Fundi-los deixava o `canMark` ligado no follow-up, e ali ele e veneno: se o segundo
        // relocate movesse a sonda de novo, escreveria outra marca `w >= 1` num update que ja e o
        // ultimo agendado — marca ORFA, ou seja histerese 0 permanente naquela sonda.
        const bool ScrollDebut = ScrolledSinceLastUpdate();
        const bool ScrollWork  = ScrollDebut || ScrollFollowUpPending;
        const bool RelocRuns   = RelocateFramesLeft > 0 || ScrollWork;
        const bool ScrollOnly  = RelocateFramesLeft == 0 && ScrollWork;
        CPU.MiscParams3       = { kInvalidateDistHysteresis,
                                  InvalidateDistFramesLeft_ > 0 ? 1.0f : 0.0f,
                                  RelocRuns ? 1.0f : 0.0f, ScrollOnly ? 1.0f : 0.0f };
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
        // `ScrollDebut` e nao `ScrollWork`, e a diferenca e o que separa uma marca com destino de
        // uma marca orfa. So a ESTREIA agenda um update seguinte, entao so nela existe o frame que
        // vai demover a marca. Autorizar no follow-up permitiria a ultima passada agendada
        // escrever uma marca que ninguem mais apaga — histerese 0 eterna naquela sonda, que e o
        // mesmo defeito que o `> 1` original existia para evitar.
        CPU.MiscParams2     = { (Relocation && (RelocateFramesLeft > 1 || ScrollDebut)) ? 1.0f : 0.0f,
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
        // Sai de kAtlasRead (contem PIXEL) -> so em fila direta.
        //
        // ProbeData entra AQUI desde a 6.2b-ii, e essa linha e o que permite a relocacao rodar em
        // compute assincrono. Antes ele ficava de fora e o passe de relocacao promovia
        // kAtlasRead -> UAV la dentro do RecordUpdate; como kAtlasRead contem PIXEL, essa
        // transicao e ILEGAL em fila de compute, e era por isso que o CanRunAsync devolvia falso
        // enquanto houvesse relocacao agendada. Saindo para NON_PIXEL aqui — na fila DIRETA, onde
        // e legal — o trace continua lendo como SRV e o relocate promove NON_PIXEL -> UAV, que e
        // compute-legal. O TransitionForRead devolve para kAtlasRead depois do wait, tambem na
        // direta. Sem isto o scrolling da fina jogaria o DDGI no caminho sincrono a cada celula
        // cruzada, que e justamente quando a camera esta andando.
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
        Transition(_CL, ProbeDataBuf.Get(), ProbeDataState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

        // 6.2b-ii: o scroll fecha aqui, pelo mesmo motivo que os decrementos acima moram aqui e
        // nao no UpdatePerFrame — o que importa e o ultimo update que RODOU. Se o DDGI ficar dois
        // frames sem atualizar enquanto a camera anda tres celulas, as laminas expostas nos tres
        // precisam ser limpas de uma vez; latchar por frame perderia as duas primeiras.
        // A lamina nova estreia com ProbeData de outro lugar do mundo: offset de relocacao alheio
        // e, pior, a MARCA de inativa — uma sonda marcada por estar enterrada 40 m atras faria o
        // gather pular uma sonda perfeitamente boa aqui, e isso aparece como buraco no indireto.
        // Quem reescreve ProbeData e so o passe de relocacao, e ele roda UMA vez por lamina: o
        // `delta` so e diferente de zero neste update, entao so aqui da para saber quais sondas
        // sao novas. Por isso a relocacao delas e one-shot (aplica o alvo inteiro em vez do lerp
        // de 0,25 — ver DDGIRelocate); agendar mais frames nao daria convergencia, daria frames
        // reclassificando o grid INTEIRO sem saber mais por que, forcando 64 raios em todas as
        // 8.832 sondas enquanto a camera anda — Adaptive Rays neutralizado exatamente em
        // movimento, que e quando ele mais renderia.
        //
        // O LATCH fecha aqui, e nao no UpdatePerFrame, pelo mesmo motivo dos decrementos acima: o
        // que conta e o ultimo update que RODOU. Dois frames sem atualizar com a camera andando
        // tres celulas tem de limpar as tres laminas de uma vez.
        const bool ScrollDebut = ScrolledSinceLastUpdate();
        const bool ScrolledNow = ScrollDebut || ScrollFollowUpPending;
        // Agenda o update seguinte SE houve estreia agora; se este JA era o follow-up e nao houve
        // estreia nova, fecha. Uma estreia durante um follow-up reabre — as duas laminas sao
        // identificadas por criterios diferentes no shader (delta e a marca w>=1), entao convivem.
        ScrollFollowUpPending = ScrollDebut;
        for (u32 C = 0; C < CascadeCount_; ++C)
            for (int A = 0; A < 3; ++A)
                Cascades[C].PrevOriginCells[A] = Cascades[C].OriginCells[A];

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

        if (RelocateFramesLeft > 0 || ScrolledNow) {
            if (RelocateFramesLeft > 0) --RelocateFramesLeft;
            Transition(_CL, ProbeDataBuf.Get(),     ProbeDataState,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            RelocatePSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ProbesTraceSRVSlot));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbeDataUAVSlot)); 
            _CL->Dispatch((NumProbes + 63) / 64, 1, 1);
            // A promocao acima e NON_PIXEL -> UAV, legal em compute: quem tira o ProbeData do
            // kAtlasRead (que contem PIXEL) e o TransitionForUpdate, na fila DIRETA. A volta para
            // kAtlasRead fica no TransitionForRead, tambem na direta, depois do wait.
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Reclassificacao pos-edicao, agendada quando a janela de invalidacao FECHA (ver
        // ReclassifyPending_). Fica no fim da funcao de proposito, e o motivo MUDOU na 6.2b-ii: era
        // que agendar antes faria o relocate despachar ja, numa list de compute onde a transicao do
        // ProbeData a partir de um estado com PIXEL seria ilegal — hoje ela e legal, porque a saida
        // do kAtlasRead migrou para o TransitionForUpdate.
        //
        // O motivo que RESTA, e que sozinho ja manda: o cbuffer deste frame ja foi escrito pelo
        // UpdatePerFrame, entao um agendamento feito aqui nao aparece no MiscParams3.z que o TRACE
        // deste frame le. O passe classificaria a partir de um trace decimado — a catraca da fase
        // 4. Agendando para o proximo frame, o trace ve o flag e manda os 64.
        //
        // A guarda de trabalho util: sem relocacao e sem raios adaptativos o passe so reescreve
        // zeros e 64, e custaria 6 frames de dispatch para nada.
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
