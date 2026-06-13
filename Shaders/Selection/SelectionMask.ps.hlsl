// PS da mascara de selecao: marca o pixel coberto pelo objeto selecionado com 1.0 num RT
// R8_UNORM. O VS (SelectionGeom.vs) ja projeta a geometria pelo MVP do ObjectCB. Sem teste de
// profundidade — a mascara cobre a silhueta inteira do mesh (estilo Flax/Cry), e o edge-detect
// fullscreen desenha a borda. Resultado: outline sempre visivel (nao ocluido), como nos editores.
float main() : SV_TARGET {
    return 1.0f;
}
