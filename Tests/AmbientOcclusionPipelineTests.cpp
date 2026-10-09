// Runs the actual GTAO pass and its production filters on synthetic geometry in WARP.
#include "Smile/Graphics/GI/AmbientOcclusion.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace Smile {
    struct FAmbientOcclusionTestAccess {
        static ID3D12Resource* Output(FAmbientOcclusion& _AO) { return _AO.AO0.Get(); }
        static D3D12_RESOURCE_STATES& State(FAmbientOcclusion& _AO) { return _AO.AO0State; }
    };
}

namespace {
    using namespace Smile;
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    int Failures = 0;
    void Hr(HRESULT _H) {
        if (FAILED(_H)) throw std::runtime_error("D3D12 failure: " + std::to_string(_H));
    }
    void Check(bool _OK, const char* _Message) {
        if (!_OK) { ++Failures; std::cerr << "FAIL: " << _Message << '\n'; }
    }
    void Barrier(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _R,
                 D3D12_RESOURCE_STATES _Before, D3D12_RESOURCE_STATES _After) {
        if (_Before == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition = {_R, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _Before, _After};
        _CL->ResourceBarrier(1, &B);
    }
    struct FFixture {
        ComPtr<ID3D12Device> Device;
        ComPtr<ID3D12InfoQueue> Info;
        ComPtr<ID3D12CommandQueue> Queue;
        ComPtr<ID3D12CommandAllocator> Allocator;
        ComPtr<ID3D12GraphicsCommandList> CL;
        ComPtr<ID3D12Fence> Fence;
        FTextureSRVHeap Heap;
        FAmbientOcclusion AO;
        FComputePipeline Blur, Upsample;
        GpuResources::FUploadBuffer CB;
        std::vector<ComPtr<ID3D12Resource>> Uploads;
        ComPtr<ID3D12Resource> Depth, Normal, Input, Output;
        u32 DepthSlot, NormalSlot, InputSlot, OutputUAV, Table;
        u32 W = 257, H = 193;
        u64 FenceValue = 0;
        Mat44 Projection = Mat44::PerspectiveFovReverseZLH(1.3f, 257.0f / 193.0f, 0.1f, 4000.0f);
        Mat44 View = Mat44::Identity();
        GTAOConstants Constants{};
        std::vector<float> Depths;
        std::vector<Vec4> Normals;
        u32 SizedSlots = 0;

        explicit FFixture(bool _Hardware) {
            ComPtr<ID3D12Debug> Debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug)))) Debug->EnableDebugLayer();
            ComPtr<IDXGIFactory4> Factory;
            Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)));
            ComPtr<IDXGIAdapter> Adapter;
            if (!_Hardware) Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Adapter)));
            Hr(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));
            Device.As(&Info);
            Heap.Initialize(Device.Get());
            D3D12_COMMAND_QUEUE_DESC Q{};
            Hr(Device->CreateCommandQueue(&Q, IID_PPV_ARGS(&Queue)));
            Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Allocator)));
            Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&CL)));
            Hr(CL->Close());
            Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
            DepthSlot = Heap.Allocate(); NormalSlot = Heap.Allocate(); InputSlot = Heap.Allocate();
            OutputUAV = Heap.Allocate(); Table = Heap.Allocate(3);
            CB = GpuResources::CreateUploadBuffer(Device.Get(), sizeof(GTAOConstants), 1);
            AO.Initialize(Device.Get());
            Blur.Initialize(Device.Get(), "GTAOBlur.cs_6_0.cso", 3, 1);
            Upsample.Initialize(Device.Get(), "GTAOUpsample.cs_6_0.cso", 3, 1);
            std::cout << (_Hardware ? "Hardware" : "WARP") << " production AO pipeline\n";
        }
        void Begin() {
            Uploads.clear();
            Hr(Allocator->Reset()); Hr(CL->Reset(Allocator.Get(), nullptr));
            ID3D12DescriptorHeap* Heaps[] = {Heap.Native()};
            CL->SetDescriptorHeaps(1, Heaps);
        }
        void Submit() {
            Hr(CL->Close());
            ID3D12CommandList* Lists[] = {CL.Get()};
            Queue->ExecuteCommandLists(1, Lists);
            Hr(Queue->Signal(Fence.Get(), ++FenceValue));
            HANDLE Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!Event) throw std::runtime_error("CreateEvent failed");
            const HRESULT Result = Fence->SetEventOnCompletion(FenceValue, Event);
            const DWORD Wait = SUCCEEDED(Result) ? WaitForSingleObject(Event, 30000) : WAIT_FAILED;
            CloseHandle(Event);
            if (Wait != WAIT_OBJECT_0) throw std::runtime_error("GPU fence timeout");
        }
        ComPtr<ID3D12Resource> Texture(u32 _W, u32 _H, DXGI_FORMAT _Format,
                                      const void* _Data, u32 _Stride, u32 _Slot) {
            auto R = GpuResources::CreateTex2D(Device.Get(), _W, _H, _Format,
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, EVramCategory::RenderTargets);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT FP{};
            UINT64 Bytes = 0;
            const auto Desc = R->GetDesc();
            Device->GetCopyableFootprints(&Desc, 0, 1, 0, &FP, nullptr, nullptr, &Bytes);
            auto U = GpuResources::CreateUploadBuffer(Device.Get(), Bytes, 1, false);
            for (u32 Y = 0; Y < _H; ++Y)
                std::memcpy(U.Mapped + FP.Offset + static_cast<size_t>(Y) * FP.Footprint.RowPitch,
                            static_cast<const u8*>(_Data) + static_cast<size_t>(Y) * _W * _Stride,
                            static_cast<size_t>(_W) * _Stride);
            D3D12_TEXTURE_COPY_LOCATION Src{}, Dst{};
            Src.pResource = U.Resource.Get(); Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint = FP;
            Dst.pResource = R.Get(); Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            CL->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            Barrier(CL.Get(), R.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format = _Format; S.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            S.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.Texture2D.MipLevels = 1;
            Heap.CreateSRV(Device.Get(), R.Get(), S, _Slot);
            Uploads.push_back(U.Resource);
            return R;
        }
        float Raw(float _Z) const { return Projection.M[2][2] + Projection.M[3][2] / _Z; }
        Vec3 Ray(u32 _X, u32 _Y) const {
            return {(((_X + 0.5f) / W) * 2 - 1 - Projection.M[2][0]) / Projection.M[0][0],
                    (1 - ((_Y + 0.5f) / H) * 2 - Projection.M[2][1]) / Projection.M[1][1], 1};
        }
        void Plane(float _Slope = 0, float _Z = 3) {
            Depths.resize(static_cast<size_t>(W) * H); Normals.resize(Depths.size());
            const float L = std::sqrt(1 + _Slope * _Slope);
            for (u32 Y = 0; Y < H; ++Y) for (u32 X = 0; X < W; ++X) {
                const size_t I = Y * W + X;
                Depths[I] = Raw(_Z / (1 - _Slope * Ray(X, Y).X));
                const Vec3 WorldN = View.TransformVectorRowTransposed({_Slope / L, 0, -1 / L});
                Normals[I] = {WorldN.X * 0.5f + 0.5f, WorldN.Y * 0.5f + 0.5f, WorldN.Z * 0.5f + 0.5f, 1};
            }
        }
        void UploadScene() {
            Depth = Texture(W, H, DXGI_FORMAT_R32_FLOAT, Depths.data(), sizeof(float), DepthSlot);
            // Match the packed normal format used by the depth-normal prepass.
            std::vector<u32> Packed(Normals.size());
            auto UNorm10 = [](float V) { return static_cast<u32>(std::round(std::clamp(V, 0.0f, 1.0f) * 1023)); };
            for (size_t I = 0; I < Normals.size(); ++I)
                Packed[I] = UNorm10(Normals[I].X) | (UNorm10(Normals[I].Y) << 10) |
                            (UNorm10(Normals[I].Z) << 20) | (3u << 30);
            Normal = Texture(W, H, DXGI_FORMAT_R10G10B10A2_UNORM, Packed.data(), sizeof(u32), NormalSlot);
        }
        void FillConstants(u32 _Scale = 1, bool _Vertical = false) {
            Constants = {};
            Constants.ProjA = {Projection.M[0][0], Projection.M[1][1], Projection.M[2][2], Projection.M[3][2]};
            Constants.ProjOffset = {Projection.M[2][0], Projection.M[2][1], 0, 0};
            Constants.ScreenParams = {float(W), float(H), 1.0f / W, 1.0f / H};
            Constants.Params = {0.6f, 1, 1, 0.6f};
            Constants.Params2 = {3, 8, 0, _Vertical ? 1.0f : 0.0f};
            Constants.Params3 = {50, 300, float(_Scale), 0};
            Constants.ViewMatrix = View;
            std::memcpy(CB.Mapped, &Constants, sizeof(Constants));
        }
        std::vector<float> Read(ID3D12Resource* _R, D3D12_RESOURCE_STATES& _State) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT FP{};
            UINT64 Bytes = 0;
            const auto Desc = _R->GetDesc();
            Device->GetCopyableFootprints(&Desc, 0, 1, 0, &FP, nullptr, nullptr, &Bytes);
            auto RB = GpuResources::CreateReadbackBuffer(Device.Get(), Bytes);
            Barrier(CL.Get(), _R, _State, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION Src{}, Dst{};
            Src.pResource = _R; Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.pResource = RB.Get(); Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Dst.PlacedFootprint = FP;
            CL->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            Barrier(CL.Get(), _R, D3D12_RESOURCE_STATE_COPY_SOURCE, _State);
            Submit();
            void* Mapped = nullptr;
            const D3D12_RANGE Range{0, static_cast<SIZE_T>(Bytes)};
            Hr(RB->Map(0, &Range, &Mapped));
            std::vector<float> Values(static_cast<size_t>(Desc.Width) * Desc.Height);
            for (u32 Y = 0; Y < Desc.Height; ++Y) for (u32 X = 0; X < Desc.Width; ++X)
                Values[Y * Desc.Width + X] =
                    (static_cast<const u8*>(Mapped) + FP.Offset)[Y * FP.Footprint.RowPitch + X] / 255.0f;
            const D3D12_RANGE NoWrites{0, 0}; RB->Unmap(0, &NoWrites);
            return Values;
        }
        std::vector<float> RunAO(bool _Half, u32 _Frame = 0, bool _Temporal = true, bool _Clear = false) {
            Begin(); UploadScene();
            AO.SetupForResize(Device.Get(), Heap, DepthSlot, NormalSlot, W, H);
            if (!SizedSlots) SizedSlots = Heap.UsedSlots();
            Check(Heap.UsedSlots() == SizedSlots, "resize leaks AO descriptor ranges");
            AO.SetHalfRes(_Half);
            AO.UpdatePerFrame(0, Projection, View, W, H, _Frame, _Temporal);
            if (_Clear) AO.ClearToWhite(CL.Get(), Heap); else AO.Execute(CL.Get(), Heap);
            return Read(FAmbientOcclusionTestAccess::Output(AO), FAmbientOcclusionTestAccess::State(AO));
        }
        std::vector<float> Filter(bool _Upsample, const std::vector<float>& _Values, bool _Vertical = false) {
            Begin(); UploadScene();
            const u32 Scale = _Upsample ? 2u : 1u;
            Input = Texture((W + Scale - 1) / Scale, (H + Scale - 1) / Scale,
                            DXGI_FORMAT_R32_FLOAT, _Values.data(), sizeof(float), InputSlot);
            Output = GpuResources::CreateTex2D(Device.Get(), W, H, DXGI_FORMAT_R8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                EVramCategory::RenderTargets);
            D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
            U.Format = DXGI_FORMAT_R8_UNORM; U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            Heap.CreateUAV(Device.Get(), Output.Get(), U, OutputUAV);
            Heap.CopyTable(Device.Get(), Table, {DepthSlot, InputSlot, NormalSlot});
            FillConstants(Scale, _Vertical);
            (_Upsample ? Upsample : Blur).Bind(CL.Get());
            CL->SetComputeRootConstantBufferView(0, CB.Resource->GetGPUVirtualAddress());
            CL->SetComputeRootDescriptorTable(1, Heap.GpuHandle(Table));
            CL->SetComputeRootDescriptorTable(2, Heap.GpuHandle(OutputUAV));
            CL->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            auto State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            return Read(Output.Get(), State);
        }
        void CheckDebugLayer() {
            if (!Info) { std::cout << "Debug layer unavailable\n"; return; }
            for (u64 I = 0; I < Info->GetNumStoredMessages(); ++I) {
                SIZE_T Bytes = 0; Hr(Info->GetMessage(I, nullptr, &Bytes));
                std::vector<char> Storage(Bytes);
                auto* M = reinterpret_cast<D3D12_MESSAGE*>(Storage.data());
                Hr(Info->GetMessage(I, M, &Bytes));
                if (M->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++Failures; std::cerr << M->pDescription << '\n';
                }
            }
        }
    };

    void TestAO(FFixture& F) {
        for (bool Half : {false, true}) {
            F.Plane();
            auto Flat = F.RunAO(Half);
            const float Min = *std::min_element(Flat.begin(), Flat.end());
            std::cout << "Open plane half=" << Half << " minimum visibility=" << Min << '\n';
            Check(Min > 0.97f, "open plane self-occludes (including screen borders)");
            std::fill(F.Depths.begin(), F.Depths.end(), 0.0f);
            auto Sky = F.RunAO(Half);
            Check(std::all_of(Sky.begin(), Sky.end(), [](float V) { return V == 1; }), "sky must stay white");
            F.View = Mat44::RotationY(0.8f) * Mat44::RotationX(0.4f);
            F.Projection.M[2][0] = 0.7f / F.W;
            F.Projection.M[2][1] = -0.6f / F.H;
            F.Plane(0.7f);
            auto Slope = F.RunAO(Half);
            double Mean = 0; for (float V : Slope) Mean += V / Slope.size();
            std::cout << "Tilted jittered plane half=" << Half << " mean visibility=" << Mean << '\n';
            Check(Mean > 0.94, "tilted jittered plane develops false occlusion");
            F.Projection.M[2][0] = F.Projection.M[2][1] = 0;
            F.View = Mat44::Identity();
            F.Plane();
            for (u32 Y = 0; Y < F.H; ++Y) for (u32 X = 0; X < F.W / 2; ++X)
                F.Depths[Y * F.W + X] = F.Raw(2.75f);
            auto Contact = F.RunAO(Half, 0, false);
            float ContactMin = 1;
            for (u32 Y = 8; Y + 8 < F.H; ++Y) for (u32 X = F.W / 2; X < F.W / 2 + 5; ++X)
                ContactMin = std::min(ContactMin, Contact[Y * F.W + X]);
            std::cout << "Contact half=" << Half << " minimum visibility=" << ContactMin << '\n';
            Check(ContactMin < 0.92f, "nearby depth step must produce contact AO");
            Check(Contact == F.RunAO(Half, 7, false), "AO changes between frames without temporal AA");
            auto Clear = F.RunAO(Half, 0, false, true);
            Check(std::all_of(Clear.begin(), Clear.end(), [](float V) { return V == 1; }), "disabled AO clear is not white");
            F.AO.SetRadius(0);
            auto Zero = F.RunAO(Half);
            Check(std::all_of(Zero.begin(), Zero.end(), [](float V) { return V == 1; }), "zero radius must disable occlusion");
            F.AO.SetRadius(0.6f);
            F.Plane(0, 350);
            auto Far = F.RunAO(Half);
            Check(std::all_of(Far.begin(), Far.end(), [](float V) { return V == 1; }), "fade end must be white");
        }
    }
    void TestFilters(FFixture& F) {
        for (bool Vertical : {false, true}) {
            F.Plane();
            std::vector<float> Input(F.Depths.size());
            for (u32 Y = 0; Y < F.H; ++Y) for (u32 X = 0; X < F.W; ++X) {
                const bool Side = Vertical ? Y >= F.H / 2 : X >= F.W / 2;
                Input[Y * F.W + X] = Side ? 1.0f : 0.0f;
                if (Side) F.Normals[Y * F.W + X] = {1, 0.5f, 0.5f, 1};
            }
            Check(F.Filter(false, Input, Vertical) == Input, "blur leaks across equal-depth normal discontinuity");
            F.Plane();
            for (u32 Y = 0; Y < F.H; ++Y) for (u32 X = 0; X < F.W; ++X)
                if (Vertical ? Y == F.H / 2 : X == F.W / 2) F.Depths[Y * F.W + X] = F.Raw(2);
            auto Thin = F.Filter(false, Input, Vertical);
            bool OK = true;
            for (u32 Y = 0; Y < F.H; ++Y) for (u32 X = 0; X < F.W; ++X)
                if (Vertical ? Y != F.H / 2 : X != F.W / 2) OK &= Thin[Y * F.W + X] == Input[Y * F.W + X];
            Check(OK, "wide blur taps jump across a one-pixel foreground strip");
            F.Plane(0.7f);
            std::fill(Input.begin(), Input.end(), 0.0f);
            Input[(F.H / 2) * F.W + F.W / 2] = 1;
            auto Slope = F.Filter(false, Input, Vertical);
            Check(Slope[(F.H / 2) * F.W + F.W / 2] < 0.5f, "blur rejects a smooth sloped plane");
            std::fill(F.Depths.begin(), F.Depths.end(), 0.0f);
            auto Sky = F.Filter(false, Input, Vertical);
            Check(std::all_of(Sky.begin(), Sky.end(), [](float V) { return V == 1; }), "blur dirties sky");
        }
        F.Plane();
        const u32 HW = (F.W + 1) / 2, HH = (F.H + 1) / 2;
        std::vector<float> Half(static_cast<size_t>(HW) * HH);
        for (u32 Y = 0; Y < HH; ++Y) for (u32 X = 0; X < HW; ++X) Half[Y * HW + X] = float(X) / (HW - 1);
        auto Upsampled = F.Filter(true, Half);
        bool Aligned = true;
        for (u32 Y = 0; Y < F.H; ++Y) for (u32 X = 0; X < F.W; ++X)
            Aligned &= std::abs(Upsampled[Y * F.W + X] - float(X) / (2 * (HW - 1))) <= 0.5f / 255 + 1e-6f;
        Check(Aligned, "half-res upsample shifted the even-pixel sample grid");
        F.Plane();
        std::fill(Half.begin(), Half.end(), 0.0f);
        F.Depths[(F.H / 2) * F.W + 33] = F.Raw(2);
        auto Missed = F.Filter(true, Half);
        Check(Missed[(F.H / 2) * F.W + 33] == 1, "upsample applies unrelated dark AO to an unsampled surface");
        F.W = F.H = 1;
        F.Plane();
        for (bool HalfRes : {false, true}) Check(F.RunAO(HalfRes)[0] == 1, "1x1 viewport self-occludes");
    }
}

int main(int argc, char**) try {
    FFixture F(argc > 1);
    TestAO(F); TestFilters(F); F.CheckDebugLayer();
    std::cout << "AO failures: " << Failures << '\n';
    return Failures ? 1 : 0;
} catch (const std::exception& E) {
    std::cerr << E.what() << '\n'; return 1;
}
