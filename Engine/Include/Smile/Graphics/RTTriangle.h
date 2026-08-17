#pragma once

#include "Smile/Core/Types.h"
#include <cmath>
#include <cstddef>
#include <cstring>

// ================================================================================================
// REGISTRO PRE-COZIDO POR TRIANGULO (payload de ray tracing).
//
// O hit de RT pagava uma cadeia DEPENDENTE de memoria: PrimitiveIndex -> IndexBuffer (3 uints) ->
// 3 vertices de 32 B espalhados no VB. As tres leituras de vertice so podem comecar depois que os
// indices voltam, e caem em enderecos arbitrarios — o pior padrao possivel para um shader de hit,
// que ja e incoerente entre lanes por natureza.
//
// Aqui o mesmo dado sai de UM registro contiguo indexado direto por PrimitiveIndex. Nao vira
// literalmente um fetch (o DXC emite varias loads para 32 B), mas elas saem de um endereco base
// unico, sem dependencia entre si, e caem na mesma regiao — que e o ganho real.
//
// ⚠️ CONTRATO COM O HLSL. O espelho e `RTTriangle` em Shaders/GI/DDGICommon.hlsli, e o layout e
// posicional: oito uint na ordem abaixo. Mexer num lado exige mexer no outro E incrementar o
// kCookedVersion — o payload e persistido no .smesh.
//
//   [0]   FaceNormalOct     octaedrica SNORM16x2 da normal de FACE, em espaco LOCAL
//   [1-3] VertexNormalOct[] octaedrica SNORM16x2 das 3 normais de VERTICE, espaco LOCAL
//   [4-6] UV[]              half2 das 3 UVs, na ordem dos vertices do triangulo
//   [7]   Flags             bit 0 = a normal de face e valida (ver kRTTriFaceNormalValid)
//
// TUDO em espaco LOCAL, como a geometria desde a v7 do cozido: e o que mantem os bytes iguais
// entre instancias e permite a deduplicacao por (malha, parte). Quem leva para mundo continua
// sendo o worldToObject no shader, com a mesma convencao de vetor-linha de antes.
//
// A normal de face NAO substitui a interpolada: ela governa facing, OffsetN e SignedDist, e a
// interpolada continua mandando na BRDF, no N.L e na amostragem do DDGI. Este registro carrega as
// duas justamente para nao trocar uma pela outra.
// ================================================================================================

namespace Smile {

    // Triangulo degenerado (arestas colineares ou de comprimento zero) nao tem normal de face.
    // O bit fica em ZERO e o shader cai na normal interpolada — exatamente o que o
    // HitFaceNormal ja fazia ao retornar false. O criterio de degeneracao e adimensional e mora
    // no cozimento (ver BuildRTTriangles), onde ha precisao dupla e nenhum custo por frame.
    constexpr u32 kRTTriFaceNormalValid = 0x1u;

    struct FRTTriangle {
        u32 FaceNormalOct     = 0;
        u32 VertexNormalOct[3]{ 0, 0, 0 };
        u32 UV[3]{ 0, 0, 0 };
        u32 Flags             = 0;
    };

    static_assert(sizeof(FRTTriangle) == 32,
                  "FRTTriangle deve ter 32 B — o HLSL le 8 uint por triangulo (DDGICommon.hlsli)");
    static_assert(alignof(FRTTriangle) == 4,
                  "FRTTriangle nao pode ganhar padding: o StructuredBuffer usa stride 32 exato");
    static_assert(offsetof(FRTTriangle, FaceNormalOct)  == 0,  "layout posicional [0]");
    static_assert(offsetof(FRTTriangle, VertexNormalOct) == 4,  "layout posicional [1-3]");
    static_assert(offsetof(FRTTriangle, UV)              == 16, "layout posicional [4-6]");
    static_assert(offsetof(FRTTriangle, Flags)           == 28, "layout posicional [7]");

    // === Codec ==================================================================================
    // Determinismo e requisito, nao detalhe: o cozido tem de gerar os MESMOS bits em qualquer
    // maquina, e o HLSL tem de decodificar exatamente o que o C++ escreveu. Por isso nada aqui usa
    // intrinseco de plataforma — as duas pontas implementam a mesma formula em float explicito.

