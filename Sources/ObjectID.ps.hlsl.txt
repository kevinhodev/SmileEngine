cbuffer IDConstants : register(b0) {
    uint ObjectId;
    uint3 _pad;
};

uint main() : SV_TARGET {
    return ObjectId;
}
