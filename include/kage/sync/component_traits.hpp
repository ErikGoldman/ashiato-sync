#pragma once

#include "kage/sync/types.hpp"

#include <string>
#include <type_traits>

namespace kage::sync {

#ifdef KAGE_SYNC_ENABLE_TRACING
void trace_rollback_reason(const char* reason);
void trace_rollback_reason(const std::string& reason);
#define TRACE_ROLLBACK_IF(condition, reason)            \
    do {                                                \
        if (condition) {                                \
            ::kage::sync::trace_rollback_reason(reason); \
            return true;                                \
        }                                               \
    } while (false)
#else
#define TRACE_ROLLBACK_IF(condition, reason) \
    do {                                     \
        if (condition) {                     \
            return true;                     \
        }                                    \
    } while (false)
#endif

template <typename T>
struct SyncComponentTraits {
    using Quantized = T;

    static Quantized quantize(const T& value) {
        static_assert(
            std::is_trivially_copyable<T>::value,
            "default SyncComponentTraits require a trivially copyable component");
        return value;
    }

    static T dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* /*previous*/, const Quantized& current, BitBuffer& out) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "default SyncComponentTraits serialization requires a trivially copyable quantized state");
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(BitBuffer& in, const Quantized* /*previous*/, Quantized& out) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "default SyncComponentTraits deserialization requires a trivially copyable quantized state");
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }
};

template <typename T>
struct SyncCueTraits;

}  // namespace kage::sync
