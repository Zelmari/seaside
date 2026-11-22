#include "live_config.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace seaside {
namespace {
constexpr int debounce_ms = 120;
// One job can hold its TOML parser, a compressed wallpaper, the decoded
// image, and codec scratch space. Snapshot jobs reserve their bytes separately.
constexpr std::size_t reload_reservation = 256 * 1024 * 1024;
constexpr auto directory_events = IN_ONLYDIR | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MODIFY |
                                  IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF |
                                  IN_ATTRIB;
constexpr auto file_events = IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF | IN_ATTRIB;
constexpr auto lost_watch = IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED;
constexpr auto replaced_entry = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
using Source = std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)>;

std::error_code work_error(WorkError error) {
    if (error == WorkError::full)
        return std::make_error_code(std::errc::no_buffer_space);
    if (error == WorkError::allocation)
        return std::make_error_code(std::errc::not_enough_memory);
    return std::make_error_code(std::errc::io_error);
}
} // namespace

struct LiveConfig::Impl {
    enum class WriteState { complete, created, target_created, modified };
    enum class Entry { config, target, ancestor };

    struct DirectoryWatch {
        int descriptor = -1;
        std::string entry;
        Entry kind = Entry::ancestor;
        bool follows_link = false;
    };

    Impl(std::unique_ptr<WorkQueue> queue, ConfigStore store, ConfigPaths paths,
         LoadedConfig startup, Prepare prepare, Apply apply, Changed changed)
        : queue_(std::move(queue)), io_(*queue_, std::move(store)), paths_(std::move(paths)),
          prepare_(std::move(prepare)), apply_(std::move(apply)), changed_(std::move(changed)),
          source_(startup.source), accepted_text_(std::move(startup.text)),
          transaction_(std::move(startup)) {}

    ~Impl() {
        watch_source_.reset();
        timer_.reset();
        if (watch_fd_ >= 0)
            close(watch_fd_);
    }

    bool start(wl_event_loop *loop) {
        watch_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (watch_fd_ < 0 || !refresh_watches())
            return false;
        watch_source_.reset(
            wl_event_loop_add_fd(loop, watch_fd_, WL_EVENT_READABLE, on_watch, this));
        timer_.reset(wl_event_loop_add_timer(loop, on_timer, this));
        // Recheck once after installing the watches, in case the editor saved
        // between the startup read and session setup.
        return watch_source_ && timer_ && arm_reload();
    }

    void watch_directories(const std::filesystem::path &path, DirectoryWatch &parent_watch,
                           DirectoryWatch &ancestor_watch, Entry kind) const {
        const auto directory = path.parent_path();
        const bool follows_link = kind == Entry::target;
        parent_watch = {inotify_add_watch(watch_fd_, directory.c_str(), directory_events),
                        path.filename().string(), kind, follows_link};

        // Keep a watch above the config directory so remove/rename and later
        // recreation can be tracked even while the immediate path is absent.
        for (auto child = directory; child.has_parent_path() && child != child.parent_path();) {
            auto ancestor = child.parent_path();
            const int descriptor = inotify_add_watch(watch_fd_, ancestor.c_str(), directory_events);
            if (descriptor >= 0) {
                ancestor_watch = {descriptor, child.filename().string(), Entry::ancestor,
                                  follows_link};
                break;
            }
            child = std::move(ancestor);
        }
    }

    std::filesystem::path symlink_target() const {
        auto target = paths_.user;
        // Follow the same bounded number of final-component links as Linux.
        for (int links = 0; links <= 40; ++links) {
            std::error_code error;
            auto next = std::filesystem::read_symlink(target, error);
            if (error)
                return target == paths_.user ? std::filesystem::path{} : target;
            target = next.is_absolute() ? std::move(next) : target.parent_path() / next;
        }
        return {};
    }

    bool refresh_watches() {
        decltype(directories_) next;
        watch_directories(paths_.user, next[0], next[1], Entry::config);
        // The inode watch disappears when a dotfile target is unlinked.
        // Its containing directory observes the next create and write-close.
        target_path_ = symlink_target();
        if (!target_path_.empty())
            watch_directories(target_path_, next[2], next[3], Entry::target);
        const int file = inotify_add_watch(watch_fd_, paths_.user.c_str(), file_events);
        for (const auto &old : directories_) {
            if (old.descriptor >= 0 && std::ranges::none_of(next, [&](const auto &watch) {
                    return watch.descriptor == old.descriptor;
                }))
                inotify_rm_watch(watch_fd_, old.descriptor);
        }
        if (file_watch_ >= 0 && file_watch_ != file)
            inotify_rm_watch(watch_fd_, file_watch_);
        directories_ = std::move(next);
        file_watch_ = file;
        return directories_[0].descriptor >= 0 || directories_[1].descriptor >= 0;
    }

