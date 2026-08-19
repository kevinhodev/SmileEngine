#pragma once

// ================================================================================================
// Importador glTF 2.0 (.gltf + .glb) do Cooker.
//
// POR QUE glTF, se a engine ja cozinha FBX: e o formato que TODO gerador de malha 3D por IA
// devolve (Meshy, Tripo, Hunyuan3D, TRELLIS, Rodin — .glb em todos), e e o unico caminho de
// entrada que fecha o ciclo "prompt -> malha na cena" sem passar por um DCC no meio. Nao substitui
// o FBX: cena autorada continua vindo por ufbx, com toda a heuristica de material do Bistro.
//
// O que este caminho assume, e que o FBX nao pode assumir: a malha vem de UM asset pequeno, com
// um punhado de materiais, texturas EMBUTIDAS no proprio arquivo e sem convencao de nome nenhuma.
// Dai as duas coisas que so existem aqui — extracao das imagens embutidas para Textures/ e a
// normalizacao de escala/pivot (ver FNormalizeOptions).
//
// Fora de escopo (recusados com mensagem clara, nunca cozidos pela metade): KHR_draco_mesh_
// compression e EXT_meshopt_compression (exigem o decodificador do formato), accessors sparse,
// modos de primitiva que nao sejam TRIANGLES, e skinning/animacao (a engine nao tem runtime para
// isso).
// ================================================================================================

#include "CookedWriter.h"

#include <filesystem>
#include <string>

namespace SmileCook {

    struct FGltfOptions {
        // Aplicada DEPOIS de montar a cena, sobre os transforms — ver NormalizeCookedScene.
        FNormalizeOptions Normalize;
        // Extrai imagens embutidas (bufferView ou data:) para <saida>/Textures/. Desligado, o
        // material sai sem textura: util quando so se quer a geometria.
        bool ExtractTextures = true;
    };

    // Le _InPath (.gltf ou .glb) e escreve _OutBase + ".smesh"/".sscene". Devolve false com
    // _Error preenchido; nada e escrito em caso de falha.
    bool CookGltf(const std::filesystem::path& _InPath, const std::filesystem::path& _OutBase,
                  const FGltfOptions& _Options, std::string& _Error);
}
