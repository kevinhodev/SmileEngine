#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Graphics/Renderer.h"

// Corpos movidos do Renderer.h. Ver a nota de transicao no RenderSettings.h: neste passo os
// corpos sao MOVIDOS, nao reescritos — nem uma invalidacao a mais, nem uma a menos. As quatro
// divergencias catalogadas no Docs/KNOBS-AUDIT.md ficam para o passo seguinte, em commit
// separado, para que o A/B consiga distinguir "a invalidacao nova esta errada" de "a fachada
// quebrou um forward".
namespace Smile {

    namespace Dom = HistoryDomain;

    // Executor unico do grafo. Uma linha por alvo — o mapeamento bit -> chamada mora SO aqui,
    // que e o que permite acrescentar um acumulador novo sem varrer os setters.
    void FRenderSettings::Invalidate(EHistoryTarget _Targets) {
        using T = EHistoryTarget;
        // FUNIL DA CAPTURA. Todo knob que derruba acumulador passa por aqui, e derrubar um
        // acumulador no meio do aquecimento quebra o contrato "N frames apos UM reset" — a
        // captura sairia sub-aquecida com o manifesto afirmando N. Como este e o ponto unico por
        // onde a invalidacao passa, um gate aqui cobre os ~40 setters de uma vez, em vez de um
        // gate por setter que o proximo knob esqueceria.
        //
        // O proprio capturador entra por aqui — no preset, no reset e na restauracao —, e por isso
        // a excecao. Ela cobre o UpdateFrameCapture INTEIRO e nao so o reset: a restauracao passa
        // pelos setters, e um pedido novo enfileirado no mesmo frame seria descartado por ela.
        //
        // NAO e completo, e vale registrar o limite: TemporalMotion e NrdDirect tem chamadas
        // diretas de InvalidateHistory no Renderer que nao passam pelo funil.
        if (!R.CaptureSetupGuard)
            R.Capture.Cancel("um ajuste derrubou historico durante o aquecimento");
        if (HasTarget(_Targets, T::DDGIAtlas))        R.DDGI.ResetHistoryOnce();
        if (HasTarget(_Targets, T::ReGIR))            R.ReGIR.InvalidateHistory();
        if (HasTarget(_Targets, T::ReSTIRGI))         R.ReSTIRGI.InvalidateHistory();
        if (HasTarget(_Targets, T::ReSTIRDI))         R.ReSTIRDI.InvalidateHistory();
        if (HasTarget(_Targets, T::Reflections))      R.Reflections.InvalidateHistory();
        if (HasTarget(_Targets, T::NrdIndirect))      R.Nrd.InvalidateHistory();
        if (HasTarget(_Targets, T::NrdDirect))        R.NrdDirect.InvalidateHistory();
        if (HasTarget(_Targets, T::RayReconstruct))   R.RRResetPending = true;
        if (HasTarget(_Targets, T::TemporalAA))       R.TAARanLastFrame = false;
        if (HasTarget(_Targets, T::VolumetricFog))    R.VolumetricFog.ResetHistory();
        if (HasTarget(_Targets, T::VolumetricClouds)) R.VolumetricClouds.InvalidateHistory();
        if (HasTarget(_Targets, T::TemporalMotion))   R.TemporalMotion.InvalidateHistory();
        if (HasTarget(_Targets, T::HiZOcclusion))     R.HiZ.InvalidateResults();
        if (HasTarget(_Targets, T::ProbeDiagnostic))  R.RepeatDebugProbePoint();
        if (HasTarget(_Targets, T::RadianceCache))    R.RadianceCache.ResetOnce();
        if (HasTarget(_Targets, T::SunShafts))        R.SunShafts.ResetHistory();
        if (HasTarget(_Targets, T::OceanTemporal)) {
            for (u32 C = 0; C < Renderer::kOceanCascades; ++C)
                R.Ocean[C].ResetTemporalHistory();
        }
    }

    // === Apresentacao e escala ==========================================================

    void FRenderSettings::SetVSync(bool _Enabled) { R.SwapChain.SetVSync(_Enabled); }
    bool FRenderSettings::GetVSync() const        { return R.SwapChain.GetVSync(); }

    void FRenderSettings::SetRenderScale(f32 _V) { R.SetRenderScale(_V); }
    f32  FRenderSettings::GetRenderScale() const { return R.RenderScale; }

    // === Upscaler e denoiser ============================================================

    void FRenderSettings::SetUpscaler(EUpscaler _U) {
        if (_U != EUpscaler::None && !UpscalerAvailable(_U)) _U = EUpscaler::None;
        // Acoplamento: o RR faz o upscale via DLSS. Trocar o upscaler p/ fora de DLSS desliga o
        // RR (cai p/ NRD). Vai pelo SetDenoiser em vez de atribuir Denoiser direto: a
        // atribuicao pulava Nrd.InvalidateHistory(), ReSTIRGI.InvalidateHistory() e o
        // RRResetPending, entao o NRD reaproveitava acumulacao de outro denoiser e os
        // reservoirs entravam no NRD ainda com o teto de firefly do modo cru (4 em vez de 8).
        // Nao recursa: o SetDenoiser so chama SetUpscaler quando o denoiser alvo e DLSS_RR, e
        // aqui o alvo e sempre NRD.
        const bool LeavingRR = (R.Denoiser == EDenoiser::DLSS_RR && _U != EUpscaler::DLSS);
        R.Upscaler = _U;
        Invalidate(Dom::TemporalOnly);
        if (LeavingRR) SetDenoiser(EDenoiser::NRD); // ja faz o ApplyUpscalerScale()
        else           R.ApplyUpscalerScale();
    }

    EUpscaler FRenderSettings::GetUpscaler() const { return R.Upscaler; }

    bool FRenderSettings::UpscalerAvailable(EUpscaler _U) const {
        switch (_U) {
            case EUpscaler::FSR:  return R.Fsr.Available();
            case EUpscaler::DLSS: return R.Dlss.Available();
            default:              return true; // None sempre disponivel
        }
    }

    void FRenderSettings::SetUpscalerQuality(int _Q) {
        R.UpscalerQuality = _Q < 0 ? 0 : (_Q > 4 ? 4 : _Q);
        R.ApplyUpscalerScale();
    }
    int FRenderSettings::GetUpscalerQuality() const { return R.UpscalerQuality; }

    void FRenderSettings::SetUseTAA(bool _V) { R.UseTAA = _V; Invalidate(Dom::TemporalOnly); }
    bool FRenderSettings::GetUseTAA() const  { return R.UseTAA; }

    void FRenderSettings::SetDenoiser(EDenoiser _D) {
        if (_D == EDenoiser::DLSS_RR && !R.DlssRR.Available()) _D = EDenoiser::NRD; // sem NVIDIA/RR
        if (_D == R.Denoiser) return;
        // Muda a natureza do sinal, entao reinicia acumulacao. Detalhes que o dominio carrega:
        // o teto de firefly do ReSTIR depende do denoiser e e aplicado ao Lo NA HORA DO TRACE
        // (fica gravado no reservoir, e com ValidateInterval = 0 nao ha re-shade que corrija);
        // o DI usa permutation temporal so no RR, entao nao misture reservoirs de politicas de
        // reprojecao diferentes; e NRD/RR param no Resolved cru, deixando o History[] das
        // reflexoes sem escrita — ao voltar ao caminho legado ele tem frames antigos.
        Invalidate(Dom::DenoiserSwap);
        R.Denoiser = _D;
        if (R.Denoiser == EDenoiser::DLSS_RR) // RR faz o upscale; trava o upscaler em DLSS
            SetUpscaler(EUpscaler::DLSS);
        R.ApplyUpscalerScale();
        // Depois do ApplyUpscalerScale: ele pode disparar RecreateInternalTargets (que ja
        // reconcilia), e ai isto vira no-op. Sair do NRD devolve os ~336 MB das duas instancias.
        R.ReconcileNrdAllocation();
    }

    EDenoiser FRenderSettings::GetDenoiser() const { return R.Denoiser; }
    bool      FRenderSettings::RRAvailable() const { return R.DlssRR.Available(); }

    void FRenderSettings::SetUseNrdDenoise(bool _V) {
        SetDenoiser(_V ? EDenoiser::NRD : EDenoiser::None);
    }
    bool FRenderSettings::GetUseNrdDenoise() const { return R.Denoiser == EDenoiser::NRD; }

