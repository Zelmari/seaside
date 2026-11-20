#include "config_store.hpp"
#include "config.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/fs.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace seaside {
namespace {
class Fd {
  public:
    explicit Fd(int value) noexcept : value_(value) {}
    ~Fd() {
        if (value_ >= 0)
            ::close(value_);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const noexcept { return value_; }
    int close() noexcept { return ::close(std::exchange(value_, -1)); }

  private:
    int value_;
};
std::error_code system_error() {
    return {errno, std::generic_category()};
}
StorageError failure(StorageStep step, const std::filesystem::path &path,
                     std::error_code error = system_error(), bool uncertain = false,
                     bool committed = false, bool cleanup_failed = false) {
    return {step, path, error, uncertain, committed, cleanup_failed};
}
bool same_file(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
bool same_inode(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

std::expected<void, StorageError> prepare_directory(const std::filesystem::path &directory,
                                                    StorageIo &io) {
    // fsync of the final parent only persists entries inside that directory.
    // Publish each newly created directory in its own parent before relying on it.
    std::vector<std::filesystem::path> missing;
    auto current = directory;
    for (;;) {
        std::error_code error;
        auto status = std::filesystem::status(current, error);
        if (error && error != std::errc::no_such_file_or_directory)
            return std::unexpected(failure(StorageStep::open_directory, current, error));
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status))
                return std::unexpected(failure(StorageStep::open_directory, current,
                                               std::make_error_code(std::errc::not_a_directory)));
            break;
        }
        missing.push_back(current);
        current = current.has_parent_path() ? current.parent_path() : std::filesystem::path(".");
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        std::error_code error;
        const bool created = std::filesystem::create_directory(*it, error);
        if (error)
            return std::unexpected(failure(StorageStep::open_directory, *it, error));
        if (created && ::chmod(it->c_str(), 0700) < 0)
            return std::unexpected(failure(StorageStep::open_directory, *it));
        const auto parent = it->has_parent_path() ? it->parent_path() : std::filesystem::path(".");
        // A concurrent creator can win mkdir; synchronize the same directory
        // chain without changing permissions on its already-existing directory.
        for (const auto &path : {*it, parent}) {
            Fd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (fd.get() < 0)
                return std::unexpected(failure(StorageStep::open_directory, path));
            auto sync_error = io.check(StorageStep::sync_ancestor);
            if (!sync_error && ::fsync(fd.get()) < 0)
                sync_error = system_error();
            if (sync_error)
                return std::unexpected(failure(StorageStep::sync_ancestor, path, sync_error));
        }
    }
    return {};
}
std::atomic<std::uint64_t> next_temporary{1};
} // namespace

ssize_t StorageIo::read(int fd, std::span<char> bytes) noexcept {
    return ::read(fd, bytes.data(), bytes.size());
}
ssize_t StorageIo::write(int fd, std::span<const char> bytes) noexcept {
    return ::write(fd, bytes.data(), bytes.size());
}
struct ConfigStore::PublicationState {
    std::mutex mutex;
};
ConfigStore::ConfigStore(std::shared_ptr<StorageIo> io)
    : io_(std::move(io)), publication_(std::make_shared<PublicationState>()) {
    if (!io_)
        io_ = std::make_shared<StorageIo>();
}

std::expected<std::optional<std::string>, StorageError>
ConfigStore::read(const std::filesystem::path &path, bool private_state) const {
    if (auto error = io_->check(StorageStep::open_read))
        return std::unexpected(failure(StorageStep::open_read, path, error));
    Fd fd(
        ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | (private_state ? O_NOFOLLOW : 0)));
    if (fd.get() < 0) {
        if (errno == ENOENT)
            return std::optional<std::string>{};
        return std::unexpected(failure(StorageStep::open_read, path));
    }
    struct stat before{}, after{};
    if (::fstat(fd.get(), &before) < 0)
        return std::unexpected(failure(StorageStep::read, path));
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > max_config_bytes)
        return std::unexpected(
            failure(StorageStep::read, path, std::make_error_code(std::errc::invalid_argument)));
    std::string text;
    std::array<char, 8192> buffer{};
    for (;;) {
        if (auto error = io_->check(StorageStep::read))
            return std::unexpected(failure(StorageStep::read, path, error));
        const auto count = io_->read(fd.get(), buffer);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(failure(StorageStep::read, path));
        }
        if (count == 0)
            break;
        if (static_cast<std::size_t>(count) > max_config_bytes - text.size())
            return std::unexpected(
                failure(StorageStep::read, path, std::make_error_code(std::errc::file_too_large)));
        text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (::fstat(fd.get(), &after) < 0)
        return std::unexpected(failure(StorageStep::read, path));
    if (!same_file(before, after) || text.size() != static_cast<std::size_t>(after.st_size))
        return std::unexpected(
            failure(StorageStep::read, path,
                    std::make_error_code(std::errc::resource_unavailable_try_again)));
    auto close_error = io_->check(StorageStep::close_file);
    if (fd.close() < 0 && !close_error)
        close_error = system_error();
    if (close_error)
        return std::unexpected(failure(StorageStep::close_file, path, close_error));
    return std::optional<std::string>(std::move(text));
}

