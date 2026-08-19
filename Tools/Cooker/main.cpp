// SmileCooker — converte uma cena de entrada no formato binario proprio da engine
// (.smesh + .sscene). Roda offline; o runtime nunca le FBX nem glTF direto.
//
// Uso:  SmileCooker <entrada.fbx|.gltf|.glb> [saida_sem_extensao] [opcoes]
//   ex: SmileCooker Assets/Scenes/Bistro/BistroExterior.fbx
//       -> gera BistroExterior.smesh e BistroExterior.sscene ao lado do .fbx
//   ex: SmileCooker Assets/Generated/dragao/dragao.glb --fit 2 --ground --center
//
// Dois caminhos de entrada, um unico artefato de saida:
//   FBX (ufbx, neste arquivo) — cena autorada, com toda a heuristica de material do Bistro.
//   glTF/GLB (GltfImport.cpp) — malha avulsa, tipicamente saida de um gerador 3D por IA.
// O que vem depois da leitura (weld, layout do blob, payload de RT, TRS do renderavel) e
// comum aos dois e mora no CookedWriter.h.
//
// Pipeline do FBX (espelha Cry RC / Unreal Interchange, versao enxuta):
//   1. ufbx carrega o FBX, normalizado p/ RH Y-up + metros (MODIFY_GEOMETRY).
//   2. Por no com mesh, por parte-de-material: triangula em espaco LOCAL, converte RH->LH
//      (nega Z + inverte winding), funde vertices (weld). O transform do no NAO entra no
//      vertice — vira TRS do renderavel (v7). Com isso (malha, parte) e DEDUPLICADA: N nos
//      que compartilham a malha viram N instancias de UMA geometria.
//   3. Resolve as texturas pela convencao Bistro (nome_Sufixo.dds) + fallback ufbx.
//   4. Escreve .smesh (geometria) e .sscene (materiais + renderaveis).

#include "CookedWriter.h"
#include "GltfImport.h"

#include "ufbx.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>

using namespace SmileCook; // fs, Vertex, FSubMesh, FCookedScene, SetStr, DecomposeToEngineTRS

// --- RH(Y-up) -> LH(Y-up): inverter winding (par com a negacao de Z nas posicoes).
// Se a cena sair "inside-out" (culling invertido), trocar p/ false e rebuildar.
// Nos ESPELHADOS (det(geometry_to_world) < 0) invertem o winding de novo, por no —
// mesmo criterio do IsOddNegativeScale da Unreal (FbxMesh.cpp do Interchange).
static constexpr bool kReverseWinding = true;

// ----------------------------------------------------------------------------
// v7: geometry_to_world (ufbx, column-vector, RH) -> TRS do FTransform da engine (S * Rx*Ry*Rz * T,
// row-vector, LH, radianos). A conta em si mora no CookedWriter.h — aqui so a adaptacao do
// ufbx_matrix, cujas colunas ja estao no layout que o FColumnMatrix espera.
//
// Medido em Bistro/Emerald/Sponza: o pior erro de reconstrucao da 3x3 e 7e-12, e nenhuma delas
// tem shear ou determinante negativo — TRS reproduz a cena.
static FNodeTRS DecomposeNodeTRS(const ufbx_matrix& G) {
    FColumnMatrix C;
    for (int i = 0; i < 4; ++i) {
        C.Cols[i][0] = G.cols[i].x;
        C.Cols[i][1] = G.cols[i].y;
        C.Cols[i][2] = G.cols[i].z;
    }
    return DecomposeToEngineTRS(C);
}

// ----------------------------------------------------------------------------
static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}
static std::string BaseNameNoExt(const std::string& path) {
    return fs::path(path).stem().string();
}
// Extensoes de textura aceitas, em ordem de preferencia (DDS BC primeiro; PNG/etc decodificados
// por WIC no runtime). O runtime escolhe o loader pela extensao do caminho gravado.
static bool IsTexExt(const std::string& lowerExt) {
    return lowerExt == ".dds" || lowerExt == ".png" || lowerExt == ".tga" ||
           lowerExt == ".jpg" || lowerExt == ".jpeg" || lowerExt == ".bmp";
}

