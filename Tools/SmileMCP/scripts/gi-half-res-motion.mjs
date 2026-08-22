import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
const scenes = [
  { id: "bistro", path: "Assets/Scenes/Bistro/BistroExterior.sscene" },
  { id: "emerald", path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene" },
];
const stepCount = 8;
const stepMeters = 0.04;
const stepYawDegrees = 0.35;
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) { if (!condition) throw new Error(message); }
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
  command: process.execPath, args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot, env: environment, stderr: "pipe",
});
const client = new Client({ name: "smile-gi-half-res-motion", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const sdkTimeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
  ), toolName);
}

async function waitForGI(predicate, timeoutMilliseconds = 120_000) {
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
    x: pose.position.x, y: pose.position.y, z: pose.position.z,
    pitchDegrees: pose.pitchDegrees, yawDegrees: pose.yawDegrees,
    cameraCut, timeoutSeconds: 30,
  });
}

async function settleHalfRes(enabled, renderSize, frames = 128) {
  const applied = await call("smile_gi_configure", { halfRes: enabled, timeoutSeconds: 120 });
  const expectedWidth = enabled ? Math.ceil(renderSize.width / 2) : renderSize.width;
  const expectedHeight = enabled ? Math.ceil(renderSize.height / 2) : renderSize.height;
  return waitForGI((status) => status.restirGI?.halfRes === enabled &&
    status.restirGI.halfResEffective === enabled &&
    status.restirGI.renderWidth === expectedWidth &&
    status.restirGI.renderHeight === expectedHeight &&
    status.frameIndex >= applied.frameIndex + frames);
}

async function liveCapture(sceneId, modeId, frameId, halfRes, renderSize) {
  const capture = await call("smile_capture_frame", {
    bookmarkSlot: -1, warmupFrames: 0, preset: "gameplay", resetHistory: false,
    pinTimeOfDay: false, timeoutSeconds: 300, includeImage: false,
  });
  const expectedWidth = halfRes ? Math.ceil(renderSize.width / 2) : renderSize.width;
  const expectedHeight = halfRes ? Math.ceil(renderSize.height / 2) : renderSize.height;
  assert(capture.manifest.preset === "gameplay" && capture.manifest.historyReset === false,
    `${sceneId}/${modeId}/${frameId}: captura live alterou o contrato temporal`);
  assert(capture.manifest.restirGIHalfResEffective === halfRes &&
    capture.manifest.restirGIRenderWidth === expectedWidth &&
    capture.manifest.restirGIRenderHeight === expectedHeight,
  `${sceneId}/${modeId}/${frameId}: dominio interno inesperado`);
  assert(capture.manifest.denoiser === "DLSS_RR" && capture.manifest.upscaler === "DLSS",
    `${sceneId}/${modeId}/${frameId}: gameplay_rr nao ficou efetivo`);
  return { id: `${modeId}_${frameId}`, modeId, frameId, halfRes,
    pngPath: capture.pngPath, manifestPath: capture.manifestPath, manifest: capture.manifest };
}

const report = {
  schema: 1, startedAt: new Date().toISOString(), scenes: [],
  regime: {
    presentation: "gameplay_rr", historyResetDuringSequence: false,
    path: { axis: "world_x", stepCount, stepMeters, stepYawDegrees,
      totalMeters: stepCount * stepMeters, totalYawDegrees: stepCount * stepYawDegrees },
    settleFrames: { afterModeOrCut: 128, finalReference: 64 },
  },
};
let currentLaunch;
let reportPath;

async function runMode(scene, sceneReport, halfRes, renderSize, baseCamera) {
  const modeId = halfRes ? "on" : "off";
  console.log(`[gi-half-motion] ${scene.id}: ${modeId} assentando`);
  await settleHalfRes(halfRes, renderSize);
  await setCamera(baseCamera, true);
  await waitFrames(128);
  sceneReport.captures.push(await liveCapture(
    scene.id, modeId, "base", halfRes, renderSize));

  let lastPose = baseCamera;
  for (let step = 1; step <= stepCount; ++step) {
    const pose = {
      position: {
        x: baseCamera.position.x + step * stepMeters,
        y: baseCamera.position.y,
        z: baseCamera.position.z,
      },
      pitchDegrees: baseCamera.pitchDegrees,
      yawDegrees: baseCamera.yawDegrees + step * stepYawDegrees,
    };
    console.log(`[gi-half-motion] ${scene.id}: ${modeId} passo ${step}/${stepCount}`);
    lastPose = await setCamera(pose, false);
    sceneReport.captures.push(await liveCapture(
      scene.id, modeId, `step_${String(step).padStart(2, "0")}`, halfRes, renderSize));
  }
  await waitFrames(64);
  sceneReport.captures.push(await liveCapture(
    scene.id, modeId, "final_settled", halfRes, renderSize));
  sceneReport.paths.push({ modeId, halfRes, start: baseCamera, end: lastPose });
}

async function runScene(scene) {
  const sceneReport = { id: scene.id, scenePath: scene.path, captures: [], paths: [] };
  report.scenes.push(sceneReport);
  currentLaunch = await call("smile_run_editor", {
    configuration: "Release", scenePath: scene.path,
    waitUntilReady: true, startupTimeoutSeconds: 240,
  });
  sceneReport.editor = currentLaunch;
  console.log(`[gi-half-motion] ${scene.id}: editor PID ${currentLaunch.pid}`);
  const profile = await call("smile_profile_configure", {
    preset: "gameplay_rr", bookmarkSlot: -1, timeOfDayHours: 10,
    backgroundThrottle: false,
  });
  assert(profile.settings.denoiser === 2 && profile.settings.upscaler === 2,
    `${scene.id}: DLSS RR indisponivel no preset gameplay_rr`);
  await call("smile_gi_configure", {
    ddgiEnabled: true, indirectPrimary: "restir_sharc", indirectFallback: "ddgi",
    cascadeCount: 2, interleavedUpdates: true, probeCompaction: true,
    halfRes: false, timeoutSeconds: 120,
  });
  const baseline = await waitForGI((status) => status.restirGI?.halfResEffective === false &&
    status.restirGI.renderWidth > 0 && status.restirGI.renderHeight > 0);
  const renderSize = { width: baseline.restirGI.renderWidth,
    height: baseline.restirGI.renderHeight };
  const baseCamera = await call("smile_camera_get", { timeoutSeconds: 5 });
  sceneReport.profile = profile;
  sceneReport.renderSize = renderSize;
  sceneReport.baseCamera = baseCamera;

  await runMode(scene, sceneReport, false, renderSize, baseCamera);
  await runMode(scene, sceneReport, true, renderSize, baseCamera);
  await setCamera(baseCamera, true);
  sceneReport.passed = true;
  await call("smile_close_editor", { timeoutSeconds: 60 });
  currentLaunch = undefined;
  await delay(1_000);
}

try {
  await client.connect(transport);
  const initial = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (initial.connected) {
    await call("smile_close_editor", { timeoutSeconds: 60 });
    await delay(1_000);
  }
  for (const scene of scenes) await runScene(scene);
  report.finishedAt = new Date().toISOString(); report.passed = true;
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  reportPath = path.join(reportDirectory, `gi-half-res-motion-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-half-motion] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString(); report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-half-motion] FAIL: ${report.error}`);
  try {
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    reportPath = path.join(reportDirectory, `gi-half-res-motion-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  } catch {}
  process.exitCode = 1;
} finally {
  try {
    if (currentLaunch) await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch {}
  await client.close().catch(() => {});
}
