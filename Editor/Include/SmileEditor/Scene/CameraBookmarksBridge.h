#pragma once

#include "SmileEditor/Scene/BookmarkSchema.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    // Quatro poses persistentes por cena. O sidecar preserva dados autorados entre recooks;
    // Restore faz apenas navegacao, enquanto resets de captura pertencem ao FFrameCapture.
    class CameraBookmarksBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(QVariantList slotsFilled READ SlotsFilled NOTIFY SlotsChanged)
        // Propriedade para o binding reavaliar ao sobrescrever um slot preenchido.
        Q_PROPERTY(QVariantList slotLabels READ SlotLabels NOTIFY SlotsChanged)
        Q_PROPERTY(QString      scenePath  READ ScenePath  NOTIFY SlotsChanged)
        Q_PROPERTY(QString      sidecarWarning READ SidecarWarning NOTIFY SlotsChanged)

    public:
        static constexpr int kSlotCount = 4;

        explicit CameraBookmarksBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle R) { Renderer = R; emit SlotsChanged(); }
        // Cargas aditivas preservam o sidecar da cena principal.
        void OnSceneLoaded(const QString& ScenePath, bool Additive);

        QVariantList SlotsFilled() const;
        QVariantList SlotLabels()  const;
        QString      ScenePath()   const { return JsonPath; }
        QString      SidecarWarning() const;

        // Persiste imediatamente para preservar a referencia mesmo apos crash.
        Q_INVOKABLE bool Save(int Slot);
        // SetCameraPose ja publica o corte de camera.
        Q_INVOKABLE bool Restore(int Slot);
        Q_INVOKABLE bool Clear(int Slot);
        Q_INVOKABLE bool HasSlot(int Slot) const;
        // Texto curto para a QML ("x=-14.48 y=3.93 z=0.28 · p=-9.1 y=78.8"), ou vazio.
        Q_INVOKABLE QString SlotLabel(int Slot) const;

    signals:
        void SlotsChanged();
        void Message(const QString& Text);

    private:
        // Tipos e regras vivem no BookmarkSchema, para o gate poder validar o sidecar com o
        // MESMO parser que o editor usa. Alias para nao mexer em nenhum call site.
        using FSlot = BookmarkSchema::FSlot;
        FSlot Slots[kSlotCount];

        // Distingue sidecar ausente de um arquivo que nao pode ser sobrescrito com seguranca.
        using ESidecarState = BookmarkSchema::EState;
        ESidecarState State = ESidecarState::None;

        bool LoadFromDisk();
        bool SaveToDisk();
        // Preserva sidecars ilegiveis em <nome>.bak antes da primeira gravacao.
        bool BackupUnreadableSidecar();
        static bool ValidSlot(int Slot) { return Slot >= 0 && Slot < kSlotCount; }
        float EngineFovYDeg() const;

        RendererHandle Renderer;
        QString        JsonPath; // vazio = nenhuma cena carregada; Save/Restore viram no-op
    };
}
