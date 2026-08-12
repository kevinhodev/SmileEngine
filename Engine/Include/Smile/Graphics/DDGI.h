#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/GIHitSampling.h"
#include "Smile/Graphics/ReGIR.h"
#include "Smile/Graphics/RadianceCache.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <algorithm> // std::max nas contas da grade de dispatch
#include <cstddef>

namespace Smile {
    class FTextureSRVHeap;
    class FCommandQueue;
    class FScene;
    class FGpuMesh;

    // O QUE mudou dentro da caixa de FDDGI::InvalidateRegion. Os dois invalidam o atlas do mesmo
    // jeito; a diferenca esta na CLASSIFICACAO das sondas (inativa/ativa e contagem de raios), que
    // e medida de GEOMETRIA e so por ela envelhece.
    //
    // Sem a distincao, editar a intensidade de uma luz agendava uma reclassificacao global de 6
    // frames, com 64 raios e fora da fila assincrona, ~69 frames depois de soltar o slider — ou
    // seja, um hitch atrasado o bastante para ninguem relacionar com o que acabou de fazer. E
    // trabalho para nada: nenhuma sonda mudou de lugar nem passou a enxergar coisa diferente.
    //
    // Sem valor default de proposito. O default seguro nao existe: "radiometrico" faz o call site
    // esquecido reintroduzir a classificacao velha em silencio, e "geometria" faz ele pagar o
    // hitch. Obrigar a declarar e a unica opcao que nao escolhe um dos dois erros por omissao.
    enum class EGIRegionChange {
        Radiometric, // luz: cor, intensidade, raio, cone, direcao, on/off, posicao, add/remove
        Geometry     // objeto: criado, removido, movido, escondido/mostrado
    };

