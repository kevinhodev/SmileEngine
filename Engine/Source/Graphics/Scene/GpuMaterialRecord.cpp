#include "Smile/Graphics/Scene/GpuMaterialRecord.h"

namespace Smile {
    FGpuMaterialRecord FGpuMaterialRecord::FromParts(const MaterialConstants& _C,
                                                     bool _TwoSidedForRT, bool _Blend,
                                                     u32 _TextureBase) {
        FGpuMaterialRecord R{};
        R.BaseColor       = _C.BaseColorFactor;
        R.AlphaCutoff     = _C.AlphaCutoff;
        R.RoughnessFactor = _C.RoughnessFactor;
        // Mesmo dado do G-buffer: tint RGB + intensidade em A. Folhagem importada recebe 0.6 no
        // loader; materiais editados preservam o valor autorado no RT.
        R.SubsurfaceColor = _C.SubsurfaceColor;

        // O RTEmissiveScale entra SO aqui: esta linha e o que os hits de RT leem (DDGI, ReSTIR GI
        // e reflexoes). O raster segue com o EmissiveStrength puro, entao baixar a escala tira a
        // malha de iluminar o ambiente sem apagar o brilho dela na tela.
        const f32 EmiRT = _C.EmissiveStrength * _C.RTEmissiveScale;
        R.EmissiveFactor = { _C.EmissiveFactor.X * EmiRT,
                             _C.EmissiveFactor.Y * EmiRT,
                             _C.EmissiveFactor.Z * EmiRT,
                             _C.MetallicFactor };

        // Mesmo criterio da flag de culling da TLAS (FMaterial::IsTwoSidedForRT): este bit e o
        // que decide, no shader, se um hit pelo verso significa "dentro de solido" — tem que
        // concordar com quem o raio consegue enxergar pelo verso.
        if (_TwoSidedForRT)         R.Flags |= kMaterialFlagTwoSided;
        if (_C.AlphaTest)           R.Flags |= kMaterialFlagAlphaTest;
        if (_C.ShadingModel == 1u)  R.Flags |= kMaterialFlagFoliage;
        // Categoria da instancia na TLAS (kRTMaskTranslucent). Espelhada aqui porque a mask e
        // propriedade do RAIO: um shader nao consegue perguntar a TLAS com que mask a instancia
        // entrou. Consumida so pelo BvhDebug — os outros passes distinguem translucido pela mask
        // que eles proprios tracam.
        if (_Blend)                 R.Flags |= kMaterialFlagTranslucent;

        // ---- Bloco de raster (Fase 3) --------------------------------------------------------
        //
        // Fica ANTES do early-return da tabela de descritores porque nenhum destes campos depende
        // dela: sao fatores e passos do MaterialCB, validos com ou sem textura. So os tres BITS
        // de mapa deste bloco (normal, AO, height) descem para o outro lado.
        //
        // O emissivo do raster e o produto SEM o RTEmissiveScale — a divergencia deliberada
        // descrita no MaterialConstants. Guardar os dois produtos, e nao fator + escala, evita que
        // o shader repita a multiplicacao por pixel.
        R.RasterEmissive = { _C.EmissiveFactor.X * _C.EmissiveStrength,
                             _C.EmissiveFactor.Y * _C.EmissiveStrength,
                             _C.EmissiveFactor.Z * _C.EmissiveStrength,
                             _C.AOStrength };
        R.NormalStrength      = _C.NormalStrength;
        R.HeightScale         = _C.HeightScale;
        R.ParallaxMinSteps    = _C.ParallaxMinSteps;
        R.ParallaxMaxSteps    = _C.ParallaxMaxSteps;
        R.ParallaxFadeStart   = _C.ParallaxFadeStart;
        R.ParallaxFadeRange   = _C.ParallaxFadeRange;
        R.ParallaxRefineSteps = _C.ParallaxRefineSteps;
        R.ShadingModel        = _C.ShadingModel;

        if (_C.NormalFlipY)        R.Flags |= kMaterialFlagNormalFlipY;
        if (_C.NormalReconstructZ) R.Flags |= kMaterialFlagNormalReconZ;
        if (_C.ParallaxRefine)     R.Flags |= kMaterialFlagParallaxRefine;

        // Sem tabela de descritores nao ha mapa nenhum: ligar o bit aqui faria o shader somar um
        // deslocamento a uma base invalida e amostrar um descritor de outro dono.
        if (_TextureBase == kInvalidTextureBase) return R;

        R.TextureBase = _TextureBase;
        if (_C.HasAlbedoMap)   R.Flags |= kMaterialFlagAlbedoMap;
        if (_C.HasEmissiveMap) R.Flags |= kMaterialFlagEmissiveMap;
        if (_C.HasMetallicRoughnessMap) {
            R.Flags |= kMaterialFlagMRMap;
            // Empacotamento do mapa: "Specular" poe metal em B, glTF poe em R. Sem este bit o RT
            // leria o canal errado e um material viraria metal (ou dieletrico) por engano no
            // bounce. Fica sob o bit do mapa porque sem mapa ele nao descreve nada.
            if (_C.SpecularPacking) R.Flags |= kMaterialFlagSpecPacked;
        }
        // Mapa Metalness SEPARADO. Precisa existir porque o loader deixa MetallicFactor = 1
        // quando ha mapa — sem enxergar o mapa, o RT leria "metal puro" e zeraria o difuso de um
        // material que e quase todo dieletrico.
        if (_C.HasMetalnessMap) R.Flags |= kMaterialFlagMetalMap;
        if (_C.HasRoughnessMap) R.Flags |= kMaterialFlagRoughMap;
        // Os tres mapas que so o raster consulta. Mesma regra dos de cima: sob a base valida,
        // porque um bit ligado com base invalida faria o shader amostrar descritor de outro dono.
        if (_C.HasNormalMap)    R.Flags |= kMaterialFlagNormalMap;
        if (_C.HasAOMap)        R.Flags |= kMaterialFlagAOMap;
        if (_C.HasHeightMap)    R.Flags |= kMaterialFlagHeightMap;
        return R;
    }

    FGpuMaterialRecord FGpuMaterialRecord::FromMaterial(const FMaterial* _Material) {
        if (!_Material) return FGpuMaterialRecord{};
        // IsFinalized() e a condicao que o snapshot antigo usava para consultar indice de
        // descritor. A comparacao extra com kInvalidTextureBase cobre o caso em que o heap
        // recusou o range: ali o snapshot antigo gravava 0xFFFFFFFF com o bit LIGADO, e o shader
        // amostrava lixo. Aqui o bit fica desligado.
        const u32 Base = _Material->IsFinalized() ? _Material->AlbedoDescriptorIndex()
                                                  : kInvalidTextureBase;
        return FromParts(_Material->Constants, _Material->IsTwoSidedForRT(), _Material->Blend,
                         Base);
    }
}
