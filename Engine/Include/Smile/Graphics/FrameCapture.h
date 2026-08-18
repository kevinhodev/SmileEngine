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
        // O denoiser PEDIDO. Continua com este nome e este significado: os manifestos ja tirados
        // da serie usam a chave assim, e renomea-la quebraria a comparacao com eles.
        const char* Denoiser = "";
        // O denoiser que RODOU, por DOMINIO. Existe porque o pedido nao descreve a execucao desde
        // que o seletor da Fase 6 entrou, e a divergencia e assimetrica: `NrdIndirectMode` sai de
        // `ReSTIRGIActive`, que sai da politica, enquanto `NrdDirectMode` nao. Com
        // `denoiser: NRD` + `primario: ddgi` o indireto sai CRU — GI e reflexao compostas sem
        // filtro — e a direta continua denoisada. Sem estes dois campos, esse rollback e a baseline
        // historica sairiam com `denoiser: NRD` nos dois e ninguem saberia comparar o que.
        //
        // O RR nao e por dominio — ele denoisa a cor COMPOSTA num eval so —, entao aparece nos dois
        // quando EXECUTA. Isso e descricao, e nao aproximacao: sob RR os dois dominios chegam crus
        // a ele e saem filtrados por ele.
        //
        // ⚠️ "Executa" e literal: o gate e `RRRanThisFrame`, marcado no ponto do Dispatch, e nao o
        // modo. Com o visualizador de debug ou o overlay de sondas escrevendo no HDR, o eval do RR
        // e PULADO e o frame sai cru — e ai os dois campos dizem `None` com `denoiser: DLSS_RR`
        // logo acima, que e a leitura correta daquele arquivo.
        const char* IndirectDenoiserEffective = "";
        const char* DirectDenoiserEffective   = "";
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
        // Scheduler do produtor dedicado. O conjunto de pixels e equivalente, mas a ordem de CAS
        // nao: sem este campo o controle full-screen e o compacto sairiam indistinguiveis.
        bool CacheCompactUpdate    = false;
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
        // QUARTO regime (`Sf`): a fonte do terminal por hit de render. Entra no manifesto e na
        // etiqueta pelo mesmo motivo dos outros dois — um atomico a mais por hit sombreado muda
        // custo e contencao, e o `Sd` fica CONGELADO como referencia historica.
        bool CacheStatsSource   = false;
        // Estado do aquecimento GLOBAL no disparo, e o knob que o governa. `cacheQuery` ja diz se
        // a consulta estava aberta; isto diz POR QUE ela estava fechada, e sao esperas de duracao
        // muito diferente ("resetando" acaba no proximo frame, "enchendo" leva dezenas).
        //
        // Ele importa mais para as capturas de N BAIXO: com o aquecimento automatico ligado, uma
        // captura em N=32 pode sair ainda em `filling` — mesma pose, mesmos knobs, imagem sem
        // cache nenhum. Sem este campo, ela seria indistinguivel de uma captura em que o cache
        // simplesmente nao acertou nada.
        const char* CacheWarmup = "";
        bool CacheAutoWarmup    = false;
        // Politica do indireto, PEDIDA e EFETIVA (Fase 6). As duas, e nao so a efetiva: sozinha,
        // ela nao mente sobre a imagem mas apaga a DEGRADACAO — `black` nao distingue "pedi preto
        // para medir" de "pedi DDGI e nao havia volume" de "pedi Environment, que nao existe".
        //
        // Com o seletor da Fase 6 no lugar, divergencia no primario significa **degradacao** e
        // nada mais: pedido que a capacidade nao honrou (SHaRC sem o passe pronto, DDGI sem
        // volume). A nota que morava aqui avisava do periodo em que o enum existia sem rotear, e
        // ela mesma dizia para sair quando o roteamento entrasse.
        const char* IndirectPrimaryRequested  = "";
        const char* IndirectPrimaryEffective  = "";
        const char* IndirectFallbackRequested = "";
        const char* IndirectFallbackEffective = "";
        // Ha raio consumindo o fallback? NAO e so o primario SHaRC: o `ShadeSurfaceHit` e
        // compartilhado, entao reflexoes e o 2o bounce das sondas terminam no mesmo fallback.
        // Falso, o campo de fallback descreve politica que ninguem exerceu no frame.
        bool IndirectFallbackActive = false;
        // DISPONIBILIDADE do atlas por papel — superficie (folhagem, subsurface, translucidos) e
        // nevoa —, e nao uso. Volume vivo numa cena sem folhagem nem translucidos ainda marca
        // superficie como disponivel; com a nevoa desligada, idem para a volumetrica. Uso REAL de
        // superficie ja tem instrumento proprio (a telemetria de fonte, `srcDdgi`); uso real da
        // volumetrica nao tem, e nao se inventa aqui.
        bool DDGISurfaceAvailable    = false;
        bool DDGIVolumetricAvailable = false;
        bool GIMeasureTerminatorOff = false; // corta o DDGI so no hit secundario
        // Politica de auto-interseccao/backface do gather E do produtor do cache — um toggle so
        // para os dois (ver FRenderSettings::SetGIBackfacePolicy). Entra aqui porque o protocolo
        // de medicao da Fase 3 pede capturas antes/depois dele: sem o campo, as duas sairiam com
        // nome e manifesto identicos, que e o mesmo defeito que o produtor do cache tinha.
        bool GIBackfacePolicy = false;

        // ---- MESH LIGHTS E AMOSTRAGEM DO ReSTIR DI (MESH-LIGHTS-PLAN.md, Fase 0) ------------
        //
        // O gate de saida da Fase 0 e "ter um manifesto que explique se mesh lights e cada
        // otimizacao realmente rodaram". Ate aqui o arquivo dizia apenas `restirDI: true`, o que
        // nao distingue nenhum dos casos que o plano precisa separar: cena sem geometria emissiva,
        // alias table ainda em readback, pool desligado no A/B, e sweep de candidatas.
        //
        // LEVANTADA contra AMOSTRAVEL: a primeira e o que a cena tem, a segunda e o que a
        // distribuicao consegue propor NESTE frame. Divergem enquanto o readback nao fecha — e um
        // manifesto com so a primeira afirmaria mesh lights participando de um frame em que o DI
        // viu pool vazio, que e exatamente o erro que o par regir/regirRequested ja custou uma
        // rodada de medicao para achar.
        u32  MeshLightSurveyed  = 0;
        u32  MeshLightSamplable = 0;
        bool MeshAliasReady     = false;
        // Fluxo total e a VALIDADE dele. Zero com `fluxValid: false` e "a tabela esta sendo
        // reconstruida"; zero com `true` e uma cena em que toda geometria emissiva tem radiancia
        // nula — dois estados que o mesmo numero descreveria igual.
        f64  MeshLightTotalFlux = 0.0;
        bool MeshLightFluxValid = false;
        // Fluxo total zero derruba a alias table para UNIFORME. Sem este campo o manifesto diria
        // "por potencia" num frame em que a distribuicao nao era por potencia.
        bool MeshLightUniformFallback = false;
        // Os tres motivos de um triangulo extraido nao contribuir, separados porque pedem acoes
        // opostas: degenerado e defeito de GEOMETRIA, radiancia zero e CONTEUDO (e pode ser o que
        // o artista quis), fluxo invalido e corrupcao. Derivados na CPU no mesmo laco que ja soma
        // o fluxo — sem atomico, sem passe extra, sem regime de medicao separado.
        u32  MeshTriDegenerate = 0, MeshTriZeroFlux = 0, MeshTriNonFinite = 0;

        // Orcamento por pool, PEDIDO e EFETIVO. Os dois porque divergem sozinhos: sem luz
        // analitica na cena o orcamento analitico efetivo vira zero, e sem alias table pronta o de
        // mesh idem. O sweep 8/4/2/1 do plano se le nesta linha.
        u32  DIAnalyticCandidatesRequested = 0, DIAnalyticCandidatesEffective = 0;
        u32  DIMeshCandidatesRequested     = 0, DIMeshCandidatesEffective     = 0;
        // Pedido = o toggle do A/B. Efetivo = o toggle E havia triangulo amostravel. A diferenca
        // entre "desliguei para medir" e "liguei e nao havia o que propor" e o achado, nao o
        // detalhe.
        //
        // ⚠️ `meshLightsInPoolEffective: true` NAO implica contribuicao de mesh light. Com
        // `diMeshCandidatesEffective: 0` o pool esta publicado mas nenhuma proposta e gerada, e
        // como a troca de orcamento limpa o historico, nenhum reservoir de triangulo sobrevive: a
        // contribuicao e zero. Quem responde "houve luz de mesh?" e o par com as candidatas, nunca
        // o toggle do pool sozinho.
        bool DIMeshLightsInPoolRequested = false, DIMeshLightsInPoolEffective = false;
        // Visibilidade inicial (Alg. 5 passo 2): pedido = toggle, efetivo = toggle E o DI rodou.
        bool DIInitialVisibilityRequested = false, DIInitialVisibilityEffective = false;
        // Resolucao INTERNA do DI. Hoje sempre igual a de render; vira o par
        // requested/effective quando a meia resolucao da Fase 3 entrar.
        u32  DIRenderWidth = 0, DIRenderHeight = 0;

        // Orcamento de memoria por recurso, que o plano pede para comparar contra os buffers RIS
        // das fases seguintes. O da alias NAO e VRAM — ela vive num heap de upload, e o
        // VramTracker ignora upload/readback de proposito. O custo existe e por isso e registrado;
        // so nao e da mesma moeda que o dos triangulos.
        u64  MeshLightTrianglePayloadBytes = 0;
        // As DUAS copias da alias, separadas por RESIDENCIA e nao por escolha: a de UPLOAD e o
        // STAGING onde a CPU escreve a tabela (system memory), a de DEFAULT e a que o Pass A
        // amostra (VRAM). Os dois custos existem e sao de moedas diferentes; um campo unico os
        // somaria como se fossem o mesmo.
        //
        // O par requested/effective que morava aqui saiu junto com o toggle: sem escolha, o campo
        // seria uma constante. ⚠️ Capturas de builds ANTERIORES a `681c1f2` podem ter amostrado o
        // upload heap — ali o discriminador e o campo `build`, nao a etiqueta do nome.
        u64  MeshLightAliasUploadPayloadBytes = 0, MeshLightAliasDefaultPayloadBytes = 0;
        // Compactacao para o suporte positivo. Pedido e EFETIVO porque divergem: com fluxo total
        // zero a compactacao nao se aplica, e o caminho de tabela uniforme sobre TODOS os
        // triangulos e preservado.
        bool MeshCompactRequested = false, MeshCompactEffective = false;
        // ⚠️ ALOCADO contra TAMANHO DO DOMINIO, e sao numeros diferentes de proposito. Os buffers
        // sao dimensionados pelo PIOR caso (todos os triangulos com fluxo) para o SRV nunca
        // precisar ser recriado quando o conteudo mudar — recriar exigiria drenar as filas, porque
        // as tabelas de trace do DI guardam copia do descritor.
        //
        // O que a compactacao reduz e o DOMINIO. ⚠️ Nao chamar isto de "bytes tocados": nao ha
        // instrumento de trafego, e o numero sai igual com o DI desligado, com zero candidatas ou
        // com a tabela em construcao. E o tamanho do conjunto sobre o qual o sorteio ACONTECERIA —
        // util porque e ele que se compara contra o tamanho do cache.
        u64  MeshLightAliasDomainBytes = 0, MeshLightTriangleDomainBytes = 0;
        // Os OUTROS buffers, que um campo unico de "bytes de triangulo" escondia: sao dois DEFAULT
        // (a saida da extracao e a copia que o Pass A amostra) mais staging e readback. Na Emerald
        // isso e 14,6 MB de VRAM em triangulo, e nao 7,3.
        u64  MeshLightTriangleCompactPayloadBytes = 0;
        u64  MeshLightTriangleStagingPayloadBytes = 0, MeshLightTriangleReadbackPayloadBytes = 0;
        // Soma do que ocupa VRAM de fato. Staging, readback e a alias de upload ficam de fora:
        // moram em system memory, e soma-los aqui inflaria o orcamento com o que nao e VRAM.
        u64  MeshLightVramBytes = 0;
        // Fases 1 e 2. Ficam em zero ate os buffers existirem — declarados agora para a serie de
        // baseline e a serie com RIS terem o MESMO conjunto de chaves, e a comparacao entre elas
        // nao depender de um campo que aparece no meio do caminho.
        u64  MeshLightRISBytes = 0, MeshLightRISCompactBytes = 0;
        // As tres otimizacoes que o plano ainda vai ligar. Todas false por ora, e pelo mesmo
        // motivo dos bytes acima: o manifesto da baseline tem de afirmar EXPLICITAMENTE que elas
        // nao rodaram, em vez de omitir e deixar a ausencia ser interpretada depois.
        bool MeshRISRequested        = false, MeshRISEffective        = false;
        bool MeshRISCompactRequested = false, MeshRISCompactEffective = false;
        bool DIHalfResRequested      = false, DIHalfResEffective      = false;

        // Ocupacao do hash e acerto de query no instante do disparo — dois dos quatro sinais que
        // a calibracao do N mede, e saem de graca porque os readbacks ja existem.
        // Queries/Hits ficam em zero sem a instrumentacao do cache ligada (ela custa dois
        // atomicos disputados por wave em todo trace, entao e knob proprio).
        u32 CacheOccupied = 0, CacheValid = 0, CacheSamples = 0, CacheCapacity = 0;
        // CONFIAVEIS (N >= piso) separadas de `CacheValid` (N >= 1). Sao numeros diferentes desde
        // que o piso existe, e e este que descreve o cache utilizavel.
        u32 CacheConfident = 0;
        // Despejos do frame. O painel ja mostrava, o arquivo nao guardava — e teste determinístico
        // de refresh/evicção perdia justamente o número que ele existe para observar.
        u32 CacheEvicted = 0;
        u32 CacheQueries = 0, CacheHits = 0;
        // Misses por motivo e saude da insercao, so com `cacheStatsDetail`. Entram no arquivo
        // porque DOIS gates de saida da Fase 4 sao exatamente estes numeros — "falha por balde
        // cheio abaixo de 0,1% das insercoes" e a leitura de ocupacao que decide a capacidade —,
        // e um gate que so existe num painel volatil nao e verificavel depois.
        u32 CacheMissShort = 0, CacheMissCone = 0, CacheMissNoEntry = 0;
        u32 CacheMissEmpty = 0, CacheMissWarming = 0, CacheMissStale = 0;
        // `InsertFull` (capacidade) separado de `Contended` (concorrencia): o gate de 0,1% e sobre
        // o primeiro, e somar o segundo mandaria aumentar a tabela por um problema que tabela
        // maior nao resolve. `Capped` e amostra descartada no teto de 64 — insercao que deu certo
        // e amostra que se perdeu mesmo assim.
        u32 CacheInsertTries = 0, CacheInsertFull = 0, CacheContended = 0;
        u32 CacheRetries = 0, CacheCapped = 0;
        u32 CacheProbeSum = 0, CacheProbeMax = 0;
        // O produtor. `cachePaths` contra `cacheUpdateFraction` e o gate de saida da Fase 3 que
        // ficou sem medida, e o terminal por tipo e o que separa "o cache realimenta" de "o cache
        // so ve ceu" numa captura de arquivo, sem depender de alguem ter olhado o painel na hora.
        u32 CachePaths = 0, CachePathVerts = 0, CachePathDepth = 0;
        u32 CacheTermSky = 0, CacheTermCache = 0, CacheTermKilled = 0;
        u32 CacheTermMiss = 0, CacheTermNoQuery = 0, CacheTermLobe = 0, CacheTermOther = 0;
        // Fonte do terminal do RENDER (so com `cacheStatsSource`). O gate de saida da Fase 5 e
        // uma CURVA nestes numeros — `srcCache` subindo e `srcDdgi` caindo conforme o cache
        // aquece —, e curva nao se confere em painel volatil.
        //
        // DOIS EIXOS: `cache + ddgi + zero == total` (quem forneceu) e `ineligible` (por que nao
        // foi o cache), que cruza com DDGI e com zero — nunca com cache. Quanto da inelegibilidade
        // caiu no DDGI e interseccao, e nao sai destes numeros: leia os dois eixos como marginais.
        u32 CacheSrcTotal = 0, CacheSrcCache = 0, CacheSrcDDGI = 0;
        u32 CacheSrcZero = 0, CacheSrcIneligible = 0;

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
        // Aquecimento automatico do radiance cache: guardado nos DOIS presets, como o estado
        // temporal acima, e pelo mesmo tipo de motivo — nao e uma preferencia de qualidade, e uma
        // coisa que quebra a sessao. Com ele ligado, a borda `Filling -> Active` cai no meio do
        // aquecimento e derruba o historico dos consumidores, o que quebra o contrato "N frames
        // apos UM reset" (e, pelo funil, cancelaria a captura). A sessao inteira roda com ele
        // desligado; o manifesto grava o que valeu, entao a captura se descreve.
        bool CacheAutoWarmup = false;
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