    // Bloco de cascatas, IDENTICO nos cinco cbuffers que o carregam (FrameConstants, fog,
    // ReSTIR GI, reflexoes e o DDGICB). Existe como POD, e nao como cinco pares de campos soltos,
    // porque cinco loops de preenchimento independentes produzindo o mesmo estado e exatamente
    // como um deles fica para tras numa edicao futura. Quem preenche e FDDGI::CascadeConstants().
    //
    // Espelhado em HLSL pelo par (CascadeParams, CascadeGridMinSpacing[4]) — a ordem dos dois
    // campos e o tamanho do array fazem parte do contrato.
    struct alignas(16) FDDGICascadeConstants {
        Vec4 Params{ 1.0f, 0.0f, 0.0f, 0.0f }; // x = nº de cascatas, y = sondas por cascata
        // Espacamento 1 e nao 0 no DEFAULT, e isto e a garantia e nao um detalhe: o seletor de
        // cascata divide pelo espacamento, entao um bloco zerado — o que qualquer instancia
        // default-construida seria — daria divisao por zero se algum caminho novo o ler. A
        // primeira versao garantia isso a mao no caminho "GI desligado" do FrameConstants, e por
        // isso a garantia NAO valia para os outros quatro cbuffers.
        Vec4 GridMinSpacing[4]{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
                                { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    };
    // O tamanho sozinho nao prende o contrato: 80 bytes tambem passariam com os campos trocados
    // de ordem ou com padding no meio. O offset e o alinhamento e que fazem o bloco ser
    // copiavel para um cbuffer sem tradutor.
    static_assert(sizeof(FDDGICascadeConstants) == 80,
                  "o bloco de cascatas e copiado campo-a-campo para cinco cbuffers");
    static_assert(alignof(FDDGICascadeConstants) == 16,
                  "o bloco entra em cbuffer: alinhamento de float4");
    static_assert(offsetof(FDDGICascadeConstants, GridMinSpacing) == 16,
                  "o array tem de seguir imediatamente o Params, sem padding");

    struct alignas(256) DDGIConstants {
        Vec4 GridMinSpacing;  // xyz = origem do grid (mundo), w = espacamento
        Vec4 GridCountRays;   // xyz = nº de probes por eixo, w = raios por probe
        Vec4 AtlasParams;     // x = tile size, y = atlasW, z = atlasH, w = numProbes
        Vec4 SunDirIntensity; // xyz = direcao P/ o sol, w = intensidade do sol
        Vec4 SunColorHyst;    // rgb = cor do sol, w = hysteresis (blend temporal)
        Vec4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = shadowRayBias
                              // (raios do DDGI partem de probes; o bias so desloca sombras no hit)
        Vec4 DistAtlasParams; // x = dist tile, y = dist atlasW, z = dist atlasH,
                              // w = hysteresis do atlas de DISTANCIA (propria; ver kDistHysteresis)
        Vec4 MiscParams;      // x = relocationEnabled (Fase 2), y = deactivThresh, z = maxRays, w = minRays
        Vec4 MiscParams2;     // x = canMarkActivated (relocacao tem +1 frame agendado), y = nº luzes (F5),
                              // z = ShadowRayMask, w = detector de histerese adaptativa (so a irradiancia le)
        // Perfil de epsilons (FRayEpsilonProfile), anexado no FIM p/ nao deslocar offsets. O DDGI
        // so usa a familia (2) — os raios dele partem de probes, nao do G-buffer.
        Vec4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
        // Gather do 2o bounce no hit (contrato do HitShading.hlsli). Duplica tile/W/H que ja
        // estao no DistAtlasParams porque o contrato e por NOME e vale para os tres passes.
        Vec4 GIDistParams;    // x=distTile, y=distAtlasW, z=distAtlasH, w=skipMode
        Vec4 GIBiasParams;    // x=escala do bias de superficie, y=teto em metros, zw=-
        Vec4 ReGIRGridMinSlots;
        Vec4 ReGIRInvCellEnabled;
        Vec4 ReGIRGridCountSamples;
        Vec4 ReGIRResources;
        // Parameterizacao do sky-view LUT p/ o ShadeSky do HitShading.hlsli, vinda do
        // FAtmosphere (fonte unica). Anexado no FIM p/ nao deslocar offset nenhum.
        Vec4 SkyParams;       // x = view height (km), y = raio do planeta (km), zw = livres
        // Invalidacao ESPACIAL do atlas (ver FDDGI::InvalidateRegion). Caixa de mundo cujas
        // sondas trocam a hysteresis por uma mais rapida durante alguns frames. Anexado no FIM,
        // como o SkyParams, p/ nao deslocar offset nenhum.
        Vec4 InvalidateMin;      // xyz = min da caixa, w = 1 se ha invalidacao ativa
        Vec4 InvalidateMaxHyst;  // xyz = max da caixa, w = hysteresis a usar dentro dela
        // World radiance cache (FRadianceCacheShaderParams). Contrato por NOME com o
        // RC_UNPACK_PARAMS do RadianceCache.hlsli; anexado no FIM como o SkyParams.
        Vec4 RadianceCacheCamCell;
        Vec4 RadianceCacheLodCapFlags;
        Vec4 RadianceCacheResources;
        // Invalidacao regional do atlas de DISTANCIA. Campos proprios porque ele nao compartilha
        // nem a hysteresis nem a JANELA com a irradiancia — so a caixa. Anexado no fim, como os
        // anteriores, p/ nao deslocar offset nenhum.
        Vec4 MiscParams3;     // x = hysteresis regional do dist atlas, y = 1 se a janela dele
                              // esta aberta, z = 1 se o passe de relocacao/classificacao roda
                              // neste frame (o TRACE le p/ nao decimar; ver DDGITrace), w = livre
        // Cascatas. Anexado no FIM como todo o resto, p/ nao deslocar offset nenhum. A cascata 0 e
        // a mais FINA; a de indice (contagem-1) e a que cobre a cena. Contagem de sondas comum a
        // todas, entao so este par varia — ver FDDGI::FCascade.
        FDDGICascadeConstants Cascades; // = CascadeParams + CascadeGridMinSpacing[4] no HLSL
    };
    static_assert(offsetof(DDGIConstants, ReGIRGridMinSlots) == 208,
                  "DDGIConstants divergiu do cbuffer DDGICB");
    static_assert(offsetof(DDGIConstants, SkyParams) == 272,
                  "SkyParams deve permanecer anexado ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, InvalidateMin) == 288,
                  "InvalidateMin/MaxHyst devem permanecer anexados ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, MiscParams3) == 368,
                  "MiscParams3 deve permanecer anexado ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, Cascades) == 384,
                  "o bloco de cascatas deve permanecer anexado ao fim do DDGICB");

    // Menor n com H^n <= Residual. Existe para a JANELA da invalidacao regional acompanhar a
    // HISTERESE dela sozinha: as duas so fazem sentido juntas — o que a regiao promete e "sobra
    // ~3% do historico quando a janela fecha", e isso e um par, nao dois numeros. Deixa-los
    // soltos foi o que permitiu 0.75/12 passar por revisao: o valor era defensavel, a variancia
    // instantanea que ele impunha (7 amostras efetivas) e que nao era.
    constexpr u32 DDGIFramesForResidual(f32 _H, f32 _Residual) {
        u32 N = 0; f32 V = 1.0f;
        while (V > _Residual && N < 512u) { V *= _H; ++N; }
        return N;
    }

    class FDDGI : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "DDGI"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        static constexpr int kRaysPerProbe = 64;
        static constexpr int kTileSize     = 6;
        static constexpr int kDistTileSize = 14;
        // Cascatas: volumes concentricos com a MESMA contagem de sondas e espacamentos
        // diferentes. A de indice 0 e a mais FINA; a ULTIMA e a que cobre a cena inteira — o
        // volume unico de hoje. Essa ordem e o oposto da Flax, onde a ultima cascata cai num
        // FallbackIrradiance constante: aqui o fallback da cascata fina e a grossa, e so quem sai
        // da grossa (o terreno) cai no ambiente hemisferico. Preserva a propriedade que o volume
        // unico ja tinha — cobertura da cena inteira, convergindo em todo lugar.
        static constexpr u32 kMaxCascades = 4;
        // Sondas por LINHA do ProbesTrace, e este numero E um knob de desempenho. A primeira
        // versao daqui dizia que 256 "e o maior que cabe, entao nao ha o que calibrar": isso
        // confunde CAPACIDADE com velocidade. 256*64 = 16384 enche a largura maxima de Texture2D,
        // mas uma textura de 16384 de largura nao e necessariamente a melhor para cache/swizzle.
        // Valores para A/B: 64 (largura 4096), 128 (8192), 256 (16384). Espelhado em
        // DDGI_TRACE_PROBES_PER_ROW (DDGICommon.hlsli) — mudar aqui exige mudar la.
        static constexpr u32 kTraceProbesPerRow = 256;
        // `<=` e nao `==`: com `==` o assert exigia justamente o 256, ou seja proibia o A/B que o
        // comentario acima manda fazer. O que a checagem tem de garantir e caber, nao encher.
        static_assert(kTraceProbesPerRow * kRaysPerProbe <= 16384,
                      "a linha do ProbesTrace nao pode passar da largura maxima de Texture2D; "
                      "mudar aqui exige mudar DDGI_TRACE_PROBES_PER_ROW junto");

        // Grade 2D de GRUPOS dos tres passes de uma sonda por grupo. O D3D12 para em 65535 grupos
        // por dimensao, e com dispatch 1D esse era o teto real de sondas — abaixo do que os atlas
        // comportam desde o reempacotamento, e invisivel ate o Dispatch falhar em runtime.
        // Espelhado em DDGI_DISPATCH_GROUPS_X (DDGICommon.hlsli); a conta e inteira dos dois lados
        // justamente para nao existir um numero a transportar e dessincronizar.
        static constexpr u32 kDispatchGroupsX = 1024;
        static_assert(kDispatchGroupsX <= 65535,
                      "a largura da grade de grupos e ela propria um Dispatch");
        // Divide NumProbes o mais exato possivel: a sobra fica abaixo do numero de linhas, entao
        // nao se paga uma faixa de grupos que so testa e retorna.
        static u32 DispatchGroupsX(u32 Probes) {
            const u32 Rows = std::max((Probes + kDispatchGroupsX - 1) / kDispatchGroupsX, 1u);
            return std::max((Probes + Rows - 1) / Rows, 1u);
        }
        static u32 DispatchGroupsY(u32 Probes) {
            const u32 X = DispatchGroupsX(Probes);
            return (Probes + X - 1) / X;
        }

        void Initialize(ID3D12Device* Device);

        void SetupForScene(ID3D12Device* Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                           const FScene& Scene, const Vec3& AABBMin, const Vec3& AABBMax,
                           u32 TlasSRVSlot, u32 SkyViewSRVSlot);

        // Re-upload do snapshot de materiais que TODO o RT le (DDGI, ReSTIR, reflexoes). Chamar
        // quando uma propriedade de material que o RT enxerga muda em runtime (AlphaTest,
        // TwoSided, emissivo...): o snapshot e criado uma vez no SetupForScene e nao acompanha a
        // edicao. O CHAMADOR precisa garantir GPU ociosa — e um upload heap sem versao por frame.
        void RefreshInstanceGeo(const FScene& Scene);

        // Quantas instancias o snapshot acima consegue descrever. Alocado no SetupForScene pelo
        // tamanho da cena de entao; o RefreshInstanceGeo trunca nele. Quem cria objeto com a cena
        // ja carregada precisa consultar antes (Renderer::OnSceneStructureChanged).
        u32 InstanceGeoCapacity() const { return InstanceGeoCount; }

        // Decide ONDE cada cascata fica neste frame. Tem de rodar ANTES de qualquer consumidor
        // capturar CascadeConstants() — hoje sao tres, e todos capturam cedo: o FrameConstants, o
        // estado de RT (reflexoes/ReSTIR) e o fog.
        //
        // A costura existe por ordenacao, nao por elegancia. O UpdatePerFrame roda DEPOIS deles,
        // dentro do PrepareIndirectLighting; se a colocacao morasse la, a cascata fina se moveria
        // para a posicao nova, o atlas seria atualizado com ela, e os consumidores daquele frame
        // ainda estariam amostrando com a origem ANTERIOR — uma dessincronizacao de um frame,
        // visivel como a iluminacao arrastando atras da camera. Com a cascata parada isto e um
        // no-op; a 6.2b e quem enche a funcao.
        //
        // Regra da casa daqui pra frente: PrepareCascadePlacement DECIDE, UpdatePerFrame apenas
        // PUBLICA o que ja foi decidido.
        void PrepareCascadePlacement(const Vec3& CameraPos);

        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, u32 PunctualLightCount = 0);

