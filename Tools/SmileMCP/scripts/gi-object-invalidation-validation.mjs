import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const sceneOption = process.argv.indexOf("--scene");
const scenePath = sceneOption >= 0 && process.argv[sceneOption + 1]
  ? process.argv[sceneOption + 1]
  : "Assets/Scenes/Bistro/BistroExterior.sscene";
const sceneId = path.basename(scenePath, path.extname(scenePath)).toLowerCase();
const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
const rayBudget = 131072;
const settleUpdates = 160;
const captureOffsets = [0, 2, 4, 8, 16, 32];
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

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
const client = new Client({ name: "smile-gi-object-invalidation-validation", version: "0.1.0" });

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
    await delay(20);
  }
  throw new Error(`GI nao atingiu estado esperado: ${JSON.stringify(last)}`);
}

async function waitUpdates(count) {
  const start = await call("smile_gi_status", { timeoutSeconds: 5 });
  return waitForGI((status) => status.ddgi.updateSerial >= start.ddgi.updateSerial + count);
}

async function setPosition(index, position) {
  return call("smile_scene_object_set_position", {
    index,
    x: position.x,
    y: position.y,
    z: position.z,
    timeoutSeconds: 10,
  });
}

function vectorLength(vector) {
  return Math.hypot(vector.x, vector.y, vector.z);
}

function selectObject(objects) {
  const candidates = objects
    .filter((object) => object.visible)
    .map((object) => {
      const extents = {
        x: object.boundsMax.x - object.boundsMin.x,
        y: object.boundsMax.y - object.boundsMin.y,
        z: object.boundsMax.z - object.boundsMin.z,
      };
      const diagonal = vectorLength(extents);
      const finite = Number.isFinite(diagonal) && diagonal > 0.35 && diagonal < 4.0;
      const preferredName = /(bush|shrub|plant|flower|pot|chair|table|lamp|tree)/i.test(object.name);
      return { object, diagonal, finite, preferredName };
    })
    .filter((candidate) => candidate.finite)
    .sort((a, b) => Number(b.preferredName) - Number(a.preferredName) ||
      Math.abs(a.diagonal - 1.5) - Math.abs(b.diagonal - 1.5));
  assert(candidates.length > 0, "nenhum objeto pequeno e visivel foi encontrado");
  return candidates[0];
}

