#pragma once

#include <d3d12.h>
#include <string>
#include "Smile/Core/Types.h"

// ================================================================================================
// CAPTURA DETERMINISTICA — a regua da serie SHaRC (Docs/CAPTURE-PROTOCOL.md).
//
// O SHARC-PRIMARY-GI-PLAN.md proibe fechar fase por "parece melhor". Este arquivo e o que da
// sentido a essa proibicao: sem uma regua, "melhor" nao e verificavel. O que se compara a partir
// da Fase 3 — ruido, convergencia temporal, ocupacao do hash do radiance cache — e funcao da
// posicao EXATA da camera e do estado inicial dos acumuladores, e nao da cena "mais ou menos
// igual".
//
// A ORDEM DE UMA CAPTURA e fixa, e cada passo esta onde esta por um motivo:
//
//   1. Aplicar preset       — antes de tudo, porque preset muda resolucao/upscaler e isso realoca
//                             alvos. Feito pelo Renderer (UpdateFrameCapture), fora da gravacao.
//   2. Restaurar camera     — do bookmark. E do CHAMADOR: quem tem os slots e o editor, e a pose
//                             precisa estar aplicada antes de o reset acontecer.
//   3. Reset deterministico — FRenderSettings::NotifyDeterministicCapture. Mais forte que o corte
//                             de camera: derruba tambem os caches de MUNDO.
//   4. Zerar a semente      — no mesmo Notify; ver o comentario la.
//   5. Aquecer N frames     — RENDERIZADOS, nao tiques da UI: um frame que nao foi renderizado nao
//                             convergiu nada, e contar tique tornaria o N dependente da carga da
//                             maquina, ou seja, da maquina.
//   6. Capturar o frame N.
//   7. PNG + manifesto.
//
// O N e FIXO para todo o A/B, escolhido uma vez por calibracao. Parada adaptativa por captura
// daria N diferente entre configuracoes, e a diferenca de N viraria vies: a configuracao que
// converge mais devagar seria medida com mais tempo de acumulacao, que e precisamente a variavel
// em teste.
// ================================================================================================
namespace Smile {

    // Cientifico e Gameplay medem coisas diferentes e os DOIS sao necessarios. Uma regressao que
    // so aparece no gameplay e interacao com o upscaler; uma que so aparece no cientifico e do
    // estimador, e o upscaler esta mascarando.
    enum class ECapturePreset : u32 {
        // Nao muta nada: captura a engine como o operador a configurou. E o que o jogador ve, que
        // e o unico resultado que importa no fim.
        Gameplay = 0,
        // Resolucao nativa, sem upscaler e sem TAA. Elimina a maior fonte de diferenca entre
        // rodadas (o jitter) e mede o SINAL, nao o reconstrutor.
        Scientific = 1
    };

    struct FCaptureRequest {
        std::wstring   OutputDir;             // vazio = <exe>/Captures
        std::string    Label;                 // prefixo do arquivo; vazio = "capture"
        std::string    SceneName;             // so p/ o manifesto (o motor nao guarda o path da cena)
        i32            BookmarkSlot = -1;     // -1 = camera livre; so p/ o manifesto
        // 128 e CALIBRADO, nao arredondado: sweep de 32/64/128/256 em Docs/CAPTURE-PROTOCOL.md. A
        // ocupacao do cache termina em 64, mas a repetibilidade da imagem so estabiliza em 128
        // (50,6 -> 54,4 -> 59,3 dB), e de 128 para 256 sao 0,03 dB pelo dobro da espera.
        // Escolher pela ocupacao teria dado 64 — metade do necessario.
        u32            WarmupFrames = 128;
        ECapturePreset Preset       = ECapturePreset::Scientific;
        // Hora do dia a FIXAR durante a sessao, em [0,24); negativo = usar a hora corrente.
        //
        // A hora nao pode ser canonicalizada como o ElapsedTime: aquele e "segundos desde o boot",
        // sem significado para o operador, enquanto esta E a iluminacao autorada. Mas tambem nao
        // pode simplesmente congelar no valor corrente: com o Time-of-Day correndo, duas capturas
        // disparadas com minutos de diferenca sairiam com o sol em posicoes diferentes, e um
        // manifesto registrando as duas horas nao torna as imagens comparaveis.
        //
        // Entao ela vira PARAMETRO da captura, como a pose: quem dispara declara a hora, e duas
        // capturas com a mesma hora declarada tem o mesmo sol por construcao.
        f32            PinTimeOfDayHours = -1.0f;
    };

