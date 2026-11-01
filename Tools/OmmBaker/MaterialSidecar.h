#pragma once

// O sidecar de MATERIAIS do editor (<cena>.materials.json), lido pelo SmileOmmBaker.
//
// Por que o baker precisa dele: o editor aplica esses overrides DEPOIS do load, e eles podem mudar
// entradas do bake de micro-map — alphaTest, alphaCutoff e a textura de albedo (slot 0). O .somm
// gravado so com os valores cozidos nao batia com nenhum desses pares, e eles caiam no bake de
// runtime, com teto (o que podia trazer UNKNOWN de volta e bloquear o 2-state do GI). Com isto o
// baker grava tambem a variante sobrescrita, e o rebake da engine a encontra (LiveOmmBakeKey).
//
// So o que entra no bake e lido. O resto do sidecar (cor, rugosidade, normal...) nao muda micro-map.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/OmmBakeCache.h"
#include "Smile/Graphics/Resources/Texture.h"

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    struct FSceneImportResult;

    struct FMaterialOmmOverride {
        std::optional<bool>        AlphaTest;
        std::optional<f32>         AlphaCutoff;
        // Slot 0 do sidecar: caminho como o editor gravou (UTF-8), ou "" = slot limpo (branco).
        std::optional<std::string> AlbedoPath;
    };

    // Overrides por indice de material COZIDO, resolvidos como o MaterialsBridge::ApplyOverrides:
    // pelo id (v2, CookedMaterialId) e, sem ele, pelo nome (v1). Sidecar ausente devolve vazio e
    // Why vazio; sidecar ilegivel devolve vazio e o motivo em Why.
    std::unordered_map<u32, FMaterialOmmOverride> LoadMaterialOmmOverrides(
        const FSceneImportResult& Scene, std::string& Why);

    // Os jobs das VARIANTES sobrescritas: um por par (malha, material) mascarado DEPOIS do
    // override cujo bake difere do cozido. Texturas de override sao carregadas em OwnedTextures
    // (que tem de viver ate o fim do bake). Log de cada textura que falhar.
    std::vector<FOmmBakeJob> CollectOmmOverrideJobs(
        const FSceneImportResult& Scene,
        const std::unordered_map<u32, FMaterialOmmOverride>& Overrides,
        std::deque<FTextureCPUData>& OwnedTextures);
}
