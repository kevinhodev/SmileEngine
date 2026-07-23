#pragma once

#include "Smile/Core/Types.h"
#include <string>
#include <vector>

namespace Smile {

    // Como o passe de debug deve INTERPRETAR o conteudo do alvo. Canal/multiplicador NAO entram
    // aqui — isso e resolvido pelo ChannelWeight (Vec4) do tile, no estilo do r_ShowRenderTarget
    // da Cry: um unico uniforme cobre "so o vermelho", "so o alfa" e "multiplique por 2".
    // Este enum e so p/ o que o peso nao resolve: dado EMPACOTADO que precisa desempacotar.
    enum class EDebugDecode : u32 {
        Raw = 0,       // rgb direto (LDR / ja em faixa visivel)
        Grayscale,     // alvo de 1 canal: replica R em rgb. Sem isso um R8 sai VERMELHO (g=b=0)
        HDR,           // tonemap + exposicao — radiance, GI, reflexao (senao estoura branco)
        GBufferField,  // decode do G-buffer; SubIndex escolhe o campo (absorve os 8 modos antigos)
        OctNormal,     // normal octaedrica empacotada -> xyz*0.5+0.5
        ReverseZ,      // depth em reverse-Z -> linearizado (mostrar cru da tela branca)
        Velocity,      // motion vector escalado pela resolucao
        Count
    };

    // Um alvo que os passes publicam p/ o visualizador. Chaveado por NOME (nao por enum): e o
    // que Cry e Unreal fazem, e significa que passe novo custa uma linha e nada a manter numa
    // lista central. Registrar de novo com o mesmo nome SOBRESCREVE — e o que torna o resize
    // (que realoca SRVs) trivial: o passe so re-registra depois de recriar as views.
    struct FDebugTarget {
        std::string  Name;                              // ex.: "Reflexos · temporal"
        u32          SrvSlot   = 0xFFFFFFFFu;
        EDebugDecode Decode    = EDebugDecode::Raw;
        u32          SubIndex  = 0;                     // campo do G-buffer, quando GBufferField
        u32          MipCount  = 1;                     // > 1 habilita seletor de mip (HZB)
    };

    // Registro global, no mesmo idioma do VramTracker (que todo passe ja chama na criacao).
    // Nao possui os recursos nem os mantem vivos: guarda so nome + slot + como interpretar.
    // Quem registra e responsavel por re-registrar apos resize.
    namespace DebugTargets {
        void Register(const std::string& Name, u32 SrvSlot,
                      EDebugDecode Decode = EDebugDecode::Raw,
                      u32 SubIndex = 0, u32 MipCount = 1);

        void Clear();

        // Todos os alvos, na ordem de registro.
        const std::vector<FDebugTarget>& All();

        // Filtro por substring (case-insensitive), no espirito do "r_ShowRenderTarget hdr".
        // Filtro vazio devolve tudo. E isto que alimenta a grade: digitar "reflex" traz os
        // 5 estagios de uma vez.
        std::vector<FDebugTarget> Query(const std::string& Filter);

        // Indice em All() do alvo com este nome exato; kInvalid se nao existir.
        static constexpr u32 kInvalid = 0xFFFFFFFFu;
        u32 IndexOf(const std::string& Name);
    }
}
