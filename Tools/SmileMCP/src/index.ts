#!/usr/bin/env node

import { spawn } from "node:child_process";
import { readFile, stat } from "node:fs/promises";
import path from "node:path";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import {
  SmileEditorBridge,
  type ProfileOverrides,
  type ProfileSnapshot,
} from "./editor-bridge.js";
import { PROVIDER_IDS, describeProviders, generateMesh, slugify } from "./meshgen.js";
import { SmileProject, discoverSmileRoot } from "./smile-project.js";

const VERSION = "0.3.0";
const project = new SmileProject(discoverSmileRoot());
const editorBridge = new SmileEditorBridge(project.root);

const server = new McpServer(
  { name: "smile-mcp", version: VERSION },
  {
    instructions:
      "Ferramentas locais da SmileEngine. Prefira primeiro as operacoes read-only de inspecao. " +
      "Build e execucao operam apenas na raiz configurada por SMILE_ROOT. Compile Shaders para " +
      "mudancas somente em HLSL e SmileEditor quando interfaces C++/HLSL tambem mudarem. " +
      "Use smile_cook_scene para regenerar os cozidos e smile_run_editor com scenePath para " +
      "aguardar a cena ficar pronta antes de smile_capture_frame. Para benchmarks, configure " +
      "um regime deterministico com smile_profile_configure antes de smile_profile_gpu. " +
      "Para prototipagem rapida, smile_generate_mesh gera uma malha por IA, cozinha e (com " +
      "loadInEditor) a coloca na cena aberta; smile_meshgen_providers diz quais provedores " +
      "estao configurados antes de gastar uma chamada paga.",
  },
);

function jsonResult(value: unknown) {
  return {
    content: [{ type: "text" as const, text: JSON.stringify(value, null, 2) }],
  };
}

function errorResult(error: unknown) {
  const message = error instanceof Error ? error.message : String(error);
  return {
    isError: true,
    content: [{ type: "text" as const, text: message }],
  };
}

function sameWindowsPath(left: string, right: string): boolean {
  if (!left || !right) return false;
  const normalize = (value: string): string => path.resolve(value).toLocaleLowerCase("en-US");
  return normalize(left) === normalize(right);
}

function summarize(values: number[]): Record<string, number> {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (sorted.length === 0) return { count: 0 };
  const percentile = (p: number): number => {
    const index = (sorted.length - 1) * p;
    const low = Math.floor(index);
    const high = Math.ceil(index);
    const weight = index - low;
    return sorted[low]! * (1 - weight) + sorted[high]! * weight;
  };
  const mean = sorted.reduce((sum, value) => sum + value, 0) / sorted.length;
  const variance =
    sorted.reduce((sum, value) => sum + (value - mean) ** 2, 0) / sorted.length;
  return {
    count: sorted.length,
    min: sorted[0]!,
    p10: percentile(0.1),
    median: percentile(0.5),
    mean,
    p90: percentile(0.9),
    p95: percentile(0.95),
    max: sorted.at(-1)!,
    stdDev: Math.sqrt(variance),
  };
}

