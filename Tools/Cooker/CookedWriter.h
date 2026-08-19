#pragma once

// ================================================================================================
// Peca comum aos importadores do Cooker (FBX via ufbx, glTF/GLB via GltfImport).
//
// Existe porque os dois produzem exatamente o MESMO artefato — .smesh + .sscene na kCookedVersion
// — e a parte cara de acertar nao e ler o formato de entrada, e sim o que vem depois: o weld de
// vertices, a ordem [VB][IB][RTTri] dentro do blob, gerar o payload de RT SO depois do winding
// final, e a decomposicao do transform de mundo no TRS que o FTransform da engine consome.
// Duplicar isso por importador significaria que um .glb e um .fbx da mesma malha divergiriam em
// silencio no dia em que um dos dois lados fosse corrigido.
// ================================================================================================

#include "Smile/Scene/CookedFormat.h"
#include "Smile/Graphics/Mesh.h" // Smile::Vertex (stride 32)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace SmileCook {

    namespace fs = std::filesystem;
    using Smile::Vertex;

    inline void SetStr(char* _Dst, size_t _Cap, const std::string& _Value) {
        std::memset(_Dst, 0, _Cap);
        std::strncpy(_Dst, _Value.c_str(), _Cap - 1);
    }

    // --- Weld de vertices: chave = 8 floats (pos3, normal3, uv2), igualdade por bytes.
    struct FVertKey {
        Vertex V;
        bool operator==(const FVertKey& _Other) const {
            return std::memcmp(&V, &_Other.V, sizeof(Vertex)) == 0;
        }
    };
    struct FVertKeyHash {
        size_t operator()(const FVertKey& _Key) const {
            // FNV-1a sobre os bytes do vertice.
            const auto* Bytes = reinterpret_cast<const unsigned char*>(&_Key.V);
            size_t Hash = 1469598103934665603ull;
            for (size_t I = 0; I < sizeof(Vertex); ++I) { Hash ^= Bytes[I]; Hash *= 1099511628211ull; }
            return Hash;
        }
    };

    // Sub-mesh acumulado durante o cook (uma parte de material de uma malha).
    struct FSubMesh {
        std::vector<Vertex>   Vertices;
        std::vector<uint32_t> Indices;
        std::unordered_map<FVertKey, uint32_t, FVertKeyHash> Weld;
        float Min[3] = {  1e30f,  1e30f,  1e30f };
        float Max[3] = { -1e30f, -1e30f, -1e30f };

        uint32_t Add(const Vertex& _V) {
            FVertKey Key{ _V };
            auto It = Weld.find(Key);
            if (It != Weld.end()) return It->second;
            const uint32_t Index = static_cast<uint32_t>(Vertices.size());
            Vertices.push_back(_V);
            Weld.emplace(Key, Index);
            for (int C = 0; C < 3; ++C) {
                Min[C] = std::min(Min[C], _V.Position[C]);
                Max[C] = std::max(Max[C], _V.Position[C]);
            }
            return Index;
        }

        bool Empty() const { return Vertices.empty() || Indices.size() < 3; }
    };

    // --- Transform de mundo (column-vector, RH Y-up) -> TRS do FTransform da engine
    // (S * Rx*Ry*Rz * T, row-vector, LH Y-up, radianos).
    //
    // Cols[0..2] sao as colunas de base da 3x3; Cols[3] e a translacao. E o layout do ufbx_matrix
    // e tambem o dos 12 primeiros valores de uma matriz glTF (column-major), entao os dois
    // importadores preenchem isto sem converter nada antes.
    //
    // O flip RH->LH das posicoes (nega Z) conjuga a matriz: p_engine = F*G*F * p_local_engine com
    // F = diag(1,1,-1). Em row-vector isso vira M[i][j] = sign(i)*sign(j) * Cols[i][j].
    //
    // A extracao de Euler sai da propria convencao do Mat44 (R = Rx*Ry*Rz):
    //   R02 = sy | R12 = sx*cy | R22 = cx*cy | R00 = cy*cz | R01 = cy*sz
    // Gimbal lock (cy ~ 0) e tratado; ali o par (x,z) e ambiguo mas a MATRIZ resultante continua
    // correta, que e o que importa para renderizar.
    struct FColumnMatrix {
        double Cols[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 0 } };
    };

    struct FNodeTRS {
        float Pos[3]   = { 0, 0, 0 };
        float Rot[3]   = { 0, 0, 0 };
        float Scale[3] = { 1, 1, 1 };
    };

    inline FNodeTRS DecomposeToEngineTRS(const FColumnMatrix& _G) {
        double M[3][3];
        for (int I = 0; I < 3; ++I) {
            const double Si = (I == 2) ? -1.0 : 1.0;
            for (int J = 0; J < 3; ++J) {
                const double Sj = (J == 2) ? -1.0 : 1.0;
                M[I][J] = Si * Sj * _G.Cols[I][J];
            }
        }

        double Scale[3], R[3][3];
        for (int I = 0; I < 3; ++I) {
            Scale[I] = std::sqrt(M[I][0]*M[I][0] + M[I][1]*M[I][1] + M[I][2]*M[I][2]);
            const double Inv = Scale[I] > 1e-12 ? 1.0 / Scale[I] : 0.0;
            for (int J = 0; J < 3; ++J) R[I][J] = M[I][J] * Inv;
        }

        const double Clamped = R[0][2] < -1.0 ? -1.0 : (R[0][2] > 1.0 ? 1.0 : R[0][2]);
        const double Ry = std::asin(Clamped);
        double Rx, Rz;
        if (std::fabs(std::cos(Ry)) > 1e-6) {
            Rx = std::atan2(R[1][2], R[2][2]);
            Rz = std::atan2(R[0][1], R[0][0]);
        } else {
            Rx = std::atan2(-R[2][1], R[1][1]);
            Rz = 0.0;
        }

        FNodeTRS T{};
        T.Pos[0] = static_cast<float>(_G.Cols[3][0]);
        T.Pos[1] = static_cast<float>(_G.Cols[3][1]);
        T.Pos[2] = static_cast<float>(-_G.Cols[3][2]); // mesmo flip de Z das posicoes
        T.Rot[0] = static_cast<float>(Rx);
        T.Rot[1] = static_cast<float>(Ry);
        T.Rot[2] = static_cast<float>(Rz);
        T.Scale[0] = static_cast<float>(Scale[0]);
        T.Scale[1] = static_cast<float>(Scale[1]);
        T.Scale[2] = static_cast<float>(Scale[2]);
        return T;
    }

    // --- Cozido em construcao: as quatro regioes que vao para disco, mais os contadores que os
    // importadores imprimem no fim.
    struct FCookedScene {
        std::vector<Smile::SMeshEntry>       Entries;
        std::vector<Smile::SSceneMaterial>   Materials;
        std::vector<Smile::SSceneRenderable> Renderables;
        std::vector<uint8_t>                 Geo; // [VB][IB][RTTri] por mesh, na ordem de Entries

        size_t TotalTriangles   = 0; // somando instancias
        size_t TotalRTTriangles = 0; // triangulos UNICOS (o payload mora por malha)
        size_t DedupHits        = 0;

        // Anexa a geometria de uma parte e devolve o indice do mesh. O payload de RT sai daqui e
        // nao do importador de proposito: neste ponto `_Sm.Indices` ja passou pelo weld e pelo
        // winding final, entao o triangulo i deste vetor e exatamente o PrimitiveIndex i que o
        // BLAS vera. Gerar antes do winding inverteria o cross e a normal de face sairia trocada —
        // em silencio, porque so o facing mudaria.
        uint32_t AppendMesh(const FSubMesh& _Sm) {
            Smile::SMeshEntry Entry{};
            Entry.VertexCount = static_cast<uint32_t>(_Sm.Vertices.size());
            Entry.IndexCount  = static_cast<uint32_t>(_Sm.Indices.size());
            for (int C = 0; C < 3; ++C) {
                Entry.AABBMin[C] = _Sm.Min[C];
                Entry.AABBMax[C] = _Sm.Max[C];
            }

            Entry.VertexOffset = Geo.size();
            AppendBytes(_Sm.Vertices.data(), _Sm.Vertices.size() * sizeof(Vertex));
            Entry.IndexOffset = Geo.size();
            AppendBytes(_Sm.Indices.data(), _Sm.Indices.size() * sizeof(uint32_t));

            std::vector<Smile::FRTTriangle> RTTris;
            Smile::BuildRTTriangles(_Sm.Vertices.data(), static_cast<uint32_t>(_Sm.Vertices.size()),
                                    _Sm.Indices.data(), static_cast<uint32_t>(_Sm.Indices.size()),
                                    RTTris);
            Entry.RTTriangleOffset = Geo.size();
            Entry.RTTriangleCount  = static_cast<uint32_t>(RTTris.size());
            AppendBytes(RTTris.data(), RTTris.size() * sizeof(Smile::FRTTriangle));
            TotalRTTriangles += RTTris.size();

            const uint32_t Index = static_cast<uint32_t>(Entries.size());
            Entries.push_back(Entry);
            return Index;
        }

        bool Write(const fs::path& _OutBase) const {
            {
                fs::path Path = _OutBase; Path += ".smesh";
                FILE* File = std::fopen(Path.string().c_str(), "wb");
                if (!File) { std::printf("[Cooker] ERRO ao abrir %s\n", Path.string().c_str()); return false; }
                const Smile::SMeshHeader Header{ Smile::kSMeshMagic, Smile::kCookedVersion,
                                                 static_cast<uint32_t>(Entries.size()), 0 };
                std::fwrite(&Header, sizeof(Header), 1, File);
                // Vetor vazio da data() == nullptr, e fwrite(nullptr, ...) e UB mesmo com
                // contagem zero. Cena sem material e legitima (malha gerada sem PBR nenhum).
                if (!Entries.empty()) std::fwrite(Entries.data(), sizeof(Smile::SMeshEntry), Entries.size(), File);
                if (!Geo.empty())     std::fwrite(Geo.data(), 1, Geo.size(), File);
                std::fclose(File);
                std::printf("[Cooker] Escrito: %s\n", Path.string().c_str());
            }
            {
                fs::path Path = _OutBase; Path += ".sscene";
                FILE* File = std::fopen(Path.string().c_str(), "wb");
                if (!File) { std::printf("[Cooker] ERRO ao abrir %s\n", Path.string().c_str()); return false; }
                const Smile::SSceneHeader Header{ Smile::kSSceneMagic, Smile::kCookedVersion,
                                                  static_cast<uint32_t>(Materials.size()),
                                                  static_cast<uint32_t>(Renderables.size()) };
                std::fwrite(&Header, sizeof(Header), 1, File);
                if (!Materials.empty())
                    std::fwrite(Materials.data(), sizeof(Smile::SSceneMaterial), Materials.size(), File);
                if (!Renderables.empty())
                    std::fwrite(Renderables.data(), sizeof(Smile::SSceneRenderable), Renderables.size(), File);
                std::fclose(File);
                std::printf("[Cooker] Escrito: %s\n", Path.string().c_str());
            }
            return true;
        }

        void PrintSummary() const {
            std::printf("[Cooker] Meshes: %zu | Renderaveis: %zu | Materiais: %zu | Triangulos: %zu | Geo: %.1f MB\n",
                        Entries.size(), Renderables.size(), Materials.size(), TotalTriangles,
                        Geo.size() / (1024.0*1024.0));
            std::printf("[Cooker] Instancing: %zu de %zu renderaveis reusam geometria ja cozida (%.1f%%)\n",
                        DedupHits, Renderables.size(),
                        Renderables.empty() ? 0.0 : 100.0 * (double)DedupHits / (double)Renderables.size());
            std::printf("[Cooker] Payload RT: %zu triangulos unicos x %zu B = %.1f MB (%.1f%% do blob)\n",
                        TotalRTTriangles, sizeof(Smile::FRTTriangle),
                        TotalRTTriangles * sizeof(Smile::FRTTriangle) / (1024.0*1024.0),
                        Geo.empty() ? 0.0 : 100.0 * (double)(TotalRTTriangles * sizeof(Smile::FRTTriangle))
                                          / (double)Geo.size());
        }

    private:
        void AppendBytes(const void* _Data, size_t _Size) {
            if (_Size == 0) return;
            const auto* Bytes = static_cast<const uint8_t*>(_Data);
            Geo.insert(Geo.end(), Bytes, Bytes + _Size);
        }
    };

    // --- Normalizacao de escala/pivot (prototipagem).
    //
    // Malha vinda de gerador 3D por IA nao tem unidade: um servico devolve o modelo dentro de um
    // cubo unitario, outro em centimetros, outro com o pivot no centro do volume. Carregar isso
    // numa cena em METROS da um objeto microscopico ou enterrado no chao, e corrigir na mao a
    // cada iteracao mata justamente o ciclo rapido que o gerador existe para dar.
    //
    // A correcao e uma SIMILARIDADE aplicada em mundo (escala uniforme + translacao), nunca um
    // rebake dos vertices: com escala uniforme s e deslocamento t,
    //   S_local * R * T_pos * S(s) * T(t)  ==  S(Scale*s) * R * T(Pos*s + t)
    // porque S(s) uniforme comuta com R. Ou seja, cada renderavel so ajusta Position e Scale — a
    // geometria (e portanto o payload de RT e a dedup por malha) fica intacta.
    struct FNormalizeOptions {
        float FitSize     = 0.0f;  // >0: maior dimensao da cena vira este tamanho, em metros
        bool  DropToGround = false; // encosta o minimo Y em 0
        bool  CenterXZ     = false; // centraliza o volume em X/Z na origem
        bool  Any() const { return FitSize > 0.0f || DropToGround || CenterXZ; }
    };

    // Matriz de modelo do TRS, na convencao do FTransform (S * Rx*Ry*Rz * T, row-vector).
    inline void BuildModelMatrix(const float _Pos[3], const float _Rot[3], const float _Scale[3],
                                 double _Out[4][4]) {
        const double Cx = std::cos(_Rot[0]), Sx = std::sin(_Rot[0]);
        const double Cy = std::cos(_Rot[1]), Sy = std::sin(_Rot[1]);
        const double Cz = std::cos(_Rot[2]), Sz = std::sin(_Rot[2]);

        // R = Rx*Ry*Rz, expandido (mesma convencao dos Mat44::Rotation*).
        const double R[3][3] = {
            {  Cy*Cz,              Cy*Sz,             Sy    },
            { -Cx*Sz + Sx*Sy*Cz,   Cx*Cz + Sx*Sy*Sz, -Sx*Cy },
            {  Sx*Sz + Cx*Sy*Cz,  -Sx*Cz + Cx*Sy*Sz,  Cx*Cy },
        };
        for (int I = 0; I < 3; ++I)
            for (int J = 0; J < 3; ++J)
                _Out[I][J] = static_cast<double>(_Scale[I]) * R[I][J];
        _Out[0][3] = _Out[1][3] = _Out[2][3] = 0.0;
        _Out[3][0] = _Pos[0];
        _Out[3][1] = _Pos[1];
        _Out[3][2] = _Pos[2];
        _Out[3][3] = 1.0;
    }

    // AABB de MUNDO da cena inteira. Pelos 8 cantos de cada caixa local, e nao pelos dois
    // extremos: sob rotacao a caixa alinhada do resultado nao e a imagem de min/max — mesmo
    // motivo do FRenderable::RefreshWorldBounds.
    inline bool ComputeSceneBounds(const FCookedScene& _Scene, float _OutMin[3], float _OutMax[3]) {
        double Min[3] = {  1e30,  1e30,  1e30 };
        double Max[3] = { -1e30, -1e30, -1e30 };
        bool Any = false;

        for (const Smile::SSceneRenderable& R : _Scene.Renderables) {
            if (R.MeshIndex >= _Scene.Entries.size()) continue;
            const Smile::SMeshEntry& E = _Scene.Entries[R.MeshIndex];
            if (E.VertexCount == 0) continue;
            double M[4][4];
            BuildModelMatrix(R.Position, R.RotationEuler, R.Scale, M);
            for (int Corner = 0; Corner < 8; ++Corner) {
                const double P[3] = {
                    (Corner & 1) ? E.AABBMax[0] : E.AABBMin[0],
                    (Corner & 2) ? E.AABBMax[1] : E.AABBMin[1],
                    (Corner & 4) ? E.AABBMax[2] : E.AABBMin[2],
                };
                for (int C = 0; C < 3; ++C) {
                    const double W = P[0]*M[0][C] + P[1]*M[1][C] + P[2]*M[2][C] + M[3][C];
                    Min[C] = std::min(Min[C], W);
                    Max[C] = std::max(Max[C], W);
                }
                Any = true;
            }
        }
        if (!Any) return false;
        for (int C = 0; C < 3; ++C) {
            _OutMin[C] = static_cast<float>(Min[C]);
            _OutMax[C] = static_cast<float>(Max[C]);
        }
        return true;
    }

    // Devolve false quando nao havia o que normalizar (cena vazia ou nenhuma opcao ligada).
    inline bool NormalizeCookedScene(FCookedScene& _Scene, const FNormalizeOptions& _Options) {
        if (!_Options.Any()) return false;
        float Min[3], Max[3];
        if (!ComputeSceneBounds(_Scene, Min, Max)) return false;

        const double Size[3] = { double(Max[0]) - Min[0], double(Max[1]) - Min[1],
                                 double(Max[2]) - Min[2] };
        const double Largest = std::max(Size[0], std::max(Size[1], Size[2]));

        // Cena degenerada (plano de espessura zero em todos os eixos, ou um unico ponto) nao tem
        // fator de escala definido — escalar por 1/0 mandaria a malha para o infinito.
        double Scale = 1.0;
        if (_Options.FitSize > 0.0f && Largest > 1e-9)
            Scale = double(_Options.FitSize) / Largest;

        double Offset[3] = { 0.0, 0.0, 0.0 };
        if (_Options.CenterXZ) {
            Offset[0] = -0.5 * (double(Min[0]) + Max[0]) * Scale;
            Offset[2] = -0.5 * (double(Min[2]) + Max[2]) * Scale;
        }
        if (_Options.DropToGround) Offset[1] = -double(Min[1]) * Scale;

        for (Smile::SSceneRenderable& R : _Scene.Renderables) {
            for (int C = 0; C < 3; ++C) {
                R.Position[C] = static_cast<float>(double(R.Position[C]) * Scale + Offset[C]);
                R.Scale[C]    = static_cast<float>(double(R.Scale[C]) * Scale);
            }
        }
        std::printf("[Cooker] Normalizacao: escala x%.4f, offset (%.3f, %.3f, %.3f) — "
                    "caixa original %.3f x %.3f x %.3f m\n",
                    Scale, Offset[0], Offset[1], Offset[2], Size[0], Size[1], Size[2]);
        return true;
    }
}
