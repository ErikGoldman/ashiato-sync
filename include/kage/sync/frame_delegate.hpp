#pragma once

#include "kage/sync/types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace kage::sync {

struct ServerFrameContext {
    const ecs::Registry& registry;
    SyncFrame frame = 0;
    ecs::Registry::DirtyView dirty;
    QueuedSyncCueView cues;
};

class FrameDelegate {
public:
    using Listener = std::function<void(const ServerFrameContext&)>;

private:
    struct Entry {
        std::uint64_t id = 0;
        Listener listener;
    };

    struct State {
        std::uint64_t next_id = 1;
        std::vector<Entry> listeners;
    };

public:
    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
            : state_(std::move(other.state_)),
              id_(other.id_) {
            other.id_ = 0;
        }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                id_ = other.id_;
                other.id_ = 0;
            }
            return *this;
        }
        ~Subscription() {
            reset();
        }

        void reset() {
            if (id_ == 0) {
                return;
            }
            if (auto state = state_.lock()) {
                state->listeners.erase(
                    std::remove_if(
                        state->listeners.begin(),
                        state->listeners.end(),
                        [id = id_](const Entry& entry) {
                            return entry.id == id;
                        }),
                    state->listeners.end());
            }
            state_.reset();
            id_ = 0;
        }

        bool active() const noexcept {
            return id_ != 0 && !state_.expired();
        }

    private:
        friend class FrameDelegate;

        Subscription(std::shared_ptr<State> state, std::uint64_t id)
            : state_(std::move(state)),
              id_(id) {}

        std::weak_ptr<State> state_;
        std::uint64_t id_ = 0;
    };

    Subscription subscribe(Listener listener) {
        if (!listener) {
            return {};
        }
        const std::uint64_t id = state_->next_id++;
        state_->listeners.push_back(Entry{id, std::move(listener)});
        return Subscription(state_, id);
    }

    void publish(const ServerFrameContext& context) const {
        for (const Entry& entry : state_->listeners) {
            entry.listener(context);
        }
    }

private:
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace kage::sync
