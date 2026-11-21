#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <utility>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;
using Bus = std::unique_ptr<sd_bus, decltype(&sd_bus_flush_close_unref)>;
using Slot = std::unique_ptr<sd_bus_slot, decltype(&sd_bus_slot_unref)>;

constexpr char service[] = "org.freedesktop.portal.Desktop";
constexpr char desktop[] = "/org/freedesktop/portal/desktop";
constexpr char screencast[] = "org.freedesktop.portal.ScreenCast";
constexpr char session_interface[] = "org.freedesktop.portal.Session";
constexpr auto bus_timeout = 7'000'000ULL;

void check(int status, const char *operation) {
    if (status < 0) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(-status));
    }
}

Bus connect_bus() {
    sd_bus *bus = nullptr;
    check(sd_bus_open_user(&bus), "connect to the user bus");
    return {bus, sd_bus_flush_close_unref};
}

Message make_call(sd_bus *bus, const char *path, const char *interface, const char *method) {
    sd_bus_message *raw = nullptr;
    check(sd_bus_message_new_method_call(bus, &raw, service, path, interface, method), method);
    return {raw, sd_bus_message_unref};
}

Message call(sd_bus *bus, const Message &request, const char *method) {
    sd_bus_message *raw = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    const int status = sd_bus_call(bus, request.get(), bus_timeout, &error, &raw);
    Message result(raw, sd_bus_message_unref);
    if (status < 0 && error.message) {
        const std::string detail = error.message;
        sd_bus_error_free(&error);
        throw std::runtime_error(std::string(method) + ": " + detail);
    }
    sd_bus_error_free(&error);
    check(status, method);
    return result;
}

struct Response {
    Message message{nullptr, sd_bus_message_unref};
    std::uint32_t code = 0;
    static int receive(sd_bus_message *message, void *data, sd_bus_error *) {
        auto &self = *static_cast<Response *>(data);
        const int status = sd_bus_message_read(message, "u", &self.code);
        if (status <= 0) {
            return -EBADMSG;
        }
        self.message.reset(sd_bus_message_ref(message));
        return 1;
    }
};

std::string unique_path(sd_bus *bus) {
    const char *name = nullptr;
    check(sd_bus_get_unique_name(bus, &name), "get the unique bus name");
    std::string escaped(name + 1);
    for (auto &character : escaped) {
        if (character == '.') {
            character = '_';
        }
    }
    return escaped;
}

