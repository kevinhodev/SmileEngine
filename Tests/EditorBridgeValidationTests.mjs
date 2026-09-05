// Validacao de borda do snapshot de perfil (Tools/SmileMCP/src/editor-bridge.ts).
//
// Existe porque o protocolo 4 subiu para EXIGIR o bloco cpu.gpuSlots — o 5, material/mesh — e
// o 6, a tabela canonica de instancias, e o 7, os contadores por passe da Fase 3.
// Exigencia que nao e
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

// Snapshot minimo VALIDO, no formato do protocolo 7.
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
      // NAO vazio: com lista vazia o laco de validacao por passe nao roda, e o snapshot valido
      // deixaria de exercitar o contrato que o protocolo 7 introduziu.
      passes: [{ key: "gbuffer", considered: 4, draws: 3, materialBinds: 0, meshBinds: 3,
                 skippedInvalid: 1, bindlessDraws: 3, materialRowSets: 2,
                 materialRowMisses: 0 }],
      scene: { renderables: 1, drawItems: 1, visibleItems: 1, occludedItems: 0 },
      objectConstantBytes: 0,
      srvHeap: { used: 1, highWater: 1, capacity: 100, retired: 0, overflows: 0 },
      gpuSlots: { capacity: 8, live: 8, retired: 0, exhausted: 0 },
      materialTable: { rows: 3, residentBytes: 2240, stagingBytes: 4480, rowsDirty: 0,
                       copyRegions: 0, rowsUploadedTotal: 3 },
      meshTable: { rows: 2, residentBytes: 1848, stagingBytes: 3696 },
      instanceTable: { rows: 8, residentBytes: 23040, stagingBytes: 46080,
                       rowsDirty: 0, bytesDirty: 0, copyRegions: 0,
                       bytesUploadedTotal: 1440 },
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
                version: 7,
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

