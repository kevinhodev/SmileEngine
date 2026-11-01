// A/B do OMM 2-state nos raios de GI (Docs/OMM-AUDIT.md, P0).
//
// Sequencia intercalada off_a, on_a, off_b, on_b: deriva termica ou de cache entre rodadas
// aparece como diferenca entre _a e _b do MESMO braco, e nao como efeito do knob. Cada rodada so
// perfila depois de o status confirmar que o braco ESTA VALENDO (omm2State.active), e nao so
// pedido — a guarda de cobertura pode recusar, e um A/B em que o "on" nao ligou mede ruido.
//
// Uso: node scripts/omm-2state-ab.mjs [--out relatorio.json]
// Exige o sidecar .somm atual da cena: sem ele a guarda mantem 4-state e o script aborta.

import { writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const warmupSeconds = 10;
const samples = 60;
const intervalMs = 250;
// O knob invalida o historico do GI (mesmo dominio do FoliageShadows): dar tempo de reconvergir
// antes de perfilar, senao o braco mede o transiente da invalidacao.
const settleMilliseconds = 6_000;
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function parseResult(result, toolName) {
  const text = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${text || "erro sem detalhe"}`);
  return JSON.parse(text || "{}");
}

const outArgument = process.argv.indexOf("--out");
const outPath = outArgument >= 0 ? path.resolve(process.argv[outArgument + 1]) : null;

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: { ...Object.fromEntries(Object.entries(process.env).filter(([, v]) => v !== undefined)),
         SMILE_ROOT: smileRoot },
  stderr: "pipe",
});
const client = new Client({ name: "smile-omm-2state-ab", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declared = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 45);
  const timeout = Math.max(90_000, (declared + 45) * 1_000);
  return parseResult(
    await client.callTool({ name: toolName, arguments: args }, undefined,
                          { timeout, maxTotalTimeout: timeout }),
    toolName,
  );
}

async function waitForOmm(expectedActive, afterFrame, timeoutMs = 60_000) {
  const deadline = Date.now() + timeoutMs;
  let last;
  while (Date.now() < deadline) {
    last = await call("smile_gi_status", { timeoutSeconds: 5 });
    if (last.omm2State?.active === expectedActive && last.frameIndex >= afterFrame + 2) return last;
    await delay(100);
  }
  throw new Error(`omm2State.active nao chegou a ${expectedActive}; ultimo=${JSON.stringify(last?.omm2State)}`);
}

const sequence = [
  { id: "off_a", on: false },
  { id: "on_a",  on: true },
  { id: "off_b", on: false },
  { id: "on_b",  on: true },
];

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  regime: { preset: "gameplay_rr", timeOfDayHours: 10, backgroundThrottle: false,
            warmupSeconds, samples, intervalMs, settleMilliseconds,
            statistic: "timestamps GPU brutos; mediana por rodada, media das duas rodadas do braco" },
  runs: [],
};

let launched = false;
try {
  await client.connect(transport);
  const launch = await call("smile_run_editor", {
    configuration: "Release", scenePath, waitUntilReady: true, startupTimeoutSeconds: 180,
  });
  launched = true;
  report.editor = { pid: launch.pid };
  console.log(`[omm-ab] editor PID ${launch.pid}`);

  const initial = await call("smile_gi_status", { timeoutSeconds: 5 });
  report.coverage = initial.omm2State;
  assert(initial.omm2State && initial.omm2State.unknownTriangles === 0,
    `cena com ${initial.omm2State?.unknownTriangles} triangulos sem micro-map: a guarda nao deixa ` +
    `o 2-state ligar. Rode SmileOmmBaker na cena.`);
  const camera = await call("smile_camera_get", { timeoutSeconds: 5 });
  report.camera = camera;

  for (const step of sequence) {
    console.log(`[omm-ab] ${step.id}: configurando`);
    await call("smile_camera_set", {
      x: camera.position.x, y: camera.position.y, z: camera.position.z,
      pitchDegrees: camera.pitchDegrees, yawDegrees: camera.yawDegrees, cameraCut: true,
    });
    const profile = await call("smile_profile_configure", {
      preset: "gameplay_rr", bookmarkSlot: -1, timeOfDayHours: 10, backgroundThrottle: false,
    });
    assert(profile.backgroundThrottle === false, `${step.id}: throttle nao desligou`);
    const applied = await call("smile_gi_configure", { omm2State: step.on, timeoutSeconds: 60 });
    const status = await waitForOmm(step.on, applied.frameIndex);
    await delay(settleMilliseconds);
    console.log(`[omm-ab] ${step.id}: perfilando (active=${status.omm2State.active})`);
    const gpu = await call("smile_profile_gpu", {
      warmupSeconds, samples, intervalMs, includeSamples: false,
    });
    report.runs.push({ id: step.id, on: step.on, omm2State: status.omm2State, profile: gpu });
  }

  // Imagem dos dois bracos, mesma camera, historico zerado e aquecido: a diferenca visivel do
  // 2-state no GI (folhagem mais "grossa" para o indireto) tem de ser julgada no pixel.
  report.captures = {};
  for (const on of [false, true]) {
    const applied = await call("smile_gi_configure", { omm2State: on, timeoutSeconds: 60 });
    await waitForOmm(on, applied.frameIndex);
    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1, warmupFrames: 256, preset: "scientific", resetHistory: true,
      pinTimeOfDay: true, timeOfDayHours: 10, timeoutSeconds: 300, includeImage: false,
    });
    report.captures[on ? "on" : "off"] = capture.pngPath;
    console.log(`[omm-ab] captura ${on ? "on" : "off"}: ${capture.pngPath}`);
  }

  // Resumo: por passe, media das medianas das duas rodadas de cada braco.
  const arms = { off: report.runs.filter((r) => !r.on), on: report.runs.filter((r) => r.on) };
  const keys = new Set();
  for (const run of report.runs) for (const p of run.profile.passes) keys.add(`${p.queue}|${p.name}`);
  const medianOf = (run, key) => {
    const [queue, name] = key.split("|");
    return run.profile.passes.find((p) => p.queue === queue && p.name === name)
      ?.rawMilliseconds?.median;
  };
  const mean = (values) => {
    const finite = values.filter(Number.isFinite);
    return finite.length ? finite.reduce((s, v) => s + v, 0) / finite.length : null;
  };
  report.summary = [...keys].map((key) => {
    const off = mean(arms.off.map((run) => medianOf(run, key)));
    const on = mean(arms.on.map((run) => medianOf(run, key)));
    const offSpread = Math.abs((medianOf(arms.off[0], key) ?? NaN) - (medianOf(arms.off[1], key) ?? NaN));
    return { pass: key, offMs: off, onMs: on,
             deltaMs: off !== null && on !== null ? on - off : null,
             deltaPct: off ? ((on - off) / off) * 100 : null,
             offRunSpreadMs: Number.isFinite(offSpread) ? offSpread : null };
  }).sort((a, b) => (a.deltaMs ?? 0) - (b.deltaMs ?? 0));

  console.log("\n[omm-ab] passe | off ms | on ms | delta ms | delta % | spread off a/b");
  for (const row of report.summary)
    console.log(`${row.pass} | ${row.offMs?.toFixed(3)} | ${row.onMs?.toFixed(3)} | ` +
                `${row.deltaMs?.toFixed(3)} | ${row.deltaPct?.toFixed(1)} | ${row.offRunSpreadMs?.toFixed(3)}`);
} finally {
  try {
    if (launched) await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch (error) {
    console.error(`[omm-ab] cleanup: ${error}`);
  }
  await client.close();
  report.finishedAt = new Date().toISOString();
  if (outPath) {
    await writeFile(outPath, JSON.stringify(report, null, 2));
    console.log(`[omm-ab] relatorio: ${outPath}`);
  }
}