Response request(sd_bus *bus, const Message &message, const char *method, const char *token) {
    const auto path = std::string(desktop) + "/request/" + unique_path(bus) + '/' + token;
    Response response;
    sd_bus_slot *raw_slot = nullptr;
    check(sd_bus_match_signal(bus, &raw_slot, service, path.c_str(),
                              "org.freedesktop.portal.Request", "Response", Response::receive,
                              &response),
          "match the portal response");
    const Slot match(raw_slot, sd_bus_slot_unref);
    auto reply = call(bus, message, method);
    const char *actual = nullptr;
    if (sd_bus_message_read(reply.get(), "o", &actual) != 1 || path != actual) {
        throw std::runtime_error(std::string(method) + " returned an unexpected request path");
    }
    const auto deadline = Clock::now() + 18s;
    while (!response.message) {
        if (Clock::now() >= deadline) {
            throw std::runtime_error(std::string(method) + " response timed out");
        }
        if (const int status = sd_bus_process(bus, nullptr); status <= 0) {
            check(status, "dispatch the portal response");
            check(sd_bus_wait(bus, 100'000), "wait for the portal response");
        }
    }
    return response;
}

class CloseSession {
  public:
    CloseSession(sd_bus *bus, std::string path) : bus_(bus), path_(std::move(path)) {}
    ~CloseSession() {
        if (!closed) {
            try {
                auto message = make_call(bus_, path_.c_str(), session_interface, "Close");
                (void)call(bus_, message, "Close session");
            } catch (const std::exception &error) {
                std::fprintf(stderr, "%s\n", error.what());
            }
        }
    }
    const char *path() const { return path_.c_str(); }
    bool closed = false;

  private:
    sd_bus *bus_;
    std::string path_;
};

int session_closed(sd_bus_message *, void *data, sd_bus_error *) {
    static_cast<CloseSession *>(data)->closed = true;
    return 1;
}

std::string create_session(sd_bus *bus) {
    auto message = make_call(bus, desktop, screencast, "CreateSession");
    check(sd_bus_message_append(message.get(), "a{sv}", 2, "handle_token", "s", "create",
                                "session_handle_token", "s", "capture"),
          "write CreateSession options");
    const auto result = request(bus, message, "CreateSession", "create");
    const auto session = std::string(desktop) + "/session/" + unique_path(bus) + "/capture";
    const char *value = nullptr;
    if (result.code != 0 ||
        sd_bus_message_enter_container(result.message.get(), 'a', "{sv}") <= 0) {
        throw std::runtime_error("the portal did not create the session");
    }
    while (sd_bus_message_enter_container(result.message.get(), 'e', "sv") > 0) {
        const char *key = nullptr;
        check(sd_bus_message_read(result.message.get(), "s", &key), "read session property");
        if (std::string_view(key) == "session_handle") {
            check(sd_bus_message_read(result.message.get(), "v", "s", &value),
                  "read session handle");
        } else {
            check(sd_bus_message_skip(result.message.get(), "v"), "skip session property");
        }
        check(sd_bus_message_exit_container(result.message.get()), "finish session property");
    }
    if (!value || value != session) {
        throw std::runtime_error("the portal returned an unexpected session handle");
    }
    return session;
}

std::uint32_t select_sources(sd_bus *bus, const char *session, std::uint32_t type) {
    auto message = make_call(bus, desktop, screencast, "SelectSources");
    check(sd_bus_message_append(message.get(), "oa{sv}", session, 4, "handle_token", "s", "select",
                                "types", "u", type, "multiple", "b", 0, "cursor_mode", "u", 1u),
          "write SelectSources options");
    return request(bus, message, "SelectSources", "select").code;
}

std::uint32_t stream_node(const Response &result, std::uint32_t requested) {
    auto *message = result.message.get();
    if (result.code || sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) {
        throw std::runtime_error("the portal did not start the session");
    }
    while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
        const char *key = nullptr;
        check(sd_bus_message_read(message, "s", &key), "read stream property");
        if (std::string_view(key) != "streams") {
            check(sd_bus_message_skip(message, "v"), "skip start property");
            check(sd_bus_message_exit_container(message), "finish start property");
            continue;
        }
        std::uint32_t node = 0;
        if (sd_bus_message_enter_container(message, 'v', "a(ua{sv})") <= 0 ||
            sd_bus_message_enter_container(message, 'a', "(ua{sv})") <= 0 ||
            sd_bus_message_enter_container(message, 'r', "ua{sv}") <= 0 ||
            sd_bus_message_read(message, "u", &node) != 1 ||
            sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) {
            throw std::runtime_error("the portal returned no stream node");
        }
        std::uint32_t source = 0;
        while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
            const char *property = nullptr;
            check(sd_bus_message_read(message, "s", &property), "read stream information");
            if (std::string_view(property) == "source_type") {
                check(sd_bus_message_read(message, "v", "u", &source), "read stream source type");
            } else {
                check(sd_bus_message_skip(message, "v"), "skip stream information");
            }
            check(sd_bus_message_exit_container(message), "finish stream information");
        }
        check(sd_bus_message_exit_container(message), "finish stream properties");
        check(sd_bus_message_exit_container(message), "finish stream");
        if (!node || source != requested || sd_bus_message_at_end(message, 0) != 1) {
            throw std::runtime_error("the portal returned an unexpected stream selection");
        }
        std::printf("started %u source %u\n", node, source);
        return node;
    }
    throw std::runtime_error("the portal omitted the streams property");
}

int remote_fd(sd_bus *bus, const char *session) {
    auto message = make_call(bus, desktop, screencast, "OpenPipeWireRemote");
    check(sd_bus_message_append(message.get(), "oa{sv}", session, 0), "write remote options");
    const auto reply = call(bus, message, "OpenPipeWireRemote");
    int fd = -1;
    if (sd_bus_message_read(reply.get(), "h", &fd) != 1 || fd < 0) {
        throw std::runtime_error("the portal omitted the PipeWire fd");
    }
    const int copy = dup(fd);
    if (copy < 0) {
        throw std::runtime_error("cannot retain the PipeWire fd");
    }
    return copy;
}

class VideoReceiver {
  public:
    VideoReceiver() { pw_init(nullptr, nullptr); }
    ~VideoReceiver() {
        if (stream_) {
            pw_stream_destroy(stream_);
        }
        if (core_) {
            pw_core_disconnect(core_);
        }
        if (context_) {
            pw_context_destroy(context_);
        }
        if (loop_) {
            pw_loop_leave(loop_);
            pw_loop_destroy(loop_);
        }
        pw_deinit();
    }

