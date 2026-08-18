#include <iostream>
#include <string_view>

#include "Smile/Graphics/Renderer/RenderPass.h"

namespace {
    int Failures = 0;

    void Check(bool _Condition, std::string_view _Message) {
        if (_Condition) return;
        ++Failures;
        std::cerr << "  FAIL: " << _Message << '\n';
    }

    class FTestPipelineOwner final : public Smile::FPipelineOwner {
    public:
        FTestPipelineOwner(const char* _Name, Smile::FPassShaderStems _Stems)
            : PassName(_Name), Stems(_Stems) {
        }

        const char* Name() const override { return PassName; }
        bool IsInitialized() const override { return true; }
        Smile::FPassShaderStems ShaderStems() const override { return Stems; }
        void OnRecreatePipelines(const Smile::FPassInitContext&) override { ++RecreateCount; }

        int RecreateCount = 0;

    private:
        const char* PassName;
        Smile::FPassShaderStems Stems;
    };

    class FTestRenderPass final : public Smile::FRenderPass {
    public:
        FTestRenderPass(
            const char* _Name, Smile::FPassShaderStems _Stems, Smile::EHistoryTarget _History)
            : PassName(_Name), Stems(_Stems), History(_History) {
        }

        const char* Name() const override { return PassName; }
        bool IsInitialized() const override { return true; }
        Smile::FPassShaderStems ShaderStems() const override { return Stems; }
        Smile::EHistoryTarget HistoryTargets() const override { return History; }

        void OnRecreatePipelines(const Smile::FPassInitContext&) override { ++RecreateCount; }
        void OnResize(const Smile::FPassInitContext&) override { ++ResizeCount; }
        void OnRegisterDebugTargets() override { ++DebugTargetCount; }
        void OnInvalidateHistory(Smile::EHistoryTarget _Domain) override {
            ++InvalidateCount;
            LastInvalidation = _Domain;
        }

        int RecreateCount = 0;
        int ResizeCount = 0;
        int DebugTargetCount = 0;
        int InvalidateCount = 0;
        Smile::EHistoryTarget LastInvalidation = Smile::EHistoryTarget::None;

    private:
        const char* PassName;
        Smile::FPassShaderStems Stems;
        Smile::EHistoryTarget History;
    };

    void TestRegistrationAndLifecycle() {
        static const char* const kBakerStems[] = { "Bake.cs", "Shared.cs" };
        static const char* const kFirstStems[] = { "First.cs", "Shared.cs" };
        static const char* const kSecondStems[] = { "Shared.cs", "Shared.cs" };

        FTestPipelineOwner Baker("Baker", Smile::FPassShaderStems{kBakerStems});
        FTestRenderPass First(
            "First", Smile::FPassShaderStems{kFirstStems}, Smile::EHistoryTarget::TemporalAA);
        FTestRenderPass Second(
            "Second", Smile::FPassShaderStems{kSecondStems}, Smile::EHistoryTarget::RadianceCache);

        Smile::FPassRegistry Registry;
        Registry.RegisterPipelineOwner(&Baker);
        Registry.RegisterPass(&First);
        Registry.RegisterPass(&Second);
        Registry.RegisterPass(nullptr);
        Registry.RegisterPipelineOwner(static_cast<FTestPipelineOwner*>(nullptr));

        const auto Passes = Registry.FramePasses();
        const auto Owners = Registry.PipelineOwners();
        Check(Passes.size() == 2, "o registro de frame deve conter apenas passes");
        Check(Owners.size() == 3, "o registro de pipelines deve conter baker e passes");
        Check(Passes[0] == &First && Passes[1] == &Second,
            "a ordem de registro dos passes nao foi preservada");
        Check(Owners[0] == &Baker && Owners[1] == &First && Owners[2] == &Second,
            "a ordem de registro dos donos nao foi preservada");

        const Smile::FPassInitContext Context;
        Registry.RecreateAllPipelines(Context);
        Check(Baker.RecreateCount == 1 && First.RecreateCount == 1 && Second.RecreateCount == 1,
            "o reload completo nao visitou cada dono uma vez");

        Baker.RecreateCount = First.RecreateCount = Second.RecreateCount = 0;
        Check(Registry.RecreatePipelinesForStem("Shared.cs", Context) == 3,
            "o reload por stem deve alcançar todos os donos correspondentes");
        Check(Baker.RecreateCount == 1 && First.RecreateCount == 1 && Second.RecreateCount == 1,
            "um stem duplicado no mesmo dono deve recriar apenas uma vez");
        Check(Registry.RecreatePipelinesForStem("Unknown.cs", Context) == 0,
            "um stem desconhecido nao deve recriar pipelines");
        const char* SharedOwner = Registry.OwnerOfShaderStem("Shared.cs");
        Check(SharedOwner && std::string_view(SharedOwner) == Baker.Name(),
            "a consulta do stem deve devolver o primeiro dono registrado");
        Check(Registry.OwnerOfShaderStem("Unknown.cs") == nullptr,
            "um stem desconhecido nao deve ter dono");

        Registry.ResizeAll(Context);
        Registry.RegisterDebugTargetsAll();
        Check(First.ResizeCount == 1 && Second.ResizeCount == 1,
            "resize deve visitar apenas os passes de frame");
        Check(First.DebugTargetCount == 1 && Second.DebugTargetCount == 1,
            "publicacao de debug deve visitar apenas os passes de frame");

        const auto TemporalDomain =
            Smile::EHistoryTarget::TemporalAA | Smile::EHistoryTarget::RayReconstruct;
        Registry.InvalidateHistory(TemporalDomain);
        Check(First.InvalidateCount == 1 && First.LastInvalidation == TemporalDomain,
            "o passe deve receber o dominio completo que intersecta seus alvos");
        Check(Second.InvalidateCount == 0,
            "um historico sem intersecao nao deve ser invalidado");

        Registry.InvalidateHistory(Smile::EHistoryTarget::RadianceCache);
        Check(Second.InvalidateCount == 1,
            "o segundo passe nao recebeu a invalidacao do proprio historico");

        Registry.Clear();
        Check(Registry.FramePasses().empty() && Registry.PipelineOwners().empty(),
            "Clear deve esvaziar os dois indices");
    }
}

int main() {
    std::cout << "Smile.RenderPassRegistry\n";
    TestRegistrationAndLifecycle();

    if (Failures == 0) {
        std::cout << "  OK\n";
        return 0;
    }
    std::cerr << "  " << Failures << " falha(s)\n";
    return 1;
}
