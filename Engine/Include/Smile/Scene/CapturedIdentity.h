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

    // Maior ID de instancia que sobrevive ao round-trip pelo canal float da superficie temporal.
    //
    // Espelha SMILE_TEMPORAL_MAX_INSTANCE_ID em Shaders/Temporal/TemporalMotionCommon.hlsli. O
    // valor gravado la e (id + 1) num float32, que representa inteiros exatamente ate 2^24
    // INCLUSIVE -- entao o maior ID e 2^24 - 1, cujo encoded e exatamente 2^24. Medido nas duas
    // bordas: 0xFFFFFF volta certo, 0x1000000 nao.
    //
    // E o mesmo teto do InstanceID do DXR, que tem 24 bits.
    inline constexpr u32 kMaxTemporalInstanceId = 0xFFFFFFu; // 16.777.215

    // Maior CONTAGEM enderecavel. Constante separada de proposito: o guard compara contagem com
    // contagem, e nao contagem com maximo de ID -- com indices comecando em zero, uma cena de
    // kMaxTemporalInstanceId + 1 renderaveis cabe inteira. Misturar as duas grandezas recusava
    // duas contagens validas.
    //
    // A recusa mora na CONSTRUCAO da TLAS porque passar do teto nao produz erro em lugar nenhum:
    // produz o objeto VIZINHO, em silencio.
    inline constexpr u32 kMaxTemporalInstanceCount = kMaxTemporalInstanceId + 1u; // 16.777.216

    // Valor gravado na GPU para a entrada `Index` da tabela. O +1 reserva o 0 para "vazio", que e
    // o que o alvo contem onde nenhum draw tocou.
    inline u32 EncodeCapturedIdentity(size_t Index) { return static_cast<u32>(Index) + 1u; }

    // Entrada da tabela de identidades de um frame, no layout que vai para a GPU.
    //
    // 8 bytes por instancia, e os dois campos sao necessarios: o slot diz ONDE a residencia esta, e
    // a generation diz se ela ainda e a MESMA. Sem a generation, um slot reciclado devolveria o
    // ocupante novo para um id gravado pelo antigo -- a troca silenciosa de identidade que esta
    // fase existe para eliminar.
    //
    // Layout fixado aqui porque o shader le a mesma memoria; mudar a ordem sem mudar o HLSL nao
    // daria erro de compilacao, daria dados trocados.
    struct FGpuIdentityEntry {
        u32 Slot       = FGpuSceneHandle::kInvalidSlot;
        u32 Generation = 0;
    };
    static_assert(sizeof(FGpuIdentityEntry) == 8, "layout compartilhado com o HLSL");

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
