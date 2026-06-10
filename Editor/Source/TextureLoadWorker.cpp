#include "SmileEditor/TextureLoadWorker.h"
#include <objbase.h>

namespace SmileEditor {
    TextureLoadWorker::TextureLoadWorker() {
        // Init/Cleanup rodam no thread do worker (started/finished sao emitidos la).
        connect(&Thread, &QThread::started,  this, &TextureLoadWorker::Init);
        connect(&Thread, &QThread::finished, this, &TextureLoadWorker::Cleanup);
        // Move este QObject para o thread encapsulado e o inicia. A partir daqui, os
        // slots (Process) executam no thread do worker via conexoes queued.
        moveToThread(&Thread);
        Thread.start();
    }

    TextureLoadWorker::~TextureLoadWorker() {
        // Chamado no thread que possui o worker (principal): para o thread com seguranca
        // antes de destruir o membro QThread.
        Thread.quit();
        Thread.wait();
    }

    void TextureLoadWorker::Init() {
        // WIC eh COM: o thread do worker precisa de um apartment (MTA).
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    void TextureLoadWorker::Cleanup() {
        CoUninitialize();
    }

    void TextureLoadWorker::Process(quint64 _ReqId, int _Slot,
                                    const QString& _Path, bool _IsNormalMap) {
        std::shared_ptr<Smile::FTextureCPUData> Data;
        if (_Path.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
            // DDS block-compressed (ex.: texturas Bistro). sRGB p/ BaseColor (slot 0) e
            // Emissive (slot 4); linear p/ Normal/Specular/dados.
            const bool sRGB = (_Slot == 0 || _Slot == 4);
            Data = std::make_shared<Smile::FTextureCPUData>(
                Smile::FTexture::LoadDDSCPU(_Path.toStdWString(), sRGB));
        } else {
            Data = std::make_shared<Smile::FTextureCPUData>(
                Smile::FTexture::LoadCPU(_Path.toStdWString(), _IsNormalMap));
        }
        emit Loaded(_ReqId, _Slot, Data);
    }
}
