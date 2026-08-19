# Geração de malhas 3D por IA — prototipagem rápida

> **O que este documento cobre:** o caminho de um *prompt* até um objeto na cena aberta do
> SmileEditor, e o que cada peça dele faz. Escrito a partir do código; quando divergir dele, o
> código vence.
>
> **Última verificação contra o código: 2026-08-19.**

---

## 1. Para que serve

Bloquear a iteração de rendering esperando arte é o gargalo clássico de uma engine pessoal:
testar sombra de contato, GI numa quina, mesh light ou o comportamento de um material precisa de
*algo* na cena, e o "algo" costuma virar mais um cubo. Um gerador 3D por IA resolve exatamente
esse pedaço — não substitui arte final, mas produz em minutos uma malha com silhueta, volume e
textura plausíveis, que é o suficiente para um teste de rendering ser honesto.

O ciclo completo, sem sair do editor:

```
  prompt / imagem
        │
        ▼
  ┌──────────────────┐   HTTPS   ┌─────────────────────┐
  │ SmileMCP         │──────────►│ provedor de IA      │
  │ (meshgen.ts)     │◄──────────│ Meshy / Tripo / local│
  └────────┬─────────┘   .glb    └─────────────────────┘
           │  Assets/Generated/<slug>/<slug>.glb
           ▼
  ┌──────────────────┐
  │ SmileCooker      │  importador glTF → weld, RH→LH, payload de RT,
  │ (GltfImport.cpp) │  extração das texturas embutidas, normalização
  └────────┬─────────┘
           │  <slug>.smesh + <slug>.sscene + Textures/
           ▼
  ┌──────────────────┐  named pipe   ┌──────────────────────┐
  │ SmileMCP         │──────────────►│ SmileEditor (McpBridge)│
  │ smile_load_scene │   load_scene  │ carga ADITIVA na cena │
  └──────────────────┘               └──────────────────────┘
```

A engine não sabe que há IA no caminho: ela recebe um `.glb` como receberia de qualquer DCC.

---

## 2. Uso

### Pelo agente (MCP)

```jsonc
// 1) o que está configurado? (não gasta chamada paga)
smile_meshgen_providers {}

// 2) gerar, cozinhar e colocar na cena aberta
smile_generate_mesh {
  "name": "tocho medieval",
  "provider": "meshy",
  "prompt": "tocho medieval de ferro com pano enrolado, estilo realista",
  "fitMeters": 2,          // malha gerada chega sem unidade; 2 m = altura de um poste
  "dropToGround": true,
  "centerXZ": true,
  "loadInEditor": true     // exige um SmileEditor aberto com renderer pronto
}
```

O resultado traz o log da geração, o resumo do cozimento (contagens do `.smesh`/`.sscene`) e o
retorno da carga.

### Pelo terminal

```powershell
cd Tools\SmileMCP
npm run build
npm run meshgen -- --name tocho --prompt "tocho medieval de ferro" --load
npm run meshgen -- --providers          # lista provedores e sai
```

### Só cozinhar um `.glb` que você já tem

```powershell
build\bin\Release\SmileCooker.exe Assets\Generated\tocho\tocho.glb --fit 2 --ground --center
```

---

## 3. Provedores

