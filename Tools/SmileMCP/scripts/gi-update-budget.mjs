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
const client = new Client({ name: "smile-gi-update-budget", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const timeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout, maxTotalTimeout: timeout },
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
  throw new Error(`GI nao atingiu o estado esperado; ultimo=${JSON.stringify(last)}`);
}

let editorStarted = false;
try {
  await client.connect(transport);
  const launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  editorStarted = true;
  console.log(`[gi-update-budget] editor PID ${launch.pid}`);

  const configured = await call("smile_gi_configure", {
    ddgiEnabled: true,
    indirectPrimary: "ddgi",
    indirectFallback: "ddgi",
    cascadeCount: 2,
    adaptiveRays: false,
    interleavedUpdates: true,
    probeCompaction: false,
    updateRayBudget: 65_536,
    timeoutSeconds: 120,
  });
  assert(configured.ddgi?.updateRayBudget === 65_536,
    "readback nao publicou updateRayBudget");

  const first = await waitForGI((status) =>
    status.ddgi?.updateBudgetEffective === true &&
    status.ddgi.effectiveProbeBudget === 1_024 &&
    status.ddgi.scheduledProbeCount === 1_024 &&
    status.ddgi.probeCompaction === false &&
    status.ddgi.probeCompactionEffective === true);

  const samples = [first.ddgi];
  let serial = first.ddgi.updateSerial;
  while (samples.length < 8) {
    const next = await waitForGI((status) =>
      status.ddgi?.updateSerial > serial && status.ddgi.updateBudgetEffective === true);
    serial = next.ddgi.updateSerial;
    samples.push(next.ddgi);
  }

  for (let index = 1; index < samples.length; ++index) {
    const previous = samples[index - 1];
    const current = samples[index];
    const expectedStart = (previous.scheduledProbeStart + previous.scheduledProbeCount) %
                          previous.totalProbes;
    assert(current.scheduledProbeStart === expectedStart,
      `cursor saltou: esperado ${expectedStart}, veio ${current.scheduledProbeStart}`);
    assert(current.scheduledProbeCount === 1_024,
      `janela deixou o teto: ${current.scheduledProbeCount}`);
  }

  const disabled = await call("smile_gi_configure", { updateRayBudget: 0 });
  assert(disabled.ddgi?.updateRayBudget === 0, "nao restaurou scheduler legado");
  await waitForGI((status) => status.ddgi?.updateBudgetEffective === false);

  console.log(`GI update budget OK: ${samples.length} janelas, ` +
              `${samples[0].totalProbes} probes totais, teto 65.536 rays/1.024 probes.`);
} finally {
  if (editorStarted) {
    try { await call("smile_close_editor", { timeoutSeconds: 60 }); } catch { /* best effort */ }
  }
  await client.close();
}
