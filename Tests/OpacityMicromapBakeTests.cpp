#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Scene/SceneLoader.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>
#include <system_error>
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

    // O DESTINO DA REVERSAO. Quando o rebake nao pode acontecer, a pergunta e se o micro-map que
    // ainda esta publicado serve de destino — material volta a MASCARADO — ou se o alpha test tem
    // de ser desligado, e nesse caso o BLAS OPACO daquelas malhas passa a ser exigido.
    //
    // A resposta e consultada duas vezes, em momentos diferentes: antes da reconstrucao dos BLAS
    // (para montar o conjunto exigido) e depois dela (para aplicar). Errar para "mantem" quando o
    // certo era "opaco" nao pede o BLAS opaco, e o CollectInstances derruba a render thread no
    // rebuild seguinte — com o dirty flag ja consumido, sem segunda chance.
    Check(Omm.CanRestoreAlphaTestFor(&TexturaA),
          "micro-map da textura atual serve de destino para a reversao");
    Check(!Omm.CanRestoreAlphaTestFor(&TexturaB),
          "micro-map de OUTRA textura nao serve: a reversao tem de ir para opaco");
    Check(!Omm.CanRestoreAlphaTestFor(nullptr),
          "material sem albedo nao tem micro-map que o sirva");
    // Um micro-map bakeado de um material OPACO (alpha test ligado depois) nao restaura nada.
    FOpacityMicromapData OmmOpaco;
    OmmOpaco.BakedInputs              = FOmmInputKey::From(&MalhaA, &Material);
    OmmOpaco.BakedInputs.AlphaTest    = 0;
    Check(!OmmOpaco.CanRestoreAlphaTestFor(&TexturaA),
          "micro-map sem alpha test bakeado nao restaura alpha test");
    // E o nunca carimbado, pelo mesmo motivo: a chave vazia tem AlphaTest 0.
    Check(!SemCarimbo.CanRestoreAlphaTestFor(&TexturaA),
          "micro-map sem carimbo nao serve de destino para a reversao");

    if (Falhas == 0) std::printf("[OMM TEST] chave de entradas do bake: 13 casos OK\n");
    return Falhas;
}

