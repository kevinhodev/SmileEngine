# Motion vectors temporais confiaveis

Implementacao do capitulo 25 de *Ray Tracing Gems II* para os acumuladores de
iluminacao da SmileEngine. O objetivo e reprojetar o efeito visual que se move,
em vez de assumir que ele sempre acompanha a superficie primaria.

## Infraestrutura compartilhada

`FTemporalMotionVectors` mantem duas superficies full-resolution em ping-pong. Cada
pixel armazena a posicao em mundo e `InstanceID + 1`; zero representa ceu ou uma
intersecao invalida. Um `RayQuery` primario recupera o `InstanceID`, pois ele nao faz
parte do G-buffer raster atual.

Tambem existe um buffer versionado por frame com, para cada instancia da TLAS:

- `CurrentToPrevious`, para levar pontos e direcoes ao frame anterior;
- `PreviousToCurrent`, para avancar o oclusor ao frame atual.

O passe `DualMotion.cs.hlsl` parte do velocity raster, detecta quando a
correspondencia convencional caiu sobre um oclusor e aplica a Eq. 25.6 do capitulo.
O resultado (`R16G16_FLOAT`, convencao `currentUV - previousUV`) e usado por ReSTIR
GI, ReSTIR DI e pelos packs do NRD.

O TAA e os upscalers continuam lendo o velocity raster. Copiar cor com o vetor dual
produz repeticao visivel; ele fica restrito aos caminhos que reutilizam amostras ou
radiancia de iluminacao.

## Sombras

O resolve espacial do ReSTIR DI conhece o receptor, o bloqueador encontrado pelo
shadow ray e a luz escolhida. Esses tres pontos sao levados ao frame anterior e a
reta luz-bloqueador e intersectada com o plano local do receptor. A distancia angular
do ponto real anterior ao plano gera uma confianca gaussiana equivalente a Eq. 25.4.

O pack direto do NRD combina esse vetor de sombra com o vetor dual. Quando nao ha
bloqueador, historico valido ou confianca suficiente, permanece o movimento base.

## Reflexos glossy

Os traces half-resolution e mirror full-resolution levam o receptor e o hit
secundario ao frame anterior. O hit e espelhado no plano receptor, observado pela
camera anterior e perturbado por uma amostra gaussiana proporcional a rugosidade e
distancia, aproximando a Eq. 25.5. O resolve propaga o vetor da amostra escolhida para
full-resolution.

O acumulador temporal proprio de reflexos usa esse vetor antes da reprojecao
especular deterministica e reduz o comprimento de historico conforme a confianca de
planaridade. No caminho NRD/Ray Reconstruction, o motion guide continua compartilhado
com os demais sinais; hit distance e os mecanismos do denoiser permanecem ativos.

## Ciclo de vida

- `SetupForScene`: cria transforms por instancia e zera o historico.
- `SetupForResize`: recria superficies, motion target e tabelas de descriptors.
- `UpdatePerFrame`: grava transforms e constantes do slot atual.
- `Record`: captura a superficie atual e calcula o vetor dual.
- alteracoes de cena, material RT ou iluminacao indireta invalidam o historico.

As tabelas e recursos sao versionados pelos dois frames em voo. O primeiro frame
apos qualquer invalidacao usa apenas o velocity raster.
