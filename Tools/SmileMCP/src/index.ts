#!/usr/bin/env node

import { spawn } from "node:child_process";
import path from "node:path";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { SmileEditorBridge } from "./editor-bridge.js";
import { SmileProject, discoverSmileRoot } from "./smile-project.js";

const VERSION = "0.1.0";
const project = new SmileProject(discoverSmileRoot());
const editorBridge = new SmileEditorBridge(project.root);

const server = new McpServer(
  { name: "smile-mcp", version: VERSION },
  {
    instructions:
      "Ferramentas locais da SmileEngine. Prefira primeiro as operacoes read-only de inspecao. " +
      "Build e execucao operam apenas na raiz configurada por SMILE_ROOT. Compile Shaders para " +
      "mudancas somente em HLSL e SmileEditor quando interfaces C++/HLSL tambem mudarem. " +
      "smile_capture_frame atua no editor aberto e aguarda PNG + manifesto.",
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
  "smile_run_editor",
  {
    title: "Run Smile editor",
    description:
      "Inicia uma versao ja compilada do SmileEditor como processo separado e retorna o PID.",
    inputSchema: {
      configuration: z.enum(["Debug", "Release", "RelWithDebInfo"]).default("Debug"),
      arguments: z.array(z.string()).max(32).default([]),
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
      const child = spawn(executable, input.arguments, {
        cwd: path.dirname(executable),
        detached: true,
        shell: false,
        stdio: "ignore",
        windowsHide: false,
      });
      child.unref();
      return jsonResult({
        configuration: input.configuration,
        executable,
        pid: child.pid ?? null,
        arguments: input.arguments,
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
