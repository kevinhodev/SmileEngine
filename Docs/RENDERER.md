# Arquitetura do Renderer

Este documento é o mapa de navegação do `Renderer`: mostra onde cada responsabilidade vive,
como o frame atravessa a classe e quais fronteiras não devem ser quebradas. A lista detalhada e
atualizada dos passes permanece em [`ARCHITECTURE.md`, §5.2](ARCHITECTURE.md#52-renderframe--ordem-real-dos-passes);
o protocolo de captura permanece em [`CAPTURE-PROTOCOL.md`](CAPTURE-PROTOCOL.md).

## Mapa físico

`Renderer` ainda é a fachada pública e o agregador de estado, mas sua implementação está separada
por motivo de mudança:

```text
 Engine/Include/Smile/Graphics/Renderer/Renderer.h
                  |
                  | declara API, estado compartilhado e fases privadas
                  v
 Engine/Source/Graphics/Renderer/
   |
   +-- Renderer.cpp          lifecycle, resize, hot reload e política efetiva
   +-- RendererScene.cpp     mutações de cena, câmera e setup de recursos por cena
   +-- RendererSceneState.h  cena e câmera CPU, seleção, bounds, versões e dirty flags
   +-- RendererCapture.cpp   captura determinística, readbacks e visualizadores
   +-- RendererFrame.cpp     snapshot CPU e orquestração de um frame
   +-- RendererFrameState.h  históricos, jitter, relógios e contadores temporais
   +-- RendererPasses.cpp    gravação de comandos e listas de draw/luzes

 Engine/Include/Smile/Graphics/Backend/RenderBackend.h
 Engine/Source/Graphics/Backend/RenderBackend.cpp
   +-- device, filas, profilers, swapchain, heap e lifecycle do backend D3D12
```

Essa divisão é uma fronteira de manutenção, não cinco objetos independentes. Os arquivos ainda
implementam a mesma classe e compartilham seu estado. Uma feature nova deve preferir um subsistema
proprietário em `Graphics/<Domínio>`; o `Renderer` deve apenas coordená-lo.

## Fronteira pública

O `Renderer` é a fachada da render thread, não um localizador de serviços do backend. Consumidores
externos devem pedir uma operação de domínio ou uma leitura de telemetria, sem receber os objetos
internos usados para executá-la:

```text
 Editor / aplicação
        |
        +-- Settings() ----------------------> configuração + invalidação
        +-- UpdateMaterialTextureSlot(...) --> operação de domínio
        +-- GetGpuMemoryInfo() -------------> snapshot de telemetria
        +-- cena, seleção, câmera -----------> integração do editor
        |
        x-- Device / CommandQueue / UploadQueue / SRVHeap
                         não fazem parte da API pública
```

Essa regra impede que o editor grave comandos, manipule descriptors ou dependa da sequência de
inicialização do backend. Se uma nova tela precisar de dados, prefira um snapshot pequeno; se
precisar alterar recursos, crie uma operação com intenção explícita no dono apropriado.

Três cortes de ownership já existem. `FRenderBackend` contém a infraestrutura D3D12;
`FRendererSceneState` contém o estado CPU persistente da cena; `FRendererFrameState` contém o
histórico CPU que conecta frames consecutivos. `Renderer.h` mantém ponteiros para esses objetos e
os `.cpp` incluem apenas as definições que usam. Os cortes continuam por grupos coerentes; um PImpl
único com todo o renderer apenas esconderia as responsabilidades sem separá-las.

`Backend` concentra a integração com a API gráfica. `FRenderBackend` compõe e governa o lifecycle;
`Backend/D3D12` contém os wrappers concretos de baixo nível. A engine não possui hoje uma RHI
multi-API, então a estrutura assume D3D12 explicitamente em vez de prometer uma abstração que o
código ainda não oferece.

```text
 Renderer (fachada e orquestração)
       |
       +--> FRendererSceneState
       |    cena, câmera, seleção, bounds e versões CPU
       |
       +--> FRendererFrameState
       |    matrizes anteriores, jitter, relógios e contadores
       |
       +--> Graphics/Backend/RenderBackend
              ownership e lifecycle
                    |
                    v
              Graphics/Backend/D3D12
              device, filas, heaps e swapchain
                    |
                    v
              Direct3D 12
```

## Fluxo de controle

O editor produz input na thread Qt, mas toda gravação e submissão de GPU acontece na render thread:

```text
 Qt / ViewportWidget
        |
        | solicita frame + publica input/debug draw
        v
 RenderThread
        |
        +--> Renderer::RenderFrame()
        |       |
        |       +-- resolve estado imutável do frame
        |       +-- atualiza subsistemas
        |       +-- grava e submete command lists
        |       +-- agenda/consome readbacks
        |
        +--> libera o lock compartilhado
        |
        +--> Renderer::PresentFrame()
                |
                v
              DXGI
```

`PresentFrame()` permanece separado para que o dono da thread não mantenha o lock do renderer
durante o `Present` do DXGI.

## Anatomia de `RenderFrame`

O frame usa snapshots explícitos para evitar que cada passe reconstrua decisões de forma diferente:

```text
 dirty flags da UI / captura pendente
                 |
                 v
          CommandQueue::BeginFrame
                 |
                 v
   FEffectiveIndirectPolicy
                 |
                 +------> FFrameModes
                 |
       +---------+----------+
       |                    |
       v                    v
   FFrameView         FFrameLighting
       |                    |
       +---------+----------+
                 v
          FrameConstants
                 |
                 v
            FPassContext
                 |
       +---------+------------------------------+
       |         |         |         |          |
       v         v         v         v          v
    sombras   G-buffer   lighting  forward   resolve/post
                 |
                 v
        EndFrame / Execute / captura
```

- `FEffectiveIndirectPolicy` é resolvida uma vez e representa o que o hardware e os recursos
  realmente permitem naquele frame.
- `FFrameModes` deriva dessa política; não deve voltar a consultar toggles para recompor decisões.
- `FFrameView` e `FFrameLighting` congelam câmera, matrizes e luz principal para todos os passes.
- `FPassContext` contém referências não proprietárias para esses snapshots, targets e listas de
  draw. Sua validade termina junto com `RenderFrame()`.

A ordem exata dos blocos `Record*` está documentada em
[`ARCHITECTURE.md`, §5.2](ARCHITECTURE.md#52-renderframe--ordem-real-dos-passes).

## Filas direta e compute

DDGI pode sobrepor parte do raster na fila compute. A dependência é explícita:

```text
 fila direta                         fila compute
 ───────────                         ────────────
 estado inicial / ReGIR
        |
        +---- signal fence ---------> wait
        |                              |
 raster, sombras, G-buffer             +-- DDGI update
        |                              |
        +<------- wait fence ----------+
        |
 lighting / forward / post
```

Ao substituir recursos acessíveis pelas duas filas, drene primeiro a fila direta e depois a
compute. O trabalho compute pode estar esperando um fence da direta; inverter a ordem pode bloquear
o dreno. Um lock de CPU protege acesso entre threads, mas não encerra uso pela GPU.

## Ciclos de vida

### Inicialização

```text
 device e filas
   -> swapchain e heaps
   -> targets persistentes
   -> pipelines e subsistemas
   -> cena de ray tracing
   -> RegisterPasses
   -> Passes.ResizeAll
```

`RegisterPasses()` registra donos de pipeline e passes de frame. A ordem do registro também é a
ordem dos hooks de lifecycle, mas não define a ordem de execução do frame.

### Resize e mudança de escala

```text
 cancelar captura em aquecimento
   -> Flush da fila
   -> recriar targets centrais
   -> atualizar inputs dependentes de SRV
   -> Passes.ResizeAll
   -> remontar tabelas por cena
   -> registrar debug targets novamente
```

Hooks `OnResize` rodam depois dos targets centrais porque vários passes copiam seus slots de SRV.

### Mudança estrutural de cena

```text
 criar / remover / duplicar renderable
                 |
                 v
      OnSceneStructureChanged
          |                 |
          | cabe na folga   | excedeu capacidade
          v                 v
 RefreshInstanceGeo     rebuild TLAS e buffers
 RebuildMeshLights      SetupGIForScene completo
          |                 |
          +--------+--------+
                   v
       invalidar históricos afetados
```

Estruturas indexadas pela ordem da cena, como picking pendente e `PrevModels`, precisam ser
canceladas ou limpas antes de a nova ordem ser observada.

## Onde colocar uma mudança

```text
 A mudança implementa comportamento interno de um passe/subsistema?
   |
   +-- sim --> coloque no dono em Graphics/<Domínio>; Renderer apenas chama
   |
   +-- não --> altera lifecycle, resize, reload ou capacidade efetiva?
               |
               +-- sim --> Renderer.cpp
               |
               +-- não --> altera cena, câmera ou setup por cena?
                           |
                           +-- sim --> RendererScene.cpp
                           |
                           +-- não --> captura, readback ou debug target?
                                       |
                                       +-- sim --> RendererCapture.cpp
                                       |
                                       +-- não --> decisão/update CPU por frame?
                                                   |
                                                   +-- sim --> RendererFrame.cpp
                                                   |
                                                   +-- não --> grava comandos ou monta draws?
                                                               |
                                                               +-- sim --> RendererPasses.cpp
```

Evite adicionar lógica grande inline em `Renderer.h`. Tipos pequenos de snapshot podem morar em
`FrameContext.h` ou `PassContext.h`; tipos com recursos e lifecycle próprios devem virar um
subsistema.

## Invariantes essenciais

1. Recursos usados pela GPU não podem ser substituídos apenas sob proteção de lock de CPU.
2. Uploads, constant buffers e readbacks precisam respeitar os frames em voo.
3. Um estado efetivo do frame é resolvido uma vez e reutilizado por execução, telemetria e captura.
4. Mudanças que afetam sinais temporais passam por `FRenderSettings` e `HistoryDomain`.
5. Tabelas que copiam descriptors precisam ser remontadas quando o recurso de origem muda.
6. Um passe que recebe recurso em estado diferente deve fazer a transição ou declarar o contrato.
7. Captura determinística congela fase de animação, mas mantém convergência com delta fixo.
8. Comentários no código devem registrar somente contratos, invariantes, ownership ou ordem
   obrigatória. Contexto histórico e protocolos completos pertencem a `Docs/`.

## Validação de mudanças

No mínimo:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Mudanças de ordem, barriers, formatos, histórico ou shaders também precisam de validação visual no
editor. Para comparações reproduzíveis, siga [`CAPTURE-PROTOCOL.md`](CAPTURE-PROTOCOL.md).
