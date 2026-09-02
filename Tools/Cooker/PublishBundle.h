#pragma once

// Publicacao ATOMICA de um BUNDLE de arquivos que so fazem sentido juntos (.smesh + .sscene +
// .cameras.json: o segundo indexa as malhas do primeiro, e o terceiro e a regua da captura).
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
#include <vector>

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

    struct FPublishItem {
        fs::path Tmp;
        fs::path Dest;
    };

    struct FPublishResult {
        std::error_code Error;          // vazio = publicou
        bool RestoreFailed  = false;    // rollback nao conseguiu devolver o estado anterior
        bool CleanupFailed  = false;    // sobrou temporario/artefato que nao foi possivel remover
        // Rastreio do que REALMENTE aconteceu, por item e na ordem de entrada. Exposto porque e
        // o que o teste precisa afirmar: "nao removeu o que nao publicou" nao e observavel so
        // pelos arquivos quando o rollback funciona.
        std::vector<bool> BackedUp;
        std::vector<bool> Published;
    };

    // Publica N artefatos como UM bundle: ou todos entram, ou nenhum. BackupTag deve ser UNICO
    // por execucao — um sufixo fixo colide com sobra de um crash anterior, e essa colisao era o
    // gatilho do bug destrutivo original.
    //
    // E bundle e nao par porque a cena sintetica tem TRES artefatos que so fazem sentido juntos:
    // .smesh, .sscene e .cameras.json. Publicar o sidecar fora da transacao produzia "par antigo
    // + camera nova" quando a publicacao do par falhava — reproduzido com dados reais.
    inline FPublishResult PublishBundle(const std::vector<FPublishItem>& _Items,
                                        const std::string& _BackupTag,
                                        const FFileOps& _Ops = FFileOps{}) {
        FPublishResult R;
        const size_t N = _Items.size();
        R.BackedUp.assign(N, false);
        R.Published.assign(N, false);

        std::vector<fs::path> Old;
        Old.reserve(N);
        for (const FPublishItem& It : _Items) Old.push_back(fs::path(It.Dest).concat(_BackupTag));

        // Limpeza de algo que ESTA execucao criou. Erro nao e ignorado: temporario abandonado em
        // silencio e a sobra que colide com a proxima execucao.
        auto Scrub = [&](const fs::path& _P) { if (_Ops.Remove(_P)) R.CleanupFailed = true; };
        auto ScrubAllTmps = [&]() { for (const FPublishItem& It : _Items) Scrub(It.Tmp); };

        // Consulta TODOS os destinos ANTES de qualquer mutacao. Erro de consulta aborta sem tocar
        // nos destinos — adivinhar "nao existe" faria o publish sobrescrever sem backup, porque
        // o fs::rename do MSVC usa MOVEFILE_REPLACE_EXISTING —, mas os temporarios sao desta
        // execucao e saem junto: deixa-los seria vazamento silencioso.
        std::vector<bool> Had(N, false);
        for (size_t I = 0; I < N && !R.Error; ++I) {
            bool Exists = false;
            R.Error = _Ops.Exists(_Items[I].Dest, Exists);
            Had[I] = Exists;
        }
        if (R.Error) { ScrubAllTmps(); return R; }

        for (size_t I = 0; I < N && !R.Error; ++I) {
            if (!Had[I]) continue;
            R.Error = _Ops.Rename(_Items[I].Dest, Old[I]);
            R.BackedUp[I] = !R.Error;
        }
        for (size_t I = 0; I < N && !R.Error; ++I) {
            R.Error = _Ops.Rename(_Items[I].Tmp, _Items[I].Dest);
            R.Published[I] = !R.Error;
        }

        if (R.Error) {
            // So desfaz o proprio trabalho. Remover incondicionalmente destruia o estado valido
            // quando a falha era no backup e nada tinha sido publicado.
            for (size_t I = 0; I < N; ++I) if (R.Published[I]) Scrub(_Items[I].Dest);
            ScrubAllTmps();
            // Restauracao CONFERIDA: se falhar, o backup e o unico dado bom que sobrou e quem
            // chamou precisa avisar onde ele esta.
            for (size_t I = 0; I < N; ++I)
                if (R.BackedUp[I] && _Ops.Rename(Old[I], _Items[I].Dest)) R.RestoreFailed = true;
            return R;
        }

        // Backups do caminho feliz: falhar aqui nao perde dado (o bundle novo esta publicado),
        // mas deixa lixo, e o chamador merece saber.
        for (size_t I = 0; I < N; ++I)
            if (R.BackedUp[I] && _Ops.Remove(Old[I])) R.CleanupFailed = true;
        return R;
    }
}