    // === Culling e geometria ============================================================

    void FRenderSettings::SetFrustumCulling(bool _Use) { R.UseFrustumCulling = _Use; }
    bool FRenderSettings::GetFrustumCulling() const    { return R.UseFrustumCulling; }

    void FRenderSettings::SetOcclusionCulling(bool _Use) {
        // Ao religar, descarta resultados velhos do readback ring — os proximos
        // kFramesInFlight frames desenham tudo ate ter teste fresco.
        if (_Use && !R.UseOcclusionCulling) Invalidate(EHistoryTarget::HiZOcclusion);
        R.UseOcclusionCulling = _Use;
    }
    bool FRenderSettings::GetOcclusionCulling() const { return R.UseOcclusionCulling; }

    void FRenderSettings::SetDepthPrepass(bool _Use) { R.UseDepthPrepass = _Use; }
    bool FRenderSettings::GetDepthPrepass() const    { return R.UseDepthPrepass; }

    void FRenderSettings::SetUseAsyncCompute(bool _V) { R.UseAsyncCompute = _V; }
    bool FRenderSettings::GetUseAsyncCompute() const  { return R.UseAsyncCompute; }

    // === Iluminacao global ==============================================================

    void FRenderSettings::SetUseGI(bool _V) { R.UseGI = _V; }
    bool FRenderSettings::GetUseGI() const  { return R.UseGI; }

    void FRenderSettings::SetUseReGIR(bool _V) {
        if (_V == R.UseReGIR) return;
        R.UseReGIR = _V;
        Invalidate(Dom::IndirectSampler);
    }
    bool FRenderSettings::GetUseReGIR() const { return R.UseReGIR; }
    bool FRenderSettings::ReGIRActive() const { return R.UseReGIR && R.ReGIR.IsReady(); }

    void FRenderSettings::SetRadianceCacheEnabled(bool _V) {
        if (_V == R.RadianceCache.GetEnabled()) return;
        R.RadianceCache.SetEnabled(_V);
        // O que o raio secundario DEVOLVE muda — e esse valor realimenta o atlas do DDGI e os
        // reservoirs do ReSTIR, que precisam esquecer o regime anterior.
        Invalidate(Dom::RayVisibility);
    }
    bool FRenderSettings::GetRadianceCacheEnabled() const { return R.RadianceCache.GetEnabled(); }

    // Um so lugar monta a mascara: "os consumidores esquecem, a tabela fica". Ela e usada por tres
    // eventos que sao o MESMO evento visto de angulos diferentes — o operador abrindo a leitura, o
    // operador mexendo no aquecimento automatico, e o aquecimento abrindo a leitura sozinho.
    // Duplicar a mascara e como um deles deixaria de acompanhar os outros.
    static EHistoryTarget RadianceCacheConsumersOnly() {
        return static_cast<EHistoryTarget>(static_cast<u32>(Dom::RayVisibility) &
                                           ~static_cast<u32>(EHistoryTarget::RadianceCache));
    }

    void FRenderSettings::SetRadianceCacheQuery(bool _V) {
        if (_V == R.RadianceCache.GetQueryEnabled()) return;
        R.RadianceCache.SetQueryEnabled(_V);
        // Trocar o terminador invalida quem ACUMULOU a resposta, mas nao a tabela que acabamos de
        // aquecer em write-only. Passar RayVisibility inteiro apagaria o cache exatamente quando
        // o usuario liga a leitura para o A/B.
        //
        // Pela funcao, e nao pela copia que estava aqui: era a terceira do mesmo `&~` no arquivo,
        // e as tres tinham de mudar juntas.
        Invalidate(RadianceCacheConsumersOnly());
    }
    bool FRenderSettings::GetRadianceCacheQuery() const {
        return R.RadianceCache.GetQueryEnabled();
    }

    void FRenderSettings::SetRadianceCacheAutoWarmup(bool _V) {
        if (_V == R.RadianceCache.GetAutoWarmup()) return;
        R.RadianceCache.SetAutoWarmup(_V);
        // Mesma forma exata do toggle de query acima, e pelo mesmo motivo: o que muda e QUEM LE a
        // tabela, nao o que ela guarda. Apagar o cache aqui destruiria justamente o aquecimento
        // que este knob existe para administrar.
        Invalidate(RadianceCacheConsumersOnly());
    }

    void FRenderSettings::NotifyRadianceCacheQueryChanged() {
        // Para os consumidores isto e a MESMA coisa que o operador mexer no toggle de leitura: o
        // terminador do raio secundario troca entre fallback e cache. O que eles acumularam foi
        // medido com o outro — os reservoirs do ReSTIR GI, o atlas do DDGI (histerese 0,99, ou
        // seja centenas de updates de memoria), o NRD.
        //
        // Vale NOS DOIS SENTIDOS, e o de fechar demorou mais a aparecer porque parecia inofensivo:
        // o reload de shader reseta a tabela — a semantica da chave pode ter mudado — e a consulta
        // fecha sozinha, sem passar por setter nenhum. Quem estava segurando radiancia vinda do
        // cache continuava a usa-la.
        //
        // Passa pelo funil de proposito: se acontecer no meio de um aquecimento de captura, a
        // sessao TEM de ser cancelada — o contrato "N frames apos UM reset" foi quebrado no meio.
        // E o caso concreto do reload de shader, que ate aqui trocava os pipelines e resetava o
        // cache sem a captura ficar sabendo. A borda do aquecimento nao chega a acontecer dentro
        // de uma sessao: ela roda com o automatico desligado, e o latch so arma quando a consulta
        // muda DE FATO (ver FRadianceCache::ConsumeQueryChange).
        Invalidate(RadianceCacheConsumersOnly());
    }
    bool FRenderSettings::GetRadianceCacheAutoWarmup() const {
        return R.RadianceCache.GetAutoWarmup();
    }
    const char* FRenderSettings::RadianceCacheWarmupName() const {
        return R.RadianceCache.WarmupStateName();
    }
    u32 FRenderSettings::RadianceCacheWarmupFrames() const {
        return R.RadianceCache.WarmupFillFrames();
    }

    void FRenderSettings::SetRadianceCacheDedicatedUpdate(bool _V) {
        if (_V == R.RadianceCache.GetDedicatedUpdate()) return;
        // O setter da classe ja arma o ResetOnce da tabela — as duas fontes produzem estatisticas
        // diferentes para a mesma celula, e uma media que mistura as duas nao descreve nenhuma.
        R.RadianceCache.SetDedicatedUpdate(_V);
        // E os CONSUMIDORES tambem esquecem: o que o raio secundario devolve muda de ORIGEM, e
        // esse valor ja esta acumulado no atlas do DDGI e nos reservoirs do ReSTIR.
        Invalidate(Dom::RayVisibility);
    }
    bool FRenderSettings::GetRadianceCacheDedicatedUpdate() const {
        return R.RadianceCache.GetDedicatedUpdate();
    }

    void FRenderSettings::SetRadianceCacheUpdateFraction(f32 _V) {
        if (_V == R.RadianceCache.GetUpdateFraction()) return;
        // NAO invalida — o que esta na tabela continua valendo, so a taxa de reposicao muda. Mas
        // cancela captura em curso, e por um motivo que a invalidacao nao cobriria: o manifesto
        // grava a fracao do frame FINAL, entao um aquecimento de 128 frames que rodasse metade a
        // 4% e metade a 20% sairia declarando 20% — um arquivo que descreve uma configuracao que
        // nunca existiu. Mesmo caso da instrumentacao do cache, que ja cancela por aqui.
        //
        // Os outros dois knobs do passe (produtor dedicado, terminal) nao precisam disto: eles
        // invalidam, e o funil do Invalidate ja cancela a captura.
        R.Capture.Cancel("a fracao do update do cache mudou durante o aquecimento");
        R.RadianceCache.SetUpdateFraction(_V);
    }
    f32 FRenderSettings::GetRadianceCacheUpdateFraction() const {
        return R.RadianceCache.GetUpdateFraction();
    }

