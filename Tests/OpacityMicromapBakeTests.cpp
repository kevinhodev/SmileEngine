#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Scene/SceneLoader.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

// As quatro ENTRADAS do bake tem de invalidar o micro-map. Antes da Fase 2 o guarda comparava
// duas (alpha e cutoff): trocar a textura de albedo de um material alpha-tested passava por baixo
// dele e deixava o OMM classificando microtriangulos pelo alpha da textura ANTIGA — folhagem com
// recorte errado, sem excecao e sem erro.
//
// Roda antes da checagem de argumentos porque nao depende de cena nenhuma: e CPU pura sobre a
// identidade das entradas.
static int TestarChaveDeEntradas() {
    using namespace Smile;
    int Falhas = 0;
    auto Check = [&](bool Cond, const char* Msg) {
        if (!Cond) { ++Falhas; std::fprintf(stderr, "  FAIL: %s\n", Msg); }
    };

    FGpuMesh MalhaA, MalhaB;
    FTexture TexturaA, TexturaB;
    FMaterial Material;
    Material.Constants.AlphaTest            = 1;
    Material.Constants.AlphaCutoff          = 0.5f;
    Material.Constants.BaseColorFactor.W    = 1.0f;
    Material.Albedo                         = &TexturaA;

    FOpacityMicromapData Omm;
    Omm.BakedInputs = FOmmInputKey::From(&MalhaA, &Material);
    Check(Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "entradas intactas: o micro-map continua valendo");

    // 1) textura de alpha — o furo real que existia.
    Material.Albedo = &TexturaB;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "trocar a textura de albedo invalida o micro-map");
    Material.Albedo = &TexturaA;

    // 2) fator de alpha do material.
    Material.Constants.BaseColorFactor.W = 0.5f;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "mudar o alpha do BaseColor invalida o micro-map");
    Material.Constants.BaseColorFactor.W = 1.0f;

    // 3) cutoff.
    Material.Constants.AlphaCutoff = 0.25f;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "mudar o cutoff invalida o micro-map");
    Material.Constants.AlphaCutoff = 0.5f;

    // 4) o proprio modo: desligar o alpha test troca o BLAS exigido pelo opaco.
    Material.Constants.AlphaTest = 0;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "desligar o alpha test invalida o micro-map");
    Material.Constants.AlphaTest = 1;

    // 5) a malha.
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaB, &Material)),
          "outra malha nao pode reusar o micro-map");

    // Um micro-map nunca carimbado nao casa com nada: caminho de criacao novo falha alto.
    FOpacityMicromapData SemCarimbo;
    Check(!SemCarimbo.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "micro-map sem carimbo de entradas e recusado");
    // E chave viva vazia tambem nao casa com um bake real.
    Check(!Omm.MatchesInputs(FOmmInputKey::From(nullptr, &Material)),
          "chave viva incompleta nao valida um bake");

    if (Falhas == 0) std::printf("[OMM TEST] chave de entradas do bake: 8 casos OK\n");
    return Falhas;
}

