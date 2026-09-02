// Gerador de cena SINTETICA para a regua de escala da Fase 0 (Docs/GPU-DRIVEN-PLAN.md).
//
// Por que gerar um .smesh/.sscene em vez de povoar a cena em runtime: assim a cena sintetica
// entra pelo MESMO caminho de load que as reais (SceneLoader -> FScene::AddMeshesBatch -> TLAS
// -> telemetria), e tudo que ja existe continua valendo sem caso especial — os testes de carga,
// o painel e o profile_snapshot do MCP nao sabem que a cena e sintetica. Um povoamento por
// runtime mediria um caminho que nenhuma cena real percorre.
//
// O que a cena precisa ter para a medida significar algo:
//
//   - GEOMETRIA COMPARTILHADA. N instancias sobre poucas malhas e o regime que interessa: e o
//     que o Emerald Square ja faz (2479 renderaveis, 281 malhas) e o que o GPU-driven explora.
//     Gerar N malhas unicas mediria custo de VRAM e de BLAS, nao de submissao.
//   - VARIACAO DE MATERIAL CONTROLAVEL. O numero de materiais e o que decide quantos binds
//     sobram depois do cache de submissao, e depois do bindless sera o que decide o numero de
//     lotes. Com material unico o teste seria otimista; com um por instancia, pessimista.
//   - FRACAO MASCARADA CONTROLAVEL. Alpha-test e two-sided partem o z-prepass em dois lacos e
//     tem PSO proprio, entao a distribuicao entre eles muda o numero de trocas de estado.

#include "Smile/Scene/CookedFormat.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/RayTracing/RTTriangle.h"
#include "PublishPair.h"

#include <chrono>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
    using Smile::Vertex;

    // Grade plana TxT quads, dobrada num "telhado" para a malha nao ser degenerada no eixo Y
    // (AABB achatada passaria despercebida no culling e no HiZ, que e justamente o que o teste
    // de escala exercita). 2*T*T triangulos.
    void BuildGridMesh(uint32_t _Tess, std::vector<Vertex>& _Vertices,
                       std::vector<uint32_t>& _Indices,
                       float _AabbMin[3], float _AabbMax[3]) {
        const uint32_t N = _Tess + 1u;
        _Vertices.clear();
        _Vertices.reserve(static_cast<size_t>(N) * N);
        _Indices.clear();
        _Indices.reserve(static_cast<size_t>(_Tess) * _Tess * 6u);

        for (int c = 0; c < 3; ++c) { _AabbMin[c] = 1e30f; _AabbMax[c] = -1e30f; }

        for (uint32_t z = 0; z < N; ++z) {
            for (uint32_t x = 0; x < N; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(_Tess);
                const float v = static_cast<float>(z) / static_cast<float>(_Tess);
                // Altura em telhado: sobe ate o meio e desce. Da volume sem custo de seno.
                const float h = 0.5f * (1.0f - std::fabs(2.0f * u - 1.0f))
                              + 0.5f * (1.0f - std::fabs(2.0f * v - 1.0f));

                Vertex Vtx{};
                Vtx.Position[0] = u - 0.5f;
                Vtx.Position[1] = h * 0.5f;
                Vtx.Position[2] = v - 0.5f;
                // Normal aproximada por diferenca do telhado; nao precisa ser exata, precisa ser
                // finita e variada (normal constante deixaria o G-buffer sem trabalho real).
                const float dx = (u < 0.5f ? 1.0f : -1.0f);
                const float dz = (v < 0.5f ? 1.0f : -1.0f);
                const float nl = std::sqrt(dx * dx + 4.0f + dz * dz);
                Vtx.Normal[0] = -dx * 0.5f / nl;
                Vtx.Normal[1] = 2.0f / nl;
                Vtx.Normal[2] = -dz * 0.5f / nl;
                Vtx.TexCoord[0] = u;
                Vtx.TexCoord[1] = v;

                for (int c = 0; c < 3; ++c) {
                    if (Vtx.Position[c] < _AabbMin[c]) _AabbMin[c] = Vtx.Position[c];
                    if (Vtx.Position[c] > _AabbMax[c]) _AabbMax[c] = Vtx.Position[c];
                }
                _Vertices.push_back(Vtx);
            }
        }

        for (uint32_t z = 0; z < _Tess; ++z) {
            for (uint32_t x = 0; x < _Tess; ++x) {
                const uint32_t I0 = z * N + x;
                const uint32_t I1 = I0 + 1u;
                const uint32_t I2 = I0 + N;
                const uint32_t I3 = I2 + 1u;
                // Winding igual ao que o cooker de FBX produz (LH, CW visto de +Y).
                _Indices.push_back(I0); _Indices.push_back(I2); _Indices.push_back(I1);
                _Indices.push_back(I1); _Indices.push_back(I2); _Indices.push_back(I3);
            }
        }
    }
}

