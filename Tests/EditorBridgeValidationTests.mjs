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

import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(here, "..", "Tools", "SmileMCP");

// COMPILA o bridge antes de importar, para um diretorio proprio.
//
// Importar de Tools/SmileMCP/dist tornava o teste nao reproduzivel: `dist/` e ignorado pelo git
// e nenhum fixture roda `npm run build`. Num checkout limpo o teste morria com
// ERR_MODULE_NOT_FOUND; com um dist antigo em disco, pior -- ficava VERDE validando codigo que
// nao e o do fonte, que e exatamente o defeito que a regua de baseline ja tinha tido.
//
// Chama o tsc por `node .../tsc` e nao por `npm run build`: npm no Windows e um .cmd, e o Node
// moderno recusa spawna-lo sem shell (EINVAL).
// O tsc vem de node_modules, entao as dependencias precisam estar instaladas. O CMake so
// confere o node, e um checkout limpo sem `npm ci` chegaria aqui com MODULE_NOT_FOUND -- um
// diagnostico que nao diz o que fazer. Falha cedo, dizendo.
const tscPath = path.join(packageRoot, "node_modules", "typescript", "bin", "tsc");
if (!existsSync(tscPath)) {
  console.error(
    "[FALHA] dependencias do SmileMCP nao instaladas: " + tscPath + " nao existe.\n" +
      "        Rode `npm ci` em Tools/SmileMCP antes desta suite.",
  );
  process.exit(1);
}

const outDir = mkdtempSync(path.join(os.tmpdir(), "smile-bridge-"));
try {
  execFileSync(
    process.execPath,
    [
      tscPath,
      "-p",
      path.join(packageRoot, "tsconfig.json"),
      "--outDir",
      outDir,
    ],
    { cwd: packageRoot, stdio: "pipe" },
  );
} catch (error) {
  console.error(
    "[FALHA] nao consegui compilar o bridge:\n" +
      String(error.stdout ?? "") + String(error.stderr ?? error.message),
  );
  rmSync(outDir, { recursive: true, force: true });
  process.exit(1);
}
// package.json com type=module no diretorio de saida. O tsc emite import/export, mas o
// temporario fica FORA do package.json do SmileMCP: sem este marcador o Node so acerta por
// deteccao automatica de sintaxe, que e do Node 24 -- com ela desligada, ou no Node 20 que o
// projeto declara suportar (engines: >=20), o import falha antes de qualquer assercao rodar.
writeFileSync(path.join(outDir, "package.json"), JSON.stringify({ type: "module" }), "utf8");

const bridgeUrl = pathToFileURL(path.join(outDir, "editor-bridge.js"));

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
        // `payload` como STRING vai crua, sem passar por JSON.stringify. Isso existe porque o
        // stringify converte NaN e Infinity em null, e um caso escrito com Infinity provaria
        // apenas que null e recusado -- uma regressao para `typeof v === "number"` continuaria
        // passando no teste. `1e400` e Infinity na borda JSON, e e escrivel a mao.
        const corpo =
          typeof payload === "string"
            ? // JSON.stringify e nao String: o id do bridge e STRING, e interpola-lo cru
              // produzia JSON invalido -- o teste falhava por payload malformado, e nao pelo
              // que ele queria provar.
              payload.replace("__ID__", JSON.stringify(pedido.id))
            : JSON.stringify({
                version: 4,
                id: pedido.id,
                ok: true,
                result: typeof payload === "function" ? payload(pedido) : payload,
              });
        socket.write(corpo + "\n");
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

// 4. Campo faltando, ou fora do contrato. Os valores vem de u32 no editor, entao negativo,
//    fracionario e acima de UINT32_MAX sao estados que a origem nao produz -- se chegarem, o
//    payload nao veio de onde diz que veio.
for (const campo of ["capacity", "live", "retired", "exhausted"]) {
  for (const valor of [undefined, "8", null, true, -1, 1.5, 4294967296]) {
    const p = snapshotValido();
    if (valor === undefined) delete p.cpu.gpuSlots[campo];
    else p.cpu.gpuSlots[campo] = valor;
    await esperaErro(p, campo, `${campo} = ${String(valor)}`);
  }
}

// 4b. Infinity pela borda JSON de verdade. NaN nao tem representacao JSON, entao nao ha caso
//     para ele aqui -- o que chega no lugar e null, ja coberto acima.
{
  const cru =
    '{"version":4,"id":__ID__,"ok":true,"result":' +
    JSON.stringify(snapshotValido()).replace('"capacity":8', '"capacity":1e400') +
    "}";
  await esperaErro(cru, "capacity", "capacity = 1e400 (Infinity na borda JSON)");
}

// 5. Estados IMPOSSIVEIS: os contadores particionam a capacidade, entao a soma nao pode
//    exceder. `live > capacity` e um snapshot que o allocator nao consegue produzir.
{
  const p = snapshotValido();
  p.cpu.gpuSlots = { capacity: 8, live: 9, retired: 0, exhausted: 0 };
  await esperaErro(p, "capacity", "live acima da capacidade");
}
{
  const p = snapshotValido();
  p.cpu.gpuSlots = { capacity: 8, live: 5, retired: 3, exhausted: 1 };
  await esperaErro(p, "capacity", "live + retired + exhausted acima da capacidade");
}

rmSync(outDir, { recursive: true, force: true });

if (failures > 0) {
  console.error(`[BRIDGE] ${failures} falha(s)`);
  process.exit(1);
}
console.log("[BRIDGE] ok - cpu.gpuSlots exigido, validado no contrato u32 e coerente");
