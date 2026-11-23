#include "session_pipewire.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pipewire/extensions/metadata.h>
#include <pipewire/extensions/protocol-native.h>
#include <pipewire/pipewire.h>
#include <string_view>
#include <thread>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {
void initialize_pipewire() {
    static std::once_flag initialized;
    // Other components may use libpipewire later. Do not globally deinitialize
    // it when a connection probe finishes.
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
}

constexpr std::array configured_keys{"default.configured.audio.sink",
                                     "default.configured.audio.source"};
constexpr std::array effective_keys{"default.audio.sink", "default.audio.source"};
constexpr char metadata_type[] = "Spa:String:JSON";

std::optional<std::string> metadata_node(const std::optional<std::string> &value) {
    if (value) {
        const auto parsed = nlohmann::json::parse(*value, nullptr, false);
        const auto name = parsed.find("name");
        if (name != parsed.end() && name->is_string()) {
            return name->get<std::string>();
        }
    }
    return {};
}
} // namespace

struct PipeWireProbe::Impl : std::enable_shared_from_this<Impl> {
    using Source = std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)>;
    using Clock = std::chrono::steady_clock;
    wl_event_loop *wayland_loop;
    const std::thread::id owner = std::this_thread::get_id();
    pw_loop *pipewire_loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    pw_metadata *metadata = nullptr;
    spa_hook core_listener{};
    spa_hook registry_listener{};
    spa_hook metadata_listener{};
    bool listening = false;
    bool listening_registry = false;
    bool listening_metadata = false;
    bool metadata_ready = false;
    std::uint32_t metadata_id = SPA_ID_INVALID;
    int metadata_sequence = -1;
    struct AudioNode {
        std::string name;
        std::size_t route;
    };
    struct AudioRoute {
        std::optional<std::string> requested;
        std::optional<std::string> effective;
        std::optional<std::string> configured;
        Clock::time_point pending_until{};
        bool apply = true;
    };
    std::map<std::uint32_t, AudioNode> audio_nodes;
    std::array<AudioRoute, configured_keys.size()> audio;
    std::shared_ptr<AudioRouteOwnership> audio_ownership;
    wl_event_source *io_source = nullptr;
    wl_event_source *deadline_source = nullptr;
    wl_event_source *completion_source = nullptr;
    int sequence = -1;
    std::chrono::steady_clock::time_point deadline;
    PipeWireResult outcome;
    Completion completion;
    RuntimeFault runtime_fault;
    int runtime_error = 0;
    bool monitoring = false;
    bool delivery_failed = false;

    Impl(wl_event_loop *loop, Completion callback, RuntimeFault fault,
         std::shared_ptr<AudioRouteOwnership> ownership)
        : wayland_loop(loop), audio_ownership(std::move(ownership)),
          completion(std::move(callback)), runtime_fault(std::move(fault)) {}
    ~Impl() noexcept { shutdown(); }

    void unbind_metadata() noexcept {
        if (listening_metadata) {
            spa_hook_remove(&metadata_listener);
            listening_metadata = false;
        }
        if (auto *proxy = std::exchange(metadata, nullptr)) {
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
        }
        metadata_id = SPA_ID_INVALID;
        metadata_sequence = -1;
        metadata_ready = false;
        for (auto &route : audio) {
            route.configured.reset();
            route.effective.reset();
            route.pending_until = {};
            route.apply = true;
        }
    }

    void disconnect() noexcept {
        unbind_metadata();
        if (listening_registry) {
            spa_hook_remove(&registry_listener);
            listening_registry = false;
        }
        if (auto *proxy = std::exchange(registry, nullptr)) {
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
        }
        audio_nodes.clear();
        if (listening) {
            spa_hook_remove(&core_listener);
            listening = false;
        }
        if (core) {
            pw_core_disconnect(core);
            core = nullptr;
        }
        if (context) {
            pw_context_destroy(context);
            context = nullptr;
        }
        if (pipewire_loop) {
            pw_loop_destroy(pipewire_loop);
            pipewire_loop = nullptr;
        }
    }

    void detach_sources() noexcept {
        for (auto **source : {&io_source, &deadline_source, &completion_source}) {
            if (*source) {
                wl_event_source_remove(*source);
                *source = nullptr;
            }
        }
    }

    void shutdown() noexcept {
        detach_sources();
        disconnect();
        completion = {};
        runtime_fault = {};
        monitoring = false;
    }

    void settle(PipeWireStatus status, int error) noexcept {
        if (outcome.status != PipeWireStatus::pending) {
            return;
        }
        outcome = {status, error};
        if (io_source && wl_event_source_fd_update(io_source, 0) < 0) {
            outcome = {PipeWireStatus::failed, -errno};
        }
        if (deadline_source && wl_event_source_timer_update(deadline_source, 0) < 0) {
            outcome = {PipeWireStatus::failed, -errno};
        }
        // This timer is allocated before any protocol dispatch. A separate
        // Wayland turn prevents tearing down pw_loop from its own callback.
        if (wl_event_source_timer_update(completion_source, 1) < 0) {
            outcome = {PipeWireStatus::failed, -errno};
            delivery_failed = true;
        }
    }

    static int deliver_startup(void *data) noexcept {
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return 0;
        }
        // Nested Wayland dispatch must not free the current timer until
        // the completion callback has returned.
        const Source current(std::exchange(self->completion_source, nullptr),
                             wl_event_source_remove);
        self->delivery_failed = false;
        auto callback = std::move(self->completion);
        if (self->outcome.status == PipeWireStatus::ready && self->runtime_fault) {
            if (wl_event_source_fd_update(self->io_source, WL_EVENT_READABLE) == 0) {
                self->monitoring = true;
                self->observe_audio();
                if (auto *deadline = std::exchange(self->deadline_source, nullptr)) {
                    wl_event_source_remove(deadline);
                }
            } else {
                self->outcome = {PipeWireStatus::failed, -errno};
            }
        }
        if (!self->monitoring) {
            self->shutdown();
        }
        try {
            if (callback) {
                callback(self->outcome);
            }
        } catch (...) {
            // A user callback cannot unwind through libwayland's C dispatch.
            self->outcome = {PipeWireStatus::failed, -ECANCELED, true};
            self->shutdown();
        }
        return 0;
    }

    void observe_error(int code) noexcept {
        if (outcome.status == PipeWireStatus::pending) {
            settle(PipeWireStatus::failed, code);
        } else if (monitoring && !runtime_error) {
            runtime_error = code;
        } else if (outcome.status == PipeWireStatus::ready && completion && runtime_fault) {
            // A peer can send done and disconnect in the same iteration.
            outcome = {PipeWireStatus::failed, code};
        }
    }

    void deliver_fault() noexcept {
        auto callback = std::exchange(runtime_fault, {});
        const auto code = std::exchange(runtime_error, 0);
        shutdown();
        try {
            callback(code);
        } catch (...) {
            outcome.callback_failed = true;
        }
    }

    static int timeout(void *data) noexcept {
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return 0;
        }
        const Source current(std::exchange(self->deadline_source, nullptr), wl_event_source_remove);
        self->settle(PipeWireStatus::timed_out, -ETIMEDOUT);
        if (self->delivery_failed) {
            deliver_startup(data);
        }
        return 0;
    }

    static int dispatch(int, std::uint32_t mask, void *data) noexcept {
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return 0;
        }
        const bool pending = self->outcome.status == PipeWireStatus::pending;
        if (!pending && !self->monitoring) {
            return 0;
        }
        // Several ready sources may be returned in the same epoll batch. An
        // overdue reply must not win just because I/O precedes its timer.
        if (pending && std::chrono::steady_clock::now() >= self->deadline) {
            self->settle(PipeWireStatus::timed_out, -ETIMEDOUT);
        } else if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
            self->observe_error(-EPIPE);
        } else {
            // The PipeWire epoll fd becomes readable for its timers and I/O.
            // One nonblocking iteration preserves the outer loop.
            pw_loop_enter(self->pipewire_loop);
            const int result = pw_loop_iterate(self->pipewire_loop, 0);
            pw_loop_leave(self->pipewire_loop);
            if (result < 0 && result != -EINTR) {
                self->observe_error(result);
            }
        }
        if (self->delivery_failed) {
            deliver_startup(data);
        } else if (self->runtime_error) {
            self->deliver_fault();
        } else if (self->monitoring) {
            self->apply_audio();
        }
        return 0;
    }

    static void done(void *data, std::uint32_t id, int received_sequence) noexcept {
        auto *self = static_cast<Impl *>(data);
        if (id == PW_ID_CORE && received_sequence == self->metadata_sequence && self->metadata) {
            self->metadata_ready = true;
            for (std::size_t index = 0; index < self->audio.size(); ++index) {
                self->observe_configured(index, true);
            }
        }
        if (id == PW_ID_CORE && received_sequence == self->sequence) {
            if (std::chrono::steady_clock::now() >= self->deadline) {
                self->settle(PipeWireStatus::timed_out, -ETIMEDOUT);
            } else {
                self->settle(PipeWireStatus::ready, 0);
            }
        }
    }

    bool available(std::size_t index) const noexcept {
        const auto &requested = audio[index].requested;
        return requested && std::ranges::any_of(audio_nodes, [&](const auto &entry) {
                   return entry.second.route == index && entry.second.name == *requested;
               });
    }

    void observe_configured(std::size_t index, bool after_reconnect) noexcept {
        auto &route = audio[index];
        auto &ownership = audio_ownership->routes_[index];
        if (ownership.pending) {
            if (route.configured == ownership.pending->value) {
                if (ownership.pending->restore) {
                    ownership = {};
                } else {
                    ownership.written = std::move(ownership.pending->value);
                    ownership.pending.reset();
                }
                route.pending_until = {};
            } else if (after_reconnect || Clock::now() >= route.pending_until) {
                ownership.pending.reset();
                route.pending_until = {};
            }
        }
        if (!ownership.pending && ownership.written != route.configured) {
            // An external default selection supersedes our preceding write.
            ownership = {};
        }
    }

    void write_audio(std::size_t index, const std::optional<std::string> &value, bool restore) {
        auto &route = audio[index];
        auto &ownership = audio_ownership->routes_[index];
        AudioRouteOwnership::Write write{value, restore};
        auto previous = !restore && (!ownership.written || ownership.written != route.configured)
                            ? route.configured
                            : ownership.previous;
        if (pw_metadata_set_property(metadata, PW_ID_CORE, configured_keys[index],
                                     value ? metadata_type : nullptr,
                                     value ? value->c_str() : nullptr) >= 0) {
            ownership.previous = std::move(previous);
            ownership.pending = std::move(write);
            route.pending_until = Clock::now() + std::chrono::seconds(2);
            route.apply = false;
        }
    }

    void apply_audio() noexcept try {
        if (!metadata_ready) {
            return;
        }
        for (std::size_t index = 0; index < audio.size(); ++index) {
            auto &route = audio[index];
            auto &ownership = audio_ownership->routes_[index];
            if (!route.apply || ownership.pending) {
                continue;
            }
            if (available(index)) {
                if (metadata_node(route.configured) == route.requested) {
                    route.apply = false;
                    continue;
                }
                const std::optional<std::string> value =
                    nlohmann::json{{"name", *route.requested}}.dump();
                write_audio(index, value, false);
            } else if (ownership.written && route.configured == ownership.written &&
                       route.requested != metadata_node(ownership.written)) {
                write_audio(index, ownership.previous, true);
            }
        }
    } catch (...) {
        // A routing failure must not disrupt the PipeWire health probe.
    }

    static int metadata_property(void *data, std::uint32_t subject, const char *key,
                                 const char *type, const char *value) noexcept try {
        if (subject != PW_ID_CORE) {
            return 0;
        }
        auto &self = *static_cast<Impl *>(data);
        for (std::size_t index = 0; index < self.audio.size(); ++index) {
            auto &route = self.audio[index];
            const bool effective = !key || std::strcmp(key, effective_keys[index]) == 0;
            const bool configured = !key || std::strcmp(key, configured_keys[index]) == 0;
            if (!effective && !configured) {
                continue;
            }
            const auto json = value && type && std::strcmp(type, metadata_type) == 0
                                  ? std::optional<std::string>{value}
                                  : std::nullopt;
            if (effective) {
                route.effective = metadata_node(json);
            }
            if (configured) {
                route.configured = json;
                if (self.metadata_ready) {
                    self.observe_configured(index, false);
                }
            }
        }
        return 0;
    } catch (...) {
        return 0;
    }

    static void global(void *data, std::uint32_t id, std::uint32_t, const char *type,
                       std::uint32_t version, const spa_dict *props) noexcept try {
        auto &self = *static_cast<Impl *>(data);
        if (!props) {
            return;
        }
        if (!self.metadata && std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            const auto *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (name && std::strcmp(name, "default") == 0) {
                self.metadata = static_cast<pw_metadata *>(
                    pw_registry_bind(self.registry, id, type,
                                     std::min(version, std::uint32_t{PW_VERSION_METADATA}), 0));
                if (self.metadata) {
                    static const pw_metadata_events events{PW_VERSION_METADATA_EVENTS,
                                                           metadata_property};
                    self.metadata_id = id;
                    self.listening_metadata =
                        pw_metadata_add_listener(self.metadata, &self.metadata_listener, &events,
                                                 &self) >= 0;
                    self.metadata_sequence = pw_core_sync(self.core, PW_ID_CORE, 0);
                    if (!self.listening_metadata || self.metadata_sequence < 0) {
                        self.unbind_metadata();
                    }
                }
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const auto *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const auto *media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            if (!name || !media) {
                return;
            }
            if (const std::string_view kind(media);
                kind == "Audio/Sink" || kind == "Audio/Source" || kind == "Audio/Source/Virtual") {
                const std::size_t index = kind == "Audio/Sink" ? 0 : 1;
                self.audio_nodes.insert_or_assign(id, AudioNode{name, index});
                if (self.audio[index].requested == name) {
                    self.audio[index].apply = true;
                }
            }
        }
    } catch (...) {
    }

    static void global_remove(void *data, std::uint32_t id) noexcept {
        auto &self = *static_cast<Impl *>(data);
        self.audio_nodes.erase(id);
        if (id == self.metadata_id) {
            self.unbind_metadata();
        }
    }

    void observe_audio() noexcept {
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (registry) {
            static const pw_registry_events events{PW_VERSION_REGISTRY_EVENTS, global,
                                                   global_remove};
            listening_registry =
                pw_registry_add_listener(registry, &registry_listener, &events, this) >= 0;
        }
    }

    void refresh_audio() noexcept {
        if (metadata_ready) {
            for (std::size_t index = 0; index < audio.size(); ++index) {
                if (audio_ownership->routes_[index].pending &&
                    Clock::now() >= audio[index].pending_until) {
                    observe_configured(index, false);
                }
            }
            apply_audio();
        }
    }

    static void error(void *data, std::uint32_t id, int, int code, const char *) noexcept {
        auto *self = static_cast<Impl *>(data);
        if (!self->monitoring || id == PW_ID_CORE) {
            self->observe_error(code < 0 ? code : -EIO);
        }
    }

    std::expected<void, PipeWireError> initialize(const Options &options) noexcept {
        deadline = std::chrono::steady_clock::now() + options.timeout;
        const auto *daemon = std::getenv("PIPEWIRE_DAEMON");
        if (daemon && pw_properties_parse_bool(daemon)) {
            return std::unexpected(PipeWireError{PipeWireFailure::connection, -EINVAL});
        }
        pipewire_loop = pw_loop_new(nullptr);
        if (!pipewire_loop) {
            return std::unexpected(PipeWireError{PipeWireFailure::loop_creation, -errno});
        }
        // A control-only check does not need real-time scheduling or factories
        // for audio nodes/devices. Keep the standard native client protocol.
        auto *properties = pw_properties_new(
            "module.rt", "false", "module.client-node", "false", "module.client-device", "false",
            "module.adapter", "false", "module.metadata", runtime_fault ? "true" : "false",
            "module.session-manager", "false", nullptr);
        if (!properties) {
            return std::unexpected(PipeWireError{PipeWireFailure::allocation, -ENOMEM});
        }
        context = pw_context_new(pipewire_loop, properties, 0);
        if (!context) {
            return std::unexpected(PipeWireError{PipeWireFailure::context_creation, -errno});
        }
        const auto *context_properties = pw_context_get_properties(context);
        const char *remote = options.remote.empty()
                                 ? pw_properties_get(context_properties, PW_KEY_REMOTE_NAME)
                                 : options.remote.c_str();
        // An internal client can complete a roundtrip against this process.
        // Use PipeWire's normal native socket resolution without its internal
        // or portal connection modes.
        if (pw_properties_get_bool(context_properties, PW_KEY_CORE_DAEMON, false) ||
            (remote && std::string_view(remote) == "internal")) {
            return std::unexpected(PipeWireError{PipeWireFailure::connection, -EINVAL});
        }
        auto *connection_properties = pw_properties_new(
            PW_KEY_PROTOCOL, PW_TYPE_INFO_PROTOCOL_Native, PW_KEY_REMOTE_INTENTION, "generic",
            PW_KEY_REMOTE_NAME, remote ? remote : "", nullptr);
        if (!connection_properties) {
            return std::unexpected(PipeWireError{PipeWireFailure::allocation, -ENOMEM});
        }
        core = pw_context_connect(context, connection_properties, 0);
        if (!core) {
            return std::unexpected(PipeWireError{PipeWireFailure::connection, -errno});
        }
        completion_source = wl_event_loop_add_timer(wayland_loop, deliver_startup, this);
        deadline_source = wl_event_loop_add_timer(wayland_loop, timeout, this);
        const int fd = pw_loop_get_fd(pipewire_loop);
        if (!completion_source || !deadline_source || fd < 0) {
            return std::unexpected(PipeWireError{PipeWireFailure::event_source, -ENOMEM});
        }
        io_source = wl_event_loop_add_fd(wayland_loop, fd, WL_EVENT_READABLE, dispatch, this);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int delay = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
        if (!io_source || wl_event_source_timer_update(deadline_source, delay) < 0) {
            return std::unexpected(PipeWireError{PipeWireFailure::event_source, -errno});
        }
        static const pw_core_events events = [] {
            pw_core_events result{};
            result.version = PW_VERSION_CORE_EVENTS;
            result.done = done;
            result.error = error;
            return result;
        }();
        const int listened = pw_core_add_listener(core, &core_listener, &events, this);
        if (listened < 0) {
            return std::unexpected(PipeWireError{PipeWireFailure::connection, listened});
        }
        listening = true;
        sequence = pw_core_sync(core, PW_ID_CORE, 0);
        if (sequence < 0) {
            return std::unexpected(PipeWireError{PipeWireFailure::connection, sequence});
        }
        return {};
    }
};

