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
    struct FFileOps {
        std::function<bool(const fs::path&)> Exists =
            [](const fs::path& P) { std::error_code E; return fs::exists(P, E); };
        std::function<std::error_code(const fs::path&, const fs::path&)> Rename =
            [](const fs::path& From, const fs::path& To) {
                std::error_code E; fs::rename(From, To, E); return E;
            };
        std::function<void(const fs::path&)> Remove =
            [](const fs::path& P) { std::error_code E; fs::remove(P, E); };
    };

    struct FPublishResult {
        std::error_code Error;          // vazio = publicou
        bool RestoreFailed  = false;    // rollback nao conseguiu devolver o par anterior
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

        if (_Ops.Exists(_MeshPath)) {
            R.Error = _Ops.Rename(_MeshPath, MeshOld);
            R.MeshBackedUp = !R.Error;
        }
        if (!R.Error && _Ops.Exists(_ScenePath)) {
            R.Error = _Ops.Rename(_ScenePath, SceneOld);
            R.SceneBackedUp = !R.Error;
        }
        if (!R.Error) { R.Error = _Ops.Rename(_MeshTmp,  _MeshPath);  R.MeshPublished  = !R.Error; }
        if (!R.Error) { R.Error = _Ops.Rename(_SceneTmp, _ScenePath); R.ScenePublished = !R.Error; }

        if (R.Error) {
            // So desfaz o proprio trabalho. Remover incondicionalmente destruia o par valido
            // quando a falha era no backup e nada tinha sido publicado.
            if (R.MeshPublished)  _Ops.Remove(_MeshPath);
            if (R.ScenePublished) _Ops.Remove(_ScenePath);
            _Ops.Remove(_MeshTmp);
            _Ops.Remove(_SceneTmp);

            // Restauracao conferida: se falhar, o backup e o unico dado bom que sobrou e quem
            // chamou precisa avisar onde ele esta.
            if (R.MeshBackedUp  && _Ops.Rename(MeshOld,  _MeshPath))  R.RestoreFailed = true;
            if (R.SceneBackedUp && _Ops.Rename(SceneOld, _ScenePath)) R.RestoreFailed = true;
            return R;
        }

        if (R.MeshBackedUp)  _Ops.Remove(MeshOld);
        if (R.SceneBackedUp) _Ops.Remove(SceneOld);
        return R;
    }
}
