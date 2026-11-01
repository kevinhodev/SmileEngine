// UMA rodada de A/B entre sidecars .somm (ex.: teto de subdivisao 8 contra 6). O sidecar e lido
// no LOAD da cena, entao cada braco e um editor novo; quem troca o arquivo entre as rodadas e o
// orquestrador. Esta rodada: abre a cena, confirma o estado do 2-state, aquece, perfila a GPU,
// captura um frame e fecha.
//
// Uso: node scripts/omm-sidecar-arm.mjs --label L8 --out rodada.json [--capture]

import { writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";

const argument = (name) => {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
};
const label = argument("--label") ?? "arm";
const outPath = argument("--out");
const capture = process.argv.includes("--capture");

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: { ...Object.fromEntries(Object.entries(process.env).filter(([, v]) => v !== undefined)),
         SMILE_ROOT: smileRoot },
  stderr: "pipe",
});
const client = new Client({ name: "smile-omm-sidecar-arm", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declared = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 45);
  const timeout = Math.max(90_000, (declared + 45) * 1_000);
  const result = await client.callTool({ name: toolName, arguments: args }, undefined,
                                       { timeout, maxTotalTimeout: timeout });
  const text = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${text}`);
  return JSON.parse(text || "{}");
}

const report = { label, startedAt: new Date().toISOString(), scenePath };
let launched = false;
try {
  await client.connect(transport);
  const launch = await call("smile_run_editor", {
    configuration: "Release", scenePath, waitUntilReady: true, startupTimeoutSeconds: 180,
  });
  launched = true;
  report.pid = launch.pid;
  const profileConfig = await call("smile_profile_configure", {
    preset: "gameplay_rr", bookmarkSlot: -1, timeOfDayHours: 10, backgroundThrottle: false,
  });
  if (profileConfig.backgroundThrottle !== false) throw new Error("throttle nao desligou");
  const status = await call("smile_gi_status", { timeoutSeconds: 5 });
  report.omm2State = status.omm2State;
  console.log(`[omm-arm ${label}] omm2State=${JSON.stringify(status.omm2State)}`);
  report.profile = await call("smile_profile_gpu", {
    warmupSeconds: 15, samples: 60, intervalMs: 250, includeSamples: false,
  });
  if (capture) {
    const frame = await call("smile_capture_frame", {
      bookmarkSlot: -1, warmupFrames: 256, preset: "scientific", resetHistory: true,
      pinTimeOfDay: true, timeOfDayHours: 10, timeoutSeconds: 300, includeImage: false,
    });
    report.capture = frame.pngPath;
    console.log(`[omm-arm ${label}] captura: ${frame.pngPath}`);
  }
} finally {
  try {
    if (launched) await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch (error) {
    console.error(`[omm-arm ${label}] cleanup: ${error}`);
  }
  await client.close();
  report.finishedAt = new Date().toISOString();
  if (outPath) await writeFile(path.resolve(outPath), JSON.stringify(report, null, 2));
}