// A DIVISAO DE WORKLOAD. Antes dela, um unico triangulo com UV enorme derrubava a malha INTEIRA
// para FULLY_UNKNOWN_OPAQUE — no BistroExterior, 10 malhas e um sexto da folhagem sem micro-map.
// Agora so os triangulos caros saem do bake; o resto tem de sair IGUAL ao bake deles sozinhos.
//
// Sintetico e sem cena: textura 1024^2 com a metade esquerda opaca, tres triangulos normais e um
// gigante cuja AABB de UV cobre 40x40 repeticoes (1,68e9 texels — acima do teto de 2^30 e abaixo
// do 2^31 em que a conta int do SDK daria wrap).
static int TestarDivisaoDeWorkload() {
    using namespace Smile;
    int Falhas = 0;
    auto Check = [&](bool Cond, const char* Msg) {
        if (!Cond) { ++Falhas; std::fprintf(stderr, "  FAIL: %s\n", Msg); }
    };

    constexpr u32 kTex = 1024;
    FTextureCPUData Tex;
    Tex.Width = Tex.Height = kTex;
    Tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    FMipData Mip;
    Mip.Width = Mip.Height = kTex;
    Mip.Pixels.assign(static_cast<size_t>(kTex) * kTex * 4, 255);
    for (u32 Y = 0; Y < kTex; ++Y)
        for (u32 X = kTex / 2; X < kTex; ++X)
            Mip.Pixels[(static_cast<size_t>(Y) * kTex + X) * 4 + 3] = 0;
    Tex.Mips.push_back(std::move(Mip));

    // Longe da borda u=0/1 de proposito: com Wrap a bilinear de u~0 mistura a coluna 1023, que e
    // transparente, e o triangulo "opaco" deixaria de ser uniforme.
    const std::vector<f32> UVs = {
        0.1f, 0.1f,  0.4f, 0.1f,  0.1f, 0.4f,   // 0-2: A, opaco por inteiro
        0.0f, 0.0f, 40.0f, 0.0f,  0.0f, 40.0f,  // 3-5: G, o gigante
        0.6f, 0.1f,  0.9f, 0.1f,  0.6f, 0.4f,   // 6-8: B, transparente por inteiro
        0.3f, 0.6f,  0.7f, 0.6f,  0.3f, 0.9f,   // 9-11: C, atravessa a borda x=512
    };
    // G no MEIO (triangulo 1), para a costura ter de devolver os bakeados ao lugar certo.
    const std::vector<u32> Todos   = { 0, 1, 2,  3, 4, 5,  6, 7, 8,  9, 10, 11 };
    const std::vector<u32> SemG    = { 0, 1, 2,  6, 7, 8,  9, 10, 11 };
    const std::vector<u32> SoG     = { 3, 4, 5 };
    auto Geo = [&](const std::vector<u32>& I) {
        FOmmGeometry G;
        G.TexCoords = UVs.data(); G.TexCoordStride = sizeof(f32) * 2;
        G.VertexCount = static_cast<u32>(UVs.size() / 2);
        G.Indices = I.data(); G.IndexCount = static_cast<u32>(I.size());
        return G;
    };
    constexpr i32 kTransparent = D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT;
    constexpr i32 kOpaque      = D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE;
    constexpr i32 kUnknownOpq  = D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE;

    const auto Dividido = FOpacityMicromapBaker::Bake(Geo(Todos), &Tex, 1.0f, 0.5f);
    const auto Referencia = FOpacityMicromapBaker::Bake(Geo(SemG), &Tex, 1.0f, 0.5f);
    Check(Dividido && Dividido->Valid(), "bake dividido e valido");
    Check(Referencia && Referencia->Valid(), "bake de referencia (sem o gigante) e valido");
    if (!Dividido || !Referencia || !Dividido->Valid() || !Referencia->Valid()) return Falhas;

    Check(Dividido->TriangleCount == 4, "uma entrada por triangulo da malha inteira");
    Check(Dividido->WorkloadExcludedTriangles == 1, "so o gigante saiu do bake");
    Check(Dividido->IndexOf(1) == kUnknownOpq, "o gigante saiu com FULLY_UNKNOWN_OPAQUE");
    Check(Referencia->WorkloadExcludedTriangles == 0, "bake que cabe nao exclui nada");

    // O que distingue a divisao do fallback antigo: os outros tres AINDA tem micro-map.
    Check(Dividido->IndexOf(0) == kOpaque, "A (opaco) virou especial opaco");
    Check(Dividido->IndexOf(2) == kTransparent, "B (transparente) virou especial transparente");
    Check(Dividido->IndexOf(3) >= 0, "C (borda) ganhou micro-map de verdade");
    Check(!Dividido->Descs.empty(), "o array nao ficou vazio");

    // E IGUAIS ao bake deles sozinhos: mesma entrada para o baker, mesmo resultado. Comparando o
    // indice decodificado, porque o dividido e 32 bits fixos e a referencia escolhe a largura.
    const u32 Origem[3] = { 0, 2, 3 };
    for (u32 K = 0; K < 3; ++K)
        Check(Dividido->IndexOf(Origem[K]) == Referencia->IndexOf(K),
              "indice do bakeado identico ao bake sem o gigante");
    Check(Dividido->ArrayData == Referencia->ArrayData, "ArrayData identico ao da referencia");
    Check(Dividido->Descs.size() == Referencia->Descs.size() &&
          std::memcmp(Dividido->Descs.data(), Referencia->Descs.data(),
                      Referencia->Descs.size() * sizeof(Referencia->Descs[0])) == 0,
          "descritores identicos aos da referencia");

    // COBERTURA. As contagens saem do buffer final; a area conhecida, do SDK sobre o que foi
    // bakeado, dobrada com a area de UV dos excluidos.
    {
        const FOmmCoverage& R = Referencia->Coverage;
        Check(R.FullyKnownTriangles == 2 && R.MicromapTriangles == 1 && R.FullyUnknownTriangles == 0,
              "referencia: A e B resolvidos inteiros, C com micro-map, nada UNKNOWN");
        Check(R.KnownArea > 0.0f && R.KnownArea < 1.0f,
              "referencia: area conhecida entre 0 e 1 (C tem microtriangulos na borda)");
        const FOmmCoverage& D = Dividido->Coverage;
        Check(D.FullyKnownTriangles == 2 && D.MicromapTriangles == 1 && D.FullyUnknownTriangles == 1,
              "dividido: o gigante conta como UNKNOWN inteiro, o resto igual a referencia");
        // Areas de UV exatas: A e B = 0,045 cada, C = 0,06, G = 800. O SDK mediu so A+B+C; o
        // gigante entra no denominador como area toda desconhecida.
        const double Esperado = static_cast<double>(R.KnownArea) * 0.15 / (0.15 + 800.0);
        Check(std::abs(D.KnownArea - Esperado) <= Esperado * 1e-3,
              "dividido: area conhecida dobrada com a area de UV dos excluidos");
    }

    // CASO NEGATIVO: nada cabe. Tem de cair no all-unknown, e nao devolver uma malha vazia nem
    // lancar — o BLAS OMM continua obrigatorio para ela.
    const auto Nada = FOpacityMicromapBaker::Bake(Geo(SoG), &Tex, 1.0f, 0.5f);
    Check(Nada && Nada->Valid(), "malha so com o gigante ainda produz OMM valido");
    if (Nada && Nada->Valid()) {
        Check(Nada->WorkloadExcludedTriangles == Nada->TriangleCount,
              "malha sem nada que caiba conta tudo como excluido");
        Check(Nada->IndexOf(0) == kUnknownOpq, "e sai inteira com FULLY_UNKNOWN_OPAQUE");
        Check(Nada->Descs.empty() && Nada->ArrayData.empty(), "sem array: so indices especiais");
        Check(Nada->Coverage.FullyUnknownTriangles == 1 && Nada->Coverage.KnownArea == 0.0f,
              "all-unknown: cobertura toda UNKNOWN e area conhecida zero");
    }

    if (Falhas == 0) std::printf("[OMM TEST] divisao de workload + cobertura: 24 casos OK\n");
    return Falhas;
}

