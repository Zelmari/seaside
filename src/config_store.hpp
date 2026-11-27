#ifndef SEASIDE_CONFIG_STORE_HPP
#define SEASIDE_CONFIG_STORE_HPP

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>

namespace seaside {

enum class StorageStep {
    open_read,
    open_directory,
    create_temporary,
    read,
    write,
    sync_file,
    close_file,
    publish,
    sync_directory,
    cleanup,
    sync_ancestor,
    rollback,
    waiting_for_publication,
    check_writer,
};
struct StorageError {
    StorageStep operation;
    std::filesystem::path path;
    std::error_code error;
    // Cleanup can fail after a confirmed durable publication. Keep that known
    // success distinct from failed rollback and from leftover private entries.
    bool recovery_uncertain = false;
    bool publication_committed = false;
    bool cleanup_failed = false;
};
// On filesystems without leases, a known publication can still be read using
// the inotify close/rename boundary. An uncertain live edit must await a new save.
enum class WriterCheck { skip, when_available, required };
enum class PublishMode { replace_snapshot, create_user_file };
enum class PublishResult { written, already_exists };

// Internal collaborators allow real filesystem tests and precise failure/short
// I/O cases. They do not add production configuration or global test switches.
class StorageIo {
  public:
    virtual ~StorageIo() = default;
    virtual std::error_code check(StorageStep) noexcept { return {}; }
    virtual ssize_t read(int fd, std::span<char> bytes) noexcept;
    virtual ssize_t write(int fd, std::span<const char> bytes) noexcept;
};

// Create missing state directories with private permissions and persist their
// directory entries. Existing directories retain their original permissions.
std::expected<void, StorageError> prepare_storage_directory(const std::filesystem::path &directory,
                                                            StorageIo &io);

class ConfigStore {
  public:
    explicit ConfigStore(std::shared_ptr<StorageIo> io = std::make_shared<StorageIo>());
    // Reading symlinked user dotfiles is supported. Private recovery state must
    // be a regular file, not a symlink. Nonblocking opens reject FIFOs/devices.
    std::expected<std::optional<std::string>, StorageError>
    read(const std::filesystem::path &path, bool private_state = false,
         WriterCheck writer_check = WriterCheck::skip) const;
    // Blocking serialization belongs on workers (or before the event loop).
    // StorageIo hooks and generation predicates must not reenter publication.
    std::expected<PublishResult, StorageError>
    publish(const std::filesystem::path &path, std::string_view text, PublishMode mode,
            const std::function<bool()> &still_current = {}) const;
    // Replace a completed user save only if its contents still match the
    // configuration the session accepted. A dotfile symlink stays in place.
    std::expected<PublishResult, StorageError>
    replace_user(const std::filesystem::path &path, std::string_view expected,
                 std::string_view text, const std::function<bool()> &still_current = {}) const;
    // As above, but require a regular file at the final path component.
    std::expected<PublishResult, StorageError>
    replace_regular(const std::filesystem::path &path, std::string_view expected,
                    std::string_view text, const std::function<bool()> &still_current = {}) const;

  private:
    std::expected<PublishResult, StorageError>
    publish_at(const std::filesystem::path &path, std::string_view text, PublishMode mode,
               const std::function<bool()> &still_current,
               const std::filesystem::path *user_link = nullptr,
               std::string_view expected = {}) const;
    std::shared_ptr<StorageIo> io_;
    struct PublicationState;
    std::shared_ptr<PublicationState> publication_;
};

} // namespace seaside
#endif
