#pragma once

#include "Smile/Core/Types.h"

// Grafo de invalidacao de historico, como DADO.
//
// Havia 19 listas de "quem cai junto" escritas a mao nos setters, parcialmente sobrepostas.
// O custo disso nao e a repeticao — e que as duas manutencoes normais exigem varrer todas:
//
//   1. Nasce um acumulador novo (outro denoiser, outro cache temporal). Ele precisa entrar
//      em N listas, e esquecer uma so aparece como "historico que nao morre" muito depois.
//   2. Um knob muda de natureza. Os dois knobs citados no FRenderSettings NASCERAM sendo de
//      sampler puro e deixaram de ser DEPOIS, em commits que nao voltaram no setter — e o
//      Docs/KNOBS-AUDIT.md catalogou quatro casos concretos disso.
//
// Aqui o ALVO e a lista sao declarados uma vez. (1) vira um bit novo + acrescenta-lo aos
// dominios certos; (2) vira uma linha mudando de dominio, e a revisao passa a ser "este knob
// pertence a este dominio?" em vez de "esta lista de sete chamadas esta completa?".
//
// Os dominios sao nomeados pelo MOTIVO, nao pelos alvos. Dois dominios com a mesma mascara
// hoje continuam separados de proposito se o motivo for diferente: eles divergem quando um
// alvo novo entra, e e ai que o nome paga.
namespace Smile {

    // Um bit por historico que sobrevive ao frame. Nao entram aqui coisas que sao REFRESH e
    // nao acumulacao (Flush da fila, RefreshInstanceGeo, TlasFlagsDirty): aquilo e trabalho a
    // refazer, nao memoria a descartar, e continua explicito em quem precisa.
    enum class EHistoryTarget : u32 {
        None             = 0,
        DDGIAtlas        = 1u << 0,  // Hysteresis 0,99: sem reset o valor velho e quase eterno
        ReGIR            = 1u << 1,
        ReSTIRGI         = 1u << 2,  // ValidateInterval = 0: nao ha re-shade que corrija o Lo
        ReSTIRDI         = 1u << 3,
        Reflections      = 1u << 4,  // temporal proprio, MaxFrames = 12
        NrdIndirect      = 1u << 5,
        NrdDirect        = 1u << 6,  // instancia dedicada a direta; historico independente
        RayReconstruct   = 1u << 7,  // historico neural do DLSS-RR
        TemporalAA       = 1u << 8,
        VolumetricFog    = 1u << 9,  // acumula o proprio inscatter, que le o DDGI
        VolumetricClouds = 1u << 10,
        TemporalMotion   = 1u << 11,
        HiZOcclusion     = 1u << 12, // readback ring do occlusion culling
        ProbeDiagnostic  = 1u << 13, // one-shot por clique: reexecuta, senao o painel mente
    };

    constexpr EHistoryTarget operator|(EHistoryTarget A, EHistoryTarget B) {
        return static_cast<EHistoryTarget>(static_cast<u32>(A) | static_cast<u32>(B));
    }
    constexpr bool HasTarget(EHistoryTarget Set, EHistoryTarget Bit) {
        return (static_cast<u32>(Set) & static_cast<u32>(Bit)) != 0;
    }

    namespace HistoryDomain {
        using T = EHistoryTarget;

        // O que acumula DEPOIS do resolve de tela e ainda faz parte do SINAL: hoje, o denoiser
        // do indireto. Aparece em quase todo dominio abaixo — e o argumento do
        // SetGIBackfacePolicy, "o clear dos reservoirs sozinho nao basta, o NRD acumula SOBRE
        // eles".
        //
        // NAO inclui mais os filtros de TELA (TemporalAA, RayReconstruct), e essa e a diferenca
        // que importa. Eles estavam aqui, e como este dominio entra em todos os outros, QUALQUER
        // knob da engine — cada tique de slider, cada toggle, criar ou apagar objeto — resetava o
        // AA e o upscaler. `TAARanLastFrame=false` faz o TAA sair sem blend (um frame aliasado) e
        // `RRResetPending` vira o `reset` do FSR2/DLSS (um frame reconstruido sem historico): a
        // tela inteira re-resolvia de uma vez, o que se ve como uma piscada.
        //
        // O `reset` desses filtros existe para CORTE DE CAMERA, nao para edicao de conteudo (a UE
        // liga o equivalente so no `bCameraCut`). Mudanca de conteudo e absorvida pelo proprio
        // filtro: clamp de vizinhanca e teste de disoclusao rejeitam o historico invalido em
        // alguns frames, sem pop. O preco aceito e que a mudanca aparece progressivamente em vez
        // de instantanea. Quem precisa mesmo de reset esta no CameraCut, no fim deste arquivo.
        inline constexpr T Resolve = T::NrdIndirect;

