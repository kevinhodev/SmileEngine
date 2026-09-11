#pragma once

// O comando de um draw indireto do main view (Fase 5 do GPU-DRIVEN-PLAN).
//
// Hoje cada renderavel do opaco custa, na CPU: duas constantes de root, um IASet* quando a malha
// muda, e um DrawIndexedInstanced. Na sintetica de 1M isso e 763.408 draws no G-buffer por frame,
// mais um std::sort proprio para agrupa-los. A Fase 5 troca isso por um buffer que a GPU escreve e
// um `ExecuteIndirect` por classe.
//
// Este header e o CONTRATO desse buffer. Ele existe separado do FPipelineState pelo mesmo motivo
// pratico do GpuInstanceTransform.h: o teste que executa o comando de verdade nao pode arrastar o
// renderer inteiro para dentro do executavel de teste.
//
// ⚠️ O layout NAO e escolhido aqui. Ele e ditado pela ORDEM das entradas em `pArgumentDescs`: o
// command processor le os campos na sequencia em que os argumentos foram declarados. Trocar duas
// entradas na descricao compila, executa, e escreve a linha do material no slot da instancia. Por
// isso a descricao e a struct nascem no MESMO lugar, e o teste executa o comando de verdade em vez
// de reler o buffer.

#include "Smile/Core/Types.h"

#include <cstddef>
#include <type_traits>
#include <d3d12.h>

namespace Smile {
    // Offsets DENTRO do bloco de constantes de root do main view (b4).
    //
    // Moraram no FPipelineState ate a Fase 5; vieram para ca porque a command signature os usa como
    // `DestOffsetIn32BitValues` e o teste precisa dos MESMOS valores sem linkar a engine. O
    // FPipelineState continua expondo os nomes antigos como alias — uma fonte, dois nomes.
    //
    // Quatro sao por PASSE e uma e por DRAW no caminho direto; no indireto, DUAS sao por comando
    // (`MaterialRow` e `InstanceSlot`) e as outras tres continuam amarradas uma vez pelo passe. Que
    // elas SOBREVIVAM ao ExecuteIndirect e propriedade testada: um `DestOffset` errado sobrescreve
    // o descritor da tabela de transform e a geometria colapsa sem erro de API.
    namespace GpuDrawConstants {
        inline constexpr u32 kMaterialTable  = 0;
        inline constexpr u32 kMaterialRow    = 1;
        inline constexpr u32 kTransformTable = 2;
        inline constexpr u32 kPrevTransform  = 3;
        inline constexpr u32 kInstanceSlot   = 4;
        inline constexpr u32 kCount          = 5;
    }

    // Um comando. Espelhado em HLSL como `GpuIndirectDrawArgs`
    // (Shaders/Scene/GpuIndirectDraw.hlsli).
    //
    // Os cinco ultimos campos sao, campo a campo, `D3D12_DRAW_INDEXED_ARGUMENTS` — e os
    // static_asserts abaixo comparam com a struct do SDK em vez de repetir os offsets a mao, que
    // seria comparar um espelho com outro espelho.
    struct FGpuIndirectDrawArgs {
        u32 MaterialRow;  // b4[kMaterialRow]  — linha na tabela canonica de materiais
        u32 InstanceSlot; // b4[kInstanceSlot] — slot na tabela canonica de instancias

        u32 IndexCountPerInstance;
        u32 InstanceCount;
        u32 StartIndexLocation;
        // INT, e nao UINT: e o contrato do D3D12, e o valor e SOMADO a cada indice antes da leitura
        // do vertex buffer. Publicar como u32 funcionaria ate uma malha passar de INT_MAX vertices
        // no pool, e ai o comportamento seria silenciosamente outro.
        i32 BaseVertexLocation;
        u32 StartInstanceLocation;
    };

