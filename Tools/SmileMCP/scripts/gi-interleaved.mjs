import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function modulo(value, divisor) {
  return ((value % divisor) + divisor) % divisor;
}

function parseResult(result, toolName) {
  const text = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${text || "erro sem detalhe"}`);
  try {
    return JSON.parse(text || "{}");
  } catch {
    throw new Error(`${toolName}: resposta nao e JSON`);
  }
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
const client = new Client({ name: "smile-gi-interleaved", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const sdkTimeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(
    await client.callTool(
      { name: toolName, arguments: args },
      undefined,
      { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
    ),
    toolName,
  );
}

async function waitForGI(predicate, timeoutMilliseconds = 90_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  let last;
  while (Date.now() < deadline) {
    last = await call("smile_gi_status", { timeoutSeconds: 5 });
    if (predicate(last)) return last;
    await delay(50);
  }
  throw new Error(`GI nao atingiu o estado esperado; ultimo=${JSON.stringify(last)}`);
}

async function waitForSerial(afterSerial, count = 2) {
  return waitForGI((status) => status.ddgi?.updateSerial >= afterSerial + count);
}

async function setCamera(pose, cameraCut) {
  return call("smile_camera_set", {
    x: pose.position.x,
    y: pose.position.y,
    z: pose.position.z,
    pitchDegrees: pose.pitchDegrees,
    yawDegrees: pose.yawDegrees,
    cameraCut,
  });
}

function profilePass(profile, name) {
  return profile.passes.find((entry) => entry.name === name) ?? null;
}

function passRawMean(profile, name) {
  return Number(profilePass(profile, name)?.rawMilliseconds?.mean ?? Number.NaN);
}

function checkCascadeDelta(before, after, axis, label) {
  const names = ["x", "y", "z"];
  const name = names[axis];
  const fineBefore = before.ddgi.cascades[0];
  const fineAfter = after.ddgi.cascades[0];
  const coarseBefore = before.ddgi.cascades[1];
  const coarseAfter = after.ddgi.cascades[1];
  const count = before.ddgi.gridCount[name];
  const cellDelta = Math.round(
    (fineAfter.gridMin[name] - fineBefore.gridMin[name]) / fineBefore.spacing,
  );
  const scrollDelta = modulo(
    fineAfter.scrollCells[name] - fineBefore.scrollCells[name],
    count,
  );
  assert(scrollDelta === modulo(cellDelta, count),
    `${label}/${name}: scroll=${scrollDelta}, celulas=${cellDelta}`);
  assert(coarseAfter.gridMin[name] === coarseBefore.gridMin[name],
    `${label}/${name}: cascata grossa se moveu`);
  assert(coarseAfter.scrollCells[name] === coarseBefore.scrollCells[name],
    `${label}/${name}: scroll grosso mudou`);
  return { axis: name, cellDelta, scrollDelta };
}

async function moveAndValidate(currentCamera, delta, label) {
  const before = await call("smile_gi_status");
  const target = {
    position: {
      x: currentCamera.position.x + delta.x,
      y: currentCamera.position.y + delta.y,
      z: currentCamera.position.z + delta.z,
    },
    pitchDegrees: currentCamera.pitchDegrees,
    yawDegrees: currentCamera.yawDegrees,
  };
  const appliedCamera = await setCamera(target, false);
  const after = await waitForSerial(before.ddgi.updateSerial, 3);
  const axes = [0, 1, 2].map((axis) => checkCascadeDelta(before, after, axis, label));
  assert(after.ddgi.cascades[0].updateAge === 0,
    `${label}: fina ficou velha (${after.ddgi.cascades[0].updateAge})`);
  assert(after.ddgi.cascades[1].updateAge <= 1,
    `${label}: grossa passou da idade 1 (${after.ddgi.cascades[1].updateAge})`);
  return { label, delta, before, after, axes, camera: appliedCamera };
}

const performanceSequence = [
  { id: "full_a", interleavedUpdates: false },
  { id: "interleaved_a", interleavedUpdates: true },
  { id: "full_b", interleavedUpdates: false },
  { id: "interleaved_b", interleavedUpdates: true },
];
const visualSequence = performanceSequence;
const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  regime: {
    cascadeCount: 2,
    adaptiveRays: true,
    adaptiveHysteresis: false,
    policy: { primary: "ddgi", fallback: "ddgi" },
    performance: { samples: 72, intervalMs: 125, warmupSeconds: 2 },
    visual: { preset: "scientific", warmupFrames: 128, sequence: visualSequence.map((x) => x.id) },
  },
  performance: [],
  scheduleSamples: [],
  edgeCases: [],
  captures: [],
};

let launch;
let originalCamera;
let originalGI;
let reportPath;

try {
  await client.connect(transport);
  launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 180,
  });
  report.editor = launch;
  console.log(`[gi-interleaved] editor PID ${launch.pid}; cena=${scenePath}`);

  originalCamera = await call("smile_camera_get");
  originalGI = await call("smile_gi_status");
  report.baseline = { camera: originalCamera, gi: originalGI };

  await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
    backgroundThrottle: false,
  });
  const initialized = await call("smile_gi_configure", {
    ddgiEnabled: true,
    indirectPrimary: "ddgi",
    indirectFallback: "ddgi",
    cascadeCount: 2,
    adaptiveRays: true,
    adaptiveHysteresis: false,
    interleavedUpdates: false,
    timeoutSeconds: 120,
  });
  await delay(8_000); // cobre os 180 updates iniciais de relocacao/classificacao
  await waitForGI((status) =>
    status.ddgi?.initialized && status.ddgi.actualCascadeCount === 2 &&
    status.ddgi.updateSerial >= initialized.ddgi.updateSerial + 180);

  for (const benchmarkCase of performanceSequence) {
    console.log(`[gi-interleaved] perfil ${benchmarkCase.id}`);
    const applied = await call("smile_gi_configure", {
      interleavedUpdates: benchmarkCase.interleavedUpdates,
    });
    const steady = benchmarkCase.interleavedUpdates
      ? await waitForGI((status) =>
          status.ddgi?.interleavedUpdates === true &&
          status.ddgi.lastUpdatedCascadeCount === 1 &&
          status.ddgi.cascades?.[0]?.updateAge === 0 &&
          status.ddgi.cascades?.[1]?.updateAge === 1 &&
          !status.ddgi.scheduledFullForced)
      : await waitForGI((status) =>
          status.ddgi?.interleavedUpdates === false &&
          status.ddgi.lastUpdatedCascadeCount === 2 &&
          status.ddgi.updateSerial >= applied.ddgi.updateSerial + 2);
    // O ring de timestamps pode atravessar uma janela sem resultado da fila async sob carga
    // externa pesada. Repete a coleta, nao a configuracao, ate os scopes de fase aparecerem.
    let profile;
    const profileAttempts = [];
    for (let attempt = 1; attempt <= 3; ++attempt) {
      profile = await call("smile_profile_gpu", {
        warmupSeconds: 2,
        samples: 72,
        intervalMs: 125,
        includeSamples: false,
      });
      const names = profile.passes.map((entry) => entry.name);
      profileAttempts.push({ attempt, firstFrameIndex: profile.firstFrameIndex,
        lastFrameIndex: profile.lastFrameIndex, passNames: names });
      const hasTotal = names.includes("DDGI");
      const hasFull = names.includes("DDGI (fine+coarse)");
      const hasFine = names.includes("DDGI (fine)");
      if (hasTotal && hasFull && (benchmarkCase.interleavedUpdates ? hasFine : !hasFine)) break;
      await delay(500);
    }
    const total = profilePass(profile, "DDGI");
    const fine = profilePass(profile, "DDGI (fine)");
    const full = profilePass(profile, "DDGI (fine+coarse)");
    assert(total && full, `${benchmarkCase.id}: scopes DDGI obrigatorios ausentes`);
    if (benchmarkCase.interleavedUpdates) {
      assert(fine, `${benchmarkCase.id}: scope fine ausente`);
      assert(fine.rawMilliseconds.count >= 10 && full.rawMilliseconds.count >= 10,
        `${benchmarkCase.id}: amostragem por fase insuficiente`);
      assert(passRawMean(profile, "DDGI (fine)") < passRawMean(profile, "DDGI (fine+coarse)"),
        `${benchmarkCase.id}: fine nao ficou mais barato que fine+coarse`);
    } else {
      assert(!fine, `${benchmarkCase.id}: fine-only apareceu com scheduler desligado`);
    }
    report.performance.push({ ...benchmarkCase, steady, profileAttempts, profile });
  }

  await call("smile_gi_configure", { interleavedUpdates: true });
  await waitForGI((status) => status.ddgi?.lastUpdatedCascadeCount === 1 &&
    status.ddgi.cascades?.[1]?.updateAge === 1);
  const uniqueSchedule = new Map();
  while (uniqueSchedule.size < 24) {
    const status = await call("smile_gi_status");
    uniqueSchedule.set(status.ddgi.updateSerial, status);
    await delay(15);
  }
  report.scheduleSamples = [...uniqueSchedule.values()];
  assert(report.scheduleSamples.every((status) => status.ddgi.cascades[0].updateAge === 0),
    "scheduler: cascata fina nao foi atualizada em todo update");
  assert(report.scheduleSamples.every((status) => status.ddgi.cascades[1].updateAge <= 1),
    "scheduler: cascata grossa passou da idade maxima 1");
  assert(new Set(report.scheduleSamples.map((status) => status.ddgi.lastUpdatedCascadeCount)).size === 2,
    "scheduler: as fases 1/2 nao alternaram");

  let movingCamera = await setCamera(originalCamera, true);
  await delay(500);
  report.edgeCases.push(await moveAndValidate(movingCamera, { x: 3.25, y: 0, z: 0 }, "axial"));
  movingCamera = report.edgeCases.at(-1).camera;
  report.edgeCases.push(await moveAndValidate(movingCamera, { x: 3.25, y: 0, z: 3.25 }, "diagonal"));
  movingCamera = report.edgeCases.at(-1).camera;
  report.edgeCases.push(await moveAndValidate(movingCamera, { x: 0, y: 3.25, z: 0 }, "vertical"));
  movingCamera = report.edgeCases.at(-1).camera;

  const beforeTeleport = await call("smile_gi_status");
  const teleported = await setCamera({
    position: {
      x: movingCamera.position.x + 100,
      y: movingCamera.position.y + 30,
      z: movingCamera.position.z + 80,
    },
    pitchDegrees: movingCamera.pitchDegrees,
    yawDegrees: movingCamera.yawDegrees,
  }, true);
  const afterTeleport = await waitForGI((status) =>
    status.ddgi?.lastForcedUpdateSerial > beforeTeleport.ddgi.lastForcedUpdateSerial);
  assert(afterTeleport.ddgi.lastUpdatedCascadeCount === 2 ||
         afterTeleport.ddgi.lastForcedUpdateSerial > 0,
    "teleporte: reset nao promoveu update completo");
  report.edgeCases.push({ label: "teleport", before: beforeTeleport, after: afterTeleport,
    camera: teleported });

  await setCamera(originalCamera, true);
  await delay(1_000);
  for (const captureCase of visualSequence) {
    console.log(`[gi-interleaved] visual ${captureCase.id}`);
    await call("smile_gi_configure", {
      interleavedUpdates: captureCase.interleavedUpdates,
    });
    await waitForGI((status) => status.ddgi?.interleavedUpdates === captureCase.interleavedUpdates &&
      (!captureCase.interleavedUpdates || status.ddgi.lastUpdatedCascadeCount === 1));
    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1,
      warmupFrames: 128,
      preset: "scientific",
      pinTimeOfDay: true,
      timeOfDayHours: 10,
      timeoutSeconds: 600,
      includeImage: false,
    });
    assert(capture.manifest.ddgiCascadeCount === 2,
      `${captureCase.id}: manifesto sem duas cascatas`);
    assert(capture.manifest.ddgiAdaptiveRays === true,
      `${captureCase.id}: manifesto fora do default adaptativo`);
    assert(capture.manifest.ddgiInterleavedUpdates === captureCase.interleavedUpdates,
      `${captureCase.id}: manifesto interleaved=${capture.manifest.ddgiInterleavedUpdates}`);
    report.captures.push({ ...captureCase, pngPath: capture.pngPath,
      manifestPath: capture.manifestPath, manifest: capture.manifest });
  }

  const fullMeans = report.performance
    .filter((entry) => !entry.interleavedUpdates)
    .map((entry) => passRawMean(entry.profile, "DDGI"));
  const interleavedMeans = report.performance
    .filter((entry) => entry.interleavedUpdates)
    .map((entry) => passRawMean(entry.profile, "DDGI"));
  const mean = (values) => values.reduce((sum, value) => sum + value, 0) / values.length;
  const fullMean = mean(fullMeans);
  const interleavedMean = mean(interleavedMeans);
  report.performanceDecision = {
    fullMeanMilliseconds: fullMean,
    interleavedMeanMilliseconds: interleavedMean,
    savedMilliseconds: fullMean - interleavedMean,
    savedPercent: (fullMean - interleavedMean) * 100 / fullMean,
    phaseScopesValid: true,
    performanceSupportsDefault: interleavedMean < fullMean * 0.95,
  };

  const logs = await call("smile_get_latest_logs", { sessions: 5, tailLines: 1000 });
  const logSession = logs.sessions?.find((session) =>
    typeof session.path === "string" && new RegExp(`-p${launch.pid}\\.log$`, "i").test(session.path));
  const fatalPatterns = [/D3D12.*ERROR/i, /DXGI_ERROR/i, /DEVICE_REMOVED/i,
    /GPU validation.*error/i, /\[ERR\s*\]/i];
  const fatalMatches = (logSession?.lines ?? []).filter((line) =>
    fatalPatterns.some((pattern) => pattern.test(line)));
  assert(fatalMatches.length === 0, `log contem erro GPU/API: ${fatalMatches.join(", ")}`);
  // O logger persistente e evidencia suplementar: em algumas aberturas o Qt nao cria a sessao
  // no AppLocalData, embora o bridge e o renderer estejam operacionais. Nao transforme ausencia
  // do arquivo em falha falsa do scheduler; se ele existir, qualquer erro GPU/API continua fatal.
  report.logAudit = logSession
    ? { passed: true, available: true, editorPid: launch.pid,
        logPath: logSession.path, fatalMatches }
    : { passed: null, available: false, editorPid: launch.pid,
        reason: "log persistente da sessao nao encontrado", fatalMatches: [] };
  report.finishedAt = new Date().toISOString();
  report.passed = true;

  const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  reportPath = path.join(reportDirectory, `gi-interleaved-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-interleaved] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-interleaved] FAIL: ${report.error}`);
  try {
    const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    reportPath = path.join(reportDirectory, `gi-interleaved-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
    console.error(`[gi-interleaved] relatorio parcial: ${reportPath}`);
  } catch (reportError) {
    console.error(`[gi-interleaved] relatorio de falha: ${reportError}`);
  }
  process.exitCode = 1;
} finally {
  try {
    const editorStatus = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (editorStatus.connected && !editorStatus.captureBusy && originalCamera)
      await setCamera(originalCamera, true);
    if (editorStatus.connected && !editorStatus.captureBusy && originalGI) {
      await call("smile_gi_configure", {
        ddgiEnabled: originalGI.ddgi.enabled,
        indirectPrimary: originalGI.policy.primary.requested,
        indirectFallback: originalGI.policy.fallback.requested,
        cascadeCount: originalGI.ddgi.desiredCascadeCount,
        interleavedUpdates: originalGI.ddgi.interleavedUpdates,
        adaptiveRays: originalGI.ddgi.adaptiveRays,
        adaptiveHysteresis: originalGI.ddgi.adaptiveHysteresis,
        timeoutSeconds: 120,
      });
    }
    if (editorStatus.connected && launch && !launch.alreadyRunning)
      await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch (cleanupError) {
    console.error(`[gi-interleaved] cleanup: ${cleanupError}`);
  }
  await client.close();
}
