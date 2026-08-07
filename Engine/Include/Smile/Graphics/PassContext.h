#pragma once

#include <d3d12.h>
#include <vector>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/FrameContext.h"

// O QUE UMA FASE DE GRAVACAO RECEBE.
//
// O FrameContext.h resolveu "qual e o estado do frame" (modos, camera, luz, ambiente). Isto aqui
// resolve o problema irmao e distinto: "o que uma fase precisa TER EM MAOS para gravar". Sao os
// ~10 locais de nivel 0 do RenderFrame com vida longa — medidos, nao chutados: o CommandList vive
// por 2009 linhas, o Viewport por 1745, o Scissor por 1715, o DSV por 1456, o ObjectCBBase por
// 913 e a lista de visiveis por 859. Enquanto eles forem locais, nenhuma fase pode virar metodo.
//
// O que NAO entra aqui e tao importante quanto o que entra. Ficaram de fora ~20 locais de vida
// curta (as 6 planos do frustum, os jobs de sombra local, os flags do visualizador de debug, o
// PostInput do resolve): eles pertencem a UMA fase e viajam com ela. Inchar o contexto com esses
// transformaria o God object em God struct — que e exatamente a armadilha que fez a Smile recusar
// o RDG (ver a nota de desenho no RenderPass.h).
namespace Smile {

    class FTextureSRVHeap;
    class FGpuProfiler;
    class FMaterial;
    class FGpuMesh;
    struct FRenderable;
    struct FSceneTargets;

    // Um renderavel aceito para este frame: passou pelos filtros baratos (visivel, malha valida,
    // nao e so-RT) e ja tem o ObjectConstants escrito no slot. SEM culling de camera — e a lista
    // que os casters de sombra e o mapa de oclusao de chuva percorrem, porque o que esta fora da
    // tela ainda projeta sombra e ainda tapa a chuva.
    struct FDrawItem {
        const FRenderable* R          = nullptr;
        FMaterial*         Mat        = nullptr;
        u32                Slot       = 0; // indice no ObjectCB deste frame em voo
        u32                SceneIndex = 0; // indice em Scene.Renderables()
    };

    // O subconjunto que a CAMERA ve: frustum + oclusao HZB aplicados, ordenado front-to-back.
    // E a lista do z-prepass, do G-buffer e dos translucidos.
    struct FVisibleItem {
        const FRenderable* R          = nullptr;
        FMaterial*         Mat        = nullptr;
        f32                Dist       = 0.0f; // AO QUADRADO — so serve para ordenar
        u32                Slot       = 0;
        u32                SceneIndex = 0;
    };

    // A malha selecionada no editor, resolvida durante a montagem das listas (e o mesmo laco que
    // ja percorre a cena). Viaja junta porque o contorno de selecao a consome ~1450 linhas depois,
    // no fim do frame, e recomputar exigiria varrer a cena outra vez.
    struct FSelectionDraw {
        static constexpr u32 kNoSlot = 0xFFFFFFFFu;
        u32             Slot       = kNoSlot;
        const FGpuMesh* Mesh       = nullptr;
        Mat44           Model      = Mat44::Identity();
        int             Renderable = -1;    // indice em Scene.Renderables(); -1 = nada
    };

    // A imagem que o pos-processamento consome. Sai do resolve (TAA, upscaler ou nada) e pode
    // ainda ser trocada pelo heatmap de flicker. NAO entra no FPassContext: nasce e morre entre
    // duas fases vizinhas, e o criterio deste header e vida longa.
    struct FPostInput {
        ID3D12Resource* Texture = nullptr;
        u32             SRVSlot = 0;
    };

    // Montado uma vez por frame e passado a cada fase. Os ponteiros para o estado resolvido sao
    // nao-donos e apontam para locais do RenderFrame, que sobrevivem a fase inteira.
    //
    // As LISTAS ficam por valor: elas nascem no meio do frame (a montagem depende do readback de
    // oclusao gravado ha kFramesInFlight frames), entao o contexto e preenchido em duas etapas —
    // primeiro o binding, depois as listas. Isto e honesto em vez de fingir que tudo esta pronto
    // no topo: as fases que rodam antes da montagem (agua, ceu, DDGI) nao tocam nas listas.
    struct FPassContext {
        // --- Gravacao ------------------------------------------------------------------
        ID3D12GraphicsCommandList* Cmd      = nullptr;
        ID3D12Device*              Device   = nullptr;
        FTextureSRVHeap*           SRVHeap  = nullptr;
        FGpuProfiler*              Profiler = nullptr;

        // --- Frame ----------------------------------------------------------------------
        u32 FrameSlot    = 0;
        u32 RenderWidth  = 0;
        u32 RenderHeight = 0;
        u32 OutputWidth  = 0;
        u32 OutputHeight = 0;

        // --- Estado resolvido (FrameContext.h) -------------------------------------------
        const FFrameModes*    Modes   = nullptr;
        const FFrameView*     View    = nullptr;
        const FFrameLighting* Light   = nullptr;
        const FFrameAmbient*  Ambient = nullptr;

        // --- Alvos e binding -------------------------------------------------------------
        FSceneTargets*              Targets      = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS   FrameCB      = 0; // FrameConstants deste slot
        D3D12_GPU_VIRTUAL_ADDRESS   ObjectCBBase = 0; // base do ObjectCB; somar Slot * stride
        D3D12_CPU_DESCRIPTOR_HANDLE DSV{};
        D3D12_VIEWPORT              Viewport{};       // res de render CHEIA — os passes que
        D3D12_RECT                  Scissor{};        // mudam (meia-res) restauram daqui

        // Fence da fila de COMPUTE assincrona. 0 = o DDGI rodou na fila direta neste frame (async
        // desligado, ou relocacao de sondas pedindo ordem estrita). Diferente do resto do
        // contexto, este campo e ESCRITO por uma fase e lido por outras duas — quem submete o
        // DDGI, quem espera por ele antes de consumir o atlas, e o fim do frame, que decide se a
        // janela de stats mostra a linha de compute. E o unico estado de sincronizacao que
        // atravessa o frame; por isso mora aqui e nao num retorno.
        u64 AsyncGIFence = 0;

        // --- Listas de draw (2a etapa: BuildDrawLists) ------------------------------------
        std::vector<FDrawItem>    All;     // sem cull de camera
        std::vector<FVisibleItem> Visible; // frustum + HZB, front-to-back
        FSelectionDraw            Selection;

        // Endereco do ObjectConstants de um item. O `* 256` do ObjectConstants e o stride real;
        // recebe-lo evita este header ter de conhecer o layout.
        D3D12_GPU_VIRTUAL_ADDRESS ObjectCB(u32 Slot, u64 Stride) const {
            return ObjectCBBase + static_cast<u64>(Slot) * Stride;
        }
    };
}
