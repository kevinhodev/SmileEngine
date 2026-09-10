// Opt-in HARDWARE integration. A private, unsaved synthetic scene exercises the real renderer
// between submissions. WARP tests cover shader math; this covers lifecycle/wiring with history
// active. It is not a visual-difference test and never edits scene assets.
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RendererSceneState.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Graphics/Backend/RenderBackend.h"
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace Smile {
    struct FRendererIdentityTestAccess {
        static bool History(const Renderer& R) { return R.TemporalMotion.HasHistory(); }
        static Mat44 Previous(Renderer& R, FGpuSceneHandle H, const Mat44& Fallback) {
            Mat44 Current{}, Previous{};
            return R.InstanceTable.TransformsFor(H, Current, Previous) ? Previous : Fallback;
        }
        static bool Meta(Renderer& R, FGpuSceneHandle H, FGpuSceneInstanceMeta& Out) {
            return R.InstanceTable.MetaFor(H, Out);
        }
        static u64 ObjectCBReserved(const Renderer& R) { return R.ObjectCBReservedBytes(); }
        static void CheckDevice(Renderer& R) {
            if (FAILED(R.Backend->Device.Native()->GetDeviceRemovedReason()))
                throw std::runtime_error("device removed during identity integration");
        }
    };
}
using namespace Smile;
namespace {
    void Require(bool Condition, const char* Message) {
        if (!Condition) throw std::runtime_error(Message);
    }
    struct FWindow {
        HWND Handle = nullptr;
        FWindow() {
            Handle = CreateWindowExW(0, L"STATIC", L"Smile identity test", WS_OVERLAPPED,
                                     0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(Handle != nullptr, "CreateWindowEx failed");
            // Deliberately never shown: this is a renderer test, not a user editor window.
        }
        ~FWindow() { if (Handle) DestroyWindow(Handle); }
    };
    void Frames(Renderer& R, unsigned Count) {
        for (unsigned i = 0; i < Count; ++i) {
            R.RenderFrame();
            R.PresentFrame();
            FRendererIdentityTestAccess::CheckDevice(R);
        }
    }
    bool Same(const Mat44& A, const Mat44& B) { return std::memcmp(&A, &B, sizeof A) == 0; }
}
int wmain(int Argc, wchar_t** Argv) try {
    Require(Argc == 2, "usage: SmileRendererIdentityTests <synthetic.sscene>");
    FWindow Window;
    Renderer R;
    R.Initialize(Window.Handle, 640, 360);
    auto& Settings = R.Settings();
    Settings.SetVSync(false);
    Settings.SetUpscaler(EUpscaler::None);
    Settings.SetDenoiser(EDenoiser::NRD);
    Settings.SetUseGI(true);
    Settings.SetUseReSTIRGI(true);
    Settings.SetUseReSTIRDI(true);
    Settings.SetUseReflections(true);
    Settings.SetOcclusionCulling(true);
    Require(R.LoadCookedScene(Argv[1]), "scene load failed");
    auto& Scene = R.GetScene();
    Require(Scene.Renderables().size() >= 8, "integration needs at least eight objects");
    R.SetCameraPose({0, 30, -120}, -12, 0);
    Frames(R, 8);
    Require(FRendererIdentityTestAccess::History(R), "temporal history never became active");
    const u64 Source = Scene.IdAt(3);
    const u64 Copy = R.DuplicateRenderable(Source);
    Require(Copy && Copy != Source, "duplicate failed");
    Require(Scene.FindRenderable(Copy)->GpuHandle != Scene.FindRenderable(Source)->GpuHandle,
            "duplicate shares source residency");
    Require(FRendererIdentityTestAccess::History(R), "duplicate discarded temporal identity history");
    Frames(R, 4);

    // Find an actual raster hit, then leave another readback for that pixel pending across erase.
    u32 PickX = 0, PickY = 0;
    u64 PickedId = 0;
    for (u32 Y = 60; Y < 340 && !PickedId; Y += 40) {
        for (u32 X = 40; X < 620 && !PickedId; X += 60) {
            R.RequestPick(X, Y);
            Frames(R, 3);
            int Index = -1;
            if (R.TryGetPickResult(Index) && Index >= 0) {
                PickedId = Scene.IdAt(static_cast<u32>(Index));
                PickX = X; PickY = Y;
            }
        }
    }
    Require(PickedId != 0, "picking never hit the synthetic scene");

    const u64 Victim = Scene.IdAt(0) != PickedId ? Scene.IdAt(0) : Scene.IdAt(1);
    const auto OldHandle = Scene.FindRenderable(Victim)->GpuHandle;
    const auto Survivor = Scene.FindRenderable(PickedId)->GpuHandle;
    const Mat44 Fallback = Mat44::Identity();
    const Mat44 Before = FRendererIdentityTestAccess::Previous(R, Survivor, Fallback);
    R.RequestPick(PickX, PickY);
    Frames(R, 1);
    Require(R.RemoveRenderable(Victim), "remove failed");
    Require(!Scene.GpuSlots().IsAlive(OldHandle), "removed residency still alive");
    Require(FRendererIdentityTestAccess::History(R), "erase discarded temporal identity history");
    Require(Same(Before, FRendererIdentityTestAccess::Previous(R, Survivor, Fallback)),
            "erase changed survivor model history");
    Frames(R, 3);
    int Resolved = -1;
    Require(R.TryGetPickResult(Resolved), "erase cancelled the pending pick");
    Require(Resolved >= 0 && Scene.IdAt(static_cast<u32>(Resolved)) == PickedId,
            "pending pick resolved to a different object after erase");
    Require(Scene.GpuSlots().RetiredCount() == 0, "frame boundary did not collect retired slots");
    const u64 Reused = R.DuplicateRenderable(Source);
    const auto NewHandle = Scene.FindRenderable(Reused)->GpuHandle;
    Require(NewHandle.Slot == OldHandle.Slot && NewHandle.Generation > OldHandle.Generation,
            "did not exercise slot reuse with a newer generation");
    Frames(R, 4);

    auto ChangeVisibility = [&](bool Visible) {
        auto* Object = Scene.FindRenderable(Reused);
        Object->Visible = Visible;
        Object->Transform.Position.X += 0.25f;
        Object->RefreshWorldBounds();
        Scene.BumpTransformsVersion();
        Scene.BumpStaticCastersVersion();
        Settings.MarkSceneContentDirty();
        Frames(R, 4);
        Require(FRendererIdentityTestAccess::History(R), "visibility discarded temporal history");
    };
    ChangeVisibility(false);
    Require(Scene.FindRenderable(Reused)->RaytracingHandle().Generation == 0,
            "hidden object still enters temporal RT table");
    Mat44 Sentinel = Mat44::Identity(); Sentinel.M[3][0] = 123456;
    Require(!Same(FRendererIdentityTestAccess::Previous(R, NewHandle, Sentinel), Sentinel),
            "hidden object disappeared from the canonical instance table");
    ChangeVisibility(true);
    Require(!Same(FRendererIdentityTestAccess::Previous(R, NewHandle, Sentinel), Sentinel),
            "visible object did not rebuild model history");

    // Mobilidade possui versao propria: nao muda transform nem estrutura. A tabela canonica tem
    // de observar esse canal, ou o bit Dynamic fica congelado enquanto o Outliner mostra outro
    // estado. Este caso nao bumpa TransformsVersion de proposito.
    {
        FRenderable* Object = Scene.FindRenderable(Reused);
        Require(Object != nullptr, "reused object disappeared before mobility check");
        Object->Mobility = EMobility::Dynamic;
        Scene.BumpStaticCastersVersion();
        Frames(R, 2);
        FGpuSceneInstanceMeta Meta{};
        Require(FRendererIdentityTestAccess::Meta(R, NewHandle, Meta),
                "canonical instance meta did not resolve after mobility edit");
        Require((Meta.Flags & GpuSceneInstanceDynamic) != 0,
                "canonical instance meta ignored mobility-only version change");
        Object->Mobility = EMobility::Static;
        Scene.BumpStaticCastersVersion();
        Frames(R, 2);
        Require(FRendererIdentityTestAccess::Meta(R, NewHandle, Meta) &&
                    (Meta.Flags & GpuSceneInstanceDynamic) == 0,
                "canonical instance meta kept stale dynamic flag");
    }

    for (unsigned Position = 0; Position != 3; ++Position) {
        const auto Count = static_cast<u32>(Scene.Renderables().size());
        const u32 Index = Position == 0 ? 0 : Position == 1 ? Count / 2 : Count - 1;
        const auto H = Scene.Renderables()[Index].GpuHandle;
        Require(R.RemoveRenderable(Scene.IdAt(Index)), "first/middle/last erase failed");
        Require(FRendererIdentityTestAccess::History(R), "structural edit reset identity history");
        Frames(R, 4);
        Require(!Scene.GpuSlots().IsAlive(H), "erased handle resurrected");
    }
    const auto Capacity = Scene.GpuSlots().Capacity();
    const auto PreReload = Scene.Renderables()[0].GpuHandle;
    Require(R.LoadCookedScene(Argv[1]), "reload failed");
    Require(&R.GetScene() == &Scene, "reload replaced the scene owner");
    Require(Scene.GpuSlots().Capacity() == Capacity, "reload grew the residency pool");
    Require(!Scene.GpuSlots().IsAlive(PreReload), "reload resurrected an old handle");
    Frames(R, 8);
    Require(FRendererIdentityTestAccess::History(R), "history failed to resume after reload");

    // --- A/B do picking: classico contra transform bindless (Fase 4) -------------------------
    //
    // O picking COMPOE a MVP no vertex shader, e nao a recebe pronta. Se a composicao bindless
    // divergir da classica, o objeto sob o cursor deixa de ser o objeto desenhado ali — e o
    // sintoma e "selecionou o vizinho" numa borda, nao uma tela quebrada.
    //
    // NO FIM e AUTOCONTIDO, com pixel proprio. A primeira versao ficava no meio, reusando o pixel
    // do teste de pick pendente, e derrubava uma assercao 40 linhas ABAIXO: os frames a mais
    // mudavam a fase do jitter, e aquele pixel e marginal. Um bloco de cobertura nova nao pode
    // mudar o resultado da cobertura que ja existia.
    //
    // O TAA sai porque OFF e ON sao medidos em FRAMES diferentes, logo em fases diferentes do
    // jitter: a MVP jitterada move a geometria por fracao de pixel entre as duas medidas, e num
    // pixel de borda isso troca a identidade sem que nada esteja errado.
    {
        const bool TAAWas = Settings.GetUseTAA();
        Settings.SetUseTAA(false);
        Frames(R, 3);

        // Drena ate o resultado chegar: `RequestPick` sai cedo com um pick em voo, entao um
        // resultado nao consumido faria a proxima chamada ser ignorada em silencio.
        auto PickAt = [&](u32 X, u32 Y) -> u64 {
            R.RequestPick(X, Y);
            int Index = -1;
            bool Arrived = false;
            for (unsigned Tries = 0; Tries < 16 && !Arrived; ++Tries) {
                Frames(R, 1);
                Arrived = R.TryGetPickResult(Index);
            }
            Require(Arrived, "pick do A/B nao resolveu; ficaria pendente e engoliria o proximo");
            return Index >= 0 ? Scene.IdAt(static_cast<u32>(Index)) : 0;
        };

        // Um hit qualquer, so para escolher QUAL objeto vai ser transformado.
        u32 HitX = 0, HitY = 0;
        u64 HitId = 0;
        for (u32 Y = 80; Y < 300 && !HitId; Y += 40) {
            for (u32 X = 60; X < 600 && !HitId; X += 24) {
                const u64 Id = PickAt(X, Y);
                if (Id != 0) { HitId = Id; HitX = X; HitY = Y; }
            }
        }
        Require(HitId != 0, "A/B do picking nao achou hit nenhum apos o reload");

        // Escala NAO UNIFORME + rotacao: e onde a ordem do produto e a convencao de linha/coluna
        // divergem. Com escala uniforme as duas composicoes coincidem mesmo estando erradas.
        FRenderable* Skewed = Scene.FindRenderable(HitId);
        Require(Skewed != nullptr, "objeto do A/B sumiu");
        Skewed->Transform.Scale = { 1.7f, 0.6f, 1.15f };
        Skewed->Transform.RotationEuler.Y += 27.0f;
        Skewed->RefreshWorldBounds();
        Scene.BumpTransformsVersion();
        Scene.BumpStaticCastersVersion();
        Frames(R, 3);

        // REACHA o objeto DEPOIS da transformacao, no classico. O pixel de antes pode ter deixado
        // de acerta-lo — a transformacao muda a silhueta — e comparar um pixel que virou FUNDO
        // com o mesmo pixel de fundo no bindless daria verde sem exercitar nada. Este teste ja
        // teve exatamente esse defeito.
        auto FindPixelFor = [&](u64 Id, u32& OutX, u32& OutY) -> bool {
            if (PickAt(HitX, HitY) == Id) { OutX = HitX; OutY = HitY; return true; }
            // Cruz em torno do hit original: a transformacao mexe em escala e rotacao, e nao na
            // posicao, entao a pegada de tela continua sobreposta. Limitada de proposito — este e
            // um teste de hardware e cada pick custa frames.
            for (i32 D = 3; D <= 60; D += 3) {
                const i32 Xs[4] = { static_cast<i32>(HitX) + D, static_cast<i32>(HitX) - D,
                                    static_cast<i32>(HitX),     static_cast<i32>(HitX) };
                const i32 Ys[4] = { static_cast<i32>(HitY),     static_cast<i32>(HitY),
                                    static_cast<i32>(HitY) + D, static_cast<i32>(HitY) - D };
                for (int K = 0; K < 4; ++K) {
                    if (Xs[K] < 1 || Xs[K] > 630 || Ys[K] < 1 || Ys[K] > 350) continue;
                    const u32 X = static_cast<u32>(Xs[K]), Y = static_cast<u32>(Ys[K]);
                    if (PickAt(X, Y) == Id) { OutX = X; OutY = Y; return true; }
                }
            }
            return false;
        };

        u32 SkewX = 0, SkewY = 0;
        Require(FindPixelFor(HitId, SkewX, SkewY),
                "caso de escala nao uniforme perdeu o objeto e compararia fundo com fundo");
        const u64 SkewedClassic = PickAt(SkewX, SkewY);
        Require(SkewedClassic == HitId,
                "o pixel do caso transformado nao resolve para o objeto transformado");

        // Borda ADJACENTE de verdade, achada DEPOIS da transformacao e a partir do proprio objeto
        // transformado: caminha de um em um pixel ate a identidade mudar. Dois pixels a 8 de
        // distancia nao sao uma borda — sao duas amostras, e uma divergencia de fracao de pixel
        // cabe inteira entre elas.
        u32 BorderX = 0;
        u64 BorderLeft = 0, BorderRight = 0;
        for (u32 X = SkewX; X + 1 < 632; ++X) {
            const u64 Right = PickAt(X + 1, SkewY);
            if (Right != HitId) { BorderX = X; BorderLeft = HitId; BorderRight = Right; break; }
        }
        // OBRIGATORIA, e nao "se achar". Ela e propriedade declarada deste teste; pular em
        // silencio quando nao acha e o mesmo gate-fantasma de sempre.
        Require(BorderX != 0, "A/B do picking nao achou borda adjacente do objeto transformado");
        Require(BorderLeft != BorderRight, "a borda encontrada tem a mesma identidade dos dois lados");

        Settings.SetBindlessMaterials(true);
        Settings.SetBindlessTransforms(true);
        if (!Settings.GetBindlessTransforms()) {
            // Device sem Resource Binding Tier 3, ou PSOs ausentes. Dizer ALTO: um A/B que some
            // sozinho num device nao suportado e o gate-fantasma que o plano proibe.
            std::cout << "  (A/B do picking PULADO: transform bindless indisponivel neste device)\n";
        } else {
            Frames(R, 3);
            // Prova de MEMORIA, e nao de escrita: com o bindless ligado o ObjectCB nao existe.
            // Separada de `objectConstantBytes` de proposito -- durante seis das sete migracoes
            // desta fase a escrita caiu por passe enquanto a memoria ficou intacta.
            Require(FRendererIdentityTestAccess::ObjectCBReserved(R) == 0,
                    "ligar transform bindless nao liberou o ObjectCB");
            Require(PickAt(SkewX, SkewY) == HitId,
                    "picking bindless resolveu outro objeto com escala nao uniforme");
            Require(PickAt(BorderX, SkewY) == BorderLeft,
                    "picking bindless trocou a identidade do lado esquerdo da borda");
            Require(PickAt(BorderX + 1, SkewY) == BorderRight,
                    "picking bindless trocou a identidade do lado direito da borda");
            Settings.SetBindlessTransforms(false);
            Settings.SetBindlessMaterials(false);
            Frames(R, 3);
            // E a volta e TRANSACIONAL: o buffer classico tem de existir de novo antes de o
            // toggle mudar, senao o raster ficaria sem de onde ler transform.
            Require(FRendererIdentityTestAccess::ObjectCBReserved(R) > 0,
                    "voltar ao classico nao recriou o ObjectCB");
            Require(PickAt(SkewX, SkewY) == HitId,
                    "voltar ao classico nao restaurou a identidade do objeto transformado");
            Require(PickAt(BorderX, SkewY) == BorderLeft &&
                        PickAt(BorderX + 1, SkewY) == BorderRight,
                    "voltar ao classico nao restaurou a borda");
        }
        Settings.SetUseTAA(TAAWas);
    }

    R.Shutdown();
    std::cout << "PASS: renderer lifecycle, pending pick, slot reuse, visibility, reload e A/B "
                 "do picking bindless\n";
    return 0;
} catch (const std::exception& Error) {
    std::cerr << "FAIL: " << Error.what() << '\n';
    return 1;
}
