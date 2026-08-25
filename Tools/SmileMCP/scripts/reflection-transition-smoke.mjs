import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(scriptDirectory, "../../..");
const scenePath = process.argv[2] ?? "Assets/Scenes/Bistro/BistroExterior.sscene";
const cycleCount = Number(process.env.SMILE_REFLECTION_TOGGLE_CYCLES ?? "8");
if (!Number.isInteger(cycleCount) || cycleCount < 1) {
  throw new Error("SMILE_REFLECTION_TOGGLE_CYCLES deve ser um inteiro positivo.");
}

const environment = {
  ...Object.fromEntries(Object.entries(process.env).filter((entry) => entry[1] !== undefined)),
  SMILE_ROOT: smileRoot,
};
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: environment,
  stderr: "pipe",
});
const client = new Client({ name: "reflection-transition-smoke", version: "1.0.0" });
let editorStarted = false;

const callJson = async (name, args) => {
  const result = await client.callTool({ name, arguments: args });
  if (result.isError) {
    const message = result.content.find((part) => part.type === "text")?.text;
    throw new Error(message || name + " retornou erro");
  }
  const text = result.content.find((part) => part.type === "text")?.text;
  return JSON.parse(text || "{}");
};

try {
  await client.connect(transport);
  const launch = await callJson("smile_run_editor", {
    configuration: "Release",
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  if (!launch.pid) throw new Error("smile_run_editor nao retornou o PID.");
  editorStarted = true;

  const regimes = [
    { label: "Local", preset: "controlled_native", denoiser: 0, manifest: "None" },
    { label: "NRD", preset: "controlled_nrd", denoiser: 1, manifest: "NRD" },
    { label: "DLSS-RR", preset: "gameplay_rr", denoiser: 2, manifest: "DLSS_RR" },
  ];
  const transitions = [regimes[1], regimes[0], regimes[2], regimes[0]];
  for (let cycle = 0; cycle < cycleCount; ++cycle) {
    for (const regime of transitions) {
      const state = await callJson("smile_profile_configure", {
        preset: regime.preset,
        bookmarkSlot: -1,
        timeOfDayHours: 10,
        backgroundThrottle: false,
      });
      if (state.settings?.denoiser !== regime.denoiser) {
        throw new Error(
          regime.preset + ": denoiser efetivo " + state.settings?.denoiser +
          ", esperado " + regime.denoiser,
        );
      }
    }
  }

  const captures = [];
  for (const regime of regimes) {
    const state = await callJson("smile_profile_configure", {
      preset: regime.preset,
      bookmarkSlot: -1,
      timeOfDayHours: 10,
      backgroundThrottle: false,
    });
    if (state.settings?.denoiser !== regime.denoiser) {
      throw new Error(regime.label + ": configuracao final divergente.");
    }

    const capture = await callJson("smile_capture_frame", {
      bookmarkSlot: -1,
      warmupFrames: 48,
      preset: "gameplay",
      resetHistory: false,
      pinTimeOfDay: true,
      timeOfDayHours: 10,
      timeoutSeconds: 180,
      includeImage: false,
    });
    if (!capture.pngPath || capture.manifest?.denoiser !== regime.manifest ||
        capture.manifest?.reflections !== true || capture.manifest?.historyReset !== false) {
      throw new Error(regime.label + ": captura final invalida.");
    }
    captures.push(regime.label + ": " + capture.pngPath);
  }

  console.log(
    "Reflection transition smoke OK (" + cycleCount + " ciclos)\n" + captures.join("\n"),
  );
} finally {
  if (editorStarted) {
    try {
      await callJson("smile_close_editor", { timeoutSeconds: 60 });
    } catch (error) {
      console.error("Falha ao encerrar o editor de teste:", error);
    }
  }
  await client.close();
}