    // O estado REAL da engine no instante do disparo. Existe para o manifesto nao ser digitado a
    // mao: erro humano na terceira rodada e exatamente o que o arquivo automatico elimina.
    // Preenchido pelo Renderer (CollectCaptureState) — ninguem mais tem acesso a tudo isto.
    struct FCaptureState {
        u32 OutputWidth = 0, OutputHeight = 0;
        u32 RenderWidth = 0, RenderHeight = 0;
        f32 RenderScale = 1.0f;

        const char* Upscaler = "";
        const char* Denoiser = "";
        int  UpscalerQuality = 0;
        bool UseTAA = false;

        // Os toggles do A/B da Fase 0. A matriz de quatro baselines do plano se le daqui.
        bool UseGI              = false; // dominio indireto ligado
        bool DDGIReady          = false; // o VOLUME existe (criterio de fallback, ver a67eadd)
        bool ReSTIRGI           = false;
        bool ReSTIRDI           = false;
        bool ReGIR              = false; // EFETIVO: a grade foi construida neste frame
        // Os dois campos que explicam um ReGIR pedido e nao construido. Ele exige consumidor E
        // luz PUNTUAL elegivel; numa cena iluminada so por geometria emissiva (a Bistro tem
        // `"lights": []`) o toggle fica ligado e a grade nunca sai. Sem isto, o manifesto diz
        // "regir: false" e nao ha como distinguir toggle desligado, recurso ausente e cena sem
        // luz puntual — foram tres hipoteses e uma rodada de medicao para descobrir a terceira.
        bool ReGIRRequested     = false; // o toggle
        u32  PunctualLightCount = 0;     // luzes elegiveis empacotadas para o indireto
        bool Reflections        = false;
        bool CacheUpdate        = false; // radiance cache: escrita
        bool CacheQuery         = false; // radiance cache: leitura
        // QUEM escreveu. Sao dois ESTIMADORES, nao dois niveis de qualidade: com false o cache
        // aprendeu dos hits do render (terminador = DDGI), com true de um path tracer proprio que
        // nunca le sonda. Sem este campo, as duas capturas sairiam indistinguiveis — mesma
        // etiqueta, mesmo manifesto, imagens que nao tem por que se parecer.
        bool CacheDedicatedUpdate  = false;
        // Fracao EFETIVA (quantizada em 1/25 pela permutacao do tile 5x5), nao a pedida: ela manda
        // na velocidade de convergencia, entao duas capturas com fracoes diferentes tiradas no
        // mesmo N estao em pontos diferentes do aquecimento.
        f32  CacheUpdateFraction   = 0.0f;
        // Terminal do caminho de update no cache resolvido. E o que separa "um bounce" de
        // "multi-bounce no tempo" — a diferenca de energia entre os dois nao e sutil.
        bool CacheUsePrevTerminal  = false;
        // Vertices sombreados por caminho e piso de roughness gravavel. Os dois decidem o que a
        // celula guarda, e os dois sao EIXO DE MEDICAO da Fase 3: o A/B 1x4 e o sweep do piso.
        // Sem eles no arquivo, as capturas dessas duas medidas sairiam indistinguiveis — o mesmo
        // defeito que o produtor e a fracao ja tiveram.
        u32  CacheMaxVertices      = 0;
        f32  CacheMinRoughness     = 0.0f;
        // Piso de confianca EFETIVO: amostras minimas para uma celula encerrar um caminho. Zero
        // quer dizer "ninguem consultou o cache neste frame", e nao "piso zero".
        //
        // Ele muda a imagem por dois caminhos ao mesmo tempo — quantos raios de render terminam no
        // cache, e o que o terminal do updater grava nas celulas seguintes —, entao duas capturas
        // que so diferem nele sao duas configuracoes. Mesmo criterio dos vertices e do piso de
        // roughness, e por isso ele entra tambem na etiqueta do nome do arquivo.
        u32  CacheMinSamples       = 0;
        // Instrumentacao do cache. NAO e um observador neutro — ver
        // FRadianceCache::PublishedStatsThisFrame. Duas capturas so sao comparaveis se estiverem
        // no MESMO regime, e por isso ela entra tambem na etiqueta do nome do arquivo.
        bool CacheStats         = false;
        // Regime de DETALHE: misses por motivo e saude da insercao. Entra no manifesto pelo mesmo
        // motivo que o de cima, e com mais forca — ele soma atomicos tambem no PRODUTOR, onde eles
        // decidem quem vence a corrida do CAS e, portanto, o conteudo da tabela.
        bool CacheStatsDetail   = false;
        bool GIMeasureTerminatorOff = false; // corta o DDGI so no hit secundario
        // Politica de auto-interseccao/backface do gather E do produtor do cache — um toggle so
        // para os dois (ver FRenderSettings::SetGIBackfacePolicy). Entra aqui porque o protocolo
        // de medicao da Fase 3 pede capturas antes/depois dele: sem o campo, as duas sairiam com
        // nome e manifesto identicos, que e o mesmo defeito que o produtor do cache tinha.
        bool GIBackfacePolicy = false;

