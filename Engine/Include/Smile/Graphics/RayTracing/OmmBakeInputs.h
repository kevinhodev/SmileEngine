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
//   MALHA — o arquivo .smesh, o indice da entrada e o hash das UVs+indices. A geometria e lida
//   sob demanda. A primeira versao disto retinha os proprios UVs e indices, e isso tinha dois
//   defeitos: pagava memoria residente por uma operacao manual e rara, e so podia reter as malhas
//   que ja eram MASCARADAS no import — entao ligar Alpha Test num material opaco depois nao tinha
//   com que bakear. Guardando procedencia, cabe reter TODAS as malhas por ~40 B cada, e a
//   transicao opaco -> masked passa a funcionar.
//
//   TEXTURA — o caminho, as flags de decodificacao do load original e o hash dos pixels. Mesma razao:
//   guardar o BC7 de folhagem residente custaria dezenas de MB por um caminho que roda quando
//   alguem mexe num slider.
//
// Nao e cache de conveniencia: sem procedencia, o rebake FALHA e diz qual peca faltou. Um rebake
// silenciosamente pulado devolveria exatamente o micro-map obsoleto que a fase existe para impedir.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/Texture.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    class FGpuMesh;
    class FTexture;

    // IDENTIDADE das entradas de bake: um hash proprio do CONTEUDO que o loader carregou.
    //
    // Caminho sozinho nao e procedencia — um recook mantem magic e versao e pode trocar UVs,
    // indices ou a ORDEM das malhas. Carimbo de arquivo {tamanho, hora} tambem nao serve, e a
    // razao e uma janela concreta: o loader le os bytes, o import bakeia, e so DEPOIS o carimbo
    // seria tirado. Um recook nesse intervalo deixa os buffers antigos na GPU e registra o
    // carimbo do arquivo NOVO — a validacao passaria justamente no caso que ela existe para pegar.
    //
    // O hash sai dos bytes que o loader ja tem em RAM, entao nao acrescenta leitura nenhuma ao
    // load: e uma passada sobre dado que ja esta residente. Na releitura do rebake o mesmo hash e
    // recalculado e comparado.
    //
    // Zero significa "sem fingerprint" e nunca casa com nada: procedencia indisponivel falha
    // FECHADA. Registrar sem fingerprint e recusado na entrada, e nao ha caminho em que duas
    // consultas com erro passem a "bater" — que era o defeito do carimbo de arquivo.
    using FOmmContentHash = u64;
    inline constexpr FOmmContentHash kOmmNoContentHash = 0;

    // MISTURA ESTILO FNV-1a, por PALAVRA — e NAO o FNV-1a canonico, que multiplica por BYTE.
    //
    // A diferenca e deliberada e vale nomear: isto passa por megabytes de pixel a cada load de
    // cena, e multiplicar por byte custaria oito vezes mais. Emprestados do FNV-1a estao o primo e
    // o offset basis, nada mais. Chamar o resultado de "FNV-1a" seria errado, e nome errado e o
    // que faz alguem depois assumir propriedades que a funcao nao tem.
    //
    // Nao e criptografico e nao precisa ser: o que ele detecta e recook e troca de asset, nao
    // adversario.
    inline constexpr FOmmContentHash kOmmHashSeed  = 14695981039346656037ull;
    inline constexpr FOmmContentHash kOmmHashPrime = 1099511628211ull;

    inline FOmmContentHash OmmHashAppend(FOmmContentHash H, const void* Data, size_t Bytes) {
        const auto* B = static_cast<const u8*>(Data);
        size_t I = 0;
        for (; I + 8 <= Bytes; I += 8) {
            u64 Word;
            std::memcpy(&Word, B + I, sizeof(Word));
            H ^= Word;
            H *= kOmmHashPrime;
        }
        for (; I < Bytes; ++I) {
            H ^= B[I];
            H *= kOmmHashPrime;
        }
        return H;
    }

    // Finalizador fmix64 do MurmurHash3. Multiplicar por palavra deixa os bits altos pouco
    // misturados quando a entrada e curta — e a comparacao usa o valor inteiro, nao um recorte.
    inline FOmmContentHash OmmHashFinish(FOmmContentHash H) {
        H ^= H >> 33; H *= 0xff51afd7ed558ccdull;
        H ^= H >> 33; H *= 0xc4ceb9fe1a85ec53ull;
        H ^= H >> 33;
        // Um hash que caia em zero por acidente viraria "sem fingerprint"; deslocar preserva a
        // distincao sem enfraquecer nada.
        return H == kOmmNoContentHash ? 1ull : H;
    }

    // Fingerprint da GEOMETRIA de bake. Percorre as UVs POR ELEMENTO e nao por bytes crus, porque
    // os dois lados usam strides diferentes — no import as UVs vivem dentro do Vertex (32 B) e na
    // releitura vem compactadas (8 B). Hashear bytes crus daria valores diferentes para a mesma
    // geometria, e a validacao recusaria tudo.
    //
    // Os bits do float, e nao o valor: comparacao de igualdade de ponto flutuante nao e o que se
    // quer aqui, e NaN nao seria igual a si mesmo.
    inline FOmmContentHash OmmHashGeometry(const f32* TexCoords, u32 StrideBytes, u32 VertexCount,
                                           const u32* Indices, u32 IndexCount) {
        if (!TexCoords || !Indices || VertexCount == 0 || IndexCount == 0)
            return kOmmNoContentHash;
        FOmmContentHash H = kOmmHashSeed;
        H = OmmHashAppend(H, &VertexCount, sizeof(VertexCount));
        H = OmmHashAppend(H, &IndexCount, sizeof(IndexCount));
        const auto* Base = reinterpret_cast<const u8*>(TexCoords);
        for (u32 V = 0; V < VertexCount; ++V) {
            u32 Bits[2];
            std::memcpy(Bits, Base + static_cast<size_t>(V) * StrideBytes, sizeof(Bits));
            H = OmmHashAppend(H, Bits, sizeof(Bits));
        }
        H = OmmHashAppend(H, Indices, static_cast<size_t>(IndexCount) * sizeof(u32));
        return OmmHashFinish(H);
    }

    // Fingerprint da TEXTURA de alpha: tudo o que o baker consome dela, e nada mais — os pixels do
    // mip 0, as dimensoes, o FORMATO (o baker manda os bytes crus para o DirectXTex, entao BC7 e
    // RGBA8 sobre os mesmos bytes sao alphas diferentes) e o IsNormalMap (que zera o canal alpha
    // no decode, o unico canal que o bake le).
    //
    // Nao recebe as flags do load por parametro: elas ja estao NO DADO (o sRGB vira variante
    // _SRGB do formato, o IsNormalMap e um campo). Passa-las de fora abriria a possibilidade de
    // registrar um fingerprint que descreve algo diferente do que foi decodificado.
    FOmmContentHash OmmHashTexture(const FTextureCPUData& Data);

    class FOmmBakeInputStore {
    public:
        // De qual arquivo cozido, e qual entrada dentro dele, a geometria desta malha veio, com o
        // fingerprint do CONTEUDO que o loader carregou. Registro sem fingerprint e recusado:
        // procedencia indisponivel tem de falhar fechada. Idempotente por malha.
        void RetainMeshSource(const FGpuMesh* Mesh, const std::wstring& MeshFilePath, u32 MeshIndex,
                              FOmmContentHash ContentHash);

        // De onde a textura veio, ja resolvido em caminho absoluto, junto com o DADO que o load
        // produziu. As flags de decodificacao e o fingerprint saem dele: passar as flags de fora
        // permitiria registrar (sRGB, normal map) que nao sao os que geraram estes pixels, e o
        // rebake releria o arquivo de um jeito diferente do que subiu para a GPU.
        void RetainTexturePath(const FTexture* Texture, std::wstring Path,
                               const FTextureCPUData& Data);

        // Remove a procedencia de uma textura de edicao cuja publicacao foi recusada. A textura
        // nunca chegou a um material; manter a entrada prenderia tanto o ponteiro morto quanto
        // bytes de telemetria.
        void ForgetTexture(const FTexture* Texture);

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
            FOmmContentHash ContentHash = kOmmNoContentHash;
        };
        struct FTextureSource {
            std::wstring    Path;
            bool            IsNormalMap = false;
            bool            sRGB        = false;
            FOmmContentHash ContentHash = kOmmNoContentHash;
        };

        std::unordered_map<const FGpuMesh*, FMeshSource>    Meshes_;
        std::unordered_map<const FTexture*, FTextureSource> Textures_;
        u64 RetainedBytes_ = 0;
    };
}