    void start(sd_bus *bus, const char *session, std::uint32_t node) {
        loop_ = pw_loop_new(nullptr);
        if (!loop_) {
            throw std::runtime_error("cannot create a PipeWire loop");
        }
        pw_loop_enter(loop_);
        context_ = pw_context_new(loop_, nullptr, 0);
        if (!context_) {
            throw std::runtime_error("cannot create a PipeWire context");
        }
        core_ = pw_context_connect_fd(context_, remote_fd(bus, session), nullptr, 0);
        if (!core_) {
            throw std::runtime_error("cannot connect to the portal's PipeWire remote");
        }
        core_events_.version = PW_VERSION_CORE_EVENTS;
        core_events_.error = core_error;
        pw_core_add_listener(core_, &core_listener_, &core_events_, this);
        auto *properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                             "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
        stream_ = pw_stream_new(core_, "Seaside Part 7 receiver", properties);
        if (!stream_) {
            throw std::runtime_error("cannot create the receiver stream");
        }
        events_.version = PW_VERSION_STREAM_EVENTS;
        events_.state_changed = state_changed;
        events_.param_changed = param_changed;
        events_.process = process;
        pw_stream_add_listener(stream_, &listener_, &events_, this);
        std::array<std::uint8_t, 512> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        const auto *format = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
            SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                   SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                                   SPA_VIDEO_FORMAT_RGBA)));
        // PipeWire defines these enum values as combinable flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        const auto flags =
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
        check(pw_stream_connect(stream_, PW_DIRECTION_INPUT, node, flags, &format, 1),
              "connect the PipeWire video stream");
    }

    void dispatch() { check(pw_loop_iterate(loop_, 40), "dispatch the PipeWire receiver"); }
    bool ended() const { return ended_; }
    std::size_t frames() const { return frames_; }

  private:
    static void core_error(void *, std::uint32_t id, int, int result, const char *message) {
        std::fprintf(stderr, "PipeWire object %u error %d: %s\n", id, result, message);
    }

    static void state_changed(void *data, pw_stream_state old, pw_stream_state next,
                              const char *error) {
        auto &self = *static_cast<VideoReceiver *>(data);
        std::fprintf(stderr, "PipeWire stream state %s\n", pw_stream_state_as_string(next));
        if (next == PW_STREAM_STATE_ERROR ||
            (next == PW_STREAM_STATE_UNCONNECTED && old >= PW_STREAM_STATE_PAUSED)) {
            self.ended_ = true;
            if (error) {
                std::fprintf(stderr, "PipeWire stream: %s\n", error);
            }
        }
    }

    static void param_changed(void *data, std::uint32_t id, const spa_pod *value) {
        auto &self = *static_cast<VideoReceiver *>(data);
        if (id != SPA_PARAM_Format || !value ||
            spa_format_video_raw_parse(value, &self.format_) < 0) {
            return;
        }
        std::fprintf(stderr, "PipeWire format %u %ux%u\n", self.format_.format,
                     self.format_.size.width, self.format_.size.height);
        std::array<std::uint8_t, 256> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        const auto *parameters = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        if (pw_stream_update_params(self.stream_, &parameters, 1) < 0) {
            self.ended_ = true;
        }
    }

    static void process(void *data) {
        auto &self = *static_cast<VideoReceiver *>(data);
        while (auto *buffer = pw_stream_dequeue_buffer(self.stream_)) {
            if (buffer->buffer && buffer->buffer->n_datas > 0) {
                self.sample(buffer->buffer->datas[0]);
            }
            pw_stream_queue_buffer(self.stream_, buffer);
        }
    }

    void sample(const spa_data &data) {
        const auto width = format_.size.width;
        const auto height = format_.size.height;
        if (observed_buffers_++ < 3 && data.chunk) {
            std::fprintf(stderr, "PipeWire buffer type=%u size=%u stride=%d mapped=%d\n", data.type,
                         data.chunk->size, data.chunk->stride, data.data != nullptr);
        }
        if (!data.data || !data.chunk || width == 0 || height == 0 || width > 16384 ||
            height > 16384 || data.chunk->size == 0 || data.chunk->offset >= data.maxsize ||
            data.chunk->stride < static_cast<std::int32_t>(width * 4)) {
            return;
        }
        const bool bgr =
            format_.format == SPA_VIDEO_FORMAT_BGRx || format_.format == SPA_VIDEO_FORMAT_BGRA;
        if (!bgr && format_.format != SPA_VIDEO_FORMAT_RGBx &&
            format_.format != SPA_VIDEO_FORMAT_RGBA) {
            return;
        }
        const auto *bytes = static_cast<const std::uint8_t *>(data.data);
        const auto limit =
            std::min<std::size_t>(data.maxsize - data.chunk->offset, data.chunk->size);
        std::array<std::uint32_t, 5> pixels{};
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            constexpr std::array<std::array<unsigned, 2>, 5> quarters{
                {{2, 2}, {1, 1}, {1, 3}, {3, 1}, {3, 3}}};
            const auto x = width * quarters[index][0] / 4;
            const auto y = height * quarters[index][1] / 4;
            const auto offset = y * static_cast<std::size_t>(data.chunk->stride) + x * 4;
            if (offset + 3 > limit) {
                return;
            }
            const auto *pixel = bytes + data.chunk->offset + offset;
            pixels[index] = (static_cast<std::uint32_t>(pixel[bgr ? 2 : 0]) << 16) |
                            (static_cast<std::uint32_t>(pixel[1]) << 8) | pixel[bgr ? 0 : 2];
        }
        ++frames_;
        if (pixels != last_pixels_ || width != last_width_ || height != last_height_) {
            std::printf("frame %u %u %06x %06x %06x %06x %06x\n", width, height, pixels[0],
                        pixels[1], pixels[2], pixels[3], pixels[4]);
            last_pixels_ = pixels;
            last_width_ = width;
            last_height_ = height;
        }
    }

    pw_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_stream *stream_ = nullptr;
    pw_core_events core_events_{};
    pw_stream_events events_{};
    spa_hook core_listener_{};
    spa_hook listener_{};
    spa_video_info_raw format_{};
    std::size_t frames_ = 0;
    std::size_t observed_buffers_ = 0;
    bool ended_ = false;
    std::array<std::uint32_t, 5> last_pixels_{};
    std::uint32_t last_width_ = 0;
    std::uint32_t last_height_ = 0;
};

