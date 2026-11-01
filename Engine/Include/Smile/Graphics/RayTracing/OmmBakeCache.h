#pragma once

// O BAKE OFFLINE dos micro-maps: o sidecar <cena>.somm, e quem decide QUAIS bakes uma cena tem.
//
// Por que existe: no BistroExterior o bake de OMM era 22 s de um load de 28 s, refeito a cada
// carga — e mesmo assim incompleto. O teto de workload que segura o load em segundos deixava 113 k
// triangulos mascarados sem micro-map (12,6%), em malhas 4x a 34x acima dele. Nenhum teto de
// runtime cobre isso sem levar o load a minutos. Offline nao ha teto.
//
// Por que SIDECAR, e nao um campo novo no .smesh/.sscene: o sidecar se valida sozinho, par a par,
// pelo hash do CONTEUDO que o loader carregou (UVs, indices, pixels de alpha) e pelos bits de
// alpha/cutoff. Nao precisa de bump de kCookedVersion, nao exige recozinhar as cenas, e um sidecar
// velho nao quebra nada: o par que nao bate volta para o bake de runtime, que continua existindo.
//
// E por que um formato proprio, e nao o omm::Cpu::Serialize do SDK: o que se guarda aqui e o
// resultado ja na forma do D3D12 (FOpacityMicromapData). O load nem precisa do SDK.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    struct FMesh;
    struct FTextureCPUData;
    struct FSceneImportResult;

    // A IDENTIDADE das entradas de um bake, em bits.
    //
    // Indices da cena (malha, material) dizem ONDE o par mora; os hashes e os bits dizem SE o que
    // esta la ainda e o que foi bakeado. As duas metades precisam bater — um recook que reordena
    // malhas mantem indices validos apontando para outra geometria.
    struct FOmmBakeKey {
        u32             MeshIndex     = 0;
        u32             MaterialIndex = 0;
        FOmmContentHash Geometry      = kOmmNoContentHash; // OmmHashGeometry das UVs + indices
        FOmmContentHash Texture       = kOmmNoContentHash; // OmmHashTexture, ou kOmmWhiteTexture
        u32             BaseAlphaBits = 0;                 // bits do f32, nao o valor
        u32             CutoffBits    = 0;

        friend bool operator==(const FOmmBakeKey& A, const FOmmBakeKey& B) {
            return A.MeshIndex == B.MeshIndex && A.MaterialIndex == B.MaterialIndex &&
                   A.Geometry == B.Geometry && A.Texture == B.Texture &&
                   A.BaseAlphaBits == B.BaseAlphaBits && A.CutoffBits == B.CutoffBits;
        }
    };

    // "Sem textura de alpha": o baker usa um plano branco (ver ExtractAlpha), e isso e uma entrada
    // como outra qualquer. Um valor proprio, e nao kOmmNoContentHash — este significa "sem
    // fingerprint" e nunca casa com nada.
    inline const FOmmContentHash kOmmWhiteTexture =
        OmmHashFinish(OmmHashAppend(kOmmHashSeed, "SMILE_OMM_WHITE_ALPHA", 21));

    // Um bake a fazer: o par (malha, material) mascarado e as entradas que o baker consome.
    struct FOmmBakeJob {
        FOmmBakeKey            Key;
        const FMesh*           Mesh         = nullptr;
        const FTextureCPUData* AlphaTexture = nullptr; // nula = plano branco
        f32                    BaseAlpha    = 1.0f;
        f32                    AlphaCutoff  = 0.5f;
    };

    // Os pares (malha, material) mascarados da cena, um job por par, do MAIOR para o menor.
    //
    // E a UNICA enumeracao: o import e o SmileOmmBaker chamam esta funcao sobre o mesmo
    // FSceneImportResult, e e isso que garante que as chaves gravadas offline sejam as que o load
    // procura. Os valores de alpha saem do SSceneMaterial, exatamente como o import monta o
    // FMaterial (BaseColorFactor[3], AlphaCutoff, AlphaTest).
    std::vector<FOmmBakeJob> CollectOmmBakeJobs(const FSceneImportResult& Scene);

    // Bakeia Jobs[Which[i]] para Results[Which[i]], em paralelo, com o teto dado. Results tem de
    // ter Jobs.size() entradas. Lanca na primeira falha.
    //
    // Duas faixas: geometrias grandes uma por vez com o OpenMP interno do SDK, as pequenas
    // distribuidas entre ate oito workers single-thread — sem OpenMP aninhado. Medido no Bistro:
    // a faixa grande e serial e os workers esperam por ela (~17 s de 22 s); nao esta otimizado.
    struct FOmmBakeRunStats {
        u32    InternalJobs = 0;
        u32    ParallelJobs = 0;
        u32    Workers      = 0;
        double Ms           = 0.0;
    };
    FOmmBakeRunStats BakeOmmJobs(const std::vector<FOmmBakeJob>& Jobs,
                                 const std::vector<size_t>& Which, u64 MaxWorkloadSize,
                                 std::vector<std::shared_ptr<FOpacityMicromapData>>& Results);

    class FOmmBakeCache {
    public:
        // <cena>.somm, ao lado do .sscene/.smesh — mesmo BasePath.
        static std::filesystem::path PathFor(const std::filesystem::path& SceneBasePath);

        // Le o sidecar INTEIRO e o valida: magic, versao do formato, versao dos parametros do bake
        // (kOmmBakeSettingsVersion), versao do SDK e o hash do payload. Qualquer uma divergindo
        // recusa o arquivo todo — e o motivo vai em WhyNot. Falso tambem quando o arquivo nao
        // existe; o chamador distingue os dois casos com std::filesystem::exists, para o log.
        bool Load(const std::filesystem::path& Path, std::string& WhyNot);

        // Escreve num .tmp e renomeia por cima: um bake interrompido nao deixa sidecar truncado.
        bool Save(const std::filesystem::path& Path, std::string& WhyNot) const;

        // Um par pode ter VARIAS entradas, uma por chave: o valor cozido (que o import procura) e
        // os valores de um override do <cena>.materials.json (que o rebake procura, porque o editor
        // aplica o override depois do load). Mesma chave substitui.
        void Put(const FOmmBakeKey& Key, std::shared_ptr<FOpacityMicromapData> Data);

        enum class ELookup { Hit, Missing, Stale };
        // Hit so com a chave INTEIRA batendo. Stale = existe entrada para o par, mas nenhuma com as
        // entradas de bake de agora.
        ELookup Lookup(const FOmmBakeKey& Key, std::shared_ptr<FOpacityMicromapData>& Out) const;

        size_t Size() const;
        void   Clear() { Entries_.clear(); Flavor_ = 0; }

        // A CONFIGURACAO de build que gravou o sidecar. Informativa, NAO entra na validacao.
        //
        // Existe porque o bake nao e reprodutivel bit a bit entre Debug e Release: o omm-lib
        // compila com /fp:fast em todas as configuracoes, e /Od contra /O2 reassocia e contrai o
        // ponto flutuante de jeitos diferentes — microtriangulo na borda do contorno cai de lado
        // diferente (medido no Bistro: mesh 1494). Os dois sao micro-maps validos; o que nao vale
        // e exigir identidade entre eles. Quem compara bakes (o teste) consulta isto.
        static constexpr u32 kFlavorDebug     = 1u;
        static constexpr u32 kFlavorOptimized = 2u;
        static u32 CurrentFlavor();
        u32 Flavor() const { return Flavor_; } // 0 antes de um Load bem-sucedido

        static constexpr u32 kMagic         = 0x4D4D4F53u; // "SOMM"
        // v2: VARIAS entradas por par (cozido + overrides do .materials.json). O layout em disco e o
        // mesmo da v1, entao a v1 continua sendo lida; o bump existe para um leitor ANTIGO recusar
        // um arquivo v2 com o motivo certo ("formato v2"), e nao com "par duplicado" — que foi o
        // que a v1 disse ao ler o primeiro arquivo com variantes.
        static constexpr u32 kFormatVersion       = 2u;
        static constexpr u32 kOldestFormatVersion = 1u;

    private:
        u32 Flavor_ = 0;
        struct FEntry {
            FOmmBakeKey                           Key;
            std::shared_ptr<FOpacityMicromapData> Data;
        };
        static u64 PairOf(u32 Mesh, u32 Material) {
            return (static_cast<u64>(Mesh) << 32) | Material;
        }
        std::unordered_map<u64, std::vector<FEntry>> Entries_; // por par (malha, material)
    };

    // Versao do OMM SDK compilado, no cabecalho do sidecar. Outro SDK pode classificar diferente.
    u32 OmmSdkVersion();
}
