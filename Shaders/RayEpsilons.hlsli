#ifndef SMILE_RAY_EPSILONS_HLSLI
#define SMILE_RAY_EPSILONS_HLSLI

// Epsilons dos raios de GI / reflexao que continuam sendo COMPILE-TIME, em METROS.
//
// Os nove que viraram knobs de calibracao (pagina "Iluminacao global") NAO estao mais aqui: vem
// pelo constant buffer em RayEpsA/RayEpsB, definidos pelo FRayEpsilonProfile em
// Engine/Include/Smile/Graphics/RayEpsilons.h. Sao eles: originFloorMin, originFloorPerMeter,
// bias angular, hitShadowRayBias (via TraceParams.w), shadowRayBiasMin, shadowRayTMin,
// visRayTMin, visRayEndMargin e maxAge (via JitterParams.w).
//
// TRES FAMILIAS — NAO calibrar juntas, cobrem fenomenos diferentes:
//   (1) ORIGEM        desloca o ponto de partida p/ fora da superficie (toda no CB agora)
//   (2) INTERVALO     TMin / margem do TMax / comprimento minimo do segmento
//   (3) CORRESPONDENCIA  tolerancia de identidade temporal do re-trace — NAO e epsilon
//                     anti-self-hit, e criterio de "acertou o mesmo ponto"
//
// O que sobrou aqui e o que ainda nao entrou no sweep. Ao mover algum destes p/ o CB, lembrar de
// mover TAMBEM o default correspondente no FRayEpsilonProfile — os dois lados tem que andar
// juntos, que foi o bug que motivou a centralizacao.

// --- (2) INTERVALO — segmento ate as luzes puntuais ------------------------------------------
// Ainda constantes: a revisao pediu p/ tratar a margem de 5 cm das puntuais separadamente dos
// shadow rays do sol, porque ela recorta o segmento no lado da LUZ, nao no da superficie.
#define kLightRayEndMargin      0.05f  // para antes da luz
#define kLightRayMinTMax        0.02f  // piso do TMax p/ luz muito proxima

// --- (3) CORRESPONDENCIA ----------------------------------------------------------------------
// So entram em jogo com ValidateInterval > 0, que hoje esta desligado. Quando a revalidacao
// periodica for religada (8, ou os 6 frames do paper), estes NUNCA foram calibrados contra nada
// e precisam de A/B proprio — em especial depois de o offset de origem cair de 20 cm.
#define kRevalidateRelTol       0.02f
#define kRevalidateAbsTol       0.05f
#define kRevalidateSkyFrac      0.98f  // fracao do TMax acima da qual a amostra "era ceu"

#endif