    void FRenderSettings::SetRadianceCacheUsePrevTerminal(bool _V) {
        if (_V == R.RadianceCache.GetUsePrevCacheAtTerminal()) return;
        R.RadianceCache.SetUsePrevCacheAtTerminal(_V);
        // A tabela guarda energias diferentes nos dois regimes (com o terminal, cada celula carrega
        // o multi-bounce acumulado; sem ele, so o primeiro bounce). Uma media que misturasse os
        // dois nao descreveria nenhum — e o A/B compararia historico contaminado.
        Invalidate(Dom::RayVisibility);
    }
    bool FRenderSettings::GetRadianceCacheUsePrevTerminal() const {
        return R.RadianceCache.GetUsePrevCacheAtTerminal();
    }

    void FRenderSettings::SetRadianceCacheMaxVertices(u32 _V) {
        if (_V == R.RadianceCache.GetUpdateMaxVertices()) return;
        R.RadianceCache.SetUpdateMaxVertices(_V); // ja arma o ResetOnce da tabela
        // Com um vertice a celula acumula a serie de ponto fixo ao longo de frames; com quatro ela
        // ja chega resolvida. Sao energias diferentes na MESMA celula, e quem consumiu a anterior
        // (atlas do DDGI, reservoirs) carrega o regime velho.
        Invalidate(Dom::RayVisibility);
    }
    u32 FRenderSettings::GetRadianceCacheMaxVertices() const {
        return R.RadianceCache.GetUpdateMaxVertices();
    }

    void FRenderSettings::SetRadianceCacheMinCacheableRoughness(f32 _V) {
        if (_V == R.RadianceCache.GetMinCacheableRoughness()) return;
        R.RadianceCache.SetMinCacheableRoughness(_V);
        // INVALIDA, e o comentario anterior aqui estava errado. Ele dizia "muda quais amostras
        // NOVAS entram, nao o significado das guardadas" — mas o que muda e justamente o que a
        // CELULA promete conter. Subir o piso quer dizer "daqui em diante so radiancia de lobo
        // largo mora aqui", e a media continuaria carregando as amostras estreitas ja aceitas,
        // por ate 64 delas. O A/B do knob seria feito sobre celulas contaminadas com o regime que
        // ele existe para retirar — e o efeito medido apareceria diluido, ou nao apareceria.
        //
        // Mesmo criterio do produtor dedicado e do numero de vertices: knob que muda o que ENTRA
        // na celula limpa a tabela. O Invalidate tambem cancela captura em curso, entao o
        // Capture.Cancel explicito que estava aqui deixou de ser necessario.
        Invalidate(Dom::RayVisibility);
    }
    f32 FRenderSettings::GetRadianceCacheMinCacheableRoughness() const {
        return R.RadianceCache.GetMinCacheableRoughness();
    }

    void FRenderSettings::SetRadianceCacheMinSampleCount(u32 _V) {
        if (_V == R.RadianceCache.GetMinSampleCount()) return;
        R.RadianceCache.SetMinSampleCount(_V); // ja arma o ResetOnce da tabela
        // INVALIDA pelos dois lados, e vale distinguir os dois porque so um deles e obvio.
        //
        // O obvio: o piso muda o que os traces de render APROVEITAM, e o que eles ja aproveitaram
        // esta acumulado no atlas do DDGI e nos reservoirs do ReSTIR.
        //
        // O outro: o TERMINAL do updater tambem consulta com este piso, entao ele muda o que a
        // celula GUARDA — subir o piso quer dizer "daqui em diante o multi-bounce so entra por
        // celula confiavel", e a media continuaria carregando o que entrou pelo regime frouxo por
        // ate 64 amostras. E o mesmo argumento do piso de roughness, e o motivo de o setter da
        // classe limpar a tabela.
        Invalidate(Dom::RayVisibility);
    }
    u32 FRenderSettings::GetRadianceCacheMinSampleCount() const {
        return R.RadianceCache.GetMinSampleCount();
    }

    void FRenderSettings::SetRadianceCacheStatsEnabled(bool _V) {
        if (_V == R.RadianceCache.GetStatsEnabled()) return;
        // NAO invalida historico: o conteudo ja acumulado continua valendo, e derrubar o cache a
        // cada vez que se liga o contador tornaria a instrumentacao inutil justamente para quem
        // quer olhar um cache quente.
        //
        // Mas TAMBEM nao e neutra, e isso foi medido: os atomicos disputados por wave mudam o
        // escalonamento e, com ele, quais threads vencem as insercoes — 73.218 celulas contra
        // 73.195, PSNR de 48 dB entre os regimes. Trocar de regime no meio de um aquecimento
        // produziria uma captura meio instrumentada e meio nao, que nao pertence a nenhuma das
        // duas series. Sem guarda de setup: o capturador nao mexe neste knob.
        R.Capture.Cancel("a instrumentacao do cache foi alternada durante o aquecimento");
        R.RadianceCache.SetStatsEnabled(_V);
    }
    void FRenderSettings::SetRadianceCacheStatsDetailEnabled(bool _V) {
        if (_V == R.RadianceCache.GetStatsDetailEnabled()) return;
        // Mesmo tratamento do knob acima, e pelo mesmo motivo levado um passo adiante: o detalhe
        // acrescenta atomicos AO PRODUTOR tambem (a telemetria de insercao), onde eles mudam quem
        // vence a corrida do CAS. E o regime que mais mexe no escalonamento, entao alterna-lo no
        // meio de um aquecimento produz uma captura que nao pertence a serie nenhuma.
        R.Capture.Cancel("o detalhe da instrumentacao do cache foi alternado durante o aquecimento");
        R.RadianceCache.SetStatsDetailEnabled(_V);
    }
    bool FRenderSettings::GetRadianceCacheStatsDetailEnabled() const {
        return R.RadianceCache.GetStatsDetailEnabled();
    }

    void FRenderSettings::SetGISourceDebug(bool _V) {
        if (_V == R.ReSTIRGI.GetSourceDebug()) return;
        R.ReSTIRGI.SetSourceDebug(_V);
        // O registro dos alvos e reconstruido do ZERO e so em eventos de setup — ele nao roda por
        // frame. Como o alvo da fonte so se registra com o toggle ligado (senao a UI ofereceria
        // uma textura que ninguem esta enchendo), a troca do toggle E um desses eventos.
        R.RegisterDebugTargets();
        // Nao invalida historico nenhum: escrever falsa-cor num alvo proprio nao muda o que
        // qualquer acumulador guarda. O `DebugParams.y` decide a escrita por frame.
    }
    bool FRenderSettings::GetGISourceDebug() const { return R.ReSTIRGI.GetSourceDebug(); }

    void FRenderSettings::SetRadianceCacheStatsSourceEnabled(bool _V) {
        if (_V == R.RadianceCache.GetStatsSourceEnabled()) return;
        // Terceira vez que este bloco se repete, e a repeticao e o ponto: cada regime de medicao
        // cancela captura em curso porque uma sessao que troca de regime no meio nao pertence a
        // serie nenhuma. Este acrescenta UM atomico por hit sombreado — nao muda o conteudo do
        // cache (os traces de render nao inserem desde a Fase 3), mas muda custo, contencao no UAV
        // de estatisticas e overlap com o updater. "Nao muda a imagem" nao e "e comparavel".
        R.Capture.Cancel("a telemetria de fonte foi alternada durante o aquecimento");
        R.RadianceCache.SetStatsSourceEnabled(_V);
    }
    bool FRenderSettings::GetRadianceCacheStatsSourceEnabled() const {
        return R.RadianceCache.GetStatsSourceEnabled();
    }

    bool FRenderSettings::GetRadianceCacheStatsEnabled() const {
        return R.RadianceCache.GetStatsEnabled();
    }

    void FRenderSettings::SetRadianceCacheCellSize(f32 _V) {
        if (_V == R.RadianceCache.GetBaseCellSize()) return;
        R.RadianceCache.SetBaseCellSize(_V);
        // A chave muda: o conteudo guardado passa a estar enderecado errado. O executor acima
        // transforma o bit RadianceCache do dominio em ResetOnce.
        Invalidate(Dom::RayVisibility);
    }
    f32 FRenderSettings::GetRadianceCacheCellSize() const {
        return R.RadianceCache.GetBaseCellSize();
    }