        // F5: copia o SRV do buffer de luzes puntuais do frame (slot de staging do Renderer)
        // pro t8 da tabela de trace DO FrameSlot (tabela versionada por frame em voo — a do
        // frame anterior ainda pode estar sendo lida pela GPU).
        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot, u32 FrameSlot);

        // Async compute (F3): transicoes envolvendo PIXEL_SHADER_RESOURCE nao podem ser
        // gravadas em fila COMPUTE. TransitionForUpdate (atlases/trace -> UAV) vai na
        // fila DIRETA antes do signal; RecordUpdate (dispatches + transicoes UAV/NON_PIXEL,
        // compute-legais) roda em qualquer fila; TransitionForRead (atlases -> PIXEL|
        // NON_PIXEL) vai na direta depois do wait. No caminho sincrono, chamar as tres em
        // sequencia na mesma list = comportamento antigo.
        void TransitionForUpdate(ID3D12GraphicsCommandList* CommandList);
        void RecordUpdate(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);
        void TransitionForRead(ID3D12GraphicsCommandList* CommandList);

        // Relocation de probes (transiente pos-setup) transiciona ProbeData no MEIO do
        // update a partir de estado com PIXEL — nesses frames o update roda sincrono.
        bool CanRunAsync() const { return RelocateFramesLeft == 0; }

        bool IsReady() const { return Ready; }
        u32  IrradianceAtlasSRV() const { return AtlasSRVSlot; }
        u32  InstanceSRV() const { return InstanceSRVSlot; }
        u32  SceneGITableStart()  const { return SceneGITableStart_; }
        u32  DistAtlasSRV()    const { return DistSRVSlot; }
        u32  ProbesTraceSRV()  const { return ProbesTraceSRVSlot; }
        u32  ProbeDataSRV()    const { return ProbeDataSRVSlot; }
        u32  ProbeRayCountSRV()const { return ProbeRayCountSRVSlot; } 
        ID3D12Resource* IrradianceAtlasResource() const { return IrradAtlas.Get(); }
        ID3D12Resource* DistanceAtlasResource() const   { return DistAtlas.Get(); }
        // TOTAL, somando as cascatas — e o que dimensiona atlas, ProbesTrace e dispatch.
        u32  NumProbesCount()  const { return NumProbes; }
        u32  ProbesPerCascade()const { return ProbesPerCascade_; }
        u32  CascadeCount()    const { return CascadeCount_; }
        u32  RaysPerProbe()    const { return kRaysPerProbe; }

        // Cascata 0 e a mais FINA; a ULTIMA e a que cobre a cena.
        //
        // GridMin()/Spacing() devolvem a GROSSA de proposito. Todo consumidor que existia antes
        // das cascatas — o fade de borda, o fallback fora do volume, a folga da invalidacao
        // regional, o alcance do trace — quer dizer "o volume da cena", e essa e a grossa. Fazer
        // estes dois apontarem para a cascata 0 mudaria o significado de cada um deles em
        // silencio no dia em que a segunda cascata acender. Com uma cascata os dois sao o mesmo.
        u32  CoarseCascade() const { return CascadeCount_ > 0 ? CascadeCount_ - 1 : 0; }
        Vec3 GridMin()   const { return Cascades[CoarseCascade()].GridMin; }
        f32  Spacing()   const { return Cascades[CoarseCascade()].Spacing; }
        Vec3 CascadeGridMin(u32 C) const { return Cascades[C < kMaxCascades ? C : 0].GridMin; }
        f32  CascadeSpacing(u32 C) const { return Cascades[C < kMaxCascades ? C : 0].Spacing; }

