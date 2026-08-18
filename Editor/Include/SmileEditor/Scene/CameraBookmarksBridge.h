#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
// RendererHandle e obrigatorio, e nao um ponteiro cru por conveniencia: gravar e restaurar pose
// acontece na thread da GUI enquanto a thread de render esta no meio de um frame. O handle
// serializa o acesso — mesma regra de todas as outras bridges.
#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    // ============================================================================================
    // Bookmarks de camera — 4 poses persistentes POR CENA.
    //
    // POR QUE EXISTE
    // O protocolo de medicao da serie SHaRC (Docs/SHARC-PRIMARY-GI-PLAN.md) abre exigindo camera
    // deterministica, e o plano proibe fechar fase por "parece melhor". O que vamos comparar a
    // partir da Fase 3 e ruido, convergencia temporal e ocupacao do hash do radiance cache — todos
    // funcao da posicao EXATA da camera. Reservoir temporal, historico do RELAX e as celulas do
    // cache mudam com deslocamento de centimetros, entao duas fotos tiradas a mao nao distinguem
    // "o estimador mudou" de "a camera esta meio metro a esquerda".
    //
    // POR QUE SIDECAR, E NAO DENTRO DO COZIDO
    // A cena cozida e SAIDA do Cooker a partir do FBX; bookmark e dado AUTORADO no editor. No
    // cozido, o proximo recook os apagaria — o oposto de "versionado no repositorio". O sidecar
    // segue a convencao ja usada por materiais, terreno, visibilidade e luzes
    // (<cena>.materials.json e companhia): viaja com a cena, entra no repo, sobrevive ao recook e
    // nao custa bump de kCookedVersion.
    //
    // O QUE ESTA BRIDGE NAO FAZ
    // Ela nao captura. Restaurar aqui e NAVEGACAO: pose + corte de camera, o mesmo que o
    // duplo-clique do outliner ja faz. A captura deterministica precisa de um reset mais forte
    // que o corte de camera — o HistoryDomain::CameraCut deliberadamente NAO derruba os caches de
    // MUNDO (DDGIAtlas, ReGIR, RadianceCache), porque numa navegacao normal derruba-los faria a GI
    // reconvergir do zero a cada clique. Para uma captura isso se inverte: o estado inicial tem de
    // ser conhecido, e nao "o que sobrou do trajeto ate aqui". Esse dominio e o contador de
    // aquecimento vem no passe seguinte.
    class CameraBookmarksBridge : public QObject {
        Q_OBJECT
        // Quais slots tem pose gravada, para a QML desenhar o estado dos botoes.
        Q_PROPERTY(QVariantList slotsFilled READ SlotsFilled NOTIFY SlotsChanged)
        // Os rotulos como PROPRIEDADE, e nao so o Q_INVOKABLE SlotLabel(). Uma chamada invocavel
        // e avaliada uma vez no binding e nao reavalia sozinha: regravar um slot que JA estava
        // preenchido nao muda `slotsFilled` (continua true), entao a QML seguia mostrando a pose
        // ANTIGA — justo a operacao que mais importa acertar, porque o usuario acabou de decidir
        // que a referencia mudou.
        Q_PROPERTY(QVariantList slotLabels READ SlotLabels NOTIFY SlotsChanged)
        Q_PROPERTY(QString      scenePath  READ ScenePath  NOTIFY SlotsChanged)
        // Estado do sidecar em disco, para a QML poder avisar em vez de deixar o usuario gravar
        // por cima de um arquivo que nao foi entendido.
        Q_PROPERTY(QString      sidecarWarning READ SidecarWarning NOTIFY SlotsChanged)

    public:
        static constexpr int kSlotCount = 4;

        explicit CameraBookmarksBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle R) { Renderer = R; emit SlotsChanged(); }
        // Deriva <cena>.cameras.json e recarrega. Carga ADITIVA nao troca de sidecar: os
        // bookmarks pertencem a cena principal, mesma regra do MaterialsBridge.
        void OnSceneLoaded(const QString& ScenePath, bool Additive);

        QVariantList SlotsFilled() const;
        QVariantList SlotLabels()  const;
        QString      ScenePath()   const { return JsonPath; }
        QString      SidecarWarning() const;

        // Grava a pose ATUAL da camera no slot e persiste na hora. Persistir no clique, e nao ao
        // fechar, e deliberado: uma sessao de captura que termine em crash nao pode levar junto a
        // referencia que torna as capturas anteriores comparaveis.
        Q_INVOKABLE bool Save(int Slot);
        // Teleporta para a pose do slot. Passa pelo Renderer::SetCameraPose, que ja emite o corte
        // de camera — nao duplicar a invalidacao aqui.
        Q_INVOKABLE bool Restore(int Slot);
        Q_INVOKABLE bool Clear(int Slot);
        Q_INVOKABLE bool HasSlot(int Slot) const;
        // Texto curto para a QML ("x=-14.48 y=3.93 z=0.28 · p=-9.1 y=78.8"), ou vazio.
        Q_INVOKABLE QString SlotLabel(int Slot) const;

    signals:
        void SlotsChanged();
        void Message(const QString& Text);

    private:
        struct FSlot {
            bool  Filled = false;
            float PosX = 0.0f, PosY = 0.0f, PosZ = 0.0f;
            float Pitch = 0.0f, Yaw = 0.0f;
            // Constante na engine hoje (Renderer::kFovYDegrees = 60). Gravado mesmo assim: o dia
            // em que o FOV virar editavel, um sidecar sem ele passaria a restaurar a pose certa
            // com o enquadramento errado, e as capturas antigas ficariam incomparaveis em
            // silencio. Um float por slot e barato demais para justificar essa aposta.
            float FovYDeg = 60.0f;
        };
        FSlot Slots[kSlotCount];

        // O que o disco tem AGORA. Existe porque "nao consegui ler" e "nao existe" pedem coisas
        // opostas na hora de gravar: no segundo caso escrever e o comportamento certo, no primeiro
        // escrever DESTROI o unico arquivo — e um sidecar que nao foi entendido pode ser
        // exatamente a referencia de uma sessao de captura anterior, ou o de uma build mais nova.
        enum class ESidecarState {
            None,         // nao existe em disco ainda: gravar e seguro
            Ok,           // lido e entendido
            Corrupt,      // JSON invalido
            Incompatible  // schema de outra versao, ou pose que esta build nao sabe restaurar
        };
        ESidecarState State = ESidecarState::None;

        bool LoadFromDisk();
        bool SaveToDisk();
        // Move o arquivo atual para <nome>.bak antes de a primeira gravacao substituir um sidecar
        // Corrupt/Incompatible. Nao bloqueia o usuario nem perde o que estava la.
        bool BackupUnreadableSidecar();
        static bool ValidSlot(int Slot) { return Slot >= 0 && Slot < kSlotCount; }
        // FOV que ESTA build sabe reproduzir. Enquanto o FOV for constante na engine, um slot
        // gravado com outro valor nao e restauravel: a pose sairia certa e o enquadramento errado,
        // que e a falha silenciosa que o campo foi gravado para impedir.
        float EngineFovYDeg() const;

        RendererHandle Renderer;
        QString        JsonPath; // vazio = nenhuma cena carregada; Save/Restore viram no-op
    };
}