    void FRenderSettings::SetRadianceCacheLodDistance(f32 _V) {
        if (_V == R.RadianceCache.GetLodDistance()) return;
        R.RadianceCache.SetLodDistance(_V);
        Invalidate(Dom::RayVisibility);
    }
    f32 FRenderSettings::GetRadianceCacheLodDistance() const {
        return R.RadianceCache.GetLodDistance();
    }

    void FRenderSettings::SetRadianceCacheDebugMode(u32 _V) {
        if (_V >= static_cast<u32>(ERadianceCacheDebugMode::Count)) return;
        R.RadianceCache.SetDebugMode(static_cast<ERadianceCacheDebugMode>(_V));
    }
    u32 FRenderSettings::GetRadianceCacheDebugMode() const {
        return static_cast<u32>(R.RadianceCache.GetDebugMode());
    }

    // O botao de limpar a tabela, pelo FUNIL e nao pelo ResetOnce cru. Chamar o metodo da classe
    // direto era a ultima porta lateral do lifecycle: o reset FECHA a consulta (o ResetPending
    // zera as flags dos consumidores), e quem estava segurando radiancia vinda do cache —
    // reservoirs do ReSTIR GI, atlas do DDGI — continuava a usa-la. Pelo mesmo motivo o botao
    // tinha de cancelar captura em curso e nao cancelava.
    //
    // `RayVisibility` inteiro, com o cache dentro: aqui a tabela DEVE morrer, e e o proprio funil
    // que chama o ResetOnce. E o oposto do toggle de leitura, que usa a mascara sem o cache.
    void FRenderSettings::ResetRadianceCache() { Invalidate(Dom::RayVisibility); }

    const FRadianceCacheStats& FRenderSettings::RadianceCacheStats() const {
        return R.RadianceCache.Stats();
    }
    const FRadianceCacheStatsMeta& FRenderSettings::RadianceCacheStatsMeta() const {
        return R.RadianceCache.StatsMetaCPU();
    }
    FRadianceCacheSnapshot FRenderSettings::RadianceCacheSnapshot() const {
        return R.RadianceCache.Snapshot();
    }
    u64 FRenderSettings::RadianceCacheBytes() const { return R.RadianceCache.MemoryBytes(); }
    u32 FRenderSettings::RadianceCacheCapacity() const { return R.RadianceCache.Capacity(); }

    void FRenderSettings::SetUseReSTIRGI(bool _V) {
        if (_V == R.UseReSTIRGI) return;
        // Os reservoirs so na borda de SUBIDA: guardam radiancia do frame em que o toggle
        // desligou (sol/emissivos/DDGI antigos sobreviveriam por tempo indeterminado). Na
        // descida eles param de ser lidos, entao limpar seria custo puro.
        if (_V) Invalidate(EHistoryTarget::ReSTIRGI);
        R.UseReSTIRGI = _V;
        // Nas DUAS bordas, espelhando o SetUseReSTIRDI: ligar ou desligar o ReSTIR GI muda
        // drasticamente o sinal que o NRD acumula, que o RR reconstroi e que o TAA integra.
        // Antes daqui so o NrdIndirect caia — o RR seguia com historico neural de um sinal
        // que deixou de existir.
        Invalidate(Dom::ScreenResolve);
        R.ReconcileNrdAllocation(); // a instancia indireta do NRD so existe com o ReSTIR GI ligado
    }
    bool FRenderSettings::GetUseReSTIRGI() const { return R.UseReSTIRGI; }

    void FRenderSettings::SetUseReSTIRDI(bool _V) {
        if (_V == R.UseReSTIRDI) return;
        // Os reservoirs do DI so caem na borda de SUBIDA; o resto cai sempre.
        if (_V) Invalidate(EHistoryTarget::ReSTIRDI);
        R.UseReSTIRDI = _V;
        Invalidate(EHistoryTarget::NrdDirect | Dom::Resolve);
        R.ReconcileNrdAllocation(); // idem p/ a instancia direta
    }
    bool FRenderSettings::GetUseReSTIRDI() const { return R.UseReSTIRDI; }
    bool FRenderSettings::ReSTIRDIActive() const { return R.UseReSTIRDI && R.ReSTIRDI.IsReady(); }

    const FRayEpsilonProfile& FRenderSettings::GetRayEpsilons() const { return R.RayEps; }

    void FRenderSettings::SetRayEpsilons(const FRayEpsilonProfile& _P) {
        R.RayEps = _P;
        // Mudar geometria de raio invalida TUDO que acumula: os reservoirs guardam Lo e x2
        // medidos com os epsilons antigos, o shadow ray da direta usa o mesmo perfil, e o NRD
        // acumula sobre uns e outros. Sem o clear o A/B compara estado misturado.
        Invalidate(Dom::RayGeometry);
    }

    // Os knobs de amostragem do DDGI entram hoje no ShadeSurfaceHit — o bias desde que o 2o
    // bounce passou a usar o gather completo, o fade de borda desde que ele tambem parou de
    // extrapolar la. Por isso mudam o que fica GRAVADO, nao so o que aparece na tela.
    //
    // O preco: o reset faz a tomada passar de novo pela realizacao aleatoria de um frame (as
    // direcoes giram com o frameIndex), entao para medir A/B e preciso esperar convergir. Sem
    // o reset seria pior: estados misturados.
    void FRenderSettings::OnGIHitSamplingChanged() { Invalidate(Dom::RayVisibility); }

    f32 FRenderSettings::GetGISurfaceBiasMax() const { return R.DDGI.GetSurfaceBiasMax(); }
    void FRenderSettings::SetGISurfaceBiasMax(f32 _V) {
        if (_V == R.DDGI.GetSurfaceBiasMax()) return;
        R.DDGI.SetSurfaceBiasMax(_V);
        OnGIHitSamplingChanged();
    }

    f32 FRenderSettings::GetGISurfaceBiasScale() const { return R.DDGI.GetSurfaceBiasScale(); }
    void FRenderSettings::SetGISurfaceBiasScale(f32 _V) {
        if (_V == R.DDGI.GetSurfaceBiasScale()) return;
        R.DDGI.SetSurfaceBiasScale(_V);
        OnGIHitSamplingChanged();
    }

    f32 FRenderSettings::GetGIVolumeFadeProbes() const { return R.DDGI.GetVolumeFadeProbes(); }
    void FRenderSettings::SetGIVolumeFadeProbes(f32 _V) {
        if (_V == R.DDGI.GetVolumeFadeProbes()) return;
        R.DDGI.SetVolumeFadeProbes(_V);
        OnGIHitSamplingChanged();
    }

    bool FRenderSettings::GetGIAdaptiveHysteresis() const {
        return R.DDGI.GetAdaptiveHysteresis();
    }
    void FRenderSettings::SetGIAdaptiveHysteresis(bool _V) {
        if (_V == R.DDGI.GetAdaptiveHysteresis()) return;
        R.DDGI.SetAdaptiveHysteresis(_V);
        // GIAccumulation e nao RayVisibility: nao mudou o que o raio ve, mudou a REGRA com que
        // o atlas acumula. A mascara e a mesma hoje; o nome e o que decide o dia em que um
        // alvo novo entrar em so um dos dois (ver HistoryDomain.h).
        //
        // Limpar e obrigatorio para o A/B: sem isso o lado "ligado" comeca com sondas
        // convergidas pelo lado "desligado" e a comparacao mede estado misturado.
        Invalidate(Dom::GIAccumulation);
    }

    u32 FRenderSettings::GetGICascadeCount() const { return R.DDGI.GetDesiredCascades(); }
    void FRenderSettings::SetGICascadeCount(u32 _V) {
        if (_V == R.DDGI.GetDesiredCascades()) return;
        R.DDGI.SetDesiredCascades(_V);
        // Recria o volume AQUI, e nao no proximo load: sem isso o knob pareceria inerte ate
        // alguem recarregar a cena, que e a pior forma de um botao mentir — ele aceita o clique
        // e nao faz nada visivel.
        R.RebuildGIVolume();
        // Tudo que se apoiava no volume antigo (sondas, indices, historicos de tela que
        // acumularam sobre ele) descreve uma grade que nao existe mais.
        Invalidate(Dom::GIAccumulation);
    }

