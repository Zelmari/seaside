#ifndef SEASIDE_CONFIG_TRANSACTION_HPP
#define SEASIDE_CONFIG_TRANSACTION_HPP

#include "config.hpp"
#include "config_store.hpp"
#include "work_queue.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace seaside {
enum class ConfigSource { user, recovery, defaults };
struct ConfigPaths {
    std::filesystem::path user, recovery;
};
struct LoadedConfig {
    Config config;
    std::string text;
    ConfigSource source = ConfigSource::defaults;
    std::optional<Diagnostic> diagnostic;
};
struct ConfigCandidate {
    std::uint64_t generation;
    Config config;
    std::string text;
};
struct PreparedConfigCandidate {
    ConfigCandidate candidate;
    std::shared_ptr<const void> prepared;
};

LoadedConfig load_startup_config(const ConfigStore &store, const ConfigPaths &paths);
std::expected<ConfigCandidate, Diagnostic> load_config_candidate(const ConfigStore &store,
                                                                 const std::filesystem::path &path,
                                                                 std::uint64_t generation);

struct ConfigEffects {
    bool reset_tiling_overrides = false;
    bool night_light_changed = false;
};

// Logical state only. The caller prepares or applies graphical changes on the
// owner thread and retains the prior effective state if that operation fails.
class ConfigTransaction {
  public:
    using Prepare = std::function<std::expected<void, Diagnostic>(const Config &, const Config &)>;
    explicit ConfigTransaction(LoadedConfig startup);
    std::expected<std::uint64_t, Diagnostic> begin_reload();
    std::expected<ConfigEffects, Diagnostic> commit(ConfigCandidate candidate,
                                                    const Prepare &prepare);
    void reject(std::uint64_t generation, Diagnostic diagnostic);
    void persisted(std::uint64_t generation, std::optional<StorageError> error);
    const Config &active() const noexcept { return active_; }
    bool recovery_persisted() const noexcept { return recovery_persisted_; }
    std::uint64_t generation() const noexcept { return generation_; }
    const std::optional<Diagnostic> &diagnostic() const noexcept { return diagnostic_; }
    const std::optional<StorageError> &persistence_error() const noexcept {
        return persistence_error_;
    }

  private:
    std::thread::id owner_ = std::this_thread::get_id();
    Config active_;
    bool recovery_persisted_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t committed_generation_ = 0;
    std::optional<Diagnostic> diagnostic_;
    std::optional<StorageError> persistence_error_;
};

// Call only from the queue's main-loop thread. Captured I/O inputs own their
// storage. Destruction cancels delivery; filesystem calls remain cooperative.
class AsyncConfigIo {
  public:
    using LoadCompletion = std::function<void(std::expected<ConfigCandidate, Diagnostic>)>;
    using Prepare =
        std::function<std::expected<std::shared_ptr<const void>, Diagnostic>(const Config &)>;
    // Empty success defers an incomplete save until the next filesystem event.
    using PreparedResult = std::expected<std::optional<PreparedConfigCandidate>, Diagnostic>;
    using PreparedCompletion = std::function<void(PreparedResult)>;
    using SaveCompletion = std::function<void(std::optional<StorageError>)>;
    AsyncConfigIo(WorkQueue &queue, ConfigStore store);
    ~AsyncConfigIo();
    AsyncConfigIo(const AsyncConfigIo &) = delete;
    AsyncConfigIo &operator=(const AsyncConfigIo &) = delete;
    std::expected<void, WorkError> load(std::filesystem::path path, std::uint64_t generation,
                                        LoadCompletion completion);
    std::expected<void, WorkError> load_prepared(std::filesystem::path path,
                                                 std::uint64_t generation,
                                                 std::size_t reservation_bytes,
                                                 WriterCheck writer_check, Prepare prepare,
                                                 PreparedCompletion completion);
    std::expected<void, WorkError> persist(std::filesystem::path path, std::string text,
                                           std::uint64_t generation, SaveCompletion completion);

  private:
    WorkQueue &queue_;
    ConfigStore store_;
    std::thread::id owner_ = std::this_thread::get_id();
    std::shared_ptr<WorkLifetime> lifetime_ = std::make_shared<WorkLifetime>();
    std::shared_ptr<std::atomic<std::uint64_t>> generation_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<std::atomic<std::uint64_t>> accepted_generation_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
};
} // namespace seaside
#endif
