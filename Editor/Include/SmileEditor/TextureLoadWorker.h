#pragma once

#include <QObject>
#include <QThread>
#include <QString>
#include <memory>
#include "Smile/Graphics/Texture.h"

namespace SmileEditor {
    // Ponteiro compartilhado com os dados CPU decodificados, transferido do worker
    // para o thread principal via sinal queued (precisa ser metatype registrado).
    using TexCPUPtr = std::shared_ptr<Smile::FTextureCPUData>;

    // Worker auto-contido: ENCAPSULA seu proprio QThread (composicao) e se move para
    // ele no construtor. Faz APENAS o decode/mipgen em CPU (FTexture::LoadCPU); o upload
    // D3D12 acontece no thread principal (quem recebe o sinal Loaded) — a command queue
    // da engine nao eh thread-safe.
    //
    // Uso: criar a instancia no thread principal, conectar Loaded e disparar Process
    // (via sinal queued). A destruicao para o thread automaticamente.
    class TextureLoadWorker : public QObject {
        Q_OBJECT

    public:
        explicit TextureLoadWorker();
        ~TextureLoadWorker() override;

    public slots:
        // Decodifica a textura (CPU). Emite Loaded com o resultado (mesmo em falha,
        // onde Data->Valid() sera false).
        void Process(quint64 ReqId, int Slot, const QString& Path, bool IsNormalMap);

    signals:
        void Loaded(quint64 ReqId, int Slot, SmileEditor::TexCPUPtr Data);

    private slots:
        // Conectados a Thread.started/finished: COM precisa estar inicializado no
        // thread do worker para o WIC funcionar.
        void Init();
        void Cleanup();

    private:
        QThread Thread; // composicao: o worker possui o thread em que roda
    };
}

Q_DECLARE_METATYPE(SmileEditor::TexCPUPtr)
