# Motion vectors temporais confiáveis

> [!NOTE]
> **Tipo:** nota técnica · **Estado:** implementação atual
> Contexto adicional: [arquitetura do frame](ARCHITECTURE.md#5-o-frame-render-loop-e-frame-graph)
> e [protocolo de captura](CAPTURE-PROTOCOL.md).

Implementação do capítulo 25 de *Ray Tracing Gems II* para os acumuladores de
iluminação da SmileEngine. O objetivo é reprojetar o efeito visual que se move,
em vez de assumir que ele sempre acompanha a superfície primária.

## Infraestrutura compartilhada

`FTemporalMotionVectors` mantém duas superfícies full-resolution em ping-pong. Cada
pixel armazena a posição em mundo e `InstanceID + 1`; zero representa céu ou uma
interseção inválida. Um `RayQuery` primário recupera o `InstanceID`, pois ele não faz
parte do G-buffer raster atual.

Também existe um buffer versionado por frame com, para cada instância da TLAS:

- `CurrentToPrevious`, para levar pontos e direções ao frame anterior;
- `PreviousToCurrent`, para avançar o oclusor ao frame atual.

O passe `DualMotion.cs.hlsl` parte do velocity raster, detecta quando a
correspondência convencional caiu sobre um oclusor e aplica a Eq. 25.6 do capítulo.
O resultado (`R16G16_FLOAT`, convenção `currentUV - previousUV`) é usado por ReSTIR
GI, ReSTIR DI e pelos packs do NRD.

O TAA e os upscalers continuam lendo o velocity raster. Copiar cor com o vetor dual
produz repetição visível; ele fica restrito aos caminhos que reutilizam amostras ou
radiância de iluminação.

## Sombras

O resolve espacial do ReSTIR DI conhece o receptor, o bloqueador encontrado pelo
shadow ray e a luz escolhida. Esses três pontos são levados ao frame anterior e a
reta luz-bloqueador é intersectada com o plano local do receptor. A distância angular
do ponto real anterior ao plano gera uma confiança gaussiana equivalente à Eq. 25.4.

O pack direto do NRD combina esse vetor de sombra com o vetor dual. Quando não há
bloqueador, histórico válido ou confiança suficiente, permanece o movimento base.

## Reflexos glossy

Os traces half-resolution e mirror full-resolution levam o receptor e o hit
secundário ao frame anterior. O hit é espelhado no plano receptor, observado pela
câmera anterior e perturbado por uma amostra gaussiana proporcional à rugosidade e
distância, aproximando a Eq. 25.5. O resolve propaga o vetor da amostra escolhida para
full-resolution.

O acumulador temporal próprio de reflexos usa esse vetor antes da reprojeção
especular determinística e reduz o comprimento de histórico conforme a confiança de
planaridade. No caminho NRD/Ray Reconstruction, o motion guide continua compartilhado
com os demais sinais; hit distance e os mecanismos do denoiser permanecem ativos.

## Ciclo de vida

- `SetupForScene`: cria transforms por instância e zera o histórico.
- `SetupForResize`: recria superfícies, motion target e tabelas de descriptors.
- `UpdatePerFrame`: grava transforms e constantes do slot atual.
- `Record`: captura a superfície atual e calcula o vetor dual.
- alterações de cena, material RT ou iluminação indireta invalidam o histórico.

As tabelas e recursos são versionados pelos dois frames em voo. O primeiro frame
após qualquer invalidação usa apenas o velocity raster.
