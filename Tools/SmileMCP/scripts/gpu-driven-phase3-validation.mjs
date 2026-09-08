import { access, mkdir, rm, writeFile } from "node:fs/promises";
import { constants as fsConstants } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { SmileEditorBridge } from "../dist/editor-bridge.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const outputDirectory = path.join(
  smileRoot, "build", "bin", "Release", "Captures", "Validation", "phase3",
);
const bistro = {
  key: "bistro",
  path: "Assets/Scenes/Bistro/BistroExterior.sscene",
  bookmarkSlot: 1,
};
const emerald = {
  key: "emerald",
  path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene",
  bookmarkSlot: 0,
};
const measureSeven = {
  key: "measure_seven",
  path: "Assets/Scenes/BEEPLE Zero-Day/MEASURE_SEVEN/MEASURE_SEVEN_COLORED_LIGHTS.sscene",
  bookmarkSlot: 0,
};
const shadowSidecar = path.join(smileRoot, "Assets", "Scenes", "Bistro", "BistroExterior.lights.json");
const shadowLight = `${JSON.stringify({
  version: 1,
  lights: [{
    name: "Phase3 Local Shadow Validation",
    type: "point",
    position: [-32.17, 10.0, 15.56],
    direction: [0.0, -1.0, 0.0],
    color: [1.0, 0.8, 0.6],
    intensity: 100.0,
    radius: 50.0,
    sourceRadius: 0.05,
    innerConeDeg: 25.0,
    outerConeDeg: 40.0,
    enabled: true,
    castShadows: true,
    rtWeight: 0.0,
  }],
}, null, 2)}\n`;

const migratedPasses = new Set([
  "zPrepassMasked", "gbuffer", "sunShadows", "localShadows",
]);
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
const client = new Client({ name: "smile-gpu-driven-phase3-validation", version: "1.0.0" });
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

async function exists(filePath) {
  try {
    await access(filePath, fsConstants.F_OK);
    return true;
  } catch (error) {
    if (error?.code === "ENOENT") return false;
    throw error;
  }
}

async function closeEditorIfRunning() {
  const status = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (status.connected) await call("smile_close_editor", { timeoutSeconds: 120 });
}

function maximum(summary) {
  return Number(summary?.max ?? 0);
}

function pass(snapshot, key) {
  return snapshot.cpu.passes.find((entry) => entry.key === key);
}

function validateProfileCounters(scene, enabled, profile) {
  assert(profile.cpu, `${scene}: profile sem bloco cpu`);
  let migratedMaterialDraws = 0;
  for (const item of profile.cpu.passes ?? []) {
    assert(maximum(item.materialRowMisses) === 0,
      `${scene}/${enabled ? "on" : "off"}/${item.key}: materialRowMisses != 0`);
    if (!enabled) {
      assert(maximum(item.bindlessDraws) === 0,
        `${scene}/off/${item.key}: bindlessDraws != 0`);
      continue;
    }
    if (!migratedPasses.has(item.key)) continue;
    const materialDraws = maximum(item.bindlessDraws) + maximum(item.materialFallbackDraws);
    if (materialDraws === 0) continue;
    migratedMaterialDraws += materialDraws;
    assert(maximum(item.bindlessDraws) > 0,
      `${scene}/on/${item.key}: nenhum draw bindless`);
    assert(maximum(item.materialFallbackDraws) === 0,
      `${scene}/on/${item.key}: fallback inesperado`);
  }
  if (enabled) assert(migratedMaterialDraws > 0, `${scene}/on: nenhum passe migrado exercitado`);
}

async function auditLatestLog(scene) {
  const logs = await call("smile_get_latest_logs", { sessions: 1, tailLines: 1000 });
  const lines = logs.sessions?.[0]?.lines ?? [];
  const fatal = lines.filter((line) =>
    /D3D12.*ERROR|DXGI_ERROR|DEVICE_REMOVED|GPU validation.*error|\[ERR\s*\]/i.test(line));
  assert(fatal.length === 0, `${scene}: log GPU/API: ${fatal.join(" | ")}`);
  return { path: logs.sessions?.[0]?.path ?? null, fatal };
}

async function configure(scene, enabled) {
  const mode = enabled ? "on" : "off";
  const configured = await call("smile_profile_configure", {
    preset: "controlled_native",
    bookmarkSlot: scene.bookmarkSlot,
    timeOfDayHours: 10,
    backgroundThrottle: false,
    bindlessMaterials: enabled,
  });
  assert(configured.settings?.bindlessMaterialsAvailable === true,
    `${scene.key}/${mode}: bindless indisponivel`);
  assert(configured.settings?.bindlessMaterials === enabled,
    `${scene.key}/${mode}: readback bindless divergente`);
  return configured;
}

async function runVisualCase(scene, enabled) {
  const mode = enabled ? "on" : "off";
  const configured = await configure(scene, enabled);
  const capture = await call("smile_capture_frame", {
    bookmarkSlot: scene.bookmarkSlot,
    warmupFrames: 64,
    preset: "scientific",
    resetHistory: true,
    pinTimeOfDay: true,
    timeOfDayHours: 10,
    timeoutSeconds: 600,
    includeImage: false,
  });
  assert(capture.manifest?.bindlessMaterials === enabled,
    `${scene.key}/${mode}: manifesto nao registrou o caminho efetivo`);
  const profile = await call("smile_profile_gpu", {
    warmupSeconds: 5,
    samples: 24,
    intervalMs: 200,
    includeSamples: false,
  });
  assert(profile.settings?.bindlessMaterials === enabled,
    `${scene.key}/${mode}: profile readback divergente`);
  validateProfileCounters(scene.key, enabled, profile);
  console.log(`[phase3] ${scene.key}/${mode}: ${capture.pngPath}`);
  return { mode, configured, capture, profile };
}

