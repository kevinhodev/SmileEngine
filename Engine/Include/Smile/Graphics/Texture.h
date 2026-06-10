#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace Smile {
    class FCommandQueue;

    enum class EDefaultTexture {
        White,
        FlatNormal,
        Black,
        MidGrey,
        ORM,
    };

    // Um nivel de mip em CPU (R8G8B8A8). Publico para permitir decode em worker thread.
    struct FMipData {
        std::vector<u8> Pixels;
        u32             Width  = 0;
        u32             Height = 0;
    };

    // Resultado do decode/mipgen em CPU (sem nenhuma chamada D3D12). Movivel, pensado
    // para ser produzido em um worker thread e consumido (upload) no thread de render.
    struct FTextureCPUData {
        std::vector<FMipData> Mips;
        u32         Width   = 0;
        u32         Height  = 0;
        DXGI_FORMAT Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool        IsNormalMap = false;
        bool Valid() const { return !Mips.empty(); }
    };

    class FTexture {
    public:
        // Loads a texture from file (JPG/PNG/BMP/TIFF via WIC) and generates a
        // full mip chain via 2x2 box filtering. When IsNormalMap=true, mips are
        // built using vector-aware averaging + re-normalization, and the alpha
        // channel of each mip stores the Toksvig variance factor (T = |sum|/N).
        // T = 1.0 at mip 0 (no loss) and < 1.0 at higher mips where normals
        // disagreed within the source 2x2 footprint. The PS reads .a and bumps
        // roughness accordingly to suppress specular aliasing.
        static FTexture LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::wstring& Path,
                                     bool IsNormalMap = false);

        // Etapa 1 (CPU, sem D3D12): decode WIC + geracao de mips. SEGURA para rodar
        // fora do thread de render (worker). Em falha, retorna dados invalidos (Valid()==false)
        // sem lancar — exceptions nao devem cruzar a fronteira do thread.
        static FTextureCPUData LoadCPU(const std::wstring& Path, bool IsNormalMap = false);

        // Etapa 2 (D3D12, thread de render): sobe os dados CPU para a GPU e cria a textura.
        // Dados invalidos => textura invalida (IsValid()==false).
        static FTexture CreateFromCPU(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      const FTextureCPUData& Data);

        // Carrega uma textura DDS block-compressed (BC1/BC2/BC3/BC4/BC5 via FourCC, ou
        // qualquer DXGI via header DX10). A cadeia de mips do arquivo eh usada como esta
        // (sem gerar/decodificar) e subida comprimida pra GPU. sRGB=true escolhe a variante
        // _UNORM_SRGB (BaseColor/Emissive); false = linear (Normal/Specular/dados).
        static FTextureCPUData LoadDDSCPU(const std::wstring& Path, bool sRGB);
        static FTexture        LoadDDS(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                       FTextureSRVHeap& SRVHeap,
                                       const std::wstring& Path, bool sRGB);

        // Sobe MUITAS texturas em lote: agrupa as copias por chunks (orcamento de staging)
        // com 1 ExecuteAndSync por chunk — bem mais rapido que CreateFromCPU por textura
        // (1 sync cada). Entradas invalidas viram FTexture invalida na mesma posicao.
        static std::vector<FTexture> CreateBatchFromCPU(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                                        FTextureSRVHeap& SRVHeap,
                                                        const std::vector<FTextureCPUData>& Data);

        static FTexture CreateDefault(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      EDefaultTexture Type);

        // Libera o slot SRV de volta para o heap e solta o recurso GPU. Idempotente.
        // Chamado quando uma textura carregada eh substituida/removida em runtime.
        void Release(FTextureSRVHeap& SRVHeap);

        ID3D12Resource* Resource()  const { return GpuResource.Get(); }
        DXGI_FORMAT     Format()    const { return TexFormat; }
        u32             SRVSlot()   const { return Slot; }
        u32             Width()     const { return TexWidth; }
        u32             Height()    const { return TexHeight; }
        u32             MipCount()  const { return TexMipCount; }
        bool            IsValid()   const { return GpuResource != nullptr; }

    private:
        // Uploads all mip levels of a single texture in one staging buffer +
        // one ExecuteAndSync. Caller owns the FMipData chain.
        static FTexture Upload(ID3D12Device* Device, FCommandQueue& CmdQueue,
                               FTextureSRVHeap& SRVHeap,
                               const std::vector<FMipData>& Mips,
                               DXGI_FORMAT Format);

        // Cria a textura (default heap), faz o staging e GRAVA as copias + barrier no
        // command list dado, aloca o SRV, mas NAO sincroniza. Staging vai em StagingOut
        // (manter vivo ate o caller executar+sincronizar). Base do upload single e batch.
        static FTexture RecordUpload(ID3D12Device* Device, ID3D12GraphicsCommandList* CommandList,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::vector<FMipData>& Mips, DXGI_FORMAT Format,
                                     std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& StagingOut);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32 Slot        = kInvalidSlot;
        u32 TexWidth    = 0;
        u32 TexHeight   = 0;
        u32 TexMipCount = 1;
        DXGI_FORMAT TexFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // formato real (RGBA8 ou BCx)
    };
}
