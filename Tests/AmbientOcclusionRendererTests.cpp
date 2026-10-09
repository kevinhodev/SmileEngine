// Optional hardware validation with captures from a cooked Bistro/Emerald scene.
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
    using namespace Smile;
    void Require(bool _OK, const char* _Message) {
        if (!_OK) throw std::runtime_error(_Message);
    }
    struct FWindow {
        HWND Handle = CreateWindowExW(0, L"STATIC", L"Smile AO validation", WS_OVERLAPPED,
            0, 0, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~FWindow() { if (Handle) DestroyWindow(Handle); }
    };
    void Frames(Renderer& _R, u32 _Count) {
        for (u32 I = 0; I < _Count; ++I) { _R.RenderFrame(); _R.PresentFrame(); }
    }
    void Capture(Renderer& _R, const std::filesystem::path& _Dir, const std::string& _Label) {
        // Apply pending setting changes before starting the deterministic warmup.
        Frames(_R, 8);
        FCaptureRequest Request;
        Request.OutputDir = _Dir.wstring(); Request.Label = _Label;
        Request.SceneName = "Ambient occlusion audit";
        Request.WarmupFrames = 128; Request.Preset = ECapturePreset::Gameplay;
        Request.PinTimeOfDayHours = 10;
        Require(_R.RequestCapture(Request), "AO capture rejected");
        FFrameCapture::FResult Result;
        bool Complete = false;
        for (u32 I = 0; I < 160 && !Complete; ++I) {
            Frames(_R, 1); Complete = _R.ConsumeCaptureResult(Result);
        }
        Require(Complete && Result.Success, "AO capture failed");
        std::wcout << L"CAPTURE " << Result.PngPath << std::endl;
    }
    void Profile(Renderer& _R, std::ofstream& _CSV, const char* _Mode) {
        Frames(_R, 32);
        u64 Previous = ~u64(0);
        u32 Samples = 0;
        for (u32 I = 0; I < 100 && Samples < 32; ++I) {
            Frames(_R, 1);
            const auto& P = _R.GetGpuProfiler();
            const u64 Frame = P.ResultsFrameIndex();
            if (Frame == Previous) continue;
            Previous = Frame;
            for (const auto& Scope : P.Results()) if (std::string_view(Scope.Name) == "GTAO") {
                _CSV << _Mode << ',' << Frame << ',' << Scope.RawMilliseconds << '\n';
                ++Samples; break;
            }
        }
        Require(Samples == 32, "AO profiling did not return 32 distinct GPU frames");
        _CSV.flush();
    }
}

int wmain(int _Argc, wchar_t** _Argv) try {
    Require(_Argc == 3, "usage: SmileAmbientOcclusionRendererTests <scene.sscene> <outputDir>");
    const auto Dir = std::filesystem::absolute(_Argv[2]);
    std::filesystem::create_directories(Dir);
    Require(InitializeLogger({Dir / "renderer.log", false}), "logger initialization failed");
    FWindow Window;
    Require(Window.Handle != nullptr, "hidden renderer window creation failed");
    Renderer R;
    R.Initialize(Window.Handle, 1280, 720);
    auto& S = R.Settings();
    S.SetVSync(false); S.SetUpscaler(EUpscaler::None); S.SetUseTAA(false);
    S.SetDenoiser(EDenoiser::NRD); S.SetUseGI(true); S.SetUseReflections(true);
    S.SetUseClouds(false); S.SetUseVolumetricFog(false); S.SetUseSunShafts(false);
    S.SetUseWater(false); S.SetRainAmount(0);
    Require(R.LoadCookedScene(_Argv[1]), "cooked scene load failed");
    const bool Emerald = std::wstring(_Argv[1]).find(L"Emerald") != std::wstring::npos;
    if (Emerald) R.SetCameraPose({0.5380736f, 3.213687f, 67.9627f}, 1.299976f, 538.5004f);
    else R.SetCameraPose({-32.16664f, 8.223711f, 15.564535f}, -1.549957f, 125.54991f);
    // Mesh-light readbacks/alias publication invalidate histories during initial frames.
    Frames(R, 160);
    S.SetUseAO(false);
    Capture(R, Dir, "ao-off");
    S.SetUseAO(true);
    std::ofstream CSV(Dir / "gpu.csv");
    CSV << "mode,sourceFrame,gtaoMilliseconds\n";
    for (bool Half : {false, true}) {
        S.SetAOHalfRes(Half);
        const std::string Mode = Half ? "half" : "full";
        Capture(R, Dir, Mode);
        Profile(R, CSV, Mode.c_str());
        const auto& Targets = DebugTargets::All();
        u32 Index = 0;
        for (; Index < Targets.size(); ++Index) if (Targets[Index].Name == "GTAO") break;
        Require(Index < Targets.size(), "GTAO debug target missing");
        R.SetDebugTargetIndex(Index);
        Capture(R, Dir, Mode + "-ao");
        R.SetDebugTargetIndex(Renderer::kNoDebugTarget);
    }
    S.SetUseTAA(true); S.SetAOHalfRes(false);
    Capture(R, Dir, "taa-full");
    S.SetAOHalfRes(true);
    R.Resize(961, 541);
    Capture(R, Dir, "taa-half-odd-resize");
    if (S.UpscalerAvailable(EUpscaler::DLSS)) {
        S.SetUpscaler(EUpscaler::DLSS);
        Capture(R, Dir, "dlss-half");
    }
    S.SetUseAO(false); Frames(R, 4); S.SetUseAO(true); Frames(R, 4);
    R.Shutdown();
    std::ofstream(Dir / "complete.txt") << "AO full/half/off, debug target, TAA, odd resize and shutdown passed.\n";
    ShutdownLogger();
    return 0;
} catch (const std::exception& E) {
    std::cerr << E.what() << '\n'; return 1;
}
