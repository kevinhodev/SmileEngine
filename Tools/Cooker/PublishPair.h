#pragma once

// Publicacao ATOMICA de um par de arquivos que so faz sentido junto (.smesh + .sscene: o
// segundo indexa as malhas do primeiro).
//
// Mora num header proprio, com as operacoes de filesystem INJETAVEIS, por um motivo especifico:
// o ramo "primeiro publicado, segundo falhou" nao e alcancavel de fora do processo. Travar um
// destino antes de rodar a ferramenta faz a falha acontecer no BACKUP, nunca na publicacao,
// porque os dois backups acontecem antes de qualquer publicacao. Sem injecao, provar esse ramo
// exigiria um failpoint por variavel de ambiente dentro da ferramenta — hook de teste em codigo
// de producao — ou uma corrida, que nao e teste.
//
// O bug que este codigo existe para nao repetir era DESTRUTIVO: o rollback removia os destinos
// incondicionalmente, entao uma falha no BACKUP (quando nada tinha sido publicado) apagava o par
// valido. A regra e: so desfazer o que ESTA execucao fez.

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace CookerPublish {
    namespace fs = std::filesystem;

    // Operacoes injetaveis. O default e o filesystem real; o teste substitui para forcar falha
    // num passo especifico.
    //
    // TODAS devolvem std::error_code, inclusive Exists e Remove. Nao e simetria por gosto:
    //
    //   Exists — devolver so `bool` transforma FALHA DE CONSULTA em "arquivo ausente". Com
    //   MOVEFILE_REPLACE_EXISTING (o que o fs::rename do MSVC usa), pular o backup por causa
    //   disso faz o publish SOBRESCREVER o arquivo anterior; se o segundo publish falhar depois,
    //   nao ha backup para restaurar e o dado morreu. `OutExists` so e valido com erro vazio.
    //
    //   Remove — descartar o erro deixa temporario ou meio par no disco sem ninguem saber, e o
    //   resultado ainda diria RestoreFailed=false. Quem chama precisa poder avisar.
    struct FFileOps {
        std::function<std::error_code(const fs::path&, bool&)> Exists =
            [](const fs::path& P, bool& Out) {
                std::error_code E; Out = fs::exists(P, E); return E;
            };
        std::function<std::error_code(const fs::path&, const fs::path&)> Rename =
            [](const fs::path& From, const fs::path& To) {
                std::error_code E; fs::rename(From, To, E); return E;
            };
        std::function<std::error_code(const fs::path&)> Remove =
            [](const fs::path& P) {
                std::error_code E;
                fs::remove(P, E);
                return E;
            };
    };

    struct FPublishResult {
        std::error_code Error;          // vazio = publicou
        bool RestoreFailed  = false;    // rollback nao conseguiu devolver o par anterior
        bool CleanupFailed  = false;    // sobrou temporario/meio par que nao foi possivel remover
        // Rastreio do que REALMENTE aconteceu. Exposto porque e o que o teste precisa afirmar:
        // "nao removeu o que nao publicou" nao e observavel so pelos arquivos quando o rollback
        // funciona.
        bool MeshBackedUp   = false;
        bool SceneBackedUp  = false;
        bool MeshPublished  = false;
        bool ScenePublished = false;
    };

    // Publica (MeshTmp -> MeshPath, SceneTmp -> ScenePath) com rollback. BackupTag deve ser
    // UNICO por execucao: um sufixo fixo colide com sobra de um crash anterior, e essa colisao
    // era exatamente o gatilho do bug destrutivo.
    inline FPublishResult PublishPair(const fs::path& _MeshTmp,  const fs::path& _MeshPath,
                                      const fs::path& _SceneTmp, const fs::path& _ScenePath,
                                      const std::string& _BackupTag,
                                      const FFileOps& _Ops = FFileOps{}) {
        FPublishResult R;
        const fs::path MeshOld  = fs::path(_MeshPath).concat(_BackupTag);
        const fs::path SceneOld = fs::path(_ScenePath).concat(_BackupTag);

        // Consulta os DOIS destinos ANTES de qualquer mutacao. Erro de consulta aborta com o
        // disco intacto: adivinhar "nao existe" aqui e o que levaria a sobrescrever sem backup.
        bool HadMesh = false, HadScene = false;
        R.Error = _Ops.Exists(_MeshPath, HadMesh);
        if (!R.Error) R.Error = _Ops.Exists(_ScenePath, HadScene);
        if (R.Error) return R; // nada foi tocado

        if (HadMesh) {
            R.Error = _Ops.Rename(_MeshPath, MeshOld);
            R.MeshBackedUp = !R.Error;
        }
        if (!R.Error && HadScene) {
            R.Error = _Ops.Rename(_ScenePath, SceneOld);
            R.SceneBackedUp = !R.Error;
        }
        if (!R.Error) { R.Error = _Ops.Rename(_MeshTmp,  _MeshPath);  R.MeshPublished  = !R.Error; }
        if (!R.Error) { R.Error = _Ops.Rename(_SceneTmp, _ScenePath); R.ScenePublished = !R.Error; }

        if (R.Error) {
            // So desfaz o proprio trabalho. Remover incondicionalmente destruia o par valido
            // quando a falha era no backup e nada tinha sido publicado.
            //
            // Falha de remocao NAO e ignorada: sem isso, um meio par publicado que nao pode ser
            // removido ficava no disco com RestoreFailed=false, ou seja, o resultado dizia que
            // estava tudo desfeito.
            auto Scrub = [&](const fs::path& _P) { if (_Ops.Remove(_P)) R.CleanupFailed = true; };
            if (R.MeshPublished)  Scrub(_MeshPath);
            if (R.ScenePublished) Scrub(_ScenePath);
            Scrub(_MeshTmp);
            Scrub(_SceneTmp);

            // Restauracao conferida: se falhar, o backup e o unico dado bom que sobrou e quem
            // chamou precisa avisar onde ele esta.
            if (R.MeshBackedUp  && _Ops.Rename(MeshOld,  _MeshPath))  R.RestoreFailed = true;
            if (R.SceneBackedUp && _Ops.Rename(SceneOld, _ScenePath)) R.RestoreFailed = true;
            return R;
        }

        // Backups do caminho feliz: falhar aqui nao perde dado (o par novo esta publicado), mas
        // deixa lixo, e o chamador merece saber.
        if (R.MeshBackedUp  && _Ops.Remove(MeshOld))  R.CleanupFailed = true;
        if (R.SceneBackedUp && _Ops.Remove(SceneOld)) R.CleanupFailed = true;
        return R;
    }
}