function summarizeProfile(snapshots: ProfileSnapshot[], includeSamples: boolean) {
  const scopes = new Map<
    string,
    { name: string; queue: string; depth: number; raw: number[]; ema: number[] }
  >();
  for (const snapshot of snapshots) {
    for (const timing of [...snapshot.direct, ...snapshot.asyncCompute]) {
      const key = `${timing.queue}\u0000${timing.depth}\u0000${timing.name}`;
      let scope = scopes.get(key);
      if (!scope) {
        scope = {
          name: timing.name,
          queue: timing.queue,
          depth: timing.depth,
          raw: [],
          ema: [],
        };
        scopes.set(key, scope);
      }
      scope.raw.push(timing.rawMilliseconds);
      scope.ema.push(timing.milliseconds);
    }
  }
  const passSummaries = [...scopes.values()]
    .map((scope) => ({
      name: scope.name,
      queue: scope.queue,
      depth: scope.depth,
      rawMilliseconds: summarize(scope.raw),
      emaMilliseconds: summarize(scope.ema),
    }))
    .sort(
      (left, right) =>
        ((right.rawMilliseconds.median as number | undefined) ?? 0) -
        ((left.rawMilliseconds.median as number | undefined) ?? 0),
    );

  return {
    sampleCount: snapshots.length,
    firstFrameIndex: snapshots[0]?.frameIndex ?? null,
    lastFrameIndex: snapshots.at(-1)?.frameIndex ?? null,
    gpu: snapshots[0]?.gpu ?? null,
    resolution: snapshots[0]
      ? {
          outputWidth: snapshots[0].outputWidth,
          outputHeight: snapshots[0].outputHeight,
          renderWidth: snapshots[0].renderWidth,
          renderHeight: snapshots[0].renderHeight,
        }
      : null,
    cpuFps: summarize(snapshots.map((snapshot) => snapshot.cpuFps)),
    localVramBytes: summarize(
      snapshots.map((snapshot) => snapshot.vram.localUsageBytes),
    ),
    settings: snapshots[0]?.settings ?? null,
    passes: passSummaries,
    ...(includeSamples ? { samples: snapshots } : {}),
  };
}

