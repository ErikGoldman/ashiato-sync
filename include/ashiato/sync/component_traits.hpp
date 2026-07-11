#pragma once

#include "ashiato/sync/types.hpp"

#include <string>
#include <type_traits>

namespace ashiato::sync {

#ifdef ASHIATO_SYNC_ENABLE_TRACING
void trace_rollback_reason(const char* reason);
void trace_rollback_reason(const std::string& reason);
#define TRACE_ROLLBACK_IF(condition, reason)            \
    do {                                                \
        if (condition) {                                \
            ::ashiato::sync::trace_rollback_reason(reason); \
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

template <typename>
inline constexpr bool sync_component_traits_must_be_specialized = false;

// Explicit opt-in for tests or homogeneous deployments that intentionally use
// the compiler's native object representation as their wire format.
template <typename T>
struct UnsafeNativeLayoutSyncComponentTraits {
    using Quantized = T;

    static void quantize(const T& value, Quantized& out) {
        static_assert(
            std::is_trivially_copyable<T>::value,
            "UnsafeNativeLayoutSyncComponentTraits requires a trivially copyable component");
        out = value;
    }

    static T dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(
        const Quantized* /*previous*/,
        const Quantized& current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& /*context*/) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "UnsafeNativeLayoutSyncComponentTraits requires a trivially copyable quantized state");
        out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        const Quantized* /*previous*/,
        Quantized& out,
        ashiato::ComponentSerializationContext& /*context*/) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "UnsafeNativeLayoutSyncComponentTraits requires a trivially copyable quantized state");
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }
};

template <typename T>
struct SyncComponentTraits {
    static_assert(
        sync_component_traits_must_be_specialized<T>,
        "SyncComponentTraits<T> must be explicitly specialized with a wire serializer");
};

template <>
struct SyncComponentTraits<NetworkOwner> {
    using Quantized = NetworkOwner;

    static void quantize(const NetworkOwner& value, Quantized& out) {
        out = value;
    }

    static NetworkOwner dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(
        const Quantized* /*previous*/,
        const Quantized& current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& /*context*/) {
        out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        const Quantized* /*previous*/,
        Quantized& out,
        ashiato::ComponentSerializationContext& /*context*/) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        return predicted.client != authoritative.client;
    }

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("client=");
        out.append_number(value.client);
    }
#endif
};

template <typename T>
struct SyncCueTraits;

}  // namespace ashiato::sync