PipeWireProbe::PipeWireProbe(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<PipeWireProbe>, PipeWireError>
PipeWireProbe::create(wl_event_loop *loop, Options options, Completion completion,
                      RuntimeFault fault) noexcept {
    if (!loop || !completion || options.timeout.count() < 1 || options.timeout.count() > INT_MAX ||
        options.remote.find('\0') != std::string::npos) {
        return std::unexpected(PipeWireError{PipeWireFailure::invalid_arguments, -EINVAL});
    }
    try {
        initialize_pipewire();
        auto ownership = options.audio_ownership ? options.audio_ownership
                                                 : std::make_shared<AudioRouteOwnership>();
        auto impl = std::make_shared<Impl>(loop, std::move(completion), std::move(fault),
                                           std::move(ownership));
        if (auto initialized = impl->initialize(options); !initialized) {
            return std::unexpected(initialized.error());
        }
        return std::unique_ptr<PipeWireProbe>(new PipeWireProbe(std::move(impl)));
    } catch (...) {
        return std::unexpected(PipeWireError{PipeWireFailure::allocation, -ENOMEM});
    }
}

PipeWireProbe::~PipeWireProbe() noexcept {
    impl_->shutdown();
}

std::expected<void, PipeWireError> PipeWireProbe::cancel() noexcept {
    auto self = impl_;
    if (std::this_thread::get_id() != self->owner) {
        return std::unexpected(PipeWireError{PipeWireFailure::wrong_thread, -EPERM});
    }
    if (self->monitoring) {
        self->shutdown();
        return {};
    }
    self->settle(PipeWireStatus::cancelled, -ECANCELED);
    if (self->delivery_failed) {
        Impl::deliver_startup(self.get());
    }
    return {};
}

PipeWireResult PipeWireProbe::result() const noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        return {PipeWireStatus::failed, -EPERM};
    }
    return impl_->outcome;
}

void PipeWireProbe::set_audio_routes(const std::optional<std::string> &output,
                                     const std::optional<std::string> &input) noexcept try {
    const std::array requests{&output, &input};
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto *requested = requests[index];
        auto &route = impl_->audio[index];
        if (route.requested != *requested) {
            route.requested = *requested;
            route.apply = true;
        }
    }
    impl_->apply_audio();
} catch (...) {
}

AudioRouteStatus PipeWireProbe::audio_route_status(bool playback) const {
    const std::size_t index = playback ? 0 : 1;
    return {impl_->audio[index].effective, impl_->available(index), impl_->metadata_ready};
}

void PipeWireProbe::refresh_audio_routes() noexcept {
    impl_->refresh_audio();
}

} // namespace seaside
