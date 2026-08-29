# Opacity Micro-Maps (DXR 1.2)

SmileEngine exige o caminho completo de Opacity Micro-Maps (OMM). Nao existe backend
raster-only nem BLAS alpha-tested legado: a inicializacao falha se o dispositivo nao expuser
DXR Tier 1.2 e Shader Model 6.9, e uma cena masked falha se nao produzir um OMM valido.

## Requisitos

- Windows 10 1909 (build 18363) ou posterior, ou Windows 11.
- GPU NVIDIA RTX 30 ou posterior, conforme a politica da engine.
- Driver NVIDIA 595 ou posterior. O driver 616.56 satisfaz o requisito.
- DirectX 12 Agility SDK 1.619.5 em `D:/Engines/AgilitySDK-1.619.5`.
- DXC 1.9.2602.24 em `D:/Engines/DXC-1.9.2602.24`.
- NVIDIA Opacity Micro-Map SDK 1.9.2 em `D:/Engines/OMM`.
- DirectXTex (tag `may2026`) em `D:/Engines/DirectXTex`.

Os caminhos podem ser substituidos no configure do CMake com
`SMILE_AGILITY_SDK_ROOT`, `SMILE_DXC_ROOT`, `SMILE_OMM_SDK_ROOT` e
`SMILE_DIRECTXTEX_ROOT`.

O build copia `D3D12Core.dll` e `d3d12SDKLayers.dll` para `D3D12/` ao lado do
executavel. `D3D12SDKVersion=619` e `D3D12SDKPath=.\\D3D12\\` sao exportados pelo
executavel, portanto o runtime nao depende da versao de D3D12 distribuida pelo sistema.

## Fluxo

1. O importador preserva o mip 0 da textura base-color na CPU durante o commit da cena.
2. Cada par mesh/material masked e enviado ao baker CPU oficial da NVIDIA.
3. O baker gera OMM 4-state, com sampler linear/wrap, subdivisao dinamica ate nivel 8 e
   orcamento de 2^30 testes. UVs patologicas acima do limite recebem indices especiais
   `FULLY_UNKNOWN_OPAQUE`: o BLAS continua OMM e o alpha-test exato resolve esses candidatos.
4. A engine envia dados, descritores e indices para a GPU e constroi primeiro os OMM Arrays.
5. Uma barreira UAV global separa os arrays dos BLAS `OMM_TRIANGLES` que os referenciam.
6. Os BLAS sao compactados e a TLAS seleciona a variante correta por par mesh/OMM.
7. Todos os `RayQuery` usam `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` e shaders SM 6.9.

Microtriangulos conhecidos como opacos ou transparentes sao resolvidos pelo hardware. Somente
o estado `UNKNOWN` chega ao `AlphaTestPass`, preservando o resultado exato da textura sem pagar
o custo do alpha-test em todos os candidatos. Isso e parte do caminho OMM 4-state, nao um
fallback para geometria sem OMM.

O cutoff usa a mesma semantica `alpha >= cutoff` do raster/HLSL. O baker da NVIDIA compara com
`>`, entao o valor enviado e deslocado por um ULP para preservar a igualdade.

## Invariantes atuais

- O bake pertence ao par mesh/material; uma mesma mesh pode gerar varios BLAS.
- `AlphaTest`, alpha do `BaseColorFactor` e `AlphaCutoff` usados no build devem continuar iguais
  durante a vida da cena. Se forem editados, a engine acusa OMM desatualizado e exige recarregar
  a cena para rebake, em vez de usar um resultado incorreto ou cair no BLAS antigo.
- Trocar a textura base-color de um material masked tambem exige recarregar a cena.
- Materiais opacos continuam usando triangulos `OPAQUE`; a obrigatoriedade de OMM se aplica a
  toda geometria alpha-tested.

## Referencias

- [Microsoft: DirectX Raytracing 1.2 and Opacity Micromaps](https://devblogs.microsoft.com/directx/d3d12-opacity-micromaps/)
- [Microsoft: Shader Model 6.9 retail and DXR 1.2](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/)
- [DirectX Raytracing specification](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
- [Microsoft D3D12 OMM sample](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingOpacityMicromaps)
- [NVIDIA Opacity Micro-Map SDK](https://github.com/NVIDIA-RTX/OMM)
- [NVIDIA OMM integration guide](https://github.com/NVIDIA-RTX/OMM/blob/main/docs/integration_guide.md)