    bool FRenderSettings::GetGIAdaptiveRays() const { return R.DDGI.GetAdaptiveRays(); }
    void FRenderSettings::SetGIAdaptiveRays(bool _V) {
        if (_V == R.DDGI.GetAdaptiveRays()) return;
        R.DDGI.SetAdaptiveRays(_V); // agenda a reclassificacao (ver FDDGI::TriggerReclassify)
        // Mesmo dominio do detector de histerese, e pela mesma razao: nao mudou o que o raio
        // enxerga, mudou o ESTIMADOR que alimenta o atlas (quantas amostras cada sonda tem por
        // frame). Sem o clear, o lado ligado do A/B comeca com sondas convergidas a 64 raios e a
        // comparacao mede estado misturado em vez da diferenca de variancia.
        Invalidate(Dom::GIAccumulation);
    }

    bool FRenderSettings::GetGIMeasureTerminatorOff() const { return R.GIMeasureTerminatorOff; }
    void FRenderSettings::SetGIMeasureTerminatorOff(bool _V) {
        if (_V == R.GIMeasureTerminatorOff) return;
        R.GIMeasureTerminatorOff = _V;
        // RayVisibility no sentido literal do dominio: mudou o que o raio ENXERGA no hit. Sem o
        // clear, o lado "sem DDGI" da medicao comecaria com reservoirs e atlas cheios de energia
        // que veio justamente do DDGI — o A/B mediria a propria memoria do sistema desligado.
        Invalidate(Dom::RayVisibility);
    }

    bool FRenderSettings::GetGIBackfacePolicy() const { return R.ReSTIRGI.GetBackfacePolicy(); }
    void FRenderSettings::SetGIBackfacePolicy(bool _V) {
        // Passa por aqui, e nao direto no FReSTIRGI, porque o clear dos reservoirs sozinho nao
        // basta: o NRD e o RR acumulam SOBRE eles e o TAA sobre o resultado, entao um A/B feito
        // so com o clear compararia um estado misturado.
        //
        // ERA ScreenResolve, com a justificativa "DDGI e reflexoes ficam de fora de proposito — a
        // politica so toca no gather". Isso deixou de ser verdade quando o passe de update do
        // radiance cache passou a ler o MESMO toggle: agora a politica decide o que um raio VE
        // tambem no produtor de um cache de MUNDO, e as celulas treinadas sob a regra anterior
        // sobreviveriam ate 64 frames misturando-se as novas — mais tempo do que qualquer A/B
        // levaria para ser feito, e sem nada na tela denunciando.
        //
        // RayVisibility e o dominio pelo MOTIVO, que e a politica de nomes deste arquivo: mudou o
        // que o raio enxerga. Ele ja carrega o RadianceCache, e leva junto DDGI e reflexoes — que
        // nao aplicam a politica, mas CONSOMEM o cache e por isso herdaram o estado velho.
        if (_V == R.ReSTIRGI.GetBackfacePolicy()) return;
        R.ReSTIRGI.SetBackfacePolicy(_V); // ja marca NeedsClear nos reservoirs
        Invalidate(Dom::RayVisibility);
    }

    // O FDDGI e a fonte da verdade na leitura, como no toggle antigo do editor.
    bool FRenderSettings::GetGIFoliageShadows() const { return R.DDGI.GetFoliageShadows(); }
    void FRenderSettings::SetGIFoliageShadows(bool _V) {
        if (_V == R.DDGI.GetFoliageShadows()) return;
        // Fan-out p/ os 3 consumidores do HitShading. Era o ViewportWidget que sabia disso.
        R.DDGI.SetFoliageShadows(_V);
        R.ReSTIRGI.SetFoliageShadows(_V);   // este ja marca NeedsClear por conta propria
        R.Reflections.SetFoliageShadows(_V);
        // O knob entra no ShadeSurfaceHit pelo ShadowRayMask: muda o Lo GRAVADO no reservoir e
        // o valor devolvido as sondas. Isso e, exatamente, a definicao do OnGIHitSamplingChanged
        // — entao reusa aquela invalidacao em vez de escrever uma lista nova. Inclui o
        // VolumetricFog (acumula inscatter que le o DDGI) e o RepeatDebugProbePoint (senao o
        // painel de diagnostico exibe os numeros do estado anterior durante o A/B).
        //
        // Antes daqui, dos tres so o FReSTIRGI se invalidava; FDDGI e FReflections eram
        // atribuicao pura, e nada derrubava NRD/RR/TAA. Com Hysteresis 0,99 no DDGI, o atlas
        // sombreado com a mascara antiga sobrevivia por centenas de updates.
        OnGIHitSamplingChanged();
    }

    bool FRenderSettings::GetGIVisibility() const { return R.ReSTIRGI.GetVisibility(); }
    void FRenderSettings::SetGIVisibility(bool _V) {
        if (_V == R.ReSTIRGI.GetVisibility()) return;
        R.ReSTIRGI.SetVisibility(_V);
        // O ReSTIRGI.h esta certo ao dizer que isto NAO toca o reservoir: o Visibility so atua
        // no Pass B e no resolve final, e o espacial nao realimenta o temporal. Por isso o
        // ReSTIRGI fica de fora — limpar reservoir aqui seria custo puro.
        //
        // Mas e modo PERSISTENTE que muda a radiancia resolvida, e o NRD acumula sobre ela (e o
        // RR/TAA sobre o resultado). Pelo mesmo criterio do SetGIBackfacePolicy — "o clear dos
        // reservoirs sozinho nao basta" — o que acumula DEPOIS do resolve precisa cair.
        Invalidate(Dom::ScreenResolve);
    }

    // === Reflexoes ======================================================================

    void FRenderSettings::SetUseReflections(bool _V) {
        if (_V == R.UseReflections) return;
        R.UseReflections = _V;
        // Com reflexoes off o spec acumula sinal zero — religar sem reset arrastaria esse
        // historico vazio pro especular real. O simetrico tambem vale: desligar deixa reflexao
        // de verdade no historico, que agora vai se misturar a zero. Por isso nas duas bordas, e
        // pelo mesmo dominio do SetReflectionsCullBackface — antes daqui faltavam o History[]
        // proprio do FReflections (com paridade de frame possivelmente velha), o RR e o TAA.
        Invalidate(Dom::Specular);
    }
    bool FRenderSettings::GetUseReflections() const { return R.UseReflections; }

    bool FRenderSettings::GetReflectionsCullBackface() const {
        return R.Reflections.GetBackfaceCull();
    }
    void FRenderSettings::SetReflectionsCullBackface(bool _V) {
        // Culling nos raios de REFLEXAO. Nao precisa de rebuild da TLAS — e parametro de
        // shader. Invalida so o que acumula reflexo: o ReSTIR e o DDGI nao veem esta chave.
        if (_V == R.Reflections.GetBackfaceCull()) return;
        R.Reflections.SetBackfaceCull(_V);
        Invalidate(Dom::Specular);
    }

    // === Oclusao de ambiente ============================================================

    void FRenderSettings::SetUseAO(bool _V) { R.UseAO = _V; }
    bool FRenderSettings::GetUseAO() const  { return R.UseAO; }
    // As duas cadeias (full e meia-res) ficam alocadas; o toggle e por frame.
    void FRenderSettings::SetAOHalfRes(bool _V) { R.AO.SetHalfRes(_V); }
    bool FRenderSettings::GetAOHalfRes() const  { return R.AO.GetHalfRes(); }

    // === Sombras do sol =================================================================

    void FRenderSettings::SetUseSunShadows(bool _Use) { R.UseSunShadows = _Use; }
    bool FRenderSettings::GetUseSunShadows() const    { return R.UseSunShadows; }

