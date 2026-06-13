// DDGI debug — PS do wireframe da AABB do volume. Cor fixa (ciano) p/ referencia visual.
struct VSOut { float4 pos : SV_POSITION; };

float4 main(VSOut i) : SV_Target {
    return float4(0.1f, 0.9f, 1.0f, 1.0f);
}
