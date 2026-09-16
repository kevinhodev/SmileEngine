import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const outputDirectory = path.join(
  smileRoot, "build", "bin", "Release", "Captures", "Validation", "phase5",
);
const scenes = [
  {
    key: "emerald",
    path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene",
    bookmarkSlot: 0,
    warmupSeconds: 3,
    samples: 16,
  },
  {
    key: "synthetic_1M",
    path: "build/Tests/SyntheticScenes/Synthetic_1M.sscene",
    bookmarkSlot: 0,
    warmupSeconds: 5,
    samples: 12,
  },
];

const environment = {
  ...Object.fromEntries(Object.entries(process.env).filter(([, value]) => value !== undefined)),
  SMILE_ROOT: smileRoot,
};
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: environment,
  stderr: "pipe",
});
const client = new Client({ name: "smile-gpu-driven-phase5-validation", version: "1.0.0" });

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function parseResult(result, toolName) {
  const body = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${body || "erro sem detalhe"}`);
  try { return JSON.parse(body || "{}"); }
  catch { throw new Error(`${toolName}: resposta nao e JSON`); }
}

async function call(toolName, args = {}) {
  const seconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const timeout = Math.max(60_000, (seconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout, maxTotalTimeout: timeout },
  ), toolName);
}

async function closeEditorIfRunning() {
  const status = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (status.connected) await call("smile_close_editor", { timeoutSeconds: 120 });
}

function maximum(summary) { return Number(summary?.max ?? 0); }
function pass(profile, key) { return profile.cpu?.passes?.find((entry) => entry.key === key); }

async function configure(scene, enabled) {
  const result = await call("smile_profile_configure", {
    preset: "controlled_native",
    bookmarkSlot: scene.bookmarkSlot,
    timeOfDayHours: 10,
    backgroundThrottle: false,
    bindlessMaterials: true,
    bindlessTransforms: true,
    gpuIndirectMainView: enabled,
  });
  assert(result.settings?.bindlessMaterials === true, `${scene.key}: material bindless recusado`);
  assert(result.settings?.bindlessTransforms === true, `${scene.key}: transform bindless recusado`);
  assert(result.settings?.gpuIndirectMainViewAvailable === true,
    `${scene.key}: main view indireto indisponivel`);
  assert(result.settings?.gpuIndirectMainView === enabled,
    `${scene.key}: readback do toggle indireto divergente`);
  return result;
}

async function profile(scene, enabled, label) {
  const result = await call("smile_profile_gpu", {
    warmupSeconds: scene.warmupSeconds,
    samples: scene.samples,
    intervalMs: 200,
    includeSamples: false,
  });
  assert(result.settings?.gpuIndirectMainView === enabled,
    `${scene.key}/${label}: profile leu outro caminho`);
  assert(maximum(result.cpu?.objectConstantBytes) === 0,
    `${scene.key}/${label}: ObjectCB voltou a ser escrito`);
  assert(maximum(result.cpu?.objectConstantBytesReserved) === 0,
    `${scene.key}/${label}: ObjectCB voltou a ser alocado`);

  const z = pass(result, "zPrepassOpaque");
  const g = pass(result, "gbuffer");
  assert(z && g, `${scene.key}/${label}: passes principais ausentes`);
  if (enabled) {
    assert(result.cpu?.indirectMainView?.active === true,
      `${scene.key}/${label}: cadeia nao ficou ativa`);
    assert(result.cpu?.indirectMainView?.lastReason === 0,
      `${scene.key}/${label}: cadeia recusada (${result.cpu?.indirectMainView?.lastReason})`);
    assert(maximum(result.cpu?.indirectMainView?.commands) > 0,
      `${scene.key}/${label}: nenhum comando indireto produzido`);
    assert(maximum(z.indirectCalls) > 0 && maximum(g.indirectCalls) > 0,
      `${scene.key}/${label}: ExecuteIndirect nao chegou aos dois passes`);
    assert(result.cpu?.indirectMainView?.countersValid === true,
      `${scene.key}/${label}: readback atrasado nao ficou valido`);
    assert(result.cpu?.indirectMainView?.gpuStatus === 1,
      `${scene.key}/${label}: finalizacao GPU invalidou o frame`);
    assert(result.cpu?.indirectMainView?.gpuReason === 0,
      `${scene.key}/${label}: canario GPU ${result.cpu?.indirectMainView?.gpuReason}`);
    assert(maximum(result.cpu?.indirectMainView?.gpuCommands) > 0,
      `${scene.key}/${label}: GPU nao confirmou comandos`);
  } else {
    assert(result.cpu?.indirectMainView?.active === false,
      `${scene.key}/${label}: cadeia ficou ativa com toggle desligado`);
    assert(maximum(z.indirectCalls) === 0 && maximum(g.indirectCalls) === 0,
      `${scene.key}/${label}: houve ExecuteIndirect no controle CPU`);
  }
  return result;
}

async function capture(scene, enabled, label) {
  const result = await call("smile_capture_frame", {
    bookmarkSlot: scene.bookmarkSlot,
    warmupFrames: 64,
    preset: "scientific",
    resetHistory: true,
    pinTimeOfDay: true,
    timeOfDayHours: 10,
    timeoutSeconds: 600,
    includeImage: false,
  });
  assert(result.manifest?.gpuIndirectMainView === enabled,
    `${scene.key}/${label}: manifesto registrou outro caminho`);
  return { label, enabled, pngPath: result.pngPath, manifestPath: result.manifestPath };
}

async function auditLatestLog(scene) {
  const logs = await call("smile_get_latest_logs", { sessions: 1, tailLines: 1000 });
  const lines = logs.sessions?.[0]?.lines ?? [];
  const fatal = lines.filter((line) =>
    /D3D12.*ERROR|DXGI_ERROR|DEVICE_REMOVED|GPU validation.*error|\[ERR\s*\]/i.test(line));
  assert(fatal.length === 0, `${scene}: log GPU/API: ${fatal.join(" | ")}`);
  return { path: logs.sessions?.[0]?.path ?? null, fatal };
}

async function runScene(scene) {
  const launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: scene.path,
    waitUntilReady: true,
    startupTimeoutSeconds: 300,
  });
  const result = { ...scene, launch, cases: [], captures: [] };

  await configure(scene, false);
  result.cases.push({ label: "off_a", profile: await profile(scene, false, "off_a") });
  result.captures.push(await capture(scene, false, "off_a"));

  await configure(scene, true);
  result.cases.push({ label: "on", profile: await profile(scene, true, "on") });
  result.captures.push(await capture(scene, true, "on"));

  // O retorno prova que o A/B nao depende de reload e que o fallback CPU continua inteiro.
  await configure(scene, false);
  result.cases.push({ label: "off_b", profile: await profile(scene, false, "off_b") });
  result.captures.push(await capture(scene, false, "off_b"));

  const offDraws = maximum(pass(result.cases[0].profile, "gbuffer")?.draws);
  const onDraws = maximum(pass(result.cases[1].profile, "gbuffer")?.draws);
  assert(onDraws < offDraws,
    `${scene.key}: submissao direta nao caiu (${offDraws} -> ${onDraws})`);
  result.logAudit = await auditLatestLog(scene.key);
  await closeEditorIfRunning();
  return result;
}

const report = { schema: 1, startedAt: new Date().toISOString(), scenes: [] };
try {
  await client.connect(transport);
  await closeEditorIfRunning();
  const tools = await client.listTools();
  const configureTool = tools.tools.find((tool) => tool.name === "smile_profile_configure");
  assert(configureTool?.inputSchema?.properties?.gpuIndirectMainView,
    "smile_profile_configure nao publicou gpuIndirectMainView");
  for (const scene of scenes) {
    report.scenes.push(await runScene(scene));
    console.log(`[phase5] ${scene.key}: OFF/ON/OFF aprovado`);
  }
  report.passed = true;
} catch (error) {
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[phase5] FAIL: ${report.error}`);
  process.exitCode = 1;
} finally {
  try { await closeEditorIfRunning(); } catch {}
  report.finishedAt = new Date().toISOString();
  await mkdir(outputDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportPath = path.join(outputDirectory, `phase5-validation-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[phase5] report: ${reportPath}`);
  await client.close();
}