function translatedPosition(object, desiredCenter) {
  const center = {
    x: (object.boundsMin.x + object.boundsMax.x) * 0.5,
    y: (object.boundsMin.y + object.boundsMax.y) * 0.5,
    z: (object.boundsMin.z + object.boundsMax.z) * 0.5,
  };
  return {
    x: object.position.x + desiredCenter.x - center.x,
    y: object.position.y + desiredCenter.y - center.y,
    z: object.position.z + desiredCenter.z - center.z,
  };
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
  rayBudget,
  cascadeCount: 3,
  settleUpdates,
  captureOffsets,
  telemetry: [],
  captures: [],
};
let editorStarted = false;
let selected;
let originalPosition;

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
  console.log(`[gi-object-invalidation] editor PID ${launch.pid}; cena=${sceneId}`);

  await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
    backgroundThrottle: false,
  });
  const applied = await call("smile_gi_configure", {
    ddgiEnabled: true,
    indirectPrimary: "ddgi",
    indirectFallback: "ddgi",
    cascadeCount: 3,
    interleavedUpdates: true,
    updateRayBudget: rayBudget,
    probeCompaction: true,
    adaptiveRays: false,
    adaptiveHysteresis: false,
    timeoutSeconds: 120,
  });
  await waitForGI((status) => status.ddgi.initialized &&
    status.ddgi.actualCascadeCount === 3 &&
    status.ddgi.updateBudgetEffective &&
    status.ddgi.updateSerial >= applied.ddgi.updateSerial + settleUpdates);

  const camera = await call("smile_camera_get", { timeoutSeconds: 5 });
  const listed = await call("smile_scene_objects", { limit: 500, timeoutSeconds: 10 });
  const choice = selectObject(listed.objects);
  selected = choice.object;
  originalPosition = { ...selected.position };
  report.selectedObject = { ...selected, diagonal: choice.diagonal };

  const yaw = camera.yawDegrees * Math.PI / 180;
  const pitch = camera.pitchDegrees * Math.PI / 180;
  const forward = {
    x: Math.sin(yaw) * Math.cos(pitch),
    y: Math.sin(pitch),
    z: Math.cos(yaw) * Math.cos(pitch),
  };
  const right = { x: Math.cos(yaw), y: 0, z: -Math.sin(yaw) };
  const desiredCenter = {
    x: camera.position.x + forward.x * 8,
    y: camera.position.y + forward.y * 8,
    z: camera.position.z + forward.z * 8,
  };
  const baselinePosition = translatedPosition(selected, desiredCenter);
  const movedPosition = {
    x: baselinePosition.x + right.x * 2,
    y: baselinePosition.y,
    z: baselinePosition.z + right.z * 2,
  };
  report.camera = camera;
  report.baselinePosition = baselinePosition;
  report.movedPosition = movedPosition;
  console.log(`[gi-object-invalidation] objeto ${selected.index}: ${selected.name}`);

  await setPosition(selected.index, baselinePosition);
  await waitUpdates(settleUpdates);
  report.captures.push(await capture("baseline"));

  const beforeMove = await call("smile_gi_status", { timeoutSeconds: 5 });
  const movementSerial = beforeMove.ddgi.updateSerial;
  await setPosition(selected.index, movedPosition);

  let lastSerial = movementSerial;
  while (report.telemetry.length < 40) {
    const status = await waitForGI((value) => value.ddgi.updateSerial > lastSerial);
    lastSerial = status.ddgi.updateSerial;
    const ddgi = status.ddgi;
    report.telemetry.push({
      offset: ddgi.updateSerial - movementSerial,
      updateSerial: ddgi.updateSerial,
      activeProbeCount: ddgi.activeProbeCount,
      selectedRayCount: ddgi.selectedRayCount,
      newProbeCount: ddgi.newProbeCount,
      regionalProbeCount: ddgi.regionalProbeCount,
      followUpProbeCount: ddgi.followUpProbeCount,
      quotaProbeCount: ddgi.quotaProbeCount,
      roundRobinProbeCount: ddgi.roundRobinProbeCount,
      scheduledFullForced: ddgi.scheduledFullForced,
      cascadeAges: ddgi.cascades.map((cascade) => cascade.updateAge),
    });
    assert(ddgi.selectedRayCount <= rayBudget,
      `budget excedido no offset ${ddgi.updateSerial - movementSerial}: ${ddgi.selectedRayCount}`);
  }

  await setPosition(selected.index, baselinePosition);
  await waitUpdates(settleUpdates);
  const visualStart = (await call("smile_gi_status", { timeoutSeconds: 5 })).ddgi.updateSerial;
  await setPosition(selected.index, movedPosition);
  for (const offset of captureOffsets) {
    await waitForGI((status) => status.ddgi.updateSerial >= visualStart + offset);
    report.captures.push(await capture(`moved_u${String(offset).padStart(2, "0")}`));
  }

  await setPosition(selected.index, originalPosition);
  originalPosition = undefined;
  report.finishedAt = new Date().toISOString();
  report.passed = true;
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportPath = path.join(reportDirectory, `gi-object-invalidation-${sceneId}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-object-invalidation] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-object-invalidation] FAIL: ${report.error}`);
  process.exitCode = 1;
} finally {
  try {
    if (selected && originalPosition) await setPosition(selected.index, originalPosition);
  } catch {}
  try {
    if (editorStarted) await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch {}
  await client.close().catch(() => {});
}
