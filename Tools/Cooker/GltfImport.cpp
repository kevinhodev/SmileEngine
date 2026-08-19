#include "GltfImport.h"
#include "Json.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SmileCook {
    namespace {

        // RH(Y-up, glTF) -> LH(Y-up, engine): nega Z nas posicoes/normais e inverte o winding.
        // Mesma conversao do caminho FBX, pelo mesmo motivo — o par negacao+winding preserva o
        // facing; so um dos dois deixaria a cena inside-out.
        constexpr bool kReverseWinding = true;

        // Limites de sanidade. O .gltf pode ter vindo pela rede de um servico de geracao, e todo
        // numero dele e entrada nao confiavel: uma contagem absurda num accessor viraria uma
        // alocacao de gigabytes antes de qualquer leitura.
        constexpr uint64_t kMaxBufferBytes   = 512ull * 1024ull * 1024ull;
        constexpr uint32_t kMaxAccessorCount = 64u * 1024u * 1024u;

        struct FGltf {
            FJson                             Root;
            std::vector<std::vector<uint8_t>> Buffers;
            fs::path                          SourceDir;
            fs::path                          OutDir;
            std::string                       OutStem;
        };

        // --- utilitarios de arquivo -------------------------------------------------------------
        bool ReadWholeFile(const fs::path& _Path, std::vector<uint8_t>& _Out, std::string& _Error) {
            std::ifstream File(_Path, std::ios::binary);
            if (!File) { _Error = "nao foi possivel abrir " + _Path.string(); return false; }
            File.seekg(0, std::ios::end);
            const std::streamoff Size = File.tellg();
            if (Size < 0) { _Error = "nao foi possivel medir " + _Path.string(); return false; }
            if (static_cast<uint64_t>(Size) > kMaxBufferBytes) {
                _Error = _Path.string() + " excede o limite de 512 MB do Cooker";
                return false;
            }
            File.seekg(0, std::ios::beg);
            _Out.resize(static_cast<size_t>(Size));
            if (Size > 0) File.read(reinterpret_cast<char*>(_Out.data()), Size);
            if (!File) { _Error = "leitura incompleta de " + _Path.string(); return false; }
            return true;
        }

        bool WriteWholeFile(const fs::path& _Path, const uint8_t* _Data, size_t _Size) {
            std::error_code Ec;
            fs::create_directories(_Path.parent_path(), Ec);
            std::ofstream File(_Path, std::ios::binary | std::ios::trunc);
            if (!File) return false;
            if (_Size > 0) File.write(reinterpret_cast<const char*>(_Data), static_cast<std::streamsize>(_Size));
            return static_cast<bool>(File);
        }

        std::string ToLower(std::string _Value) {
            for (char& C : _Value)
                if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
            return _Value;
        }

        // --- URIs -------------------------------------------------------------------------------
        std::string PercentDecode(const std::string& _Uri) {
            std::string Out;
            Out.reserve(_Uri.size());
            auto HexDigit = [](char C) -> int {
                if (C >= '0' && C <= '9') return C - '0';
                if (C >= 'a' && C <= 'f') return C - 'a' + 10;
                if (C >= 'A' && C <= 'F') return C - 'A' + 10;
                return -1;
            };
            for (size_t I = 0; I < _Uri.size(); ++I) {
                if (_Uri[I] == '%' && I + 2 < _Uri.size()) {
                    const int Hi = HexDigit(_Uri[I + 1]);
                    const int Lo = HexDigit(_Uri[I + 2]);
                    if (Hi >= 0 && Lo >= 0) {
                        Out.push_back(static_cast<char>((Hi << 4) | Lo));
                        I += 2;
                        continue;
                    }
                }
                Out.push_back(_Uri[I]);
            }
            return Out;
        }

        bool DecodeBase64(const std::string& _Text, std::vector<uint8_t>& _Out) {
            auto Value = [](char C) -> int {
                if (C >= 'A' && C <= 'Z') return C - 'A';
                if (C >= 'a' && C <= 'z') return C - 'a' + 26;
                if (C >= '0' && C <= '9') return C - '0' + 52;
                if (C == '+') return 62;
                if (C == '/') return 63;
                return -1;
            };
            _Out.clear();
            _Out.reserve(_Text.size() / 4 * 3);
            uint32_t Accum = 0;
            int      Bits  = 0;
            for (const char C : _Text) {
                if (C == '=' ) break;
                if (C == '\n' || C == '\r' || C == ' ' || C == '\t') continue;
                const int V = Value(C);
                if (V < 0) return false;
                Accum = (Accum << 6) | static_cast<uint32_t>(V);
                Bits += 6;
                if (Bits >= 8) {
                    Bits -= 8;
                    _Out.push_back(static_cast<uint8_t>((Accum >> Bits) & 0xFFu));
                }
            }
            return true;
        }

        // "data:<mime>;base64,<payload>". Devolve false se nao for um data URI.
        bool DecodeDataUri(const std::string& _Uri, std::vector<uint8_t>& _Out, std::string& _Mime) {
            if (_Uri.rfind("data:", 0) != 0) return false;
            const size_t Comma = _Uri.find(',');
            if (Comma == std::string::npos) return false;
            const std::string Meta = _Uri.substr(5, Comma - 5);
            const size_t Semi = Meta.find(';');
            _Mime = (Semi == std::string::npos) ? Meta : Meta.substr(0, Semi);
            const bool Base64 = Meta.find("base64") != std::string::npos;
            const std::string Payload = _Uri.substr(Comma + 1);
            if (!Base64) {
                const std::string Decoded = PercentDecode(Payload);
                _Out.assign(Decoded.begin(), Decoded.end());
                return true;
            }
            return DecodeBase64(Payload, _Out);
        }

        // Caminho externo referenciado pelo .gltf. Recusa tudo que escape do diretorio do asset:
        // o arquivo pode ter sido baixado de um servico, e um "uri" pode apontar para qualquer
        // lugar do disco. O Cooker so LE aqui, mas ler arbitrariamente ja e o suficiente para
        // vazar conteudo para dentro de um asset cozido.
        bool ResolveExternalUri(const fs::path& _BaseDir, const std::string& _Uri,
                                fs::path& _Out, std::string& _Error) {
            const std::string Decoded = PercentDecode(_Uri);
            if (Decoded.find("://") != std::string::npos) {
                _Error = "uri remoto nao suportado: " + _Uri;
                return false;
            }
            const fs::path Relative(Decoded);
            if (Relative.is_absolute()) {
                _Error = "uri absoluto recusado: " + _Uri;
                return false;
            }
            for (const fs::path& Part : Relative)
                if (Part == "..") { _Error = "uri saindo do diretorio do asset: " + _Uri; return false; }
            _Out = _BaseDir / Relative;
            return true;
        }

        // --- container GLB ----------------------------------------------------------------------
        bool ParseGlb(const std::vector<uint8_t>& _Bytes, FJson& _Json,
                      std::vector<uint8_t>& _Bin, std::string& _Error) {
            constexpr uint32_t kMagic = 0x46546C67u; // 'glTF'
            constexpr uint32_t kJson  = 0x4E4F534Au; // 'JSON'
            constexpr uint32_t kBin   = 0x004E4942u; // 'BIN\0'
            if (_Bytes.size() < 12) { _Error = "GLB truncado"; return false; }

            auto ReadU32 = [&_Bytes](size_t _Offset) {
                uint32_t Value = 0;
                std::memcpy(&Value, _Bytes.data() + _Offset, 4);
                return Value;
            };
            if (ReadU32(0) != kMagic) { _Error = "GLB sem o magic 'glTF'"; return false; }
            const uint32_t Version = ReadU32(4);
            if (Version != 2) {
                _Error = "GLB versao " + std::to_string(Version) + " (o Cooker le glTF 2.0)";
                return false;
            }
            const uint32_t Declared = ReadU32(8);
            // O tamanho declarado manda: um arquivo com bytes a mais no fim (concatenacao, download
            // parcial reaproveitado) nao pode virar um chunk fantasma.
            const size_t Total = std::min<size_t>(Declared, _Bytes.size());

            bool   HasJson = false;
            size_t Offset  = 12;
            while (Offset + 8 <= Total) {
                const uint32_t ChunkLength = ReadU32(Offset);
                const uint32_t ChunkType   = ReadU32(Offset + 4);
                Offset += 8;
                if (ChunkLength > Total - Offset) { _Error = "chunk GLB alem do fim do arquivo"; return false; }
                if (ChunkType == kJson) {
                    if (HasJson) { _Error = "GLB com dois chunks JSON"; return false; }
                    std::string ParseError;
                    if (!ParseJson(reinterpret_cast<const char*>(_Bytes.data() + Offset),
                                   ChunkLength, _Json, ParseError)) {
                        _Error = "JSON do GLB invalido: " + ParseError;
                        return false;
                    }
                    HasJson = true;
                } else if (ChunkType == kBin) {
                    _Bin.assign(_Bytes.begin() + static_cast<std::ptrdiff_t>(Offset),
                                _Bytes.begin() + static_cast<std::ptrdiff_t>(Offset + ChunkLength));
                }
                // Chunks desconhecidos sao ignorados por spec.
                Offset += ChunkLength;
                Offset = (Offset + 3u) & ~static_cast<size_t>(3u); // padding de 4 bytes
            }
            if (!HasJson) { _Error = "GLB sem chunk JSON"; return false; }
            return true;
        }

        // --- buffers e accessors ----------------------------------------------------------------
        bool LoadBuffers(FGltf& _Gltf, std::vector<uint8_t>&& _GlbBin, std::string& _Error) {
            const FJson* Buffers = _Gltf.Root.Find("buffers");
            if (!Buffers || !Buffers->IsArray()) return true; // cena sem geometria: nada a carregar

            for (size_t I = 0; I < Buffers->Size(); ++I) {
                const FJson& Buffer = *Buffers->At(I);
                const std::string Uri = Buffer.GetString("uri", "");
                std::vector<uint8_t> Bytes;
                if (Uri.empty()) {
                    // Sem uri = o chunk BIN do GLB, e so o buffer 0 pode reivindica-lo.
                    if (I != 0 || _GlbBin.empty()) {
                        _Error = "buffer " + std::to_string(I) + " sem uri e sem chunk BIN";
                        return false;
                    }
                    Bytes = std::move(_GlbBin);
                } else if (Uri.rfind("data:", 0) == 0) {
                    std::string Mime;
                    if (!DecodeDataUri(Uri, Bytes, Mime)) {
                        _Error = "data uri invalido no buffer " + std::to_string(I);
                        return false;
                    }
                } else {
                    fs::path Path;
                    if (!ResolveExternalUri(_Gltf.SourceDir, Uri, Path, _Error)) return false;
                    if (!ReadWholeFile(Path, Bytes, _Error)) return false;
                }
                _Gltf.Buffers.push_back(std::move(Bytes));
            }
            return true;
        }

        struct FAccessorView {
            const uint8_t* Data   = nullptr;
            size_t         Stride = 0;
            uint32_t       Count  = 0;
            int            ComponentType = 0;
            int            Components    = 0;
            bool           Normalized    = false;
        };

        int ComponentBytes(int _ComponentType) {
            switch (_ComponentType) {
                case 5120: case 5121: return 1; // byte / unsigned byte
                case 5122: case 5123: return 2; // short / unsigned short
                case 5125: case 5126: return 4; // unsigned int / float
                default: return 0;
            }
        }

        int TypeComponents(const std::string& _Type) {
            if (_Type == "SCALAR") return 1;
            if (_Type == "VEC2")   return 2;
            if (_Type == "VEC3")   return 3;
            if (_Type == "VEC4")   return 4;
            if (_Type == "MAT4")   return 16;
            return 0;
        }

        bool ResolveAccessor(const FGltf& _Gltf, int _AccessorIndex, FAccessorView& _Out,
                             std::string& _Error) {
            const FJson* Accessors = _Gltf.Root.Find("accessors");
            const FJson* Accessor  = (Accessors && _AccessorIndex >= 0)
                                   ? Accessors->At(static_cast<size_t>(_AccessorIndex)) : nullptr;
            if (!Accessor || !Accessor->IsObject()) {
                _Error = "accessor " + std::to_string(_AccessorIndex) + " inexistente";
                return false;
            }
            // Sparse muda o VALOR de entradas individuais depois do bufferView. Ignorar seria
            // cozinhar geometria silenciosamente errada — melhor recusar.
            if (Accessor->Find("sparse")) {
                _Error = "accessor sparse nao suportado (reexporte o asset sem sparse)";
                return false;
            }

            const int Count = Accessor->GetInt("count", 0);
            if (Count <= 0 || static_cast<uint32_t>(Count) > kMaxAccessorCount) {
                _Error = "accessor com count invalido";
                return false;
            }
            _Out.Count         = static_cast<uint32_t>(Count);
            _Out.ComponentType = Accessor->GetInt("componentType", 0);
            _Out.Components    = TypeComponents(Accessor->GetString("type", ""));
            _Out.Normalized    = Accessor->GetBool("normalized", false);
            const int ElemBytes = ComponentBytes(_Out.ComponentType);
            if (ElemBytes == 0 || _Out.Components == 0) {
                _Error = "accessor com componentType/type desconhecido";
                return false;
            }
            const size_t ElementSize = static_cast<size_t>(ElemBytes) * static_cast<size_t>(_Out.Components);

            const int ViewIndex = Accessor->GetInt("bufferView", -1);
            if (ViewIndex < 0) {
                // Sem bufferView os valores sao zero por spec. Ninguem gera isso para POSITION;
                // aceitar caladamente daria uma malha colapsada na origem.
                _Error = "accessor sem bufferView nao suportado";
                return false;
            }
            const FJson* Views = _Gltf.Root.Find("bufferViews");
            const FJson* View  = Views ? Views->At(static_cast<size_t>(ViewIndex)) : nullptr;
            if (!View || !View->IsObject()) { _Error = "bufferView inexistente"; return false; }

            const int BufferIndex = View->GetInt("buffer", -1);
            if (BufferIndex < 0 || static_cast<size_t>(BufferIndex) >= _Gltf.Buffers.size()) {
                _Error = "bufferView aponta para buffer inexistente";
                return false;
            }
            const std::vector<uint8_t>& Buffer = _Gltf.Buffers[static_cast<size_t>(BufferIndex)];

            const int64_t ViewOffset = View->GetInt("byteOffset", 0);
            const int64_t ViewLength = View->GetInt("byteLength", 0);
            const int64_t Stride     = View->GetInt("byteStride", 0);
            const int64_t AccOffset  = Accessor->GetInt("byteOffset", 0);
            if (ViewOffset < 0 || ViewLength < 0 || Stride < 0 || AccOffset < 0) {
                _Error = "offsets negativos no bufferView/accessor";
                return false;
            }
            if (static_cast<uint64_t>(ViewOffset) + static_cast<uint64_t>(ViewLength) > Buffer.size()) {
                _Error = "bufferView alem do fim do buffer";
                return false;
            }

            const size_t Effective = (Stride > 0) ? static_cast<size_t>(Stride) : ElementSize;
            if (Stride > 0 && static_cast<size_t>(Stride) < ElementSize) {
                _Error = "byteStride menor que o elemento do accessor";
                return false;
            }
            // Ultimo byte lido = AccOffset + (Count-1)*Stride + ElementSize. Em uint64 porque o
            // produto estoura 32 bits muito antes de estourar o limite de tamanho do buffer.
            const uint64_t Span = static_cast<uint64_t>(AccOffset) +
                                  static_cast<uint64_t>(_Out.Count - 1u) * Effective + ElementSize;
            if (Span > static_cast<uint64_t>(ViewLength)) {
                _Error = "accessor le alem do bufferView";
                return false;
            }

            _Out.Data   = Buffer.data() + ViewOffset + AccOffset;
            _Out.Stride = Effective;
            return true;
        }

        // Le um componente ja convertido para float, aplicando a normalizacao da spec quando o
        // accessor e `normalized` (o caso de UV/cor em ushort que varios exportadores usam).
        float ReadComponent(const uint8_t* _Element, int _ComponentType, bool _Normalized, int _Index) {
            switch (_ComponentType) {
                case 5126: { float V; std::memcpy(&V, _Element + 4 * _Index, 4); return V; }
                case 5125: { uint32_t V; std::memcpy(&V, _Element + 4 * _Index, 4);
                             return static_cast<float>(V); }
                case 5123: { uint16_t V; std::memcpy(&V, _Element + 2 * _Index, 2);
                             return _Normalized ? static_cast<float>(V) / 65535.0f : static_cast<float>(V); }
                case 5122: { int16_t V; std::memcpy(&V, _Element + 2 * _Index, 2);
                             return _Normalized ? std::max(static_cast<float>(V) / 32767.0f, -1.0f)
                                                : static_cast<float>(V); }
                case 5121: { uint8_t V = _Element[_Index];
                             return _Normalized ? static_cast<float>(V) / 255.0f : static_cast<float>(V); }
                case 5120: { int8_t V; std::memcpy(&V, _Element + _Index, 1);
                             return _Normalized ? std::max(static_cast<float>(V) / 127.0f, -1.0f)
                                                : static_cast<float>(V); }
                default: return 0.0f;
            }
        }

        bool ReadFloats(const FGltf& _Gltf, int _AccessorIndex, int _ExpectedComponents,
                        std::vector<float>& _Out, std::string& _Error) {
            FAccessorView View;
            if (!ResolveAccessor(_Gltf, _AccessorIndex, View, _Error)) return false;
            if (View.Components < _ExpectedComponents) {
                _Error = "accessor com menos componentes que o atributo pede";
                return false;
            }
            _Out.resize(static_cast<size_t>(View.Count) * static_cast<size_t>(_ExpectedComponents));
            for (uint32_t I = 0; I < View.Count; ++I) {
                const uint8_t* Element = View.Data + static_cast<size_t>(I) * View.Stride;
                for (int C = 0; C < _ExpectedComponents; ++C)
                    _Out[static_cast<size_t>(I) * _ExpectedComponents + C] =
                        ReadComponent(Element, View.ComponentType, View.Normalized, C);
            }
            return true;
        }

        bool ReadIndices(const FGltf& _Gltf, int _AccessorIndex, std::vector<uint32_t>& _Out,
                         std::string& _Error) {
            FAccessorView View;
            if (!ResolveAccessor(_Gltf, _AccessorIndex, View, _Error)) return false;
            if (View.Components != 1) { _Error = "accessor de indices nao e SCALAR"; return false; }
            _Out.resize(View.Count);
            for (uint32_t I = 0; I < View.Count; ++I) {
                const uint8_t* Element = View.Data + static_cast<size_t>(I) * View.Stride;
                switch (View.ComponentType) {
                    case 5121: _Out[I] = Element[0]; break;
                    case 5123: { uint16_t V; std::memcpy(&V, Element, 2); _Out[I] = V; break; }
                    case 5125: { uint32_t V; std::memcpy(&V, Element, 4); _Out[I] = V; break; }
                    default: _Error = "indices em componentType nao inteiro"; return false;
                }
            }
            return true;
        }

        // --- transforms -------------------------------------------------------------------------
        // 4x4 na convencao COLUNA do glTF (p' = M * p), que e a mesma do ufbx_matrix. A conversao
        // para o TRS row-vector da engine acontece uma unica vez, no DecomposeToEngineTRS.
        struct FMat4 {
            double M[4][4] = { {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1} };
        };

        FMat4 Multiply(const FMat4& _A, const FMat4& _B) {
            FMat4 R;
            for (int Row = 0; Row < 4; ++Row)
                for (int Col = 0; Col < 4; ++Col) {
                    double Sum = 0.0;
                    for (int K = 0; K < 4; ++K) Sum += _A.M[Row][K] * _B.M[K][Col];
                    R.M[Row][Col] = Sum;
                }
            return R;
        }

        FMat4 NodeLocalMatrix(const FJson& _Node) {
            // `matrix` tem precedencia e vem column-major: os 4 primeiros valores sao a coluna 0.
            if (const FJson* Matrix = _Node.Find("matrix"); Matrix && Matrix->Size() == 16) {
                FMat4 Out;
                for (int Col = 0; Col < 4; ++Col)
                    for (int Row = 0; Row < 4; ++Row) {
                        const FJson* V = Matrix->At(static_cast<size_t>(Col * 4 + Row));
                        Out.M[Row][Col] = (V && V->IsNumber()) ? V->Number : 0.0;
                    }
                return Out;
            }

            double T[3] = { 0, 0, 0 };
            double R[4] = { 0, 0, 0, 1 }; // quaternion (x, y, z, w)
            double S[3] = { 1, 1, 1 };
            auto ReadVec = [&_Node](const char* _Key, double* _Out, int _Count) {
                const FJson* Value = _Node.Find(_Key);
                if (!Value || Value->Size() != static_cast<size_t>(_Count)) return;
                for (int I = 0; I < _Count; ++I) {
                    const FJson* V = Value->At(static_cast<size_t>(I));
                    if (V && V->IsNumber()) _Out[I] = V->Number;
                }
            };
            ReadVec("translation", T, 3);
            ReadVec("rotation",    R, 4);
            ReadVec("scale",       S, 3);

            const double Len = std::sqrt(R[0]*R[0] + R[1]*R[1] + R[2]*R[2] + R[3]*R[3]);
            if (Len > 1e-12) { R[0] /= Len; R[1] /= Len; R[2] /= Len; R[3] /= Len; }
            const double X = R[0], Y = R[1], Z = R[2], W = R[3];

            FMat4 Out;
            // Rotacao do quaternion, ja multiplicada pela escala por coluna (M = T * R * S).
            const double Rot[3][3] = {
                { 1 - 2*(Y*Y + Z*Z),     2*(X*Y - Z*W),     2*(X*Z + Y*W) },
                {     2*(X*Y + Z*W), 1 - 2*(X*X + Z*Z),     2*(Y*Z - X*W) },
                {     2*(X*Z - Y*W),     2*(Y*Z + X*W), 1 - 2*(X*X + Y*Y) },
            };
            for (int Row = 0; Row < 3; ++Row) {
                for (int Col = 0; Col < 3; ++Col) Out.M[Row][Col] = Rot[Row][Col] * S[Col];
                Out.M[Row][3] = T[Row];
            }
            return Out;
        }

        FColumnMatrix ToColumnMatrix(const FMat4& _M) {
            FColumnMatrix Out;
            for (int Col = 0; Col < 4; ++Col)
                for (int Row = 0; Row < 3; ++Row)
                    Out.Cols[Col][Row] = _M.M[Row][Col];
            return Out;
        }

        // --- materiais --------------------------------------------------------------------------
        struct FTextureExporter {
            const FGltf* Gltf = nullptr;
            bool         Enabled = true;
            // image index -> caminho relativo ja resolvido ("" = falhou e ja foi avisado).
            std::unordered_map<int, std::string> Cache;

            std::string ExtensionForMime(const std::string& _Mime, const std::string& _Fallback) {
                const std::string Mime = ToLower(_Mime);
                if (Mime == "image/png")  return ".png";
                if (Mime == "image/jpeg") return ".jpg";
                if (Mime == "image/bmp")  return ".bmp";
                return _Fallback;
            }

            // Caminho RELATIVO ao diretorio do cozido, que e como o SceneLoader resolve textura.
            std::string Resolve(int _TextureIndex) {
                if (!Enabled || _TextureIndex < 0) return "";
                const FJson* Textures = Gltf->Root.Find("textures");
                const FJson* Texture  = Textures ? Textures->At(static_cast<size_t>(_TextureIndex)) : nullptr;
                if (!Texture) return "";
                int Source = Texture->GetInt("source", -1);
                if (Source < 0) {
                    // KHR_texture_basisu e afins guardam a imagem numa extensao. O arquivo e KTX2
                    // (o runtime nao le), mas resolver o indice aqui deixa o aviso preciso.
                    if (const FJson* Ext = Texture->Find("extensions"); Ext && Ext->IsObject())
                        for (const auto& KV : Ext->Object)
                            if (const FJson* S = KV.second.Find("source"); S && S->IsNumber())
                                Source = static_cast<int>(S->Number);
                }
                if (Source < 0) return "";

                if (auto Hit = Cache.find(Source); Hit != Cache.end()) return Hit->second;
                std::string Relative = Export(Source);
                Cache.emplace(Source, Relative);
                return Relative;
            }

            std::string Export(int _ImageIndex) {
                const FJson* Images = Gltf->Root.Find("images");
                const FJson* Image  = Images ? Images->At(static_cast<size_t>(_ImageIndex)) : nullptr;
                if (!Image) return "";

                const std::string Uri  = Image->GetString("uri", "");
                const std::string Mime = Image->GetString("mimeType", "");

                std::vector<uint8_t> Bytes;
                std::string          Extension;

                if (!Uri.empty() && Uri.rfind("data:", 0) != 0) {
                    // Arquivo ao lado do .gltf. Se o cozido vai para o MESMO diretorio, referencia
                    // direto — copiar so criaria uma segunda copia da mesma textura no disco.
                    std::string Error;
                    fs::path Path;
                    if (!ResolveExternalUri(Gltf->SourceDir, Uri, Path, Error)) {
                        std::printf("[Cooker]   ! textura ignorada: %s\n", Error.c_str());
                        return "";
                    }
                    if (!fs::exists(Path)) {
                        std::printf("[Cooker]   ! textura ausente: %s\n", Path.string().c_str());
                        return "";
                    }
                    std::error_code Ec;
                    if (fs::equivalent(Gltf->SourceDir, Gltf->OutDir, Ec) && !Ec)
                        return fs::path(PercentDecode(Uri)).generic_string();
                    std::string ReadError;
                    if (!ReadWholeFile(Path, Bytes, ReadError)) {
                        std::printf("[Cooker]   ! %s\n", ReadError.c_str());
                        return "";
                    }
                    Extension = ToLower(Path.extension().string());
                } else if (!Uri.empty()) {
                    std::string DataMime;
                    if (!DecodeDataUri(Uri, Bytes, DataMime)) {
                        std::printf("[Cooker]   ! data uri de imagem invalido\n");
                        return "";
                    }
                    Extension = ExtensionForMime(DataMime.empty() ? Mime : DataMime, ".png");
                } else {
                    const int ViewIndex = Image->GetInt("bufferView", -1);
                    if (ViewIndex < 0) return "";
                    const FJson* Views = Gltf->Root.Find("bufferViews");
                    const FJson* View  = Views ? Views->At(static_cast<size_t>(ViewIndex)) : nullptr;
                    if (!View) return "";
                    const int BufferIndex = View->GetInt("buffer", -1);
                    if (BufferIndex < 0 || static_cast<size_t>(BufferIndex) >= Gltf->Buffers.size()) return "";
                    const std::vector<uint8_t>& Buffer = Gltf->Buffers[static_cast<size_t>(BufferIndex)];
                    const int64_t Offset = View->GetInt("byteOffset", 0);
                    const int64_t Length = View->GetInt("byteLength", 0);
                    if (Offset < 0 || Length <= 0 ||
                        static_cast<uint64_t>(Offset) + static_cast<uint64_t>(Length) > Buffer.size())
                        return "";
                    Bytes.assign(Buffer.begin() + static_cast<std::ptrdiff_t>(Offset),
                                 Buffer.begin() + static_cast<std::ptrdiff_t>(Offset + Length));
                    Extension = ExtensionForMime(Mime, ".png");
                }

                if (Bytes.empty()) return "";
                if (Extension == ".jpeg") Extension = ".jpg";
                if (Extension != ".png" && Extension != ".jpg" && Extension != ".bmp" &&
                    Extension != ".tga" && Extension != ".dds") {
                    std::printf("[Cooker]   ! imagem %d em formato nao suportado (%s)\n",
                                _ImageIndex, Extension.empty() ? "?" : Extension.c_str());
                    return "";
                }

                // Convencao do cozido: texturas em Textures/ ao lado do .sscene. O stem do cozido
                // entra no nome para dois assets gerados na mesma pasta nao se sobrescreverem.
                const std::string FileName = Gltf->OutStem + "_img" + std::to_string(_ImageIndex) + Extension;
                const fs::path    Out      = Gltf->OutDir / "Textures" / FileName;
                if (!WriteWholeFile(Out, Bytes.data(), Bytes.size())) {
                    std::printf("[Cooker]   ! nao foi possivel escrever %s\n", Out.string().c_str());
                    return "";
                }
                std::printf("[Cooker]   textura extraida: Textures/%s (%zu KB)\n",
                            FileName.c_str(), Bytes.size() / 1024);
                return "Textures/" + FileName;
            }
        };

        int TextureIndexOf(const FJson* _Owner, const char* _Key, int& _OutTexCoord) {
            const FJson* Ref = _Owner ? _Owner->Find(_Key) : nullptr;
            if (!Ref || !Ref->IsObject()) return -1;
            _OutTexCoord = Ref->GetInt("texCoord", 0);
            return Ref->GetInt("index", -1);
        }
    }

    bool CookGltf(const fs::path& _InPath, const fs::path& _OutBase,
                  const FGltfOptions& _Options, std::string& _Error) {
        std::vector<uint8_t> FileBytes;
        if (!ReadWholeFile(_InPath, FileBytes, _Error)) return false;

        FGltf Gltf;
        Gltf.SourceDir = _InPath.parent_path().empty() ? fs::path(".") : _InPath.parent_path();
        Gltf.OutDir    = _OutBase.parent_path().empty() ? fs::path(".") : _OutBase.parent_path();
        Gltf.OutStem   = _OutBase.stem().string();

        std::vector<uint8_t> GlbBin;
        const bool IsGlb = FileBytes.size() >= 4 && FileBytes[0] == 'g' && FileBytes[1] == 'l' &&
                           FileBytes[2] == 'T' && FileBytes[3] == 'F';
        if (IsGlb) {
            if (!ParseGlb(FileBytes, Gltf.Root, GlbBin, _Error)) return false;
        } else {
            std::string ParseError;
            if (!ParseJson(reinterpret_cast<const char*>(FileBytes.data()), FileBytes.size(),
                           Gltf.Root, ParseError)) {
                _Error = "glTF invalido: " + ParseError;
                return false;
            }
        }
        if (!Gltf.Root.IsObject()) { _Error = "raiz do glTF nao e um objeto"; return false; }

        if (const FJson* Asset = Gltf.Root.Find("asset")) {
            const std::string Version = Asset->GetString("version", "");
            if (!Version.empty() && Version.rfind("2.", 0) != 0) {
                _Error = "glTF " + Version + " nao suportado (o Cooker le a 2.x)";
                return false;
            }
        }
        // Extensao REQUERIDA que nao entendemos = recusa. As de compressao mudam o significado
        // dos bufferViews: sem o decodificador, o que sairia daqui seria ruido com cara de malha.
        if (const FJson* Required = Gltf.Root.Find("extensionsRequired"); Required && Required->IsArray()) {
            for (size_t I = 0; I < Required->Size(); ++I) {
                const FJson* Name = Required->At(I);
                if (!Name || !Name->IsString()) continue;
                const std::string& Ext = Name->String;
                if (Ext == "KHR_materials_emissive_strength" || Ext == "KHR_materials_unlit" ||
                    Ext == "KHR_texture_transform" || Ext == "KHR_materials_ior" ||
                    Ext == "KHR_materials_specular")
                    continue; // ignoraveis: afetam sombreamento fino, nao a leitura da geometria
                _Error = "extensao requerida nao suportada: " + Ext +
                         (Ext.rfind("KHR_draco", 0) == 0 || Ext.rfind("EXT_meshopt", 0) == 0
                              ? " (reexporte o .glb sem compressao de malha)" : "");
                return false;
            }
        }

        if (!LoadBuffers(Gltf, std::move(GlbBin), _Error)) return false;

        FCookedScene Cooked;
        FTextureExporter Textures;
        Textures.Gltf    = &Gltf;
        Textures.Enabled = _Options.ExtractTextures;

        // --- materiais (resolvidos sob demanda, na ordem de uso) ---
        std::unordered_map<int, uint32_t> MaterialCache;
        auto ResolveMaterial = [&](int _Index) -> uint32_t {
            if (_Index < 0) return Smile::kNoMaterial;
            if (auto Hit = MaterialCache.find(_Index); Hit != MaterialCache.end()) return Hit->second;
            const FJson* Materials = Gltf.Root.Find("materials");
            const FJson* Material  = Materials ? Materials->At(static_cast<size_t>(_Index)) : nullptr;
            if (!Material) return Smile::kNoMaterial;

            Smile::SSceneMaterial Out{};
            std::string Name = Material->GetString("name", "");
            if (Name.empty()) Name = "Material_" + std::to_string(_Index);
            SetStr(Out.Name, Smile::kCookedMaxName, Name);

            const FJson* Pbr = Material->Find("pbrMetallicRoughness");
            Out.BaseColorFactor[0] = Out.BaseColorFactor[1] = Out.BaseColorFactor[2] =
                Out.BaseColorFactor[3] = 1.0f;
            Out.MetallicFactor  = 1.0f; // defaults da spec glTF
            Out.RoughnessFactor = 1.0f;
            if (Pbr) {
                if (const FJson* Factor = Pbr->Find("baseColorFactor"); Factor && Factor->Size() == 4)
                    for (int C = 0; C < 4; ++C) {
                        const FJson* V = Factor->At(static_cast<size_t>(C));
                        if (V && V->IsNumber()) Out.BaseColorFactor[C] = static_cast<float>(V->Number);
                    }
                Out.MetallicFactor  = static_cast<float>(Pbr->GetNumber("metallicFactor", 1.0));
                Out.RoughnessFactor = static_cast<float>(Pbr->GetNumber("roughnessFactor", 1.0));
            }

            float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
            if (const FJson* Emissive = Material->Find("emissiveFactor"); Emissive && Emissive->Size() == 3)
                for (int C = 0; C < 3; ++C) {
                    const FJson* V = Emissive->At(static_cast<size_t>(C));
                    if (V && V->IsNumber()) EmissiveFactor[C] = static_cast<float>(V->Number);
                }
            Out.EmissiveStrength = 1.0f;
            if (const FJson* Ext = Material->Find("extensions"))
                if (const FJson* Strength = Ext->Find("KHR_materials_emissive_strength"))
                    Out.EmissiveStrength = static_cast<float>(
                        Strength->GetNumber("emissiveStrength", 1.0));
            for (int C = 0; C < 3; ++C) Out.EmissiveFactor[C] = EmissiveFactor[C];

            int TexCoord = 0;
            bool NonZeroTexCoord = false;
            auto Slot = [&](const FJson* _Owner, const char* _Key) {
                const int Index = TextureIndexOf(_Owner, _Key, TexCoord);
                if (Index >= 0 && TexCoord != 0) NonZeroTexCoord = true;
                return Textures.Resolve(Index);
            };
            // metallicRoughness do glTF ja E o packing que o slot Specular espera: G=roughness,
            // B=metalness (o R fica com o AO quando o exportador junta as tres, que e o caso
            // comum de asset gerado). Por isso ele vai direto, sem reempacotar nada.
            const std::string BaseColor = Slot(Pbr, "baseColorTexture");
            const std::string Specular  = Slot(Pbr, "metallicRoughnessTexture");
            const std::string Normal    = Slot(Material, "normalTexture");
            const std::string Emissive  = Slot(Material, "emissiveTexture");
            if (NonZeroTexCoord)
                std::printf("[Cooker]   ! material '%s' usa TEXCOORD_%d; a engine so tem um canal de UV\n",
                            Name.c_str(), TexCoord);

            SetStr(Out.BaseColor, Smile::kCookedMaxPath, BaseColor);
            SetStr(Out.Specular,  Smile::kCookedMaxPath, Specular);
            SetStr(Out.Normal,    Smile::kCookedMaxPath, Normal);
            SetStr(Out.Emissive,  Smile::kCookedMaxPath, Emissive);

            const std::string AlphaMode = Material->GetString("alphaMode", "OPAQUE");
            Out.AlphaCutoff = static_cast<float>(Material->GetNumber("alphaCutoff", 0.5));
            Out.TwoSided    = Material->GetBool("doubleSided", false) ? 1u : 0u;
            if (AlphaMode == "MASK") {
                Out.AlphaTest = 1u;
            } else if (AlphaMode == "BLEND") {
                Out.Blend = 1u;
            } else {
                // OPAQUE ignora o alpha por spec. Sem isto, um baseColorFactor com A<1 (comum em
                // asset gerado) faria o material cair no passe forward e sumir do G-buffer.
                Out.BaseColorFactor[3] = 1.0f;
            }
            // Foliage fica em 0: e um shading model que o cozido de FBX liga por heuristica de nome
            // do Bistro, e nome de malha gerada por IA nao carrega essa convencao.

            const uint32_t Index = static_cast<uint32_t>(Cooked.Materials.size());
            Cooked.Materials.push_back(Out);
            MaterialCache.emplace(_Index, Index);
            if (BaseColor.empty())
                std::printf("[Cooker]   ! material '%s' sem BaseColor (usa o fator RGBA)\n", Name.c_str());
            return Index;
        };

        // --- percurso dos nos ---
        const FJson* Nodes  = Gltf.Root.Find("nodes");
        const FJson* Meshes = Gltf.Root.Find("meshes");
        const size_t NodeCount = Nodes ? Nodes->Size() : 0;

        // Raizes: a cena default quando existe, senao todo no sem pai (asset sem "scenes" e raro
        // mas legal, e e o que alguns servicos de geracao emitem).
        std::vector<int> Roots;
        const FJson* Scenes = Gltf.Root.Find("scenes");
        const int SceneIndex = Gltf.Root.GetInt("scene", 0);
        if (const FJson* Scene = Scenes ? Scenes->At(static_cast<size_t>(SceneIndex)) : nullptr) {
            if (const FJson* SceneNodes = Scene->Find("nodes"))
                for (size_t I = 0; I < SceneNodes->Size(); ++I) {
                    const FJson* V = SceneNodes->At(I);
                    if (V && V->IsNumber()) Roots.push_back(static_cast<int>(V->Number));
                }
        }
        if (Roots.empty()) {
            std::vector<bool> HasParent(NodeCount, false);
            for (size_t I = 0; I < NodeCount; ++I) {
                const FJson* Node = Nodes->At(I);
                const FJson* Children = Node ? Node->Find("children") : nullptr;
                if (!Children) continue;
                for (size_t C = 0; C < Children->Size(); ++C) {
                    const FJson* V = Children->At(C);
                    if (V && V->IsNumber()) {
                        const int Child = static_cast<int>(V->Number);
                        if (Child >= 0 && static_cast<size_t>(Child) < NodeCount)
                            HasParent[static_cast<size_t>(Child)] = true;
                    }
                }
            }
            for (size_t I = 0; I < NodeCount; ++I)
                if (!HasParent[I]) Roots.push_back(static_cast<int>(I));
        }

        // Dedup por (mesh, primitiva): dois nos apontando para a mesma malha viram duas instancias
        // de UMA geometria, exatamente como a v7 faz no caminho FBX.
        std::map<std::pair<int, size_t>, uint32_t> MeshCache;
        std::vector<uint8_t> Visited(NodeCount, 0);

        struct FStackItem { int Node; FMat4 Parent; int32_t ParentRenderable; };
        std::vector<FStackItem> Stack;
        for (auto It = Roots.rbegin(); It != Roots.rend(); ++It)
            Stack.push_back(FStackItem{ *It, FMat4{}, -1 });

        size_t SkippedPrimitives = 0;
        while (!Stack.empty()) {
            const FStackItem Item = Stack.back();
            Stack.pop_back();
            if (Item.Node < 0 || static_cast<size_t>(Item.Node) >= NodeCount) continue;
            // O grafo de nos DEVE ser uma arvore por spec; um ciclo (ou um no compartilhado por
            // dois pais) faria este laco nao terminar.
            if (Visited[static_cast<size_t>(Item.Node)]) continue;
            Visited[static_cast<size_t>(Item.Node)] = 1;

            const FJson* Node = Nodes->At(static_cast<size_t>(Item.Node));
            if (!Node) continue;
            const FMat4 World = Multiply(Item.Parent, NodeLocalMatrix(*Node));

            int32_t FirstRenderable = Item.ParentRenderable;
            const int MeshIndex = Node->GetInt("mesh", -1);
            const FJson* Mesh = (MeshIndex >= 0 && Meshes) ? Meshes->At(static_cast<size_t>(MeshIndex)) : nullptr;
            const FJson* Primitives = Mesh ? Mesh->Find("primitives") : nullptr;

            if (Primitives && Primitives->IsArray()) {
                std::string NodeName = Node->GetString("name", "");
                if (NodeName.empty()) NodeName = Mesh->GetString("name", "");
                if (NodeName.empty()) NodeName = "Mesh_" + std::to_string(MeshIndex);
                const bool MultiPart = Primitives->Size() > 1;

                for (size_t Pi = 0; Pi < Primitives->Size(); ++Pi) {
                    const FJson* Primitive = Primitives->At(Pi);
                    if (!Primitive || !Primitive->IsObject()) continue;
                    const int Mode = Primitive->GetInt("mode", 4);
                    if (Mode != 4) { ++SkippedPrimitives; continue; } // so TRIANGLES

                    const uint32_t MaterialIndex = ResolveMaterial(Primitive->GetInt("material", -1));

                    std::string RenderableName = NodeName;
                    if (MultiPart && MaterialIndex != Smile::kNoMaterial &&
                        MaterialIndex < Cooked.Materials.size()) {
                        const char* MatName = Cooked.Materials[MaterialIndex].Name;
                        if (MatName[0] != '\0')
                            RenderableName += " [" +
                                std::string(MatName, strnlen(MatName, Smile::kCookedMaxName)) + "]";
                    }

                    auto Emit = [&](uint32_t _Mesh) {
                        Smile::SSceneRenderable R{};
                        R.MeshIndex     = _Mesh;
                        R.MaterialIndex = MaterialIndex;
                        const FNodeTRS T = DecomposeToEngineTRS(ToColumnMatrix(World));
                        std::memcpy(R.Position,      T.Pos,   sizeof(R.Position));
                        std::memcpy(R.RotationEuler, T.Rot,   sizeof(R.RotationEuler));
                        std::memcpy(R.Scale,         T.Scale, sizeof(R.Scale));
                        R.ParentIndex = Item.ParentRenderable;
                        SetStr(R.Name, Smile::kCookedMaxName, RenderableName);
                        if (FirstRenderable == Item.ParentRenderable)
                            FirstRenderable = static_cast<int32_t>(Cooked.Renderables.size());
                        Cooked.Renderables.push_back(R);
                    };

                    if (auto Hit = MeshCache.find({ MeshIndex, Pi }); Hit != MeshCache.end()) {
                        // TotalTriangles conta triangulos UNICOS, como no caminho FBX: o que a
                        // instancia acrescenta aparece na linha de instancing, nao aqui.
                        Emit(Hit->second);
                        ++Cooked.DedupHits;
                        continue;
                    }

                    const FJson* Attributes = Primitive->Find("attributes");
                    const int PositionAccessor = Attributes ? Attributes->GetInt("POSITION", -1) : -1;
                    if (PositionAccessor < 0) { ++SkippedPrimitives; continue; }

                    std::vector<float> Positions, Normals, UVs;
                    if (!ReadFloats(Gltf, PositionAccessor, 3, Positions, _Error)) return false;
                    const size_t VertexCount = Positions.size() / 3;

                    const int NormalAccessor = Attributes->GetInt("NORMAL", -1);
                    if (NormalAccessor >= 0) {
                        if (!ReadFloats(Gltf, NormalAccessor, 3, Normals, _Error)) return false;
                        if (Normals.size() / 3 != VertexCount) Normals.clear();
                    }
                    const int UVAccessor = Attributes->GetInt("TEXCOORD_0", -1);
                    if (UVAccessor >= 0) {
                        if (!ReadFloats(Gltf, UVAccessor, 2, UVs, _Error)) return false;
                        if (UVs.size() / 2 != VertexCount) UVs.clear();
                    }

                    std::vector<uint32_t> Indices;
                    const int IndexAccessor = Primitive->GetInt("indices", -1);
                    if (IndexAccessor >= 0) {
                        if (!ReadIndices(Gltf, IndexAccessor, Indices, _Error)) return false;
                        for (const uint32_t I : Indices)
                            if (I >= VertexCount) {
                                _Error = "indice fora da faixa numa primitiva";
                                return false;
                            }
                    } else {
                        Indices.resize(VertexCount);
                        for (size_t I = 0; I < VertexCount; ++I) Indices[I] = static_cast<uint32_t>(I);
                    }
                    if (Indices.size() < 3) { ++SkippedPrimitives; continue; }

                    // Sem NORMAL a spec manda sombrear FLAT: a normal e a da face, e cada canto
                    // vira um vertice proprio (o weld nao pode unir cantos de faces vizinhas, ou
                    // a normal viraria uma media que a spec nao pede).
                    const bool FlatShade = Normals.empty();

                    FSubMesh SubMesh;
                    const size_t TriangleCount = Indices.size() / 3;
                    for (size_t T = 0; T < TriangleCount; ++T) {
                        const uint32_t Corner[3] = { Indices[T*3+0], Indices[T*3+1], Indices[T*3+2] };

                        float FaceNormal[3] = { 0.0f, 1.0f, 0.0f };
                        if (FlatShade) {
                            const float* P0 = &Positions[Corner[0] * 3];
                            const float* P1 = &Positions[Corner[1] * 3];
                            const float* P2 = &Positions[Corner[2] * 3];
                            const double E1[3] = { P1[0]-P0[0], P1[1]-P0[1], P1[2]-P0[2] };
                            const double E2[3] = { P2[0]-P0[0], P2[1]-P0[1], P2[2]-P0[2] };
                            double N[3] = { E1[1]*E2[2] - E1[2]*E2[1],
                                            E1[2]*E2[0] - E1[0]*E2[2],
                                            E1[0]*E2[1] - E1[1]*E2[0] };
                            const double Len = std::sqrt(N[0]*N[0] + N[1]*N[1] + N[2]*N[2]);
                            if (Len > 1e-20) {
                                FaceNormal[0] = static_cast<float>(N[0] / Len);
                                FaceNormal[1] = static_cast<float>(N[1] / Len);
                                FaceNormal[2] = static_cast<float>(N[2] / Len);
                            }
                        }

                        uint32_t Out[3];
                        for (int C = 0; C < 3; ++C) {
                            const uint32_t Source = Corner[C];
                            Vertex V{};
                            // RH -> LH: nega Z. A UV NAO e invertida — o glTF ja poe a origem no
                            // canto superior esquerdo, igual ao D3D (o caminho FBX inverte V
                            // justamente porque la a origem e a de baixo).
                            V.Position[0] =  Positions[Source * 3 + 0];
                            V.Position[1] =  Positions[Source * 3 + 1];
                            V.Position[2] = -Positions[Source * 3 + 2];
                            if (FlatShade) {
                                V.Normal[0] =  FaceNormal[0];
                                V.Normal[1] =  FaceNormal[1];
                                V.Normal[2] = -FaceNormal[2];
                            } else {
                                float Nx = Normals[Source * 3 + 0];
                                float Ny = Normals[Source * 3 + 1];
                                float Nz = Normals[Source * 3 + 2];
                                const double Len = std::sqrt(double(Nx)*Nx + double(Ny)*Ny + double(Nz)*Nz);
                                if (Len > 1e-12) {
                                    Nx = static_cast<float>(Nx / Len);
                                    Ny = static_cast<float>(Ny / Len);
                                    Nz = static_cast<float>(Nz / Len);
                                } else {
                                    Nx = 0.0f; Ny = 1.0f; Nz = 0.0f;
                                }
                                V.Normal[0] = Nx; V.Normal[1] = Ny; V.Normal[2] = -Nz;
                            }
                            if (!UVs.empty()) {
                                V.TexCoord[0] = UVs[Source * 2 + 0];
                                V.TexCoord[1] = UVs[Source * 2 + 1];
                            }
                            Out[C] = SubMesh.Add(V);
                        }
                        if (kReverseWinding) {
                            SubMesh.Indices.push_back(Out[0]);
                            SubMesh.Indices.push_back(Out[2]);
                            SubMesh.Indices.push_back(Out[1]);
                        } else {
                            SubMesh.Indices.push_back(Out[0]);
                            SubMesh.Indices.push_back(Out[1]);
                            SubMesh.Indices.push_back(Out[2]);
                        }
                        ++Cooked.TotalTriangles;
                    }
                    if (SubMesh.Empty()) { ++SkippedPrimitives; continue; }

                    const uint32_t Emitted = Cooked.AppendMesh(SubMesh);
                    MeshCache.emplace(std::make_pair(MeshIndex, Pi), Emitted);
                    Emit(Emitted);
                }
            }

            if (const FJson* Children = Node->Find("children"); Children && Children->IsArray()) {
                for (size_t I = Children->Size(); I > 0; --I) {
                    const FJson* V = Children->At(I - 1);
                    if (V && V->IsNumber())
                        Stack.push_back(FStackItem{ static_cast<int>(V->Number), World, FirstRenderable });
                }
            }
        }

        if (Cooked.Entries.empty()) {
            _Error = "o glTF nao produziu geometria (sem primitivas TRIANGLES utilizaveis)";
            return false;
        }
        if (SkippedPrimitives > 0)
            std::printf("[Cooker] %zu primitiva(s) ignorada(s): modo != TRIANGLES, sem POSITION ou vazia\n",
                        SkippedPrimitives);

        NormalizeCookedScene(Cooked, _Options.Normalize);
        Cooked.PrintSummary();
        if (!Cooked.Write(_OutBase)) { _Error = "falha ao escrever o cozido"; return false; }
        return true;
    }
}
