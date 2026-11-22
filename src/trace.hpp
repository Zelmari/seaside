#ifndef SEASIDE_TRACE_HPP
#define SEASIDE_TRACE_HPP

#include "ipc.hpp"

#include <chrono>
#include <filesystem>

namespace seaside {

class StorageIo;

// Trace state and its timer belong to the Wayland event-loop thread. Export
// workers receive an immutable snapshot and return only data.
class Trace final {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    static constexpr auto time_limit = std::chrono::seconds(60);
    static constexpr std::size_t entry_limit = 20'000;
    static constexpr std::size_t byte_limit = 16 * 1024 * 1024;

    static std::expected<std::unique_ptr<Trace>, std::string>
    create(wl_event_loop *loop, std::filesystem::path state_directory, Now now = Clock::now,
           std::shared_ptr<StorageIo> storage = {});
    ~Trace();
    Trace(const Trace &) = delete;
    Trace &operator=(const Trace &) = delete;

    IpcResult start();
    IpcResult stop();
    IpcJson status();
    bool running() noexcept;

    // Gate event construction through running(). Each retained object gains an
    // observed_ns field from the compositor monotonic clock, as a decimal string.
    void record(IpcJson event) noexcept;
    void drop_sample() noexcept;
    std::optional<IpcResult> export_completed(IpcServer::Completion complete);

  private:
    struct Impl;
    explicit Trace(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace seaside
#endif