    void FRenderSettings::SetShadowMaxDistance(f32 _V) { R.SunShadows.SetMaxDistance(_V); }
    f32  FRenderSettings::GetShadowMaxDistance() const { return R.SunShadows.GetMaxDistance(); }
    void FRenderSettings::SetShadowDepthBias(f32 _T)   { R.SunShadows.SetDepthBias(_T); }
    f32  FRenderSettings::GetShadowDepthBias() const   { return R.SunShadows.GetDepthBias(); }
    void FRenderSettings::SetShadowNormalOffset(f32 _T) { R.SunShadows.SetNormalOffset(_T); }
    f32  FRenderSettings::GetShadowNormalOffset() const { return R.SunShadows.GetNormalOffset(); }
    void FRenderSettings::SetShadowMinCasterTexels(f32 _V) {
        R.SunShadows.SetMinCasterTexels(_V);
    }
    f32 FRenderSettings::GetShadowMinCasterTexels() const {
        return R.SunShadows.GetMinCasterTexels();
    }
    void FRenderSettings::SetShadowCascadeCache(bool _V) { R.SunShadows.SetCascadeCache(_V); }
    bool FRenderSettings::GetShadowCascadeCache() const  { return R.SunShadows.GetCascadeCache(); }
    void FRenderSettings::SetShadowDebugCascades(bool _V) { R.SunShadows.SetDebugCascades(_V); }
    bool FRenderSettings::GetShadowDebugCascades() const {
        return R.SunShadows.GetDebugCascades();
    }
    void FRenderSettings::SetShadowCascadeBiasScale(u32 _C, f32 _S) {
        R.SunShadows.SetCascadeBiasScale(_C, _S);
    }
    f32 FRenderSettings::GetShadowCascadeBiasScale(u32 _C) const {
        return R.SunShadows.GetCascadeBiasScale(_C);
    }
    void FRenderSettings::SetSunAngularSize(f32 _Deg) { R.SunShadows.SetSunAngularSize(_Deg); }
    f32  FRenderSettings::GetSunAngularSize() const   { return R.SunShadows.GetSunAngularSize(); }

    // === Sol e ceu ======================================================================

    void FRenderSettings::SetSunDirection(const Vec3& _Dir) { R.SetSunDirection(_Dir); }
    Vec3 FRenderSettings::GetSunDirection() const           { return R.SunDir; }
    void FRenderSettings::SetSunColor(const Vec3& _Color)   { R.SunColorRGB = _Color; }
    Vec3 FRenderSettings::GetSunColor() const               { return R.SunColorRGB; }
    void FRenderSettings::SetSunAzimuthElevation(f32 _AzimuthDeg, f32 _ElevationDeg) {
        R.SetSunAzimuthElevation(_AzimuthDeg, _ElevationDeg);
    }

    void FRenderSettings::SetPerPixelAtmoTransmittance(bool _Use) {
        R.UsePerPixelAtmoTransmittance = _Use;
    }
    bool FRenderSettings::GetPerPixelAtmoTransmittance() const {
        return R.UsePerPixelAtmoTransmittance;
    }
    void FRenderSettings::SetSkyAmbientSH(bool _Use) { R.UseSkyAmbientSH = _Use; }
    bool FRenderSettings::GetSkyAmbientSH() const    { return R.UseSkyAmbientSH; }

    // === Fog e sun shafts ===============================================================

    void FRenderSettings::SetUseVolumetricFog(bool _Use) { R.UseVolumetricFog = _Use; }
    bool FRenderSettings::GetUseVolumetricFog() const    { return R.UseVolumetricFog; }

    void FRenderSettings::SetFogHeightSkyContribution(f32 _V) {
        R.Fog.SetHeightFogSkyContribution(_V);
    }
    f32 FRenderSettings::GetFogHeightSkyContribution() const {
        return R.Fog.GetHeightFogSkyContribution();
    }

    void FRenderSettings::SetVolFogMaxDistance(f32 _V) { R.VolumetricFog.SetMaxDistance(_V); }
    f32  FRenderSettings::GetVolFogMaxDistance() const { return R.VolumetricFog.GetMaxDistance(); }
    void FRenderSettings::SetVolFogTemporal(bool _V)   { R.VolumetricFog.SetTemporal(_V); }
    bool FRenderSettings::GetVolFogTemporal() const    { return R.VolumetricFog.GetTemporal(); }
    void FRenderSettings::SetVolFogConservativeDepth(bool _V) {
        R.VolumetricFog.SetConservativeDepth(_V);
    }
    bool FRenderSettings::GetVolFogConservativeDepth() const {
        return R.VolumetricFog.GetConservativeDepth();
    }
    void FRenderSettings::SetVolFogPhaseG(f32 _V) { R.VolumetricFog.SetPhaseG(_V); }
    f32  FRenderSettings::GetVolFogPhaseG() const { return R.VolumetricFog.GetPhaseG(); }
    void FRenderSettings::SetVolFogExtinctionScale(f32 _V) {
        R.VolumetricFog.SetExtinctionScale(_V);
    }
    f32 FRenderSettings::GetVolFogExtinctionScale() const {
        return R.VolumetricFog.GetExtinctionScale();
    }
    void FRenderSettings::SetVolFogAmbientIntensity(f32 _V) {
        R.VolumetricFog.SetAmbientIntensity(_V);
    }
    f32 FRenderSettings::GetVolFogAmbientIntensity() const {
        return R.VolumetricFog.GetAmbientIntensity();
    }
    void FRenderSettings::SetVolFogLightsIntensity(f32 _V) {
        R.VolumetricFog.SetLightsIntensity(_V);
    }
    f32 FRenderSettings::GetVolFogLightsIntensity() const {
        return R.VolumetricFog.GetLightsIntensity();
    }

    void FRenderSettings::SetUseSunShafts(bool _Use) { R.UseSunShafts = _Use; }
    bool FRenderSettings::GetUseSunShafts() const    { return R.UseSunShafts; }

    void FRenderSettings::SetShaftsIntensity(f32 _V) { R.SunShafts.SetVolIntensity(_V); }
    f32  FRenderSettings::GetShaftsIntensity() const { return R.SunShafts.GetVolIntensity(); }
    void FRenderSettings::SetShaftsPhaseG(f32 _V)    { R.SunShafts.SetVolPhaseG(_V); }
    f32  FRenderSettings::GetShaftsPhaseG() const    { return R.SunShafts.GetVolPhaseG(); }
    void FRenderSettings::SetShaftsSteps(f32 _V)     { R.SunShafts.SetVolSteps(_V); }
    f32  FRenderSettings::GetShaftsSteps() const     { return R.SunShafts.GetVolSteps(); }
    void FRenderSettings::SetShaftsMaxDist(f32 _V)   { R.SunShafts.SetVolMaxDist(_V); }
    f32  FRenderSettings::GetShaftsMaxDist() const   { return R.SunShafts.GetVolMaxDist(); }
    void FRenderSettings::SetShaftsDust(f32 _V)      { R.SunShafts.SetVolDust(_V); }
    f32  FRenderSettings::GetShaftsDust() const      { return R.SunShafts.GetVolDust(); }
    void FRenderSettings::SetShaftsTemporal(bool _V) { R.SunShafts.SetVolTemporal(_V); }
    bool FRenderSettings::GetShaftsTemporal() const  { return R.SunShafts.GetVolTemporal(); }

    // === Nuvens volumetricas ============================================================

    void FRenderSettings::SetUseClouds(bool _Use) {
        if (_Use && !R.UseClouds) Invalidate(EHistoryTarget::VolumetricClouds);
        R.UseClouds = _Use;
    }
    bool FRenderSettings::GetUseClouds() const { return R.UseClouds; }

    void FRenderSettings::SetCloudsHalfRes(bool _HalfRes) { R.SetCloudsHalfRes(_HalfRes); }
    bool FRenderSettings::GetCloudsHalfRes() const { return R.VolumetricClouds.GetHalfRes(); }
    void FRenderSettings::SetCloudsTemporal(bool _V) { R.VolumetricClouds.SetUseTemporal(_V); }
    bool FRenderSettings::GetCloudsTemporal() const  { return R.VolumetricClouds.GetUseTemporal(); }