        // Fonte UNICA do bloco de cascatas para todos os cbuffers que o carregam. As entradas
        // acima da contagem repetem a GROSSA em vez de ficarem lixo: se algum dia um shader ler
        // fora da faixa, ele amostra o volume da cena — degradado, nao indefinido.
        FDDGICascadeConstants CascadeConstants() const {
            FDDGICascadeConstants C{};
            C.Params = { static_cast<f32>(CascadeCount_),
                         static_cast<f32>(ProbesPerCascade_), 0.0f, 0.0f };
            for (u32 i = 0; i < kMaxCascades; ++i) {
                const FCascade& Cs = Cascades[i < CascadeCount_ ? i : CoarseCascade()];
                C.GridMinSpacing[i] = { Cs.GridMin.X, Cs.GridMin.Y, Cs.GridMin.Z, Cs.Spacing };
            }
            return C;
        }
        Vec3 GridCount() const { return Vec3{ (f32)CountX, (f32)CountY, (f32)CountZ }; }
        f32  AtlasW()    const { return (f32)AtlasWidth; }
        f32  AtlasH()    const { return (f32)AtlasHeight; }
        f32  TileSizeF() const { return (f32)kTileSize; }
        f32  DistAtlasW()    const { return (f32)DistAtlasWidth; }
        f32  DistAtlasH()    const { return (f32)DistAtlasHeight; }
        f32  DistTileSizeF() const { return (f32)kDistTileSize; }
        f32  MaxRayDistance() const { return MaxRayDist; } 
        // O atlas de distancia guarda os dois momentos ja limitados a esta vizinhanca.
        // Nao confundir com MaxRayDistance(), que e o alcance do trace na cena inteira.
        // Da cascata GROSSA por default: e uma escala GLOBAL, usada para normalizar a vista do
        // atlas inteiro, e o atlas tem tiles de todas as cascatas. Com o teto da fina (2 m) os
        // tiles da grossa (8 m) saturariam por completo e o modo deixaria de discriminar; com o
        // da grossa, os da fina ficam escuros mas legiveis — e quem inspeciona UM tile pede a
        // escala dele por CascadeDistanceMomentMax.
        f32  DistanceMomentMax() const { return Spacing() * 2.6f; }
        f32  CascadeDistanceMomentMax(u32 C) const { return CascadeSpacing(C) * 2.6f; }
        // Conversao sonda <-> tile fisico do atlas. O atlas tem ORDEM PROPRIA — plano (x,z) nas
        // colunas, y nas linhas, o plano enrolado em bandas — e ela nao e a dos buffers
        // (DDGI_ProbeLinear). Manter as duas custa estas duas funcoes; unifica-las custaria a
        // vizinhanca 2x2 que o gather de 8 sondas percorre, medida em ~0,4 ms. Ver DDGI_TileOrigin.
        //
        // As duas sao INVERSAS uma da outra e as duas espelham o shader. Qualquer mudanca no
        // layout mexe nas tres.
        // O indice de sonda aqui e o GLOBAL (cascata * ProbesPerCascade + local); as cascatas
        // empilham em blocos de LINHAS, uma depois da outra, como no DDGI_TileOrigin.
        u32  AtlasTileFromProbe(u32 ProbeIndex) const {
            if (ProbeIndex >= NumProbes || CountX <= 0 || CountY <= 0 || TilesPerRow == 0 ||
                ProbesPerCascade_ == 0)
                return 0;
            const u32 CX = static_cast<u32>(CountX), CY = static_cast<u32>(CountY);
            const u32 Cascade = ProbeIndex / ProbesPerCascade_;
            const u32 Local   = ProbeIndex - Cascade * ProbesPerCascade_;
            const u32 X = Local % CX;
            const u32 Y = (Local / CX) % CY;
            const u32 Z = Local / (CX * CY);
            const u32 Plane = X + Z * CX;
            const u32 Band  = Plane / TilesPerRow;
            const u32 Row   = Y + Band * CY + Cascade * TileRowsPerCascade;
            return Row * TilesPerRow + (Plane - Band * TilesPerRow);
        }
        // Devolve false para celula de SOBRA: a grade tem TilesPerRow*bandas colunas de plano, e
        // as ultimas podem nao corresponder a sonda nenhuma.
        bool ProbeFromAtlasTile(u32 Tile, u32& OutProbe) const {
            if (CountX <= 0 || CountY <= 0 || TilesPerRow == 0 || TileRowsPerCascade == 0)
                return false;
            const u32 CX = static_cast<u32>(CountX), CY = static_cast<u32>(CountY);
            const u32 RowAbs  = Tile / TilesPerRow;
            const u32 C       = Tile - RowAbs * TilesPerRow;
            const u32 Cascade = RowAbs / TileRowsPerCascade;
            const u32 Row     = RowAbs - Cascade * TileRowsPerCascade;
            const u32 Y       = Row % CY;
            const u32 Band    = Row / CY;
            const u32 Plane   = Band * TilesPerRow + C;
            if (Cascade >= CascadeCount_ || Plane >= CX * static_cast<u32>(CountZ)) return false;
            OutProbe = Cascade * ProbesPerCascade_ +
                       (Plane % CX) + Y * CX + (Plane / CX) * CX * CY;
            return OutProbe < NumProbes;
        }
        // Tiles por linha da grade dos atlas. O shader recupera este mesmo numero da LARGURA
        // (DDGI_TilesPerRow) em vez de recebe-lo por cbuffer — ver o SetupForScene.
        u32  AtlasTilesPerRow() const { return TilesPerRow; }
        u32  AtlasTileRows() const { return TileRowsPerCascade * CascadeCount_; }

