#include "Smile/Graphics/DebugTargets.h"
#include <algorithm>
#include <cctype>
#include <mutex>

namespace Smile::DebugTargets {

    namespace {
        std::mutex                GMutex;
        std::vector<FDebugTarget> GTargets;

        std::string ToLower(const std::string& S) {
            std::string R = S;
            std::transform(R.begin(), R.end(), R.begin(),
                           [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
            return R;
        }
    }

    void Register(const std::string& _Name, u32 _SrvSlot, EDebugDecode _Decode,
                  u32 _SubIndex, u32 _MipCount, f32 _Exposure, u32 _AtlasTilePx,
                  bool _LinearFilter) {
        if (_Name.empty() || _SrvSlot == 0xFFFFFFFFu) return;

        std::lock_guard<std::mutex> Lock(GMutex);
        // Mesmo nome sobrescreve: e assim que resize funciona sem ninguem des-registrar nada.
        for (FDebugTarget& T : GTargets) {
            if (T.Name == _Name) {
                T.SrvSlot  = _SrvSlot;
                T.Decode   = _Decode;
                T.SubIndex = _SubIndex;
                T.MipCount = _MipCount == 0 ? 1 : _MipCount;
                T.Exposure = _Exposure;
                T.AtlasTilePx = _AtlasTilePx;
                T.LinearFilter = _LinearFilter;
                return;
            }
        }
        GTargets.push_back(FDebugTarget{ _Name, _SrvSlot, _Decode, _SubIndex,
                                         _MipCount == 0 ? 1 : _MipCount, _Exposure,
                                         _AtlasTilePx, _LinearFilter });
    }

    void Clear() {
        std::lock_guard<std::mutex> Lock(GMutex);
        GTargets.clear();
    }

    const std::vector<FDebugTarget>& All() { return GTargets; }

    std::vector<FDebugTarget> Query(const std::string& _Filter) {
        std::lock_guard<std::mutex> Lock(GMutex);
        if (_Filter.empty()) return GTargets;

        const std::string Needle = ToLower(_Filter);
        std::vector<FDebugTarget> Out;
        for (const FDebugTarget& T : GTargets)
            if (ToLower(T.Name).find(Needle) != std::string::npos)
                Out.push_back(T);
        return Out;
    }

    u32 IndexOf(const std::string& _Name) {
        std::lock_guard<std::mutex> Lock(GMutex);
        for (size_t I = 0; I < GTargets.size(); ++I)
            if (GTargets[I].Name == _Name) return static_cast<u32>(I);
        return kInvalid;
    }
}