        // Trocou o filtro de tela em si (upscaler/TAA). Aqui o reset e legitimo: o filtro novo
        // nao tem historico nenhum, e o do antigo nao lhe serve.
        inline constexpr T TemporalOnly = T::TemporalAA | T::RayReconstruct;

        // Mudou a radiancia resolvida na tela, sem mexer no que esta GRAVADO no reservoir.
        // (ReSTIR GI: Visibility e Spatial atuam no Pass B e no resolve; o espacial nao
        // realimenta o temporal.)
        inline constexpr T ScreenResolve = Resolve;

        // Mudou o sinal ESPECULAR: o raio de reflexao, ou os guides de material que definem o
        // lobo dele (roughness/normal/albedo do G-buffer).
        inline constexpr T Specular = T::Reflections | Resolve;

        // Mudaram so os guides que o Ray Reconstruction consome, sem mexer em cor nem em
        // rugosidade (hoje: depth/velocity da agua).
        //
        // Reseta filtro de tela, e aqui isso e CORRETO — nao e mudanca de conteudo: o que mudou
        // foi o proprio depth/velocity com que TAA e RR reprojetam. Com a referencia de
        // reprojecao trocada debaixo deles, o historico aponta para o lugar errado, que e a
        // mesma situacao do CameraCut. Por isso este entra na lista dos que PODEM resetar.
        inline constexpr T Guides = T::RayReconstruct | T::TemporalAA;

        // Mudou o que o RAIO ENXERGA no hit (mascara, backface, alpha-test, fade de borda,
        // bias de amostragem). Muda o Lo gravado no reservoir E o valor devolvido as sondas.
        inline constexpr T RayVisibility = T::DDGIAtlas | T::ReSTIRGI | T::Reflections |
                                           T::VolumetricFog | T::ProbeDiagnostic | Resolve;

        // Mudou a GEOMETRIA do raio (epsilons, offsets, TMin). Alcanca tambem o shadow ray da
        // direta, por isso inclui o eixo do DI.
        inline constexpr T RayGeometry = T::DDGIAtlas | T::ReSTIRGI | T::ReSTIRDI |
                                         T::Reflections | T::NrdDirect | Resolve;

        // Mudou a ENERGIA que alimenta o indireto: radiancia do ceu, cor da luz-chave,
        // peso de luz no RT. Distinto do RayVisibility — la muda o que o raio VE, aqui muda
        // quanto de luz existe para ele ver.
        //
        // NAO inclui TemporalMotion, apesar de o NotifyIndirectLightingChanged ter incluido por
        // muito tempo. Aquele historico e GEOMETRICO — world position + InstanceID por pixel,
        // mais os transforms por instancia — e nada nele deriva de radiancia. Mudar a cor da luz
        // nao move superficie nem troca InstanceID. Ele continua no MaterialRTState, onde e
        // legitimo: la as flags de instancia da TLAS mudam, e o buffer e indexado pelo InstanceID.
        inline constexpr T SkyRadiance = T::DDGIAtlas | T::ReSTIRGI | T::Reflections |
                                         T::VolumetricFog | Resolve;

        // Trocou de denoiser. O teto de firefly do ReSTIR depende do denoiser e e aplicado ao
        // Lo NA HORA DO TRACE, ou seja, fica gravado no reservoir. Inclui o RayReconstruct
        // explicitamente (o Resolve nao o carrega mais): trocar PARA ou DE DLSS-RR troca o
        // proprio acumulador, entao ele tem de comecar limpo — mesmo caso do TemporalOnly.
        inline constexpr T DenoiserSwap = T::ReSTIRGI | T::ReSTIRDI | T::Reflections |
                                          T::NrdDirect | T::RayReconstruct | Resolve;

        // Trocou o SAMPLER de luzes do mundo para hits secundarios (ReGIR).
        inline constexpr T IndirectSampler = T::ReGIR | T::DDGIAtlas | T::ReSTIRGI |
                                            T::Reflections | Resolve;

        // Propriedade de material que o RT enxerga mudou. O mais largo: alem de tudo que
        // acumula, o chamador ainda tem refresh a fazer (Flush + InstanceGeo + flags da TLAS).
        inline constexpr T MaterialRTState = T::DDGIAtlas | T::TemporalMotion | T::ReSTIRGI |
                                             T::ReSTIRDI | T::NrdDirect | T::Reflections |
                                             Resolve;