        // Zero e SENTINELA no cbuffer, nao valor: o Renderer manda DDGIParams.x = 0 para dizer "GI
        // desligado" e os cinco consumidores leem `(x > 0) ? x : 1.0`. Sem o piso, um slider de
        // intensidade em 0 viraria intensidade 1 — o oposto exato do pedido. Quem quer zero
        // desliga o GI. Hoje o bug e latente (nao ha call site do setter, a intensidade e sempre
        // 1.0); isto e o pre-requisito de expor o slider, nao detalhe de estilo.
        static constexpr f32 kIntensityMin = 1e-3f;
        void SetIntensity(f32 V)  { Intensity = V < kIntensityMin ? kIntensityMin : V; }
        f32  GetIntensity() const { return Intensity; }
        // Perfil compartilhado de epsilons (dono = Renderer, empurra todo frame).
        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        // Gather do 2o bounce (dono = Renderer, empurra todo frame; ver FGIHitSampling).
        void SetGIHitSampling(const FGIHitSampling& S) { GIHit = S; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        // Cache de radiancia de mundo (dono = Renderer, empurra todo frame). O DDGI recebe COM o
        // bit de update: a sonda integra o hit num cosseno, entao a radiancia dela e nao-direcional
        // e pode alimentar o cache. Ver FRadianceCache::ShaderParams.
        void SetRadianceCacheParams(const FRadianceCacheShaderParams& P) { RadianceCacheParams = P; }
        // Parameterizacao do sky-view LUT p/ o ShadeSky dos raios que escapam (dono = Renderer,
        // empurra todo frame a partir do FAtmosphere — fonte unica, ver Atmosphere.h).
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }
        // Escurecimento do ceu na chuva (dono = Renderer; mesma politica das reflexoes e do
        // ReSTIR GI). O membro existia fixo em 1.0, sem setter — o DDGI ignorava o temporal.
        void SetSkyIntensity(f32 V) { SkyIntensity = V; }
        f32  GetSkyIntensity() const { return SkyIntensity; }

        // Reset one-shot do atlas: o proximo update que REALMENTE rodar usa histerese 0, ou seja,
        // substitui o conteudo em vez de misturar. Necessario p/ calibracao: com Hysteresis 0.99
        // o atlas guarda 99% do resultado por update, entao um knob de epsilon pareceria inerte
        // por dezenas de frames. O flag e consumido no RecordUpdate, nao aqui — se o passe nao
        // rodar neste frame, o reset continua pendente.
        void ResetHistoryOnce()   { HysteresisResetPending = true; }

        // Invalidacao ESPACIAL: so as sondas dentro da caixa de mundo trocam a hysteresis por
        // kInvalidateHysteresis durante kInvalidateFrames frames.
        //
        // Existe porque criar ou apagar UM objeto deixa 99% das sondas ainda corretas — so as
        // vizinhas dele mudaram. O ResetHistoryOnce acima e global e zera a hysteresis, ou seja,
        // troca o atlas inteiro pela estimativa de UM trace de 64 raios: a GI toda pula para um
        // valor ruidoso e volta a assentar. E o que se via como uma piscada leve ao duplicar ou
        // remover. Ele continua certo para mudanca GLOBAL de iluminacao (calibracao, cor do sol).
        //
        // Modelo da UE: o Lumen invalida por PRIMITIVA (LumenInvalidateSurfaceCacheForPrimitive),
        // nunca o cache inteiro. A hysteresis de dentro da caixa nao e 0 e sim alta-mas-rapida,
        // diferente da relocacao (que usa 0 porque o historico dela e de OUTRO lugar): aqui o
        // historico da sonda continua quase todo valido, entao misturar rapido converge sem o
        // pop de amostra unica.
        //
        // Chamadas sucessivas UNEM as caixas, e a uniao e CRUA — a folga de um espacamento sai
        // no envio (UpdatePerFrame), nao aqui. E isso que torna seguro chamar por FRAME durante
        // um gesto continuo: "antiga + nova" de um arraste vira uma chamada com cada caixa, sem
        // a regiao inchar a cada uma.
        // `Change` decide se a classificacao das sondas tambem envelheceu — ver EGIRegionChange.
        void InvalidateRegion(const Vec3& Min, const Vec3& Max, EGIRegionChange Change);

        // Teto de 0.98 na histerese ESTAVEL. Nao e preferencia: o paper (secao 4.4) usa alfa
        // entre 0.85 e 0.98, e o Flax GRAMPEIA em 0.98
        // (DynamicDiffuseGlobalIllumination.cpp: Clamp(TemporalResponse, 0.0f, 0.98f)). A 0.99
        // o atlas leva ~300 updates — 5 s a 60 fps — para absorver 95% de qualquer mudanca, e
        // cada evento de invalidacao que ninguem ligou custa esses 5 s inteiros.
        static constexpr f32 kHysteresisMax = 0.98f;
        void SetHysteresis(f32 V) {
            Hysteresis = V > kHysteresisMax ? kHysteresisMax : (V < 0.0f ? 0.0f : V);
        }
        f32  GetHysteresis() const{ return Hysteresis; }

        // Rede de seguranca da invalidacao por evento: o proprio update mede quanto a sonda
        // mudou e derruba a histerese dela sozinho. Ver DDGI_AdaptiveHysteresis (DDGICommon).
        // Vale so para a irradiancia — o atlas de distancia reamostra menos de um raio por texel
        // por frame e um detector la dispararia com o proprio ruido (ver DDGIUpdateDist).
        //
        // Default OFF desde o A/B de 2026-08-11: com os limiares originais (0.20/0.50) ele
        // acendia com o RUIDO do estimador, nao com mudanca de cena, e prendia sonda de alta
        // variancia em histerese reduzida — flicker espalhado. Os limiares foram recalibrados
        // para fora do ruido (ver DDGICommon), mas a versao recalibrada ainda nao passou por A/B
        // proprio, e o valor dela caiu muito depois da fase 2: com movimento, visibilidade e luz
        // ja ligados por evento, o que sobra para a rede pegar sao mudancas de ordem de
        // grandeza, nao de 40%.
        void SetAdaptiveHysteresis(bool V) { AdaptiveHysteresis = V; }
        bool GetAdaptiveHysteresis() const { return AdaptiveHysteresis; }

        void SetDeactivationThreshold(f32 V) { DeactivationThreshold = V; TriggerReclassify(); }
        f32  GetDeactivationThreshold() const { return DeactivationThreshold; }