    void FRenderSettings::SetCloudCoverage(f32 _V) { R.VolumetricClouds.SetCoverage(_V); }
    f32  FRenderSettings::GetCloudCoverage() const { return R.VolumetricClouds.GetCoverage(); }
    void FRenderSettings::SetCloudDensityScale(f32 _V) {
        R.VolumetricClouds.SetDensityScale(_V);
    }
    f32 FRenderSettings::GetCloudDensityScale() const {
        return R.VolumetricClouds.GetDensityScale();
    }
    void FRenderSettings::SetCloudErosion(f32 _V) { R.VolumetricClouds.SetErosion(_V); }
    f32  FRenderSettings::GetCloudErosion() const { return R.VolumetricClouds.GetErosion(); }
    void FRenderSettings::SetCloudPhaseG(f32 _V)  { R.VolumetricClouds.SetPhaseG(_V); }
    f32  FRenderSettings::GetCloudPhaseG() const  { return R.VolumetricClouds.GetPhaseG(); }
    void FRenderSettings::SetCloudPowder(f32 _V)  { R.VolumetricClouds.SetPowder(_V); }
    f32  FRenderSettings::GetCloudPowder() const  { return R.VolumetricClouds.GetPowder(); }
    void FRenderSettings::SetCloudWindSpeed(f32 _V) { R.VolumetricClouds.SetWindSpeed(_V); }
    f32  FRenderSettings::GetCloudWindSpeed() const { return R.VolumetricClouds.GetWindSpeed(); }
    void FRenderSettings::SetCloudAmbientScale(f32 _V) {
        R.VolumetricClouds.SetAmbientScale(_V);
    }
    f32 FRenderSettings::GetCloudAmbientScale() const {
        return R.VolumetricClouds.GetAmbientScale();
    }
    void FRenderSettings::SetCloudTypeBias(f32 _V) { R.VolumetricClouds.SetCloudTypeBias(_V); }
    f32  FRenderSettings::GetCloudTypeBias() const { return R.VolumetricClouds.GetCloudTypeBias(); }
    void FRenderSettings::SetCloudPeakVariation(f32 _V) {
        R.VolumetricClouds.SetPeakVariation(_V);
    }
    f32 FRenderSettings::GetCloudPeakVariation() const {
        return R.VolumetricClouds.GetPeakVariation();
    }
    void FRenderSettings::SetCloudMarchSteps(f32 _V) { R.VolumetricClouds.SetMarchSteps(_V); }
    f32  FRenderSettings::GetCloudMarchSteps() const { return R.VolumetricClouds.GetMarchSteps(); }
    void FRenderSettings::SetCloudShadowsEnabled(bool _V) {
        R.VolumetricClouds.SetShadowsEnabled(_V);
    }
    bool FRenderSettings::GetCloudShadowsEnabled() const {
        return R.VolumetricClouds.GetShadowsEnabled();
    }
    void FRenderSettings::SetCloudShadowStrength(f32 _V) {
        R.VolumetricClouds.SetShadowStrength(_V);
    }
    f32 FRenderSettings::GetCloudShadowStrength() const {
        return R.VolumetricClouds.GetShadowStrength();
    }
    void FRenderSettings::SetCloudAltitude(f32 _BottomKm, f32 _ThicknessKm) {
        R.VolumetricClouds.SetAltitude(_BottomKm, _ThicknessKm);
    }
    f32 FRenderSettings::GetCloudBottomAltitude() const {
        return R.VolumetricClouds.GetBottomAltitude();
    }
    f32 FRenderSettings::GetCloudThickness() const { return R.VolumetricClouds.GetThickness(); }

    void FRenderSettings::SetCloudWeatherSeed(u32 _Seed)  { R.SetCloudWeatherSeed(_Seed); }
    u32  FRenderSettings::GetCloudWeatherSeed() const     { return R.CloudNoise.GetSeed(); }
    void FRenderSettings::SetCloudWeatherCells(u32 _Mult) { R.SetCloudWeatherCells(_Mult); }
    u32  FRenderSettings::GetCloudWeatherCells() const    { return R.CloudNoise.GetCellMult(); }
    bool FRenderSettings::LoadCloudWeatherTexture(const std::wstring& _Path) {
        return R.LoadCloudWeatherTexture(_Path);
    }
    void FRenderSettings::ClearCloudWeatherTexture() { R.ClearCloudWeatherTexture(); }
    bool FRenderSettings::CloudWeatherAuthored() const {
        return R.CloudNoise.HasWeatherOverride();
    }

    // === Agua ===========================================================================