    inline u32 RTPackSnorm16(f32 _V) {
        _V = _V < -1.0f ? -1.0f : (_V > 1.0f ? 1.0f : _V);
        const f32 S = _V * 32767.0f;
        // Arredondamento afastando-se do zero, e nao truncamento: truncar enviesaria toda a
        // quantizacao para dentro do octaedro e o erro deixaria de ser centrado.
        const i32 I = static_cast<i32>(S >= 0.0f ? S + 0.5f : S - 0.5f);
        return static_cast<u32>(static_cast<u16>(static_cast<i16>(I)));
    }

    // Octaedrica: a projecao padrao (Cigolle et al. 2014), com o dobramento do hemisferio
    // inferior. O espelho em HLSL e RT_OctDecode.
    inline u32 RTOctEncodeSnorm16(f32 _Nx, f32 _Ny, f32 _Nz) {
        const f32 L = std::fabs(_Nx) + std::fabs(_Ny) + std::fabs(_Nz);
        if (!(L > 0.0f)) return RTPackSnorm16(0.0f) | (RTPackSnorm16(0.0f) << 16); // +Z
        f32 X = _Nx / L;
        f32 Y = _Ny / L;
        if (_Nz < 0.0f) {
            const f32 Sx = (X >= 0.0f) ? 1.0f : -1.0f;
            const f32 Sy = (Y >= 0.0f) ? 1.0f : -1.0f;
            const f32 Ax = (1.0f - std::fabs(Y)) * Sx;
            const f32 Ay = (1.0f - std::fabs(X)) * Sy;
            X = Ax;
            Y = Ay;
        }
        return RTPackSnorm16(X) | (RTPackSnorm16(Y) << 16);
    }

    // IEEE-754 binary16 com round-to-nearest-even — a mesma regra do f16tof32/f32tof16 do HLSL,
    // entao o `f16tof32` do shader devolve exatamente o valor que foi cozido. Escrito a mao
    // porque `_Float16`/`__fp16` nao existe em todos os compiladores que constroem o Cooker.
    inline u16 RTPackHalf(f32 _V) {
        u32 Bits = 0;
        std::memcpy(&Bits, &_V, sizeof(Bits));
        const u32 Sign    = (Bits >> 16) & 0x8000u;
        const u32 RawExp  = (Bits >> 23) & 0xFFu;
        u32       Mant    = Bits & 0x007FFFFFu;

        if (RawExp == 0xFFu)                              // Inf / NaN
            return static_cast<u16>(Sign | 0x7C00u | (Mant ? 0x0200u : 0u));

        const i32 Exp = static_cast<i32>(RawExp) - 127 + 15;
        if (Exp >= 0x1F) return static_cast<u16>(Sign | 0x7C00u); // estoura -> Inf
        if (Exp <= 0) {                                            // subnormal ou zero
            if (Exp < -10) return static_cast<u16>(Sign);
            Mant |= 0x00800000u;
            const u32 Shift   = static_cast<u32>(14 - Exp);
            const u32 Half    = Mant >> Shift;
            const u32 Rem     = Mant & ((1u << Shift) - 1u);
            const u32 Halfway = 1u << (Shift - 1);
            u32 Out = Half;
            if (Rem > Halfway || (Rem == Halfway && (Half & 1u))) ++Out;
            return static_cast<u16>(Sign | Out);
        }
        // Normal. O ++ pode transbordar a mantissa para o expoente, e isso e CORRETO — o padrao
        // manda o carry subir, e 0x7C00 (Inf) e o resultado certo no topo da faixa.
        u32       Out = (static_cast<u32>(Exp) << 10) | (Mant >> 13);
        const u32 Rem = Mant & 0x1FFFu;
        if (Rem > 0x1000u || (Rem == 0x1000u && (Out & 1u))) ++Out;
        return static_cast<u16>(Sign | Out);
    }

    inline u32 RTPackHalf2(f32 _U, f32 _V) {
        return static_cast<u32>(RTPackHalf(_U)) | (static_cast<u32>(RTPackHalf(_V)) << 16);
    }