        // Raios por sonda variaveis com a proximidade de geometria (DDGI_DesiredRays, no passe de
        // relocacao). Quem escreve o ProbeRayCount e SO aquele passe, entao os tres setters
        // agendam uma reclassificacao — inclusive com relocacao DESLIGADA, caso em que o passe
        // classifica sem mover a sonda. Antes o agendamento era guardado por `Relocation` e o
        // toggle ficava inerte: sem relocacao, a contagem congelava no ultimo valor escrito.
        void SetAdaptiveRays(bool V) { AdaptiveRays = V; TriggerReclassify(); }
        bool GetAdaptiveRays() const { return AdaptiveRays; }
        // Teto REAL = kRaysPerProbe. O trace tem um thread por raio (`numthreads(64)`) e decima
        // com `stride = 64 / rayCount`, que so REDUZ: pedir 256 dava stride 0 -> 1 -> os mesmos 64
        // raios. Os setters aceitavam ate 256 e o numero era ficcao na UI.
        void SetMaxRays(int V) { MaxRays = ClampRays(V); TriggerReclassify(); }
        int  GetMaxRays() const { return MaxRays; }
        void SetMinRays(int V) { MinRays = ClampRays(V); TriggerReclassify(); }
        int  GetMinRays() const { return MinRays; }

        // Folhagem nos shadow rays do hit (ON = alpha-test por candidato; OFF = mask so-opaco).
        void SetFoliageShadows(bool V) { FoliageShadows = V; }
        bool GetFoliageShadows() const { return FoliageShadows; }

        void SetRelocation(bool V) { Relocation = V; RelocateFramesLeft = V ? kRelocateConvergeFrames : 4; }
        bool GetRelocation() const { return Relocation; }

        // Amostragem (nao afeta o atlas — sao knobs do SAMPLER, lidos pelo deferred/forward e
        // pelo diagnostico pontual). Ficam aqui, e nao no FRayEpsilonProfile, porque nao sao
        // epsilons de RAIO: o perfil descreve a geometria dos raios de GI/reflexo/sombra e sua
        // troca limpa reservoirs e denoiser, o que nao faz sentido para um offset de leitura.
        //
        // Teto do bias em METROS. 0 = sem teto = comportamento historico (bias = 0.75*spacing*
        // scale, formula do Flax). Existe porque o espacamento do grid aqui vem da AABB da cena
        // inteira: com spacing de 8 m o bias historico vale 1,20 m e o ponto de amostragem
        // atravessa parede. O RTXGI moderno resolve com normalBias/viewBias absolutos; o teto e
        // a versao barata, que preserva cena pequena e corta cena grande.
        void SetSurfaceBiasMax(f32 V)   { SurfaceBiasMax = V < 0.0f ? 0.0f : V; }
        f32  GetSurfaceBiasMax() const  { return SurfaceBiasMax; }
        void SetSurfaceBiasScale(f32 V) { SurfaceBiasScale = V; }
        f32  GetSurfaceBiasScale() const{ return SurfaceBiasScale; }

        // O peso de backface e sempre medido da posicao SEM bias (o que o Flax faz,
        // DDGI.hlsl:210-215). Teve toggle enquanto era hipotese; virou comportamento fixo depois
        // do A/B — nao e trade-off, e correcao, e o modo antigo so serviria p/ reintroduzir o bug.

        // Largura, EM CELULAS, do fade para o ambiente hemisferico nas bordas do volume.
        // 0 = desligado (o gather clampa e estende as probes de borda ao infinito, que e o
        // comportamento historico). O terreno fica fora do volume de proposito, entao sem isto
        // ele inteiro herda a irradiancia da ultima fileira de probes.
        void SetVolumeFadeProbes(f32 V) { VolumeFadeProbes = V < 0.0f ? 0.0f : V; }
        f32  GetVolumeFadeProbes() const { return VolumeFadeProbes; }

        // Numero de cascatas PEDIDO. So vira o vigente no proximo SetupForScene, porque a
        // contagem dimensiona atlas, ProbesTrace, buffers e dispatch — trocar em runtime exigiria
        // realocar tudo no meio do frame. Quem chama e responsavel por recriar a cena.
        void SetDesiredCascades(u32 V) {
            DesiredCascades = V < 1 ? 1u : (V > kMaxCascades ? kMaxCascades : V);
        }
        u32  GetDesiredCascades() const { return DesiredCascades; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseSceneResources(FTextureSRVHeap& SRVHeap);

        // Potencia de 2, arredondando PARA BAIXO. O trace decima com `stride = kRaysPerProbe /
        // rayCount` em inteiro, entao so potencia de 2 e exata: pedir 48 da stride 1 e traca os 64
        // — o numero pedido e o numero gasto divergiam em silencio. Arredondar para baixo faz o
        // getter devolver o que a GPU realmente vai fazer; mentir para baixo e melhor que para
        // cima, porque o erro aparece como "pedi 48 e vi 32" em vez de "pedi 48 e paguei 64".
        static int ClampRays(int V) {
            if (V >= kRaysPerProbe) return kRaysPerProbe;
            int R = 1;
            while (R * 2 <= V) R *= 2;
            return R;
        }

        // Reagenda o passe de relocacao, que e quem escreve ProbeData E ProbeRayCount. Nao pode
        // depender de `Relocation`: a contagem de raios sai do MESMO passe, entao com relocacao
        // desligada os setters de AdaptiveRays/Max/MinRays nao chegavam a lugar nenhum.
        void TriggerReclassify() {
            if (Ready && RelocateFramesLeft < kReclassifyFrames)
                RelocateFramesLeft = kReclassifyFrames;
        }
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return CB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(DDGIConstants);
        }