int main(int argc, char** argv) {
    using namespace Smile;
    if (TestarChaveDeEntradas() != 0) return 5;
    if (TestarDivisaoDeWorkload() != 0) return 9;
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
    // Cobertura do teto de workload na cena real: quantos pares foram divididos, quantos nem
    // assim couberam, e quantos triangulos mascarados ficaram sem micro-map.
    u32 ParesDivididos = 0, ParesSemNada = 0;
    u64 TriangulosMascarados = 0, TriangulosExcluidos = 0;
    u64 CobResolvidos = 0, CobMicromap = 0, CobUnknown = 0;
    double CobAreaPorTri = 0.0;
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
        // Todo excluido tem de estar no buffer como FULLY_UNKNOWN_OPAQUE. Conta >= e nao ==: o
        // baker tambem usa esse especial para triangulo de UV nao-finita, que nao e exclusao.
        if (Omm->WorkloadExcludedTriangles > 0) {
            u32 Especiais = 0;
            for (u32 T = 0; T < Omm->TriangleCount; ++T)
                if (Omm->IndexOf(T) ==
                    D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE)
                    ++Especiais;
            if (Especiais < Omm->WorkloadExcludedTriangles) {
                std::fprintf(stderr, "bake %u: %u excluidos mas so %u indices UNKNOWN\n",
                             BakeCount, Omm->WorkloadExcludedTriangles, Especiais);
                return 10;
            }
            if (Omm->WorkloadExcludedTriangles == Omm->TriangleCount) ++ParesSemNada;
            else ++ParesDivididos;
        }
        TriangulosMascarados += Omm->TriangleCount;
        TriangulosExcluidos  += Omm->WorkloadExcludedTriangles;

        // A cobertura tem de fechar com o buffer: as tres contagens somam a malha, a area e uma
        // fracao, e todo excluido e um UNKNOWN inteiro.
        const FOmmCoverage& Cob = Omm->Coverage;
        if (static_cast<u64>(Cob.FullyKnownTriangles) + Cob.MicromapTriangles +
                Cob.FullyUnknownTriangles != Omm->TriangleCount ||
            !(Cob.KnownArea >= 0.0f && Cob.KnownArea <= 1.0f) ||
            Cob.FullyUnknownTriangles < Omm->WorkloadExcludedTriangles) {
            std::fprintf(stderr, "bake %u: cobertura inconsistente (%u+%u+%u de %u, area %f)\n",
                         BakeCount, Cob.FullyKnownTriangles, Cob.MicromapTriangles,
                         Cob.FullyUnknownTriangles, Omm->TriangleCount, Cob.KnownArea);
            return 11;
        }
        CobResolvidos += Cob.FullyKnownTriangles;
        CobMicromap   += Cob.MicromapTriangles;
        CobUnknown    += Cob.FullyUnknownTriangles;
        CobAreaPorTri += static_cast<double>(Cob.KnownArea) * Omm->TriangleCount;

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
        // O fingerprint sai do FMesh do loader, como no import de verdade: UVs com o stride do
        // Vertex (32 B) de um lado, compactadas (8 B) do outro. Se o hash nao fosse por elemento,
        // este teste ficaria vermelho em todas as malhas.
        auto HashDoLoader = [](const FMesh& M) {
            return OmmHashGeometry(&M.Vertices.front().TexCoord[0], sizeof(Vertex),
                                   static_cast<u32>(M.Vertices.size()), M.Indices.data(),
                                   static_cast<u32>(M.Indices.size()));
        };
        u32 Conferidas = 0, Falhas = 0;
        for (u32 I = 0; I < Imported->Meshes.size(); ++I) {
            const FMesh& Esperado = Imported->Meshes[I];
            if (Esperado.Vertices.empty() || Esperado.Indices.empty()) continue;
            Store.RetainMeshSource(&Chaves[I], MeshFile, I, HashDoLoader(Esperado));

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
        // FINGERPRINT ERRADO tem de recusar, mesmo com caminho, magic, versao e indice corretos.
        //
        // E o caso que o caminho sozinho nao cobria: um recook preserva magic e versao e pode
        // trocar UVs, indices ou a ordem das malhas. Sem esta recusa o rebake bakearia geometria
        // que nao esta nos buffers de GPU, em silencio.
        //
        // Um carimbo {tamanho, hora} tambem nao servia, e por um motivo que este teste nao alcanca:
        // ele seria tirado DEPOIS de o loader ler os bytes, entao um recook nessa janela registra o
        // arquivo novo contra os buffers antigos. O hash sai dos bytes carregados; nao ha janela.
        u32 IndiceValido = u32(-1);
        for (u32 I = 0; I < Imported->Meshes.size(); ++I)
            if (!Imported->Meshes[I].Vertices.empty() && !Imported->Meshes[I].Indices.empty()) {
                IndiceValido = I;
                break;
            }
        if (IndiceValido != u32(-1)) {
            FOmmBakeInputStore Envelhecido;
            FGpuMesh Chave;
            const FOmmContentHash Certo = HashDoLoader(Imported->Meshes[IndiceValido]);
            // Como se o .smesh tivesse sido recozinhado depois do load: mesmo arquivo, outro
            // conteudo do ponto de vista de quem carregou.
            Envelhecido.RetainMeshSource(&Chave, MeshFile, IndiceValido, Certo ^ 1ull);
            FOmmBakeInputStore::FLoadedGeometry Lixo;
            if (Envelhecido.LoadGeometry(&Chave, Lixo)) {
                std::fprintf(stderr, "fingerprint divergente foi aceito pelo leitor\n");
                ++Falhas;
            }
            // E com o fingerprint certo o MESMO caminho aceita — senao isto passaria recusando
            // tudo, que e o modo de falha silencioso deste tipo de gate.
            FOmmBakeInputStore Correto;
            Correto.RetainMeshSource(&Chave, MeshFile, IndiceValido, Certo);
            if (!Correto.LoadGeometry(&Chave, Lixo)) {
                std::fprintf(stderr, "fingerprint correto foi recusado pelo leitor\n");
                ++Falhas;
            }
            // FAIL-CLOSED NO REGISTRO: sem fingerprint nao ha procedencia, e aceitar o registro
            // criaria uma entrada que casa com qualquer arquivo. Duas consultas "sem hash" nao
            // podem passar a bater uma com a outra.
            FOmmBakeInputStore SemHash;
            SemHash.RetainMeshSource(&Chave, MeshFile, IndiceValido, kOmmNoContentHash);
            if (SemHash.MeshCount() != 0 || SemHash.LoadGeometry(&Chave, Lixo)) {
                std::fprintf(stderr, "registro sem fingerprint foi aceito\n");
                ++Falhas;
            }
        }

        // ARQUIVO MALFORMADO: as contagens vem DO ARQUIVO e dimensionam os vetores do leitor.
        // Validar os ranges depois de alocar significaria um .smesh corrompido pedindo dezenas de
        // GB antes de qualquer checagem.
        //
        // O primeiro caso e a prova negativa de verdade: 4,29e9 vertices x 32 B sao ~137 GB, e sem
        // a checagem previa o vector e construido antes de o leitor olhar o tamanho do arquivo — o
        // teste morre em bad_alloc em vez de ficar vermelho. Os outros dois sao ranges impossiveis
        // com contagens sensatas, o caso comum de um offset apontando para fora do blob.
        {
            namespace fs = std::filesystem;
            const fs::path Falso = fs::temp_directory_path() / L"smile_omm_malformado.smesh";
            SMeshHeader Cabecalho{};
            Cabecalho.Magic     = kSMeshMagic;
            Cabecalho.Version   = kCookedVersion;
            Cabecalho.MeshCount = 1;
            auto Escrever = [&](const SMeshEntry& E, size_t BlobBytes) {
                std::ofstream Out(Falso, std::ios::binary | std::ios::trunc);
                Out.write(reinterpret_cast<const char*>(&Cabecalho), sizeof(Cabecalho));
                Out.write(reinterpret_cast<const char*>(&E), sizeof(E));
                const std::vector<char> Blob(BlobBytes, 0);
                if (BlobBytes) Out.write(Blob.data(), static_cast<std::streamsize>(BlobBytes));
            };
            auto Recusa = [&](const char* Caso) {
                FOmmBakeInputStore Loja;
                FGpuMesh Chave;
                Loja.RetainMeshSource(&Chave, Falso.wstring(), 0, 0x1234ull);
                FOmmBakeInputStore::FLoadedGeometry Lixo;
                if (Loja.LoadGeometry(&Chave, Lixo)) {
                    std::fprintf(stderr, "arquivo malformado aceito: %s\n", Caso);
                    ++Falhas;
                }
            };
            SMeshEntry Entrada{};
            Entrada.VertexCount = 0xFFFFFFFFu;
            Entrada.IndexCount  = 3;
            Escrever(Entrada, 256);
            Recusa("VertexCount absurdo");

            Entrada.VertexCount  = 4;
            Entrada.IndexCount   = 3;
            Entrada.VertexOffset = 1u << 20;
            Escrever(Entrada, 256);
            Recusa("VertexOffset fora do blob");

            Entrada.VertexOffset = 0;
            Entrada.IndexCount   = 300;
            Entrada.IndexOffset  = 128;
            Escrever(Entrada, 256);
            Recusa("IndexCount maior que o blob");

            std::error_code Ec;
            fs::remove(Falso, Ec);
        }
        // A PROCEDENCIA DA TEXTURA, com o mesmo criterio: releitura do disco batendo com o que o
        // loader decodificou, conteudo divergente sendo recusado.
        //
        // O que este bloco prova de verdade e que as duas pontas decodificam IGUAL — o loader por
        // LoadCPUAuto com as flags do cozido, o store por LoadCPUAuto com as flags que ele
        // derivou do proprio dado. Se essa derivacao errasse, o hash divergiria e TODO rebake de
        // material com textura trocada seria recusado em silencio.
        u32 TexturasConferidas = 0;
        for (u32 I = 0; I < Imported->TexturePaths.size() && TexturasConferidas < 4; ++I) {
            if (I >= Imported->TextureData.size() || !Imported->TextureData[I].Valid()) continue;
            const std::wstring Caminho =
                (Imported->SceneDir / std::filesystem::path(Imported->TexturePaths[I])).wstring();

            FOmmBakeInputStore Loja;
            FTexture ChaveTex;
            Loja.RetainTexturePath(&ChaveTex, Caminho, Imported->TextureData[I]);
            if (Loja.TextureCount() != 1) {
                std::fprintf(stderr, "textura %u: registro de procedencia recusado\n", I);
                ++Falhas;
                continue;
            }
            FTextureCPUData Relida;
            if (!Loja.LoadAlphaTexture(&ChaveTex, Relida)) {
                std::fprintf(stderr, "textura %u: releitura recusada pelo fingerprint\n", I);
                ++Falhas;
                continue;
            }
            if (!Relida.Valid() ||
                Relida.Mips.front().Pixels != Imported->TextureData[I].Mips.front().Pixels) {
                std::fprintf(stderr, "textura %u: pixels relidos divergem do loader\n", I);
                ++Falhas;
                continue;
            }
            ++TexturasConferidas;
        }
        {
            // Textura sem procedencia (gerada em runtime) nao pode devolver pixels.
            FTexture Desconhecida;
            FTextureCPUData Lixo;
            if (Store.LoadAlphaTexture(&Desconhecida, Lixo)) {
                std::fprintf(stderr, "textura sem procedencia devolveu pixels\n");
                ++Falhas;
            }
        }
        // CONTEUDO TROCADO tem de recusar. Nao da para injetar um hash errado — o store o calcula
        // do dado, e essa e a metade da protecao —, entao o teste registra o caminho de UMA
        // textura com o dado de OUTRA: exatamente o que um recook produziria, um arquivo que nao e
        // mais o que virou os bytes na GPU.
        {
            u32 A = u32(-1), B = u32(-1);
            for (u32 I = 0; I < Imported->TexturePaths.size(); ++I) {
                if (I >= Imported->TextureData.size() || !Imported->TextureData[I].Valid()) continue;
                if (A == u32(-1)) { A = I; continue; }
                if (Imported->TextureData[I].Mips.front().Pixels !=
                    Imported->TextureData[A].Mips.front().Pixels) { B = I; break; }
            }
            if (A != u32(-1) && B != u32(-1)) {
                FOmmBakeInputStore Trocada;
                FTexture ChaveTex;
                Trocada.RetainTexturePath(
                    &ChaveTex,
                    (Imported->SceneDir / std::filesystem::path(Imported->TexturePaths[B])).wstring(),
                    Imported->TextureData[A]);
                FTextureCPUData Lixo;
                if (Trocada.LoadAlphaTexture(&ChaveTex, Lixo)) {
                    std::fprintf(stderr, "textura com conteudo divergente foi aceita\n");
                    ++Falhas;
                }
            }
        }
        if (Falhas > 0) {
            std::fprintf(stderr, "[OMM TEST] %u falha(s) no leitor sob demanda\n", Falhas);
            return 8;
        }
        std::printf("[OMM TEST] leitor sob demanda: %u malhas e %u texturas identicas ao loader\n",
                    Conferidas, TexturasConferidas);
    }

    std::printf("[OMM TEST] %u pares mesh/material masked validados; entrada compacta identica a "
                "malha completa nos primeiros 8\n", BakeCount);
    std::printf("[OMM TEST] teto de workload: %u pares divididos, %u sem nada que coubesse; "
                "%llu de %llu triangulos mascarados sem micro-map\n",
                ParesDivididos, ParesSemNada,
                static_cast<unsigned long long>(TriangulosExcluidos),
                static_cast<unsigned long long>(TriangulosMascarados));
    if (TriangulosMascarados) {
        const double N = static_cast<double>(TriangulosMascarados);
        std::printf("[OMM TEST] cobertura: %.1f%% resolvidos inteiros, %.1f%% com micro-map, "
                    "%.1f%% UNKNOWN inteiros; area de UV conhecida (media por triangulo) %.1f%%\n",
                    100.0 * CobResolvidos / N, 100.0 * CobMicromap / N, 100.0 * CobUnknown / N,
                    100.0 * CobAreaPorTri / N);
    }
    return BakeCount ? 0 : 6;
}
