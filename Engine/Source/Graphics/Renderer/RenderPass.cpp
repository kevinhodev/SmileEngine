#include "Smile/Graphics/Renderer/RenderPass.h"

namespace Smile {

    void FPipelineOwner::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        (void)_Ctx;
    }

    FPassShaderStems FPipelineOwner::ShaderStems() const {
        return {};
    }

    bool FRenderPass::IsActive(const FFrameModes& _Modes) const {
        (void)_Modes;
        return IsInitialized();
    }

    void FRenderPass::OnResize(const FPassInitContext& _Ctx) {
        (void)_Ctx;
    }

    void FRenderPass::OnRegisterDebugTargets() {
    }

    EHistoryTarget FRenderPass::HistoryTargets() const {
        return EHistoryTarget::None;
    }

    void FRenderPass::OnInvalidateHistory(EHistoryTarget _Domain) {
        (void)_Domain;
    }

    void FPassRegistry::RegisterPass(FRenderPass* _Pass) {
        if (!_Pass) return;
        Passes.push_back(_Pass);
        Owners.push_back(_Pass);
    }

    void FPassRegistry::RegisterPipelineOwnerImpl(FPipelineOwner* _Owner) {
        if (_Owner) Owners.push_back(_Owner);
    }

    void FPassRegistry::Clear() {
        Passes.clear();
        Owners.clear();
    }

    std::span<FRenderPass* const> FPassRegistry::FramePasses() const {
        return Passes;
    }

    std::span<FPipelineOwner* const> FPassRegistry::PipelineOwners() const {
        return Owners;
    }

    void FPassRegistry::RecreateAllPipelines(const FPassInitContext& _Ctx) const {
        for (FPipelineOwner* Owner : Owners) Owner->OnRecreatePipelines(_Ctx);
    }

    u32 FPassRegistry::RecreatePipelinesForStem(
        std::string_view _Stem, const FPassInitContext& _Ctx) const {
        u32 Matched = 0;
        for (FPipelineOwner* Owner : Owners) {
            for (const char* Stem : Owner->ShaderStems()) {
                if (_Stem != Stem) continue;
                Owner->OnRecreatePipelines(_Ctx);
                ++Matched;
                break;
            }
        }
        return Matched;
    }

    const char* FPassRegistry::OwnerOfShaderStem(std::string_view _Stem) const {
        for (FPipelineOwner* Owner : Owners) {
            for (const char* Stem : Owner->ShaderStems()) {
                if (_Stem == Stem) return Owner->Name();
            }
        }
        return nullptr;
    }

    void FPassRegistry::ResizeAll(const FPassInitContext& _Ctx) const {
        for (FRenderPass* Pass : Passes) Pass->OnResize(_Ctx);
    }

    void FPassRegistry::RegisterDebugTargetsAll() const {
        for (FRenderPass* Pass : Passes) Pass->OnRegisterDebugTargets();
    }

    void FPassRegistry::InvalidateHistory(EHistoryTarget _Domain) const {
        for (FRenderPass* Pass : Passes) {
            if (HasTarget(_Domain, Pass->HistoryTargets())) {
                Pass->OnInvalidateHistory(_Domain);
            }
        }
    }
}