// O par do esperaErro: prova que um valor LEGITIMO atravessa. Sem ele, um contrato apertado
// demais -- exatamente o que deu errado com o rowsUploadedTotal, u64 recebendo teto de u32 --
// ficaria verde, porque toda assercao do teste seria de rejeicao.
async function esperaOk(payload, rotulo) {
  await comEditorFalso(payload, async () => {
    const bridge = new SmileEditorBridge(raizDoProjeto);
    let erro = null;
    try {
      await bridge.profileSnapshot(3000);
    } catch (e) {
      erro = e;
    }
    check(erro === null, `${rotulo}: nao pode ser recusado`, erro ? String(erro.message) : "");
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
    '{"version":7,"id":__ID__,"ok":true,"result":' +
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

// 6. Tabelas canonicas da Fase 2. Mesmo contrato do gpuSlots, e pela mesma razao: elas entraram
//    no RESUMO, que e o que a regua le com o includeSamples default. Enquanto a exigencia
//    existisse so no numero da versao, um editor anterior a Fase 2 passaria e o relatorio sairia
//    com null no lugar do numero do gate -- parecendo completo.
//
//    DOIS contratos. Contagem e u32 no produtor; bytes e acumulados sao u64 -- o
//    rowsUploadedTotal soma desde o ultimo setup e nao volta a zero por frame. Exigir a
//    rejeicao de 2^32 nele recusaria um editor que so ficou muito tempo aberto.
const kU32Max = 4294967295;
const kU64Max = Number.MAX_SAFE_INTEGER; // 2^53-1: o editor serializa como double
for (const [bloco, campos] of [
  ["materialTable", [
    ["rows", kU32Max], ["residentBytes", kU64Max], ["stagingBytes", kU64Max],
    ["rowsDirty", kU32Max], ["copyRegions", kU32Max], ["rowsUploadedTotal", kU64Max],
  ]],
  ["meshTable", [
    ["rows", kU32Max], ["residentBytes", kU64Max], ["stagingBytes", kU64Max],
  ]],
  ["instanceTable", [
    ["rows", kU32Max], ["residentBytes", kU64Max], ["stagingBytes", kU64Max],
    ["rowsDirty", kU32Max], ["bytesDirty", kU64Max], ["copyRegions", kU32Max],
    ["bytesUploadedTotal", kU64Max],
  ]],
]) {
  {
    const p = snapshotValido();
    delete p.cpu[bloco];
    await esperaErro(p, bloco, `${bloco} ausente`);
  }
  for (const valor of [null, "tabela", 42, []]) {
    const p = snapshotValido();
    p.cpu[bloco] = valor;
    await esperaErro(p, bloco, `${bloco} como ${JSON.stringify(valor)}`);
  }
  for (const [campo, teto] of campos) {
    for (const valor of [undefined, "8", null, true, -1, 1.5]) {
      const p = snapshotValido();
      if (valor === undefined) delete p.cpu[bloco][campo];
      else p.cpu[bloco][campo] = valor;
      await esperaErro(p, campo, `${bloco}.${campo} = ${String(valor)}`);
    }
    // O teto do PROPRIO campo passa; um a mais e recusado. Sem o caso positivo, um contrato que
    // recusasse tudo acima de zero ficaria verde; sem o negativo, um sem teto tambem.
    {
      const p = snapshotValido();
      p.cpu[bloco][campo] = teto;
      await esperaOk(p, `${bloco}.${campo} = ${teto} (teto do contrato) e aceito`);
    }
    {
      const p = snapshotValido();
      p.cpu[bloco][campo] = teto + 1;
      await esperaErro(p, campo, `${bloco}.${campo} = ${teto + 1} (acima do teto)`);
    }
    // E o caso que motivou separar os contratos: 2^32 e invalido numa contagem e VALIDO num
    // acumulado de u64.
    {
      const p = snapshotValido();
      p.cpu[bloco][campo] = 4294967296;
      if (teto === kU32Max) await esperaErro(p, campo, `${bloco}.${campo} = 2^32 numa contagem u32`);
      else await esperaOk(p, `${bloco}.${campo} = 2^32 num acumulado u64 e aceito`);
    }
  }
}

// 7. Contadores POR PASSE (protocolo 7). Os tres da Fase 3 entraram no RESUMO, entao valem a
//    mesma regra do gpuSlots: exigencia nao validada existe so no numero da versao, e um editor
//    anterior a fase produziria agregado de `undefined` num relatorio que parece completo.
for (const campo of ["considered", "draws", "materialBinds", "meshBinds", "skippedInvalid",
                     "bindlessDraws", "materialRowSets", "materialRowMisses"]) {
  for (const valor of [undefined, "3", null, true, -1, 1.5, 4294967296]) {
    const p = snapshotValido();
    if (valor === undefined) delete p.cpu.passes[0][campo];
    else p.cpu.passes[0][campo] = valor;
    await esperaErro(p, campo, `passes[0].${campo} = ${String(valor)}`);
  }
}
{
  const p = snapshotValido();
  delete p.cpu.passes[0].key;
  await esperaErro(p, "passes", "passe sem chave");
}

// 7b. Estado IMPOSSIVEL: todo draw do passe leu material por um caminho ou pelo outro, entao
//     bindless acima de draws descreve um passe que o produtor nao produz. Um contrato so
//     campo a campo aceitaria — cada numero isolado continua plausivel.
{
  const p = snapshotValido();
  p.cpu.passes[0].bindlessDraws = p.cpu.passes[0].draws + 1;
  await esperaErro(p, "bindlessDraws", "bindlessDraws acima de draws");
}
{
  const p = snapshotValido();
  p.cpu.passes[0].bindlessDraws = p.cpu.passes[0].draws;
  await esperaOk(p, "bindlessDraws igual a draws (passe inteiro migrado) e aceito");
}
{
  const p = snapshotValido();
  p.cpu.passes[0].bindlessDraws = 0;
  await esperaOk(p, "bindlessDraws zero (passe inteiro no fallback) e aceito");
}

// 7c. Heap de descritores (protocolo 7). Mesmo contrato u32, e a mesma coerencia de particao
//     que o gpuSlots ja tinha: usados e aposentados nao podem passar da capacidade.
for (const campo of ["used", "highWater", "capacity", "retired", "overflows"]) {
  for (const valor of [undefined, "1", null, true, -1, 1.5, 4294967296]) {
    const p = snapshotValido();
    if (valor === undefined) delete p.cpu.srvHeap[campo];
    else p.cpu.srvHeap[campo] = valor;
    await esperaErro(p, campo, `srvHeap.${campo} = ${String(valor)}`);
  }
}
{
  const p = snapshotValido();
  p.cpu.srvHeap = { used: 60, highWater: 60, capacity: 100, retired: 41, overflows: 0 };
  await esperaErro(p, "srvHeap", "used + retired acima da capacidade");
}
{
  const p = snapshotValido();
  p.cpu.srvHeap = { used: 60, highWater: 60, capacity: 100, retired: 40, overflows: 3 };
  await esperaOk(p, "used + retired exatamente na capacidade e aceito");
}

rmSync(outDir, { recursive: true, force: true });

if (failures > 0) {
  console.error(`[BRIDGE] ${failures} falha(s)`);
  process.exit(1);
}
console.log("[BRIDGE] ok - cpu.gpuSlots, as tres tabelas canonicas e os contadores por passe " +
            "exigidos, validados no contrato u32 e coerentes");
