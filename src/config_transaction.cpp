#include "config_transaction.hpp"

#include <algorithm>
#include <latch>
#include <limits>
#include <utility>

namespace seaside {
namespace {
Diagnostic storage_diagnostic(const StorageError &error) {
    auto path = error.path.string();
    auto length = std::min<std::size_t>(path.size(), 4096);
    while (length && length < path.size() &&
           (static_cast<unsigned char>(path[length]) & 0xc0U) == 0x80U)
        --length;
    path.resize(length);
    return {std::move(path), 0, 0, "", "configuration I/O: " + error.error.message()};
}
Diagnostic transaction_error(std::string message) {
    return {"config.toml", 0, 0, "", std::move(message)};
}
LoadedConfig defaults_with(std::optional<Diagnostic> diagnostic) {
    return {default_config(), std::string(default_template()), ConfigSource::defaults,
            std::move(diagnostic)};
}
std::expected<ConfigCandidate, Diagnostic>
parse_candidate(const std::filesystem::path &path, std::uint64_t generation,
                std::expected<std::optional<std::string>, StorageError> text) {
    if (!text)
        return std::unexpected(storage_diagnostic(text.error()));
    if (!*text)
        return std::unexpected(Diagnostic{path.string(), 0, 0, "", "configuration is missing"});
    auto config = parse_config(**text, path.string());
    if (!config)
        return std::unexpected(std::move(config.error()));
    return ConfigCandidate{generation, std::move(*config), std::move(**text)};
}
} // namespace

LoadedConfig load_startup_config(const ConfigStore &store, const ConfigPaths &paths) {
    auto user = store.read(paths.user, false, WriterCheck::when_available);
    if (user && !*user) {
        auto created = store.publish(paths.user, default_template(), PublishMode::create_user_file);
        if (!created)
            return defaults_with(storage_diagnostic(created.error()));
        user = store.read(paths.user, false, WriterCheck::when_available);
    }
    std::optional<Diagnostic> error;
    if (!user)
        error = storage_diagnostic(user.error());
    else if (*user) {
        auto parsed = parse_config(**user, paths.user.string());
        if (parsed)
            return {std::move(*parsed), std::move(**user), ConfigSource::user, std::nullopt};
        error = std::move(parsed.error());
    } else
        error = transaction_error("configuration disappeared while loading");
    auto recovery = store.read(paths.recovery, true);
    if (recovery && *recovery) {
        auto parsed = parse_config(**recovery, paths.recovery.string());
        if (parsed)
            return {std::move(*parsed), std::move(**recovery), ConfigSource::recovery,
                    std::move(error)};
    }
    return defaults_with(std::move(error));
}

std::expected<ConfigCandidate, Diagnostic> load_config_candidate(const ConfigStore &store,
                                                                 const std::filesystem::path &path,
                                                                 std::uint64_t generation) {
    return parse_candidate(path, generation, store.read(path));
}

ConfigTransaction::ConfigTransaction(LoadedConfig startup)
    : active_(startup.config), requested_(std::move(startup.config)),
      runtime_night_light_(active_.night_light.enabled),
      recovery_persisted_(startup.source == ConfigSource::recovery),
      diagnostic_(std::move(startup.diagnostic)) {}

std::expected<std::uint64_t, Diagnostic> ConfigTransaction::begin_reload() {
    if (owner_ != std::this_thread::get_id())
        return std::unexpected(
            transaction_error("configuration state belongs to the main-loop thread"));
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(transaction_error("configuration generation exhausted"));
    return ++generation_;
}

std::expected<ConfigEffects, Diagnostic> ConfigTransaction::commit(ConfigCandidate candidate,
                                                                   const Prepare &prepare) {
    if (owner_ != std::this_thread::get_id() || candidate.generation != generation_ ||
        candidate.generation <= committed_generation_)
        return std::unexpected(transaction_error("stale or non-owner configuration commit"));
    if (!prepare)
        return std::unexpected(transaction_error("configuration preparation is required"));
    // Allocate before applying graphical changes. Successful preparation can
    // leave output state committed, so the logical publication must be no-throw.
    auto new_active = candidate.config;
    new_active.workspaces.mode = active_.workspaces.mode;
    auto ready = [&]() -> std::expected<void, Diagnostic> {
        try {
            return prepare(active_, candidate.config);
        } catch (...) {
            return std::unexpected(transaction_error("configuration preparation failed"));
        }
    }();
    if (candidate.generation != generation_ || candidate.generation <= committed_generation_)
        return std::unexpected(transaction_error("configuration superseded during preparation"));
    if (!ready) {
        diagnostic_ = ready.error();
        return std::unexpected(ready.error());
    }
    const bool reset = candidate.config.window.tiling != requested_.window.tiling ||
                       candidate.config.window.layout != requested_.window.layout;
    const bool change_night =
        candidate.config.night_light.enabled != requested_.night_light.enabled;
    const bool enabled = candidate.config.night_light.enabled;
    static_assert(std::is_nothrow_move_assignable_v<Config>);
    active_ = std::move(new_active);
    requested_ = std::move(candidate.config);
    committed_generation_ = candidate.generation;
    if (change_night)
        runtime_night_light_ = enabled;
    diagnostic_.reset();
    persistence_error_.reset();
    recovery_persisted_ = false;
    return ConfigEffects{reset, restart_pending(), change_night};
}
void ConfigTransaction::reject(std::uint64_t generation, Diagnostic diagnostic) {
    if (owner_ == std::this_thread::get_id() && generation == generation_)
        diagnostic_ = std::move(diagnostic);
}
void ConfigTransaction::persisted(std::uint64_t generation, std::optional<StorageError> error) {
    if (owner_ != std::this_thread::get_id() || generation != committed_generation_)
        return;
    recovery_persisted_ = !error || error->publication_committed;
    persistence_error_ = std::move(error);
}
void ConfigTransaction::set_runtime_night_light(bool enabled) {
    if (owner_ == std::this_thread::get_id())
        runtime_night_light_ = enabled;
}

AsyncConfigIo::AsyncConfigIo(WorkQueue &queue, ConfigStore store)
    : queue_(queue), store_(std::move(store)) {}
AsyncConfigIo::~AsyncConfigIo() {
    lifetime_->invalidate();
}

std::expected<void, WorkError> AsyncConfigIo::load(std::filesystem::path path,
                                                   std::uint64_t generation,
                                                   LoadCompletion completion) {
    if (!completion)
        return std::unexpected(WorkError::invalid_job);
    return load_prepared(std::move(path), generation, 64 * 1024 * 1024, WriterCheck::skip, {},
                         [completion = std::move(completion)](auto result) {
                             if (result)
                                 completion(std::move(result->value().candidate));
                             else
                                 completion(std::unexpected(std::move(result.error())));
                         });
}

std::expected<void, WorkError>
AsyncConfigIo::load_prepared(std::filesystem::path path, std::uint64_t generation,
                             std::size_t reservation_bytes, WriterCheck writer_check,
                             Prepare prepare, PreparedCompletion completion) {
    if (owner_ != std::this_thread::get_id())
        return std::unexpected(WorkError::wrong_thread);
    if (!completion)
        return std::unexpected(WorkError::invalid_job);
    if (generation <= generation_->load())
        return std::unexpected(WorkError::cancelled);
    auto attempts = generation_;
    auto store = store_;
    // This queue is dedicated to config I/O at transport generation zero.
    // Attempt generations filter parses; accepted generations govern writes.
    // An invalid new attempt must not cancel a prior valid snapshot job.
    auto submitted = queue_.submit(
        0, lifetime_, reservation_bytes,
        [store = std::move(store), path = std::move(path), generation, reservation_bytes,
         writer_check, prepare = std::move(prepare)](std::stop_token stop) -> WorkResult {
            if (stop.stop_requested())
                return {{}, 0, WorkError::cancelled};
            auto text = store.read(path, false, writer_check);
            if (!text && text.error().operation == StorageStep::check_writer)
                return {std::make_shared<PreparedResult>(), sizeof(PreparedResult),
                        WorkError::none};
            auto loaded = parse_candidate(path, generation, std::move(text));
            if (!loaded)
                return {
                    std::make_shared<PreparedResult>(std::unexpected(std::move(loaded.error()))),
                    reservation_bytes, WorkError::none};
            auto prepared = prepare ? prepare(loaded->config)
                                    : std::expected<std::shared_ptr<const void>, Diagnostic>{};
            if (!prepared)
                return {
                    std::make_shared<PreparedResult>(std::unexpected(std::move(prepared.error()))),
                    reservation_bytes, WorkError::none};
            return {std::make_shared<PreparedResult>(
                        PreparedConfigCandidate{std::move(*loaded), std::move(*prepared)}),
                    reservation_bytes, WorkError::none};
        },
        [attempts = std::move(attempts), generation,
         completion = std::move(completion)](WorkResult result) {
            if (generation != attempts->load())
                return;
            if (result.error != WorkError::none || !result.value)
                completion(std::unexpected(transaction_error("configuration worker failed")));
            else
                completion(*std::static_pointer_cast<const PreparedResult>(result.value));
        });
    if (submitted)
        generation_->store(generation);
    return submitted;
}

std::expected<void, WorkError> AsyncConfigIo::persist(std::filesystem::path path, std::string text,
                                                      std::uint64_t generation,
                                                      SaveCompletion completion) {
    if (owner_ != std::this_thread::get_id())
        return std::unexpected(WorkError::wrong_thread);
    if (!completion)
        return std::unexpected(WorkError::invalid_job);
    if (generation < accepted_generation_->load())
        return std::unexpected(WorkError::cancelled);
    if (text.size() > max_config_bytes)
        return std::unexpected(WorkError::result_too_large);
    auto store = store_;
    auto latest = accepted_generation_;
    auto admission = std::make_shared<std::latch>(1);
    auto error_path = path;
    std::weak_ptr<WorkLifetime> lifetime = lifetime_;
    auto submitted = queue_.submit(
        0, lifetime_, 2 * max_config_bytes,
        [store = std::move(store), latest, admission, lifetime = std::move(lifetime),
         path = std::move(path), text = std::move(text),
         generation](std::stop_token stop) -> WorkResult {
            // Publish acceptance only after successful queue admission, before
            // any worker can write. A rejected newer request cannot cancel an
            // older accepted save; a fast new worker cannot publish out of order.
            admission->wait();
            auto error = std::make_shared<std::optional<StorageError>>();
            auto current = [&] {
                auto owner = lifetime.lock();
                return owner && owner->active() && !stop.stop_requested() &&
                       latest->load() == generation;
            };
            auto saved = store.publish(path, text, PublishMode::replace_snapshot, current);
            if (!saved)
                *error = std::move(saved.error());
            return {std::move(error), sizeof(StorageError), WorkError::none};
        },
        [latest, generation, error_path = std::move(error_path),
         completion = std::move(completion)](WorkResult result) {
            if (generation != latest->load())
                return;
            if (result.error != WorkError::none || !result.value)
                completion(StorageError{StorageStep::write, error_path,
                                        std::make_error_code(std::errc::io_error), true});
            else
                completion(
                    *std::static_pointer_cast<const std::optional<StorageError>>(result.value));
        });
    if (submitted)
        accepted_generation_->store(generation);
    admission->count_down();
    return submitted;
}
} // namespace seaside
