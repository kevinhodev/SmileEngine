// PS do passe de picking (estilo hit-proxy da Unreal): emite o ID do renderavel sob o pixel
// num RT R32_UINT. O ID vem de uma root 32-bit constant setada por draw. O Renderer grava
// (indiceDoRenderavel + 1); 0 fica reservado p/ "vazio" (limpo no clear), entao o readback
// devolve indice = valor - 1 (-1 = nada). O teste depth-EQUAL (no PSO) garante que so o
// fragmento da superficie visivel — a mesma que escreveu o depth no passe principal — passa,
// inclusive recortando corretamente a folhagem alpha-masked (os furos das folhas nunca
// escreveram depth, entao selecionam o objeto atras).
cbuffer IDConstants : register(b0) {
    uint ObjectId;
    uint3 _pad;
};

uint main() : SV_TARGET {
    return ObjectId;
}