        // Ocupacao do hash e acerto de query no instante do disparo — dois dos quatro sinais que
        // a calibracao do N mede, e saem de graca porque os readbacks ja existem.
        // Queries/Hits ficam em zero sem a instrumentacao do cache ligada (ela custa dois
        // atomicos disputados por wave em todo trace, entao e knob proprio).
        u32 CacheOccupied = 0, CacheValid = 0, CacheSamples = 0, CacheCapacity = 0;
        u32 CacheQueries = 0, CacheHits = 0;
        // Misses por motivo e saude da insercao, so com `cacheStatsDetail`. Entram no arquivo
        // porque DOIS gates de saida da Fase 4 sao exatamente estes numeros — "falha por balde
        // cheio abaixo de 0,1% das insercoes" e a leitura de ocupacao que decide a capacidade —,
        // e um gate que so existe num painel volatil nao e verificavel depois.
        u32 CacheMissShort = 0, CacheMissCone = 0, CacheMissNoEntry = 0;
        u32 CacheMissEmpty = 0, CacheMissWarming = 0, CacheMissStale = 0;
        u32 CacheInsertTries = 0, CacheInsertFull = 0;
        u32 CacheProbeSum = 0, CacheProbeMax = 0;
        // O produtor. `cachePaths` contra `cacheUpdateFraction` e o gate de saida da Fase 3 que
        // ficou sem medida, e o terminal por tipo e o que separa "o cache realimenta" de "o cache
        // so ve ceu" numa captura de arquivo, sem depender de alguem ter olhado o painel na hora.
        u32 CachePaths = 0, CachePathVerts = 0, CachePathDepth = 0;
        u32 CacheTermSky = 0, CacheTermCache = 0, CacheTermKilled = 0, CacheTermZero = 0;

        // Semente com que ESTE frame amostrou. Tem de bater com o N do aquecimento; se nao bater,
        // o contrato de warm-up quebrou em algum lugar e o manifesto denuncia.
        u32 TemporalSampleIndex = 0;
        u32 FrameIndex          = 0;

        f32 CameraPos[3]{};
        f32 PitchDeg = 0.0f, YawDeg = 0.0f, FovYDeg = 0.0f;
        f32 SunDir[3]{};
        f32 TimeOfDayHours = 0.0f;
        // Time-of-Day LIGADO neste frame, e a hora que a sessao de fato fixou (negativa = nenhuma).
        //
        // O pedido pode trazer um pin e ele nao ser aplicado: com o TOD desligado o sol e autorado
        // a mao e nao deriva da hora, entao fixar a hora nao faria nada. Gravar o pin PEDIDO nesse
        // caso faria o manifesto afirmar um controle que nao houve — e duas capturas com o mesmo
        // pin declarado, mas TOD off, poderiam ter sois completamente diferentes.
        bool TimeOfDayEnabled   = false;
        f32  PinnedHoursApplied = -1.0f;
    };

