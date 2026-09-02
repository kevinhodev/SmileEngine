// Valida o sidecar de bookmarks pelo PARSER REAL do editor (SmileEditor::BookmarkSchema).
//
// Existe porque os testes sinteticos so passavam o .sscene ao loader: remover a geracao do
// .cameras.json, emitir JSON invalido ou escrever indices errados nao derrubava nada, e o
// "6/6 sintetico" ficava verde com o bookmark ausente. Um gate que nao abre o artefato nao
// prova que ele existe.
//
// Usa o BookmarkSchema e nao uma reimplementacao: com duas copias das regras, o arquivo que o
// gate aprova deixa de ser o arquivo que o editor aceita, e a divergencia so aparece quando
// alguem tenta capturar.

#include "SmileEditor/Scene/BookmarkSchema.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
    int Failures = 0;

    void Check(bool _Ok, const char* _What) {
        if (!_Ok) { std::fprintf(stderr, "[FALHA] %s\n", _What); ++Failures; }
    }

    using namespace SmileEditor;

    // Os quatro slots existem para exercitar REGIMES diferentes de culling/submissao; se dois
    // deles coincidirem, o baseline mede a mesma coisa duas vezes.
    // _Strict: pose repetida e FALHA quando o chamador declarou --expect-slots, ou seja quando o
    // conjunto de poses ja esta CONGELADO (sinteticas, e as duas cenas de gate). Sem a flag, vira
    // AVISO: as poses ainda estao sendo autoradas e repetir e trabalho por terminar, nao defeito.
    void CheckDistinctPoses(const BookmarkSchema::FSlot* _S, bool _Strict) {
        for (int A = 0; A < BookmarkSchema::kSlotCount; ++A) {
            if (!_S[A].Filled) continue; // slot vazio e tudo-zero: comparar dois deles acusaria
            for (int B = A + 1; B < BookmarkSchema::kSlotCount; ++B) {
                if (!_S[B].Filled) continue;
                const bool SamePos = std::abs(_S[A].PosX - _S[B].PosX) < 1e-3f &&
                                     std::abs(_S[A].PosY - _S[B].PosY) < 1e-3f &&
                                     std::abs(_S[A].PosZ - _S[B].PosZ) < 1e-3f;
                const bool SameDir = std::abs(_S[A].Pitch - _S[B].Pitch) < 1e-3f &&
                                     std::abs(_S[A].Yaw   - _S[B].Yaw)   < 1e-3f;
                if (SamePos && SameDir) {
                    std::fprintf(stderr, "[%s] slots %d e %d tem a MESMA pose\n",
                                 _Strict ? "FALHA" : "AVISO", A, B);
                    if (_Strict) ++Failures;
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    // --expect-slots N exige EXATAMENTE N slots preenchidos; sem ele, basta pelo menos um.
    //
    // A distincao e entre INVALIDO e INCOMPLETO, e quem decide qual e cada cena e o CMakeLists,
    // nao este arquivo. As sinteticas e as duas cenas de GATE (Bistro e Emerald) passam
    // --expect-slots 4: nelas as quatro poses ja existem, e a partir daqui apagar ou duplicar uma
    // e regressao do baseline. As cenas opcionais nao passam a flag, porque travar um numero
    // deixaria a suite vermelha por autoria pendente — suite permanentemente vermelha para de
    // informar. Em todas, o minimo que o gate cobre e o mesmo: o arquivo VERSIONADO, que vai ser
    // editado a mao, continua parseavel e sao.
    if (argc < 2) {
        std::fprintf(stderr,
                     "uso: SmileBookmarkSidecarTests <cena.cameras.json> [--expect-slots N]\n");
        return 2;
    }
    int ExpectSlots = -1;
    for (int A = 2; A < argc; ++A) {
        if (std::string(argv[A]) == "--expect-slots" && A + 1 < argc) {
            ExpectSlots = std::atoi(argv[++A]);
            continue;
        }
        std::fprintf(stderr, "[FALHA] argumento desconhecido: %s\n", argv[A]);
        return 2;
    }

    QFile F(QString::fromLocal8Bit(argv[1]));
    if (!F.exists()) {
        std::fprintf(stderr, "[FALHA] sidecar nao existe: %s\n", argv[1]);
        return 3;
    }
    if (!F.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "[FALHA] nao consegui abrir: %s\n", argv[1]);
        return 3;
    }
    const QByteArray Bytes = F.readAll();
    F.close();

    BookmarkSchema::FSlot Slots[BookmarkSchema::kSlotCount];
    QString Msg;
    // 60 graus e o FOV que o gerador grava; o parser REJEITA pose cujo FOV esta build nao
    // reproduz, entao passar outro valor aqui faria os quatro slots serem recusados.
    const BookmarkSchema::EState State = BookmarkSchema::Parse(Bytes, 60.0f, Slots, &Msg);

    Check(State == BookmarkSchema::EState::Ok,
          "o parser real aceitou o sidecar sem ressalvas");
    if (State != BookmarkSchema::EState::Ok)
        std::fprintf(stderr, "        estado=%d msg=%s\n",
                     static_cast<int>(State), Msg.toLocal8Bit().constData());

    int Filled = 0;
    for (int I = 0; I < BookmarkSchema::kSlotCount; ++I) {
        if (!Slots[I].Filled) continue;
        ++Filled;
        const bool Finite = std::isfinite(Slots[I].PosX) && std::isfinite(Slots[I].PosY) &&
                            std::isfinite(Slots[I].PosZ) && std::isfinite(Slots[I].Pitch) &&
                            std::isfinite(Slots[I].Yaw);
        if (!Finite) {
            std::fprintf(stderr, "[FALHA] slot %d tem componente nao-finito\n", I);
            ++Failures;
        }
    }

    if (ExpectSlots >= 0) {
        Check(Filled == ExpectSlots, "numero de slots preenchidos bate com o esperado");
        if (Filled != ExpectSlots)
            std::fprintf(stderr, "        %d preenchidos, %d esperados\n", Filled, ExpectSlots);
    } else if (Filled == 0) {
        std::fprintf(stderr, "[FALHA] sidecar sem NENHUMA pose utilizavel\n");
        ++Failures;
    }

    CheckDistinctPoses(Slots, ExpectSlots >= 0);

    if (Failures > 0) {
        std::fprintf(stderr, "[BOOKMARK] %d falha(s)\n", Failures);
        return 4;
    }
    std::printf("[BOOKMARK] ok - %d slot(s) valido(s), pelo parser do editor\n", Filled);
    return 0;
}