void run(std::string_view source, std::string_view lifetime) {
    const auto bus = connect_bus();
    std::uint32_t capabilities = 0;
    check(sd_bus_get_property_trivial(bus.get(), service, desktop, screencast,
                                      "AvailableSourceTypes", nullptr, 'u', &capabilities),
          "query portal source types");
    if ((capabilities & 3u) != 3u) {
        throw std::runtime_error("the portal must advertise monitor and window capture");
    }
    const auto requested = source == "monitor" ? 1u : 2u;
    CloseSession session(bus.get(), create_session(bus.get()));
    sd_bus_slot *raw_slot = nullptr;
    check(sd_bus_match_signal(bus.get(), &raw_slot, service, session.path(), session_interface,
                              "Closed", session_closed, &session),
          "match session closure");
    const Slot match(raw_slot, sd_bus_slot_unref);
    const auto selection = select_sources(bus.get(), session.path(), requested);
    if (selection != 0) {
        if (lifetime == "cancel" && selection == 1) {
            session.closed = true;
            std::puts("cancelled during source selection");
            return;
        }
        throw std::runtime_error("the portal rejected SelectSources with code " +
                                 std::to_string(selection));
    }
    auto message = make_call(bus.get(), desktop, screencast, "Start");
    check(sd_bus_message_append(message.get(), "osa{sv}", session.path(), "", 1, "handle_token",
                                "s", "start"),
          "write Start options");
    const auto result = request(bus.get(), message, "Start", "start");
    if (lifetime == "cancel") {
        if (result.code != 1) {
            throw std::runtime_error("portal Start did not report chooser cancellation");
        }
        std::puts("cancelled");
        return;
    }
    VideoReceiver receiver;
    receiver.start(bus.get(), session.path(), stream_node(result, requested));
    const auto deadline = Clock::now() + 24s;
    while (!receiver.ended() && !session.closed && Clock::now() < deadline) {
        receiver.dispatch();
        check(sd_bus_process(bus.get(), nullptr), "dispatch active session events");
        if (receiver.frames() != 0 && lifetime == "frame") {
            return;
        }
    }
    if (receiver.frames() == 0 || (!session.closed && !receiver.ended()) || lifetime != "closed") {
        throw std::runtime_error("the portal stream did not reach its expected lifetime");
    }
    std::printf("closed frames %zu\n", receiver.frames());
}
} // namespace

int main(int argc, char **argv) {
    const std::string_view source = argc > 1 ? argv[1] : "";
    const std::string_view lifetime = argc > 2 ? argv[2] : "";
    if (argc != 3 || (source != "monitor" && source != "window") ||
        (lifetime != "cancel" && lifetime != "frame" && lifetime != "closed")) {
        std::fputs("Usage: seaside-portal-stream monitor|window cancel|frame|closed\n", stderr);
        return EXIT_FAILURE;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        run(source, lifetime);
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }
}
