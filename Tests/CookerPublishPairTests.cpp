// Ramos do publicador de par do cooker (Tools/Cooker/PublishPair.h).
//
// Existe porque o ramo "primeiro publicado, segundo falhou" NAO e alcancavel de fora do
// processo: travar um destino antes de rodar a ferramenta faz a falha cair no BACKUP, ja que os
// dois backups acontecem antes de qualquer publicacao. Provar esse ramo por fora exigiria uma
// corrida — que nao e teste — ou um failpoint por variavel de ambiente dentro da ferramenta, que
// e hook de teste em codigo de producao. Aqui as operacoes de filesystem sao injetadas e cada
// ramo e forcado deterministicamente.
//
// O bug guardado era DESTRUTIVO: o rollback removia os destinos incondicionalmente, entao uma
// falha no BACKUP — quando nada tinha sido publicado — apagava o par valido.

#include "../Tools/Cooker/PublishPair.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {
    int Failures = 0;

    void Check(bool _Ok, const char* _What) {
        if (!_Ok) { std::fprintf(stderr, "[FALHA] %s\n", _What); ++Failures; }
    }

    namespace fs = std::filesystem;

    // Filesystem em memoria: um conjunto de caminhos existentes, mais a regra de qual rename
    // deve falhar. Sem disco, sem locks, sem corrida.
    struct FFakeFs {
        std::set<std::string>    Files;
        std::vector<std::string> Renames; // "from -> to", na ordem
        int  FailOnRename = -1;           // indice (0-based) do rename que falha; -1 = nenhum
        int  RenameCount  = 0;
        // Falha de CONSULTA e de REMOCAO por caminho. Sem poder representa-las, o fake nao
        // conseguia expressar os dois modos que levam a perda silenciosa de dado.
        std::set<std::string> FailExistsOn;
        std::set<std::string> FailRemoveOn;

        CookerPublish::FFileOps Ops() {
            CookerPublish::FFileOps O;
            O.Exists = [this](const fs::path& P, bool& Out) -> std::error_code {
                if (FailExistsOn.count(P.string()))
                    return std::make_error_code(std::errc::permission_denied);
                Out = Files.count(P.string()) != 0;
                return {};
            };
            O.Rename = [this](const fs::path& From, const fs::path& To) -> std::error_code {
                const int Index = RenameCount++;
                Renames.push_back(From.filename().string() + " -> " + To.filename().string());
                if (Index == FailOnRename)
                    return std::make_error_code(std::errc::permission_denied);
                auto It = Files.find(From.string());
                if (It == Files.end())
                    return std::make_error_code(std::errc::no_such_file_or_directory);
                Files.erase(It);
                Files.insert(To.string());
                return {};
            };
            O.Remove = [this](const fs::path& P) -> std::error_code {
                if (FailRemoveOn.count(P.string()))
                    return std::make_error_code(std::errc::permission_denied);
                Files.erase(P.string());
                return {};
            };
            return O;
        }
    };

    const fs::path kMeshTmp   = "/x/P.smesh.tmp";
    const fs::path kMeshPath  = "/x/P.smesh";
    const fs::path kSceneTmp  = "/x/P.sscene.tmp";
    const fs::path kScenePath = "/x/P.sscene";
    const std::string kTag    = ".123.old";

    FFakeFs MakeFsWithExistingPair() {
        FFakeFs F;
        F.Files = { kMeshTmp.string(), kSceneTmp.string(),
                    kMeshPath.string(), kScenePath.string() };
        return F;
    }

    bool Has(const FFakeFs& _F, const fs::path& _P) {
        return _F.Files.count(_P.string()) != 0;
    }
}