        FComputePipeline TracePSO;
        FComputePipeline UpdatePSO;
        FComputePipeline UpdateDistPSO;
        FComputePipeline RelocatePSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> IrradAtlas;       
        Microsoft::WRL::ComPtr<ID3D12Resource> DistAtlas;        
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbesTrace;      
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceGeoBuf;
        u32                                    InstanceGeoCount = 0; // capacidade do snapshot acima
        // Definido no .cpp (DDGIInstanceGeo e local daquele arquivo); _Mapped tem _Count entradas.
        void FillInstanceGeo(const FScene& Scene, u8* Mapped, u32 Count) const;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeDataBuf;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeRayCountBuf; 

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 AtlasSRVSlot       = kInvalidSlot;
        u32 AtlasUAVSlot       = kInvalidSlot;
        u32 DistSRVSlot        = kInvalidSlot;
        u32 DistUAVSlot        = kInvalidSlot;
        u32 ProbesTraceSRVSlot = kInvalidSlot;
        u32 ProbesTraceUAVSlot = kInvalidSlot;
        u32 InstanceSRVSlot    = kInvalidSlot;
        // SRVs bindless de VB/IB por mesh único (2 slots contíguos por mesh: base+2i = VB,
        // base+2i+1 = IB) — o InstanceGeo carrega os índices; substitui os merged buffers.
        u32 MeshGeoSlotBase    = kInvalidSlot;
        u32 MeshGeoSlotCount   = 0;
        // Mesh -> slot bindless do VB (IB = +1). Era local do SetupForScene; virou membro porque
        // o RefreshInstanceGeo precisa remontar o snapshot sem refazer a alocacao de descriptors.
        std::unordered_map<const FGpuMesh*, u32> MeshGeoSlot;
        u32 ProbeDataSRVSlot   = kInvalidSlot;
        u32 ProbeDataUAVSlot   = kInvalidSlot;
        u32 ProbeRayCountSRVSlot = kInvalidSlot;
        u32 ProbeRayCountUAVSlot = kInvalidSlot;
        // Tabela do trace versionada por frame em voo: o t8 (luzes) e reescrito todo frame e o
        // frame anterior ainda pode estar lendo a tabela dele no heap shader-visible.
        static constexpr u32 kTraceTables = 2; // == FCommandQueue::kFramesInFlight (assert no .cpp)
        u32 TraceTable[kTraceTables] = { kInvalidSlot, kInvalidSlot };
        u32 SceneGITableStart_ = kInvalidSlot;
        u32 UpdateTableStart   = kInvalidSlot;   // [ProbesTrace, ProbeData] p/ Update/UpdateDist

        D3D12_RESOURCE_STATES AtlasState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES DistState      = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbesState    = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB   = nullptr;
        u32 FrameSlot  = 0;
        DDGIConstants CPU{};

        // Uma cascata = um volume. A CONTAGEM de sondas por eixo e comum a todas (mesma escolha da
        // Flax): so o par (origem, espacamento) varia, o que faz caber num Vec4 por cascata e
        // deixa bandas, linhas e indice local derivaveis de `count` — sem campo novo de cbuffer em
        // nenhum dos cinco consumidores do gather.
        struct FCascade {
            Vec3 GridMin{ 0,0,0 };
            f32  Spacing = 1.0f;
        };
        FCascade Cascades[kMaxCascades]{};
        u32  CascadeCount_    = 1;
        // Centro da AABB da cena, guardado no SetupForScene. A 6.2b-i ancora as cascatas finas
        // AQUI, e nao na camera: com duas variaveis novas (a segunda cascata e o scrolling) um
        // artefato na transicao seria ambiguo. A 6.2b-ii troca isto pela posicao da camera, que e
        // quando o scrolling passa a ser obrigatorio.
        Vec3 SceneCenter{};
        // Razao de espacamento entre cascatas vizinhas. 4 leva os 8,02 m do Bistro a 2,0 m na
        // fina, dentro dos 1-2 m que o paper pede para escala humana. A Flax usa {1,3,6,10} a
        // partir da mais fina; aqui a GROSSA e que esta fixada pela cena, entao a escala corre no
        // sentido contrario.
        static constexpr f32 kCascadeSpacingRatio = 4.0f;
        // Pedido pelo usuario; so vira CascadeCount_ no proximo SetupForScene, porque a contagem
        // dimensiona atlas e buffers. Default 1 = comportamento historico ate a 6.2b ter A/B.
        u32  DesiredCascades  = 1;
        int  CountX = 0, CountY = 0, CountZ = 0;
        u32  ProbesPerCascade_ = 0;
        u32  NumProbes   = 0; // TOTAL = ProbesPerCascade_ * CascadeCount_
        u32  AtlasWidth  = 0, AtlasHeight = 0;
        u32  DistAtlasWidth = 0, DistAtlasHeight = 0;
        // Grade de tiles do atlas. TilesPerRow e sempre um MULTIPLO de CountX (a banda contem
        // fileiras Z inteiras — ver atlasGridFor no SetupForScene), e TileRows ja inclui as
        // bandas: `TileRows = ceil(CountZ/zRowsPerBand) * CountY`.
        u32  TilesPerRow        = 1;
        u32  TileRowsPerCascade = 1; // total de linhas = este x CascadeCount_

        static constexpr D3D12_RESOURCE_STATES kAtlasRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        f32  Intensity    = 1.0f;
        f32  Hysteresis   = kHysteresisMax;
        bool AdaptiveHysteresis = false; // OFF ate A/B da recalibracao; ver SetAdaptiveHysteresis
        bool HysteresisResetPending = false; // ver ResetHistoryOnce
        // Histerese do atlas de DISTANCIA, deliberadamente acima do teto da irradiancia: la a
        // mistura temporal e atraso, aqui e a RECONSTRUCAO de um estimador com ~0,44 raio por
        // texel por frame (o expoente 50 do UpdateDist estreita o lobo a 9,5 graus a meio peso).
        // O tamanho efetivo da amostragem de uma EMA e (1+h)/(1-h): 0.99 da ~199, 0.98 daria
        // ~99 — metade das amostras que formam os momentos do Chebyshev. Constante e nao knob
        // ate haver A/B que peca outra coisa. Ver o bloco no DDGIUpdateDist.cs.hlsl.
        static constexpr f32 kDistHysteresis = 0.99f;

