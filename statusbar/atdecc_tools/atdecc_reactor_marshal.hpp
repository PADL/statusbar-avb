#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ReactorMarshal — cross-thread task delivery into the reactor loop.
///
/// A ControllerService backend whose native event delivery happens on a
/// foreign thread (e.g. a platform framework's dispatch queue) posts
/// tasks here from that thread; the reactor polls the marshal's fd and
/// runs every posted task on the reactor thread in on_ready(). This is
/// what keeps the ControllerService contract's "all sink callbacks and
/// completions are delivered single-threaded" promise on such backends.
///
/// Implementation: a self-pipe plus a mutex-guarded task queue. post()
/// is safe from any thread; on_ready() must only run on the reactor
/// thread (the Pollable contract already guarantees that).

#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace statusbar::atdecc_tools {

class ReactorMarshal final : public net::Pollable
{
  public:
    /// Task capacity is sized so a marshaled AEM command outcome — the
    /// completion closure plus an owned copy of a full 512-byte response
    /// payload — fits without heap allocation.
    static constexpr size_t TASK_CAPACITY = 768;
    using Task = statusbar::sg14::inplace_function<void(), TASK_CAPACITY>;

    ReactorMarshal()
    {
        std::array<int, 2> fds{-1, -1};
        if (::pipe(fds.data()) != 0) {
            return;
        }
        rd_ = fds[0];
        wr_ = fds[1];
        for (int const fd : fds) {
            (void)::fcntl(fd, F_SETFL, O_NONBLOCK);
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }

    ~ReactorMarshal() override
    {
        if (rd_ >= 0) {
            ::close(rd_);
        }
        if (wr_ >= 0) {
            ::close(wr_);
        }
    }

    ReactorMarshal(ReactorMarshal const&) = delete;
    auto operator=(ReactorMarshal const&) -> ReactorMarshal& = delete;
    ReactorMarshal(ReactorMarshal&&) = delete;
    auto operator=(ReactorMarshal&&) -> ReactorMarshal& = delete;

    /// True when the wakeup pipe opened successfully.
    [[nodiscard]] auto valid() const noexcept -> bool { return rd_ >= 0 && wr_ >= 0; }

    /// Queue @p task for execution on the reactor thread. Safe from any
    /// thread. A full pipe is harmless (the queue still grows; any byte
    /// already in the pipe wakes the reactor, which drains the whole queue).
    void post(Task task)
    {
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            queue_.push_back(std::move(task));
        }
        uint8_t const wake = 1;
        (void)::write(wr_, &wake, 1);
    }

    /// Number of tasks waiting (diagnostic; racy by nature).
    [[nodiscard]] auto pending() const -> size_t
    {
        std::lock_guard<std::mutex> const lock{mutex_};
        return queue_.size();
    }

    // ---- Pollable (reactor thread) ----------------------------------------

    [[nodiscard]] auto fd() const noexcept -> int override { return rd_; }

    void on_ready(int64_t /*now_ns*/) override
    {
        // Drain the wakeup bytes, then run every queued task, in order.
        std::array<uint8_t, 64> sink{};
        while (::read(rd_, sink.data(), sink.size()) > 0) {
        }
        std::vector<Task> tasks;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            tasks.swap(queue_);
        }
        for (auto& task : tasks) {
            task();
        }
    }

    void tick(int64_t /*now_ns*/) override {}

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    int rd_{-1};
    int wr_{-1};
    mutable std::mutex mutex_;
    std::vector<Task> queue_;
};

}  // namespace statusbar::atdecc_tools