    // 28 B, e nao 32: o payload e exatamente isto. Um quarto dword de padding so se justificaria
    // como canario, e canario sem leitor e padding com nome bonito.
    static_assert(sizeof(FGpuIndirectDrawArgs) == 28);
    static_assert(alignof(FGpuIndirectDrawArgs) == 4);
    static_assert(offsetof(FGpuIndirectDrawArgs, MaterialRow)  == 0);
    static_assert(offsetof(FGpuIndirectDrawArgs, InstanceSlot) == 4);

    // A metade de DRAW tem de casar com a struct do SDK, campo a campo. Sem isto, reordenar dois
    // campos aqui continuaria compilando: o command processor leria `InstanceCount` onde esta
    // `IndexCountPerInstance` e desenharia a malha errada com a contagem errada.
    inline constexpr size_t kGpuIndirectDrawOffset = offsetof(FGpuIndirectDrawArgs,
                                                              IndexCountPerInstance);
    static_assert(sizeof(FGpuIndirectDrawArgs) ==
                  kGpuIndirectDrawOffset + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
#define SMILE_DRAW_ARG_MATCHES(Field)                                                        \
    static_assert(offsetof(FGpuIndirectDrawArgs, Field) - kGpuIndirectDrawOffset ==          \
                  offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, Field));                            \
    static_assert(sizeof(FGpuIndirectDrawArgs::Field) ==                                     \
                  sizeof(D3D12_DRAW_INDEXED_ARGUMENTS::Field));                              \
    static_assert(std::is_same_v<decltype(FGpuIndirectDrawArgs::Field),                      \
                                 decltype(D3D12_DRAW_INDEXED_ARGUMENTS::Field)>)
    SMILE_DRAW_ARG_MATCHES(IndexCountPerInstance);
    SMILE_DRAW_ARG_MATCHES(InstanceCount);
    SMILE_DRAW_ARG_MATCHES(StartIndexLocation);
    SMILE_DRAW_ARG_MATCHES(BaseVertexLocation); // pega o u32 no lugar do INT
    SMILE_DRAW_ARG_MATCHES(StartInstanceLocation);
#undef SMILE_DRAW_ARG_MATCHES

    // A DESCRICAO do comando, que e o que de fato define o layout acima.
    //
    // Recebe o indice do parametro de root porque ele NAO e universal: o main view usa 14, e as
    // sombras, a chuva e o picking tem root signatures proprias com o bloco em outro indice (ver
    // `kOccDrawConstantsRootIndex`, `kDrawConstantsRootIndex`). Cada familia ganha a sua command
    // signature quando chegar a vez dela; o que todas compartilham e a struct.
    //
    // Devolve o desc pronto para `CreateCommandSignature`, apontando para `_Out`, que tem de
    // sobreviver a chamada.
    inline D3D12_COMMAND_SIGNATURE_DESC MakeIndirectDrawSignatureDesc(
        D3D12_INDIRECT_ARGUMENT_DESC (&_Out)[3], u32 _RootParameterIndex) {
        // A ORDEM aqui E o layout da struct. Os dois offsets nao sao contiguos no bloco (2 e 3 sao
        // descritores por passe), entao sao duas entradas de uma constante cada.
        _Out[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        _Out[0].Constant.RootParameterIndex      = _RootParameterIndex;
        _Out[0].Constant.DestOffsetIn32BitValues = GpuDrawConstants::kMaterialRow;
        _Out[0].Constant.Num32BitValuesToSet     = 1;

        _Out[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        _Out[1].Constant.RootParameterIndex      = _RootParameterIndex;
        _Out[1].Constant.DestOffsetIn32BitValues = GpuDrawConstants::kInstanceSlot;
        _Out[1].Constant.Num32BitValuesToSet     = 1;

        _Out[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

        D3D12_COMMAND_SIGNATURE_DESC Desc{};
        Desc.ByteStride       = sizeof(FGpuIndirectDrawArgs);
        Desc.NumArgumentDescs = 3;
        Desc.pArgumentDescs   = _Out;
        return Desc;
    }
}
