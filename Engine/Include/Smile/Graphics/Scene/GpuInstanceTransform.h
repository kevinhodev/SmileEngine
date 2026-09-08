#pragma once

// O que o vertex shader precisa por instancia, e de onde isso passa a vir (Fase 4 do
// GPU-DRIVEN-PLAN).
//
// Hoje o VS recebe QUATRO matrizes por draw, num CBV de 256 B escrito por instancia por frame
// (`ObjectConstants`). Elas nao sao dados: sao DERIVADAS de duas matrizes que ja moram na tabela
// canonica de instancias — `Transform` e `PrevTransform` — mais tres matrizes de VIEW que sao as
// mesmas para o passe inteiro.
//
// Medido na Fase 0: 407 KB por frame na Bistro, 634 KB na Emerald e 256 MB em 1M, num upload heap.
// A conta que a fase persegue e trocar isso por (a) tres matrizes por PASSE e (b) um indice de
// slot por draw.
//
// Este header e o contrato dessa troca, e existe separado do Renderer.h por um motivo pratico: o
// teste de paridade que compara a composicao do HLSL com a do C++ nao pode arrastar o renderer
// inteiro para dentro do executavel de teste.

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"

namespace Smile {
    // As tres matrizes de view do passe. Espelhado em HLSL como `GpuViewMatrices`.
    //
    // Por PASSE, e nao por instancia: e a metade do `ObjectConstants` que nunca dependeu do
    // objeto. Um CBV amarrado uma vez substitui N escritas de 256 B.
    struct FGpuViewMatrices {
        Mat44 ViewProj{};               // jittered — vai para SV_POSITION
        Mat44 ViewProjUnjittered{};     // motion vector do frame ATUAL
        Mat44 PrevViewProjUnjittered{}; // motion vector do frame ANTERIOR
    };
    static_assert(sizeof(FGpuViewMatrices) == 192);
    static_assert(offsetof(FGpuViewMatrices, ViewProj)               ==   0);
    static_assert(offsetof(FGpuViewMatrices, ViewProjUnjittered)     ==  64);
    static_assert(offsetof(FGpuViewMatrices, PrevViewProjUnjittered) == 128);

    // O que o VS consome. E o layout que o `ObjectConstants` carrega hoje; quando o caminho
    // bindless virar default, ele deixa de existir como BUFFER e passa a ser um valor composto
    // dentro do shader.
    //
    // O alinhamento de 256 vem do requisito de root CBV, e nao do conteudo: enquanto o caminho
    // classico existir, esta struct e indexada por `sizeof()` dentro do ObjectCB.
    struct alignas(256) FGpuObjectTransforms {
        Mat44 MVP;            // Model * ViewProj (jittered) — SV_POSITION
        Mat44 ModelMatrix;    // world, para worldPos/worldNormal
        Mat44 CurMVPNoJitter; // Model * ViewProjUnjittered — motion vector
        Mat44 PrevMVP;        // PrevModel * PrevViewProjUnjittered — motion vector
    };
    static_assert(sizeof(FGpuObjectTransforms) == 256);
    static_assert(offsetof(FGpuObjectTransforms, MVP)            ==   0);
    static_assert(offsetof(FGpuObjectTransforms, ModelMatrix)    ==  64);
    static_assert(offsetof(FGpuObjectTransforms, CurMVPNoJitter) == 128);
    static_assert(offsetof(FGpuObjectTransforms, PrevMVP)        == 192);

    // A composicao. Uma funcao, chamada pelos DOIS lados do A/B.
    //
    // Escrita aqui e nao repetida no laco de montagem porque o espelho HLSL
    // (Shaders/Scene/InstanceTransform.hlsli) reproduz exatamente estas quatro linhas, e o teste de
    // paridade compara as duas saidas. Se a versao do C++ fosse uma copia solta dentro do
    // BuildDrawLists, o teste passaria a comparar o espelho com OUTRA copia — e as duas poderiam
    // divergir da que roda no frame.
    //
    // `A * B` do Mat44 e o produto linha-major padrao (result[r][c] = soma_k A[r][k]*B[k][c]), que
    // e o mesmo que `mul(A, B)` em HLSL sobre `row_major`. Trocar a ordem aqui e o erro classico
    // desta migracao, e e o que o teste de paridade existe para pegar.
    inline FGpuObjectTransforms ComposeObjectTransforms(const FGpuViewMatrices& View,
                                                        const Mat44& Model, const Mat44& PrevModel) {
        FGpuObjectTransforms Out;
        Out.MVP            = Model * View.ViewProj;
        Out.ModelMatrix    = Model;
        Out.CurMVPNoJitter = Model * View.ViewProjUnjittered;
        // PrevModel, e nao Model: sem historico daquele objeto os dois sao iguais e o motion
        // vector do OBJETO zera, o que e o desfecho certo para quem acabou de nascer ou teve o
        // slot reciclado. Herdar o Model do ocupante anterior inventaria um passado.
        Out.PrevMVP        = PrevModel * View.PrevViewProjUnjittered;
        return Out;
    }
}
