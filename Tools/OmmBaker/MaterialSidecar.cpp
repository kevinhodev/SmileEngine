#include "MaterialSidecar.h"

#include "Smile/Core/Logger.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Scene/SceneLoader.h"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace Smile {
    namespace {
        // JSON minimo e COMPLETO (objeto, array, string com escapes, numero, bool, null), por
        // descida recursiva. A engine nao tem parser de JSON — o .terrain.json e lido por busca de
        // string —, e esse truque nao serve aqui: o .materials.json e um array de objetos com
        // objetos aninhados ("textures") e chaves repetidas entre materiais.
        struct FJson {
            enum class EType { Null, Bool, Number, String, Array, Object } Type = EType::Null;
            bool   Bool   = false;
            double Number = 0.0;
            std::string String;
            std::vector<FJson> Array;
            std::map<std::string, FJson> Object;

            const FJson* Find(const char* _Key) const {
                if (Type != EType::Object) return nullptr;
                const auto It = Object.find(_Key);
                return It == Object.end() ? nullptr : &It->second;
            }
        };

        class FJsonParser {
        public:
            explicit FJsonParser(const std::string& _Text) : S(_Text) {}
            FJson Parse() {
                FJson V = Value();
                Ws();
                if (P != S.size()) Error("lixo depois do documento");
                return V;
            }

        private:
            const std::string& S;
            size_t P = 0;

            [[noreturn]] void Error(const char* _What) const {
                throw std::runtime_error(std::string("JSON invalido em ") + std::to_string(P) +
                                         ": " + _What);
            }
            void Ws() { while (P < S.size() && (S[P] == ' ' || S[P] == '\t' || S[P] == '\n' || S[P] == '\r')) ++P; }
            bool Eat(char _C) { Ws(); if (P < S.size() && S[P] == _C) { ++P; return true; } return false; }
            void Expect(char _C) { if (!Eat(_C)) Error("caractere esperado ausente"); }
            bool Literal(const char* _L) {
                const size_t N = std::strlen(_L);
                if (S.compare(P, N, _L) != 0) return false;
                P += N;
                return true;
            }

            FJson Value() {
                Ws();
                if (P >= S.size()) Error("fim inesperado");
                FJson V;
                const char C = S[P];
                if (C == '{') {
                    ++P;
                    V.Type = FJson::EType::Object;
                    if (Eat('}')) return V;
                    do {
                        Ws();
                        if (P >= S.size() || S[P] != '"') Error("chave de objeto nao e string");
                        std::string Key = String();
                        Expect(':');
                        V.Object[std::move(Key)] = Value();
                    } while (Eat(','));
                    Expect('}');
                } else if (C == '[') {
                    ++P;
                    V.Type = FJson::EType::Array;
                    if (Eat(']')) return V;
                    do V.Array.push_back(Value()); while (Eat(','));
                    Expect(']');
                } else if (C == '"') {
                    V.Type = FJson::EType::String;
                    V.String = String();
                } else if (Literal("true"))  { V.Type = FJson::EType::Bool; V.Bool = true; }
                else if (Literal("false"))   { V.Type = FJson::EType::Bool; V.Bool = false; }
                else if (Literal("null"))    { V.Type = FJson::EType::Null; }
                else {
                    V.Type = FJson::EType::Number;
                    const char* Begin = S.c_str() + P;
                    char* End = nullptr;
                    V.Number = std::strtod(Begin, &End);
                    if (End == Begin) Error("valor desconhecido");
                    P += static_cast<size_t>(End - Begin);
                }
                return V;
            }

            static void AppendUtf8(std::string& _Out, u32 _Cp) {
                if (_Cp < 0x80) _Out += static_cast<char>(_Cp);
                else if (_Cp < 0x800) {
                    _Out += static_cast<char>(0xC0 | (_Cp >> 6));
                    _Out += static_cast<char>(0x80 | (_Cp & 0x3F));
                } else if (_Cp < 0x10000) {
                    _Out += static_cast<char>(0xE0 | (_Cp >> 12));
                    _Out += static_cast<char>(0x80 | ((_Cp >> 6) & 0x3F));
                    _Out += static_cast<char>(0x80 | (_Cp & 0x3F));
                } else {
                    _Out += static_cast<char>(0xF0 | (_Cp >> 18));
                    _Out += static_cast<char>(0x80 | ((_Cp >> 12) & 0x3F));
                    _Out += static_cast<char>(0x80 | ((_Cp >> 6) & 0x3F));
                    _Out += static_cast<char>(0x80 | (_Cp & 0x3F));
                }
            }
            u32 Hex4() {
                if (P + 4 > S.size()) Error("\\u truncado");
                u32 V = 0;
                const auto R = std::from_chars(S.data() + P, S.data() + P + 4, V, 16);
                if (R.ptr != S.data() + P + 4) Error("\\u invalido");
                P += 4;
                return V;
            }
            std::string String() {
                ++P; // aspas de abertura
                std::string Out;
                while (true) {
                    if (P >= S.size()) Error("string sem fechamento");
                    const char C = S[P++];
                    if (C == '"') return Out;
                    if (C != '\\') { Out += C; continue; }
                    if (P >= S.size()) Error("escape truncado");
                    switch (S[P++]) {
                        case '"':  Out += '"';  break;
                        case '\\': Out += '\\'; break;
                        case '/':  Out += '/';  break;
                        case 'b':  Out += '\b'; break;
                        case 'f':  Out += '\f'; break;
                        case 'n':  Out += '\n'; break;
                        case 'r':  Out += '\r'; break;
                        case 't':  Out += '\t'; break;
                        case 'u': {
                            u32 Cp = Hex4();
                            // Par substituto UTF-16 -> um codepoint so.
                            if (Cp >= 0xD800 && Cp <= 0xDBFF && S.compare(P, 2, "\\u") == 0) {
                                P += 2;
                                const u32 Lo = Hex4();
                                Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
                            }
                            AppendUtf8(Out, Cp);
                            break;
                        }
                        default: Error("escape desconhecido");
                    }
                }
            }
        };

        // O que o editor le de um objeto de material, limitado as entradas do bake.
        FMaterialOmmOverride ReadOverride(const FJson& _O) {
            FMaterialOmmOverride Ov;
            if (const FJson* V = _O.Find("alphaTest"); V && V->Type == FJson::EType::Bool)
                Ov.AlphaTest = V->Bool;
            if (const FJson* V = _O.Find("alphaCutoff"); V && V->Type == FJson::EType::Number)
                Ov.AlphaCutoff = static_cast<f32>(V->Number);
            if (const FJson* T = _O.Find("textures"); T && T->Type == FJson::EType::Object)
                if (const FJson* Slot0 = T->Find("0"); Slot0 && Slot0->Type == FJson::EType::String)
                    Ov.AlbedoPath = Slot0->String;
            return Ov;
        }
    }

    std::unordered_map<u32, FMaterialOmmOverride> LoadMaterialOmmOverrides(
        const FSceneImportResult& _Scene, std::string& _Why) {
        _Why.clear();
        std::unordered_map<u32, FMaterialOmmOverride> Out;
        // Mesma convencao do MaterialsBridge::OnSceneLoaded: <dir>/<nome da cena>.materials.json.
        std::filesystem::path Path = _Scene.BasePath;
        Path += L".materials.json";
        if (!std::filesystem::exists(Path)) return Out;

        std::ifstream File(Path, std::ios::binary);
        const std::string Text((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        FJson Root;
        try {
            Root = FJsonParser(Text).Parse();
        } catch (const std::exception& E) {
            _Why = E.what();
            return Out;
        }
        if (Root.Type != FJson::EType::Object) { _Why = "raiz nao e objeto"; return Out; }

        // v2 chaveia por id (hex do FMaterial::Id); v1, por nome. Igual ao editor: o id primeiro,
        // o nome so como ponte para um sidecar v1 ainda nao migrado.
        const FJson* Version = Root.Find("version");
        const int Ver = Version && Version->Type == FJson::EType::Number
                            ? static_cast<int>(Version->Number) : 1;
        std::unordered_map<u64, const FJson*> ById;
        std::unordered_map<std::string, const FJson*> ByName;
        if (const FJson* Mats = Root.Find("materials"); Mats && Mats->Type == FJson::EType::Array) {
            for (const FJson& O : Mats->Array) {
                if (O.Type != FJson::EType::Object) continue;
                u64 Id = 0;
                const FJson* IdV = O.Find("id");
                const bool IdOk = IdV && IdV->Type == FJson::EType::String &&
                    std::from_chars(IdV->String.data(), IdV->String.data() + IdV->String.size(),
                                    Id, 16).ec == std::errc{};
                if (Ver >= 2 && IdOk && Id != 0) ById[Id] = &O;
                else if (const FJson* N = O.Find("name"); N && N->Type == FJson::EType::String &&
                                                        !N->String.empty())
                    ByName[N->String] = &O;
            }
        }

        for (u32 I = 0; I < _Scene.Materials.size(); ++I) {
            const SSceneMaterial& M = _Scene.Materials[I];
            const FJson* O = nullptr;
            if (const auto It = ById.find(CookedMaterialId(M)); It != ById.end()) O = It->second;
            else if (const auto Nt = ByName.find(std::string(M.Name, strnlen(M.Name, kCookedMaxName)));
                     Nt != ByName.end())
                O = Nt->second;
            if (!O) continue;
            FMaterialOmmOverride Ov = ReadOverride(*O);
            if (Ov.AlphaTest || Ov.AlphaCutoff || Ov.AlbedoPath) Out.emplace(I, std::move(Ov));
        }
        return Out;
    }

    std::vector<FOmmBakeJob> CollectOmmOverrideJobs(
        const FSceneImportResult& _Scene,
        const std::unordered_map<u32, FMaterialOmmOverride>& _Overrides,
        std::deque<FTextureCPUData>& _OwnedTextures) {
        std::vector<FOmmBakeJob> Jobs;
        if (_Overrides.empty()) return Jobs;

        std::unordered_map<std::string, const FTextureCPUData*> TextureByPath;
        for (size_t I = 0; I < _Scene.TexturePaths.size() && I < _Scene.TextureData.size(); ++I)
            TextureByPath[_Scene.TexturePaths[I]] = &_Scene.TextureData[I];
        // Texturas de override por caminho: carregadas uma vez, com a MESMA receita do editor
        // (Renderer::ImportRuntimeTexture, slot 0 = sRGB, nao-normal), para o hash bater com a
        // procedencia que o editor registra.
        std::unordered_map<std::string, const FTextureCPUData*> OverrideTextures;
        auto LoadOverride = [&](const std::string& _Utf8) -> const FTextureCPUData* {
            const auto [It, Inserted] = OverrideTextures.try_emplace(_Utf8, nullptr);
            if (!Inserted) return It->second;
            try {
                const std::filesystem::path P(std::u8string(_Utf8.begin(), _Utf8.end()));
                FTextureCPUData Data = FTexture::LoadCPUAuto(P.wstring(), false, true);
                if (Data.Valid()) {
                    _OwnedTextures.push_back(std::move(Data));
                    It->second = &_OwnedTextures.back();
                }
            } catch (const std::exception& E) {
                LogWarning(std::string("[OMM BAKER] textura do sidecar de materiais nao carregou (")
                           + E.what() + "): " + _Utf8);
            }
            if (!It->second)
                LogWarning("[OMM BAKER] textura do sidecar de materiais ausente, mantendo a cozida: " +
                           _Utf8);
            return It->second;
        };
        auto HashOf = [](const FTextureCPUData* _T) {
            return (!_T || !_T->Valid()) ? kOmmWhiteTexture : OmmHashTexture(*_T);
        };

        const u32 MeshCount = std::min<u32>(_Scene.MeshHeader.MeshCount,
                                            static_cast<u32>(_Scene.Meshes.size()));
        std::unordered_set<u64> Seen;
        for (const SSceneRenderable& R : _Scene.Renderables) {
            if (R.MeshIndex >= MeshCount || R.MaterialIndex == kNoMaterial ||
                R.MaterialIndex >= _Scene.Materials.size())
                continue;
            const auto Ov = _Overrides.find(R.MaterialIndex);
            if (Ov == _Overrides.end()) continue;
            const SSceneMaterial& M = _Scene.Materials[R.MaterialIndex];
            const bool AlphaTest = Ov->second.AlphaTest.value_or(M.AlphaTest != 0);
            if (!AlphaTest) continue; // sem alpha test o par usa o BLAS opaco: nada a bakear
            const u64 Pair = (static_cast<u64>(R.MeshIndex) << 32) | R.MaterialIndex;
            if (!Seen.insert(Pair).second) continue;

            // A textura EFETIVA: override ("" = slot limpo = branco), senao a cozida.
            const FTextureCPUData* Cooked = nullptr;
            if (M.BaseColor[0]) {
                const auto It = TextureByPath.find(
                    std::string(M.BaseColor, strnlen(M.BaseColor, kCookedMaxPath)));
                if (It != TextureByPath.end()) Cooked = It->second;
            }
            const FTextureCPUData* Alpha = Cooked;
            if (Ov->second.AlbedoPath) {
                if (Ov->second.AlbedoPath->empty()) Alpha = nullptr;
                else if (const FTextureCPUData* L = LoadOverride(*Ov->second.AlbedoPath)) Alpha = L;
            }

            FOmmBakeJob J;
            J.Mesh         = &_Scene.Meshes[R.MeshIndex];
            J.AlphaTexture = Alpha;
            J.BaseAlpha    = M.BaseColorFactor[3]; // o sidecar grava so o RGB da cor base
            J.AlphaCutoff  = Ov->second.AlphaCutoff.value_or(M.AlphaCutoff);
            J.Key.MeshIndex     = R.MeshIndex;
            J.Key.MaterialIndex = R.MaterialIndex;
            J.Key.Texture       = HashOf(Alpha);
            std::memcpy(&J.Key.BaseAlphaBits, &J.BaseAlpha, sizeof(f32));
            std::memcpy(&J.Key.CutoffBits, &J.AlphaCutoff, sizeof(f32));
            if (!J.Mesh->Vertices.empty() && !J.Mesh->Indices.empty())
                J.Key.Geometry = OmmHashGeometry(
                    &J.Mesh->Vertices.front().TexCoord[0], sizeof(Vertex),
                    static_cast<u32>(J.Mesh->Vertices.size()), J.Mesh->Indices.data(),
                    static_cast<u32>(J.Mesh->Indices.size()));
            Jobs.push_back(J);
        }
        return Jobs;
    }
}
