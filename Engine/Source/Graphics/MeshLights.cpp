#include "Smile/Graphics/MeshLights.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/Logger.h"

#include <algorithm>
#include <string>

namespace Smile {
    namespace {
        // Mesmo criterio do FillInstanceGeo (DDGI.cpp): o que vale para o RT e o EmissiveStrength
        // JA escalado pelo RTEmissiveScale. Uma malha com RTEmissiveScale = 0 foi marcada pelo
        // artista como "brilha na tela mas nao ilumina" e nao pode virar mesh light — senao o
        // slider deixaria de significar o que promete.
        bool IsEmissiveForRT(const FMaterial& Material) {
            const MaterialConstants& MC = Material.Constants;
            const f32 Strength = MC.EmissiveStrength * MC.RTEmissiveScale;
            if (Strength <= 0.0f) return false;
            return MC.EmissiveFactor.X > 0.0f || MC.EmissiveFactor.Y > 0.0f ||
                   MC.EmissiveFactor.Z > 0.0f;
        }
    }

    void FMeshLights::Survey(const FScene& _Scene) {
        SceneStats = FStats{};

        for (const FRenderable& R : _Scene.Renderables()) {
            if (!R.Mesh || !R.Mesh->IsValid()) continue;
            const u32 Tris = R.Mesh->GetIndexCount() / 3u;
            ++SceneStats.TotalRenderables;
            SceneStats.TotalTriangles += Tris;

            if (!R.Visible || !R.Material || !IsEmissiveForRT(*R.Material)) continue;

            ++SceneStats.EmissiveMeshes;
            SceneStats.EmissiveTriangles += Tris;
            SceneStats.LargestMeshTris = std::max(SceneStats.LargestMeshTris, Tris);

            // Malha com mapa emissivo exige amostrar a textura por triangulo na extracao (o RTXDI
            // faz SampleGrad anisotropico com uma elipse inscrita no triangulo em UV, para obter a
            // radiancia MEDIA e nao um point sample do centro). Sem mapa, a radiancia e constante
            // e o triangulo sai direto do EmissiveFactor.
            if (R.Material->Constants.HasEmissiveMap) {
                ++SceneStats.MeshesWithMap;
                SceneStats.TrianglesWithMap += Tris;
            }
        }
    }

    void FMeshLights::LogSummary() const {
        const FStats& S = SceneStats;
        if (S.EmissiveTriangles == 0) {
            LogInfo("MeshLights: nenhuma geometria emissiva na cena (" +
                    std::to_string(S.TotalRenderables) + " renderables).");
            return;
        }

        const f64 Pct = S.TotalTriangles > 0
                      ? (100.0 * S.EmissiveTriangles / static_cast<f64>(S.TotalTriangles)) : 0.0;

        LogInfo("MeshLights: " + std::to_string(S.EmissiveTriangles) +
                " triangulos emissivos em " + std::to_string(S.EmissiveMeshes) + " malhas (" +
                std::to_string(static_cast<u32>(Pct + 0.5)) + "% dos " +
                std::to_string(S.TotalTriangles) + " triangulos da cena).");
        LogInfo("MeshLights: com mapa emissivo: " + std::to_string(S.TrianglesWithMap) +
                " triangulos em " + std::to_string(S.MeshesWithMap) +
                " malhas | maior malha isolada: " + std::to_string(S.LargestMeshTris) +
                " triangulos.");

        // A leitura da contagem. Os limiares vem da razao entre candidatas iniciais e tamanho do
        // pool: com M candidatas uniformes de N luzes, a chance de achar a dominante e 1-(1-1/N)^M,
        // que despenca com N. Ver a comparacao com RTXDI/Falcor na revisao do ReSTIR DI.
        if (S.EmissiveTriangles <= 512u) {
            LogInfo("MeshLights: faixa BAIXA — proposta uniforme deve bastar; alias table por "
                    "potencia fica como refinamento.");
        } else if (S.EmissiveTriangles <= 8192u) {
            LogInfo("MeshLights: faixa MEDIA — alias table por potencia vira obrigatoria "
                    "(uniforme perde a luz dominante na maioria dos frames).");
        } else {
            LogInfo("MeshLights: faixa ALTA — alias table por potencia MAIS ReGIR; nesta faixa o "
                    "pool de 64 slots por celula do ReGIR colapsa e precisa do presample "
                    "cooperativo antes de servir de proposta.");
        }
    }
}
