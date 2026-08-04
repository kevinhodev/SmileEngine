#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    class FScene;

    // Levantamento dos triangulos emissivos da cena — primeira fase do projeto de mesh lights.
    //
    // Existe para MEDIR antes de escolher a estrategia de amostragem inicial, porque e a ordem de
    // grandeza da contagem que decide, e nao a intuicao:
    //   centenas          -> proposta uniforme ainda funciona
    //   milhares          -> alias table por potencia vira obrigatoria
    //   dezenas de milhar -> alias table + ReGIR consertado (o pool de 64 slots por celula colapsa
    //                        nessa faixa, ver a auditoria do ReGIR)
    //
    // O que NAO da para levantar aqui: area e fluxo por triangulo. As posicoes dos vertices vivem
    // so na GPU depois do upload (FGpuMesh nao guarda copia na CPU), entao area = |cross(e1,e2)|/2
    // e o fluxo = area*pi*luminancia so saem no passe de extracao. A contagem, que e o numero que
    // destrava a decisao, vem de FGpuMesh::GetIndexCount() e esta disponivel de graca.
    class FMeshLights {
    public:
        struct FStats {
            u32 EmissiveMeshes      = 0; // renderables com material emissivo
            u32 EmissiveTriangles   = 0; // total de triangulos emissivos (o numero que decide)
            u32 MeshesWithMap       = 0; // precisam amostrar textura emissiva na extracao
            u32 TrianglesWithMap    = 0;
            u32 LargestMeshTris     = 0; // maior contribuinte isolado
            u32 TotalRenderables    = 0;
            u32 TotalTriangles      = 0;
        };

        // Percorre a cena e preenche as estatisticas. Barato: so le material e contagem de indices,
        // sem tocar em GPU. Chamado por cena.
        void Survey(const FScene& Scene);

        // Emite o resumo no log, com a leitura do que a contagem implica para a amostragem.
        void LogSummary() const;

        const FStats& Stats() const { return SceneStats; }
        u32  EmissiveTriangleCount() const { return SceneStats.EmissiveTriangles; }
        bool HasEmissiveGeometry()   const { return SceneStats.EmissiveTriangles > 0; }

    private:
        FStats SceneStats{};
    };
}