    // O espectro da FFT recomeca, entao o historico de displacement/foam das cascatas cai. Passa
    // pelo funil e nao por um laco no setter: o FOceanFFT ja se reseta sozinho nos proprios
    // setters de espectro, mas aquilo e invariante INTERNA da classe e ninguem de fora fica
    // sabendo — nem uma captura em aquecimento, que precisa ser cancelada quando o mundo muda
    // debaixo dela. Declarar aqui e o que poe o oceano no grafo.
    void FRenderSettings::SetUseWater(bool _Use) {
        if (_Use == R.UseWater) return;
        R.SetUseWater(_Use);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    bool FRenderSettings::GetUseWater() const    { return R.UseWater; }

    bool FRenderSettings::GetWaterGuideInvisible() const { return R.Water.GetGuideInvisible(); }
    void FRenderSettings::SetWaterGuideInvisible(bool _V) {
        if (_V == R.Water.GetGuideInvisible()) return;
        R.Water.SetGuideInvisible(_V);
        Invalidate(Dom::Guides); // muda os guides do RR: historico neural velho mente
    }

    // Os seis abaixo mudam o ESPECTRO: o FOceanFFT marca H0Dirty e derruba o historico temporal
    // por conta propria. O Invalidate aqui nao existe para repetir esse reset — existe para
    // DECLARA-LO, que e o que o funil precisa para cancelar uma captura em aquecimento. Sem isso o
    // mundo mudava no meio da medicao e a captura saia como se nada tivesse acontecido.
    void FRenderSettings::SetWaterWindSpeed(f32 _V) {
        if (_V == R.Water.GetWindSpeed()) return;
        R.Water.SetWindSpeed(_V);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    f32  FRenderSettings::GetWaterWindSpeed() const { return R.Water.GetWindSpeed(); }
    void FRenderSettings::SetWaterWindDirection(f32 _Rad) {
        if (_Rad == R.Water.GetWindDirection()) return;
        R.Water.SetWindDirection(_Rad);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    f32  FRenderSettings::GetWaterWindDirection() const   { return R.Water.GetWindDirection(); }
    void FRenderSettings::SetWaterWavesAmount(f32 _V) {
        if (_V == R.Water.GetWavesAmount()) return;
        R.Water.SetWavesAmount(_V);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    f32  FRenderSettings::GetWaterWavesAmount() const { return R.Water.GetWavesAmount(); }
    void FRenderSettings::SetWaterSwell(f32 _V) {
        if (_V == R.Water.GetSwell()) return;
        R.Water.SetSwell(_V);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    f32  FRenderSettings::GetWaterSwell() const { return R.Water.GetSwell(); }
    void FRenderSettings::SetWaterSpectrumFetch(f32 _Km) {
        if (_Km == R.Water.GetSpectrumFetch()) return;
        R.Water.SetSpectrumFetch(_Km);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    f32  FRenderSettings::GetWaterSpectrumFetch() const  { return R.Water.GetSpectrumFetch(); }
    void FRenderSettings::SetWaterOceanDepth(f32 _M) {
        if (_M == R.Water.GetOceanDepth()) return;
        R.Water.SetOceanDepth(_M);
        Invalidate(EHistoryTarget::OceanTemporal);
    }
    f32  FRenderSettings::GetWaterOceanDepth() const { return R.Water.GetOceanDepth(); }
    void FRenderSettings::SetWaterFFTDisplacementScale(f32 _V) {
        R.Water.SetFFTDisplacementScale(_V);
    }
    f32 FRenderSettings::GetWaterFFTDisplacementScale() const {
        return R.Water.GetFFTDisplacementScale();
    }
    void FRenderSettings::SetWaterFFTChoppyScale(f32 _V) { R.Water.SetFFTChoppyScale(_V); }
    f32  FRenderSettings::GetWaterFFTChoppyScale() const { return R.Water.GetFFTChoppyScale(); }

    // === Terreno ========================================================================

    void FRenderSettings::SetUseTerrain(bool _Use) { R.UseTerrain = _Use; }
    bool FRenderSettings::GetUseTerrain() const    { return R.UseTerrain; }

    // === Clima ==========================================================================

    // RainAmount e DriveSky sao os dois unicos knobs de clima que entram em sinal GRAVADO. Os
    // dois alimentam o RainSky do RenderFrame, e dele saem tres coisas que voltam para
    // historico de vida longa:
    //   RainSkyDim -> DDGI.SetSkyIntensity, reflexoes e ReSTIR GI (a radiancia do ceu vista
    //                 por um raio);
    //   RainKeyDim -> EffectiveSunColor/MoonLightCol, que viram o SunColor dos tres passes
    //                 de trace (a cor da luz-chave no hit);
    //   RainAmbDim -> ambiente hemisferico e SH.
    // O DDGI tem Hysteresis 0,99: sem o reset, 99% do atlas de tempo seco sobrevive POR
    // UPDATE, e a chuva fica calibrada contra energia que o usuario ja mandou remover. Os
    // reservoirs do ReSTIR sao piores ainda — com ValidateInterval = 0 nao ha re-shade que
    // corrija o Lo gravado.
    //
    // A politica NAO e nova: e a mesma de NotifyIndirectLightingChanged, que ja existe para
    // "a energia que alimenta o indireto mudou" (hoje FLight::RTWeight). Chuva mexendo no ceu
    // e na luz-chave e o mesmo caso, entao reusa em vez de virar a decima quinta lista escrita
    // a mao. Pela versao COALESCIDA porque RainAmount e slider: sem isso, cada tick do arraste
    // custaria um reset completo e o GI ficaria em reset permanente durante o gesto.
    void FRenderSettings::SetRainAmount(f32 _V) {
        if (_V == R.Weather.GetRainAmount()) return;
        // Com DriveSky off, RainSky e 0 nos dois estados: o knob so mexe em wetness (G-buffer,
        // screen-space) e na cortina. Nada de vida longa cai, entao nao invalide.
        const bool DrivesSky = R.Weather.GetDriveSky();
        R.Weather.SetRainAmount(_V);
        if (DrivesSky) MarkIndirectLightingDirty();
    }
    f32 FRenderSettings::GetRainAmount() const { return R.Weather.GetRainAmount(); }

    void FRenderSettings::SetRainDriveSky(bool _V) {
        if (_V == R.Weather.GetDriveSky()) return;
        R.Weather.SetDriveSky(_V);
        // Sem chuva, RainSky e 0 dos dois lados do toggle — nada mudou no ceu.
        if (R.Weather.Raining()) MarkIndirectLightingDirty();
    }
    bool FRenderSettings::GetRainDriveSky() const { return R.Weather.GetDriveSky(); }
    // Knobs de WETNESS. O RainWetness.ps.hlsl le copias de GBufferA/B e REESCREVE os originais:
    // BaseColor no SV_Target0 e normal + roughness no SV_Target1. Ou seja, mexem exatamente nos
    // guides que o Ray Reconstruction consome e no G-buffer de onde as reflexoes tiram a
    // rugosidade que escolhe o lobo especular. Quem acumulou com a rugosidade antiga precisa
    // cair — mesmo raciocinio do SetWaterGuideInvisible, mais o especular (aquele mexe em
    // depth/velocity, este mexe em roughness).
    //
    // Tier B, nao C: wetness e screen-space, aplicada DEPOIS do geometry pass, e os hits de RT
    // leem o material do InstanceGeo. Os reservoirs nao a enxergam — por isso nem ReSTIR nem o
    // atlas do DDGI entram aqui.
    //
    // Sem coalescer, apesar de serem sliders: o dominio e so escrita de flag (nenhum
    // CommandQueue.Flush, ao contrario do NotifyMaterialRTStateChanged) e os historicos sao
    // curtos (Reflections MaxFrames = 12). Durante o arraste o especular fica reconvergindo, o
    // que e a leitura honesta do que esta acontecendo. Se virar incomodo, a generalizacao certa
    // e coalescer POR DOMINIO, nao uma flag ad-hoc por knob.
    void FRenderSettings::SetRainPuddleAmount(f32 _V) {
        if (_V == R.Weather.GetPuddleAmount()) return;
        R.Weather.SetPuddleAmount(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainPuddleAmount() const { return R.Weather.GetPuddleAmount(); }

    void FRenderSettings::SetRainPuddleScale(f32 _V) {
        if (_V == R.Weather.GetPuddleScale()) return;
        R.Weather.SetPuddleScale(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainPuddleScale() const { return R.Weather.GetPuddleScale(); }

    void FRenderSettings::SetRainRippleStrength(f32 _V) {
        if (_V == R.Weather.GetRippleStrength()) return;
        R.Weather.SetRippleStrength(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainRippleStrength() const { return R.Weather.GetRippleStrength(); }

    void FRenderSettings::SetRainWetDarkening(f32 _V) {
        if (_V == R.Weather.GetWetDarkening()) return;
        R.Weather.SetWetDarkening(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainWetDarkening() const { return R.Weather.GetWetDarkening(); }

    void FRenderSettings::SetRainOcclusion(bool _V) {
        if (_V == R.Weather.GetRainOcclusion()) return;
        R.Weather.SetRainOcclusion(_V);
        Invalidate(Dom::Specular);
    }
    bool FRenderSettings::GetRainOcclusion() const { return R.Weather.GetRainOcclusion(); }
    void FRenderSettings::SetRainCurtainAmount(f32 _V) { R.Weather.SetCurtainAmount(_V); }
    f32  FRenderSettings::GetRainCurtainAmount() const { return R.Weather.GetCurtainAmount(); }
    void FRenderSettings::SetRainParticles(bool _V) { R.Weather.SetRainParticles(_V); }
    bool FRenderSettings::GetRainParticles() const  { return R.Weather.GetRainParticles(); }

    // === Notificacoes de edicao =========================================================

    void FRenderSettings::MarkMaterialRTStateDirty()  { R.MaterialRTStateDirty  = true; }
    void FRenderSettings::MarkIndirectLightingDirty() { R.IndirectLightingDirty = true; }
    void FRenderSettings::MarkSceneContentDirty()     { R.SceneContentDirty     = true; }

    // O par do Renderer::NotifyGIRegionChanged: aquele cuida do atlas do DDGI por REGIAO, este
    // cuida de todo o resto que acumulou sobre a luz/geometria antiga. Separados porque so o
    // DDGI sabe invalidar por caixa — os outros nao tem granularidade espacial nenhuma.
    void FRenderSettings::NotifySceneContentChanged() { Invalidate(Dom::SceneContent); }

    // Reservoirs guardam Lo medido com a luz antiga; o atlas do DDGI, idem.
    void FRenderSettings::NotifyIndirectLightingChanged() { Invalidate(Dom::SkyRadiance); }

    void FRenderSettings::NotifySceneStructureChanged() { Invalidate(Dom::SceneStructure); }

    void FRenderSettings::NotifyCameraCut() { Invalidate(Dom::CameraCut); }

    // Os passos 3 e 4 do protocolo de captura, juntos porque separa-los nao tem uso: um reset que
    // limpe todo acumulador mas deixe a semente correndo produz ruido diferente a cada rodada,
    // e zerar so a semente deixa o resto do estado herdado do trajeto. Ver Docs/CAPTURE-PROTOCOL.md.
    //
    // O FrameIndex absoluto NAO e tocado — fences, frame slots e lifetime dependem de ele ser
    // monotonico. Essa e a razao de os dois contadores existirem separados.
    // Chamada de dentro do UpdateFrameCapture, sob o CaptureSetupGuard — sem ele o funil
    // cancelaria a sessao no ato de comeca-la.
    void FRenderSettings::NotifyDeterministicCapture() {
        Invalidate(Dom::DeterministicCapture);
        R.TemporalSampleIndex = 0;
    }

    void FRenderSettings::NotifyMaterialRTStateChanged() {
        // REFRESH (nao e invalidacao — e trabalho a refazer, nao memoria a descartar). O dreno
        // e necessario: o InstanceGeo e um upload heap sem versao por frame em voo, entao
        // reescrever com frames voando corromperia o que eles leem. Custa um stall, mas isto so
        // dispara em edicao manual de material.
        //
        // As DUAS filas: havia so o Flush da direta, e o trace do DDGI le o snapshot na COMPUTE.
        // Ver a nota do caminho barato do Renderer::OnSceneStructureChanged, que tinha o mesmo
        // buraco pelo mesmo motivo.
        R.CommandQueue.Flush();
        R.ComputeQueue.WaitIdle();
        R.RaytracingScene.RefreshInstanceGeo(R.Scene);
        R.TlasFlagsDirty = true; // mask/FORCE_NON_OPAQUE/culling saem do material
        // E os historicos acumulados sobre a aparencia antiga.
        Invalidate(Dom::MaterialRTState);
    }
}
