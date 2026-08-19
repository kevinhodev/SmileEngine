#pragma once

// ================================================================================================
// Parser JSON minimo, so para o que o Cooker precisa ler: o chunk JSON de um .glb / .gltf.
//
// Por que nao uma dependencia: o Cooker linka UM arquivo de terceiro (ufbx.c) e headers da
// engine, e essa disciplina e o que o mantem compilando sozinho. glTF usa um subconjunto
// pequenissimo de JSON (objetos rasos, arrays de numero, strings ASCII em quase toda parte), e
// um DOM de 200 linhas cobre isso inteiro sem arrastar um build system novo para dentro de
// Tools/.
//
// O parser trata a entrada como HOSTIL: o .gltf pode ter vindo de um servico de geracao por IA
// pela rede. Dai o limite de profundidade (recursao), a checagem de fim de buffer em cada passo e
// a ausencia de qualquer alocacao dimensionada por numero lido do arquivo.
//
// Fora de escopo de proposito: numeros em precisao maior que double, chaves duplicadas (fica a
// primeira) e ordenacao/indice das chaves — objetos glTF tem punhados de campos e a busca linear
// e mais rapida que montar um mapa.
// ================================================================================================

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace SmileCook {

    struct FJson {
        enum class EType : uint8_t { Null, Bool, Number, String, Array, Object };

        EType       Type   = EType::Null;
        bool        Bool   = false;
        double      Number = 0.0;
        std::string String;
        std::vector<FJson>                          Array;
        std::vector<std::pair<std::string, FJson>>  Object;

        bool IsNull()   const { return Type == EType::Null; }
        bool IsNumber() const { return Type == EType::Number; }
        bool IsString() const { return Type == EType::String; }
        bool IsArray()  const { return Type == EType::Array; }
        bool IsObject() const { return Type == EType::Object; }

        // Busca linear: objetos glTF tem poucas chaves. nullptr = ausente.
        const FJson* Find(const char* _Key) const {
            if (Type != EType::Object) return nullptr;
            for (const auto& KV : Object)
                if (KV.first == _Key) return &KV.second;
            return nullptr;
        }

        const FJson* At(size_t _Index) const {
            if (Type != EType::Array || _Index >= Array.size()) return nullptr;
            return &Array[_Index];
        }

        size_t Size() const { return Type == EType::Array ? Array.size() : 0; }

        // Acessores tolerantes: chave ausente OU de tipo errado devolve o default. O glTF e cheio
        // de campos opcionais com default definido pela spec, e propagar "existe?" ate cada uso
        // so encheria o importador de if.
        double GetNumber(const char* _Key, double _Default) const {
            const FJson* V = Find(_Key);
            return (V && V->Type == EType::Number) ? V->Number : _Default;
        }
        // Devolve _Default tambem para numero fracionario ou fora da faixa de int: no glTF todo
        // campo inteiro e indice ou contagem, e um valor assim so pode vir de arquivo corrompido.
        int GetInt(const char* _Key, int _Default) const {
            const FJson* V = Find(_Key);
            if (!V || V->Type != EType::Number) return _Default;
            const double N = V->Number;
            if (N < -2147483648.0 || N > 2147483647.0) return _Default;
            const int I = static_cast<int>(N);
            return (static_cast<double>(I) == N) ? I : _Default;
        }
        bool GetBool(const char* _Key, bool _Default) const {
            const FJson* V = Find(_Key);
            return (V && V->Type == EType::Bool) ? V->Bool : _Default;
        }
        std::string GetString(const char* _Key, const std::string& _Default) const {
            const FJson* V = Find(_Key);
            return (V && V->Type == EType::String) ? V->String : _Default;
        }
    };

    namespace JsonDetail {
        constexpr int kMaxDepth = 64; // aninhamento glTF real fica em <10; o limite e contra abuso

        struct FParser {
            const char* P   = nullptr;
            const char* End = nullptr;
            std::string Error;

            bool Fail(const char* _Message) {
                if (Error.empty()) Error = _Message;
                return false;
            }

            void SkipSpace() {
                while (P < End) {
                    const char C = *P;
                    if (C == ' ' || C == '\t' || C == '\n' || C == '\r') ++P;
                    else break;
                }
            }

            bool Literal(const char* _Text) {
                const size_t Len = std::char_traits<char>::length(_Text);
                if (static_cast<size_t>(End - P) < Len) return false;
                for (size_t I = 0; I < Len; ++I)
                    if (P[I] != _Text[I]) return false;
                P += Len;
                return true;
            }

            // \uXXXX -> UTF-8. Pares substitutos sao combinados; um substituto solto vira U+FFFD
            // em vez de derrubar o parse — nome de no nao vale abortar um cook.
            bool ParseEscapeUnicode(std::string& _Out) {
                auto Hex4 = [this](uint32_t& _Value) -> bool {
                    if (End - P < 4) return false;
                    _Value = 0;
                    for (int I = 0; I < 4; ++I) {
                        const char C = P[I];
                        uint32_t D;
                        if (C >= '0' && C <= '9')      D = static_cast<uint32_t>(C - '0');
                        else if (C >= 'a' && C <= 'f') D = static_cast<uint32_t>(C - 'a' + 10);
                        else if (C >= 'A' && C <= 'F') D = static_cast<uint32_t>(C - 'A' + 10);
                        else return false;
                        _Value = (_Value << 4) | D;
                    }
                    P += 4;
                    return true;
                };

                uint32_t Code = 0;
                if (!Hex4(Code)) return Fail("escape \\u invalido");
                if (Code >= 0xD800u && Code <= 0xDBFFu) {
                    uint32_t Low = 0;
                    const char* Save = P;
                    if (End - P >= 6 && P[0] == '\\' && P[1] == 'u') {
                        P += 2;
                        if (Hex4(Low) && Low >= 0xDC00u && Low <= 0xDFFFu)
                            Code = 0x10000u + ((Code - 0xD800u) << 10) + (Low - 0xDC00u);
                        else { P = Save; Code = 0xFFFDu; }
                    } else {
                        Code = 0xFFFDu;
                    }
                } else if (Code >= 0xDC00u && Code <= 0xDFFFu) {
                    Code = 0xFFFDu;
                }

                if (Code < 0x80u) {
                    _Out.push_back(static_cast<char>(Code));
                } else if (Code < 0x800u) {
                    _Out.push_back(static_cast<char>(0xC0u | (Code >> 6)));
                    _Out.push_back(static_cast<char>(0x80u | (Code & 0x3Fu)));
                } else if (Code < 0x10000u) {
                    _Out.push_back(static_cast<char>(0xE0u | (Code >> 12)));
                    _Out.push_back(static_cast<char>(0x80u | ((Code >> 6) & 0x3Fu)));
                    _Out.push_back(static_cast<char>(0x80u | (Code & 0x3Fu)));
                } else {
                    _Out.push_back(static_cast<char>(0xF0u | (Code >> 18)));
                    _Out.push_back(static_cast<char>(0x80u | ((Code >> 12) & 0x3Fu)));
                    _Out.push_back(static_cast<char>(0x80u | ((Code >> 6) & 0x3Fu)));
                    _Out.push_back(static_cast<char>(0x80u | (Code & 0x3Fu)));
                }
                return true;
            }

            bool ParseString(std::string& _Out) {
                if (P >= End || *P != '"') return Fail("string esperada");
                ++P;
                _Out.clear();
                while (true) {
                    if (P >= End) return Fail("string sem fechamento");
                    const char C = *P++;
                    if (C == '"') return true;
                    if (C != '\\') { _Out.push_back(C); continue; }
                    if (P >= End) return Fail("escape truncado");
                    const char E = *P++;
                    switch (E) {
                        case '"':  _Out.push_back('"');  break;
                        case '\\': _Out.push_back('\\'); break;
                        case '/':  _Out.push_back('/');  break;
                        case 'b':  _Out.push_back('\b'); break;
                        case 'f':  _Out.push_back('\f'); break;
                        case 'n':  _Out.push_back('\n'); break;
                        case 'r':  _Out.push_back('\r'); break;
                        case 't':  _Out.push_back('\t'); break;
                        case 'u':  if (!ParseEscapeUnicode(_Out)) return false; break;
                        default:   return Fail("escape desconhecido");
                    }
                }
            }

            bool ParseNumber(FJson& _Out) {
                // strtod precisa de um buffer terminado em NUL; o chunk JSON do GLB nao e. Copiar
                // o token (sempre curto) e mais simples do que reimplementar a conversao, e evita
                // ler um byte alem do fim do chunk.
                const char* Start = P;
                if (P < End && (*P == '-' || *P == '+')) ++P;
                while (P < End) {
                    const char C = *P;
                    const bool Numeric = (C >= '0' && C <= '9') || C == '.' || C == 'e' ||
                                         C == 'E' || C == '+' || C == '-';
                    if (!Numeric) break;
                    ++P;
                }
                if (P == Start) return Fail("numero esperado");
                const std::string Token(Start, static_cast<size_t>(P - Start));
                char* Stop = nullptr;
                const double Value = std::strtod(Token.c_str(), &Stop);
                if (Stop != Token.c_str() + Token.size()) return Fail("numero mal formado");
                _Out.Type   = FJson::EType::Number;
                _Out.Number = Value;
                return true;
            }

            bool ParseValue(FJson& _Out, int _Depth) {
                if (_Depth > kMaxDepth) return Fail("JSON aninhado demais");
                SkipSpace();
                if (P >= End) return Fail("fim inesperado do JSON");

                switch (*P) {
                    case '{': {
                        ++P;
                        _Out.Type = FJson::EType::Object;
                        SkipSpace();
                        if (P < End && *P == '}') { ++P; return true; }
                        while (true) {
                            SkipSpace();
                            std::string Key;
                            if (!ParseString(Key)) return false;
                            SkipSpace();
                            if (P >= End || *P != ':') return Fail("':' esperado");
                            ++P;
                            FJson Value;
                            if (!ParseValue(Value, _Depth + 1)) return false;
                            _Out.Object.emplace_back(std::move(Key), std::move(Value));
                            SkipSpace();
                            if (P >= End) return Fail("objeto sem fechamento");
                            if (*P == ',') { ++P; continue; }
                            if (*P == '}') { ++P; return true; }
                            return Fail("',' ou '}' esperado");
                        }
                    }
                    case '[': {
                        ++P;
                        _Out.Type = FJson::EType::Array;
                        SkipSpace();
                        if (P < End && *P == ']') { ++P; return true; }
                        while (true) {
                            FJson Value;
                            if (!ParseValue(Value, _Depth + 1)) return false;
                            _Out.Array.push_back(std::move(Value));
                            SkipSpace();
                            if (P >= End) return Fail("array sem fechamento");
                            if (*P == ',') { ++P; continue; }
                            if (*P == ']') { ++P; return true; }
                            return Fail("',' ou ']' esperado");
                        }
                    }
                    case '"':
                        _Out.Type = FJson::EType::String;
                        return ParseString(_Out.String);
                    case 't':
                        if (!Literal("true")) return Fail("literal invalido");
                        _Out.Type = FJson::EType::Bool; _Out.Bool = true;  return true;
                    case 'f':
                        if (!Literal("false")) return Fail("literal invalido");
                        _Out.Type = FJson::EType::Bool; _Out.Bool = false; return true;
                    case 'n':
                        if (!Literal("null")) return Fail("literal invalido");
                        _Out.Type = FJson::EType::Null; return true;
                    default:
                        return ParseNumber(_Out);
                }
            }
        };
    }

    inline bool ParseJson(const char* _Data, size_t _Size, FJson& _Out, std::string& _Error) {
        JsonDetail::FParser Parser;
        Parser.P   = _Data;
        Parser.End = _Data + _Size;
        _Out = FJson{};
        if (!Parser.ParseValue(_Out, 0)) {
            _Error = Parser.Error.empty() ? "JSON invalido" : Parser.Error;
            return false;
        }
        Parser.SkipSpace();
        // Lixo depois do valor raiz e sinal de arquivo truncado/concatenado, nao de folga do
        // formato — recusar aqui evita cozinhar meia cena.
        if (Parser.P != Parser.End) {
            _Error = "conteudo extra depois do JSON";
            return false;
        }
        return true;
    }
}