    // O que a sessao guarda para devolver no fim. Um struct, e nao variaveis soltas, porque
    // aplicar e restaurar tem de ser a MESMA funcao lendo o mesmo campo: era assim que um dos dois
    // lados esquecia o UseTAA.
    struct FCaptureSettings {
        // --- Knobs de render: mutados SO pelo preset cientifico ---------------------------
        u32  Upscaler        = 0;      // EUpscaler, opaco aqui (o enum vive no Upscaler.h)
        u32  Denoiser        = 0;      // EDenoiser, idem
        int  UpscalerQuality = 0;
        bool UseTAA          = false;
        f32  RenderScale     = 1.0f;
        bool KnobsMutated    = false;  // gameplay nao mexe em nenhum deles

        // --- Estado temporal: guardado SEMPRE, nos dois presets ---------------------------
        // Sem isto a captura ainda dependia de QUANDO foi disparada. Congelar o relogio no valor
        // corrente fixa uma fase ARBITRARIA de nuvem, onda e vento — diferente a cada sessao —, e
        // um processo que assenta congela onde o exp() estivesse no instante do clique. Os dois
        // viram valor canonico durante a sessao e voltam ao real no fim, porque fora da captura
        // eles pertencem ao mundo, nao a medicao.
        f32  ElapsedTime = 0.0f;
        f32  Wetness     = 0.0f;
        // Hora e sol de antes da sessao. A captura pode FIXAR outra hora (ver
        // FCaptureRequest::PinTimeOfDayHours) e reafirma-la a cada frame; no fim o mundo volta ao
        // relogio do operador. O sol vai junto porque com o Time-of-Day desligado ele e autorado a
        // mao e nao deriva da hora.
        f32  TimeOfDayHours = 0.0f;
        f32  SunDir[3]      = { 0.0f, 1.0f, 0.0f };
    };

    // Maquina de estados + recursos + IO. NAO conhece o Renderer: quem aplica preset, faz o reset
    // e sabe o estado da engine e ele, e sao tres call sites explicitos (UpdateFrameCapture,
    // RecordPost, FinishFrameCapture). Ver a nota do RenderPass.h sobre por que a Smile nao
    // uniformiza Execute.
    class FFrameCapture {
    public:
        enum class EStage : u32 {
            Idle,     // sem sessao
            Warmup,   // aquecendo; Remaining frames renderizados ate o disparo
            Shoot     // ESTE frame e o capturado
        };

        struct FResult {
            bool         Success = false;
            std::wstring PngPath;
            std::wstring ManifestPath;
            std::string  Error;
            u32          WarmupFrames = 0;
        };

        // Enfileira. Recusa (false) se ja houver captura em curso — duas sessoes concorrentes
        // dariam um PNG com o aquecimento da outra.
        bool Request(const FCaptureRequest& Req);
        bool HasPendingRequest() const { return RequestPending; }
        const FCaptureRequest& Pending() const { return PendingRequest; }

        // Consome o pedido e comeca a contar. O chamador ja aplicou o preset e ja fez o reset.
        void BeginSession();

        EStage Stage() const           { return CurrentStage; }
        bool   ShouldShoot() const     { return CurrentStage == EStage::Shoot; }
        bool   Busy() const            { return CurrentStage != EStage::Idle || RequestPending; }
        u32    WarmupRemaining() const { return Remaining; }
        const FCaptureRequest& Active() const { return ActiveRequest; }

