#pragma once

// Tipos basicos e aliases compartilhados pela engine.

#include <cstdint>
#include <wrl/client.h>

namespace Smile {

// Aliases numericos curtos
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// Smart pointer COM padrao (encapsula AddRef/Release)
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

} // namespace Smile