namespace {
    // Remocao de algo que ESTA execucao criou, com o erro REPORTADO. A propagacao de falha de
    // limpeza nao pode valer so dentro do PublishPair: a transacao do gerador comeca nas
    // escritas, e um temporario truncado abandonado em silencio e a sobra que colide depois.
    bool ScrubTemp(const fs::path& _Path) {
        std::error_code Ec;
        fs::remove(_Path, Ec);
        if (Ec) {
            std::printf("[Cooker] ATENCAO: nao consegui remover %s: %s\n",
                        _Path.string().c_str(), Ec.message().c_str());
            return false;
        }
        return true;
    }

    // Escrita verificada. fwrite/fclose sem checagem transformavam disco cheio num par de
    // arquivos truncado com "OK" impresso na tela — e o proximo a ler seria o loader, meses
    // depois, com uma mensagem que nao aponta para aqui.
    bool WriteAllChecked(const fs::path& _Path, const void* _Header, size_t _HeaderBytes,
                         const void* _A, size_t _ABytes, const void* _B, size_t _BBytes) {
        FILE* F = std::fopen(_Path.string().c_str(), "wb");
        if (!F) {
            std::printf("[Cooker] ERRO ao abrir %s\n", _Path.string().c_str());
            return false;
        }
        auto Put = [&](const void* _Data, size_t _Bytes) {
            return _Bytes == 0 || std::fwrite(_Data, 1, _Bytes, F) == _Bytes;
        };
        const bool Ok = Put(_Header, _HeaderBytes) && Put(_A, _ABytes) && Put(_B, _BBytes);
        // fclose PRECISA ser checado: e nele que o buffer do stdio e drenado, entao um disco
        // cheio costuma falhar aqui e nao no fwrite.
        const bool Closed = (std::fclose(F) == 0);
        if (!Ok || !Closed) {
            std::printf("[Cooker] ERRO ao escrever %s (disco cheio?)\n", _Path.string().c_str());
            ScrubTemp(_Path); // nao deixa temporario truncado para tras, e avisa se nao conseguir
            return false;
        }
        return true;
    }
}

