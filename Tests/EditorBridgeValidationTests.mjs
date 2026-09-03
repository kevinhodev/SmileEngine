// Validacao de borda do snapshot de perfil (Tools/SmileMCP/src/editor-bridge.ts).
//
// Existe porque o protocolo 4 subiu para EXIGIR o bloco cpu.gpuSlots, e exigencia que nao e
// validada existe so no numero da versao: um payload sem o bloco, ou com um campo string/undefined,
// atravessava o cast do TypeScript e quebrava la adiante no summarizeProfile com um TypeError.
//
// A diferenca nao e cosmetica. Erro na BORDA diz "o editor mandou snapshot sem cpu.gpuSlots";
// TypeError no agregador diz "cannot read properties of undefined", numa linha que nao tem nada a
// ver com a causa, depois de o benchmark ja ter rodado.
//
// O bridge fala por named pipe com o editor, entao aqui ele e exercitado por um FAKE de pipe: o
// que esta sob teste e a validacao, nao o transporte.

import assert from "node:assert";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const bridgeUrl = new URL(
  "../Tools/SmileMCP/dist/editor-bridge.js",
  `file://${here.replace(/\\/g, "/")}/`,
);

let failures = 0;
function check(condition, what, detail = "") {
  if (condition) return;
  console.error(`[FALHA] ${what}${detail ? `\n        ${detail}` : ""}`);
  failures += 1;
}

// Snapshot minimo VALIDO, no formato do protocolo 4.
function snapshotValido() {
  return {
    direct: [],
    asyncCompute: [],
    directGpuSourceFrameIndex: 10,
    asyncComputeSourceFrameIndex: null,
    frameIndex: 10,
    cpu: {
      sourceFrameIndex: 10,
      timingsMs: { buildDrawLists: 1, cullSort: 1, batchSort: 1, shadowLists: 1, submit: 1 },
      passes: [],
      scene: { renderables: 1, drawItems: 1, visibleItems: 1, occludedItems: 0 },
      objectConstantBytes: 0,
      srvHeap: { used: 1, highWater: 1, capacity: 100 },
      gpuSlots: { capacity: 8, live: 8, retired: 0, exhausted: 0 },
    },
  };
}

// Pipe falso: responde a UMA requisicao com o payload dado.
async function comEditorFalso(payload, body) {
  // O bridge monta o caminho a partir de SMILE_MCP_PIPE, que aceita SO o nome (ele recusa barras).
  const nome = `smile-bridge-test-${process.pid}-${Math.random().toString(36).slice(2)}`;
  const pipeName = `\\\\.\\pipe\\${nome}`;
  process.env.SMILE_MCP_PIPE = nome;
  const server = net.createServer((socket) => {
    let buffer = "";
    socket.on("data", (chunk) => {
      buffer += chunk.toString("utf8");
      let index;
      while ((index = buffer.indexOf("\n")) >= 0) {
        const linha = buffer.slice(0, index);
        buffer = buffer.slice(index + 1);
        let pedido;
        try {
          pedido = JSON.parse(linha);
        } catch {
          continue;
        }
        socket.write(
          JSON.stringify({
            version: 4,
            id: pedido.id,
            ok: true,
            result: typeof payload === "function" ? payload(pedido) : payload,
          }) + "\n",
        );
      }
    });
    socket.on("error", () => {});
  });

  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(pipeName, resolve);
  });
  try {
    await body();
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
}

const { SmileEditorBridge } = await import(bridgeUrl.href);
const raizDoProjeto = path.resolve(here, "..");

async function esperaErro(payload, trecho, rotulo) {
  await comEditorFalso(payload, async () => {
    const bridge = new SmileEditorBridge(raizDoProjeto);
    let erro = null;
    try {
      await bridge.profileSnapshot(3000);
    } catch (e) {
      erro = e;
    }
    check(erro !== null, `${rotulo}: tem de ser recusado`);
    if (erro) {
      check(
        String(erro.message).includes(trecho),
        `${rotulo}: a mensagem tem de citar '${trecho}'`,
        String(erro.message),
      );
      // O erro tem de vir da BORDA, e nao de um TypeError la adiante.
      check(
        !(erro instanceof TypeError),
        `${rotulo}: tem de ser erro de validacao, e nao TypeError`,
        String(erro),
      );
    }
  });
}

// 1. Caminho valido continua passando -- senao uma validacao que recusa tudo ficaria verde.
await comEditorFalso(snapshotValido(), async () => {
  const bridge = new SmileEditorBridge(raizDoProjeto);
  const snapshot = await bridge.profileSnapshot(3000);
  check(snapshot.cpu.gpuSlots.capacity === 8, "payload valido atravessa");
});

// 2. Bloco ausente.
{
  const p = snapshotValido();
  delete p.cpu.gpuSlots;
  await esperaErro(p, "cpu.gpuSlots", "gpuSlots ausente");
}

// 3. Bloco de tipo errado.
for (const valor of [null, "oito", 42, []]) {
  const p = snapshotValido();
  p.cpu.gpuSlots = valor;
  await esperaErro(p, "gpuSlots", `gpuSlots como ${JSON.stringify(valor)}`);
}

// 4. Campo faltando, ou com tipo/valor invalido. NaN e Infinity sao `number` e passariam por um
//    typeof -- e um contador que vira NaN contamina a agregacao inteira em silencio.
for (const campo of ["capacity", "live", "retired", "exhausted"]) {
  for (const valor of [undefined, "8", null, NaN, Infinity]) {
    const p = snapshotValido();
    if (valor === undefined) delete p.cpu.gpuSlots[campo];
    else p.cpu.gpuSlots[campo] = valor;
    await esperaErro(p, campo, `${campo} = ${String(valor)}`);
  }
}

if (failures > 0) {
  console.error(`[BRIDGE] ${failures} falha(s)`);
  process.exit(1);
}
console.log("[BRIDGE] ok - cpu.gpuSlots exigido e validado na borda");