    // === Construcao =============================================================================
    // Uma funcao so, usada pelo Cooker (grava no .smesh) E pelo runtime (malhas procedurais, o
    // proxy do terreno, o preview de material). Fosse duplicada, o cozido e o gerado em runtime
    // divergiriam em silencio no dia em que alguem mexesse num dos dois.
    //
    // ⚠️ ORDEM. `_Indices` tem de ser o buffer FINAL — depois do weld e depois do winding —
    // porque a correspondencia com o BLAS e posicional: RTTriangle[i] <-> PrimitiveIndex i. O DXR
    // define PrimitiveIndex como o indice do triangulo na ordem do IB, entao gerar aqui na mesma
    // ordem e o que amarra os dois. Gerar antes do winding inverteria a normal de face.
    //
    // _VertexStrideBytes permite alimentar direto o Vertex (stride 32) da engine sem copia; os
    // offsets de Position/Normal/TexCoord seguem o Smile::Vertex.
    template<typename TVertex, typename TOut>
    void BuildRTTriangles(const TVertex* _Vertices, u32 _VertexCount,
                          const u32* _Indices, u32 _IndexCount, TOut& _Out) {
        const u32 TriCount = _IndexCount / 3u;
        _Out.clear();
        _Out.resize(TriCount);
        for (u32 T = 0; T < TriCount; ++T) {
            const u32 I0 = _Indices[T * 3u + 0u];
            const u32 I1 = _Indices[T * 3u + 1u];
            const u32 I2 = _Indices[T * 3u + 2u];
            FRTTriangle& R = _Out[T];
            if (I0 >= _VertexCount || I1 >= _VertexCount || I2 >= _VertexCount) {
                // Indice fora da faixa e cozido corrompido. Nao ha o que codificar; o registro
                // fica zerado e SEM o bit de face valida, entao o shader cai na interpolada (que
                // tambem sera zero). Melhor que ler fora do vetor no cozimento.
                R = FRTTriangle{};
                continue;
            }
            const TVertex& V0 = _Vertices[I0];
            const TVertex& V1 = _Vertices[I1];
            const TVertex& V2 = _Vertices[I2];

            // Normal de FACE pelo cross das POSICOES, em espaco de objeto — mesma expressao que o
            // HitFaceNormal calculava por hit. Em double porque aqui nao ha custo por frame e a
            // degeneracao de triangulo fino e justamente onde float perde o sinal.
            const double E1[3] = { double(V1.Position[0]) - double(V0.Position[0]),
                                   double(V1.Position[1]) - double(V0.Position[1]),
                                   double(V1.Position[2]) - double(V0.Position[2]) };
            const double E2[3] = { double(V2.Position[0]) - double(V0.Position[0]),
                                   double(V2.Position[1]) - double(V0.Position[1]),
                                   double(V2.Position[2]) - double(V0.Position[2]) };
            const double N[3] = { E1[1] * E2[2] - E1[2] * E2[1],
                                  E1[2] * E2[0] - E1[0] * E2[2],
                                  E1[0] * E2[1] - E1[1] * E2[0] };
            const double NLen2  = N[0] * N[0] + N[1] * N[1] + N[2] * N[2];
            const double Scale2 = (E1[0]*E1[0] + E1[1]*E1[1] + E1[2]*E1[2]) *
                                  (E2[0]*E2[0] + E2[1]*E2[1] + E2[2]*E2[2]);

            // Teste ADIMENSIONAL, identico ao que o HitFaceNormal fazia em espaco de objeto:
            // |cross|^2 / (|e1|^2 |e2|^2) = sin^2 do angulo entre as arestas. Comparar
            // comprimento absoluto faria o limiar depender da unidade do asset.
            //
            // O outro braco daquele teste — `nLen > 0` DEPOIS de multiplicar por worldToObject,
            // que pega transform singular — nao pode ser cozido: ele depende da instancia, nao da
            // malha. Ele CONTINUA no shader (ver RT_LoadFaceNormal).
            if (NLen2 > 1e-12 * Scale2 && NLen2 > 0.0) {
                const double Inv = 1.0 / std::sqrt(NLen2);
                R.FaceNormalOct = RTOctEncodeSnorm16(static_cast<f32>(N[0] * Inv),
                                                     static_cast<f32>(N[1] * Inv),
                                                     static_cast<f32>(N[2] * Inv));
                R.Flags |= kRTTriFaceNormalValid;
            }

            const TVertex* const Vs[3] = { &V0, &V1, &V2 };
            for (int C = 0; C < 3; ++C) {
                R.VertexNormalOct[C] = RTOctEncodeSnorm16(Vs[C]->Normal[0],
                                                          Vs[C]->Normal[1],
                                                          Vs[C]->Normal[2]);
                R.UV[C] = RTPackHalf2(Vs[C]->TexCoord[0], Vs[C]->TexCoord[1]);
            }
        }
    }
}
