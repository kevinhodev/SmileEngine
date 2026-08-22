import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const configuration = "Release";
const bookmarkSlot = 1;
const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  configuration,
  preset: "controlled_nrd",
  timeOfDayHours: 22,
  bookmarkSlot,
  cases: [],
};

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
const client = new Client({ name: "smile-di-brdf-ratio-validation", version: "0.4.0" });

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function textOf(result) {
  return result.content.find((part) => part.type === "text")?.text ?? "";
}

function parseResult(result, toolName) {
  const text = textOf(result);
  if (result.isError) throw new Error(`${toolName}: ${text || "erro sem detalhe"}`);
  try {
    return JSON.parse(text || "{}");
  } catch {
    throw new Error(`${toolName}: resposta nao e JSON`);
  }
}

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

function pass(profile, fragment) {
  return profile.passes?.find((entry) => entry.name.includes(fragment)) ?? null;
}

async function runCase(enabled) {
  const id = enabled ? "on" : "off";
  const configured = await call("smile_profile_configure", {
    preset: "controlled_nrd",
    bookmarkSlot,
    timeOfDayHours: 22,
    diBrdfRatio: enabled,
    backgroundThrottle: false,
  });
  assert(configured.settings?.denoiser === 1,
    `${id}: controlled_nrd nao selecionou NRD`);
  assert(configured.settings?.restirDI === true,
    `${id}: ReSTIR DI nao ficou ligado`);
  assert(configured.settings?.diBrdfRatio === enabled,
    `${id}: readback diBrdfRatio=${configured.settings?.diBrdfRatio}`);

  const profile = await call("smile_profile_gpu", {
    warmupSeconds: 5,
    samples: 16,
    intervalMs: 200,
    includeSamples: false,
  });
  const capture = await call("smile_capture_frame", {
    bookmarkSlot,
    warmupFrames: 64,
    preset: "scientific",
    resetHistory: true,
    pinTimeOfDay: true,
    timeOfDayHours: 22,
    timeoutSeconds: 600,
    includeImage: false,
  });
  assert(capture.manifest?.directDenoiserEffective === "NRD",
    `${id}: directDenoiserEffective=${capture.manifest?.directDenoiserEffective}`);
  assert(capture.manifest?.diBrdfRatioRequested === enabled,
    `${id}: manifesto requested=${capture.manifest?.diBrdfRatioRequested}`);
  assert(capture.manifest?.diBrdfRatioEffective === enabled,
    `${id}: manifesto effective=${capture.manifest?.diBrdfRatioEffective}`);

  const result = {
    id,
    enabled,
    configured,
    pngPath: capture.pngPath,
    manifestPath: capture.manifestPath,
    manifest: capture.manifest,
    profile: {
      gpu: profile.gpu,
      resolution: profile.resolution,
      cpuFps: profile.cpuFps,
      localVramBytes: profile.localVramBytes,
      restirDI: pass(profile, "ReSTIR DI"),
      spatialVisibility: pass(profile, "Espacial + visibilidade"),
      nrdDirect: pass(profile, "NRD Direct"),
      allPasses: profile.passes,
    },
  };
  report.cases.push(result);
  console.log(`[di-brdf-ratio] ${id}: ${capture.pngPath}`);
  return result;
}

let launch;
let reportPath;
try {
  await client.connect(transport);
  const tools = await client.listTools();
  const profileTool = tools.tools.find((tool) => tool.name === "smile_profile_configure");
  assert(profileTool, "smile_profile_configure nao foi publicado");
  const presetEnum = profileTool.inputSchema?.properties?.preset?.enum ?? [];
  assert(presetEnum.includes("controlled_nrd"), "schema MCP nao publicou controlled_nrd");
  assert(profileTool.inputSchema?.properties?.diBrdfRatio,
    "schema MCP nao publicou diBrdfRatio");

  // Validacao visual/performance nunca reutiliza uma instancia Debug. Fecha qualquer editor vivo,
  // compila os dois alvos Release pelo proprio MCP e so entao inicia a cena.
  const existing = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (existing.connected) await call("smile_close_editor", { timeoutSeconds: 60 });
  report.build = {
    shaders: await call("smile_compile_shaders", {
      configuration,
      timeoutSeconds: 900,
    }),
    editor: await call("smile_build", {
      configuration,
      target: "SmileEditor",
      timeoutSeconds: 900,
    }),
  };

  launch = await call("smile_run_editor", {
    configuration,
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  report.editor = launch;
  console.log(`[di-brdf-ratio] editor PID ${launch.pid}; cena=${scenePath}`);

  await runCase(true);
  await runCase(false);

  const logs = await call("smile_get_latest_logs", { sessions: 1, tailLines: 1000 });
  const lines = logs.sessions?.[0]?.lines ?? [];
  const fatalPatterns = [
    /D3D12.*ERROR/i,
    /DXGI_ERROR/i,
    /DEVICE_REMOVED/i,
    /GPU validation.*error/i,
    /\[ERR\s*\]/i,
  ];
  const fatalMatches = lines.filter((line) =>
    fatalPatterns.some((pattern) => pattern.test(line)));
  assert(fatalMatches.length === 0, `log contem erro GPU/API: ${fatalMatches.join(" | ")}`);
  report.logAudit = {
    passed: true,
    logPath: logs.sessions?.[0]?.path ?? null,
    fatalMatches,
  };
  report.finishedAt = new Date().toISOString();
  report.passed = true;

  const reportDirectory = path.join(
    smileRoot, "build", "bin", configuration, "Captures", "Validation",
  );
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  reportPath = path.join(reportDirectory, `di-brdf-ratio-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[di-brdf-ratio] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[di-brdf-ratio] FAIL: ${report.error}`);
  process.exitCode = 1;
} finally {
  try {
    const status = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (status.connected && launch && !launch.alreadyRunning) {
      await call("smile_close_editor", { timeoutSeconds: 60 });
    }
  } catch (cleanupError) {
    console.error(`[di-brdf-ratio] cleanup: ${cleanupError}`);
  }
  await client.close();
}