// Procura "Textures/<base>.<ext>" no diretorio da cena (case-insensitive no basename; em Windows
// "Textures" tambem casa com "textures"). Retorna o caminho relativo se existir, senao "".
static std::string FindTexture(const fs::path& sceneDir, const std::string& base) {
    if (base.empty()) return "";
    // Caminho rapido: tenta cada extensao na ordem de preferencia.
    static const char* kExts[] = { ".dds", ".png", ".tga", ".jpg", ".jpeg", ".bmp" };
    for (const char* ext : kExts) {
        fs::path rel = fs::path("Textures") / (base + ext);
        if (fs::exists(sceneDir / rel)) return rel.generic_string();
    }
    // Fallback case-insensitive: varre a pasta Textures uma vez (DDS tem prioridade sobre o resto).
    static std::vector<std::pair<std::string,std::string>> cache; // (lowerStem -> relPath), DDS na frente
    static bool built = false;
    if (!built) {
        fs::path texDir = sceneDir / "Textures";
        if (fs::exists(texDir)) {
            for (auto& e : fs::directory_iterator(texDir)) {
                if (!e.is_regular_file()) continue;
                std::string ext = ToLower(e.path().extension().string());
                if (!IsTexExt(ext)) continue;
                std::string rel = (fs::path("Textures") / e.path().filename()).generic_string();
                if (ext == ".dds") cache.emplace(cache.begin(), ToLower(e.path().stem().string()), rel);
                else               cache.emplace_back(ToLower(e.path().stem().string()), rel);
            }
        }
        built = true;
    }
    std::string lb = ToLower(base);
    for (auto& kv : cache) if (kv.first == lb) return kv.second;
    return "";
}

// Classifica o sufixo do basename de uma textura nos slots do material.
// Metalness/Roughness SEPARADOS tem precedencia sobre o ORM packed (Specular): so cai em Specular
// quando o nome indica canais combinados (orm/metalrough/metallicroughness).
enum class Slot { None, BaseColor, Specular, Normal, Emissive, Metalness, Roughness };
static Slot ClassifySuffix(const std::string& base) {
    std::string b = ToLower(base);
    // Ganha o token que TERMINA mais a direita (sufixo real), com desempate p/ o match mais longo.
    // Substring simples em ordem fixa classificava "Emissive_Light_2_Inst_Specular" como Emissive
    // (contem "emissive") — a lanterna da Emerald Square emitia o proprio spec map (verde puro,
    // G=roughness do packing Falcor) e a textura emissiva real ficava de fora. O desempate por
    // comprimento mantem "MetallicRoughness" no slot packed (Specular), nao em Roughness.
    struct KV { const char* k; Slot s; };
    static const KV kMap[] = {
        { "basecolor",         Slot::BaseColor }, { "base_color", Slot::BaseColor },
        { "albedo",            Slot::BaseColor }, { "diffuse",    Slot::BaseColor },
        { "normal",            Slot::Normal    },
        { "emissive",          Slot::Emissive  }, { "emission",   Slot::Emissive  },
        { "_orm",              Slot::Specular  }, { "metalrough", Slot::Specular  },
        { "metallicroughness", Slot::Specular  }, { "specular",   Slot::Specular  },
        { "roughness",         Slot::Roughness }, { "_rough",     Slot::Roughness },
        { "metalness",         Slot::Metalness }, { "metallic",   Slot::Metalness },
        { "_metal",            Slot::Metalness },
    };
    Slot   best     = Slot::None;
    size_t bestEnd  = 0;
    size_t bestLen  = 0;
    for (const KV& kv : kMap) {
        const size_t len = std::strlen(kv.k);
        const size_t pos = b.rfind(kv.k);
        if (pos == std::string::npos) continue;
        const size_t end = pos + len;
        if (end > bestEnd || (end == bestEnd && len > bestLen)) {
            best = kv.s; bestEnd = end; bestLen = len;
        }
    }
    return best;
}

