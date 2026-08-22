#!/usr/bin/env node

import { spawn } from "node:child_process";
import { stat } from "node:fs/promises";
import path from "node:path";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import {
  SmileEditorBridge,
  type GIConfigureOverrides,
  type ProfileOverrides,
  type ProfileSnapshot,
} from "./editor-bridge.js";
import { SmileProject, discoverSmileRoot } from "./smile-project.js";

const VERSION = "0.4.0";
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
      "um regime deterministico com smile_profile_configure antes de smile_profile_gpu. Use " +
      "smile_gi_configure/smile_gi_status para matrizes da politica indireta e " +
      "smile_camera_get/smile_camera_set para percursos reproduziveis de DDGI.",
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
      "Recozinha um FBX com o SmileCooker e valida os cabecalhos .smesh/.sscene produzidos ao lado da fonte.",
    inputSchema: {
      sourcePath: z
        .string()
        .min(1)
        .describe("Caminho relativo de um FBX dentro da raiz da SmileEngine."),
      configuration: z.enum(["Debug", "Release", "RelWithDebInfo"]).default("Release"),
      opaqueGlass: z.boolean().default(false),
      timeoutSeconds: z.number().int().min(10).max(1800).default(900),
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
  "smile_camera_get",
  {
    title: "Read Smile camera pose",
    description:
      "Le a posicao e a orientacao efetivas da camera do viewport para percursos reproduziveis.",
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
      return jsonResult(await editorBridge.cameraPose(Math.round(input.timeoutSeconds * 1000)));
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_camera_set",
  {
    title: "Set Smile camera pose",
    description:
      "Define uma pose absoluta da camera e retorna o readback efetivo. cameraCut=true preserva o teleporte historico; false produz movimento continuo para benchmark. O editor recusa a mudanca durante captura deterministica.",
    inputSchema: {
      x: z.number().min(-1_000_000).max(1_000_000),
      y: z.number().min(-1_000_000).max(1_000_000),
      z: z.number().min(-1_000_000).max(1_000_000),
      pitchDegrees: z.number().min(-89.9).max(89.9),
      yawDegrees: z.number().min(-36_000).max(36_000),
      cameraCut: z
        .boolean()
        .default(true)
        .describe("true invalida historicos como teleporte; false preserva continuidade temporal."),
      timeoutSeconds: z.number().min(0.1).max(30).default(5),
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
      const { timeoutSeconds, ...pose } = input;
      return jsonResult(
        await editorBridge.setCameraPose(pose, Math.round(timeoutSeconds * 1000)),
      );
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_gi_status",
  {
    title: "Read Smile GI state",
    description:
      "Le politica indireta pedida/efetiva e estado vivo do DDGI: cascatas desejadas/reais, grade, sondas, espacamento, scroll e fase do update intercalado.",
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
      return jsonResult(await editorBridge.giStatus(Math.round(input.timeoutSeconds * 1000)));
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_gi_configure",
  {
    title: "Configure Smile GI",
    description:
      "Muda somente os eixos de GI informados e devolve o estado efetivo. Serve para a matriz primary/fallback, A/B do volume e testes de cascata/scroll; pode mudar politica durante uma captura para validar o cancelamento no frame seguinte.",
    inputSchema: {
      ddgiEnabled: z.boolean().nullish(),
      indirectPrimary: z.enum(["restir_sharc", "ddgi", "off"]).nullish(),
      indirectFallback: z.enum(["ddgi", "environment", "black"]).nullish(),
      cascadeCount: z.number().int().min(1).max(4).nullish(),
      interleavedUpdates: z.boolean().nullish(),
      probeCompaction: z.boolean().nullish(),
      halfRes: z.boolean().nullish(),
      adaptiveRays: z.boolean().nullish(),
      adaptiveHysteresis: z.boolean().nullish(),
      timeoutSeconds: z.number().min(1).max(120).default(30),
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
      const { timeoutSeconds, ...rest } = input;
      const overrides = Object.fromEntries(
        Object.entries(rest).filter(([, value]) => value !== null && value !== undefined),
      ) as GIConfigureOverrides;
      return jsonResult(
        await editorBridge.configureGI(overrides, Math.round(timeoutSeconds * 1000)),
      );
    } catch (error) {
      return errorResult(error);
    }
  },
);

server.registerTool(
  "smile_profile_configure",
  {
    title: "Configure Smile profiling regime",
    description:
      "Fixa camera opcional, TOD e os consumidores do hit path; gameplay_rr usa DLSS Ray Reconstruction, controlled_native desliga denoise/upscale e controlled_nrd usa NRD nativo. timeOfDayHours permite escolher a hora fixa (10 por default). Os knobs do ReSTIR DI sao opcionais e, quando omitidos, preservam o valor corrente.",
    inputSchema: {
      preset: z.enum(["gameplay_rr", "controlled_native", "controlled_nrd"])
        .default("gameplay_rr"),
      bookmarkSlot: z.number().int().min(-1).max(3).default(-1),
      timeOfDayHours: z
        .number()
        .min(0)
        .lt(24)
        .nullish()
        .describe("Hora fixa do teste em [0, 24); ausente ou null usa o default historico de 10:00."),
      backgroundThrottle: z
        .boolean()
        .default(false)
        .describe(
          "false mantem o viewport em pacing integral sem foco; true restaura o limite interativo de ~10 FPS em segundo plano.",
        ),
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
      diBrdfRatio: z
        .boolean()
        .nullish()
        .describe(
          "A/B da demodulacao por razao de BRDF na direta com NRD/RELAX; restaura detalhe local de normal map depois do denoise.",
        ),
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
      resetHistory: z
        .boolean()
        .default(true)
        .describe("false captura continuidade temporal; exige preset gameplay e bookmarkSlot -1."),
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
        resetHistory: input.resetHistory,
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