    static int on_watch(int, std::uint32_t mask, void *data) noexcept {
        auto &self = *static_cast<Impl *>(data);
        try {
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
                self.watch_source_.reset();
                self.watch_failed();
            } else {
                self.drain_watch();
            }
        } catch (...) {
            std::fputs("seaside: could not process a configuration file event\n", stderr);
        }
        return 0;
    }

    static int on_timer(void *data) noexcept {
        auto &self = *static_cast<Impl *>(data);
        if (self.write_state_ != WriteState::complete)
            return 0;
        self.due_ = true;
        self.submit_reload();
        return 0;
    }

    void drain_watch() {
        bool changed = false;
        bool rewatch = false;
        bool replaced = false;
        bool config_replaced = false;
        std::array<char, 16 * 1024> buffer{};
        // Bound the amount of input processed in one dispatch, including when
        // other applications are busy writing to the watched ancestor.
        for (int budget = 0; budget < 4; ++budget) {
            const auto count = read(watch_fd_, buffer.data(), buffer.size());
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            if (count <= 0) {
                watch_source_.reset();
                watch_failed();
                return;
            }
            for (std::size_t offset = 0;
                 offset + sizeof(inotify_event) <= static_cast<std::size_t>(count);) {
                inotify_event event{};
                std::memcpy(&event, buffer.data() + offset, sizeof(event));
                const auto *name = buffer.data() + offset + sizeof(event);
                offset += sizeof(event) + event.len;
                if (event.mask & IN_Q_OVERFLOW) {
                    changed = true;
                    rewatch = true;
                } else if (event.wd == file_watch_) {
                    // The file watch can still deliver events from an unlinked inode.
                    if (replaced)
                        continue;
                    changed = true;
                    rewatch |= (event.mask & (lost_watch | IN_ATTRIB)) != 0;
                    record_write(event.mask, WriteState::created);
                } else {
                    for (const auto &directory : directories_) {
                        if (event.wd != directory.descriptor ||
                            (config_replaced && directory.follows_link))
                            continue;
                        if ((event.mask & lost_watch) ||
                            (event.len && std::string_view(name) == directory.entry)) {
                            changed = true;
                            const bool replacement =
                                (event.mask & (lost_watch | replaced_entry)) != 0;
                            rewatch |= replacement || (event.mask & IN_ATTRIB);
                            replaced |= replacement;
                            config_replaced |= replacement && !directory.follows_link;
                            if (directory.kind != Entry::ancestor)
                                record_write(event.mask, directory.kind == Entry::target
                                                             ? WriteState::target_created
                                                             : WriteState::created);
                            else if (replacement)
                                write_state_ = WriteState::complete;
                        }
                    }
                }
            }
            if (budget == 3) {
                // Events still queued may describe the config. Do not publish
                // a prepared candidate until a new scan has caught up.
                changed = true;
                rewatch = true;
            }
        }
        if (rewatch)
            refresh_watches();
        if ((write_state_ == WriteState::created && !created_regular_file(paths_.user)) ||
            (write_state_ == WriteState::target_created && !created_regular_file(target_path_)))
            write_state_ = WriteState::complete;
        if (changed && !arm_reload())
            watch_failed();
    }

    void record_write(std::uint32_t mask, WriteState creation) noexcept {
        if (mask & (IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | lost_watch)) {
            write_state_ = WriteState::complete;
        } else if (mask & IN_MODIFY) {
            write_state_ = WriteState::modified;
        } else if (mask & IN_CREATE) {
            write_state_ = creation;
        }
    }

    static bool created_regular_file(const std::filesystem::path &path) noexcept {
        struct stat status{};
        // A symlink or hard link publishes an existing target without a write-close.
        return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 1;
    }

    bool arm_reload() {
        due_ = false;
        // A create or modify invalidates any older worker immediately. Only a
        // completed save may arm another candidate, even if a prefix parses.
        const int delay = write_state_ == WriteState::complete ? debounce_ms : 0;
        return transaction_.begin_reload() &&
               wl_event_source_timer_update(timer_.get(), delay) == 0;
    }

    void watch_failed() {
        due_ = false;
        wl_event_source_timer_update(timer_.get(), 0);
        if (const auto generation = transaction_.begin_reload())
            reject(*generation, error("configuration watch failed"));
    }

    Diagnostic error(std::string message) const {
        return {paths_.user.string(), 0, 0, "", std::move(message)};
    }

    void notify(Change change, const Prepared &prepared = {}) noexcept {
        try {
            if (changed_)
                changed_(change, prepared);
        } catch (...) {
            std::fputs("seaside: configuration observer failed\n", stderr);
        }
    }

    void reject(std::uint64_t generation, Diagnostic diagnostic) {
        if (generation != transaction_.generation())
            return;
        if (diagnostic.path.empty())
            diagnostic.path = paths_.user.string();
        if (transaction_.diagnostic() == diagnostic)
            return;
        transaction_.reject(generation, std::move(diagnostic));
        notify(Change::rejected);
    }

    void submit_reload() noexcept {
        if (!due_ || loading_)
            return;
        due_ = false;
        const auto generation = transaction_.generation();
        try {
            AsyncConfigIo::Prepare prepare;
            if (prepare_) {
                prepare = [worker = prepare_, active = transaction_.active()](
                              const Config &candidate) { return worker(candidate, active); };
            }
            const auto submitted = io_.load_prepared(
                paths_.user, generation, reload_reservation, std::move(prepare),
                [this, generation](auto result) { loaded(generation, std::move(result)); });
            if (submitted)
                loading_ = true;
            else
                reject(generation, error("could not queue configuration reload: " +
                                         work_error(submitted.error()).message()));
        } catch (...) {
            std::fputs("seaside: could not queue configuration reload\n", stderr);
        }
    }

    void loaded(std::uint64_t generation,
                std::expected<PreparedConfigCandidate, Diagnostic> result) noexcept {
        loading_ = false;
        try {
            // The queue and inotify may wake the loop together. Drain path
            // events before considering this result for publication.
            if (watch_source_)
                drain_watch();
            if (generation == transaction_.generation()) {
                if (result)
                    commit(std::move(*result));
                else
                    reject(generation, std::move(result.error()));
            }
        } catch (...) {
            std::fputs("seaside: could not apply configuration reload\n", stderr);
        }
        submit_reload();
    }

    void commit(PreparedConfigCandidate item) {
        if (source_ == ConfigSource::user && item.candidate.text == accepted_text_ &&
            !transaction_.diagnostic() && !transaction_.persistence_error())
            return;
        auto previous_diagnostic = transaction_.diagnostic();
        const auto generation = item.candidate.generation;
        auto text = std::move(item.candidate.text);
        const auto result = transaction_.commit(
            std::move(item.candidate), [&](const Config &active, const Config &candidate) {
                if (!apply_)
                    return std::expected<void, Diagnostic>{};
                auto ready = apply_(active, candidate, item.prepared);
                if (!ready && ready.error().path.empty())
                    ready.error().path = paths_.user.string();
                return ready;
            });
        if (!result) {
            if (previous_diagnostic != transaction_.diagnostic())
                notify(Change::rejected);
            return;
        }
        source_ = ConfigSource::user;
        accepted_generation_ = generation;
        accepted_text_ = std::move(text);
        notify(Change::committed, item.prepared);
        if (ready_)
            persist();
    }

    void session_ready() noexcept {
        if (ready_)
            return;
        ready_ = true;
        if (source_ == ConfigSource::user)
            persist();
    }

    void persist() noexcept {
        const auto generation = accepted_generation_;
        try {
            const auto submitted = io_.persist(
                paths_.recovery, accepted_text_, generation,
                [this, generation](auto error) { persisted(generation, std::move(error)); });
            if (!submitted)
                persisted(generation, StorageError{StorageStep::write, paths_.recovery,
                                                   work_error(submitted.error())});
        } catch (...) {
            std::fputs("seaside: could not queue recovery snapshot\n", stderr);
        }
    }

    void persisted(std::uint64_t generation, std::optional<StorageError> error) {
        if (generation != accepted_generation_)
            return;
        transaction_.persisted(generation, std::move(error));
        notify(Change::persistence);
    }

    std::unique_ptr<WorkQueue> queue_;
    AsyncConfigIo io_;
    ConfigPaths paths_;
    Prepare prepare_;
    Apply apply_;
    Changed changed_;
    ConfigSource source_;
    std::string accepted_text_;
    ConfigTransaction transaction_;
    std::uint64_t accepted_generation_ = 0;
    std::array<DirectoryWatch, 4> directories_;
    std::filesystem::path target_path_;
    Source watch_source_{nullptr, wl_event_source_remove};
    Source timer_{nullptr, wl_event_source_remove};
    int watch_fd_ = -1;
    int file_watch_ = -1;
    WriteState write_state_ = WriteState::complete;
    bool ready_ = false;
    bool due_ = false;
    bool loading_ = false;
};

