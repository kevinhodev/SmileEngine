// Smoke do gerador de malha por IA, contra um provedor FALSO em loopback.
//
// O que este teste protege: submeter -> acompanhar o polling -> baixar -> validar o formato ->
// gravar no lugar certo. Nenhum provedor real e chamado (nao ha chave, nao ha custo, nao ha rede
// externa), e e exatamente essa a parte do caminho que da errado em silencio — um HTML de erro
// devolvido com HTTP 200 chegaria ao Cooker como se fosse asset.

import assert from "node:assert/strict";
import { createServer } from "node:http";
import { readFile, rm, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(scriptDirectory, "../../..");

const { generateMesh, slugify, describeProviders } = await import(
  path.join(packageRoot, "dist", "meshgen.js")
);

// GLB minimo valido: header 'glTF' + um chunk JSON. Basta para a verificacao de formato.
function buildGlb() {
  const json = Buffer.from('{"asset":{"version":"2.0"}}', "utf8");
  const padded = Buffer.concat([json, Buffer.alloc((4 - (json.length % 4)) % 4, 0x20)]);
  const out = Buffer.alloc(12 + 8 + padded.length);
  out.write("glTF", 0, "ascii");
  out.writeUInt32LE(2, 4);
  out.writeUInt32LE(out.length, 8);
  out.writeUInt32LE(padded.length, 12);
  out.writeUInt32LE(0x4e4f534a, 16);
  padded.copy(out, 20);
  return out;
}

const model = buildGlb();

// Servidor falso: uma passada de "processando" antes do "pronto", para o polling ser exercitado.
let pollCount = 0;
let servedBody = model;
let servedType = "model/gltf-binary";

const server = createServer((request, response) => {
  const url = new URL(request.url ?? "/", "http://127.0.0.1");
  if (request.method === "POST" && url.pathname === "/generate") {
    let body = "";
    request.on("data", (chunk) => { body += chunk; });
    request.on("end", () => {
      const parsed = JSON.parse(body);
      assert.equal(parsed.format, "glb", "o cliente deve pedir glb");
      assert.ok(parsed.prompt.length > 0, "o prompt deve chegar ao provedor");
      response.writeHead(200, { "content-type": "application/json" });
      response.end(JSON.stringify({ task_id: "t1" }));
    });
    return;
  }
  if (request.method === "GET" && url.pathname === "/task/t1") {
    pollCount += 1;
    const port = server.address().port;
    const payload = pollCount < 2
      ? { status: "processing", progress: 40 }
      : { status: "done", progress: 100, model_url: `http://127.0.0.1:${port}/model.glb` };
    response.writeHead(200, { "content-type": "application/json" });
    response.end(JSON.stringify(payload));
    return;
  }
  if (request.method === "GET" && url.pathname === "/model.glb") {
    response.writeHead(200, { "content-type": servedType });
    response.end(servedBody);
    return;
  }
  response.writeHead(404);
  response.end();
});

await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const baseUrl = `http://127.0.0.1:${server.address().port}`;
process.env.SMILE_MESHGEN_LOCAL_URL = baseUrl;

const assetName = "smoke meshgen";
const slug = slugify(assetName);
const generatedDirectory = path.join(smileRoot, "Assets", "Generated", slug);

try {
  assert.equal(slug, "smoke-meshgen");
  // slugify e a unica barreira entre um nome vindo do agente e um caminho no disco.
  assert.equal(slugify("../../etc/passwd"), "etc-passwd");

  const providers = describeProviders();
  const local = providers.find((provider) => provider.id === "local");
  assert.ok(local, "o provedor local deve existir");
  assert.equal(local.baseUrl, baseUrl, "SMILE_MESHGEN_LOCAL_URL deve sobrescrever o endpoint");
  assert.ok(
    providers.every((provider) => !("apiKey" in provider)),
    "a descricao dos provedores nao pode carregar o valor da chave",
  );

  const result = await generateMesh(smileRoot, {
    provider: "local",
    prompt: "um cubo de pedra",
    name: assetName,
    timeoutSeconds: 60,
    pollSeconds: 2,
  });

  assert.equal(result.modelPath, `Assets/Generated/${slug}/${slug}.glb`);
  assert.equal(result.bytes, model.length);
  assert.ok(result.pollCount >= 2, "o polling deve ter acompanhado o estado 'processing'");
  const written = await readFile(path.join(generatedDirectory, `${slug}.glb`));
  assert.deepEqual(written, model, "o .glb gravado deve ser byte a byte o baixado");
  await stat(path.join(generatedDirectory, `${slug}.glb`));

  // Modo de falha classico do servico: pagina de erro com HTTP 200.
  servedBody = Buffer.from("<html><body>rate limited</body></html>", "utf8");
  servedType = "text/html";
  pollCount = 0;
  await assert.rejects(
    generateMesh(smileRoot, {
      provider: "local",
      prompt: "um cubo de pedra",
      name: assetName,
      timeoutSeconds: 60,
      pollSeconds: 2,
    }),
    /nao e um glTF/,
    "conteudo que nao e glTF precisa ser recusado antes de virar asset",
  );

  // Endpoint http fora de loopback vazaria a chave em claro.
  process.env.SMILE_MESHGEN_LOCAL_URL = "http://exemplo.invalido";
  await assert.rejects(
    generateMesh(smileRoot, {
      provider: "local",
      prompt: "x",
      name: assetName,
      timeoutSeconds: 60,
      pollSeconds: 2,
    }),
    /Endpoint recusado/,
    "http remoto precisa ser recusado",
  );

  // Provedor sem chave configurada nao pode sequer tentar a chamada paga.
  delete process.env.SMILE_MESHGEN_MESHY_KEY;
  await assert.rejects(
    generateMesh(smileRoot, {
      provider: "meshy",
      prompt: "x",
      name: assetName,
      timeoutSeconds: 60,
      pollSeconds: 2,
    }),
    /SMILE_MESHGEN_MESHY_KEY/,
    "provedor sem chave deve falhar com a variavel de ambiente no texto",
  );

  // Tripo nao tem o passo de upload de imagem implementado; dizer o contrario so daria uma falha
  // obscura no meio do polling.
  await assert.rejects(
    generateMesh(smileRoot, {
      provider: "tripo",
      imageDataUri: "data:image/png;base64,AAAA",
      name: assetName,
      timeoutSeconds: 60,
      pollSeconds: 2,
    }),
    /imagem|chave/i,
  );

  console.log("Smoke do meshgen OK: submissao, polling, download, validacao de formato e recusas.");
} finally {
  await rm(generatedDirectory, { recursive: true, force: true });
  await new Promise((resolve) => server.close(resolve));
}
