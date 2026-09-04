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
            return R.SceneState->PreviousModels.PreviousOr(H, Fallback);
        }
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
    Require(Same(FRendererIdentityTestAccess::Previous(R, NewHandle, Sentinel), Sentinel),
            "hidden object retained stale raster model history");
    ChangeVisibility(true);
    Require(!Same(FRendererIdentityTestAccess::Previous(R, NewHandle, Sentinel), Sentinel),
            "visible object did not rebuild model history");

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
    R.Shutdown();
    std::cout << "PASS: renderer lifecycle, pending pick, slot reuse, visibility and reload\n";
    return 0;
} catch (const std::exception& Error) {
    std::cerr << "FAIL: " << Error.what() << '\n';
    return 1;
}
