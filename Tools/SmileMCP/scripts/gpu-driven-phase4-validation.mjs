import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { SmileEditorBridge } from "../dist/editor-bridge.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const outputDirectory = path.join(
  smileRoot, "build", "bin", "Release", "Captures", "Validation", "phase4",
);
const scenes = [
  {
    key: "bistro",
    path: "Assets/Scenes/Bistro/BistroExterior.sscene",
    bookmarkSlot: 1,
    storm: true,
  },
  {
    key: "emerald",
    path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene",
    bookmarkSlot: 0,
    storm: false,
  },
  {
    key: "synthetic_1M",
    path: "build/Tests/SyntheticScenes/Synthetic_1M.sscene",
    bookmarkSlot: 0,
    storm: false,
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
const client = new Client({ name: "smile-gpu-driven-phase4-validation", version: "1.0.0" });
const bridge = new SmileEditorBridge(smileRoot);

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

function maximum(summary) {
  return Number(summary?.max ?? 0);
}

function sleep(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function configure(scene, derived) {
  const mode = derived ? "derived" : "classic";
  const configured = await call("smile_profile_configure", {
    preset: "controlled_native",
    bookmarkSlot: scene.bookmarkSlot,
    timeOfDayHours: 10,
    backgroundThrottle: false,
    bindlessMaterials: true,
    bindlessTransforms: true,
    derivedTemporalTransforms: derived,
  });
  assert(configured.settings?.derivedTemporalTransformsAvailable === true,
    `${scene.key}/${mode}: caminho derivado indisponivel`);
  assert(configured.settings?.derivedTemporalTransforms === derived,
    `${scene.key}/${mode}: readback temporal divergente`);
  assert(configured.settings?.bindlessTransforms === true,
    `${scene.key}/${mode}: transform bindless nao ficou ligado`);
  return configured;
}

async function profile(scene, mode, suffix = "static") {
  const result = await call("smile_profile_gpu", {
    warmupSeconds: 4,
    samples: 24,
    intervalMs: 200,
    includeSamples: false,
  });
  assert(result.settings?.derivedTemporalTransforms === (mode === "derived"),
    `${scene.key}/${mode}/${suffix}: profile leu outro caminho temporal`);
  assert(maximum(result.cpu?.objectConstantBytes) === 0,
    `${scene.key}/${mode}/${suffix}: ObjectCB voltou a ser escrito`);
  assert(maximum(result.cpu?.objectConstantBytesReserved) === 0,
    `${scene.key}/${mode}/${suffix}: ObjectCB voltou a ser alocado`);
  return result;
}

async function capture(scene, mode) {
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
  assert(result.manifest?.derivedTemporalTransforms === (mode === "derived"),
    `${scene.key}/${mode}: manifesto nao registrou o caminho temporal`);
  return result;
}

async function transformStorm(scene, mode, staticProfile) {
  const listed = await call("smile_scene_objects", { limit: 64, timeoutSeconds: 30 });
  const object = listed.objects.find((item) => item.visible);
  assert(object, `${scene.key}: nenhum objeto visivel para transform storm`);
  const original = { ...object.position };
  const bytesBefore = Number(staticProfile.cpu?.instanceTable?.bytesUploadedTotal ?? 0);

  // The profiler warms for four seconds before collecting samples. Moving immediately after
  // starting it would finish the entire storm during warmup and silently measure a settled scene.
  const profilePromise = profile(scene, mode, "storm");
  await sleep(4_200);
  for (let i = 0; i < 128; ++i) {
    await bridge.setSceneObjectPosition({
      index: object.index,
      x: original.x + (i & 1 ? 0.75 : -0.75),
      y: original.y,
      z: original.z,
    }, 10_000);
    await sleep(25);
  }
  await bridge.setSceneObjectPosition({ index: object.index, ...original }, 10_000);
  const result = await profilePromise;
  const bytesAfter = Number(result.cpu?.instanceTable?.bytesUploadedTotal ?? 0);
  assert(bytesAfter > bytesBefore,
    `${scene.key}/${mode}: transform storm nao publicou nenhuma linha`);
  result.transformStorm = {
    objectIndex: object.index,
    updates: 129,
    bytesUploadedDuringStorm: bytesAfter - bytesBefore,
  };
  return result;
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
  const result = { ...scene, launch, cases: [] };
  for (const derived of [false, true, false]) {
    const mode = derived ? "derived" : "classic";
    const configured = await configure(scene, derived);
    const image = await capture(scene, mode);
    const staticProfile = await profile(scene, mode);
    const stormProfile = scene.storm ? await transformStorm(scene, mode, staticProfile) : null;
    result.cases.push({ mode, configured, image, staticProfile, stormProfile });
    console.log(`[phase4] ${scene.key}/${mode}: ${image.pngPath}`);
  }
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
  assert(configureTool?.inputSchema?.properties?.derivedTemporalTransforms,
    "smile_profile_configure nao publicou derivedTemporalTransforms");
  for (const scene of scenes) report.scenes.push(await runScene(scene));
  report.passed = true;
} catch (error) {
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[phase4] FAIL: ${report.error}`);
  process.exitCode = 1;
} finally {
  try { await closeEditorIfRunning(); } catch {}
  report.finishedAt = new Date().toISOString();
  await mkdir(outputDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportPath = path.join(outputDirectory, `phase4-validation-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[phase4] report: ${reportPath}`);
  await client.close();
}
