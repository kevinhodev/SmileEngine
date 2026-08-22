#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <functional>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FGpuProfiler;
    class FGpuMesh;
    class FMaterial;

    struct alignas(256) CSMConstants {
        Mat44 WorldToShadow[4];  // 4*64 = 256 bytes
        Vec4  CascadeTexelWorld; // x..w = tamanho de 1 texel em mundo, por cascata (normal-offset)
        Vec4  Params;            // x = numCascades, y = depthBias EM TEXELS (informativo; o shader usa BiasNdc), z = 1/res, w = enabled
        Vec4  Params2;           // x = normal-offset (em texels), yzw reservado
        Vec4  Params3;           // x = frame do ruido do PCF, y = tan(meio-angulo do sol; 0 = PCSS off), z = penumbra max (texels)
        // Depth bias JA EM NDC, resolvido por cascata na CPU: DepthBiasTexels * texel[c] /
        // range[c] * CascadeBiasScale[c]. E o que torna o bias constante EM TEXELS em todas
        // as cascatas — a forma da Unreal (ShadowCascadeBiasDistribution = 1) e do
        // e_ShadowsAutoBias da Cry. Com um escalar unico em NDC o bias valia 3,28 texels na
        // cascata 0 contra 1,28 na 3, porque o CasterPullback e aditivo (range = 2R + 80).
        Vec4  BiasNdc;
        Vec4  DepthRangeWorld;   // extensao em mundo do range de depth do ortho, por cascata (PCSS)
        Vec4  CascadeSplits;     // profundidade view-space do fim de cada cascata
        Vec4  CameraPosition;    // xyz = camera em mundo
        Vec4  CameraForwardNear; // xyz = frente da camera, w = near plane
        Vec4  SunDirection;      // xyz = direcao PARA a key light (normal-offset por N.L)
    };

    struct alignas(256) ShadowCascadeConstants {
        Mat44 LightViewProj;
    };

    class FSunShadows : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Sombras — sol (CSM)"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        static constexpr u32 kNumCascades = 4;
        static constexpr u32 kResolution  = 2048;

        struct FShadowDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
            // Ver EMobility. A lista chega ordenada com os estaticos PRIMEIRO, entao este
            // campo tambem marca uma fronteira: [0, N) estatico, [N, Count) dinamico. E a
            // divisao que o cache vai usar para rasterizar as duas metades em alvos
            // diferentes; por ora ela so e medida.
            bool                      Dynamic;
        };

        // Contadores por cascata. Existem para MEDIR: a decisao de separar casters em
        // estatico/dinamico so se paga se as cascatas caras forem tambem as frequentes, e
        // isso depende do tamanho do mundo — na Bistro (~200 m) o culling da fatia ja nao
        // corta quase nada na cascata 3, num mundo grande corta muito. Sem numero por
        // cascata a escolha vira chute.
        //
        // Os campos de contagem descrevem o ULTIMO UPDATE desta cascata, nao o frame atual:
        // cascata congelada pelo cache nao desenha nada, e zerar aqui perderia justamente a
        // informacao que interessa (o que ela custa QUANDO dispara). A frequencia sai do
        // historico, e o custo medio por frame e Submitted * popcount(UpdateHistory) / 64.
        struct FCascadeStats {
            u32  Submitted        = 0; // total desenhado (estatico + dinamico), pos-culling
            // Estatico RETIDO entre frames: so e reescrito quando o mapa estatico e refeito, e
            // por isso e a base a partir da qual o total e recomposto todo frame. Somar o
            // dinamico direto em Submitted faria o numero crescer sem limite nas cascatas que
            // nao refazem o estatico.
            u32  SubmittedStatic  = 0;
            u32  SubmittedDynamic = 0; // redesenhado todo frame, seja qual for a taxa do cache
            u32  CulledPlanes     = 0; // fora do volume da fatia em espaco de luz
            u32  CulledSize       = 0; // menores que MinCasterTexels
            // Bit 0 = frame mais recente. Deslocado uma vez por frame no UpdatePerFrame,
            // para TODA cascata — inclusive a congelada, que grava 0.
            u64  UpdateHistory    = 0;
            bool UpdatedThisFrame = false;
            // Historico de RE-RASTERIZACAO do mapa estatico, mesma janela de 64 frames. E o
            // numero que a F2 move: o UpdateHistory acima passa a ser quase sempre 64/64
            // (copia + dinamicos e barato), e o que encolhe e este.
            u64  StaticHistory    = 0;
            // Quanto o centro SNAPADO andou desde o ultimo fit desta cascata, em texels dela
            // (o maior dos eixos right/up). Como o snap e floor(centro/texel)*texel, o valor e
            // inteiro enquanto o raio nao muda.
            //
            // E a medida que decide se vale reaproveitar o mapa da cascata proxima em vez de
            // redesenha-la: um delta pequeno significa que quase todo o conteudo continua
            // valido, deslocado, e so uma faixa na borda e conteudo novo — a faixa custa
            // (delta/resolucao) do passe. Delta da ordem da resolucao significa que nao ha o
            // que reaproveitar. Nas cascatas 2/3 o cache ja congela o fit, entao o numero so
            // muda quando elas refazem.
            f32  SnapDeltaTexels  = 0.0f;
        };

        const FCascadeStats& CascadeStats(u32 Cascade) const {
            return Stats[Cascade < kNumCascades ? Cascade : 0];
        }
        // Tamanho da lista que o Renderer submeteu (igual para as 4 cascatas): o teto contra
        // o qual os cortes de cada cascata sao lidos.
        u32 GetSubmittedCasterCount() const { return LastCasterCount; }

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        // StaticCastersVersion vem do FScene: muda quando o CONTEUDO do mapa estatico muda
        // (estatico se move, aparece, some, e ocultado, ou troca de mobilidade). E o sinal que
        // dispensa o redesenho periodico "por garantia" — ver o cache abaixo.
        void UpdatePerFrame(u32 FrameSlot, bool Enabled, const Mat44& View, const Vec3& CamPos,
                            f32 FovYRadians, f32 Aspect, const Vec3& DirToSun, f32 NearZ,
                            f32 NoiseFrame, u64 StaticCastersVersion);

        // Caster extra por cascata (terreno): chamado depois dos itens, com o CB e a matriz
        // da cascata. Pode trocar root signature/PSO — o loop re-liga os do CSM na proxima
        // cascata.
        using FExtraCascadeDraw = std::function<void(ID3D12GraphicsCommandList*, u32 Cascade,
                                                     D3D12_GPU_VIRTUAL_ADDRESS CascadeCB,
                                                     const Mat44& CascadeViewProj)>;

        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             const FShadowDrawItem* Items, size_t Count,
                             const FExtraCascadeDraw& ExtraDraw = {},
                             FGpuProfiler* Profiler = nullptr);

        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);
        // Leitura tambem em compute (volumetric fog): PIXEL | NON_PIXEL.
        void EnsureReadableCompute(ID3D12GraphicsCommandList* CommandList);

        u32  ShadowSRVSlot() const { return ShadowSRVSlot_; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const {
            return CSMCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(CSMConstants);
        }
        bool IsInitialized() const override { return Initialized; }

        void SetMaxDistance(f32 D)      { ShadowMaxDistance = D; InvalidateCache(); }
        void SetDepthBias(f32 Texels)   { DepthBiasTexels = Texels; }
        void SetCasterPullback(f32 P)   { CasterPullback = P; InvalidateCache(); }
        void SetNormalOffset(f32 Texels){ NormalOffsetTexels = Texels; }
        void SetPenumbra(f32 Texels)    { PcfRadiusTexels = Texels; } // piso da penumbra do PCSS
        void SetBlendBand(f32 Fraction) { BlendBand = Fraction; InvalidateCache(); }
        void SetDebugCascades(bool On)  { DebugCascades = On; }
        f32  GetMaxDistance() const     { return ShadowMaxDistance; }
        f32  GetDepthBias() const       { return DepthBiasTexels; } // em texels da cascata
        f32  GetNormalOffset() const    { return NormalOffsetTexels; }
        f32  GetPenumbra() const        { return PcfRadiusTexels; }
        f32  GetBlendBand() const       { return BlendBand; }
        bool GetDebugCascades() const   { return DebugCascades; }

        // Cache de cascatas distantes (round-robin) + filtro de caster pequeno + bias por cascata.
        void SetCascadeCache(bool On)   { CacheEnabled = On; InvalidateCache(); }
        bool GetCascadeCache() const    { return CacheEnabled; }
        void SetMinCasterTexels(f32 T)  { MinCasterTexels = T; }
        f32  GetMinCasterTexels() const { return MinCasterTexels; }
        void SetCascadeBiasScale(u32 C, f32 S) { if (C < kNumCascades) CascadeBiasScale[C] = S; }
        f32  GetCascadeBiasScale(u32 C) const  { return C < kNumCascades ? CascadeBiasScale[C] : 1.0f; }

        // PCSS (contact hardening, cascata 0). Tamanho angular do sol em graus; 0 = off.
        void SetSunAngularSize(f32 Deg)   { SunAngularSizeDeg = Deg; }
        f32  GetSunAngularSize() const    { return SunAngularSizeDeg; }
        void SetMaxPenumbraTexels(f32 T)  { MaxPenumbraTexels = T; }
        f32  GetMaxPenumbraTexels() const { return MaxPenumbraTexels; }

        // Bypass ativo: o sol esta rapido demais e o cache foi abandonado neste frame.
        bool IsCacheBypassed() const { return CacheBypass; }
        void SetSunToleranceTexels(f32 T) { SunToleranceTexels = T; InvalidateCache(); }
        f32  GetSunToleranceTexels() const { return SunToleranceTexels; }

    private:
        void InvalidateCache() { for (u32 c = 0; c < kNumCascades; ++c) CacheValid[c] = false; }

        // Os dois filtros do passe de profundidade, no lugar unico onde as tres fases (mapa
        // estatico, copia, dinamicos) precisam concordar. Ordem: tamanho e depois planos —
        // e a ordem em que os contadores atribuem o corte.
        bool CasterVisible(const FShadowDrawItem& It, u32 Cascade, f32 MinExtent,
                           FCascadeStats* St) const;
        // Desenha [Begin, End) da lista na cascata; devolve quantos passaram. O DSV, o
        // viewport e a root signature ja tem de estar ligados pelo chamador.
        u32  DrawCasters(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                         const FShadowDrawItem* Items, size_t Begin, size_t End,
                         u32 Cascade, FCascadeStats* St);
        void TransitionStatic(ID3D12GraphicsCommandList* CommandList,
                              D3D12_RESOURCE_STATES After);

        void CreateResources(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);
        void TransitionArray(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CascadeCBAddr(u32 Cascade) const {
            return CascadeCB->GetGPUVirtualAddress() +
                   (static_cast<UINT64>(FrameSlot) * kNumCascades + Cascade) *
                       sizeof(ShadowCascadeConstants);
        }

        Microsoft::WRL::ComPtr<ID3D12Resource>      DepthArray;
        FDescriptorHeap                             DSVHeap;
        u32                                         ShadowSRVSlot_ = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES                       ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        // Gemeo do DepthArray com SO os casters estaticos. Mesmo formato e mesmas dimensoes de
        // proposito: a transferencia para o alvo de runtime vira um CopyTextureRegion exato,
        // sem shader e sem reamostragem.
        Microsoft::WRL::ComPtr<ID3D12Resource>      StaticArray;
        FDescriptorHeap                             StaticDSVHeap;
        D3D12_RESOURCE_STATES                       StaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OpaquePSO; 
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskedPSO; 

        Microsoft::WRL::ComPtr<ID3D12Resource>      CascadeCB; 
        u8*                                         MappedCascade = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource>      CSMCB;      
        u8*                                         MappedCSM = nullptr;
        CSMConstants                                CPUConstants{};
        Mat44                                       CascadeViewProj[kNumCascades]{};
        // 5 planos de culling em MUNDO por cascata (4 laterais da fatia + far do ortho; sem
        // near, por causa do pancaking). Retidos entre updates junto com a matriz, para que
        // cascata congelada pelo cache continue cullando contra o volume que gerou o mapa.
        Vec4                                        CullPlanes[kNumCascades][5]{};

        u32  FrameSlot = 0;
        f32  ShadowMaxDistance   = 800.0f;
        f32  DistributionExponent = 3.0f;
        // Bias em TEXELS da propria cascata (nao em NDC): a conversao para NDC e por cascata,
        // em UpdatePerFrame. 2,0 fica dentro da faixa que ja era usada nas cascatas proximas
        // (a 0 valia 3,28 e a 1 valia 1,75) e acima da que valia nas distantes (1,37 e 1,28),
        // entao nao introduz acne em lugar nenhum e corta ~40% do peter-panning de contato.
        f32  DepthBiasTexels     = 2.0f;
        f32  NormalOffsetTexels  = 2.5f;
        f32  CasterPullback      = 80.0f;
        // PISO da penumbra do PCSS, em texels. O caminho de raio fixo virou o optimized PCF
        // 5x5 (determinístico, 9 taps), entao este knob so governa o PCSS das cascatas 0-1 —
        // e o valor casa com a meia-largura do 5x5, para que a troca entre as duas familias
        // de filtro seja continua no contato, onde o PCSS colapsa.
        f32  PcfRadiusTexels     = 2.0f;
        // Fracao final de cada intervalo de view-depth usada no crossfade. A cascata
        // seguinte e ajustada com a mesma sobreposicao (contrato Flax/Unreal).
        f32  BlendBand           = 0.1f;
        bool DebugCascades       = false;
        bool Initialized         = false;

        // CACHE ESTATICO. O shadow map e uma foto da cena tirada do ponto de vista do sol.
        // Camera andando nao muda essa foto (muda o recorte); objeto andando muda so um
        // pedaco dela; sol andando produz OUTRA foto. Dai a estrutura: o que nao se move vive
        // num alvo proprio (StaticArray), rasterizado so quando invalida, e o array que todo
        // mundo amostra recebe uma copia dele mais os dinamicos por cima, todo frame.
        //
        // Antes daqui existia um round-robin que redesenhava as cascatas 2/3 a cada 2 e 4
        // frames INCONDICIONALMENTE. Aquilo era um timer cobrindo a falta de um sinal: nao
        // havia como saber se o conteudo tinha mudado, entao redesenhava de tempos em tempos
        // por garantia. Com o StaticCastersVersion o sinal existe, e a mesma aritmetica de
        // fase passa a servir ao contrario — nao para FORCAR redesenho, mas para ESPALHAR os
        // que ficaram pendentes (ver StaticRefreshMask), que e o eFullUpdateTimesliced da Cry
        // em miniatura.
        // Esteve em false durante o baseline de 2026-08-07 (investigacao de firefly), e o teste
        // INOCENTOU o cache: o artefato apareceu igual com ele desligado. Restaurado.
        // ⚠️ Nota de contexto: o cache so passou a rodar de fato em 2026-08-07 — antes disso o
        // call site do UpdatePerFrame nao compilava (faltava o StaticCastersVersion) e a frente
        // inteira estava inerte. Ou seja, ele nunca teve A/B proprio; se aparecer sombra estatica
        // grudada ou pop ao mover objeto, suspeitar daqui. `SetCascadeCache(false)` desliga.
        bool CacheEnabled  = true;
        u32  UpdateMask    = 0xFu;              // cascatas com trabalho no alvo de runtime
        u32  StaticRefreshMask = 0xFu;          // cascatas que re-rasterizam o mapa estatico
        u64  UpdateCounter = 0;
        bool CacheValid[kNumCascades]  = {};
        Vec3 CachedFwd[kNumCascades]{};         // dir da luz no ultimo update
        Vec3 CachedCenter[kNumCascades]{};      // centro (snapped) da esfera congelada
        f32  CachedRadius[kNumCascades] = {};   // raio (com folga) da esfera congelada
        // Versao do conjunto estatico com que cada mapa foi rasterizado.
        u64  CachedContentVersion[kNumCascades] = {};
        // Dinamicos desenhados no alvo de runtime desta cascata no frame ANTERIOR. Se agora
        // sao 0 e antes tambem eram, e o estatico nao mudou, o alvo de runtime ja esta certo e
        // a cascata nao custa NADA — nem a copia. E o caso da cena 100% estatica.
        u32  LastDynamicCount[kNumCascades] = {};
        // Sol girando rapido demais para qualquer cache sobreviver a um frame: manter o mapa
        // estatico so paga custo sem colher nada, entao ele e abandonado e o passe volta a
        // rasterizar tudo direto no alvo de runtime — exatamente o caminho de antes do cache.
        // Nao e um switch de "TOD ligado": um ciclo dia/noite lento mantem o cache inteiro, e
        // e o caso comum. O que decide e a velocidade medida, nao a existencia do TOD.
        bool CacheBypass   = false;
        Vec3 LastFrameSunFwd{};
        bool HasLastSunFwd = false;
        // Tolerancia do sol EM TEXELS de deslocamento da sombra, nao em graus. Um limiar
        // angular unico erra por um fator de 2,6 entre as cascatas: o deslocamento vale
        // range * delta, e range/texel e 5470 na cascata 0 contra 2127 na 3 — a proxima e a
        // mais sensivel, ao contrario da intuicao. Convertido por cascata em UpdatePerFrame.
        f32  SunToleranceTexels = 1.0f;
        FCascadeStats Stats[kNumCascades]{};
        // Centro snapado do fit anterior, em coordenadas de luz (right, up), p/ o delta acima.
        f32  PrevSnapR[kNumCascades] = {};
        f32  PrevSnapU[kNumCascades] = {};
        bool HasPrevSnap[kNumCascades] = {};
        u32  LastCasterCount = 0;               // itens que o Renderer submeteu no ultimo passe
        f32  MinCasterTexels = 2.0f;            // caster menor que N texels da cascata nao desenha (0 = off)
        f32  CascadeBiasScale[kNumCascades] = { 1.0f, 1.0f, 1.0f, 1.0f };
        f32  SunAngularSizeDeg = 0.53f;         // diametro angular do sol (PCSS); 0 = penumbra fixa
        f32  MaxPenumbraTexels = 8.0f;          // teto do kernel PCSS = raio do blocker search
    };
}
