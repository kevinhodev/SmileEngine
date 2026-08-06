#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include "SmileEditor/RenderThread.h"
#include "Smile/Math/Math.h"

namespace SmileEditor {
    // Camada AUTORADA da cena, persistida em <cena>.smap.
    //
    // O .sscene e um ASSET cozido do FBX: read-only, regenerado a cada re-cook, e fora do
    // controle de versao (derivado). Tudo que o usuario faz no editor — mover, esconder,
    // duplicar, apagar — nao cabe nele. Antes desta classe simplesmente NAO cabia em lugar
    // nenhum: criar e apagar objeto funcionava e se perdia ao fechar o editor.
    //
    // O .smap e um DIFF contra o cozido, nao uma copia da cena. Isso e o que o mantem legivel,
    // pequeno e util em merge — e o que faz um re-cook do FBX continuar valendo, desde que a
    // ordem dos renderaveis nao mude. Objetos sao referenciados por FRenderable::CookedIndex
    // (a origem no asset), nunca pelo indice na lista viva, que anda a cada remocao, nem pelo
    // Id, que so vale dentro da sessao.
    //
    // ESCOPO: cobre a camada de OBJETOS (visibilidade, transform, criados, apagados). O
    // <cena>.materials.json e o <cena>.terrain.json seguem separados de proposito — aquele e
    // chaveado pela identidade do MATERIAL e vale para o asset em qualquer mapa, e este e um
    // descritor de IMPORTACAO (caminho do heightmap, escala), nao estado autorado. O
    // <cena>.lights.json continua no LightsBridge por ora.
    class SceneDocument : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool dirty READ Dirty NOTIFY DirtyChanged)
        Q_PROPERTY(bool available READ Available NOTIFY DirtyChanged)

    public:
        explicit SceneDocument(QObject* Parent = nullptr);

        void SetRenderer(RendererHandle R);
        // Depois de LoadCookedScene: aplica o .smap ao lado da cena, se existir.
        void OnSceneLoaded(const QString& ScenePath, bool Additive);

        bool Dirty() const     { return IsDirty; }
        bool Available() const { return !MapPath.isEmpty(); }

        // Chamado por quem muta a camada autorada (outliner: criar/apagar/esconder; gizmo: mover).
        Q_INVOKABLE void markDirty();
        Q_INVOKABLE bool save();
        Q_INVOKABLE QString mapPath() const { return MapPath; }

    signals:
        void DirtyChanged();
        void Applied(); // .smap aplicado: as pontes precisam reconstruir suas listas

    private:
        bool Apply(const QString& Json);

        RendererHandle Renderer;
        QString        MapPath;
        // Quantos renderaveis o cozido trouxe. Fronteira entre "veio do asset" e "criado aqui".
        int            CookedCount = 0;
        bool           IsDirty = false;

        // Transform de cada renderavel COMO O ASSET o trouxe, tirado antes de aplicar o .smap e
        // indexado pelo CookedIndex. E a referencia que decide se um objeto foi movido pelo
        // usuario: sem ela o save gravaria um override por objeto da cena (1591 na Bistro)
        // descrevendo o proprio cozido, e o arquivo deixaria de ser um diff. Mora aqui e nao no
        // FRenderable porque e estado de PERSISTENCIA — a cena nao precisa saber disto.
        struct FBaseline { Smile::Vec3 Position, Rotation, Scale; };
        QVector<FBaseline> Baseline;
    };
}
