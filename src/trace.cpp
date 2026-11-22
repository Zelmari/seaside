#include "trace.hpp"

#include "config_store.hpp"
#include "storage_fd.hpp"
#include "work_queue.hpp"

#include <atomic>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <linux/fs.h>
#include <stop_token>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {

struct SampleBuffer {
    std::deque<std::string> lines;
    std::size_t bytes = 0;
};

struct TraceFile {
    int directory;
    const std::string *name;
    const struct stat *authored = nullptr;
    ~TraceFile() {
        struct stat current{};
        if (name && (!authored ||
                     (fstatat(directory, name->c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
                      current.st_dev == authored->st_dev && current.st_ino == authored->st_ino))) {
            unlinkat(directory, name->c_str(), 0);
        }
    }
};

std::unexpected<IpcError> export_error(std::string_view operation, std::error_code error = {}) {
    std::string message = "Trace export: ";
    message += operation;
    if (error) {
        message += " (" + error.message() + ")";
    }
    return std::unexpected(IpcError{"internal_error", std::move(message)});
}

std::error_code system_error() noexcept {
    return {errno, std::generic_category()};
}

std::error_code sync_fd(int fd, StorageIo &storage, StorageStep step) {
    if (auto error = storage.check(step)) {
        return error;
    }
    for (;;) {
        if (::fsync(fd) == 0) {
            return {};
        }
        if (errno != EINTR) {
            return system_error();
        }
    }
}

IpcResult write_trace(const std::filesystem::path &state, const SampleBuffer &samples,
                      StorageIo &storage, std::stop_token stop) {
    const auto cancelled = [] { return export_error("cancelled"); };
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (auto error = storage.check(StorageStep::open_directory)) {
        return export_error("could not open the private state directory", error);
    }
    if (auto prepared = prepare_storage_directory(state, storage); !prepared) {
        return export_error("could not prepare the state directory", prepared.error().error);
    }
    const StorageFd parent(open(state.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat state_status{};
    if (parent.get() < 0 || fstat(parent.get(), &state_status) < 0) {
        return export_error("could not open the state directory", system_error());
    }
    if (state_status.st_uid != getuid() || (state_status.st_mode & 0022) != 0) {
        return export_error(
            "the state directory must be owned by the session user and private to write");
    }
    const bool created = mkdirat(parent.get(), "traces", 0700) == 0;
    if (!created && errno != EEXIST) {
        return export_error("could not create the traces directory", system_error());
    }
    const StorageFd directory(
        openat(parent.get(), "traces", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat directory_status{};
    if (directory.get() < 0 || (created && fchmod(directory.get(), 0700) < 0) ||
        fstat(directory.get(), &directory_status) < 0) {
        return export_error("could not open the private traces directory", system_error());
    }
    if (directory_status.st_uid != getuid() || (directory_status.st_mode & 07777) != 0700) {
        return export_error(
            "the traces directory must be owned by the session user with mode 0700");
    }
    if (auto error = sync_fd(parent.get(), storage, StorageStep::sync_ancestor)) {
        return export_error("could not synchronize the state directory", error);
    }

    static std::atomic<std::uint64_t> next_name{1};
    const auto stamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Trace::Clock::now().time_since_epoch())
            .count();
    const auto name = "trace-" + std::to_string(getpid()) + "-" + std::to_string(stamp) + "-" +
                      std::to_string(next_name.fetch_add(1, std::memory_order_relaxed)) + ".jsonl";
    const auto temporary = "." + name + ".tmp";
    const auto path = state / "traces" / name;
    auto result = IpcJson{{"path", path.string()}};
    if (auto error = storage.check(StorageStep::create_temporary)) {
        return export_error("could not create a private temporary file", error);
    }
    StorageFd file(openat(directory.get(), temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) {
        return export_error("could not create a private temporary file", system_error());
    }
    struct stat file_status{};
    TraceFile cleanup{directory.get(), &temporary};
    if (fchmod(file.get(), 0600) < 0 || fstat(file.get(), &file_status) < 0) {
        return export_error("could not secure the temporary file", system_error());
    }
    cleanup.authored = &file_status;
    for (const auto &line : samples.lines) {
        auto remaining = std::span<const char>(line);
        while (!remaining.empty()) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            if (auto error = storage.check(StorageStep::write)) {
                return export_error("could not write the trace", error);
            }
            const auto written = storage.write(file.get(), remaining);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0 || static_cast<std::size_t>(written) > remaining.size()) {
                return export_error("could not write the trace",
                                    written < 0 ? system_error()
                                                : std::make_error_code(std::errc::io_error));
            }
            remaining = remaining.subspan(static_cast<std::size_t>(written));
        }
    }
    if (auto error = sync_fd(file.get(), storage, StorageStep::sync_file)) {
        return export_error("could not synchronize the trace file", error);
    }
    if (file.close() < 0) {
        return export_error("could not close the trace file", system_error());
    }
    if (auto error = storage.check(StorageStep::publish)) {
        return export_error("could not publish the trace", error);
    }
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (renameat2(directory.get(), temporary.c_str(), directory.get(), name.c_str(),
                  RENAME_NOREPLACE) < 0) {
        return export_error("could not publish the trace", system_error());
    }
    cleanup.name = &name;
    if (auto error = sync_fd(directory.get(), storage, StorageStep::sync_directory)) {
        return export_error("could not synchronize the traces directory", error);
    }
    struct stat visible{};
    if (lstat(path.c_str(), &visible) < 0 || visible.st_dev != file_status.st_dev ||
        visible.st_ino != file_status.st_ino) {
        return export_error("the published file is no longer at its generated path");
    }
    cleanup.name = nullptr;
    return result;
}

} // namespace

struct Trace::Impl {
    using Timer = std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)>;

    std::filesystem::path state_directory;
    Now now;
    std::shared_ptr<StorageIo> storage;
    std::shared_ptr<SampleBuffer> samples;
    Clock::time_point deadline{};
    std::string_view stop_reason;
    std::size_t dropped_samples = 0;
    Timer timer{nullptr, wl_event_source_remove};
    std::unique_ptr<WorkQueue> queue;
    std::shared_ptr<WorkLifetime> lifetime = std::make_shared<WorkLifetime>();

    void finish(std::string_view reason) noexcept {
        stop_reason = reason;
        wl_event_source_timer_update(timer.get(), 0);
    }

    bool active(Clock::time_point observation) noexcept {
        if (!samples || !stop_reason.empty()) {
            return false;
        }
        if (observation >= deadline) {
            finish("time");
            return false;
        }
        return true;
    }

    static int expire(void *data) noexcept {
        auto &trace = *static_cast<Impl *>(data);
        const auto now = trace.now();
        if (trace.active(now)) {
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(trace.deadline - now);
            if (wl_event_source_timer_update(trace.timer.get(),
                                             static_cast<int>(remaining.count())) < 0) {
                trace.finish("time");
            }
        }
        return 0;
    }
};

Trace::Trace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Trace::~Trace() = default;

std::expected<std::unique_ptr<Trace>, std::string>
Trace::create(wl_event_loop *loop, std::filesystem::path state_directory, Now now,
              std::shared_ptr<StorageIo> storage) {
    if (!loop || !state_directory.is_absolute() || !now) {
        return std::unexpected(
            "Tracing needs a Wayland loop, an absolute state directory and a clock");
    }
    auto impl = std::make_unique<Impl>();
    impl->state_directory = std::move(state_directory);
    impl->now = std::move(now);
    impl->storage = storage ? std::move(storage) : std::make_shared<StorageIo>();
    impl->timer.reset(wl_event_loop_add_timer(loop, Impl::expire, impl.get()));
    if (!impl->timer) {
        return std::unexpected("Could not register the trace deadline in the Wayland event loop");
    }
    auto queue = WorkQueue::create(loop, {.workers = 1, .max_tasks = 1});
    if (!queue) {
        return std::unexpected("Could not initialize the trace export worker");
    }
    impl->queue = std::move(*queue);
    return std::unique_ptr<Trace>(new Trace(std::move(impl)));
}

bool Trace::running() noexcept {
    return impl_->active(impl_->now());
}

void Trace::drop_sample() noexcept {
    if (running()) {
        ++impl_->dropped_samples;
    }
}

IpcJson Trace::status() {
    const bool active = running();
    return {{"state", active           ? "running"
                      : impl_->samples ? "stopped"
                                       : "empty"},
            {"entries", impl_->samples ? impl_->samples->lines.size() : 0},
            {"bytes", impl_->samples ? impl_->samples->bytes : 0},
            {"dropped_samples", impl_->dropped_samples},
            {"stop_reason",
             impl_->stop_reason.empty() ? IpcJson(nullptr) : IpcJson(impl_->stop_reason)}};
}

IpcResult Trace::start() {
    if (running()) {
        return std::unexpected(IpcError{"trace_already_running", "A trace is already running"});
    }
    auto buffer = std::make_shared<SampleBuffer>();
    const auto now = impl_->now();
    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(time_limit);
    if (wl_event_source_timer_update(impl_->timer.get(), static_cast<int>(timeout.count())) < 0) {
        return std::unexpected(IpcError{"internal_error", "Could not arm the trace deadline"});
    }
    impl_->samples = std::move(buffer);
    impl_->deadline = now + time_limit;
    impl_->dropped_samples = 0;
    impl_->stop_reason = {};
    return IpcJson{{"trace", status()}};
}

IpcResult Trace::stop() {
    if (!running()) {
        return std::unexpected(IpcError{"trace_not_running", "No trace is running"});
    }
    impl_->finish("requested");
    return IpcJson{{"trace", status()}};
}

void Trace::record(IpcJson event) noexcept {
    const auto now = impl_->now();
    if (!impl_->active(now)) {
        return;
    }
    if (!event.is_object()) {
        ++impl_->dropped_samples;
        return;
    }
    auto &samples = *impl_->samples;
    try {
        const auto stamp =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
        event["observed_ns"] = std::to_string(stamp.count());
        auto line = event.dump(-1, ' ', false, IpcJson::error_handler_t::replace);
        if (line.size() >= byte_limit - samples.bytes) {
            ++impl_->dropped_samples;
            impl_->finish("bytes");
            return;
        }
        line += '\n';
        const auto size = line.size();
        samples.lines.push_back(std::move(line));
        samples.bytes += size;
    } catch (...) {
        ++impl_->dropped_samples;
        return;
    }
    if (samples.lines.size() == entry_limit) {
        impl_->finish("entries");
    } else if (samples.bytes == byte_limit) {
        impl_->finish("bytes");
    }
}

std::optional<IpcResult> Trace::export_completed(IpcServer::Completion complete) {
    if (running()) {
        return IpcResult(
            std::unexpected(IpcError{"invalid_state", "Stop the trace before exporting it"}));
    }
    if (!impl_->samples) {
        return IpcResult(
            std::unexpected(IpcError{"trace_not_running", "No completed trace is available"}));
    }
    if (!complete) {
        return export_error("no completion callback was supplied");
    }
    const std::shared_ptr<const SampleBuffer> snapshot = impl_->samples;
    // Charge both the serialized bytes and string metadata for the single
    // export. Replacing the recorded trace can leave only this in-flight copy.
    const auto reservation =
        snapshot->bytes + snapshot->lines.size() * sizeof(std::string) + 64 * 1024;
    auto submitted = impl_->queue->submit(
        0, impl_->lifetime, reservation,
        [snapshot, state = impl_->state_directory, storage = impl_->storage](std::stop_token stop) {
            auto result =
                std::make_shared<const IpcResult>(write_trace(state, *snapshot, *storage, stop));
            return WorkResult{.value = std::move(result), .retained_bytes = 4096};
        },
        [complete = std::move(complete)](WorkResult result) {
            if (result.error != WorkError::none || !result.value) {
                complete(export_error("the export worker could not finish"));
                return;
            }
            complete(*std::static_pointer_cast<const IpcResult>(result.value));
        });
    if (!submitted) {
        return export_error(submitted.error() == WorkError::full ? "another export is in progress"
                                                                 : "could not queue the export");
    }
    return std::nullopt;
}

} // namespace seaside