// Heuristica de material masked/two-sided (folhagem/toldos do Bistro) pelo nome.
// Tokeniza o nome do material em palavras: separa em nao-alfanumerico, em fronteiras camelCase
// (minuscula->maiuscula) e letra<->digito; tudo minusculo. Ex.: "Emissive_StreetLight" ->
// {emissive, street, light}. Match por TOKEN INTEIRO evita o classico "street" conter "tree".
static std::vector<std::string> Tokenize(const std::string& name) {
    std::vector<std::string> toks;
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } };
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum) { flush(); continue; }
        if (!cur.empty()) {
            char p = name[i - 1];
            bool boundary = (p >= 'a' && p <= 'z') && (c >= 'A' && c <= 'Z');      // camelCase
            bool digitSwap = (((p >= '0' && p <= '9') ? 1 : 0) != ((c >= '0' && c <= '9') ? 1 : 0));
            if (boundary || digitSwap) flush();
        }
        cur.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    flush();
    return toks;
}

static bool HasToken(const std::vector<std::string>& toks, std::initializer_list<const char*> kws) {
    for (const auto& t : toks)
        for (const char* k : kws)
            if (t == k) return true;
    return false;
}

// Superficies duras: vetam o shading model de FOLHAGEM (nao o glow de subsurface em calcada/tronco/
// poste). NAO vetam cutout — uma corrente num poste ainda e cutout.
static bool IsHardSurface(const std::vector<std::string>& toks) {
    return HasToken(toks, { "street","road","pavement","cobble","cobblestone","stone","concrete",
                            "brick","asphalt","wall","floor","ground","metal","wood","trunk","sign",
                            "pivot","support","bulb","glass","plaster","pot","pots" });
}

// Vegetacao -> shading model folhagem (+ cutout). Vetado por superficie dura (ex.: calcada c/ folhas).
static bool LooksFoliage(const std::vector<std::string>& toks) {
    if (IsHardSurface(toks)) return false;
    return HasToken(toks, { "leaf","leaves","leafs","foliage","plant","plants","ivy","ivies","vine",
                            "vines","grass","fern","ferns","hedge","hedges","bush","bushes","branch",
                            "branches","flower","flowers","petal","petals","shrub","shrubs" });
}

// Cutout (alpha-test + two-sided) SEM ser folhagem: SO geometria perfurada/vazada de verdade —
// correntes, grades, telas, arames. NAO inclui tecido (toldo/cortina/pano sao OPACOS, nao recortam)
// nem "mesh" (ambiguo: pode ser so o nome do 3D mesh). NAO vetado por superficie dura.
static bool LooksCutout(const std::vector<std::string>& toks) {
    return HasToken(toks, { "chain","chains","fence","fences","grate","grates","grille","grilles",
                            "grill","wire","wires","net","nets","lattice","grid" });
}

// Vidro -> translucido (Blend, passe forward). Override por NOME porque o FBX nao carrega
// transmissao (todo vidro do Bistro vem com opacity=1) — mesmo espirito dos overrides que o
// Falcor faz no .pyscene (specularTransmission=1, IoR 1.55). Vidro EMISSIVO fica de fora:
// cooka opaco p/ manter o glow no caminho deferred (lanterna/spotlight). SO o token "glass";
// "window" pegaria caixilho/moldura opaca junto.
static bool LooksGlass(const std::vector<std::string>& toks) {
    if (HasToken(toks, { "emissive", "emission" })) return false;
    return HasToken(toks, { "glass" });
}

// ----------------------------------------------------------------------------
static void PrintUsage() {
    std::printf(
        "Uso: SmileCooker <entrada.fbx|.gltf|.glb> [saida_sem_extensao] [opcoes]\n"
        "\n"
        "  --opaque-glass     vidro cooka OPACO em vez de translucido (so FBX)\n"
        "  --fit <metros>     escala a cena para caber num cubo de N metros\n"
        "  --ground           encosta o ponto mais baixo em Y=0\n"
        "  --center           centraliza o volume em X/Z na origem\n"
        "  --no-textures      nao extrai as imagens embutidas do glTF\n"
        "\n"
        "  --fit/--ground/--center existem para malha gerada por IA, que chega sem unidade nem\n"
        "  pivot definido. Ajustam so o transform dos renderaveis; a geometria fica intacta.\n");
}