int main(int argc, char** argv) {
    using namespace Smile;
    if (TestarChaveDeEntradas() != 0) return 5;
    if (argc != 2) {
        std::fprintf(stderr, "uso: SmileOpacityMicromapBakeTests <cena.sscene>\n");
        return 2;
    }

    const auto Imported = LoadCookedSceneData(std::filesystem::path(argv[1]).wstring());
    if (!Imported) {
        std::fprintf(stderr, "falha ao preparar cena: %s\n", argv[1]);
        return 3;
    }

    std::unordered_map<std::string, const FTextureCPUData*> Textures;
    for (size_t I = 0; I < Imported->TexturePaths.size(); ++I)
        Textures[Imported->TexturePaths[I]] = &Imported->TextureData[I];

    std::unordered_set<u64> Seen;
    u32 BakeCount = 0;
    for (u32 I = 0; I < Imported->Renderables.size(); ++I) {
        const SSceneRenderable& R = Imported->Renderables[I];
        if (R.MaterialIndex == kNoMaterial || R.MaterialIndex >= Imported->Materials.size() ||
            R.MeshIndex >= Imported->Meshes.size())
            continue;
        const SSceneMaterial& M = Imported->Materials[R.MaterialIndex];
        if (!M.AlphaTest) continue;

        const u64 Key = (static_cast<u64>(R.MeshIndex) << 32) | R.MaterialIndex;
        if (!Seen.insert(Key).second) continue;
        const FMesh& Mesh = Imported->Meshes[R.MeshIndex];
        for (u32 Index : Mesh.Indices) {
            if (Index >= Mesh.Vertices.size()) {
                std::fprintf(stderr, "mesh %u possui indice %u >= %zu\n",
                             R.MeshIndex, Index, Mesh.Vertices.size());
                return 4;
            }
        }

        const FTextureCPUData* Alpha = nullptr;
        if (M.BaseColor[0]) {
            const auto It = Textures.find(M.BaseColor);
            if (It != Textures.end()) Alpha = It->second;
        }
        std::printf("[OMM TEST] bake=%u renderable=%u mesh=%u material=%u verts=%zu tris=%zu "
                    "tex=%ux%u fmt=%u path=%s\n",
                    BakeCount, I, R.MeshIndex, R.MaterialIndex, Mesh.Vertices.size(),
                    Mesh.Indices.size() / 3, Alpha ? Alpha->Width : 1,
                    Alpha ? Alpha->Height : 1, Alpha ? static_cast<unsigned>(Alpha->Format) : 0,
                    M.BaseColor);
        std::fflush(stdout);

        const auto Omm = FOpacityMicromapBaker::Bake(
            Mesh, Alpha, M.BaseColorFactor[3], M.AlphaCutoff);
        if (!Omm || !Omm->Valid()) {
            std::fprintf(stderr, "bake %u devolveu resultado invalido\n", BakeCount);
            return 5;
        }

        // O MESMO bake, a partir da entrada COMPACTA que o rebake em runtime monta: UVs extraidas
        // do Vertex e os indices, sem posicao nem normal.
        //
        // Isto e o que autoriza o rebake ler do .smesh apenas UV + indice. Se o baker passasse a
        // ler qualquer outra coisa da malha — posicao, normal, o payload de RT — os dois
        // resultados divergiriam aqui, e nao meses depois como recorte errado de folhagem numa
        // cena que ninguem esta olhando.
        //
        // Nos primeiros kEquivalencePairs pares, e nao em todos: a propriedade sob teste e do
        // ENCANAMENTO da entrada, nao do conteudo da malha — um par ja a exerceria, e a amostra
        // cobre layouts de UV diferentes. Bakear os 122 duas vezes dobrava o custo do teste e
        // estourava o timeout em Debug, que e um jeito ruim de um gate morrer.
        constexpr u32 kEquivalencePairs = 8;
        if (BakeCount >= kEquivalencePairs) { ++BakeCount; continue; }

        std::vector<f32> UVs(Mesh.Vertices.size() * 2);
        for (size_t V = 0; V < Mesh.Vertices.size(); ++V) {
            UVs[V * 2 + 0] = Mesh.Vertices[V].TexCoord[0];
            UVs[V * 2 + 1] = Mesh.Vertices[V].TexCoord[1];
        }
        FOmmGeometry Compacta;
        Compacta.TexCoords      = UVs.data();
        Compacta.TexCoordStride = sizeof(f32) * 2;
        Compacta.VertexCount    = static_cast<u32>(Mesh.Vertices.size());
        Compacta.Indices        = Mesh.Indices.data();
        Compacta.IndexCount     = static_cast<u32>(Mesh.Indices.size());

        const auto Compacto = FOpacityMicromapBaker::Bake(
            Compacta, Alpha, M.BaseColorFactor[3], M.AlphaCutoff);
        if (!Compacto || !Compacto->Valid()) {
            std::fprintf(stderr, "bake compacto %u devolveu resultado invalido\n", BakeCount);
            return 5;
        }
        const bool Igual =
            Compacto->TriangleCount == Omm->TriangleCount &&
            Compacto->IndexFormat   == Omm->IndexFormat &&
            Compacto->IndexStride   == Omm->IndexStride &&
            Compacto->Indices       == Omm->Indices &&
            Compacto->ArrayData     == Omm->ArrayData &&
            Compacto->Histogram.size() == Omm->Histogram.size() &&
            Compacto->Descs.size()  == Omm->Descs.size() &&
            std::memcmp(Compacto->Descs.data(), Omm->Descs.data(),
                        Omm->Descs.size() * sizeof(Omm->Descs[0])) == 0;
        if (!Igual) {
            std::fprintf(stderr,
                         "bake %u: entrada compacta divergiu da malha completa "
                         "(mesh=%u material=%u)\n", BakeCount, R.MeshIndex, R.MaterialIndex);
            return 7;
        }
        ++BakeCount;
    }

    // O LEITOR SOB DEMANDA do .smesh, contra o que o loader produziu.
    //
    // E o codigo mais arriscado do rebake: ele reabre o arquivo cozido e navega o layout
    // [header][entries][blob] a mao, com offsets relativos ao inicio do blob. Um erro de um
    // sizeof aqui nao daria crash — daria UVs de outra malha, e um micro-map plausivel e errado.
    //
    // O oraculo e o proprio FMesh que o SceneLoader montou: se os dois divergirem em um float, o
    // rebake estaria bakeando geometria que nao e a da malha.
    {
        FOmmBakeInputStore Store;
        const std::wstring MeshFile =
            (std::filesystem::path(Imported->BasePath) += L".smesh").wstring();
        // A malha e so a CHAVE do store; nada nela e lido, entao um objeto vazio serve e o teste
        // segue sem device.
        std::vector<FGpuMesh> Chaves(Imported->Meshes.size());
        u32 Conferidas = 0, Falhas = 0;
        for (u32 I = 0; I < Imported->Meshes.size(); ++I) {
            const FMesh& Esperado = Imported->Meshes[I];
            if (Esperado.Vertices.empty() || Esperado.Indices.empty()) continue;
            Store.RetainMeshSource(&Chaves[I], MeshFile, I, FOmmFileStamp::Of(MeshFile));

            FOmmBakeInputStore::FLoadedGeometry Lido;
            if (!Store.LoadGeometry(&Chaves[I], Lido)) {
                std::fprintf(stderr, "LoadGeometry falhou para a malha %u\n", I);
                ++Falhas;
                continue;
            }
            if (Lido.Indices != Esperado.Indices) {
                std::fprintf(stderr, "malha %u: indices lidos divergem do loader\n", I);
                ++Falhas;
                continue;
            }
            if (Lido.TexCoords.size() != Esperado.Vertices.size() * 2) {
                std::fprintf(stderr, "malha %u: contagem de UV divergente\n", I);
                ++Falhas;
                continue;
            }
            bool UVOk = true;
            for (size_t V = 0; V < Esperado.Vertices.size() && UVOk; ++V) {
                UVOk = Lido.TexCoords[V * 2 + 0] == Esperado.Vertices[V].TexCoord[0] &&
                       Lido.TexCoords[V * 2 + 1] == Esperado.Vertices[V].TexCoord[1];
            }
            if (!UVOk) {
                std::fprintf(stderr, "malha %u: UV lida diverge do loader\n", I);
                ++Falhas;
                continue;
            }
            ++Conferidas;
        }
        // Uma malha desconhecida nao pode devolver geometria: o rebake tem de RECUSAR, e nao
        // seguir com o micro-map antigo.
        {
            FGpuMesh Desconhecida;
            FOmmBakeInputStore::FLoadedGeometry Lixo;
            if (Store.LoadGeometry(&Desconhecida, Lixo)) {
                std::fprintf(stderr, "malha sem procedencia devolveu geometria\n");
                ++Falhas;
            }
        }
        // CARIMBO ERRADO tem de recusar, mesmo com caminho, magic, versao e indice corretos.
        //
        // E o caso que o caminho sozinho nao cobria: um recook preserva magic e versao e pode
        // trocar UVs, indices ou a ordem das malhas. Sem esta recusa o rebake bakearia geometria
        // que nao esta nos buffers de GPU, em silencio.
        {
            FOmmBakeInputStore Envelhecido;
            FGpuMesh Chave;
            FOmmFileStamp Falso = FOmmFileStamp::Of(MeshFile);
            Falso.WriteTime += 1; // como se o arquivo tivesse sido reescrito depois do load
            Envelhecido.RetainMeshSource(&Chave, MeshFile, 0, Falso);
            FOmmBakeInputStore::FLoadedGeometry Lixo;
            if (Envelhecido.LoadGeometry(&Chave, Lixo)) {
                std::fprintf(stderr, "carimbo divergente foi aceito pelo leitor\n");
                ++Falhas;
            }
            // E com o carimbo certo o MESMO store aceita — senao isto passaria recusando tudo.
            FOmmBakeInputStore Correto;
            Correto.RetainMeshSource(&Chave, MeshFile, 0, FOmmFileStamp::Of(MeshFile));
            if (!Correto.LoadGeometry(&Chave, Lixo)) {
                std::fprintf(stderr, "carimbo correto foi recusado pelo leitor\n");
                ++Falhas;
            }
        }
        if (Falhas > 0) {
            std::fprintf(stderr, "[OMM TEST] %u falha(s) no leitor sob demanda\n", Falhas);
            return 8;
        }
        std::printf("[OMM TEST] leitor sob demanda: %u malhas identicas ao loader\n", Conferidas);
    }

    std::printf("[OMM TEST] %u pares mesh/material masked validados; entrada compacta identica a "
                "malha completa nos primeiros 8\n", BakeCount);
    return BakeCount ? 0 : 6;
}
