#include "ashiato/sync/profiling.hpp"

#include <memory>

namespace ashiato::sync {
namespace {

struct ProfileScopeCallbacks {
    ProfileScopeBeginFn begin = nullptr;
    ProfileScopeEndFn end = nullptr;
};

std::shared_ptr<const ProfileScopeCallbacks> profile_scope_callbacks;

}  // namespace

void set_profile_scope_callbacks(ProfileScopeBeginFn begin, ProfileScopeEndFn end) {
    std::shared_ptr<const ProfileScopeCallbacks> callbacks;
    if (begin != nullptr && end != nullptr) {
        callbacks = std::make_shared<const ProfileScopeCallbacks>(ProfileScopeCallbacks{begin, end});
    }
    std::atomic_store_explicit(&profile_scope_callbacks, std::move(callbacks), std::memory_order_release);
}

ProfileScopeEndFn begin_profile_scope(const char* name) noexcept {
    const std::shared_ptr<const ProfileScopeCallbacks> callbacks =
        std::atomic_load_explicit(&profile_scope_callbacks, std::memory_order_acquire);
    if (callbacks != nullptr) {
        callbacks->begin(name);
        return callbacks->end;
    }
    return nullptr;
}

}  // namespace ashiato::sync