std::expected<PublishResult, StorageError>
ConfigStore::publish(const std::filesystem::path &path, std::string_view text, PublishMode mode,
                     const std::function<bool()> &still_current) const {
    std::unique_lock publication_lock(publication_->mutex, std::defer_lock);
    if (!publication_lock.try_lock()) {
        // Main-loop callers enqueue work instead of entering this function.
        // A busy writer serializes here; contention is not a filesystem error.
        if (auto error = io_->check(StorageStep::waiting_for_publication))
            return std::unexpected(failure(StorageStep::waiting_for_publication, path, error));
        publication_lock.lock();
    }
    auto stale = [&]() noexcept {
        try {
            return still_current && !still_current();
        } catch (...) {
            return true;
        }
    };
    if (stale())
        return std::unexpected(failure(StorageStep::publish, path,
                                       std::make_error_code(std::errc::operation_canceled)));
    if (text.size() > max_config_bytes)
        return std::unexpected(
            failure(StorageStep::write, path, std::make_error_code(std::errc::file_too_large)));
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (auto error = io_->check(StorageStep::open_directory))
        return std::unexpected(failure(StorageStep::open_directory, path, error));
    if (auto prepared = prepare_directory(parent, *io_); !prepared)
        return std::unexpected(std::move(prepared.error()));
    Fd directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0)
        return std::unexpected(failure(StorageStep::open_directory, path));
    const auto name = path.filename().string();
    std::string temporary;
    int raw = -1;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temporary = ".seaside-" + std::to_string(::getpid()) + "-" +
                    std::to_string(next_temporary.fetch_add(1)) + ".tmp";
        if (auto error = io_->check(StorageStep::create_temporary))
            return std::unexpected(failure(StorageStep::create_temporary, path, error));
        raw = ::openat(directory.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       0600);
        if (raw >= 0 || errno != EEXIST)
            break;
    }
    if (raw < 0)
        return std::unexpected(failure(StorageStep::create_temporary, path));
    Fd file(raw);
    struct stat authored{};
    bool temporary_exists = true;
    auto remove_private = [&](const std::string &entry) -> std::error_code {
        if (auto error = io_->check(StorageStep::cleanup))
            return error;
        if (::unlinkat(directory.get(), entry.c_str(), 0) < 0)
            return system_error();
        return {};
    };
    auto remove_temporary = [&]() -> std::error_code {
        if (!temporary_exists)
            return {};
        auto error = remove_private(temporary);
        if (!error)
            temporary_exists = false;
        return error;
    };
    auto abort_write = [&](StorageStep step,
                           std::error_code error) -> std::expected<PublishResult, StorageError> {
        const auto cleanup_error = remove_temporary();
        return std::unexpected(failure(step, path, error, false, false, bool(cleanup_error)));
    };
    if (::fstat(file.get(), &authored) < 0)
        return abort_write(StorageStep::create_temporary, system_error());
    for (std::size_t offset = 0; offset < text.size();) {
        if (auto error = io_->check(StorageStep::write))
            return abort_write(StorageStep::write, error);
        auto count = io_->write(file.get(), std::span(text.data() + offset, text.size() - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return abort_write(StorageStep::write, system_error());
        }
        if (count == 0)
            return abort_write(StorageStep::write, std::make_error_code(std::errc::io_error));
        offset += static_cast<std::size_t>(count);
    }
    auto sync_error = io_->check(StorageStep::sync_file);
    if (!sync_error && ::fsync(file.get()) < 0)
        sync_error = system_error();
    if (sync_error)
        return abort_write(StorageStep::sync_file, sync_error);
    auto close_error = io_->check(StorageStep::close_file);
    if (file.close() < 0 && !close_error)
        close_error = system_error();
    if (close_error)
        return abort_write(StorageStep::close_file, close_error);
    if (stale())
        return abort_write(StorageStep::publish,
                           std::make_error_code(std::errc::operation_canceled));
    if (auto error = io_->check(StorageStep::publish))
        return abort_write(StorageStep::publish, error);

    // Keep the old inode until directory synchronization confirms publication.
    // A failed post-rename sync can then restore the previous recovery file.
    const auto backup = temporary + ".previous";
    const auto quarantined = temporary + ".rollback";
    bool has_backup = false;
    if (mode == PublishMode::replace_snapshot) {
        struct stat previous{};
        if (::fstatat(directory.get(), name.c_str(), &previous, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(previous.st_mode))
                return abort_write(StorageStep::publish,
                                   std::make_error_code(std::errc::invalid_argument));
            if (::linkat(directory.get(), name.c_str(), directory.get(), backup.c_str(), 0) < 0)
                return abort_write(StorageStep::publish, system_error());
            has_backup = true;
        } else if (errno != ENOENT)
            return abort_write(StorageStep::publish, system_error());
    }
    int published =
        mode == PublishMode::create_user_file
            ? ::linkat(directory.get(), temporary.c_str(), directory.get(), name.c_str(), 0)
            : ::renameat(directory.get(), temporary.c_str(), directory.get(), name.c_str());
    if (published < 0) {
        const auto error = system_error();
        const auto backup_cleanup = has_backup ? remove_private(backup) : std::error_code{};
        const auto temporary_cleanup = remove_temporary();
        if (mode == PublishMode::create_user_file && error.value() == EEXIST) {
            if (temporary_cleanup)
                return std::unexpected(
                    failure(StorageStep::cleanup, path, temporary_cleanup, false, false, true));
            return PublishResult::already_exists;
        }
        return std::unexpected(failure(StorageStep::publish, path, error, false, false,
                                       bool(backup_cleanup || temporary_cleanup)));
    }
    if (mode == PublishMode::replace_snapshot)
        temporary_exists = false;
    struct stat published_version{};
    const bool observed_publication =
        ::fstatat(directory.get(), name.c_str(), &published_version, AT_SYMLINK_NOFOLLOW) == 0 &&
        same_inode(published_version, authored);
    auto directory_sync_error = io_->check(StorageStep::sync_directory);
    if (!directory_sync_error && ::fsync(directory.get()) < 0)
        directory_sync_error = system_error();
    if (directory_sync_error) {
        bool uncertain = false;
        bool cleanup_failed = false;
        if (mode == PublishMode::create_user_file) {
            // Once visible, the user pathname may already have been edited or
            // atomically replaced. Never delete it to undo our failed fsync.
            uncertain = true;
        } else {
            struct stat current{};
            if (::fstatat(directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0 ||
                !same_inode(current, authored)) {
                uncertain = true;
            } else {
                // The inode can change after fstat. Move it without overwriting
                // another entry, then recheck before removing any file content.
                auto rollback_error = io_->check(StorageStep::rollback);
                if (rollback_error || ::renameat2(directory.get(), name.c_str(), directory.get(),
                                                  quarantined.c_str(), RENAME_NOREPLACE) < 0) {
                    uncertain = true;
                } else {
                    struct stat moved{};
                    const bool ours = ::fstatat(directory.get(), quarantined.c_str(), &moved,
                                                AT_SYMLINK_NOFOLLOW) == 0 &&
                                      same_inode(moved, authored);
                    if (!ours) {
                        // A writer won the fstat/rename race. Restore its entry
                        // only if the pathname is still empty; never clobber a
                        // still newer writer. Otherwise retain its data here.
                        if (::renameat2(directory.get(), quarantined.c_str(), directory.get(),
                                        name.c_str(), RENAME_NOREPLACE) < 0)
                            cleanup_failed = true;
                        uncertain = true;
                    } else {
                        if (has_backup) {
                            rollback_error = io_->check(StorageStep::rollback);
                            if (rollback_error ||
                                ::renameat2(directory.get(), backup.c_str(), directory.get(),
                                            name.c_str(), RENAME_NOREPLACE) < 0)
                                uncertain = true;
                            else
                                has_backup = false;
                        }
                        if (remove_private(quarantined))
                            cleanup_failed = true;
                    }
                }
            }
            if (::fsync(directory.get()) < 0)
                uncertain = true;
        }
        if (remove_temporary())
            cleanup_failed = true;
        if (has_backup) {
            // Failed rollback may leave this as the only copy of the previously
            // durable snapshot. Preserve it rather than deleting last-good data.
            if (uncertain || remove_private(backup))
                cleanup_failed = true;
        }
        return std::unexpected(failure(StorageStep::sync_directory, path, directory_sync_error,
                                       uncertain, false, cleanup_failed));
    }
    if (mode == PublishMode::replace_snapshot) {
        struct stat current{};
        if (!observed_publication ||
            ::fstatat(directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0 ||
            !same_file(current, published_version)) {
            // Directory fsync can succeed after another writer replaced or
            // changed the destination. That does not prove our requested
            // snapshot remains there. Keep its prior backup for recovery.
            return std::unexpected(failure(StorageStep::publish, path,
                                           std::make_error_code(std::errc::state_not_recoverable),
                                           true, false, has_backup));
        }
    }
    auto temporary_cleanup = remove_temporary();
    auto backup_cleanup = has_backup ? remove_private(backup) : std::error_code{};
    const auto cleanup_error = temporary_cleanup ? temporary_cleanup : backup_cleanup;
    // Successful directory fsync already established the new durable snapshot;
    // an unlink failure is a cleanup diagnostic, not lost persistence.
    if (cleanup_error)
        return std::unexpected(
            failure(StorageStep::cleanup, path, cleanup_error, false, true, true));
    return PublishResult::written;
}

} // namespace seaside