int main() {
    // --- 1. Caminho feliz: publica os dois e apaga os backups.
    {
        FFakeFs F = MakeFsWithExistingPair();
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(!R.Error, "feliz: sem erro");
        Check(R.MeshPublished && R.ScenePublished, "feliz: os dois publicados");
        Check(Has(F, kMeshPath) && Has(F, kScenePath), "feliz: par no destino");
        Check(!Has(F, fs::path(kMeshPath).concat(kTag)), "feliz: backup do mesh removido");
        Check(!Has(F, fs::path(kScenePath).concat(kTag)), "feliz: backup da cena removido");
    }

    // --- 2. Falha no PRIMEIRO backup. Era o caso destrutivo: nada foi publicado, entao NENHUM
    // destino pode ser removido.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailOnRename = 0;
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "backup1: reporta erro");
        Check(!R.MeshBackedUp && !R.MeshPublished && !R.ScenePublished,
              "backup1: nada foi publicado");
        Check(Has(F, kMeshPath),  "backup1: .smesh PRESERVADO");
        Check(Has(F, kScenePath), "backup1: .sscene PRESERVADO");
        Check(!R.RestoreFailed, "backup1: nada a restaurar");
    }

    // --- 3. Falha no SEGUNDO backup: o mesh ja foi para o backup e precisa VOLTAR.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailOnRename = 1;
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "backup2: reporta erro");
        Check(R.MeshBackedUp && !R.SceneBackedUp, "backup2: so o mesh foi para backup");
        Check(!R.MeshPublished && !R.ScenePublished, "backup2: nada publicado");
        Check(Has(F, kMeshPath),  "backup2: .smesh restaurado");
        Check(Has(F, kScenePath), "backup2: .sscene intocado");
        Check(!R.RestoreFailed, "backup2: restauracao ok");
    }

    // --- 4. O RAMO QUE SO ESTE TESTE ALCANCA: mesh publicado, cena falhou. Os dois backups
    // deram certo, o primeiro publish deu certo, o segundo nao. O par nao pode ficar misturado.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailOnRename = 3; // 0,1 = backups; 2 = publish do mesh; 3 = publish da cena
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "publish2: reporta erro");
        Check(R.MeshBackedUp && R.SceneBackedUp, "publish2: os dois backups aconteceram");
        Check(R.MeshPublished && !R.ScenePublished,
              "publish2: mesh publicado, cena nao (o ramo em questao)");
        Check(Has(F, kMeshPath),  "publish2: .smesh restaurado ao ANTERIOR");
        Check(Has(F, kScenePath), "publish2: .sscene restaurado ao ANTERIOR");
        Check(!Has(F, fs::path(kMeshPath).concat(kTag)),  "publish2: backup do mesh consumido");
        Check(!Has(F, fs::path(kScenePath).concat(kTag)), "publish2: backup da cena consumido");
        Check(!Has(F, kMeshTmp) && !Has(F, kSceneTmp), "publish2: temporarios removidos");
        Check(!R.RestoreFailed, "publish2: restauracao ok");
    }

    // --- 5. Destino VAZIO (primeira geracao): nao ha backup, e a falha na publicacao nao pode
    // inventar restauracao.
    {
        FFakeFs F;
        F.Files = { kMeshTmp.string(), kSceneTmp.string() };
        F.FailOnRename = 1; // 0 = publish do mesh; 1 = publish da cena
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "vazio: reporta erro");
        Check(!R.MeshBackedUp && !R.SceneBackedUp, "vazio: nao havia o que salvar");
        Check(R.MeshPublished && !R.ScenePublished, "vazio: so o mesh saiu");
        Check(!Has(F, kMeshPath) && !Has(F, kScenePath),
              "vazio: o mesh publicado foi removido (nao deixa meio par)");
        Check(!R.RestoreFailed, "vazio: nada a restaurar");
    }

    // --- 6. Restauracao que TAMBEM falha: precisa ser reportada, senao o backup vira arquivo
    // orfao que ninguem sabe que existe.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailOnRename = 3; // publish da cena falha...
        auto Ops = F.Ops();
        const auto InnerRename = Ops.Rename;
        Ops.Rename = [&](const fs::path& From, const fs::path& To) {
            // ...e a restauracao do mesh tambem.
            if (From.string().find(kTag) != std::string::npos && To == kMeshPath)
                return std::make_error_code(std::errc::permission_denied);
            return InnerRename(From, To);
        };
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, Ops);
        Check(static_cast<bool>(R.Error), "restore: reporta erro de publicacao");
        Check(R.RestoreFailed, "restore: sinaliza que a restauracao falhou");
    }

    // --- 7. Falha de CONSULTA. Se `exists` erra e isso vira "nao existe", o backup e pulado e o
    // publish SOBRESCREVE o anterior (fs::rename do MSVC usa MOVEFILE_REPLACE_EXISTING). Se o
    // segundo publish falhar depois, nao ha backup e o dado morreu. Tem de abortar com o disco
    // INTACTO, antes de qualquer mutacao.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailExistsOn = { kMeshPath.string() };
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "exists1: reporta erro de consulta");
        Check(F.RenameCount == 0, "exists1: NENHUM rename foi tentado");
        Check(Has(F, kMeshPath) && Has(F, kScenePath), "exists1: DESTINOS intocados");
        // Os temporarios sao desta execucao: abandona-los seria vazamento silencioso, e a sobra
        // e justamente o que colide com a proxima execucao.
        Check(!Has(F, kMeshTmp) && !Has(F, kSceneTmp), "exists1: temporarios removidos");
        Check(!R.CleanupFailed, "exists1: limpeza sem erro");
        Check(!R.MeshBackedUp && !R.MeshPublished, "exists1: nada marcado como feito");
    }
    // Mesma coisa para o SEGUNDO destino: a consulta dos dois acontece antes da primeira mutacao.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailExistsOn = { kScenePath.string() };
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "exists2: reporta erro de consulta");
        Check(F.RenameCount == 0, "exists2: nenhum rename tentado (consulta vem antes de tudo)");
        Check(Has(F, kMeshPath) && Has(F, kScenePath), "exists2: destinos intocados");
        Check(!Has(F, kMeshTmp) && !Has(F, kSceneTmp), "exists2: temporarios removidos");
    }
    // Consulta falha E o temporario nao sai: o vazamento tem de ser SINALIZADO, senao o chamador
    // conclui que nao sobrou nada.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailExistsOn = { kMeshPath.string() };
        F.FailRemoveOn = { kSceneTmp.string() };
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "exists+lixo: reporta erro de consulta");
        Check(R.CleanupFailed, "exists+lixo: SINALIZA o temporario que ficou");
        Check(Has(F, kSceneTmp), "exists+lixo: o temporario de fato ficou");
        Check(Has(F, kMeshPath) && Has(F, kScenePath), "exists+lixo: destinos intocados");
    }

    // --- 8. Falha de REMOCAO no rollback. O mesh publicado nao pode ser removido, entao sobra
    // MEIO PAR no disco: isso precisa aparecer em CleanupFailed, senao o resultado afirma que
    // esta tudo desfeito quando nao esta.
    {
        FFakeFs F;
        F.Files = { kMeshTmp.string(), kSceneTmp.string() }; // primeira geracao, sem backup
        F.FailOnRename  = 1;                                 // publish da cena falha
        F.FailRemoveOn  = { kMeshPath.string() };            // e o mesh publicado nao sai
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(static_cast<bool>(R.Error), "cleanup: reporta erro");
        Check(R.MeshPublished && !R.ScenePublished, "cleanup: meio par publicado");
        Check(R.CleanupFailed, "cleanup: SINALIZA que a limpeza falhou");
        Check(Has(F, kMeshPath), "cleanup: o meio par de fato ficou no disco");
        Check(!R.RestoreFailed, "cleanup: nao havia backup a restaurar");
    }

    // --- 9. Falha de remocao no caminho FELIZ (backup nao sai). Nao ha perda de dado — o par
    // novo esta publicado —, mas sobra lixo, e o resultado tem de dizer.
    {
        FFakeFs F = MakeFsWithExistingPair();
        F.FailRemoveOn = { fs::path(kMeshPath).concat(kTag).string() };
        const auto R = CookerPublish::PublishPair(kMeshTmp, kMeshPath, kSceneTmp, kScenePath,
                                                  kTag, F.Ops());
        Check(!R.Error, "feliz+lixo: publicou");
        Check(R.CleanupFailed, "feliz+lixo: sinaliza backup nao removido");
        Check(Has(F, fs::path(kMeshPath).concat(kTag)), "feliz+lixo: backup ficou mesmo");
    }

    if (Failures > 0) {
        std::fprintf(stderr, "[PUBLISH] %d falha(s)\n", Failures);
        return 1;
    }
    std::printf("[PUBLISH] ok - 9 cenarios, incluindo os inalcancaveis de fora do processo\n");
    return 0;
}
