#include "ashiato/sync/profiling.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

int begin_a_count = 0;
int end_a_count = 0;
int begin_b_count = 0;
int end_b_count = 0;

void begin_b(const char*) noexcept {
    ++begin_b_count;
}

void end_b() noexcept {
    ++end_b_count;
}

void begin_a_and_replace_callbacks(const char*) noexcept {
    ++begin_a_count;
    ashiato::sync::set_profile_scope_callbacks(&begin_b, &end_b);
}

void end_a() noexcept {
    ++end_a_count;
}

void reset_counts() noexcept {
    begin_a_count = 0;
    end_a_count = 0;
    begin_b_count = 0;
    end_b_count = 0;
}

}  // namespace

TEST_CASE("profiling scopes retain the callback pair active at scope entry") {
    reset_counts();
    ashiato::sync::set_profile_scope_callbacks(&begin_a_and_replace_callbacks, &end_a);

    {
        ASHIATO_SYNC_PROFILE_SCOPE("outer");
    }
    REQUIRE(begin_a_count == 1);
    REQUIRE(end_a_count == 1);
    REQUIRE(begin_b_count == 0);
    REQUIRE(end_b_count == 0);

    {
        ASHIATO_SYNC_PROFILE_SCOPE("next");
    }
    REQUIRE(begin_b_count == 1);
    REQUIRE(end_b_count == 1);

    ashiato::sync::set_profile_scope_callbacks(nullptr, nullptr);
}

TEST_CASE("profiling rejects incomplete callback pairs") {
    reset_counts();
    ashiato::sync::set_profile_scope_callbacks(&begin_b, nullptr);
    {
        ASHIATO_SYNC_PROFILE_SCOPE("disabled");
    }
    REQUIRE(begin_b_count == 0);
    REQUIRE(end_b_count == 0);

    ashiato::sync::set_profile_scope_callbacks(nullptr, &end_b);
    {
        ASHIATO_SYNC_PROFILE_SCOPE("disabled");
    }
    REQUIRE(begin_b_count == 0);
    REQUIRE(end_b_count == 0);

    ashiato::sync::set_profile_scope_callbacks(nullptr, nullptr);
}
