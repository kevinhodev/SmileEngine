import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(scriptDirectory, "../../..");

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: {
    ...Object.fromEntries(Object.entries(process.env).filter(([, value]) => value !== undefined)),
    SMILE_ROOT: smileRoot,
  },
  stderr: "pipe",
});

const client = new Client({ name: "smile-mcp-smoke", version: "0.3.0" });

try {
  await client.connect(transport);
  const tools = await client.listTools();
  const expected = [
    "smile_project_info",
    "smile_list_files",
    "smile_read_file",
    "smile_find_text",
    "smile_get_latest_logs",
    "smile_build",
    "smile_compile_shaders",
    "smile_cook_scene",
    "smile_editor_status",
    "smile_profile_configure",
    "smile_profile_gpu",
    "smile_close_editor",
    "smile_run_editor",
    "smile_capture_frame",
  ];
  const names = new Set(tools.tools.map((tool) => tool.name));
  const missing = expected.filter((name) => !names.has(name));
  if (missing.length > 0) throw new Error(`Ferramentas ausentes: ${missing.join(", ")}`);

  const info = await client.callTool({ name: "smile_project_info", arguments: {} });
  if (info.isError) throw new Error("smile_project_info retornou erro");

  const readOnlyCalls = [
    {
      name: "smile_list_files",
      arguments: { relativePath: "Tools/SmileMCP", maxDepth: 1, limit: 50 },
    },
    {
      name: "smile_read_file",
      arguments: { relativePath: "CMakeLists.txt", startLine: 1, maxLines: 5 },
    },
    {
      name: "smile_find_text",
      arguments: { query: "project(SmileEngine", globs: ["CMakeLists.txt"], maxResults: 5 },
    },
    {
      name: "smile_get_latest_logs",
      arguments: { sessions: 1, tailLines: 5 },
    },
    {
      name: "smile_editor_status",
      arguments: { timeoutSeconds: 0.1 },
    },
  ];

  const payloads = new Map();
  for (const request of readOnlyCalls) {
    const result = await client.callTool(request);
    if (result.isError) throw new Error(`${request.name} retornou erro`);
    const text = result.content.find((part) => part.type === "text")?.text;
    if (!text) throw new Error(`${request.name} nao retornou conteudo textual`);
    payloads.set(request.name, JSON.parse(text));
  }

  if (payloads.get("smile_list_files")?.count < 1) {
    throw new Error("smile_list_files nao encontrou o proprio pacote");
  }
  if (!payloads.get("smile_read_file")?.content.includes("cmake_minimum_required")) {
    throw new Error("smile_read_file nao retornou o inicio do CMakeLists.txt");
  }
  if (payloads.get("smile_find_text")?.count < 1) {
    throw new Error("smile_find_text nao encontrou o marcador do projeto");
  }
  if (typeof payloads.get("smile_editor_status")?.connected !== "boolean") {
    throw new Error("smile_editor_status nao retornou estado de conexao");
  }

  const escapedPath = await client.callTool({
    name: "smile_read_file",
    arguments: { relativePath: "../fora-da-raiz.txt" },
  });
  if (!escapedPath.isError) throw new Error("smile_read_file aceitou um caminho fora da raiz");

  const invalidCookSource = await client.callTool({
    name: "smile_cook_scene",
    arguments: { sourcePath: "CMakeLists.txt", configuration: "Release" },
  });
  if (!invalidCookSource.isError) throw new Error("smile_cook_scene aceitou uma fonte que nao e FBX");

  const invalidEditorScene = await client.callTool({
    name: "smile_run_editor",
    arguments: {
      configuration: "Release",
      scenePath: "CMakeLists.txt",
      waitUntilReady: false,
    },
  });
  if (!invalidEditorScene.isError) throw new Error("smile_run_editor aceitou uma cena que nao e SScene");

  console.log(
    `Smoke test OK: ${tools.tools.length} ferramentas; leitura, busca, logs e isolamento validados.`,
  );
} finally {
  await client.close();
}