| id | serviço | texto→3D | imagem→3D | chave |
|----|---------|:--------:|:---------:|-------|
| `meshy` | [Meshy](https://docs.meshy.ai) | sim | sim | `SMILE_MESHGEN_MESHY_KEY` |
| `tripo` | [Tripo3D](https://platform.tripo3d.ai/docs) | sim | — ¹ | `SMILE_MESHGEN_TRIPO_KEY` |
| `local` | serviço self-hosted (Hunyuan3D, TRELLIS, wrapper de ComfyUI) | sim | sim | opcional (`SMILE_MESHGEN_LOCAL_KEY`) |

¹ imagem no Tripo exige um passo de upload com resposta própria, que ainda não existe aqui. O
provedor declara `supports.image: false` em vez de falhar no meio do polling.

**Os provedores são dado, não código.** Endpoint e nome de campo mudam com a versão da API de cada
serviço, então os dois lados são ajustáveis sem recompilar:

| variável | efeito |
|----------|--------|
| `SMILE_MESHGEN_<PROVEDOR>_KEY` | chave de API. Lida **só** do ambiente do processo — nunca de arquivo do projeto, nunca de argumento de linha de comando (ficaria no histórico do shell e na lista de processos). |
| `SMILE_MESHGEN_<PROVEDOR>_URL` | sobrescreve o endpoint base. É o que salva o dia quando um serviço muda de `/v2/` para `/v3/`. |

Se um provedor mudar o **formato** da resposta (e não só o endpoint), o ajuste é a entrada dele em
`Tools/SmileMCP/src/meshgen.ts` — `buildSubmit`, `readTaskId` e `readStatus`, três funções curtas.

### Contrato do provedor `local`

Mínimo para um serviço self-hosted funcionar sem adaptador:

```
POST /generate   { prompt, image?, negative_prompt?, format: "glb" }
                 -> { task_id }         ou já { model_url }  (síncrono)
GET  /task/<id>  -> { status: "processing" | "done" | "failed", progress?, model_url? }
```

---

## 4. O que o Cooker faz com uma malha gerada

O importador é `Tools/Cooker/GltfImport.cpp`; o que ele compartilha com o caminho FBX (weld,
layout do blob, payload de RT, TRS do renderável) mora em `CookedWriter.h`.

- **RH → LH.** glTF é RH Y-up, como o FBX normalizado: nega Z e inverte o winding. O par é
  obrigatório — só um dos dois deixa a cena *inside-out*.
- **UV não é invertida.** Diferente do FBX: o glTF já põe a origem da UV no canto superior
  esquerdo, igual ao D3D.
- **Sem `NORMAL` → flat shading**, como a spec manda (normal de face, sem média entre faces
  vizinhas).
- **Texturas embutidas** (o caso normal de asset gerado) são extraídas para `Textures/` ao lado do
  cozido, com o stem do asset no nome para dois assets na mesma pasta não se sobrescreverem.
  `metallicRoughness` do glTF já é o packing que o slot `Specular` da engine espera (G=roughness,
  B=metalness).
- **Instancing.** Dois nós apontando para a mesma malha viram duas instâncias de uma geometria,
  igual à dedup da v7 no caminho FBX.
- **Normalização de escala/pivot** (§5).

### O que ele recusa, alto

Nunca cozinha pela metade — em todos estes casos nada é escrito e a mensagem diz o motivo:

| entrada | por quê |
|---------|---------|
| `KHR_draco_mesh_compression`, `EXT_meshopt_compression` | mudam o significado dos bufferViews; sem o decodificador, o que sairia seria ruído com cara de malha. **Reexporte sem compressão de malha.** |
| accessor `sparse` | altera valores individuais depois do bufferView; ignorar daria geometria silenciosamente errada |
| accessor sem `bufferView` | valeria zero por spec — uma malha colapsada na origem |
| índice fora da faixa, accessor lendo além do bufferView, GLB truncado | arquivo corrompido |
| nenhuma primitiva `TRIANGLES` utilizável | não há o que renderizar |

Primitivas isoladas em modo `POINTS`/`LINES` são puladas com aviso; só a ausência **total** de
triângulos é erro.

### O que ele não faz

- **Cor por vértice** (`COLOR_0`): o `Vertex` da engine tem 32 bytes (pos3, normal3, uv2) e não
  carrega cor. Malha gerada só com vertex color sai na cor do fator do material.
- **Skinning e animação**: não há runtime para isso na engine.
- **Shading model de folhagem**: no FBX ele é ligado por heurística de nome do Bistro, e nome de
  malha gerada não carrega essa convenção. Ajuste pelo editor de materiais se precisar.

---

## 5. Normalização de escala e pivot

Malha gerada por IA **não tem unidade**. Um serviço devolve o modelo dentro de um cubo unitário,
outro em centímetros, outro com o pivot no centro do volume. Numa cena em metros isso vira um
objeto microscópico ou enterrado no chão — e corrigir na mão a cada iteração mata o ciclo rápido
que o gerador existe para dar.

| flag do Cooker | campo MCP | efeito |
|----------------|-----------|--------|
| `--fit <metros>` | `fitMeters` | maior dimensão da cena vira N metros |
| `--ground` | `dropToGround` | encosta o ponto mais baixo em Y=0 |
| `--center` | `centerXZ` | centraliza o volume em X/Z na origem |

É uma **similaridade aplicada em mundo** (escala uniforme + translação), não um rebake dos
vértices: com escala uniforme `s` e deslocamento `t`,

```
S_local * R * T_pos * S(s) * T(t)  ==  S(Scale*s) * R * T(Pos*s + t)
```

porque `S(s)` uniforme comuta com `R`. Cada renderável ajusta só `Position` e `Scale`; a
geometria — e portanto o payload de RT e a dedup por malha — fica intacta. As flags valem para
os dois importadores e vêm **desligadas** por default; o `smile_generate_mesh` liga as três,
porque ali a malha é sempre um asset avulso sem unidade.

---

## 6. Onde os arquivos ficam

```
Assets/Generated/<slug>/
├── <slug>.glb              baixado do provedor
├── <slug>.meshgen.json     procedência: provedor, prompt, task id, data
├── <slug>.smesh            geometria cozida (v8)
├── <slug>.sscene           materiais + renderáveis
└── Textures/<slug>_img*.png   imagens extraídas do .glb
```

`Assets/Generated/` é **ignorado pelo git**: é derivado de um prompt, não fonte. O
`<slug>.meshgen.json` existe justamente para o asset não virar um binário sem história — ele diz
qual prompt e qual provedor o produziram. Se um asset gerado virar permanente, mova-o para
`Assets/Scenes/` e trate-o como qualquer outro asset importado.

---

## 7. Segurança

Esta é a única rota do projeto que sai para a internet. As regras que o código impõe:

- **Só `https`**, exceto `http` em loopback (o provedor `local`). Endpoint http remoto mandaria a
  chave de API em claro.
- **A chave nunca acompanha o download do modelo.** A URL do `.glb` costuma ser assinada e apontar
  para outro host (S3 e afins); mandar o header de autorização junto vazaria a chave para lá.
- **O conteúdo baixado é conferido antes de virar asset** — o modo de falha comum destes serviços
  é uma página de erro entregue com HTTP 200, que chegaria ao Cooker como se fosse malha.
- **Teto de 256 MB** por modelo, **8 MiB** por imagem de referência.
- **`name` vira slug** (`[a-z0-9-]`, 48 caracteres): é nome de diretório, e é a única barreira
  entre um nome vindo do agente e um caminho no disco.
- O `.gltf` é tratado como **entrada hostil** no Cooker: limite de profundidade no parser, bounds
  checados em todo accessor, e URIs externos recusados quando são absolutos, remotos ou saem do
  diretório do asset.

---

## 8. Testes

| o quê | como rodar |
|-------|-----------|
| Importador glTF (9 grupos: RH→LH, UV, normal de face vs. winding, `byteStride`, hierarquia/instancing, material, normalização, recusas) | `ctest -R Smile.GltfImport` |
| Pipeline de geração contra um provedor falso em loopback (submissão, polling, download, recusas) | `cd Tools/SmileMCP && npm run smoke:meshgen` |
| Superfície do servidor MCP | `cd Tools/SmileMCP && npm run smoke` |

Nenhum deles chama serviço real: não há chave, não há custo, não há rede externa.

---

## 9. Limitações conhecidas

- **Qualidade de malha.** O que estes serviços devolvem costuma ter topologia ruim (triângulos
  finos, ilhas de UV distorcidas) e uma contagem de triângulos alta demais para o que a silhueta
  entrega. Serve para teste de rendering; não serve como asset final.
- **Sem LOD, sem lightmap UV.** A engine não tem os dois, então não faz falta hoje — mas é o que
  primeiro vai faltar se um asset gerado virar permanente.
- **A carga aditiva não persiste.** O objeto entra na cena da sessão; o `.smap` da cena base não
  passa a referenciá-lo. Para tornar permanente, adicione a cena pelo menu (`Arquivo → Adicionar
  Cena`) na cena que você vai salvar.
- **Uma geração por vez** por bridge: o `load_scene` recusa enquanto outra carga ou uma captura
  estiver em andamento.