int main(int argc, char** argv) {
    // --opaque-glass: vidro cooka OPACO (entra no G-buffer -> reflexoes RT pintam nele) em vez
    // de translucido. P/ cenas de casca oca (Emerald Square: predios vazios atras da janela);
    // cenas com interior real atras do vidro (Bistro) ficam no default translucido.
    bool opaqueGlass = false;
    bool extractTextures = true;
    FNormalizeOptions normalize;
    std::vector<fs::path> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--opaque-glass") { opaqueGlass = true; continue; }
        if (arg == "--no-textures")  { extractTextures = false; continue; }
        if (arg == "--ground")       { normalize.DropToGround = true; continue; }
        if (arg == "--center")       { normalize.CenterXZ = true; continue; }
        if (arg == "--fit") {
            if (i + 1 >= argc) { std::printf("[Cooker] --fit precisa de um valor em metros\n"); return 1; }
            const double meters = std::atof(argv[++i]);
            // Faixa fechada: 0 desligaria a normalizacao sem dizer, e um valor absurdo so pode ser
            // erro de digitacao — em ambos os casos falhar aqui e mais barato que depurar a cena.
            if (!(meters > 0.0001 && meters < 100000.0)) {
                std::printf("[Cooker] --fit fora da faixa (0, 100000) metros\n");
                return 1;
            }
            normalize.FitSize = (float)meters;
            continue;
        }
        if (arg == "--help" || arg == "-h") { PrintUsage(); return 0; }
        if (!arg.empty() && arg[0] == '-') {
            std::printf("[Cooker] opcao desconhecida: %s\n", arg.c_str());
            PrintUsage();
            return 1;
        }
        positional.emplace_back(arg);
    }
    if (positional.empty()) {
        PrintUsage();
        return 1;
    }
    fs::path inPath = positional[0];
    fs::path sceneDir = inPath.parent_path();
    fs::path outBase = (positional.size() >= 2) ? positional[1]
                                                : (sceneDir / inPath.stem());

    // Despacho por extensao. glTF/GLB e outro importador inteiro (GltfImport.cpp) — nao ha
    // caminho comum antes da leitura, so depois dela.
    {
        std::string ext = ToLower(inPath.extension().string());
        if (ext == ".gltf" || ext == ".glb") {
            std::printf("[Cooker] Carregando glTF: %s\n", inPath.string().c_str());
            FGltfOptions options;
            options.Normalize       = normalize;
            options.ExtractTextures = extractTextures;
            std::string error;
            if (!CookGltf(inPath, outBase, options, error)) {
                std::printf("[Cooker] ERRO glTF: %s\n", error.c_str());
                return 2;
            }
            std::printf("[Cooker] OK.\n");
            return 0;
        }
    }

    std::printf("[Cooker] Carregando FBX: %s\n", inPath.string().c_str());

    ufbx_load_opts opts{};
    opts.target_axes        = ufbx_axes_right_handed_y_up; // normaliza qualquer eixo -> RH Y-up
    opts.target_unit_meters = 1.0f;                         // -> metros
    opts.space_conversion   = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY; // baka nas matrizes/geometria
    opts.generate_missing_normals = true;

    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_file(inPath.string().c_str(), &opts, &err);
    if (!scene) {
        std::printf("[Cooker] ERRO ufbx: %s\n", err.description.data ? err.description.data : "desconhecido");
        return 2;
    }
    std::printf("[Cooker] Cena carregada: %zu nos, %zu meshes, %zu materiais\n",
                scene->nodes.count, scene->meshes.count, scene->materials.count);

    // O cozido em construcao — as quatro regioes que vao para disco — e o writer moram no
    // CookedWriter.h, compartilhados com o importador glTF. As referencias abaixo mantem o resto
    // deste arquivo lendo exatamente como antes da extracao.
    FCookedScene cooked;
    auto& materials   = cooked.Materials;
    auto& renderables = cooked.Renderables;
    auto& totalTris   = cooked.TotalTriangles;
    auto& dedupHits   = cooked.DedupHits;

    // --- Tabela global de materiais (ufbx_material* -> indice) ---
    std::unordered_map<const ufbx_material*, uint32_t> matIndex;

    auto ResolveMaterial = [&](const ufbx_material* m) -> uint32_t {
        if (!m) return Smile::kNoMaterial;
        auto it = matIndex.find(m);
        if (it != matIndex.end()) return it->second;

        Smile::SSceneMaterial out{};
        std::string name(m->name.data ? m->name.data : "", m->name.length);
        if (name.empty()) name = "Material_" + std::to_string(materials.size());
        SetStr(out.Name, Smile::kCookedMaxName, name);

        // Fatores PBR constantes do material (ufbx mapeia Phong/Lambert/PBR -> pbr.*). Lidos aqui, mas
        // so ASSINADOS apos resolver texturas: com textura o fator e neutro (a textura E a cor); este
        // FBX traz base_color=0.5 ate em materiais texturizados, e aplica-lo escureceria tudo 2x.
        const ufbx_material_pbr_maps& pbr = m->pbr;
        auto mapVec = [](const ufbx_material_map& mm, float def, float out4[4]) {
            if (mm.has_value) { out4[0]=(float)mm.value_vec4.x; out4[1]=(float)mm.value_vec4.y;
                                out4[2]=(float)mm.value_vec4.z; out4[3]=(float)mm.value_vec4.w; }
            else { out4[0]=out4[1]=out4[2]=out4[3]=def; }
        };
        float baseFac[4]; mapVec(pbr.base_color, 1.0f, baseFac);
        float emisCol[4]; mapVec(pbr.emission_color, 0.0f, emisCol);
        const float opacity = pbr.opacity.has_value         ? (float)pbr.opacity.value_real         : 1.0f;
        const float emisFac = pbr.emission_factor.has_value ? (float)pbr.emission_factor.value_real : 1.0f;
        out.MetallicFactor  = pbr.metalness.has_value ? (float)pbr.metalness.value_real : 0.0f;
        out.RoughnessFactor = pbr.roughness.has_value ? (float)pbr.roughness.value_real : 0.8f;
        out.EmissiveStrength = 1.0f;
        out.AlphaCutoff      = 0.5f;

        std::string base, spec, norm, emis, metal, rough;

        // 1) Texturas referenciadas pelo proprio material (ufbx), classificadas por sufixo.
        for (size_t t = 0; t < m->textures.count; ++t) {
            const ufbx_texture* tex = m->textures.data[t].texture;
            if (!tex) continue;
            const ufbx_string& fn = tex->relative_filename.length ? tex->relative_filename : tex->filename;
            if (!fn.length) continue;
            std::string b = BaseNameNoExt(std::string(fn.data, fn.length));
            std::string rel = FindTexture(sceneDir, b);
            if (rel.empty()) continue;
            switch (ClassifySuffix(b)) {
                case Slot::BaseColor: if (base.empty())  base  = rel; break;
                case Slot::Specular:  if (spec.empty())  spec  = rel; break;
                case Slot::Normal:    if (norm.empty())  norm  = rel; break;
                case Slot::Emissive:  if (emis.empty())  emis  = rel; break;
                case Slot::Metalness: if (metal.empty()) metal = rel; break;
                case Slot::Roughness: if (rough.empty()) rough = rel; break;
                default: break;
            }
        }
        // 2) Fallback pela convencao de nome do material: <Nome>_<Sufixo>.<ext>
        if (base.empty())  base  = FindTexture(sceneDir, name + "_BaseColor");
        if (spec.empty())  spec  = FindTexture(sceneDir, name + "_Specular");
        if (norm.empty())  norm  = FindTexture(sceneDir, name + "_Normal");
        if (emis.empty())  emis  = FindTexture(sceneDir, name + "_Emissive");
        if (metal.empty()) metal = FindTexture(sceneDir, name + "_Metalness");
        if (rough.empty()) rough = FindTexture(sceneDir, name + "_Roughness");

        SetStr(out.BaseColor, Smile::kCookedMaxPath, base);
        SetStr(out.Specular,  Smile::kCookedMaxPath, spec);
        SetStr(out.Normal,    Smile::kCookedMaxPath, norm);
        SetStr(out.Emissive,  Smile::kCookedMaxPath, emis);
        SetStr(out.Metalness, Smile::kCookedMaxPath, metal);
        SetStr(out.Roughness, Smile::kCookedMaxPath, rough);

        // Fator neutro (1.0) quando ha textura naquele slot — a textura E a cor. Sem textura, usa o
        // valor do material (vidro=preto, lampada=cinza). Opacidade SEMPRE conta (translucidos).
        const bool hasBaseTex = !base.empty();
        const bool hasEmisTex = !emis.empty();
        out.BaseColorFactor[0] = hasBaseTex ? 1.0f : baseFac[0];
        out.BaseColorFactor[1] = hasBaseTex ? 1.0f : baseFac[1];
        out.BaseColorFactor[2] = hasBaseTex ? 1.0f : baseFac[2];
        out.BaseColorFactor[3] = std::min(hasBaseTex ? 1.0f : baseFac[3], opacity);
        out.EmissiveFactor[0]  = (hasEmisTex ? 1.0f : emisCol[0]) * emisFac;
        out.EmissiveFactor[1]  = (hasEmisTex ? 1.0f : emisCol[1]) * emisFac;
        out.EmissiveFactor[2]  = (hasEmisTex ? 1.0f : emisCol[2]) * emisFac;
        // Translucido: opacidade < 1 (alpha-blend no passe forward; ex.: dirt_decal).
        out.Blend = (out.BaseColorFactor[3] < 0.999f) ? 1u : 0u;

        {
            const std::vector<std::string> toks = Tokenize(name);
            const bool foliage = LooksFoliage(toks);
            const bool cutout  = foliage || LooksCutout(toks); // folhagem tambem e cutout
            out.Foliage   = foliage ? 1u : 0u;
            out.AlphaTest = cutout  ? 1u : 0u;
            out.TwoSided  = cutout  ? 1u : 0u;

            // Vidro: translucido no passe forward. Alpha por tipo: janela/vitrine 0.4 (lamina
            // fina, transmissao alta); GARRAFA 0.75 (vidro grosso e escuro, quase opaco — a 0.4
            // a garrafa de vinho praticamente sumia). O especular vem do Fresnel no shader,
            // nao do alpha. Two-sided (lamina fina).
            if (LooksGlass(toks)) {
                if (opaqueGlass) {
                    // --opaque-glass: janela-espelho — opaco no G-buffer, reflexoes RT pintam o
                    // ceu/rua (look da Falcor p/ casca oca). Liso e dieletrico; alpha restaurado.
                    out.Blend     = 0u;
                    out.AlphaTest = 0u;
                    out.TwoSided  = 0u;
                    out.BaseColorFactor[3] = 1.0f;
                    out.MetallicFactor  = 0.0f;
                    out.RoughnessFactor = std::min(out.RoughnessFactor, 0.08f);
                } else {
                    out.Blend     = 1u;
                    out.AlphaTest = 0u; // mutuamente exclusivo com blend
                    out.TwoSided  = 1u;
                    const float glassAlpha = HasToken(toks, { "bottle", "bottles" }) ? 0.75f : 0.4f;
                    out.BaseColorFactor[3] = std::min(out.BaseColorFactor[3], glassAlpha);
                }
            }
        }

        uint32_t idx = static_cast<uint32_t>(materials.size());
        materials.push_back(out);
        matIndex.emplace(m, idx);
        if (base.empty())
            std::printf("[Cooker]   ! material '%s' sem BaseColor resolvido\n", name.c_str());
        return idx;
    };

    // Nome do renderavel. Um no com N partes de material vira N renderaveis, e todos herdariam o
    // MESMO nome do no — no Sponza sao 115 nos virando 405 linhas identicas na arvore. Quando ha
    // mais de uma parte, desambigua pelo material (mesma ideia das sections da Unreal).
    auto NodeRenderableName = [&](const ufbx_node* node, const ufbx_mesh* mesh,
                                  uint32_t matIdx) -> std::string {
        std::string base(node->name.data, node->name.length);
        if (base.empty()) base = "Mesh";
        if (mesh->material_parts.count <= 1 || matIdx == Smile::kNoMaterial ||
            matIdx >= materials.size())
            return base;
        const char* mn = materials[matIdx].Name;
        if (mn[0] == '\0') return base;
        return base + " [" + std::string(mn, strnlen(mn, Smile::kCookedMaxName)) + "]";
    };

    // --- Geometria ---

    // Dedup (v7): a geometria agora sai em espaco LOCAL, entao dois nos que apontam para o mesmo
    // ufbx_mesh produzem bytes IDENTICOS — a diferenca entre eles passou a viver so no transform.
    // Chave = (ufbx_mesh, indice da parte de material). Medido: Emerald Square 2479 -> 281 partes
    // (uma malha usada por 201 nos); Bistro e Sponza nao tem instancing por ponteiro e nao mudam.
    std::map<std::pair<const ufbx_mesh*, size_t>, uint32_t> meshCache;

    // no -> indice do PRIMEIRO renderavel que ele emitiu (para resolver ParentIndex).
    std::unordered_map<const ufbx_node*, int32_t> firstRenderableOfNode;

    std::vector<uint32_t> triBuf;

    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        if (!node || !node->mesh) continue;
        const ufbx_mesh* mesh = node->mesh;
        triBuf.resize(std::max<size_t>(mesh->max_face_triangles * 3, 3));

        // v7: geometria em LOCAL. Sobra do mundo apenas a conversao de eixo RH->LH (nega Z), que
        // e do ESPACO e nao do no — por isso continua aqui e mantem os bytes iguais entre
        // instancias. A inversa-transposta saiu junto com o bake: sem transform no vertice, a
        // normal local nao precisa de correcao de escala (quem aplica escala e a matriz de modelo).
        //
        // O espelhamento tambem deixa de ser por no: com o transform fora do vertice, o
        // determinante negativo passa a viver na ESCALA do TRS, e quem inverte o winding
        // efetivo e a matriz de modelo. Ficaria errado bakear o flip na geometria compartilhada.
        // (Medido: nenhuma das 3 cenas tem determinante negativo.)
        const bool reverseWinding = kReverseWinding;

        // Ancestral renderavel mais proximo (o pai direto pode ser um grupo sem mesh).
        int32_t parentIdx = -1;
        for (const ufbx_node* a = node->parent; a; a = a->parent) {
            auto it = firstRenderableOfNode.find(a);
            if (it != firstRenderableOfNode.end()) { parentIdx = it->second; break; }
        }

        for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh->material_parts.data[pi];
            if (part.num_triangles == 0) continue;

            // Material desta parte: per-instance (node->materials) tem precedencia.
            const ufbx_material* mat = nullptr;
            if (pi < node->materials.count)      mat = node->materials.data[pi];
            else if (pi < mesh->materials.count) mat = mesh->materials.data[pi];
            uint32_t matIdx = ResolveMaterial(mat);

            // Ja cozinhamos esta (malha, parte)? Em espaco local os bytes seriam identicos —
            // so referencia. O MATERIAL nao entra na chave de proposito: ele e por RENDERAVEL
            // (node->materials tem precedencia sobre mesh->materials), entao duas instancias da
            // mesma malha podem legitimamente usar materiais diferentes sem duplicar geometria.
            if (auto hit = meshCache.find({ mesh, pi }); hit != meshCache.end()) {
                const FNodeTRS T = DecomposeNodeTRS(node->geometry_to_world);
                Smile::SSceneRenderable r{};
                r.MeshIndex = hit->second;
                r.MaterialIndex = matIdx;
                std::memcpy(r.Position, T.Pos, sizeof(r.Position));
                std::memcpy(r.RotationEuler, T.Rot, sizeof(r.RotationEuler));
                std::memcpy(r.Scale, T.Scale, sizeof(r.Scale));
                r.ParentIndex = parentIdx;
                SetStr(r.Name, Smile::kCookedMaxName, NodeRenderableName(node, mesh, matIdx));
                firstRenderableOfNode.emplace(node, (int32_t)renderables.size());
                renderables.push_back(r);
                ++dedupHits;
                continue;
            }

            FSubMesh sm;
            for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                uint32_t numTri = ufbx_triangulate_face(triBuf.data(), triBuf.size(), mesh, face);
                for (uint32_t t = 0; t < numTri; ++t) {
                    uint32_t corner[3] = { triBuf[t*3+0], triBuf[t*3+1], triBuf[t*3+2] };
                    uint32_t outIdx[3];
                    for (int c = 0; c < 3; ++c) {
                        ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, corner[c]);
                        ufbx_vec3 n = mesh->vertex_normal.exists
                                    ? ufbx_get_vertex_vec3(&mesh->vertex_normal, corner[c])
                                    : ufbx_vec3{0,1,0};
                        ufbx_vec2 uv = mesh->vertex_uv.exists
                                    ? ufbx_get_vertex_vec2(&mesh->vertex_uv, corner[c])
                                    : ufbx_vec2{0,0};
                        // v7: SEM transform aqui. A posicao e a normal ficam em espaco local; o
                        // geometry_to_world virou o TRS do renderavel. E isto que faz duas
                        // instancias da mesma malha gerarem bytes identicos e poderem ser
                        // deduplicadas — com o bake, cada uma era uma copia unica.
                        double nl = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
                        if (nl > 1e-12) { n.x/=nl; n.y/=nl; n.z/=nl; }
                        Vertex v;
                        v.Position[0] = (float)p.x; v.Position[1] = (float)p.y; v.Position[2] = -(float)p.z; // RH->LH
                        v.Normal[0]   = (float)n.x; v.Normal[1]   = (float)n.y; v.Normal[2]   = -(float)n.z;
                        v.TexCoord[0] = (float)uv.x; v.TexCoord[1] = 1.0f - (float)uv.y; // V flip (FBX->D3D)
                        outIdx[c] = sm.Add(v);
                    }
                    if (reverseWinding) {
                        sm.Indices.push_back(outIdx[0]);
                        sm.Indices.push_back(outIdx[2]);
                        sm.Indices.push_back(outIdx[1]);
                    } else {
                        sm.Indices.push_back(outIdx[0]);
                        sm.Indices.push_back(outIdx[1]);
                        sm.Indices.push_back(outIdx[2]);
                    }
                    ++totalTris;
                }
            }
            if (sm.Vertices.empty() || sm.Indices.empty()) continue;

            // O AppendMesh anexa [VB][IB][RTTri] ao blob e gera o payload de RT — nesta ordem
            // e neste ponto, porque `sm.Indices` so agora esta na forma final (depois do weld e
            // do reverseWinding), que e o que amarra RTTriangle[i] ao PrimitiveIndex i do BLAS.
            const uint32_t meshIdx = cooked.AppendMesh(sm);
            meshCache.emplace(std::make_pair(mesh, pi), meshIdx);

            const FNodeTRS T = DecomposeNodeTRS(node->geometry_to_world);
            Smile::SSceneRenderable r{};
            r.MeshIndex = meshIdx;
            r.MaterialIndex = matIdx;
            std::memcpy(r.Position, T.Pos, sizeof(r.Position));
            std::memcpy(r.RotationEuler, T.Rot, sizeof(r.RotationEuler));
            std::memcpy(r.Scale, T.Scale, sizeof(r.Scale));
            r.ParentIndex = parentIdx;
            SetStr(r.Name, Smile::kCookedMaxName, NodeRenderableName(node, mesh, matIdx));
            firstRenderableOfNode.emplace(node, (int32_t)renderables.size());
            renderables.push_back(r);
        }
    }

    ufbx_free_scene(scene);

    // A conta de RT sai em triangulos UNICOS, e nao instanciados: o payload mora por (malha,
    // parte), entao uma malha usada por 201 nos paga uma vez so — mesmo motivo pelo qual a dedup
    // da v7 vale tanto aqui.
    NormalizeCookedScene(cooked, normalize);
    cooked.PrintSummary();

    if (!cooked.Write(outBase)) return 3;

    std::printf("[Cooker] OK.\n");
    return 0;
}
