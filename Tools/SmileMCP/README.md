# SmileMCP

> [!NOTE]
> **Estado:** MVP ativo · **Plataforma:** Windows
> As ferramentas de bridge exigem uma build recente do SmileEditor. Para o contrato das capturas,
> consulte o [protocolo de captura](../../Docs/CAPTURE-PROTOCOL.md).

Servidor MCP local da SmileEngine. O transporte inicial é STDIO: o cliente inicia o processo,
descobre as ferramentas e recebe resultados estruturados sem acoplar o protocolo ao runtime C++.

## Estado do MVP

Ferramentas disponíveis:

- `smile_project_info`: versão, componentes, builds e executáveis encontrados.
- `smile_list_files`: navegação limitada pela árvore do projeto.
- `smile_read_file`: leitura paginada de arquivos texto.
- `smile_find_text`: pesquisa no código com `rg`.
- `smile_get_latest_logs`: trecho final dos logs persistentes do editor.
- `smile_build`: build de um alvo CMake.
- `smile_compile_shaders`: atalho para o alvo `Shaders`.
- `smile_cook_scene`: recozinha um FBX e valida os cabeçalhos `.smesh`/`.sscene` gerados.
- `smile_editor_status`: consulta PID, executável, commit, prontidão, cena e captura do editor vivo.
- `smile_profile_configure`: fixa o regime de render, a hora (`10:00` por default ou
  `timeOfDayHours`) e opcionalmente a câmera do teste.
- `smile_profile_gpu`: amostra timestamps brutos e a EMA do Mini Profiler, com percentis e VRAM.
- `smile_close_editor`: encerra o editor pelo bridge, esperando o desligamento da render thread.
- `smile_run_editor`: inicia um `SmileEditor.exe`, aceita uma `.sscene` tipada e aguarda o
  renderer ficar pronto.
- `smile_capture_frame`: captura o viewport do editor aberto e retorna PNG, manifesto e,
  opcionalmente, a imagem embutida na resposta MCP.

Os acessos a arquivos aceitam somente caminhos relativos que permanecem dentro da raiz da
SmileEngine. Processos são iniciados sem shell, e nomes de alvo CMake são validados.

## Fluxo de validação

O ciclo completo pode ser feito sem voltar ao terminal:

1. `smile_build` para `SmileCooker`, `Shaders` e `SmileEditor`, conforme o escopo da mudança.
2. `smile_cook_scene` com o caminho relativo do FBX.
3. `smile_run_editor` com `scenePath`; por default ele espera até 90 s pelo renderer e confirma
   que a cena pedida, não apenas algum editor, ficou pronta.
4. `smile_capture_frame` para publicar PNG e manifesto.

`smile_run_editor` detecta uma instância já conectada para não abrir dois editores apontando para
a mesma named pipe. Se a instância existente estiver com outra cena, a ferramenta falha
explicitamente em vez de capturar silenciosamente o viewport errado.

## Requisitos

- Node.js 20 ou mais recente.
- `rg` (ripgrep) no `PATH` para `smile_find_text`.
- CMake no `PATH` para as ferramentas de build.
- Uma árvore de build já configurada em `build/`.
- Uma build recente do `SmileEditor` para as ferramentas que usam a named pipe local.

## Preparação

No PowerShell:

```powershell
cd D:\Engines\SmileEngine\Tools\SmileMCP
npm install
npm run build
npm run smoke
```

O servidor encontra a raiz subindo a partir do diretório atual. Para eliminar ambiguidade, defina
`SMILE_ROOT`; `SMILE_LOG_DIR` pode sobrescrever o diretório dos logs persistentes.
`SMILE_MCP_PIPE` pode trocar o nome da named pipe nos dois processos; o default é
`SmileEngine-MCP-v1`.

## Registro no Codex

Copie a secao de [`mcp-config.example.toml`](mcp-config.example.toml) para o `.codex/config.toml`
do projeto ou adicione pela interface de MCP servers. Depois reinicie o cliente local para que o
novo processo seja descoberto.

O exemplo usa `default_tools_approval_mode = "writes"`: consultas read-only podem fluir, enquanto
build e execução continuam sujeitos à aprovação do cliente.

## Bridge do editor

O MCP opera do lado de fora do processo. Um bridge local no editor usa protocolo versionado e
mensagens JSON para expor estado vivo sem incorporar o SDK MCP ao C++:

```text
Codex <-> SmileMCP (STDIO) <-> bridge local <-> SmileEditor/Renderer
```

Os comandos vivos atuais cobrem status, captura, configuração/snapshot de profiling e desligamento.
`McpBridge` fica restrito a validar e traduzir o protocolo; acesso sincronizado ao renderer,
aplicação dos presets e snapshots tipados moram no `RenderSettingsController`. O mesmo controlador
notifica os bridges QML depois de uma mutação externa, evitando que a engine mude pela pipe e a UI
continue exibindo valores antigos.

Entidades e render graph ainda são extensões futuras. Novas operações mutáveis e scripting devem
entrar somente com allowlist, timeout e identificador de sessão. `smile_capture_frame` mantém a
named pipe aberta durante o aquecimento e responde apenas depois que PNG + manifesto foram
publicados.
