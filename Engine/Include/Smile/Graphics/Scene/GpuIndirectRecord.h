#pragma once

// A GRAVACAO dos `ExecuteIndirect` do main view (Fase 5 do GPU-DRIVEN-PLAN, passos 5 e 9 da secao 6.2).
//
// O ultimo elo da cadeia reset -> classify -> scatter -> finalizacao -> ESTE. O compute ja escreveu os
// argumentos dentro das regioes e publicou o count buffer; aqui a CPU grava, para cada lote, uma
// chamada que le a regiao DAQUELE lote com o par VB/IB DAQUELE pool.
//
// ⚠️ O MESMO layout dos dois lados. A versao que o `ScatterCB` recebe tem de sair do `Layout` passado
// para `GpuRecordIndirectDraws` — e e ela que faz a finalizacao zerar o frame quando a tabela na GPU
// for de outro layout. Gravar os `ExecuteIndirect` com um layout e subir a tabela de outro e o defeito
// silencioso que a versao existe para pegar (ver GpuClassify.h).
//
// ⚠️ O estado que continua sendo do PASSE: PSO, root signature, as tres constantes que sobrevivem ao
// comando, viewport, render targets e a topologia (TRIANGLELIST). E o `ExecuteIndirect` ZERA as duas
// constantes que o comando escreve (medido em WARP, ver GpuIndirectDraw.h): o primeiro draw direto
// depois desta gravacao tem de reamarra-las.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Scene/GpuClassify.h"

#include <span>
#include <d3d12.h>

namespace Smile {
    // O par VB/IB de um pool, em forma de view. Indexado pelo `GeometryPoolId`.
    //
    // ⚠️ Nas UNIDADES da tabela de malhas, e a partir da BASE do pool: o `VertexOffset` da linha conta
    // `Vertex` e o `IndexOffset` conta indices de 4 B, os dois desde o inicio do recurso. Stride
    // diferente de `sizeof(Vertex)`, ou indice em R16, a API aceita — e le outro trecho do pool:
    // geometria errada sem erro. A porta confere stride, formato, endereco nao nulo e que cabe um
    // elemento; a BASE ela nao tem como ver (a view nao traz o recurso), e e de quem monta a view.
    struct FGpuPoolViews {
        D3D12_VERTEX_BUFFER_VIEW Vertex{};
        D3D12_INDEX_BUFFER_VIEW  Index{};
    };

    // Os recursos que as chamadas leem.
    struct FGpuIndirectDrawTargets {
        // A command signature de PRODUCAO (`MakeIndirectDrawSignatureDesc`), criada contra a root
        // signature do passe.
        ID3D12CommandSignature* Signature  = nullptr;
        // `IndirectArgs`, em INDIRECT_ARGUMENT.
        ID3D12Resource*         Args       = nullptr;
        // O count buffer que a FINALIZACAO publica — nunca `BatchCounts`, que o classify escreve sem
        // saber se o frame fecha. Em INDIRECT_ARGUMENT.
        ID3D12Resource*         DrawCounts = nullptr;
    };

    // Por que a gravacao NAO pode acontecer. Tudo aqui e da CPU.
    enum EGpuIndirectRecord : u32 {
        GpuIndirectRecordOk = 0,
        // Signature, argumentos ou count buffer nulos.
        GpuIndirectRecordMissingTarget = 1,
        // Layout nunca construido: versao 0, sem regioes. A versao que o scatter compararia seria zero.
        GpuIndirectRecordInvalidLayout = 2,
        // Classe de pipeline fora de `kGpuPipelineClassCount`: o indice do lote sairia da tabela.
        GpuIndirectRecordInvalidClass = 3,
        // Sem pool, ou pools demais para a tabela de lotes — a porta `GpuClassifyBatchBufferSlots`.
        GpuIndirectRecordBadPoolCount = 4,
        // O layout nao tem regiao para todo lote destes pools.
        GpuIndirectRecordSmallRegionTable = 5,
        // `IndirectArgs` menor que o total do layout: o `ExecuteIndirect` leria alem do recurso.
        GpuIndirectRecordSmallArgsBuffer = 6,
        // O count buffer nao tem uma entrada por lote.
        GpuIndirectRecordSmallCountBuffer = 7,
        // `IndirectArgs` ou o count buffer nao sao BUFFER. O tamanho sozinho nao pega: numa textura o
        // `Width` e largura em texels, e uma larga o bastante passa pela conta de bytes.
        GpuIndirectRecordArgsNotBuffer  = 8,
        GpuIndirectRecordCountNotBuffer = 9,
        // A view de um pool que vai ganhar chamada nao serve: endereco nulo, stride que nao e o de
        // `Vertex`, ou menos de um vertice. Pool sem chamada nao tem a view conferida — ela nao chega
        // a lista.
        GpuIndirectRecordBadVertexView = 10,
        // O mesmo para o indice: endereco nulo, formato que nao e R32_UINT, ou menos de um indice.
        GpuIndirectRecordBadIndexView  = 11,
    };

    // A porta da gravacao. Consultada ANTES de gravar o compute do frame: recusada, o frame vai inteiro
    // para o caminho classico, porque um dispatch ja gravado nao se desfaz.
    u32 GpuIndirectRecordPreflight(const FGpuIndirectLayout& Layout, u32 PipelineClass,
                                   std::span<const FGpuPoolViews> Pools,
                                   const FGpuIndirectDrawTargets& Targets);

    // Grava os `ExecuteIndirect` de UMA classe: um por pool cuja regiao tem capacidade, na ordem dos
    // pools, cada um com
    //
    //     IASetVertexBuffers / IASetIndexBuffer(Pools[p])
    //     ExecuteIndirect(Signature, MaxCommandCount      = Capacity,
    //                     Args,       ArgumentBufferOffset = Offset * 28,
    //                     DrawCounts, CountBufferOffset    = lote * 4)
    //
    // Regiao de capacidade ZERO nao grava chamada: nenhum comando cabe nela, e um que tentasse vira falha
    // que zera o frame inteiro. Pular ali nao esconde geometria.
    //
    // Refaz a porta; recusada, nao grava NADA e devolve o motivo. As views de todos os pools com
    // chamada sao conferidas ali, antes da primeira: nao existe gravacao parcial. `OutCalls` recebe
    // quantas chamadas foram gravadas.
    u32 GpuRecordIndirectDraws(ID3D12GraphicsCommandList* List, const FGpuIndirectLayout& Layout,
                               u32 PipelineClass, std::span<const FGpuPoolViews> Pools,
                               const FGpuIndirectDrawTargets& Targets, u32& OutCalls);
}
