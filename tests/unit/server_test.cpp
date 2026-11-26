#include "server.hpp"
#include "wlroots_bridge.hpp"
#include "wlroots_fixture.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string_view>
#include <wayland-server-core.h>

namespace {
enum class Failure { none, display, allocator, renderer, compositor, backend };
Failure failure = Failure::none;
std::array<char, 16> destruction{};
std::size_t destruction_count = 0;

void record(char resource) noexcept {
    if (destruction_count < destruction.size()) {
        destruction[destruction_count++] = resource;
    }
}

class ServerTest : public testing::Test {
    void SetUp() override {
        failure = Failure::none;
        destruction_count = 0;
    }
    void TearDown() override { failure = Failure::none; }
};
} // namespace

// Linker wrapping injects failures only into this test executable. Production
// has neither global failure switches nor an alternative startup path.
extern "C" {
wl_display *__real_wl_display_create();
void __real_wl_display_destroy(wl_display *);
wlr_allocator *__real_seaside_wlr_allocator_create(wlr_backend *, wlr_renderer *);
bool __real_seaside_wlr_renderer_init_display(wlr_renderer *, wl_display *, wlr_backend *,
                                              wlr_linux_dmabuf_v1 **);
wlr_compositor *__real_seaside_wlr_compositor_create(wl_display *, wlr_renderer *);
bool __real_seaside_wlr_backend_start(wlr_backend *);
void __real_seaside_wlr_backend_destroy(wlr_backend *);
void __real_seaside_wlr_allocator_destroy(wlr_allocator *);
void __real_seaside_wlr_renderer_destroy(wlr_renderer *);

wl_display *__wrap_wl_display_create() {
    return failure == Failure::display ? nullptr : __real_wl_display_create();
}
void __wrap_wl_display_destroy(wl_display *display) {
    record('D');
    __real_wl_display_destroy(display);
}
wlr_allocator *__wrap_seaside_wlr_allocator_create(wlr_backend *backend, wlr_renderer *renderer) {
    return failure == Failure::allocator ? nullptr
                                         : __real_seaside_wlr_allocator_create(backend, renderer);
}
bool __wrap_seaside_wlr_renderer_init_display(wlr_renderer *renderer, wl_display *display,
                                              wlr_backend *backend, wlr_linux_dmabuf_v1 **dmabuf) {
    return failure != Failure::renderer &&
           __real_seaside_wlr_renderer_init_display(renderer, display, backend, dmabuf);
}
wlr_compositor *__wrap_seaside_wlr_compositor_create(wl_display *display, wlr_renderer *renderer) {
    return failure == Failure::compositor ? nullptr
                                          : __real_seaside_wlr_compositor_create(display, renderer);
}
bool __wrap_seaside_wlr_backend_start(wlr_backend *backend) {
    return failure != Failure::backend && __real_seaside_wlr_backend_start(backend);
}
void __wrap_seaside_wlr_backend_destroy(wlr_backend *backend) {
    record('B');
    __real_seaside_wlr_backend_destroy(backend);
}
void __wrap_seaside_wlr_allocator_destroy(wlr_allocator *allocator) {
    record('A');
    __real_seaside_wlr_allocator_destroy(allocator);
}
void __wrap_seaside_wlr_renderer_destroy(wlr_renderer *renderer) {
    record('R');
    __real_seaside_wlr_renderer_destroy(renderer);
}
}

TEST_F(ServerTest, ReportsDisplayCreationFailure) {
    failure = Failure::display;
    auto result = seaside::Server::create();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), seaside::ServerError::display_creation);
    EXPECT_EQ(destruction_count, 0U);
}

TEST_F(ServerTest, InvalidArgumentsDoNotTransferOwnership) {
    auto created = seaside::Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    auto *backend = seaside_test_backend_create(server.display());
    ASSERT_NE(backend, nullptr);
    auto result = server.start(backend, nullptr);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), seaside::ServerError::invalid_arguments);
    EXPECT_EQ(server.backend(), nullptr);
    // The rejected backend is still caller-owned and borrows the display loop.
    seaside_wlr_backend_destroy(backend);
    created->reset();
    EXPECT_EQ(std::string_view(destruction.data(), destruction_count), "BD");
}

TEST_F(ServerTest, DestructionReleasesResourcesInDependencyOrder) {
    auto created = seaside::Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    auto *backend = seaside_test_backend_create(server.display());
    auto *renderer = seaside_test_renderer_create();
    ASSERT_NE(backend, nullptr);
    ASSERT_NE(renderer, nullptr);
    ASSERT_TRUE(server.start(backend, renderer));
    ASSERT_NE(server.compositor(), nullptr);
    created->reset();
    EXPECT_EQ(std::string_view(destruction.data(), destruction_count), "BARD");
}

TEST_F(ServerTest, FinishIsIdempotentAndRejectsRestartAfterDisplayTeardown) {
    auto created = seaside::Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    server.finish();
    server.finish();
    auto result = server.start(nullptr, nullptr);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), seaside::ServerError::invalid_state);
    created->reset();
    EXPECT_EQ(std::string_view(destruction.data(), destruction_count), "D");
}

TEST_F(ServerTest, StartupFailuresRetainOwnedResourcesUntilOrderedCleanup) {
    const std::array cases{
        std::pair{Failure::allocator, seaside::ServerError::allocator_creation},
        std::pair{Failure::renderer, seaside::ServerError::renderer_initialization},
        std::pair{Failure::compositor, seaside::ServerError::compositor_creation},
        std::pair{Failure::backend, seaside::ServerError::backend_start},
    };
    for (const auto &[point, error] : cases) {
        SCOPED_TRACE(static_cast<int>(point));
        destruction_count = 0;
        auto created = seaside::Server::create();
        ASSERT_TRUE(created);
        auto &server = **created;
        auto *backend = seaside_test_backend_create(server.display());
        auto *renderer = seaside_test_renderer_create();
        ASSERT_NE(backend, nullptr);
        ASSERT_NE(renderer, nullptr);
        failure = point;
        auto result = server.start(backend, renderer);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
        EXPECT_EQ(server.backend(), backend);
        EXPECT_EQ(server.renderer(), renderer);
        auto retry = server.start(backend, renderer);
        ASSERT_FALSE(retry);
        EXPECT_EQ(retry.error(), seaside::ServerError::invalid_state);
        created->reset();
        EXPECT_EQ(std::string_view(destruction.data(), destruction_count),
                  point == Failure::allocator ? "BRD" : "BARD");
        failure = Failure::none;
    }
}