        // Guarda o estado a devolver (Before) e o que o preset acabou de aplicar (Applied). O
        // Renderer aplica; aqui so mora o dado.
        //
        // GUARDAR e DEVOLVER sao dois estados, e nao um: o stash nasce no comeco da sessao e a
        // devolucao so vence no fim dela. Com um flag so, o "ha algo para restaurar" ficava
        // verdadeiro ja no primeiro frame de aquecimento e o preset durava exatamente um frame —
        // a captura sairia com o upscaler do operador de volta, silenciosamente.
        //
        // O Applied existe para a restauracao nao pisar numa escolha do operador: se ele mexer num
        // knob durante a sessao, o funil cancela a captura, mas o valor NOVO tem de sobreviver. Ver
        // Renderer::RestoreCaptureState.
        void StashSettings(const FCaptureSettings& Before, const FCaptureSettings& Applied) {
            RestoreSettings = Before;
            AppliedSettings = Applied;
            HasStash        = true;
        }
        bool HasPendingRestore() const { return RestoreDue; }
        const FCaptureSettings& AppliedByPreset() const { return AppliedSettings; }
        FCaptureSettings ConsumeRestore() { RestoreDue = false; return RestoreSettings; }

        // Copia o backbuffer para o readback. Chamar DEPOIS do tonemap e ANTES dos overlays do
        // editor (contorno de selecao, gizmos): a captura e da imagem, nao da ferramenta.
        // O backbuffer tem de estar em RENDER_TARGET; volta nesse estado.
        void RecordCopy(ID3D12Device* Device, ID3D12GraphicsCommandList* CL,
                        ID3D12Resource* BackBuffer, u32 Width, u32 Height);

        // Fim do frame. Devolve true quando o frame que acabou foi o de captura — o chamador
        // entao sincroniza a fila e chama Finish.
        bool AdvanceFrame();

        // Aborta. Existe porque o contrato e "N frames consecutivos apos UM reset", e ha eventos
        // que o quebram sem tocar no capturador: carga de cena, resize (recria alvos e invalida
        // historico) e qualquer knob que derrube acumulador no meio do aquecimento. Nenhum deles
        // pode ser recusado — o operador tem o direito de redimensionar a janela —, entao a
        // medicao e que cede, e em voz alta: uma captura sub-aquecida em silencio e pior que
        // captura nenhuma, porque entra no A/B parecendo valida.
        //
        // Alcanca tambem o pedido AINDA PENDENTE. A janela entre o Request e o primeiro frame e
        // curta mas real, e uma cena trocada dentro dela faria a sessao comecar na cena nova
        // carregando nome e bookmark da antiga — errado de um jeito que so apareceria ao ler o
        // manifesto muito depois.
        void Cancel(const char* Reason);
        // A sessao COMECOU (aquecendo ou disparando). Diferente do Busy(), que ja conta o pedido
        // enfileirado.
        bool SessionActive() const { return CurrentStage != EStage::Idle; }

        // Grava PNG + manifesto. Exige que a copia ja tenha terminado na GPU.
        void Finish(const FCaptureState& State);

        // O editor consome quando o resultado ficar pronto (a sessao vive na render thread).
        bool ConsumeResult(FResult& Out);

        void Release();

    private:
        bool EnsureReadback(ID3D12Device* Device, u32 Width, u32 Height);
        std::wstring ResolveOutputDir() const;

        FCaptureRequest PendingRequest;
        FCaptureRequest ActiveRequest;
        bool            RequestPending = false;

        EStage CurrentStage = EStage::Idle;
        u32    Remaining    = 0;

        FCaptureSettings RestoreSettings; // estado de ANTES da sessao
        FCaptureSettings AppliedSettings; // o que o preset pos no lugar
        bool             HasStash   = false;
        bool             RestoreDue = false; // a sessao acabou; o Renderer devolve no proximo frame

        // Um so buffer, e nao um por frame em voo: a captura sincroniza a fila logo depois da
        // copia. O stall custa um frame numa operacao que ja e explicitamente offline, e paga
        // com a simplicidade de nao existir estado pendente atravessando frames.
        ComPtr<ID3D12Resource> Readback;
        u64 ReadbackBytes  = 0;
        u32 ReadbackWidth  = 0;
        u32 ReadbackHeight = 0;
        u32 ReadbackPitch  = 0;
        bool CopyRecorded  = false;

        FResult LastResult;
        bool    ResultReady = false;
    };
}