        // Invalidacao espacial (ver InvalidateRegion). O par (histerese, janela) e escolhido por
        // UM criterio: quanto do historico velho pode sobrar quando a janela fecha. Fixado o
        // residual, a histerese vira um knob de VARIANCIA — quanto mais alta, mais frames e menos
        // ruido por frame, com a mesma limpeza no fim.
        //
        // Era 0.75 / 12 frames (3,2% de residual). Reprovado em A/B 2026-08-11: mover um objeto
        // fazia a regiao dele PISCAR, com o detector ligado ou desligado. A causa e o orcamento
        // de raios: o numero efetivo de amostras de uma EMA e (1+h)/(1-h), ou seja 7 a 0.75. Com
        // 64 raios por sonda (~32 com peso positivo por texel), 7 amostras nao seguram a
        // variancia do estimador e a media temporal vira cintilacao visivel.
        //
        // 0.90 da 19 amostras efetivas — 2,7x a variancia a menos — pela mesma limpeza, so que
        // em 34 frames em vez de 12. Para um CACHE isso e barato: o custo e a regiao levar ~0,6 s
        // em vez de ~0,2 s para assentar. Se ainda piscar, o proximo degrau e 0.95 (39 amostras,
        // 69 frames) e basta trocar este numero — a janela acompanha.
        static constexpr f32 kInvalidateResidual   = 0.03f;
        static constexpr f32 kInvalidateHysteresis = 0.90f;
        static constexpr u32 kInvalidateFrames =
            DDGIFramesForResidual(kInvalidateHysteresis, kInvalidateResidual);
        // Faixa util da janela derivada. Pega os dois erros de retune: histerese baixa demais
        // (janela curta = a regiao volta a piscar, que e o bug que 0.75 tinha) e histerese perto
        // da base (janela longa demais = a regiao fica reduzida por mais de 2 s e a invalidacao
        // vira lag visivel em vez de resposta).
        static_assert(kInvalidateFrames >= 4 && kInvalidateFrames <= 128,
                      "kInvalidateHysteresis fora da faixa util para o residual pedido");

        // O atlas de DISTANCIA usa a mesma CAIXA e nada mais: histerese e janela sao proprias.
        // Motivo, de novo, o orcamento de raios — 0.90 daria 19 amostras efetivas de um
        // estimador com ~0,44 raio por texel por frame, e o flicker de iluminacao viraria
        // flicker de VISIBILIDADE. 0.95 da 39 amostras, o que poe o ruido temporal dos momentos
        // ABAIXO do piso de precisao que a quantizacao em half ja impoe (~4% relativo). O preco
        // e a janela: 69 frames contra 34.
        static constexpr f32 kInvalidateDistHysteresis = 0.95f;
        static constexpr u32 kInvalidateDistFrames =
            DDGIFramesForResidual(kInvalidateDistHysteresis, kInvalidateResidual);
        static_assert(kInvalidateDistFrames >= kInvalidateFrames,
                      "a janela do dist atlas nao pode ser mais curta que a da irradiancia: ele "
                      "reconstroi de menos amostras, entao precisa de MAIS tempo, nao menos");

        Vec3 InvalidateMin_{};
        Vec3 InvalidateMax_{};
        u32  InvalidateFramesLeft_ = 0;
        u32  InvalidateDistFramesLeft_ = 0; // janela propria, mais longa (ver acima)
        // A classificacao de sondas (inativa/ativa e contagem de raios) congela quando a
        // relocacao converge, entao uma edicao de cena a deixa DESATUALIZADA: parede removida =
        // sonda que continua com a contagem de espaco fechado, objeto novo = sonda engolida que
        // ninguem mais marca inativa (o proprio DDGIUpdate lamenta esse caso no comentario da
        // sonda enterrada). A invalidacao regional sabe que a cena mudou ali; e o gancho certo.
        //
        // COALESCIDO no fechamento da janela, e nao disparado na hora: `InvalidateRegion` e
        // chamada por FRAME durante um arraste de gizmo, e reclassificar a cada tique prenderia
        // o update no caminho SINCRONO (CanRunAsync le RelocateFramesLeft) pelo gesto inteiro,
        // alem de segurar o trace nos 64 raios o tempo todo. Como cada chamada reabre a janela,
        // ela so fecha quando o gesto para — e ai a reclassificacao acontece uma vez so.
        bool ReclassifyPending_ = false;
        f32  SkyIntensity = 1.0f;
        // NormalBias saiu daqui: o bias dos shadow rays do 2o hit e o mesmo p/ ReSTIR, reflexoes e
        // DDGI (o nome era historico), entao vive no perfil compartilhado — sem isso o sweep de
        // calibracao deixaria o DDGI em 20 cm enquanto os outros descem.
        FRayEpsilonProfile RayEps; // perfil compartilhado (dono = Renderer)
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        FRadianceCacheShaderParams RadianceCacheParams{};
        Vec4               SkyLutParams{};
        f32  MaxRayDist   = 0.0f;
        bool FoliageShadows = true; // sombra de folhagem nos shadow rays do GI (GATHER vs OPAQUE)
        bool Relocation     = true; 
        f32  DeactivationThreshold = 0.20f; 
        bool AdaptiveRays   = false;
        int  MaxRays        = 64;
        int  MinRays        = 16;
        f32  SurfaceBiasScale = 0.2f;  // o `bias` do GetDDGISurfaceBias do Flax
        // Defaults LIGADOS (o legado seria 0.0f / false). Com o grid dimensionado pela AABB da
        // cena, o bias sem teto vale 1,20 m no Bistro — o ponto de amostragem atravessa parede.
        f32  SurfaceBiasMax   = 0.40f; // metros; 0 = sem teto (comportamento historico)
        f32  VolumeFadeProbes = 1.0f;  // celulas de fade na borda; 0 = sem fallback (historico)
        static constexpr u32 kRelocateConvergeFrames = 180;
        static constexpr u32 kReclassifyFrames = 6;
        u32  RelocateFramesLeft = 0;

        bool Initialized = false;
        bool Ready       = false;
    };
}
