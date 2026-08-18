#pragma once

#include <d3d12.h>

#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Renderer/HistoryDomain.h"

namespace Smile {

    class FTextureSRVHeap;
    struct FFrameModes;
    struct FSceneTargets;

    // Recursos compartilhados pelos hooks de ciclo de vida dos passes.
    struct FPassInitContext {
        ID3D12Device* Device = nullptr;
        FTextureSRVHeap* SRVHeap = nullptr;
        FSceneTargets* Targets = nullptr;
        u32 RenderWidth = 0;
        u32 RenderHeight = 0;

        DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        DXGI_FORMAT SceneDepthFormat = DXGI_FORMAT_D32_FLOAT;
        DXGI_FORMAT VelocityFormat = DXGI_FORMAT_R16G16_FLOAT;
    };

    // Nomes de .cso sem perfil ou extensao, mantidos pelo objeto que possui os PSOs.
    using FPassShaderStems = std::span<const char* const>;

    // Objeto que possui shaders/PSOs, mas nao necessariamente grava trabalho a cada frame.
    class FPipelineOwner {
    public:
        virtual ~FPipelineOwner() = default;

        virtual const char* Name() const = 0;
        virtual bool IsInitialized() const = 0;
        virtual void OnRecreatePipelines(const FPassInitContext& Ctx);
        virtual FPassShaderStems ShaderStems() const;
    };

    // Passe de frame. A gravacao permanece explicita porque cada passe possui insumos proprios.
    class FRenderPass : public FPipelineOwner {
    public:
        virtual bool IsActive(const FFrameModes& Modes) const;
        virtual void OnResize(const FPassInitContext& Ctx);
        virtual void OnRegisterDebugTargets();
        virtual EHistoryTarget HistoryTargets() const;
        virtual void OnInvalidateHistory(EHistoryTarget Domain);
    };

    // Indice nao-dono: o Renderer continua responsavel pelo tempo de vida dos objetos registrados.
    class FPassRegistry {
    public:
        void RegisterPass(FRenderPass* Pass);

        template <class T>
        void RegisterPipelineOwner(T* Owner) {
            static_assert(!std::is_base_of_v<FRenderPass, T>,
                "passes de frame devem ser registrados com RegisterPass");
            static_assert(std::is_base_of_v<FPipelineOwner, T>,
                "o objeto deve implementar FPipelineOwner");
            RegisterPipelineOwnerImpl(Owner);
        }

        void Clear();

        std::span<FRenderPass* const> FramePasses() const;
        std::span<FPipelineOwner* const> PipelineOwners() const;

        void RecreateAllPipelines(const FPassInitContext& Ctx) const;
        u32 RecreatePipelinesForStem(std::string_view Stem, const FPassInitContext& Ctx) const;
        const char* OwnerOfShaderStem(std::string_view Stem) const;

        void ResizeAll(const FPassInitContext& Ctx) const;
        void RegisterDebugTargetsAll() const;
        void InvalidateHistory(EHistoryTarget Domain) const;

    private:
        void RegisterPipelineOwnerImpl(FPipelineOwner* Owner);

        std::vector<FRenderPass*> Passes;
        std::vector<FPipelineOwner*> Owners;
    };
}
