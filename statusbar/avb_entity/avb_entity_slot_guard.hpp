#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// SlotGuard: a stream slot shared by its data-plane thread (try_enter, never waits) and the
/// reactor that re-shapes it (lock/unlock); see docs/STREAM_PORTS.md.

#include <atomic>
#include <cstdint>
#include <thread>

namespace statusbar::avb_entity {

class SlotGuard
{
  public:
    /// Data-plane thread: enter the slot, or false (skip the packet) while the
    /// reactor re-shapes it. Never waits.
    [[nodiscard]] auto try_enter() noexcept -> bool
    {
        uint8_t expected = IDLE;
        return state_.compare_exchange_strong(expected, IN_USE, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void leave() noexcept { state_.store(IDLE, std::memory_order_release); }

    /// Reactor: wait for the data-plane thread to leave, and keep it out until unlock().
    void lock() noexcept
    {
        uint8_t expected = IDLE;
        while (!state_.compare_exchange_weak(expected, RESHAPING, std::memory_order_acquire, std::memory_order_relaxed)) {
            expected = IDLE;
            std::this_thread::yield();
        }
    }
    void unlock() noexcept { state_.store(IDLE, std::memory_order_release); }

    /// try_enter() / leave() for a scope; entered() says whether to go on.
    class Entry
    {
      public:
        explicit Entry(SlotGuard& guard) noexcept
            : guard_{guard}
            , entered_{guard.try_enter()}
        {}
        ~Entry()
        {
            if (entered_) {
                guard_.leave();
            }
        }
        Entry(Entry const&) = delete;
        auto operator=(Entry const&) -> Entry& = delete;
        Entry(Entry&&) = delete;
        auto operator=(Entry&&) -> Entry& = delete;
        [[nodiscard]] auto entered() const noexcept -> bool { return entered_; }

      private:
        SlotGuard& guard_;
        bool entered_;
    };

    /// lock() / unlock() for a scope.
    class Lock
    {
      public:
        explicit Lock(SlotGuard& guard) noexcept
            : guard_{guard}
        {
            guard_.lock();
        }
        ~Lock() { guard_.unlock(); }
        Lock(Lock const&) = delete;
        auto operator=(Lock const&) -> Lock& = delete;
        Lock(Lock&&) = delete;
        auto operator=(Lock&&) -> Lock& = delete;

      private:
        SlotGuard& guard_;
    };

  private:
    static constexpr uint8_t IDLE = 0;
    static constexpr uint8_t IN_USE = 1;
    static constexpr uint8_t RESHAPING = 2;
    std::atomic<uint8_t> state_{IDLE};
};

}  // namespace statusbar::avb_entity
