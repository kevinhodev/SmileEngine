#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Core/Types.h"
#include <string>

namespace Smile {
    enum class ELightType : u32 { Point = 0, Spot = 1 };

    // Luz puntual da cena (point/spot). Unidades: Intensity = radiancia a 1 unidade de mundo
    // (inverse-square dali em diante, estilo candela). AttenuationRadius corta a contribuicao
    // a zero via janela (1-(d/r)^4)^2 (Unreal) — a luz morre exatamente no raio, o que tambem
    // limita o culling. SourceRadius e o "bulb" (Cry): piso da distancia no inverse-square,
    // superficie encostada na luz nao estoura a branco. Na F4 ele vira tambem o raio da fonte
    // p/ o especular de area (representative point).
    struct FLight {
        // Identidade ESTAVEL da luz (0 = ainda nao atribuida; o renderer atribui no primeiro
        // frame em que ve a luz). O indice na FScene::Lights() NAO serve: remover a luz 0
        // desloca todo mundo e duplicar copia o struct inteiro. Quem depende disso e o slot
        // persistente de shadow map — sem identidade, o slice era a posicao no ranking por
        // distancia e trocava sozinho quando a camera andava, o que impede cache do depth
        // entre frames e histerese/fade na troca de slot.
        u64 Id = 0;

        std::string Name = "Luz";
        ELightType  Type = ELightType::Point;

        Vec3 Position  = { 0.0f, 2.0f, 0.0f };
        Vec3 Direction = { 0.0f, -1.0f, 0.0f }; // eixo do cone (spot); ignorado no point

        Vec3 Color     = { 1.0f, 1.0f, 1.0f };
        f32  Intensity = 10.0f;

        f32  AttenuationRadius = 10.0f;
        f32  SourceRadius      = 0.05f; // bulb: distancia minima do inverse-square

        f32  InnerConeDeg = 25.0f;      // spot: onde o falloff angular comeca
        f32  OuterConeDeg = 40.0f;      // spot: cone externo (contribuicao zero)

        // Peso da luz SO no mundo indireto (GI/reflexoes/DDGI); o raster nao ve este campo.
        // Existe porque a autoria costuma pôr uma luz analitica DENTRO de uma malha emissiva
        // (luminaria, letreiro, tela) para representar a emissao dela. Com o RT, a malha passa a
        // iluminar por conta propria — os raios do gather acertam o emissivo e trazem a radiancia —,
        // e a luz analitica vira contribuicao DUPLICADA no indireto. Baixar este peso (ou zerar)
        // remove a duplicata sem tocar no direto rasterizado, que continua precisando da luz.
        // 0 tira a luz da lista do GI de vez, o que tambem economiza o hit. Ver o par do outro lado
        // em MaterialConstants::RTEmissiveScale.
        f32  RTWeight = 1.0f;

        // Caixa de MUNDO onde esta luz ainda contribui. Exata para point: o AttenuationRadius e
        // um corte DURO — a janela (1-(d/r)^4)^2 zera exatamente no raio. Para spot e
        // conservadora (o cone cabe na esfera), e conservador e o lado certo aqui: quem consome
        // isto e a invalidacao de GI por regiao, onde sobrar sonda custa alguns frames de
        // reconvergencia e faltar sonda deixa luz fantasma presa no atlas.
        void InfluenceBounds(Vec3& OutMin, Vec3& OutMax) const {
            const f32 R = AttenuationRadius > 0.0f ? AttenuationRadius : 0.0f;
            OutMin = { Position.X - R, Position.Y - R, Position.Z - R };
            OutMax = { Position.X + R, Position.Y + R, Position.Z + R };
        }

        bool Enabled = true;
        // Projeta sombras. Spot vai pro atlas 2D (budget FLocalShadows::kActiveShadows) e point
        // pro cube array (kActiveCubes) — ganham slot as de maior influencia na camera (energia
        // x area angular, com histerese), e o slot e mantido pela identidade da luz entre
        // frames. Acima do budget a luz continua iluminando, so que SEM oclusao (vaza parede).
        bool CastShadows = true;
    };
}