async function runVisualScene(scene) {
  const launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: scene.path,
    waitUntilReady: true,
    startupTimeoutSeconds: 300,
  });
  const result = { ...scene, launch, cases: [] };
  result.cases.push(await runVisualCase(scene, false));
  result.cases.push(await runVisualCase(scene, true));
  result.logAudit = await auditLatestLog(scene.key);
  await closeEditorIfRunning();
  return result;
}

async function runHeapCapacityGate() {
  const launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: measureSeven.path,
    waitUntilReady: true,
    startupTimeoutSeconds: 300,
  });
  await call("smile_profile_configure", {
    preset: "controlled_native",
    bookmarkSlot: measureSeven.bookmarkSlot,
    timeOfDayHours: 10,
    backgroundThrottle: false,
    bindlessMaterials: true,
  });
  const snapshot = await bridge.profileSnapshot(10_000);
  const heap = snapshot.cpu.srvHeap;
  assert(heap.capacity === 65_536,
    `MEASURE_SEVEN: capacidade do heap=${heap.capacity}, esperado 65536`);
  assert(heap.used >= 33_810,
    `MEASURE_SEVEN: uso do heap=${heap.used}, a tabela de meshes nao foi exercitada`);
  assert(heap.used + heap.retired <= heap.capacity,
    "MEASURE_SEVEN: ocupacao do heap incoerente");
  const logAudit = await auditLatestLog(measureSeven.key);
  await closeEditorIfRunning();
  return { ...measureSeven, launch, heap, logAudit };
}

async function forceShadowFrame(object) {
  let observed = null;
  for (let attempt = 0; attempt < 100; ++attempt) {
    // Uma mudanca estrutural produz um unico frame de stats. Repetir a mesma posicao ainda
    // incrementa a versao pelo comando de teste e evita perder esse frame para o event loop.
    await call("smile_scene_object_set_position", {
      index: object.index,
      x: object.position.x,
      y: object.position.y,
      z: object.position.z,
      timeoutSeconds: 30,
    });
    const snapshot = await bridge.profileSnapshot(5_000);
    observed = {
      sun: pass(snapshot, "sunShadows"),
      local: pass(snapshot, "localShadows"),
    };
    if ((observed.sun?.draws ?? 0) > 0 && (observed.local?.draws ?? 0) > 0)
      return observed;
  }
  throw new Error(`nao observei o frame de sombras: ${JSON.stringify(observed)}`);
}

function validateShadowPass(mode, enabled, item) {
  assert(item.materialRowMisses === 0, `${mode}/${item.key}: materialRowMisses != 0`);
  if (enabled) {
    assert(item.bindlessDraws > 0 && item.materialFallbackDraws === 0,
      `${mode}/${item.key}: caminho bindless nao foi exercitado`);
  } else {
    assert(item.bindlessDraws === 0 && item.materialFallbackDraws > 0,
      `${mode}/${item.key}: caminho classico nao foi exercitado`);
  }
}

async function runShadowGate() {
  const launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: bistro.path,
    waitUntilReady: true,
    startupTimeoutSeconds: 300,
  });
  const objects = await call("smile_scene_objects", { limit: 500, timeoutSeconds: 30 });
  const object = objects.objects.find((item) => !item.dynamic && item.visible);
  assert(object, "Bistro sem objeto estatico visivel para invalidar sombras");

  const cases = [];
  for (const enabled of [false, true]) {
    const mode = enabled ? "on" : "off";
    const configured = await configure(bistro, enabled);
    const shadows = await forceShadowFrame(object);
    validateShadowPass(mode, enabled, shadows.sun);
    validateShadowPass(mode, enabled, shadows.local);
    cases.push({ mode, configured, ...shadows });
  }
  const logAudit = await auditLatestLog("bistro_shadows");
  await closeEditorIfRunning();
  return {
    scene: bistro.path,
    injectedLight: JSON.parse(shadowLight).lights[0],
    launch,
    cases,
    logAudit,
  };
}

const report = { schema: 1, startedAt: new Date().toISOString(), visual: [] };
let sidecarCreated = false;
try {
  await client.connect(transport);
  await closeEditorIfRunning();

  const tools = await client.listTools();
  const configureTool = tools.tools.find((tool) => tool.name === "smile_profile_configure");
  assert(configureTool?.inputSchema?.properties?.bindlessMaterials,
    "smile_profile_configure nao publicou bindlessMaterials");

  report.build = {
    shaders: await call("smile_compile_shaders", {
      configuration: "Release", timeoutSeconds: 900,
    }),
    editor: await call("smile_build", {
      configuration: "Release", target: "SmileEditor", timeoutSeconds: 900,
    }),
  };

  report.visual.push(await runVisualScene(bistro));
  report.visual.push(await runVisualScene(emerald));
  report.heap = await runHeapCapacityGate();
  assert(!(await exists(shadowSidecar)),
    `o gate nao sobrescreve autoria existente: remova ou preserve ${shadowSidecar}`);
  await writeFile(shadowSidecar, shadowLight, { encoding: "utf8", flag: "wx" });
  sidecarCreated = true;
  report.shadows = await runShadowGate();
  report.passed = true;
} catch (error) {
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[phase3] FAIL: ${report.error}`);
  process.exitCode = 1;
} finally {
  try { await closeEditorIfRunning(); } catch {}
  if (sidecarCreated) {
    try { await rm(shadowSidecar); }
    catch (error) {
      report.passed = false;
      report.cleanupError = String(error);
      process.exitCode = 1;
    }
  }
  report.finishedAt = new Date().toISOString();
  await mkdir(outputDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportPath = path.join(outputDirectory, `phase3-validation-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[phase3] report: ${reportPath}`);
  await client.close();
}
