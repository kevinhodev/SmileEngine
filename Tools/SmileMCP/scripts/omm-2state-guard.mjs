// O caso NEGATIVO do OMM 2-state (Docs/OMM-AUDIT.md, P0): numa cena com triangulos mascarados
// sem micro-map, PEDIR o 2-state tem de ser recusado. O 2-state transforma FULLY_UNKNOWN_OPAQUE
// em opaco solido; se a guarda falhar, os cards de folhagem viram quadrados para o GI, e nada
// quebra — so a imagem fica errada.
//
// Uso: rodar com o .somm da cena AUSENTE (o script nao mexe nele). Sai com erro se a cena
// estiver toda coberta (nao ha o que provar) ou se o pedido for aceito.

import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: { ...Object.fromEntries(Object.entries(process.env).filter(([, v]) => v !== undefined)),
         SMILE_ROOT: smileRoot },
  stderr: "pipe",
});
const client = new Client({ name: "smile-omm-2state-guard", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declared = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 45);
  const timeout = Math.max(90_000, (declared + 45) * 1_000);
  const result = await client.callTool({ name: toolName, arguments: args }, undefined,
                                       { timeout, maxTotalTimeout: timeout });
  const text = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${text}`);
  return JSON.parse(text || "{}");
}

let launched = false;
let failed = false;
try {
  await client.connect(transport);
  await call("smile_run_editor", {
    configuration: "Release", scenePath, waitUntilReady: true, startupTimeoutSeconds: 180,
  });
  launched = true;
  const before = await call("smile_gi_status", { timeoutSeconds: 5 });
  console.log(`[omm-guard] antes: ${JSON.stringify(before.omm2State)}`);
  if (!(before.omm2State?.unknownTriangles > 0))
    throw new Error("cena toda coberta: nao ha o que provar. Rode com o .somm ausente.");

  const applied = await call("smile_gi_configure", { omm2State: true, timeoutSeconds: 60 });
  // Alguns frames: a recusa nao pode ser so "ainda nao aplicou".
  let after = applied;
  for (let i = 0; i < 20 && after.frameIndex < applied.frameIndex + 10; ++i) {
    await delay(100);
    after = await call("smile_gi_status", { timeoutSeconds: 5 });
  }
  console.log(`[omm-guard] depois do pedido: ${JSON.stringify(after.omm2State)}`);
  if (after.omm2State.requested !== true) throw new Error("o pedido nem foi registrado");
  if (after.omm2State.active !== false) {
    failed = true;
    throw new Error("GUARDA FALHOU: 2-state ativo com triangulos sem micro-map");
  }
  console.log("[omm-guard] OK: pedido registrado e recusado pela guarda de cobertura");
} catch (error) {
  failed = true;
  console.error(`[omm-guard] ${error.message ?? error}`);
} finally {
  try {
    if (launched) await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch (error) {
    console.error(`[omm-guard] cleanup: ${error}`);
  }
  await client.close();
  process.exitCode = failed ? 1 : 0;
}
