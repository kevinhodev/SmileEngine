#pragma once

// De onde saem as entradas de bake de micro-map quando e preciso REBAKEAR em runtime.
//
// Por que existe: o gate da Fase 2 exige que, mudada qualquer entrada do bake, o
// FOpacityMicromapData seja substituido e os BLAS afetados reconstruidos. O runtime nao guarda
// nada do que o bake consome — o FGpuMesh so tem os buffers de GPU e o FTextureCPUData e local do
// import —, e sem uma resposta para "de onde tiro isso de novo" a unica saida era RECUSAR as
// edicoes que invalidavam o micro-map.
//
// O que ele guarda e PROCEDENCIA, nao dado:
//
//   MALHA — o arquivo .smesh e o indice da entrada. A geometria de bake (UVs + indices) e lida
//   sob demanda. A primeira versao disto retinha os proprios UVs e indices, e isso tinha dois
//   defeitos: pagava memoria residente por uma operacao manual e rara, e so podia reter as malhas
//   que ja eram MASCARADAS no import — entao ligar Alpha Test num material opaco depois nao tinha
//   com que bakear. Guardando procedencia, cabe reter TODAS as malhas por ~40 B cada, e a
//   transicao opaco -> masked passa a funcionar.
//
//   TEXTURA — o caminho do arquivo, com as flags de decodificacao do load original. Mesma razao:
//   guardar o BC7 de folhagem residente custaria dezenas de MB por um caminho que roda quando
//   alguem mexe num slider.
//
// Nao e cache de conveniencia: sem procedencia, o rebake FALHA e diz qual peca faltou. Um rebake
// silenciosamente pulado devolveria exatamente o micro-map obsoleto que a fase existe para impedir.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/Texture.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    class FGpuMesh;
    class FTexture;

    // IDENTIDADE do arquivo de onde uma entrada de bake veio: tamanho e hora de escrita.
    //
    // Caminho sozinho NAO e procedencia. Um recook mantem magic e versao, e pode trocar as UVs,
    // os indices ou a ORDEM das malhas — e o rebake aceitaria em silencio, produzindo um
    // micro-map que descreve geometria diferente da que esta nos buffers de GPU. A textura tem o
    // mesmo TOCTOU: reler o caminho nao garante reler os bytes que foram enviados a GPU.
    //
    // O que ele prova, exatamente: a geometria residente foi carregada DAQUELE arquivo e nao muda
    // depois; logo, arquivo inalterado implica geometria inalterada. Nao prova mais que isso — uma
    // edicao in-place que preserve tamanho E hora passaria —, e essa nao e a ameaca: o caso real e
    // recozinhar com o editor aberto, que reescreve o arquivo.
    //
    // Fingerprint de ARQUIVO e nao de conteudo por causa do custo: hashear a geometria e as
    // texturas de BaseColor da cena somaria centenas de MB de leitura ao load, todo load, para
    // proteger uma operacao manual e rara.
    struct FOmmFileStamp {
        u64 SizeBytes = 0;
        i64 WriteTime = 0; // rep() do file_time_type; so a igualdade importa
        bool Valid() const { return SizeBytes != 0; }
        friend bool operator==(const FOmmFileStamp& A, const FOmmFileStamp& B) {
            return A.SizeBytes == B.SizeBytes && A.WriteTime == B.WriteTime;
        }
        friend bool operator!=(const FOmmFileStamp& A, const FOmmFileStamp& B) { return !(A == B); }
        // Zerado quando o arquivo nao existe ou nao pode ser consultado.
        static FOmmFileStamp Of(const std::wstring& Path);
    };

    class FOmmBakeInputStore {
    public:
        // De qual arquivo cozido, e qual entrada dentro dele, a geometria desta malha veio. O
        // carimbo do arquivo e tirado AQUI, no import, que e quando ele descreve os bytes que
        // foram para a GPU. Idempotente por malha.
        void RetainMeshSource(const FGpuMesh* Mesh, const std::wstring& MeshFilePath, u32 MeshIndex,
                              const FOmmFileStamp& Stamp);

        // De onde a textura veio, ja resolvido em caminho absoluto, com as flags de decodificacao
        // que o load original usou. Sem elas o rebake decodificaria o mesmo arquivo de outro jeito.
        void RetainTexturePath(const FTexture* Texture, std::wstring Path, bool IsNormalMap,
                               bool sRGB);

        void Clear();

        // Geometria de bake carregada sob demanda. Os vetores sao do CHAMADOR: a view aponta para
        // dentro deles, e nada disso fica residente depois.
        struct FLoadedGeometry {
            std::vector<f32> TexCoords; // compactadas, 2 floats por vertice
            std::vector<u32> Indices;

            FOmmGeometry View() const {
                FOmmGeometry G;
                if (TexCoords.empty() || Indices.empty()) return G;
                G.TexCoords      = TexCoords.data();
                G.TexCoordStride = sizeof(f32) * 2;
                G.VertexCount    = static_cast<u32>(TexCoords.size() / 2);
                G.Indices        = Indices.data();
                G.IndexCount     = static_cast<u32>(Indices.size());
                return G;
            }
        };

        // Le do .smesh as UVs e os indices desta malha. Falso quando a malha nao tem procedencia
        // (criada em runtime) ou o arquivo nao bate com o que foi registrado — e nesse caso quem
        // chamou tem de recusar o rebake, nunca seguir com o micro-map antigo.
        bool LoadGeometry(const FGpuMesh* Mesh, FLoadedGeometry& Out) const;

        // A textura de alpha relida do disco, com o carimbo conferido. Falso quando a textura nao
        // tem procedencia (gerada em runtime), quando o arquivo mudou desde o load, ou quando a
        // releitura falha. O chamador RECUSA o rebake nesses casos — nunca segue com o antigo.
        //
        // Mora aqui, e nao no chamador, para a conferencia do carimbo nao poder ser esquecida por
        // quem so queria os pixels.
        bool LoadAlphaTexture(const FTexture* Texture, FTextureCPUData& Out) const;

        // Se a textura tem procedencia registrada. Distingue "gerada em runtime" de "arquivo
        // mudou", que dao mensagens diferentes.
        bool HasTextureSource(const FTexture* Texture) const;

        u32 MeshCount()    const { return static_cast<u32>(Meshes_.size()); }
        u32 TextureCount() const { return static_cast<u32>(Textures_.size()); }
        // Memoria que a cena segura por causa disto. CACHEADA, e nao recontada: isto e lido em
        // TODO frame pela telemetria, e uma travessia dos dois mapas por frame seria custo de
        // regime permanente para exibir um contador — exatamente o tipo de coisa que a Fase 2
        // existe para tirar do caminho quente.
        u64 RetainedBytes() const { return RetainedBytes_; }

    private:
        struct FMeshSource {
            std::wstring  FilePath;
            u32           MeshIndex = 0;
            FOmmFileStamp Stamp;
        };
        struct FTextureSource {
            std::wstring  Path;
            bool          IsNormalMap = false;
            bool          sRGB        = false;
            FOmmFileStamp Stamp;
        };

        std::unordered_map<const FGpuMesh*, FMeshSource>    Meshes_;
        std::unordered_map<const FTexture*, FTextureSource> Textures_;
        u64 RetainedBytes_ = 0;
    };
}