        // A LISTA de renderaveis mudou: um objeto nasceu ou morreu, e o indice de todos os que
        // vinham depois andou. Mais largo que o MaterialRTState, por dois motivos que se somam:
        //
        //  1. Os historicos indexados por INDICE passam a ler outro objeto — nao e valor velho,
        //     e valor de outra pessoa. Sao o TemporalMotion (buffer de transforms por InstanceID)
        //     e o HiZOcclusion (ring de readback por indice de cena, com kFramesInFlight de
        //     atraso, ou seja: resultado gravado ANTES da mudanca chega DEPOIS dela).
        //  2. Os historicos de radiancia acumularam sobre uma geometria que nao existe mais (ou
        //     sem uma que passou a existir). Vale ate para o ReGIR: a geometria removida podia
        //     ser emissiva e estar nos pools de luz.
        //
        // VolumetricClouds fica de fora — as nuvens sao um volume proprio, nao veem a lista.
        inline constexpr T SceneStructure = T::DDGIAtlas | T::ReGIR | T::ReSTIRGI | T::ReSTIRDI |
                                            T::Reflections | T::NrdDirect | T::TemporalMotion |
                                            T::HiZOcclusion | T::VolumetricFog |
                                            T::ProbeDiagnostic | Resolve;

        // A CAMERA SALTOU: teleporte do foco do editor, carga de cena, buffers recriados. E o
        // unico caso em que resetar os filtros de tela e a resposta certa — nao existe vetor de
        // movimento ligando o frame novo ao antigo, entao reprojetar so traria imagem de outro
        // lugar. E exatamente para isto que serve o flag `reset` do FSR2/DLSS.
        //
        // Cai tudo que REPROJETA por movimento: os filtros de tela, os denoisers, os reservoirs
        // de tela do ReSTIR, o froxel (grid em espaco de view), o readback do HiZ e o
        // TemporalMotion. As nuvens entram porque o temporal delas tambem e reprojecao de tela.
        //
        // NAO caem os caches de MUNDO — DDGIAtlas e ReGIR. Uma sonda irradia o mesmo
        // independentemente de onde a camera esteja; derruba-las faria o GI reconvergir do zero
        // a cada duplo-clique no outliner, que e o oposto do que este dominio existe para evitar.
        inline constexpr T CameraCut = T::TemporalAA | T::RayReconstruct | T::NrdIndirect |
                                       T::NrdDirect | T::ReSTIRGI | T::ReSTIRDI |
                                       T::Reflections | T::TemporalMotion | T::HiZOcclusion |
                                       T::VolumetricFog | T::VolumetricClouds;

        // --- Invariante deste arquivo, verificada pelo compilador -------------------------
        // Os filtros de TELA so podem cair onde o historico deles realmente perdeu a
        // referencia: corte de camera, troca do proprio filtro, troca de denoiser. Um dominio
        // de CONTEUDO que volte a inclui-los faz a cena piscar a cada slider — e o sintoma
        // (piscada) fica a 20 call sites da causa (uma constante aqui), que foi exatamente por
        // que isto durou tanto sem ser visto. Entao o erro passa a ser de compilacao.
        inline constexpr T ScreenFilters = T::TemporalAA | T::RayReconstruct;
        constexpr bool ResetsScreenFilters(T Domain) {
            return (static_cast<u32>(Domain) & static_cast<u32>(ScreenFilters)) != 0;
        }

        static_assert(!ResetsScreenFilters(Resolve),
            "Resolve entra em quase todo dominio de conteudo; resetar filtro de tela aqui faz "
            "a engine inteira piscar a cada knob");
        static_assert(!ResetsScreenFilters(ScreenResolve), "dominio de conteudo");
        static_assert(!ResetsScreenFilters(Specular),      "dominio de conteudo");
        static_assert(!ResetsScreenFilters(RayVisibility), "dominio de conteudo");
        static_assert(!ResetsScreenFilters(RayGeometry),   "dominio de conteudo");
        static_assert(!ResetsScreenFilters(SkyRadiance),   "dominio de conteudo");
        static_assert(!ResetsScreenFilters(IndirectSampler), "dominio de conteudo");
        static_assert(!ResetsScreenFilters(MaterialRTState), "dominio de conteudo");
        static_assert(!ResetsScreenFilters(SceneStructure),  "dominio de conteudo");
        // E os quatro onde resetar E a resposta certa — em todos, o que quebrou foi a propria
        // referencia de reprojecao, nao o conteudo da cena.
        static_assert(ResetsScreenFilters(CameraCut),
            "CameraCut existe justamente para resetar os filtros de tela");
        static_assert(ResetsScreenFilters(Guides),       "trocou o depth/velocity da reprojecao");
        static_assert(ResetsScreenFilters(TemporalOnly), "trocou o filtro de tela");
        static_assert(ResetsScreenFilters(DenoiserSwap), "trocou o acumulador do denoiser");
    }
}