// Declarado em main.cpp; mantido fora de header proprio porque e o unico ponto de contato.
int GenerateSyntheticScene(const fs::path& _OutBase, uint32_t _Instances, uint32_t _MeshCount,
                           uint32_t _MaterialCount, uint32_t _MaskedPercent, float _Spacing,
                           uint32_t _BaseTess) {
    // Validacao AQUI e nao so no CLI: o gerador e quem conhece o contrato, e um chamador futuro
    // (script, teste, outro modo) nao deve poder produzir cozido invalido. Com --tess 0 a grade
    // saia sem area, a AABB nascia invertida e o cooker imprimia OK — o loader e que recusava
    // depois, com uma mensagem que nao aponta para o parametro culpado.
    if (_Instances == 0 || _MeshCount == 0 || _MaterialCount == 0) {
        std::printf("[Cooker] ERRO: --instances/--meshes/--materials precisam ser > 0\n");
        return 2;
    }
    if (_BaseTess == 0) {
        std::printf("[Cooker] ERRO: --tess precisa ser >= 1 (0 gera malha sem area)\n");
        return 2;
    }
    if (_MaskedPercent > 100u) {
        std::printf("[Cooker] ERRO: --masked-percent precisa estar em [0,100]\n");
        return 2;
    }
    if (!std::isfinite(_Spacing) || _Spacing <= 0.0f) {
        std::printf("[Cooker] ERRO: --spacing precisa ser finito e > 0\n");
        return 2;
    }

    // fopen nao cria diretorio. Garantir aqui e nao no chamador: o destino natural desta cena e
    // um diretorio de build recem-criado, e exigir que quem chama o prepare seria uma pegadinha
    // que so aparece na primeira execucao limpa.
    if (_OutBase.has_parent_path()) {
        std::error_code Ec;
        fs::create_directories(_OutBase.parent_path(), Ec);
        if (Ec && !fs::exists(_OutBase.parent_path())) {
            std::printf("[Cooker] ERRO ao criar %s: %s\n",
                        _OutBase.parent_path().string().c_str(), Ec.message().c_str());
            return 3;
        }
    }

    // --- Preflight do custo DERIVADO. Tetos independentes por flag nao compoem: a tesselacao
    // cresce com o indice da malha (BaseTess + M*2), entao "--meshes 4096 --tess 512" — cada um
    // dentro do seu limite — leva a ultima malha a tess 8702 e o conjunto a ~220 bilhoes de
    // triangulos. Sem esta conta, o primeiro sintoma seria um reserve gigante e o processo
    // morrendo sem mensagem.
    //
    // Aritmetica em u64 com checagem de estouro em cada passo, e nao multiplicacao "que deve
    // caber": o proposito aqui e justamente pegar o caso que nao cabe.
    {
        constexpr uint64_t kMaxGeoBytes   = 2ull * 1024 * 1024 * 1024; // 2 GiB de .smesh
        constexpr uint64_t kMaxSceneBytes = 2ull * 1024 * 1024 * 1024; // 2 GiB de .sscene
        const uint64_t LastTess = static_cast<uint64_t>(_BaseTess) + (_MeshCount - 1ull) * 2ull;

        uint64_t GeoBytes = 0;
        bool Overflow = false;
        auto AddChecked = [&](uint64_t _A, uint64_t _B) {
            if (_B != 0 && _A > (UINT64_MAX / _B)) { Overflow = true; return uint64_t{ 0 }; }
            return _A * _B;
        };
        for (uint32_t M = 0; M < _MeshCount && !Overflow; ++M) {
            const uint64_t T  = static_cast<uint64_t>(_BaseTess) + M * 2ull;
            const uint64_t NV = AddChecked(T + 1ull, T + 1ull);      // vertices
            const uint64_t NT = AddChecked(AddChecked(T, T), 2ull);  // triangulos
            const uint64_t Bytes = AddChecked(NV, sizeof(Vertex))
                                 + AddChecked(NT, 3ull * sizeof(uint32_t))
                                 + AddChecked(NT, sizeof(Smile::FRTTriangle));
            if (Overflow || GeoBytes > UINT64_MAX - Bytes) { Overflow = true; break; }
            GeoBytes += Bytes;
        }
        const uint64_t SceneBytes =
            AddChecked(_Instances, sizeof(Smile::SSceneRenderable)) +
            AddChecked(_MaterialCount, sizeof(Smile::SSceneMaterial));

        if (Overflow || GeoBytes > kMaxGeoBytes) {
            std::printf("[Cooker] ERRO: geometria derivada excede o teto. --meshes %u com --tess "
                        "%u leva a ultima malha a tess %llu; seriam %.1f GiB de .smesh (max 2 GiB).\n"
                        "          A tesselacao cresce com o indice da malha (base + M*2).\n",
                        _MeshCount, _BaseTess, static_cast<unsigned long long>(LastTess),
                        Overflow ? 1e9 : static_cast<double>(GeoBytes) / (1024.0*1024.0*1024.0));
            return 2;
        }
        if (SceneBytes > kMaxSceneBytes) {
            std::printf("[Cooker] ERRO: cena derivada excede o teto: %u instancias seriam %.1f GiB "
                        "de .sscene (max 2 GiB).\n", _Instances,
                        static_cast<double>(SceneBytes) / (1024.0*1024.0*1024.0));
            return 2;
        }
    }

    std::vector<Smile::SMeshEntry>      Entries;
    std::vector<uint8_t>                Geo;
    std::vector<Smile::SSceneMaterial>  Materials;
    std::vector<Smile::SSceneRenderable> Renderables;
    std::vector<Smile::FRTTriangle>     RtTris;

    // --- Malhas. Tesselacao cresce com o indice: a distribuicao de triangulos por draw importa
    // para o custo de VS, e malhas identicas dariam um perfil que nenhuma cena real tem.
    size_t TotalTris = 0;
    std::vector<Vertex>   Vertices;
    std::vector<uint32_t> Indices;
    for (uint32_t M = 0; M < _MeshCount; ++M) {
        const uint32_t Tess = _BaseTess + M * 2u;
        float MinP[3], MaxP[3];
        BuildGridMesh(Tess, Vertices, Indices, MinP, MaxP);

        Smile::SMeshEntry E{};
        E.VertexCount = static_cast<uint32_t>(Vertices.size());
        E.IndexCount  = static_cast<uint32_t>(Indices.size());
        for (int c = 0; c < 3; ++c) { E.AABBMin[c] = MinP[c]; E.AABBMax[c] = MaxP[c]; }

        E.VertexOffset = Geo.size();
        Geo.insert(Geo.end(), reinterpret_cast<uint8_t*>(Vertices.data()),
                   reinterpret_cast<uint8_t*>(Vertices.data()) + Vertices.size() * sizeof(Vertex));
        E.IndexOffset = Geo.size();
        Geo.insert(Geo.end(), reinterpret_cast<uint8_t*>(Indices.data()),
                   reinterpret_cast<uint8_t*>(Indices.data()) + Indices.size() * sizeof(uint32_t));

        // Mesmo contrato posicional da v8: um registro por PrimitiveIndex, gerado DEPOIS do
        // winding final (aqui o winding ja e o definitivo, nao ha weld nem inversao depois).
        Smile::BuildRTTriangles(Vertices.data(), static_cast<uint32_t>(Vertices.size()),
                                Indices.data(), static_cast<uint32_t>(Indices.size()), RtTris);
        E.RTTriangleOffset = Geo.size();
        E.RTTriangleCount  = static_cast<uint32_t>(RtTris.size());
        Geo.insert(Geo.end(), reinterpret_cast<uint8_t*>(RtTris.data()),
                   reinterpret_cast<uint8_t*>(RtTris.data()) + RtTris.size() * sizeof(Smile::FRTTriangle));

        TotalTris += RtTris.size();
        Entries.push_back(E);
    }

    // --- Materiais. Sem textura de proposito: o teste mede submissao e culling, e carregar
    // milhares de texturas mediria o streaming, que e outro eixo e tem regua propria.
    const uint32_t MaskedCount =
        (_MaterialCount * (_MaskedPercent > 100u ? 100u : _MaskedPercent) + 99u) / 100u;
    for (uint32_t K = 0; K < _MaterialCount; ++K) {
        Smile::SSceneMaterial Mat{};
        std::snprintf(Mat.Name, Smile::kCookedMaxName, "synthetic_mat_%u", K);
        const float T = _MaterialCount > 1
                      ? static_cast<float>(K) / static_cast<float>(_MaterialCount - 1u)
                      : 0.0f;
        Mat.BaseColorFactor[0] = 0.25f + 0.70f * T;
        Mat.BaseColorFactor[1] = 0.55f;
        Mat.BaseColorFactor[2] = 0.95f - 0.70f * T;
        Mat.BaseColorFactor[3] = 1.0f;
        Mat.MetallicFactor  = (K % 3u == 0u) ? 1.0f : 0.0f;
        Mat.RoughnessFactor = 0.15f + 0.80f * T;
        // Os primeiros K materiais sao mascarados. Sem alpha map, o clip cai no fator de alpha,
        // que fica em 1.0 — o objetivo aqui e exercitar o PSO e o laco separados do prepass,
        // nao recortar pixel.
        if (K < MaskedCount) {
            Mat.AlphaTest   = 1;
            Mat.AlphaCutoff = 0.5f;
            Mat.TwoSided    = 1;
        }
        Materials.push_back(Mat);
    }

    // --- Instancias num cubo aproximado. Distribuicao 3D e nao um tapete plano: com tudo no
    // mesmo plano, o frustum culling aceita ou rejeita quase tudo de uma vez e a fracao visivel
    // deixa de responder ao movimento da camera, que e o eixo que o teste precisa exercitar.
    const uint32_t Side = static_cast<uint32_t>(
        std::ceil(std::cbrt(static_cast<double>(_Instances))));
    Renderables.reserve(_Instances);
    for (uint32_t I = 0; I < _Instances; ++I) {
        const uint32_t X = I % Side;
        const uint32_t Y = (I / Side) % Side;
        const uint32_t Z = I / (Side * Side);

        Smile::SSceneRenderable R{};
        R.MeshIndex     = I % _MeshCount;
        R.MaterialIndex = I % _MaterialCount;
        R.Position[0] = (static_cast<float>(X) - 0.5f * Side) * _Spacing;
        R.Position[1] = (static_cast<float>(Y) - 0.5f * Side) * _Spacing;
        R.Position[2] = (static_cast<float>(Z) - 0.5f * Side) * _Spacing;
        R.RotationEuler[0] = 0.0f;
        R.RotationEuler[1] = static_cast<float>(I % 8u) * 0.7853981634f; // 45 graus por passo
        R.RotationEuler[2] = 0.0f;
        R.Scale[0] = R.Scale[1] = R.Scale[2] = 1.0f;
        R.ParentIndex = -1;
        std::snprintf(R.Name, Smile::kCookedMaxName, "synth_%u", I);
        Renderables.push_back(R);
    }

    const double GeoMB = static_cast<double>(Geo.size()) / (1024.0 * 1024.0);
    const double SceneMB = static_cast<double>(
        sizeof(Smile::SSceneHeader) + Materials.size() * sizeof(Smile::SSceneMaterial) +
        Renderables.size() * sizeof(Smile::SSceneRenderable)) / (1024.0 * 1024.0);
    std::printf("[Cooker] Sintetica: %u instancias | %u malhas | %u materiais (%u mascarados) | "
                "%zu triangulos unicos\n",
                _Instances, _MeshCount, _MaterialCount, MaskedCount, TotalTris);
    std::printf("[Cooker] Instancias por malha: %.1f | grid %ux%ux%u | espacamento %.2f\n",
                static_cast<double>(_Instances) / _MeshCount, Side, Side, Side, _Spacing);
    std::printf("[Cooker] Geo: %.1f MB | Cena: %.1f MB\n", GeoMB, SceneMB);

    // Publicacao ATOMICA do par. Os dois arquivos so fazem sentido juntos — o .sscene indexa as
    // malhas do .smesh —, entao escrever direto no destino permitia um .smesh novo ao lado de um
    // .sscene antigo se a segunda escrita falhasse. Grava em temporarios, confere as duas, e so
    // entao renomeia.
    const fs::path MeshPath  = fs::path(_OutBase).concat(".smesh");
    const fs::path ScenePath = fs::path(_OutBase).concat(".sscene");
    const fs::path MeshTmp   = fs::path(_OutBase).concat(".smesh.tmp");
    const fs::path SceneTmp  = fs::path(_OutBase).concat(".sscene.tmp");

    const Smile::SMeshHeader MeshHeader{ Smile::kSMeshMagic, Smile::kCookedVersion,
                                         static_cast<uint32_t>(Entries.size()), 0 };
    const Smile::SSceneHeader SceneHeader{ Smile::kSSceneMagic, Smile::kCookedVersion,
                                           static_cast<uint32_t>(Materials.size()),
                                           static_cast<uint32_t>(Renderables.size()) };

    if (!WriteAllChecked(MeshTmp, &MeshHeader, sizeof(MeshHeader),
                         Entries.data(), Entries.size() * sizeof(Smile::SMeshEntry),
                         Geo.data(), Geo.size())) {
        return 3;
    }
    if (!WriteAllChecked(SceneTmp, &SceneHeader, sizeof(SceneHeader),
                         Materials.data(), Materials.size() * sizeof(Smile::SSceneMaterial),
                         Renderables.data(),
                         Renderables.size() * sizeof(Smile::SSceneRenderable))) {
        ScrubTemp(MeshTmp); // o par nao sai pela metade, e o residuo nao fica mudo
        return 3;
    }

    // Publicacao com ROLLBACK. Renomear os dois em sequencia nao basta: se o primeiro conclui e
    // o segundo falha (destino travado, permissao), fica malha nova ao lado de cena antiga, e o
    // loader encontra "mesh N >= N" — um par que nunca existiu junto. Os destinos existentes vao
    // para .old antes, e qualquer falha restaura o estado anterior.
    //
    // Isto cobre falha de I/O, nao crash no meio: atomicidade real contra queda exigiria
    // generation id no nome e um manifesto publicado por ultimo, que so vale a pena quando o
    // formato tiver um indice proprio.
    // Sufixo UNICO por execucao: um ".old" fixo colide com sobra de uma execucao anterior que
    // caiu no meio, e essa colisao era o gatilho do bug destrutivo.
    const std::string Tag = "." + std::to_string(
        static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())) + ".old";

    const CookerPublish::FPublishResult Pub =
        CookerPublish::PublishPair(MeshTmp, MeshPath, SceneTmp, ScenePath, Tag);
    if (Pub.Error) {
        std::printf("[Cooker] ERRO ao publicar o par: %s\n", Pub.Error.message().c_str());
        if (Pub.RestoreFailed) {
            std::printf("[Cooker] ATENCAO: falha ao restaurar o par anterior. Os arquivos bons\n"
                        "          estao preservados com o sufixo '%s' ao lado do destino.\n",
                        Tag.c_str());
        }
        if (Pub.CleanupFailed) {
            std::printf("[Cooker] ATENCAO: nao foi possivel remover tudo que esta execucao criou;\n"
                        "          pode ter sobrado temporario ou meio par no destino.\n");
        }
        return 3;
    }
    if (Pub.CleanupFailed) {
        // Publicou, mas o backup nao saiu: nao ha perda de dado, ha lixo — e lixo silencioso
        // vira a sobra que colide com a proxima execucao.
        std::printf("[Cooker] ATENCAO: par publicado, mas sobrou backup com o sufixo '%s'.\n",
                    Tag.c_str());
    }

    std::printf("[Cooker] Escrito: %s\n", MeshPath.string().c_str());
    std::printf("[Cooker] Escrito: %s\n", ScenePath.string().c_str());
    std::printf("[Cooker] OK.\n");
    return 0;
}