std::expected<std::unique_ptr<LiveConfig>, std::string>
LiveConfig::create(wl_event_loop *loop, ConfigStore store, ConfigPaths paths, LoadedConfig startup,
                   Prepare prepare, Apply apply, Changed changed) {
    const auto budget = reload_reservation + 6 * max_config_bytes;
    auto queue =
        WorkQueue::create(loop, {.workers = 1, .max_tasks = 4, .max_retained_bytes = budget});
    if (!queue)
        return std::unexpected("could not create configuration worker queue");
    auto impl = std::make_unique<Impl>(std::move(*queue), std::move(store), std::move(paths),
                                       std::move(startup), std::move(prepare), std::move(apply),
                                       std::move(changed));
    if (!impl->start(loop))
        return std::unexpected("could not watch the configuration file");
    return std::unique_ptr<LiveConfig>(new LiveConfig(std::move(impl)));
}

LiveConfig::LiveConfig(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LiveConfig::~LiveConfig() = default;

const ConfigTransaction &LiveConfig::transaction() const noexcept {
    return impl_->transaction_;
}

ConfigSource LiveConfig::source() const noexcept {
    return impl_->source_;
}

void LiveConfig::set_runtime_night_light(bool enabled) noexcept {
    impl_->transaction_.set_runtime_night_light(enabled);
}

void LiveConfig::session_ready() noexcept {
    impl_->session_ready();
}

} // namespace seaside
