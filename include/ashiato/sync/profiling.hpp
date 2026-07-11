#pragma once

#ifdef ASHIATO_SYNC_ENABLE_PROFILING

namespace ashiato::sync {

using ProfileScopeBeginFn = void (*)(const char* name) noexcept;
using ProfileScopeEndFn = void (*)() noexcept;

void set_profile_scope_callbacks(ProfileScopeBeginFn begin, ProfileScopeEndFn end);
ProfileScopeEndFn begin_profile_scope(const char* name) noexcept;

class ScopedProfileScope {
public:
    explicit ScopedProfileScope(const char* name) noexcept
        : end_(begin_profile_scope(name)) {
    }

    ScopedProfileScope(const ScopedProfileScope&) = delete;
    ScopedProfileScope& operator=(const ScopedProfileScope&) = delete;

    ~ScopedProfileScope() {
        if (end_ != nullptr) {
            end_();
        }
    }

private:
    ProfileScopeEndFn end_ = nullptr;
};

}  // namespace ashiato::sync

#define ASHIATO_SYNC_PROFILE_SCOPE_CONCAT_INNER(a, b) a##b
#define ASHIATO_SYNC_PROFILE_SCOPE_CONCAT(a, b) ASHIATO_SYNC_PROFILE_SCOPE_CONCAT_INNER(a, b)
#define ASHIATO_SYNC_PROFILE_SCOPE(name) \
    ::ashiato::sync::ScopedProfileScope ASHIATO_SYNC_PROFILE_SCOPE_CONCAT(_ashiato_sync_profile_scope_, __LINE__)(name)

#else

#define ASHIATO_SYNC_PROFILE_SCOPE(...) ((void)0)

#endif