server.registerTool(
  "smile_project_info",
  {
    title: "Smile project info",
    description:
      "Retorna versao, componentes, configuracoes de build, executaveis e contagens basicas da SmileEngine.",
    inputSchema: {},
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async () => {
    try {
      return jsonResult(await project.projectInfo());
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_list_files",
  {
    title: "List Smile files",
    description:
      "Lista arquivos e diretorios dentro da SmileEngine, com profundidade e quantidade limitadas.",
    inputSchema: {
      relativePath: z.string().default(".").describe("Caminho relativo a raiz da SmileEngine."),
      maxDepth: z.number().int().min(0).max(8).default(2),
      limit: z.number().int().min(1).max(500).default(200),
      includeBuild: z.boolean().default(false),
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      return jsonResult(
        await project.listFiles({
          relativePath: input.relativePath,
          maxDepth: input.maxDepth,
          limit: input.limit,
          includeBuild: input.includeBuild,
        }),
      );
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_read_file",
  {
    title: "Read Smile file",
    description: "Le um trecho numerado de um arquivo texto dentro da SmileEngine.",
    inputSchema: {
      relativePath: z.string().min(1).describe("Caminho relativo do arquivo."),
      startLine: z.number().int().min(1).default(1),
      maxLines: z.number().int().min(1).max(1000).default(250),
      includeLineNumbers: z.boolean().default(true),
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      return jsonResult(
        await project.readTextFile({
          relativePath: input.relativePath,
          startLine: input.startLine,
          maxLines: input.maxLines,
          includeLineNumbers: input.includeLineNumbers,
        }),
      );
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_find_text",
  {
    title: "Search Smile source",
    description: "Pesquisa texto no projeto com ripgrep e retorna ocorrencias com linha e coluna.",
    inputSchema: {
      query: z.string().min(1),
      relativePath: z.string().default("."),
      globs: z.array(z.string()).max(10).default([]),
      caseSensitive: z.boolean().default(false),
      fixedStrings: z.boolean().default(true),
      includeBuild: z.boolean().default(false),
      maxResults: z.number().int().min(1).max(500).default(100),
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      return jsonResult(
        await project.findText({
          query: input.query,
          relativePath: input.relativePath,
          globs: input.globs,
          caseSensitive: input.caseSensitive,
          fixedStrings: input.fixedStrings,
          includeBuild: input.includeBuild,
          maxResults: input.maxResults,
        }),
      );
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_get_latest_logs",
  {
    title: "Read Smile editor logs",
    description: "Retorna as linhas finais dos logs de sessao mais recentes do SmileEditor.",
    inputSchema: {
      sessions: z.number().int().min(1).max(5).default(1),
      tailLines: z.number().int().min(1).max(1000).default(200),
      contains: z.string().min(1).optional().describe("Filtro textual opcional."),
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      return jsonResult(await project.latestLogs(input.sessions, input.tailLines, input.contains));
    } catch (error) {
      return errorResult(error);
    }
  },
);

const buildSchema = {
  configuration: z.enum(["Debug", "Release", "RelWithDebInfo"]).default("Debug"),
  buildDirectory: z.string().default("build"),
  timeoutSeconds: z.number().int().min(10).max(1800).default(900),
};

server.registerTool(
  "smile_build",
  {
    title: "Build Smile target",
    description: "Compila um alvo CMake permitido da SmileEngine e retorna stdout, stderr e exit code.",
    inputSchema: {
      ...buildSchema,
      target: z
        .string()
        .regex(/^[A-Za-z0-9_.+-]+$/)
        .default("SmileEditor")
        .describe("Nome de um unico alvo CMake; nenhum argumento de shell e aceito."),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const result = await project.build({
        buildDirectory: input.buildDirectory,
        configuration: input.configuration,
        target: input.target,
        timeoutSeconds: input.timeoutSeconds,
      });
      return result.exitCode === 0 ? jsonResult(result) : { ...jsonResult(result), isError: true };
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_compile_shaders",
  {
    title: "Compile Smile shaders",
    description: "Compila o alvo CMake Shaders na configuracao escolhida.",
    inputSchema: buildSchema,
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const result = await project.build({
        buildDirectory: input.buildDirectory,
        configuration: input.configuration,
        target: "Shaders",
        timeoutSeconds: input.timeoutSeconds,
      });
      return result.exitCode === 0 ? jsonResult(result) : { ...jsonResult(result), isError: true };
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_cook_scene",
  {
    title: "Cook a Smile scene",
    description:
      "Recozinha um FBX ou glTF/GLB com o SmileCooker e valida os cabecalhos .smesh/.sscene " +
      "produzidos ao lado da fonte.",
    inputSchema: {
      sourcePath: z
        .string()
        .min(1)
        .describe("Caminho relativo de um .fbx/.gltf/.glb dentro da raiz da SmileEngine."),
      configuration: z.enum(["Debug", "Release", "RelWithDebInfo"]).default("Release"),
      opaqueGlass: z.boolean().default(false),
      timeoutSeconds: z.number().int().min(10).max(1800).default(900),
      fitMeters: z
        .number()
        .min(0.01)
        .max(1000)
        .optional()
        .describe("Escala a cena para caber num cubo deste tamanho. Para malha sem unidade definida."),
      dropToGround: z.boolean().default(false),
      centerXZ: z.boolean().default(false),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const result = await project.cookScene({
        sourcePath: input.sourcePath,
        configuration: input.configuration,
        opaqueGlass: input.opaqueGlass,
        timeoutSeconds: input.timeoutSeconds,
        fitMeters: input.fitMeters,
        dropToGround: input.dropToGround,
        centerXZ: input.centerXZ,
      });
      return result.exitCode === 0 ? jsonResult(result) : { ...jsonResult(result), isError: true };
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_editor_status",
  {
    title: "Smile editor status",
    description:
      "Consulta o editor vivo pela named pipe e retorna PID, prontidao, cena carregada e estado da captura.",
    inputSchema: {
      timeoutSeconds: z.number().min(0.1).max(10).default(2),
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const status = await editorBridge.status(Math.round(input.timeoutSeconds * 1000));
      return jsonResult({ connected: true, ...status });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      return jsonResult({ connected: false, ready: false, captureBusy: false, message });
    }
  },
);

server.registerTool(
  "smile_profile_configure",
  {
    title: "Configure Smile profiling regime",
    description:
      "Fixa camera opcional, TOD e os consumidores do hit path; gameplay_rr replica o regime do Mini Profiler com DLSS Ray Reconstruction. timeOfDayHours permite escolher a hora fixa (10 por default). Os knobs de amostragem do ReSTIR DI sao opcionais e, quando omitidos, preservam o valor corrente.",
    inputSchema: {
      preset: z.enum(["gameplay_rr", "controlled_native"]).default("gameplay_rr"),
      bookmarkSlot: z.number().int().min(-1).max(3).default(-1),
      timeOfDayHours: z
        .number()
        .min(0)
        .lt(24)
        .nullish()
        .describe("Hora fixa do teste em [0, 24); ausente ou null usa o default historico de 10:00."),
      // Matriz da Fase 0 do MESH-LIGHTS-PLAN.md. Sem `.default()` de proposito: o editor le
      // ausencia como "preserve o valor corrente", e um default aqui reescreveria a cada chamada
      // o eixo que nao esta em teste. Aplicados DEPOIS do preset.
      //
      // `.nullish()` e nao `.optional()`: o bridge nativo ja trata `null` como ausente, e um script
      // que monta o objeto dinamicamente produz `null` para "nao setado". Com `.optional()` os dois
      // caminhos de entrada discordavam — o mesmo payload passava pela pipe e era recusado pelo
      // schema antes de chegar ao editor. Os nulls sao normalizados para ausencia no handler.
      diAnalyticCandidates: z
        .number()
        .int()
        .min(1)
        .max(64)
        .nullish()
        .describe("Candidatas iniciais do pool de luzes analiticas."),
      diMeshCandidates: z
        .number()
        .int()
        .min(0)
        .max(64)
        .nullish()
        .describe(
          "Candidatas iniciais do pool de triangulos emissivos; 0 = nenhuma proposta nova, o que nao e o mesmo que tirar o pool.",
        ),
      meshLightsInPool: z
        .boolean()
        .nullish()
        .describe("A/B do pool de mesh lights: false tira os triangulos das propostas do DI."),
      diInitialVisibility: z
        .boolean()
        .nullish()
        .describe("A/B do passo 2 do Alg. 5: raio de visibilidade sobre a candidata escolhida."),
      // Fase 0.5, passo 3: TAMANHO DO DOMINIO, agora que a residencia ja esta fixa em VRAM.
      meshCompactSupport: z
        .boolean()
        .nullish()
        .describe(
          "Compacta o pool para o suporte positivo (fluxo > 0). Nao muda a distribuicao — os descartados ja tinham probabilidade zero — e reconstroi a alias table.",
        ),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      // Rest, e nao um objeto enumerado campo a campo: so as chaves que o chamador realmente
      // mandou existem em `rest`, entao "omitido" chega ao editor como ausencia de chave — que e o
      // que ele le como "preserve o valor corrente".
      //
      // `null` vira ausencia AQUI, na borda: o schema o aceita para nao recusar o payload que a
      // pipe aceitaria, e a normalizacao acontece uma vez so, no ponto em que o schema mora. Assim
      // o ProfileOverrides continua com um significado unico — chave presente e valor real.
      const { preset, bookmarkSlot, ...rest } = input;
      const overrides = Object.fromEntries(
        Object.entries(rest).filter(([, value]) => value !== null && value !== undefined),
      ) as ProfileOverrides;
      return jsonResult(await editorBridge.configureProfile(preset, bookmarkSlot, overrides));
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_profile_gpu",
  {
    title: "Sample Smile GPU profiler",
    description:
      "Aquece por tempo real e amostra timestamps brutos + EMA do Mini Profiler, retornando mediana, percentis, desvio e VRAM por passe.",
    inputSchema: {
      warmupSeconds: z.number().min(0).max(120).default(15),
      samples: z.number().int().min(5).max(240).default(60),
      intervalMs: z.number().int().min(100).max(2000).default(250),
      includeSamples: z.boolean().default(false),
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      if (input.warmupSeconds > 0) {
        await new Promise((resolve) => setTimeout(resolve, input.warmupSeconds * 1000));
      }
      const snapshots: ProfileSnapshot[] = [];
      for (let index = 0; index < input.samples; index += 1) {
        snapshots.push(await editorBridge.profileSnapshot(Math.max(2_000, input.intervalMs * 4)));
        if (index + 1 < input.samples) {
          await new Promise((resolve) => setTimeout(resolve, input.intervalMs));
        }
      }
      return jsonResult({
        warmupSeconds: input.warmupSeconds,
        intervalMs: input.intervalMs,
        ...summarizeProfile(snapshots, input.includeSamples),
      });
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_close_editor",
  {
    title: "Close Smile editor",
    description:
      "Solicita fechamento ordenado ao editor vivo e aguarda renderer, janela e named pipe encerrarem.",
    inputSchema: {
      timeoutSeconds: z.number().int().min(5).max(120).default(30),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const status = await editorBridge.status(300).catch(() => undefined);
      if (!status) return jsonResult({ alreadyStopped: true, stopped: true });
      return jsonResult({ pid: status.pid, ...(await editorBridge.shutdown(input.timeoutSeconds)) });
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_run_editor",
  {
    title: "Run Smile editor",
    description:
      "Inicia o SmileEditor, opcionalmente com uma .sscene, e pode aguardar renderer e cena ficarem prontos.",
    inputSchema: {
      configuration: z.enum(["Debug", "Release", "RelWithDebInfo"]).default("Debug"),
      scenePath: z
        .string()
        .min(1)
        .optional()
        .describe("Caminho relativo da .sscene que deve ser carregada no boot."),
      arguments: z.array(z.string()).max(32).default([]),
      waitUntilReady: z.boolean().default(true),
      startupTimeoutSeconds: z.number().int().min(10).max(300).default(90),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const executable = project.editorExecutable(input.configuration);
      let scenePath: string | undefined;
      if (input.scenePath) {
        scenePath = await project.resolveProjectPath(input.scenePath);
        const sceneStat = await stat(scenePath);
        if (!sceneStat.isFile() || path.extname(scenePath).toLocaleLowerCase() !== ".sscene") {
          throw new Error("scenePath precisa apontar para um arquivo .sscene.");
        }
      }

      try {
        const existing = await editorBridge.status(300);
        if (!existing.executablePath) {
          throw new Error(
            "O SmileEditor conectado usa um bridge antigo que nao identifica a build. " +
              "Feche-o e recompile o alvo SmileEditor.",
          );
        }
        if (!sameWindowsPath(existing.executablePath, executable)) {
          throw new Error(
            `Ja existe um SmileEditor aberto por '${existing.executablePath}', mas foi pedida ` +
              `'${executable}'. Feche-o antes de trocar de configuracao.`,
          );
        }
        if (scenePath && !sameWindowsPath(existing.scenePath, scenePath)) {
          throw new Error(
            `Ja existe um SmileEditor aberto com '${existing.scenePath || "nenhuma cena"}'. ` +
              "Feche-o antes de iniciar outra cena.",
          );
        }
        const status = input.waitUntilReady
          ? await editorBridge.waitUntilReady(input.startupTimeoutSeconds, scenePath)
          : existing;
        return jsonResult({
          configuration: input.configuration,
          executable,
          pid: existing.pid,
          alreadyRunning: true,
          status,
        });
      } catch (error) {
        if (error instanceof Error && error.message.startsWith("Ja existe um SmileEditor")) {
          throw error;
        }
        if (
          !(error instanceof Error) ||
          !error.message.startsWith("SmileEditor nao encontrado na named pipe local")
        ) {
          throw error;
        }
      }

      const editorArguments = scenePath ? [scenePath, ...input.arguments] : input.arguments;
      const child = spawn(executable, editorArguments, {
        cwd: path.dirname(executable),
        detached: true,
        shell: false,
        stdio: "ignore",
        windowsHide: false,
      });
      await new Promise<void>((resolve, reject) => {
        child.once("spawn", resolve);
        child.once("error", reject);
      });
      child.unref();
      const status = input.waitUntilReady
        ? await editorBridge.waitUntilReady(input.startupTimeoutSeconds, scenePath)
        : undefined;
      return jsonResult({
        configuration: input.configuration,
        executable,
        pid: child.pid ?? null,
        alreadyRunning: false,
        scenePath: scenePath ? project.relative(scenePath) : null,
        arguments: editorArguments,
        status: status ?? null,
      });
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_capture_frame",
  {
    title: "Capture a Smile frame",
    description:
      "Captura o viewport do SmileEditor aberto: restaura opcionalmente um bookmark, aquece frames renderizados e retorna PNG + manifesto.",
    inputSchema: {
      bookmarkSlot: z
        .number()
        .int()
        .min(-1)
        .max(3)
        .default(-1)
        .describe("-1 usa a camera atual; 0 a 3 restauram o bookmark antes da captura."),
      warmupFrames: z.number().int().min(0).max(512).default(128),
      preset: z.enum(["scientific", "gameplay"]).default("scientific"),
      pinTimeOfDay: z
        .boolean()
        .default(true)
        .describe("Fixa a hora declarada; sem timeOfDayHours usa a hora atual do mundo."),
      timeOfDayHours: z.number().min(0).lt(24).optional(),
      timeoutSeconds: z.number().int().min(10).max(900).default(180),
      includeImage: z
        .boolean()
        .default(true)
        .describe("Inclui o PNG como conteudo de imagem quando tiver ate 16 MiB."),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const capture = await editorBridge.captureFrame({
        bookmarkSlot: input.bookmarkSlot,
        warmupFrames: input.warmupFrames,
        preset: input.preset,
        pinTimeOfDay: input.pinTimeOfDay,
        timeOfDayHours: input.timeOfDayHours,
        timeoutSeconds: input.timeoutSeconds,
        includeImage: input.includeImage,
      });
      const { imageData, ...metadata } = capture;
      const content: Array<
        | { type: "text"; text: string }
        | { type: "image"; data: string; mimeType: string }
      > = [{ type: "text", text: JSON.stringify(metadata, null, 2) }];
      if (imageData) content.push({ type: "image", data: imageData, mimeType: "image/png" });
      return { content };
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_meshgen_providers",
  {
    title: "List AI mesh providers",
    description:
      "Lista os provedores de geracao de malha 3D por IA, o endpoint em uso e se a chave de API " +
      "esta configurada. Nunca retorna o valor da chave. Consulte antes de smile_generate_mesh.",
    inputSchema: {},
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  async () => {
    try {
      return jsonResult({ providers: describeProviders() });
    } catch (error) {
      return errorResult(error);
    }
  },
);

// Extensoes de imagem aceitas como referencia para image-to-3D, e o mime que vai no data URI.
const IMAGE_MIME_BY_EXTENSION: Record<string, string> = {
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".webp": "image/webp",
};
const MAX_REFERENCE_IMAGE_BYTES = 8 * 1024 * 1024;

server.registerTool(
  "smile_generate_mesh",
  {
    title: "Generate a 3D mesh with AI",
    description:
      "Gera uma malha 3D a partir de um prompt (ou de uma imagem de referencia) num provedor de " +
      "IA, grava o .glb em Assets/Generated/<nome>/, cozinha para .smesh/.sscene e, com " +
      "loadInEditor, adiciona a cena do editor aberto. Chamada paga e demorada: confirme o " +
      "provedor com smile_meshgen_providers antes.",
    inputSchema: {
      name: z
        .string()
        .min(1)
        .max(64)
        .describe("Nome do asset; vira o diretorio Assets/Generated/<slug>."),
      provider: z.enum(PROVIDER_IDS as [string, ...string[]]).default("meshy"),
      prompt: z.string().min(1).max(2000).optional().describe("Descricao textual da malha."),
      imagePath: z
        .string()
        .min(1)
        .optional()
        .describe("Caminho relativo de uma imagem de referencia dentro da SmileEngine."),
      negativePrompt: z.string().max(2000).optional(),
      artStyle: z.string().max(64).optional(),
      timeoutSeconds: z.number().int().min(30).max(3600).default(900),
      pollSeconds: z.number().int().min(2).max(60).default(5),
      cook: z.boolean().default(true).describe("Cozinha o .glb para .smesh/.sscene."),
      configuration: z.enum(["Debug", "Release", "RelWithDebInfo"]).default("Release"),
      fitMeters: z
        .number()
        .min(0.01)
        .max(1000)
        .default(2)
        .describe("Escala a malha para caber num cubo deste tamanho. Malha gerada chega sem unidade."),
      dropToGround: z.boolean().default(true),
      centerXZ: z.boolean().default(true),
      loadInEditor: z
        .boolean()
        .default(false)
        .describe("Adiciona o cozido a cena do SmileEditor aberto, sem reinicia-lo."),
      loadTimeoutSeconds: z.number().int().min(10).max(600).default(120),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      // Unica ferramenta do servidor que fala com a rede: sai para a API do provedor.
      openWorldHint: true,
    },
  },
  async (input) => {
    try {
      if ((input.prompt ? 1 : 0) + (input.imagePath ? 1 : 0) !== 1) {
        throw new Error("Informe exatamente um de prompt ou imagePath.");
      }

      let imageDataUri: string | undefined;
      if (input.imagePath) {
        const imagePath = await project.resolveProjectPath(input.imagePath);
        const imageStat = await stat(imagePath);
        const extension = path.extname(imagePath).toLocaleLowerCase();
        const mime = IMAGE_MIME_BY_EXTENSION[extension];
        if (!imageStat.isFile() || !mime) {
          throw new Error(
            `imagePath precisa ser um ${Object.keys(IMAGE_MIME_BY_EXTENSION).join("/")} dentro da SmileEngine.`,
          );
        }
        if (imageStat.size > MAX_REFERENCE_IMAGE_BYTES) {
          throw new Error("A imagem de referencia excede 8 MiB.");
        }
        imageDataUri = `data:${mime};base64,${(await readFile(imagePath)).toString("base64")}`;
      }

      const generation = await generateMesh(project.root, {
        provider: input.provider as (typeof PROVIDER_IDS)[number],
        name: input.name,
        prompt: input.prompt,
        imageDataUri,
        negativePrompt: input.negativePrompt,
        artStyle: input.artStyle,
        timeoutSeconds: input.timeoutSeconds,
        pollSeconds: input.pollSeconds,
      });

      if (!input.cook) {
        return jsonResult({ generation, cooked: null, loaded: null });
      }

      const cooked = await project.cookScene({
        sourcePath: generation.modelPath,
        configuration: input.configuration,
        opaqueGlass: false,
        timeoutSeconds: Math.min(900, input.timeoutSeconds),
        fitMeters: input.fitMeters,
        dropToGround: input.dropToGround,
        centerXZ: input.centerXZ,
      });
      if (typeof cooked.exitCode === "number" && cooked.exitCode !== 0) {
        return jsonResult({ generation, cooked, loaded: null });
      }

      let loaded: unknown = null;
      if (input.loadInEditor) {
        const slug = slugify(input.name);
        const scenePath = await project.resolveProjectPath(
          `Assets/Generated/${slug}/${slug}.sscene`,
        );
        loaded = await editorBridge.loadScene(scenePath, true, input.loadTimeoutSeconds);
      }
      return jsonResult({ generation, cooked, loaded });
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_load_scene",
  {
    title: "Load a scene into the running editor",
    description:
      "Carrega uma .sscene ja cozida no SmileEditor aberto. Aditivo por default: acrescenta a " +
      "cena atual sem reiniciar o editor, preservando camera e hora do dia. Com additive=false " +
      "substitui a cena inteira.",
    inputSchema: {
      scenePath: z.string().min(1).describe("Caminho relativo de uma .sscene dentro da SmileEngine."),
      additive: z.boolean().default(true),
      timeoutSeconds: z.number().int().min(10).max(600).default(120),
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  async (input) => {
    try {
      const scenePath = await project.resolveProjectPath(input.scenePath);
      if (path.extname(scenePath).toLocaleLowerCase() !== ".sscene") {
        throw new Error("scenePath precisa apontar para um arquivo .sscene.");
      }
      const result = await editorBridge.loadScene(scenePath, input.additive, input.timeoutSeconds);
      return jsonResult({ scenePath: project.relative(scenePath), additive: input.additive, result });
    } catch (error) {
      return errorResult(error);
    }
  },
);

async function main(): Promise<void> {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error(`[smile-mcp] v${VERSION} conectado; raiz=${project.root}`);
}

main().catch((error: unknown) => {
  const message = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[smile-mcp] falha fatal: ${message}`);
  process.exitCode = 1;
});
