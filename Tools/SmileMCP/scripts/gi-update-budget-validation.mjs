import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const sceneArgIndex = process.argv.indexOf("--scene");
const scenePath = sceneArgIndex >= 0 && process.argv[sceneArgIndex + 1]
  ? process.argv[sceneArgIndex + 1]
  : "Assets/Scenes/Bistro/BistroExterior.sscene";
const sceneId = path.basename(scenePath, path.extname(scenePath))
  .replaceAll(/[^a-zA-Z0-9_-]/g, "-").toLowerCase();
const referenceView = process.argv.includes("--reference-view");
const budgetsArgIndex = process.argv.indexOf("--budgets");
const budgets = budgetsArgIndex >= 0 && process.argv[budgetsArgIndex + 1]
  ? process.argv[budgetsArgIndex + 1].split(",").map((value) => Number(value))
  : [0, 65_536, 131_072, 262_144];
assertBudgets(budgets);
const cascadesArgIndex = process.argv.indexOf("--cascades");
const cascadeCount = cascadesArgIndex >= 0 && process.argv[cascadesArgIndex + 1]
  ? Number(process.argv[cascadesArgIndex + 1])
  : 3;
assert(Number.isInteger(cascadeCount) && cascadeCount >= 1 && cascadeCount <= 3,
  `quantidade de cascatas invalida: ${cascadeCount}`);
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assertBudgets(values) {
  if (values.length === 0 || values.some((value) => !Number.isInteger(value) || value < 0))
    throw new Error(`lista de budgets invalida: ${values.join(",")}`);
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function parseResult(result, toolName) {
  const body = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${body || "erro sem detalhe"}`);
  return JSON.parse(body || "{}");
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
const client = new Client({ name: "smile-gi-update-budget-validation", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const timeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout, maxTotalTimeout: timeout },
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
  throw new Error(`GI nao atingiu o estado esperado; ultimo=${JSON.stringify(last)}`);
}

function profilePass(profile, name) {
  return profile.passes.find((entry) => entry.name === name) ?? null;
}

function passMean(profile, name) {
  return Number(profilePass(profile, name)?.rawMilliseconds?.mean ?? Number.NaN);
}

async function profileGPU(label) {
  const profiles = [];
  for (let repeat = 0; repeat < 2; ++repeat) {
    const profile = await call("smile_profile_gpu", {
      warmupSeconds: 2,
      samples: 72,
      intervalMs: 125,
      includeSamples: false,
    });
    assert(profilePass(profile, "DDGI"), `${label}: scope DDGI ausente`);
    assert(profilePass(profile, "Frame (GPU)"), `${label}: scope Frame (GPU) ausente`);
    profiles.push(profile);
  }
  return profiles;
}

async function sampleSchedule(count = 24) {
  const samples = new Map();
  const deadline = Date.now() + 30_000;
  while (samples.size < count && Date.now() < deadline) {
    const status = await call("smile_gi_status", { timeoutSeconds: 5 });
    samples.set(status.ddgi.updateSerial, status.ddgi);
    await delay(15);
  }
  assert(samples.size >= count, `telemetria insuficiente: ${samples.size}/${count}`);
  return [...samples.values()];
}

async function setCamera(camera, cameraCut = true) {
  return call("smile_camera_set", {
    x: camera.position.x,
    y: camera.position.y,
    z: camera.position.z,
    pitchDegrees: camera.pitchDegrees,
    yawDegrees: camera.yawDegrees,
    cameraCut,
  });
}

async function capture(label, warmupFrames) {
  const result = await call("smile_capture_frame", {
    bookmarkSlot: -1,
    warmupFrames,
    preset: "scientific",
    pinTimeOfDay: true,
    timeOfDayHours: 10,
    timeoutSeconds: 600,
    includeImage: false,
  });
  assert(result.pngPath && result.manifestPath && result.manifest,
    `${label}: captura incompleta`);
  return result;
}

async function auditLogs(editorPid) {
  const logs = await call("smile_get_latest_logs", { sessions: 5, tailLines: 1000 });
  const session = logs.sessions?.find((entry) => typeof entry.path === "string" &&
    new RegExp(`-p${editorPid}\\.log$`, "i").test(entry.path));
  const fatalPatterns = [/D3D12.*ERROR/i, /DXGI_ERROR/i, /DEVICE_REMOVED/i,
    /GPU validation.*error/i, /\[ERR\s*\]/i];
  const fatalMatches = (session?.lines ?? []).filter((line) =>
    fatalPatterns.some((pattern) => pattern.test(line)));
  assert(fatalMatches.length === 0, `log contem erro GPU/API: ${fatalMatches.join(", ")}`);
  return { available: Boolean(session), path: session?.path ?? null, fatalMatches };
}

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  sceneId,
  budgets,
  regime: {
    cascadeCount,
    adaptiveRays: false,
    probeCompaction: true,
    interleavedUpdates: true,
    primary: "ddgi",
    fallback: "ddgi",
    timeOfDayHours: 10,
    performance: { repeats: 2, samplesPerRepeat: 72, intervalMs: 125, warmupSeconds: 2 },
    captures: { staticWarmupFrames: 128, scrollSettleUpdates: 3 },
  },
  cases: [],
};

let activeEditor = false;
let baseCamera;
let reportPath;

try {
  await client.connect(transport);
  for (const budget of budgets) {
    const id = budget === 0 ? "legacy" : `budget-${budget}`;
    console.log(`[gi-update-budget-validation] ${id}: iniciando`);
    const launch = await call("smile_run_editor", {
      configuration: "Release",
      scenePath,
      waitUntilReady: true,
      startupTimeoutSeconds: 240,
    });
    activeEditor = true;

    const startupCamera = await call("smile_camera_get");
    if (!baseCamera) baseCamera = referenceView ? {
      position: { x: -14.4764862060547, y: 3.93282294273376, z: 0.278742998838425 },
      pitchDegrees: -9.050000190734863,
      yawDegrees: 78.75,
    } : startupCamera;
    await setCamera(baseCamera);
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
      cascadeCount,
      adaptiveRays: false,
      adaptiveHysteresis: false,
      interleavedUpdates: true,
      probeCompaction: true,
      updateRayBudget: budget,
      timeoutSeconds: 120,
    });

    const steady = await waitForGI((status) => {
      const ddgi = status.ddgi;
      if (!ddgi?.initialized || ddgi.actualCascadeCount !== cascadeCount ||
          ddgi.updateRayBudget !== budget || ddgi.updateSerial < applied.ddgi.updateSerial + 200)
        return false;
      return budget === 0 ? !ddgi.updateBudgetEffective : ddgi.updateBudgetEffective;
    });
    const schedule = await sampleSchedule();
    if (budget > 0) {
      for (const [sampleIndex, ddgi] of schedule.entries()) {
        assert(ddgi.selectedRayCount <= budget,
          `${id}/amostra-${sampleIndex}: ${ddgi.selectedRayCount} raios excedem ${budget}`);
        const categorized = ddgi.newProbeCount + ddgi.regionalProbeCount +
          ddgi.followUpProbeCount + ddgi.quotaProbeCount + ddgi.roundRobinProbeCount;
        assert(categorized === ddgi.activeProbeCount,
          `${id}/amostra-${sampleIndex}: categorias=${categorized}, ativos=${ddgi.activeProbeCount}`);
      }
    }
    const profiles = await profileGPU(id);
    const staticCapture = await capture(`${id}/static`, 128);

    const fineSpacing = steady.ddgi.cascades[0].spacing;
    const movedCamera = {
      position: {
        x: baseCamera.position.x + fineSpacing * 2,
        y: baseCamera.position.y,
        z: baseCamera.position.z,
      },
      pitchDegrees: baseCamera.pitchDegrees,
      yawDegrees: baseCamera.yawDegrees,
    };
    const beforeMove = await call("smile_gi_status");
    await setCamera(movedCamera, false);
    const afterMove = await waitForGI((status) =>
      status.ddgi?.updateSerial >= beforeMove.ddgi.updateSerial + 3);
    const scrollCapture = await capture(`${id}/scroll`, 0);

    const manifest = staticCapture.manifest;
    assert(manifest.ddgiUpdateRayBudget === budget,
      `${id}: manifesto budget=${manifest.ddgiUpdateRayBudget}`);
    assert(manifest.ddgiAdaptiveRays === false && manifest.ddgiProbeCompaction === true,
      `${id}: regime divergiu no manifesto`);

    const caseResult = {
      id,
      budget,
      editorPid: launch.pid,
      steady,
      schedule,
      profiles,
      summary: {
        ddgiMilliseconds: profiles.map((profile) => passMean(profile, "DDGI")),
        frameMilliseconds: profiles.map((profile) => passMean(profile, "Frame (GPU)")),
      },
      staticCapture: {
        pngPath: staticCapture.pngPath,
        manifestPath: staticCapture.manifestPath,
        manifest: staticCapture.manifest,
      },
      scroll: {
        camera: movedCamera,
        afterMove,
        pngPath: scrollCapture.pngPath,
        manifestPath: scrollCapture.manifestPath,
        manifest: scrollCapture.manifest,
      },
      logAudit: await auditLogs(launch.pid),
    };
    report.cases.push(caseResult);
    await call("smile_close_editor", { timeoutSeconds: 60 });
    activeEditor = false;
    console.log(`[gi-update-budget-validation] ${id}: concluido`);
  }

  report.finishedAt = new Date().toISOString();
  report.passed = true;
  const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  reportPath = path.join(reportDirectory, `gi-update-budget-${sceneId}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-update-budget-validation] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-update-budget-validation] FAIL: ${report.error}`);
  throw error;
} finally {
  if (activeEditor) {
    try { await call("smile_close_editor", { timeoutSeconds: 60 }); } catch { /* best effort */ }
  }
  await client.close();
}
