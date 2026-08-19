// Testes do importador glTF/GLB do Cooker (Tools/Cooker/GltfImport.cpp).
//
// Nao tocam GPU: o cozimento e uma transformacao de bytes em bytes, e e exatamente ai que os
// erros deste caminho aparecem — Z negado sem o winding correspondente, V invertido a mais (o
// glTF ja vem na origem do D3D, o FBX nao), payload de RT gerado antes do winding final, ou um
// accessor lido alem do bufferView. Cada teste monta um .glb minimo em memoria, cozinha para um
// diretorio temporario e le o .smesh/.sscene de volta.

#include "GltfImport.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    bool Near(float A, float B, float Epsilon = 1.0e-5f) {
        const float D = A - B;
        return (D < 0.0f ? -D : D) <= Epsilon;
    }

    // --- montagem do .glb ---------------------------------------------------------------------
    void AppendU32(std::vector<uint8_t>& _Out, uint32_t _Value) {
        _Out.push_back(static_cast<uint8_t>( _Value        & 0xFFu));
        _Out.push_back(static_cast<uint8_t>((_Value >>  8) & 0xFFu));
        _Out.push_back(static_cast<uint8_t>((_Value >> 16) & 0xFFu));
        _Out.push_back(static_cast<uint8_t>((_Value >> 24) & 0xFFu));
    }

    void AppendFloats(std::vector<uint8_t>& _Out, std::initializer_list<float> _Values) {
        for (const float V : _Values) {
            uint32_t Bits;
            std::memcpy(&Bits, &V, 4);
            AppendU32(_Out, Bits);
        }
    }

    void AppendU16s(std::vector<uint8_t>& _Out, std::initializer_list<uint16_t> _Values) {
        for (const uint16_t V : _Values) {
            _Out.push_back(static_cast<uint8_t>(V & 0xFFu));
            _Out.push_back(static_cast<uint8_t>(V >> 8));
        }
    }

    void PadTo4(std::vector<uint8_t>& _Out, uint8_t _Filler) {
        while (_Out.size() % 4 != 0) _Out.push_back(_Filler);
    }

    std::vector<uint8_t> BuildGlb(const std::string& _Json, const std::vector<uint8_t>& _Bin) {
        std::vector<uint8_t> JsonChunk(_Json.begin(), _Json.end());
        PadTo4(JsonChunk, ' ');
        std::vector<uint8_t> BinChunk = _Bin;
        PadTo4(BinChunk, 0);

        std::vector<uint8_t> Out;
        AppendU32(Out, 0x46546C67u); // 'glTF'
        AppendU32(Out, 2);
        const uint32_t Total = 12u + 8u + static_cast<uint32_t>(JsonChunk.size()) +
                               (BinChunk.empty() ? 0u : 8u + static_cast<uint32_t>(BinChunk.size()));
        AppendU32(Out, Total);
        AppendU32(Out, static_cast<uint32_t>(JsonChunk.size()));
        AppendU32(Out, 0x4E4F534Au); // 'JSON'
        Out.insert(Out.end(), JsonChunk.begin(), JsonChunk.end());
        if (!BinChunk.empty()) {
            AppendU32(Out, static_cast<uint32_t>(BinChunk.size()));
            AppendU32(Out, 0x004E4942u); // 'BIN\0'
            Out.insert(Out.end(), BinChunk.begin(), BinChunk.end());
        }
        return Out;
    }

    void WriteBytes(const fs::path& _Path, const std::vector<uint8_t>& _Bytes) {
        fs::create_directories(_Path.parent_path());
        std::ofstream File(_Path, std::ios::binary | std::ios::trunc);
        File.write(reinterpret_cast<const char*>(_Bytes.data()),
                   static_cast<std::streamsize>(_Bytes.size()));
    }

    // --- leitura do cozido ---------------------------------------------------------------------
    struct FCookedRead {
        Smile::SMeshHeader                   MeshHeader{};
        std::vector<Smile::SMeshEntry>       Entries;
        std::vector<uint8_t>                 Blob;
        Smile::SSceneHeader                  SceneHeader{};
        std::vector<Smile::SSceneMaterial>   Materials;
        std::vector<Smile::SSceneRenderable> Renderables;

        const Smile::Vertex* VerticesOf(size_t _Mesh) const {
            return reinterpret_cast<const Smile::Vertex*>(Blob.data() + Entries[_Mesh].VertexOffset);
        }
        const uint32_t* IndicesOf(size_t _Mesh) const {
            return reinterpret_cast<const uint32_t*>(Blob.data() + Entries[_Mesh].IndexOffset);
        }
        const Smile::FRTTriangle* RTTrianglesOf(size_t _Mesh) const {
            return reinterpret_cast<const Smile::FRTTriangle*>(Blob.data() + Entries[_Mesh].RTTriangleOffset);
        }
    };

    bool ReadAll(const fs::path& _Path, std::vector<uint8_t>& _Out) {
        std::ifstream File(_Path, std::ios::binary);
        if (!File) return false;
        File.seekg(0, std::ios::end);
        const std::streamoff Size = File.tellg();
        File.seekg(0, std::ios::beg);
        _Out.resize(static_cast<size_t>(Size));
        if (Size > 0) File.read(reinterpret_cast<char*>(_Out.data()), Size);
        return static_cast<bool>(File);
    }

    bool ReadCooked(const fs::path& _OutBase, FCookedRead& _Out) {
        std::vector<uint8_t> Mesh, Scene;
        fs::path MeshPath = _OutBase;  MeshPath  += ".smesh";
        fs::path ScenePath = _OutBase; ScenePath += ".sscene";
        if (!ReadAll(MeshPath, Mesh) || !ReadAll(ScenePath, Scene)) return false;
        if (Mesh.size() < sizeof(Smile::SMeshHeader)) return false;

        std::memcpy(&_Out.MeshHeader, Mesh.data(), sizeof(Smile::SMeshHeader));
        const size_t EntryBytes = sizeof(Smile::SMeshEntry) * _Out.MeshHeader.MeshCount;
        if (Mesh.size() < sizeof(Smile::SMeshHeader) + EntryBytes) return false;
        _Out.Entries.resize(_Out.MeshHeader.MeshCount);
        if (EntryBytes > 0)
            std::memcpy(_Out.Entries.data(), Mesh.data() + sizeof(Smile::SMeshHeader), EntryBytes);
        _Out.Blob.assign(Mesh.begin() + static_cast<std::ptrdiff_t>(sizeof(Smile::SMeshHeader) + EntryBytes),
                         Mesh.end());

        if (Scene.size() < sizeof(Smile::SSceneHeader)) return false;
        std::memcpy(&_Out.SceneHeader, Scene.data(), sizeof(Smile::SSceneHeader));
        const size_t MatBytes = sizeof(Smile::SSceneMaterial) * _Out.SceneHeader.MaterialCount;
        const size_t RenBytes = sizeof(Smile::SSceneRenderable) * _Out.SceneHeader.RenderableCount;
        if (Scene.size() < sizeof(Smile::SSceneHeader) + MatBytes + RenBytes) return false;
        _Out.Materials.resize(_Out.SceneHeader.MaterialCount);
        _Out.Renderables.resize(_Out.SceneHeader.RenderableCount);
        if (MatBytes > 0)
            std::memcpy(_Out.Materials.data(), Scene.data() + sizeof(Smile::SSceneHeader), MatBytes);
        if (RenBytes > 0)
            std::memcpy(_Out.Renderables.data(),
                        Scene.data() + sizeof(Smile::SSceneHeader) + MatBytes, RenBytes);
        return true;
    }

    fs::path TempRoot() {
        static const fs::path Root = fs::temp_directory_path() / "SmileGltfImportTests";
        return Root;
    }

    // Cozinha bytes de entrada e devolve o cozido lido de volta. _Ok diz se o Cooker aceitou.
    FCookedRead CookBytes(const std::string& _Name, const std::vector<uint8_t>& _Bytes,
                          const SmileCook::FGltfOptions& _Options, bool& _Ok, std::string& _Error,
                          const char* _Extension = ".glb") {
        const fs::path Dir = TempRoot() / _Name;
        std::error_code Ec;
        fs::remove_all(Dir, Ec);
        fs::create_directories(Dir);
        const fs::path In      = Dir / (std::string("asset") + _Extension);
        const fs::path OutBase = Dir / "asset";
        WriteBytes(In, _Bytes);

        _Error.clear();
        _Ok = SmileCook::CookGltf(In, OutBase, _Options, _Error);
        FCookedRead Cooked;
        if (_Ok) _Ok = ReadCooked(OutBase, Cooked);
        return Cooked;
    }

    // --- fixtures ------------------------------------------------------------------------------
    // Triangulo com Z variando (para provar a negacao de Z), UV assimetrica (para provar que V
    // NAO e invertido) e indices em ushort.
    //
    // A ordem dos vertices e escolhida para que a normal de FACE em espaco glTF (cross das
    // arestas, CCW) seja +Y, igual as normais de vertice declaradas: com um triangulo cuja face
    // contradiz os proprios vertices, os testes de normal nao provariam nada.
    std::vector<uint8_t> TriangleBin() {
        std::vector<uint8_t> Bin;
        AppendFloats(Bin, { 0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f }); // POSITION
        AppendFloats(Bin, { 0.0f, 1.0f, 0.0f,   0.0f, 1.0f, 0.0f,   0.0f, 1.0f, 0.0f }); // NORMAL
        AppendFloats(Bin, { 0.0f, 0.0f,   1.0f, 0.0f,   0.25f, 0.75f });                 // TEXCOORD_0
        AppendU16s(Bin, { 0, 1, 2 });
        PadTo4(Bin, 0);
        return Bin;
    }

    // byteOffsets do TriangleBin: pos=0 (36 B), normal=36 (36 B), uv=72 (24 B), indices=96 (6 B).
    const char* kTriangleViews =
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],";

    std::string TriangleJson(const std::string& _Nodes, const std::string& _SceneNodes,
                             const std::string& _Extra = "",
                             const std::string& _PrimitiveExtra = "") {
        std::string Json = "{\"asset\":{\"version\":\"2.0\"},";
        Json += "\"buffers\":[{\"byteLength\":104}],";
        Json += kTriangleViews;
        Json += "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":"
                "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3" + _PrimitiveExtra + "}]}],";
        Json += "\"nodes\":[" + _Nodes + "],";
        Json += "\"scenes\":[{\"nodes\":[" + _SceneNodes + "]}],\"scene\":0";
        Json += _Extra;
        Json += "}";
        return Json;
    }

    // --- testes --------------------------------------------------------------------------------
    void TestGeometryConversion() {
        std::cout << "[glTF] conversao de geometria RH->LH, UV e payload de RT\n";
        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const FCookedRead Cooked = CookBytes(
            "geometry", BuildGlb(TriangleJson("{\"mesh\":0}", "0"), TriangleBin()),
            Options, Ok, Error);
        Check(Ok, "cozimento do triangulo falhou: " + Error);
        if (!Ok) return;

        Check(Cooked.MeshHeader.Magic == Smile::kSMeshMagic, "magic do .smesh errado");
        Check(Cooked.MeshHeader.Version == Smile::kCookedVersion, "versao do .smesh errada");
        Check(Cooked.SceneHeader.Magic == Smile::kSSceneMagic, "magic do .sscene errado");
        Check(Cooked.Entries.size() == 1, "deveria sair exatamente 1 mesh");
        Check(Cooked.Renderables.size() == 1, "deveria sair exatamente 1 renderavel");
        if (Cooked.Entries.empty()) return;

        const Smile::SMeshEntry& Entry = Cooked.Entries[0];
        Check(Entry.VertexCount == 3, "o weld nao deveria unir vertices distintos");
        Check(Entry.IndexCount == 3, "contagem de indices errada");
        Check(Entry.RTTriangleCount == 1, "payload de RT deveria ter 1 triangulo");

        const Smile::Vertex* V = Cooked.VerticesOf(0);
        // Z negado, X e Y intactos.
        Check(Near(V[1].Position[2], -1.0f), "Z nao foi negado na conversao RH->LH");
        Check(Near(V[2].Position[0], 1.0f) && Near(V[2].Position[1], 0.0f), "X/Y foram alterados");
        // V NAO e invertido: o glTF ja poe a origem da UV no canto superior esquerdo, como o D3D.
        Check(Near(V[2].TexCoord[0], 0.25f) && Near(V[2].TexCoord[1], 0.75f),
              "a UV do glTF nao deveria ser invertida em V");
        Check(Near(V[0].Normal[1], 1.0f), "normal alterada indevidamente");

        // Winding invertido, par obrigatorio da negacao de Z.
        const uint32_t* I = Cooked.IndicesOf(0);
        Check(I[0] == 0 && I[1] == 2 && I[2] == 1, "o winding nao foi invertido");

        // A normal de FACE cozida tem de sair do IB final: a negacao de Z espelha a face e a
        // inversao do winding a desespelha de volta, entao ela continua do mesmo lado das normais
        // de vertice (+Y). Gerar o payload antes do winding deixaria as duas opostas.
        const Smile::FRTTriangle& RT = Cooked.RTTrianglesOf(0)[0];
        Check((RT.Flags & Smile::kRTTriFaceNormalValid) != 0, "normal de face marcada invalida");
        Check(RT.FaceNormalOct == RT.VertexNormalOct[0],
              "normal de face diverge da de vertice: payload de RT gerado antes do winding");

        Check(Near(Entry.AABBMin[2], -1.0f) && Near(Entry.AABBMax[2], 0.0f),
              "AABB local nao acompanhou a negacao de Z");
    }

    void TestNodeTransformAndInstancing() {
        std::cout << "[glTF] TRS do no, hierarquia e dedup por (mesh, primitiva)\n";
        // Dois nos apontando para a MESMA malha; o segundo e filho do primeiro.
        const std::string Nodes =
            "{\"mesh\":0,\"translation\":[1,2,3],\"scale\":[2,2,2],\"children\":[1]},"
            "{\"mesh\":0,\"translation\":[0,1,0]}";
        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const FCookedRead Cooked = CookBytes(
            "transform", BuildGlb(TriangleJson(Nodes, "0"), TriangleBin()), Options, Ok, Error);
        Check(Ok, "cozimento com hierarquia falhou: " + Error);
        if (!Ok || Cooked.Renderables.size() < 2) {
            Check(false, "esperados 2 renderaveis");
            return;
        }

        Check(Cooked.Entries.size() == 1, "a mesma malha deveria ser deduplicada em 1 geometria");
        Check(Cooked.Renderables[0].MeshIndex == Cooked.Renderables[1].MeshIndex,
              "as duas instancias deveriam apontar para a mesma geometria");

        const Smile::SSceneRenderable& Root = Cooked.Renderables[0];
        Check(Near(Root.Position[0], 1.0f) && Near(Root.Position[1], 2.0f),
              "translacao X/Y do no nao chegou ao renderavel");
        Check(Near(Root.Position[2], -3.0f), "a translacao em Z deveria ser negada (RH->LH)");
        Check(Near(Root.Scale[0], 2.0f) && Near(Root.Scale[2], 2.0f), "escala do no perdida");

        // Filho: o transform gravado e de MUNDO (v7), entao acumula o do pai — (0,1,0) escalado
        // por 2 e somado a (1,2,-3).
        const Smile::SSceneRenderable& Child = Cooked.Renderables[1];
        Check(Near(Child.Position[0], 1.0f) && Near(Child.Position[1], 4.0f) &&
              Near(Child.Position[2], -3.0f),
              "o transform do filho nao acumulou o do pai");
        Check(Child.ParentIndex == 0, "ParentIndex do filho deveria apontar para a raiz");
        Check(Root.ParentIndex == -1, "a raiz nao deveria ter pai");
    }

    void TestFlatShadingWithoutNormals() {
        std::cout << "[glTF] primitiva sem NORMAL cai em flat shading\n";
        std::string Json = "{\"asset\":{\"version\":\"2.0\"},";
        Json += "\"buffers\":[{\"byteLength\":104}],";
        Json += kTriangleViews;
        Json += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":3}]}],";
        Json += "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const FCookedRead Cooked = CookBytes("flat", BuildGlb(Json, TriangleBin()), Options, Ok, Error);
        Check(Ok, "cozimento sem NORMAL falhou: " + Error);
        if (!Ok || Cooked.Entries.empty()) return;

        const Smile::Vertex* V = Cooked.VerticesOf(0);
        // Triangulo (0,0,0)-(0,0,1)-(1,0,0) em RH tem normal de face +Y; a negacao de Z mais a
        // inversao do winding preservam esse lado.
        for (int I = 0; I < 3; ++I)
            Check(Near(V[I].Normal[1], 1.0f, 1.0e-4f) && Near(V[I].Normal[0], 0.0f, 1.0e-4f),
                  "normal de face gerada aponta para o lado errado");
        Check(Cooked.Entries[0].VertexCount == 3, "flat shading nao deveria colapsar os vertices");
    }

    // Atributos INTERCALADOS num unico bufferView (byteStride), que e como boa parte dos
    // exportadores emite. O caminho packed e o intercalado tem de produzir a mesma malha.
    void TestInterleavedAttributes() {
        std::cout << "[glTF] atributos intercalados (byteStride)\n";
        std::vector<uint8_t> Bin;
        // 3 vertices de 32 B: pos3 + normal3 + uv2, a mesma ordem do Vertex da engine.
        AppendFloats(Bin, { 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f });
        AppendFloats(Bin, { 0.0f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f });
        AppendFloats(Bin, { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.25f, 0.75f });
        const size_t IndexOffset = Bin.size();
        AppendU16s(Bin, { 0, 1, 2 });
        PadTo4(Bin, 0);

        std::string Json = "{\"asset\":{\"version\":\"2.0\"},";
        Json += "\"buffers\":[{\"byteLength\":" + std::to_string(Bin.size()) + "}],";
        Json += "\"bufferViews\":["
                "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":96,\"byteStride\":32},"
                "{\"buffer\":0,\"byteOffset\":" + std::to_string(IndexOffset) +
                ",\"byteLength\":6}],";
        Json += "\"accessors\":["
                "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":0,\"byteOffset\":24,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
                "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],";
        Json += "\"meshes\":[{\"primitives\":[{\"attributes\":"
                "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3}]}],";
        Json += "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const FCookedRead Cooked = CookBytes("interleaved", BuildGlb(Json, Bin), Options, Ok, Error);
        Check(Ok, "cozimento intercalado falhou: " + Error);
        if (!Ok || Cooked.Entries.empty()) return;
        const Smile::Vertex* V = Cooked.VerticesOf(0);
        Check(Near(V[1].Position[2], -1.0f), "byteStride ignorado na posicao");
        Check(Near(V[2].Position[0], 1.0f), "byteStride ignorado na posicao");
        Check(Near(V[0].Normal[1], 1.0f), "byteStride ignorado na normal");
        Check(Near(V[2].TexCoord[0], 0.25f) && Near(V[2].TexCoord[1], 0.75f),
              "byteStride ignorado na UV");
    }

    void TestMaterialAndEmbeddedTexture() {
        std::cout << "[glTF] material PBR e extracao de textura embutida\n";
        // PNG 1x1 valido: o Cooker nao decodifica, mas o arquivo escrito tem de ser o mesmo que
        // entrou — e no runtime quem le e o WIC.
        const std::vector<uint8_t> Png = {
            0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0,0,0,0x0D,'I','H','D','R',
            0,0,0,1, 0,0,0,1, 8,6,0,0,0, 0x1F,0x15,0xC4,0x89,
            0,0,0,0x0A,'I','D','A','T', 0x78,0x9C,0x63,0,1,0,0,5,0,1,
            0x0D,0x0A,0x2D,0xB4, 0,0,0,0,'I','E','N','D',0xAE,0x42,0x60,0x82
        };
        std::vector<uint8_t> Bin = TriangleBin();
        const size_t PngOffset = Bin.size();
        Bin.insert(Bin.end(), Png.begin(), Png.end());
        PadTo4(Bin, 0);

        std::string Json = "{\"asset\":{\"version\":\"2.0\"},";
        Json += "\"buffers\":[{\"byteLength\":" + std::to_string(Bin.size()) + "}],";
        Json += "\"bufferViews\":["
                "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
                "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6},"
                "{\"buffer\":0,\"byteOffset\":" + std::to_string(PngOffset) +
                ",\"byteLength\":" + std::to_string(Png.size()) + "}],";
        Json += "\"accessors\":["
                "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
                "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],";
        Json += "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}],";
        Json += "\"textures\":[{\"source\":0}],";
        Json += "\"materials\":[{\"name\":\"Pedra\",\"doubleSided\":true,\"alphaMode\":\"MASK\","
                "\"alphaCutoff\":0.25,\"emissiveFactor\":[1,0,0],"
                "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.6,0.7,0.4],"
                "\"metallicFactor\":0.25,\"roughnessFactor\":0.75,"
                "\"baseColorTexture\":{\"index\":0}}}],";
        Json += "\"meshes\":[{\"primitives\":[{\"attributes\":"
                "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],";
        Json += "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const FCookedRead Cooked = CookBytes("material", BuildGlb(Json, Bin), Options, Ok, Error);
        Check(Ok, "cozimento com material falhou: " + Error);
        if (!Ok || Cooked.Materials.empty()) { Check(false, "nenhum material cozido"); return; }

        const Smile::SSceneMaterial& M = Cooked.Materials[0];
        Check(std::string(M.Name) == "Pedra", "nome do material perdido");
        Check(Near(M.BaseColorFactor[0], 0.5f) && Near(M.BaseColorFactor[3], 0.4f),
              "baseColorFactor nao chegou ao cozido");
        Check(Near(M.MetallicFactor, 0.25f) && Near(M.RoughnessFactor, 0.75f),
              "fatores metallic/roughness errados");
        Check(M.TwoSided == 1u, "doubleSided deveria virar TwoSided");
        Check(M.AlphaTest == 1u && Near(M.AlphaCutoff, 0.25f), "alphaMode MASK nao virou AlphaTest");
        Check(M.Blend == 0u, "MASK e BLEND sao mutuamente exclusivos");
        Check(Near(M.EmissiveFactor[0], 1.0f), "emissiveFactor perdido");

        const std::string BaseColor(M.BaseColor);
        Check(BaseColor == "Textures/asset_img0.png",
              "caminho da textura extraida inesperado: " + BaseColor);
        const fs::path Written = TempRoot() / "material" / "Textures" / "asset_img0.png";
        Check(fs::exists(Written), "a imagem embutida nao foi escrita em Textures/");
        std::vector<uint8_t> Roundtrip;
        Check(ReadAll(Written, Roundtrip) && Roundtrip == Png,
              "os bytes da imagem extraida diferem do embutido");
    }

    void TestOpaqueAlphaIsForced() {
        std::cout << "[glTF] alphaMode OPAQUE zera o alpha do fator\n";
        const std::string Extra =
            ",\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,0.3]}}]";
        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const FCookedRead Cooked = CookBytes(
            "opaque", BuildGlb(TriangleJson("{\"mesh\":0}", "0", Extra, ",\"material\":0"),
                               TriangleBin()), Options, Ok, Error);
        Check(Ok, "cozimento OPAQUE falhou: " + Error);
        if (!Ok || Cooked.Materials.empty()) return;
        Check(Near(Cooked.Materials[0].BaseColorFactor[3], 1.0f),
              "OPAQUE deveria ignorar o alpha (senao o material cai no passe forward)");
        Check(Cooked.Materials[0].Blend == 0u, "OPAQUE nao pode virar Blend");
    }

    void TestNormalization() {
        std::cout << "[glTF] normalizacao de escala e pivot\n";
        // Triangulo de 1 m em X e Z, com o no deslocado para longe da origem.
        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        Options.Normalize.FitSize     = 4.0f;
        Options.Normalize.DropToGround = true;
        Options.Normalize.CenterXZ     = true;
        const FCookedRead Cooked = CookBytes(
            "normalize",
            BuildGlb(TriangleJson("{\"mesh\":0,\"translation\":[10,5,0]}", "0"), TriangleBin()),
            Options, Ok, Error);
        Check(Ok, "cozimento normalizado falhou: " + Error);
        if (!Ok || Cooked.Renderables.empty()) return;

        // Maior dimensao original = 1 m (X e Z) -> escala x4.
        const Smile::SSceneRenderable& R = Cooked.Renderables[0];
        Check(Near(R.Scale[0], 4.0f), "a escala de --fit nao foi aplicada");

        // Com a geometria intacta, o AABB de mundo tem de ficar centrado em X/Z e apoiado em Y=0.
        SmileCook::FCookedScene Scene;
        Scene.Entries     = Cooked.Entries;
        Scene.Renderables = Cooked.Renderables;
        float Min[3], Max[3];
        Check(SmileCook::ComputeSceneBounds(Scene, Min, Max), "cena sem bounds apos normalizar");
        Check(Near(Min[1], 0.0f, 1.0e-4f), "--ground nao encostou a cena em Y=0");
        Check(Near(0.5f * (Min[0] + Max[0]), 0.0f, 1.0e-4f), "--center nao centralizou em X");
        Check(Near(0.5f * (Min[2] + Max[2]), 0.0f, 1.0e-4f), "--center nao centralizou em Z");
        Check(Near(Max[0] - Min[0], 4.0f, 1.0e-4f), "--fit nao levou a maior dimensao a 4 m");
    }

    void TestGltfJsonWithDataUri() {
        std::cout << "[glTF] .gltf de texto com buffer em data: URI\n";
        // Mesmo triangulo, agora com o buffer inteiro em base64 dentro do JSON.
        const std::vector<uint8_t> Bin = TriangleBin();
        static const char* kBase64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string Encoded;
        for (size_t I = 0; I < Bin.size(); I += 3) {
            const uint32_t B0 = Bin[I];
            const uint32_t B1 = (I + 1 < Bin.size()) ? Bin[I + 1] : 0u;
            const uint32_t B2 = (I + 2 < Bin.size()) ? Bin[I + 2] : 0u;
            const uint32_t Triple = (B0 << 16) | (B1 << 8) | B2;
            Encoded += kBase64[(Triple >> 18) & 0x3F];
            Encoded += kBase64[(Triple >> 12) & 0x3F];
            Encoded += (I + 1 < Bin.size()) ? kBase64[(Triple >> 6) & 0x3F] : '=';
            Encoded += (I + 2 < Bin.size()) ? kBase64[Triple & 0x3F] : '=';
        }

        std::string Json = "{\"asset\":{\"version\":\"2.0\"},";
        Json += "\"buffers\":[{\"byteLength\":" + std::to_string(Bin.size()) +
                ",\"uri\":\"data:application/octet-stream;base64," + Encoded + "\"}],";
        Json += kTriangleViews;
        Json += "\"meshes\":[{\"primitives\":[{\"attributes\":"
                "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3}]}],";
        Json += "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

        bool Ok = false;
        std::string Error;
        SmileCook::FGltfOptions Options;
        const std::vector<uint8_t> Bytes(Json.begin(), Json.end());
        const FCookedRead Cooked = CookBytes("datauri", Bytes, Options, Ok, Error, ".gltf");
        Check(Ok, "cozimento do .gltf com data: URI falhou: " + Error);
        if (!Ok || Cooked.Entries.empty()) return;
        Check(Cooked.Entries[0].VertexCount == 3, "geometria do data: URI diverge");
        const Smile::Vertex* V = Cooked.VerticesOf(0);
        Check(Near(V[1].Position[2], -1.0f), "data: URI decodificado errado");
    }

    // Entrada que o Cooker TEM de recusar em vez de cozinhar pela metade.
    void TestRejections() {
        std::cout << "[glTF] entradas invalidas sao recusadas\n";
        SmileCook::FGltfOptions Options;
        bool Ok = false;
        std::string Error;

        CookBytes("draco",
                  BuildGlb(TriangleJson("{\"mesh\":0}", "0",
                                        ",\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]"),
                           TriangleBin()),
                  Options, Ok, Error);
        Check(!Ok && Error.find("KHR_draco") != std::string::npos,
              "malha comprimida com Draco deveria ser recusada com mensagem clara");

        CookBytes("sparse",
                  BuildGlb(TriangleJson("{\"mesh\":0}", "0").replace(
                               TriangleJson("{\"mesh\":0}", "0").find("\"count\":3,\"type\":\"VEC3\"}"),
                               std::strlen("\"count\":3,\"type\":\"VEC3\"}"),
                               "\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":1}}"),
                           TriangleBin()),
                  Options, Ok, Error);
        Check(!Ok && Error.find("sparse") != std::string::npos,
              "accessor sparse deveria ser recusado");

        // Accessor que le alem do bufferView (count inflado).
        std::string Overrun = TriangleJson("{\"mesh\":0}", "0");
        const std::string Needle = "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}";
        Overrun.replace(Overrun.find(Needle), Needle.size(),
                        "{\"bufferView\":0,\"componentType\":5126,\"count\":300,\"type\":\"VEC3\"}");
        CookBytes("overrun", BuildGlb(Overrun, TriangleBin()), Options, Ok, Error);
        Check(!Ok && Error.find("alem do bufferView") != std::string::npos,
              "accessor lendo alem do bufferView deveria ser recusado");

        // GLB truncado no meio do chunk.
        std::vector<uint8_t> Truncated = BuildGlb(TriangleJson("{\"mesh\":0}", "0"), TriangleBin());
        Truncated.resize(Truncated.size() / 2);
        CookBytes("truncated", Truncated, Options, Ok, Error);
        Check(!Ok, "GLB truncado deveria ser recusado");

        // Indice de vertice fora da faixa.
        std::vector<uint8_t> BadIndexBin = TriangleBin();
        BadIndexBin[96] = 0xFF; BadIndexBin[97] = 0x00; // indice 255 num vertex buffer de 3
        CookBytes("badindex", BuildGlb(TriangleJson("{\"mesh\":0}", "0"), BadIndexBin),
                  Options, Ok, Error);
        Check(!Ok && Error.find("indice fora da faixa") != std::string::npos,
              "indice fora da faixa deveria ser recusado");

        // Cena sem nenhuma primitiva de triangulo (modo 0 = POINTS).
        CookBytes("points",
                  BuildGlb(TriangleJson("{\"mesh\":0}", "0", "", ",\"mode\":0"), TriangleBin()),
                  Options, Ok, Error);
        Check(!Ok, "cena so com pontos nao deveria produzir cozido");
    }
}

int main() {
    std::error_code Ec;
    fs::remove_all(TempRoot(), Ec);

    TestGeometryConversion();
    TestNodeTransformAndInstancing();
    TestFlatShadingWithoutNormals();
    TestInterleavedAttributes();
    TestMaterialAndEmbeddedTexture();
    TestOpaqueAlphaIsForced();
    TestNormalization();
    TestGltfJsonWithDataUri();
    TestRejections();

    if (Failures != 0) {
        std::cerr << Failures << " glTF import assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All glTF import diagnostics passed.\n";
    fs::remove_all(TempRoot(), Ec);
    return 0;
}
