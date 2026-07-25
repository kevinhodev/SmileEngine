#ifndef SMILE_RAY_EPSILONS_HLSLI
#define SMILE_RAY_EPSILONS_HLSLI

// Perfil de epsilons dos raios de GI / reflexao, em METROS (unidade de mundo da engine).
//
// Centralizado para que a calibracao mexa num lugar so: o sweep do termo relativo e o A/B do
// bias angular (estilo Lumen) precisam variar um eixo de cada vez, e antes disto os valores
// estavam espalhados por quatro arquivos como literais soltos.
//
// TRES FAMILIAS — NAO calibrar juntas, elas cobrem fenomenos diferentes:
//
//   (1) ORIGEM        Desloca o ponto de partida p/ fora da superficie. Cobre self-hit
//                     numerico (a parte ULP fica na OffsetRayWB), erro de reconstrucao do
//                     G-buffer, mismatch da shading normal e geometria coplanar/embutida.
//   (2) INTERVALO     TMin / margem do TMax / comprimento minimo do segmento. Recorta o raio
//                     p/ nao bater na superficie de partida nem na de chegada.
//   (3) CORRESPONDENCIA  Tolerancia p/ decidir se um re-trace acertou o MESMO ponto de antes.
//                     NAO e epsilon anti-self-hit — e criterio de identidade temporal.
//
// Referencia externa (Unreal 5.7): o gather ReSTIR do Lumen usa 0,1-0,5 mm modulados por
// dot(N, rayDir) (ApplyPositionBias em RayTracingCommon.ush) e NAO tem termo escalado por
// distancia — ele opera em coordenadas camera-relative. A Smile reconstroi posicao absoluta,
// entao o termo por camDist abaixo pode ou nao ser necessario: entra no sweep como hipotese
// a MEDIR (incluindo zero), nao como constante justificada.
//
// IMPORTANTE: esta centralizacao nao mudou NENHUMA magnitude. Todos os valores abaixo sao
// exatamente os que estavam nos shaders antes dela.

// --- (1) ORIGEM ----------------------------------------------------------------------------

// Termo proporcional a distancia da camera. Heuristico: a quantizacao do depth (D32 reverse-Z,
// near 0.1 / far 4000) da ~0,003 mm a 50 m, ordens de grandeza abaixo dos 10 mm que este termo
// produz na mesma distancia. Candidato numero 1 do sweep.
#define kRayOriginFloorPerMeter 2e-4f  // ~0.2 mm por metro de distancia da camera

// MODO LEGADO: piso constante de 20 cm. Reproduz o normal-bias antigo e e a causa medida das
// manchas nas cortinas do Bistro (a origem do gather nasce fora do predio e ve ceu/rua, o
// reservoir trava na amostra brilhante). Calibrado em 2 cm no passado porque props flush/
// embutidos na casca ficavam pretos a 1 mm — sintoma de interpenetracao de conteudo, nao de
// precisao numerica. So baixar junto com a familia (2), senao os TMin passam a dominar.
#define kRayOriginFloorMin      2e-1f

// Piso do ShadowRayBias que chega pelo CB (TraceParams.w), aplicado no 2o hit.
#define kShadowRayBiasMin       1e-3f

// --- (2) INTERVALO -------------------------------------------------------------------------

// Visibility rays do reuso espacial (correcao de bias MIS e shading visibility).
#define kVisRayTMin             0.02f  // afasta da superficie de partida
#define kVisRayEndMargin        0.05f  // para antes da superficie do x2
#define kVisRayMinLength        0.15f  // < isto o teste e PULADO (garante TMax > TMin)

// Shadow rays disparados no ponto de hit (sol e luzes puntuais).
#define kShadowRayTMin          0.01f
#define kLightRayEndMargin      0.05f  // para antes da luz
#define kLightRayMinTMax        0.02f  // piso do TMax p/ luz muito proxima

// --- (3) CORRESPONDENCIA -------------------------------------------------------------------

// Revalidacao periodica da amostra temporal: o re-trace acertou o mesmo ponto se
// |t - len| <= max(kRevalidateRelTol * len, kRevalidateAbsTol).
#define kRevalidateRelTol       0.02f
#define kRevalidateAbsTol       0.05f
// Fracao do TMax acima da qual a amostra revalidada e considerada "era ceu".
#define kRevalidateSkyFrac      0.98f

#endif
