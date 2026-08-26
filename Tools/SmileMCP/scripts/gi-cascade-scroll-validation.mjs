import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const sceneOption = process.argv.indexOf("--scene");
const oppositeView = process.argv.includes("--opposite-view");
const referenceView = process.argv.includes("--reference-view");
const scenePath = sceneOption >= 0 && process.argv[sceneOption + 1]
  ? process.argv[sceneOption + 1]
  : "Assets/Scenes/Bistro/BistroExterior.sscene";
const sceneId = path.basename(scenePath, path.extname(scenePath)).toLowerCase();
const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
const stepCount = 10;
const stepMeters = 0.5;
const settleFrames = 192;
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

const regimes = [
  { id: "single", cascadeCount: 1, interleavedUpdates: false },
  { id: "dual_all", cascadeCount: 2, interleavedUpdates: false },
  { id: "triple_all", cascadeCount: 3, interleavedUpdates: false },
  { id: "dual_interleaved", cascadeCount: 2, interleavedUpdates: true },
];

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function parseResult(result, toolName) {
  const value = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${value || "erro sem detalhe"}`);
  try { return JSON.parse(value || "{}"); }
  catch { throw new Error(`${toolName}: resposta nao e JSON`); }
}

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
const client = new Client({ name: "smile-gi-cascade-scroll-validation", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const sdkTimeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
  ), toolName);
}

async function waitForGI(predicate, timeoutMilliseconds = 180_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  let last;
  while (Date.now() < deadline) {
    last = await call("smile_gi_status", { timeoutSeconds: 5 });
    if (predicate(last)) return last;
    await delay(50);
  }
  throw new Error(`GI nao atingiu estado esperado: ${JSON.stringify(last)}`);
}

async function waitFrames(count) {
  const start = await call("smile_gi_status", { timeoutSeconds: 5 });
  return waitForGI((status) => status.frameIndex >= start.frameIndex + count);
}

async function setCamera(pose, cameraCut) {
  return call("smile_camera_set", {
    x: pose.position.x,
    y: pose.position.y,
    z: pose.position.z,
    pitchDegrees: pose.pitchDegrees,
    yawDegrees: pose.yawDegrees,
    cameraCut,
    timeoutSeconds: 30,
  });
}

async function capture(id) {
  const result = await call("smile_capture_frame", {
    bookmarkSlot: -1,
    warmupFrames: 0,
    preset: "gameplay",
    resetHistory: false,
    pinTimeOfDay: false,
    timeoutSeconds: 300,
    includeImage: false,
  });
  assert(result.manifest.ddgiUpdateRayBudget === 0,
    `${id}: scheduler orcado contaminou a validacao`);
  return {
    id,
    pngPath: result.pngPath,
    manifestPath: result.manifestPath,
    frameIndex: result.manifest.frameIndex,
  };
}

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  route: { axis: "camera_forward", stepCount, stepMeters,
    totalMeters: stepCount * stepMeters, oppositeView, referenceView },
  isolation: { updateRayBudget: 0, adaptiveRays: false, adaptiveHysteresis: false },
  regimes: [],
};
let editorStarted = false;

try {
  await client.connect(transport);
  const initial = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (initial.connected) {
    await call("smile_close_editor", { timeoutSeconds: 60 });
    await delay(1_000);
  }

  const launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  editorStarted = true;
  report.editor = launch;
  console.log(`[gi-cascade-scroll] editor PID ${launch.pid}`);

  await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
    backgroundThrottle: false,
  });
  const loadedCamera = await call("smile_camera_get", { timeoutSeconds: 5 });
  const loadedYawRadians = loadedCamera.yawDegrees * Math.PI / 180;
  const loadedForward = { x: Math.sin(loadedYawRadians), z: Math.cos(loadedYawRadians) };
  const baseCamera = referenceView ? {
    position: { x: -14.4764862060547, y: 3.93282294273376, z: 0.278742998838425 },
    pitchDegrees: -9.050000190734863,
    yawDegrees: 78.75,
  } : oppositeView ? {
    position: {
      x: loadedCamera.position.x + loadedForward.x * 5,
      y: loadedCamera.position.y,
      z: loadedCamera.position.z + loadedForward.z * 5,
    },
    pitchDegrees: loadedCamera.pitchDegrees,
    yawDegrees: loadedCamera.yawDegrees + 180,
  } : loadedCamera;
  const yawRadians = baseCamera.yawDegrees * Math.PI / 180;
  const forward = { x: Math.sin(yawRadians), z: Math.cos(yawRadians) };
  report.baseCamera = baseCamera;

  for (const regime of regimes) {
    console.log(`[gi-cascade-scroll] ${regime.id}: configurando`);
    await setCamera(baseCamera, true);
    const applied = await call("smile_gi_configure", {
      ddgiEnabled: true,
      indirectPrimary: "ddgi",
      indirectFallback: "ddgi",
      cascadeCount: regime.cascadeCount,
      interleavedUpdates: regime.interleavedUpdates,
      updateRayBudget: 0,
      probeCompaction: true,
      adaptiveRays: false,
      adaptiveHysteresis: false,
      timeoutSeconds: 120,
    });
    await waitForGI((status) => status.ddgi?.initialized === true &&
      status.ddgi.actualCascadeCount === regime.cascadeCount &&
      status.ddgi.updateRayBudget === 0 &&
      status.ddgi.updateSerial >= applied.ddgi.updateSerial + settleFrames);

    const regimeReport = { ...regime, captures: [], samples: [] };
    report.regimes.push(regimeReport);
    for (let step = 0; step <= stepCount; ++step) {
      const pose = {
        position: {
          x: baseCamera.position.x + forward.x * step * stepMeters,
          y: baseCamera.position.y,
          z: baseCamera.position.z + forward.z * step * stepMeters,
        },
        pitchDegrees: baseCamera.pitchDegrees,
        yawDegrees: baseCamera.yawDegrees,
      };
      if (step > 0) {
        await setCamera(pose, false);
        await waitFrames(3);
      }
      const status = await call("smile_gi_status", { timeoutSeconds: 5 });
      const captureResult = await capture(`${regime.id}_step_${String(step).padStart(2, "0")}`);
      regimeReport.samples.push({ step, pose, status });
      regimeReport.captures.push(captureResult);
      console.log(`[gi-cascade-scroll] ${regime.id}: passo ${step}/${stepCount}`);
    }
  }

  report.finishedAt = new Date().toISOString();
  report.passed = true;
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportPath = path.join(reportDirectory, `gi-cascade-scroll-${sceneId}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-cascade-scroll] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-cascade-scroll] FAIL: ${report.error}`);
  process.exitCode = 1;
} finally {
  try {
    if (editorStarted) await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch {}
  await client.close().catch(() => {});
}
