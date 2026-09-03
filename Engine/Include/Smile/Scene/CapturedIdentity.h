#pragma once

// Identidade de um renderavel CAPTURADA no momento em que um passe foi gravado, e a codificacao
// compacta que a GPU consegue carregar.
//
// Por que existe: um passe com readback atrasado (picking hoje, HiZ a seguir) grava um u32 por
// draw e le o resultado frames depois. Gravar o INDICE do renderavel torna esse u32 uma referencia
// a "quem ocupa aquela posicao agora", e entre a gravacao e a leitura a lista pode ter andado.
// Era assim que o pick funcionava, e so nao trocava de objeto porque toda mudanca estrutural
// cancelava o pick em voo -- descarte global, que funciona e joga fora trabalho valido.
//
// Aqui o u32 indexa uma TABELA daquele passe, e a tabela guarda de quem era cada draw. CPU puro e
// sem d3d12 de proposito: e o que permite testar a codificacao e a decodificacao sem device.

#include "Smile/Core/Types.h"
#include "Smile/Scene/GpuSceneHandle.h"

#include <vector>

namespace Smile {
    struct FCapturedIdentity {
        // Residencia GPU no momento da captura. Guarda para consumidores que NAO tem Id (o
        // readback do HiZ endereca por slot) e para o caso de a residencia ser aposentada sem o
        // renderavel sair da cena.
        FGpuSceneHandle Handle;
        // Identidade de EDITOR. E ela que resolve para a posicao atual, e como Id nunca e
        // reciclado dentro da sessao, um objeto removido simplesmente nao resolve.
        u64             Id = 0;
    };

    // Valor gravado na GPU para a entrada `Index` da tabela. O +1 reserva o 0 para "vazio", que e
    // o que o alvo contem onde nenhum draw tocou.
    inline u32 EncodeCapturedIdentity(size_t Index) { return static_cast<u32>(Index) + 1u; }

    // Inverso do Encode. Devolve identidade ZERADA (Id == 0) para o vazio e para qualquer valor
    // fora da tabela.
    //
    // Fora de faixa nao e erro de programacao a ser assertado: significa que o readback nao
    // corresponde a tabela atual -- passe regravado no meio, tabela ja limpa --, e a resposta
    // segura ali e "nada", nao "o que estiver naquela posicao".
    inline FCapturedIdentity DecodeCapturedIdentity(const std::vector<FCapturedIdentity>& Table,
                                                    u32 Value) {
        if (Value == 0 || Value > Table.size()) return FCapturedIdentity{};
        return Table[Value - 1];
    }
}
