#include "client/store/frame_ring_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

ashiato::sync::client_detail::EntityBufferedFrame make_sample(
    ashiato::sync::SyncFrame frame,
    bool entity_present,
    std::initializer_list<std::uint8_t> bytes = {}) {
    ashiato::sync::client_detail::EntityBufferedFrame sample;
    sample.frame = frame;
    sample.valid = true;
    sample.entity_present = entity_present;
    sample.baseline.tag_mask = 3;
    sample.baseline.present_mask = 5;
    sample.baseline.bytes.assign(bytes.begin(), bytes.end());
    return sample;
}

}  // namespace

TEST_CASE("ClientFrameRingStore rejects invalid capacities") {
    REQUIRE_THROWS_AS(ashiato::sync::client_detail::ClientFrameRingStore(0), std::invalid_argument);
    REQUIRE_THROWS_AS(ashiato::sync::client_detail::ClientFrameRingStore(3), std::invalid_argument);
    REQUIRE_NOTHROW(ashiato::sync::client_detail::ClientFrameRingStore(4));
}

TEST_CASE("ClientFrameRingStore lazily creates independent entity rings") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);
    ashiato::sync::client_detail::EntityBufferedFrame found;
    std::vector<ashiato::sync::client_detail::EntityBufferedFrame> frames;

    REQUIRE(store.capacity() == 4);
    REQUIRE(store.empty(0));
    REQUIRE(store.empty(7));
    REQUIRE_FALSE(store.copy_frames(0, frames));

    store.ensure(7);
    REQUIRE(store.empty(0));
    REQUIRE_FALSE(store.empty(7));
    REQUIRE(store.copy_frames(7, frames));
    REQUIRE(frames.size() == 4);

    store.write(7, make_sample(10, true, {1, 2, 3, 4}));

    REQUIRE_FALSE(store.contains(0, 10));
    REQUIRE(store.read(7, 10, found));
    REQUIRE(found.entity_present);
    REQUIRE(found.baseline.tag_mask == 3);
    REQUIRE(found.baseline.present_mask == 5);
    REQUIRE(found.baseline.bytes == std::vector<std::uint8_t>{1, 2, 3, 4});
}

TEST_CASE("ClientFrameRingStore rejects stale wrapped samples by frame") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);
    ashiato::sync::client_detail::EntityBufferedFrame found;

    store.write(2, make_sample(1, true, {9, 8}));
    REQUIRE(store.contains(2, 1));
    REQUIRE(store.read(2, 1, found));
    REQUIRE(found.baseline.bytes == std::vector<std::uint8_t>{9, 8});

    store.write(2, make_sample(5, false, {7, 6}));

    REQUIRE_FALSE(store.contains(2, 1));
    REQUIRE_FALSE(store.read(2, 1, found));
    REQUIRE(store.read(2, 5, found));
    REQUIRE_FALSE(found.entity_present);
    REQUIRE(found.baseline.bytes == std::vector<std::uint8_t>{7, 6});
}

TEST_CASE("ClientFrameRingStore clear invalidates samples without removing ring") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);
    std::vector<ashiato::sync::client_detail::EntityBufferedFrame> frames;

    ashiato::sync::client_detail::EntityBufferedFrame sample = make_sample(3, true, {1, 2, 3});
    store.write(1, sample);

    REQUIRE(store.contains(1, 3));
    REQUIRE(store.copy_frames(1, frames));

    store.clear(1);

    REQUIRE_FALSE(store.empty(1));
    REQUIRE(store.copy_frames(1, frames));
    REQUIRE_FALSE(store.contains(1, 3));
    for (const ashiato::sync::client_detail::EntityBufferedFrame& frame : frames) {
        REQUIRE_FALSE(frame.valid);
        REQUIRE_FALSE(frame.entity_present);
        REQUIRE(frame.baseline.tag_mask == 0);
        REQUIRE(frame.baseline.present_mask == 0);
    }
}

TEST_CASE("ClientFrameRingStore reset removes entity ring storage for index reuse") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);
    std::vector<ashiato::sync::client_detail::EntityBufferedFrame> frames;

    store.write(1, make_sample(3, true, {1, 2, 3}));
    REQUIRE_FALSE(store.empty(1));
    REQUIRE(store.copy_frames(1, frames));

    store.reset(1);

    REQUIRE(store.empty(1));
    REQUIRE_FALSE(store.copy_frames(1, frames));
}

TEST_CASE("ClientFrameRingStore clear_all invalidates every entity ring") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);

    store.write(1, make_sample(2, true));
    store.write(5, make_sample(6, true));

    REQUIRE(store.contains(1, 2));
    REQUIRE(store.contains(5, 6));

    store.clear_all();

    REQUIRE_FALSE(store.contains(1, 2));
    REQUIRE_FALSE(store.contains(5, 6));
    REQUIRE_FALSE(store.empty(1));
    REQUIRE_FALSE(store.empty(5));
}

TEST_CASE("ClientFrameRingStore stores payload bytes in stable per-entity slots") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);
    ashiato::sync::client_detail::EntityBufferedFrame found;

    store.write(1, make_sample(2, true, {1, 2, 3, 4}));
    store.write(1, make_sample(3, true, {5, 6, 7, 8}));

    REQUIRE(store.read(1, 2, found));
    REQUIRE(found.baseline.bytes == std::vector<std::uint8_t>{1, 2, 3, 4});

    REQUIRE(store.read(1, 3, found));
    REQUIRE(found.baseline.bytes == std::vector<std::uint8_t>{5, 6, 7, 8});
}

TEST_CASE("ClientFrameRingStore writes and reads non-owning frame views") {
    ashiato::sync::client_detail::ClientFrameRingStore store(4);
    ashiato::sync::client_detail::MutableEntityFrameView writable = store.begin_write(3, 12, 4);
    *writable.valid = true;
    *writable.entity_present = true;
    *writable.baseline.tag_mask = 11;
    *writable.baseline.present_mask = 1;
    writable.baseline.bytes[0] = 7;
    writable.baseline.bytes[1] = 8;
    writable.baseline.bytes[2] = 9;
    writable.baseline.bytes[3] = 10;

    ashiato::sync::client_detail::EntityFrameView view;
    REQUIRE(store.view(3, 12, view));
    REQUIRE(view.frame == 12);
    REQUIRE(view.entity_present);
    REQUIRE(view.baseline.tag_mask == 11);
    REQUIRE(view.baseline.present_mask == 1);
    REQUIRE(view.baseline.byte_count == 4);
    REQUIRE(view.baseline.bytes[0] == 7);
    REQUIRE(view.baseline.bytes[3] == 10);

    ashiato::sync::client_detail::EntityFrameView latest;
    REQUIRE(store.latest_present(3, latest));
    REQUIRE(latest.frame == 12);
}
