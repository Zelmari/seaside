#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <pixman.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>

enum { BUFFER_WIDTH = 16, BUFFER_HEIGHT = 8, SELF_TEST_ROUNDS = 3 };

struct fixture {
	struct seaside_server server;
	char *runtime_dir;
	const char *socket;
	struct wlr_output *output;
	struct wlr_surface *surface;
	struct wl_listener new_surface;
	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;
	struct wl_event_source *terminate_signal;
	struct wl_event_source *interrupt_signal;
	struct wl_event_source *deadline;
	struct wl_event_source *client_done;
	bool runtime_created;
	bool listening;
	bool failed;
	bool client_succeeded;
	bool timed_out;
	unsigned buffered_commits;
	unsigned detached_commits;
	unsigned destroyed_surfaces;
	unsigned created_outputs;
	int done_fd;
};

static void fail(struct fixture *fixture, const char *message) {
	fprintf(stderr, "headless fixture: %s\n", message);
	fixture->failed = true;
	wl_display_terminate(fixture->server.display);
}

static uint32_t pixel_value(unsigned x, unsigned y) {
	return 0xff000055u | (x << 16) | (y << 8);
}

static bool create_output(struct fixture *fixture, unsigned width,
		unsigned height) {
	fixture->output = wlr_headless_add_output(fixture->server.backend, width,
		height);
	if (!fixture->output || !wlr_output_init_render(fixture->output,
			fixture->server.allocator, fixture->server.renderer)) {
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, (int)width, (int)height, 60000);
	bool committed = wlr_output_commit_state(fixture->output, &state);
	wlr_output_state_finish(&state);
	if (!committed || !fixture->output->enabled ||
			fixture->output->width != (int)width ||
			fixture->output->height != (int)height) {
		return false;
	}
	wlr_output_create_global(fixture->output, fixture->server.display);
	fixture->created_outputs++;
	return true;
}

static bool check_texture(struct wlr_surface *surface) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture || !wlr_texture_is_pixman(texture) ||
			surface->current.width != BUFFER_WIDTH ||
			surface->current.height != BUFFER_HEIGHT) {
		return false;
	}
	pixman_image_t *image = wlr_pixman_texture_get_image(texture);
	if (!image || pixman_image_get_width(image) != BUFFER_WIDTH ||
			pixman_image_get_height(image) != BUFFER_HEIGHT) {
		return false;
	}
	const uint32_t *pixels = pixman_image_get_data(image);
	size_t stride = (size_t)pixman_image_get_stride(image) / sizeof(*pixels);
	for (unsigned y = 0; y < BUFFER_HEIGHT; y++) {
		for (unsigned x = 0; x < BUFFER_WIDTH; x++) {
			if (pixels[y * stride + x] != pixel_value(x, y)) {
				return false;
			}
		}
	}
	return true;
}

static void on_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct fixture *fixture = wl_container_of(listener, fixture, surface_commit);
	if (wlr_surface_has_buffer(fixture->surface)) {
		if (fixture->buffered_commits++ || !check_texture(fixture->surface)) {
			fail(fixture, "client SHM buffer did not become the expected texture");
			return;
		}
		/* Removing the sole output must leave the existing client and its
		 * surface alive. The client verifies removal before its next commit. */
		struct wlr_output *output = fixture->output;
		fixture->output = NULL;
		wlr_output_destroy(output);
	} else {
		if (fixture->buffered_commits != 1 || fixture->output ||
				fixture->detached_commits++ || !create_output(fixture, 320, 180)) {
			fail(fixture, "output return after a zero-output commit failed");
		}
	}
}

static void on_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct fixture *fixture = wl_container_of(listener, fixture, surface_destroy);
	wl_list_remove(&fixture->surface_commit.link);
	wl_list_remove(&fixture->surface_destroy.link);
	fixture->surface = NULL;
	fixture->destroyed_surfaces++;
}

static void on_new_surface(struct wl_listener *listener, void *data) {
	struct fixture *fixture = wl_container_of(listener, fixture, new_surface);
	if (fixture->surface || fixture->destroyed_surfaces) {
		fail(fixture, "unexpected additional client surface");
		return;
	}
	fixture->surface = data;
	fixture->surface_commit.notify = on_surface_commit;
	fixture->surface_destroy.notify = on_surface_destroy;
	wl_signal_add(&fixture->surface->events.commit, &fixture->surface_commit);
	wl_signal_add(&fixture->surface->events.destroy, &fixture->surface_destroy);
}

static int on_terminate(int signal_number, void *data) {
	(void)signal_number;
	struct fixture *fixture = data;
	wl_display_terminate(fixture->server.display);
	return 0;
}

static bool fixture_init(struct fixture *fixture, const char *runtime_root) {
	memset(fixture, 0, sizeof(*fixture));
	fixture->done_fd = -1;
	char *absolute_root = realpath(runtime_root, NULL);
	if (!absolute_root) {
		fprintf(stderr, "headless fixture: runtime root: %s\n", strerror(errno));
		return false;
	}
	char *runtime_dir = NULL;
	int length = asprintf(&runtime_dir, "%s/seaside-fixture-XXXXXX", absolute_root);
	free(absolute_root);
	if (length < 0) {
		return false;
	}
	fixture->runtime_dir = runtime_dir;
	if (!mkdtemp(fixture->runtime_dir)) {
		return false;
	}
	fixture->runtime_created = true;
	if (setenv("XDG_RUNTIME_DIR", fixture->runtime_dir, 1) < 0 ||
			!seaside_server_init(&fixture->server)) {
		return false;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(fixture->server.display);
	struct wlr_backend *backend = wlr_headless_backend_create(loop);
	struct wlr_renderer *renderer = wlr_pixman_renderer_create();
	if (!backend || !renderer) {
		if (backend) {
			wlr_backend_destroy(backend);
		}
		if (renderer) {
			wlr_renderer_destroy(renderer);
		}
		return false;
	}
	if (!seaside_server_start(&fixture->server, backend, renderer)) {
		return false;
	}
	fixture->socket = wl_display_add_socket_auto(fixture->server.display);
	fixture->terminate_signal = wl_event_loop_add_signal(loop, SIGTERM,
		on_terminate, fixture);
	fixture->interrupt_signal = wl_event_loop_add_signal(loop, SIGINT,
		on_terminate, fixture);
	if (!fixture->socket || !fixture->terminate_signal ||
			!fixture->interrupt_signal || !create_output(fixture, 160, 90)) {
		return false;
	}
	fixture->new_surface.notify = on_new_surface;
	wl_signal_add(&fixture->server.compositor->events.new_surface,
		&fixture->new_surface);
	fixture->listening = true;
	return true;
}

static bool fixture_finish(struct fixture *fixture) {
	struct wl_event_source *sources[] = {fixture->terminate_signal,
		fixture->interrupt_signal, fixture->deadline, fixture->client_done};
	for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		if (sources[i]) {
			wl_event_source_remove(sources[i]);
		}
	}
	if (fixture->done_fd >= 0) {
		close(fixture->done_fd);
	}
	if (fixture->listening) {
		wl_list_remove(&fixture->new_surface.link);
	}
	/* Surface destroy callbacks remain live until clients are disconnected. */
	seaside_server_finish(&fixture->server);
	seaside_server_finish(&fixture->server);
	bool clean = true;
	if (fixture->runtime_dir) {
		if (fixture->runtime_created && rmdir(fixture->runtime_dir) < 0) {
			fprintf(stderr, "headless fixture: runtime cleanup: %s\n", strerror(errno));
			clean = false;
		}
		free(fixture->runtime_dir);
	}
	return clean;
}

struct client_state {
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	uint32_t output_names[4];
	unsigned output_count;
	unsigned output_removals;
	bool argb8888;
};

static void on_shm_format(void *data, struct wl_shm *shm, uint32_t format) {
	(void)shm;
	struct client_state *client = data;
	if (format == WL_SHM_FORMAT_ARGB8888) {
		client->argb8888 = true;
	}
}

static const struct wl_shm_listener shm_listener = {.format = on_shm_format};

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct client_state *client = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
		client->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(client->shm, &shm_listener, client);
	} else if (strcmp(interface, wl_output_interface.name) == 0 &&
			client->output_count < 4) {
		client->output_names[client->output_count++] = name;
	}
}

static void on_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)registry;
	struct client_state *client = data;
	for (unsigned i = 0; i < client->output_count; i++) {
		if (client->output_names[i] == name) {
			client->output_removals++;
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = on_global,
	.global_remove = on_global_remove,
};

static bool exercise_client(const char *socket_name) {
	/* libwayland gives an inherited connection FD precedence even over an
	 * explicit name. This client must connect to the fixture's private socket. */
	if (unsetenv("WAYLAND_SOCKET") < 0) {
		return false;
	}
	struct wl_display *display = wl_display_connect(socket_name);
	if (!display) {
		return false;
	}
	bool success = false;
	struct client_state client = {0};
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &client);
	struct wl_surface *surface = NULL;
	struct wl_buffer *buffer = NULL;
	void *pixels = MAP_FAILED;
	size_t length = BUFFER_WIDTH * BUFFER_HEIGHT * sizeof(uint32_t);
	int memory_fd = -1;
	if (wl_display_roundtrip(display) < 0 || !client.compositor || !client.shm ||
			wl_display_roundtrip(display) < 0 || !client.argb8888 ||
			client.output_count != 1) {
		goto done;
	}
	memory_fd = memfd_create("seaside-test-buffer", MFD_CLOEXEC);
	if (memory_fd < 0 || ftruncate(memory_fd, (off_t)length) < 0) {
		goto done;
	}
	pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd, 0);
	if (pixels == MAP_FAILED) {
		goto done;
	}
	for (unsigned y = 0; y < BUFFER_HEIGHT; y++) {
		for (unsigned x = 0; x < BUFFER_WIDTH; x++) {
			((uint32_t *)pixels)[y * BUFFER_WIDTH + x] = pixel_value(x, y);
		}
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(client.shm, memory_fd,
		(int32_t)length);
	buffer = wl_shm_pool_create_buffer(pool, 0, BUFFER_WIDTH, BUFFER_HEIGHT,
		BUFFER_WIDTH * (int32_t)sizeof(uint32_t), WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	surface = wl_compositor_create_surface(client.compositor);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT);
	wl_surface_commit(surface);
	if (wl_display_roundtrip(display) < 0 || client.output_removals != 1 ||
			client.output_count != 1) {
		goto done;
	}
	/* This roundtrip and detach occur with no output at all. A replacement
	 * output is published only after the server receives the detach. */
	if (wl_display_roundtrip(display) < 0) {
		goto done;
	}
	wl_surface_attach(surface, NULL, 0, 0);
	wl_surface_commit(surface);
	if (wl_display_roundtrip(display) < 0 || client.output_count != 2 ||
			client.output_removals != 1) {
		goto done;
	}
	wl_surface_destroy(surface);
	surface = NULL;
	if (wl_display_roundtrip(display) < 0) {
		goto done;
	}
	success = true;
done:
	if (surface) {
		wl_surface_destroy(surface);
	}
	if (buffer) {
		wl_buffer_destroy(buffer);
	}
	if (client.shm) {
		wl_shm_destroy(client.shm);
	}
	if (client.compositor) {
		wl_compositor_destroy(client.compositor);
	}
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	if (pixels != MAP_FAILED) {
		munmap(pixels, length);
	}
	if (memory_fd >= 0) {
		close(memory_fd);
	}
	return success;
}

static int on_client_done(int fd, uint32_t mask, void *data) {
	(void)mask;
	struct fixture *fixture = data;
	unsigned char result;
	ssize_t count = read(fd, &result, sizeof(result));
	if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
		return 0;
	}
	fixture->client_succeeded = count == 1 && result == 1;
	if (!fixture->client_succeeded) {
		fail(fixture, "real Wayland client failed or disconnected unexpectedly");
	} else {
		wl_display_terminate(fixture->server.display);
	}
	return 0;
}

static int on_deadline(void *data) {
	struct fixture *fixture = data;
	fixture->timed_out = true;
	fail(fixture, "client lifecycle exceeded its 10 second deadline");
	return 0;
}

static bool self_test_round(const char *runtime_root) {
	struct fixture fixture;
	if (!fixture_init(&fixture, runtime_root)) {
		fprintf(stderr, "headless fixture: initialization failed\n");
		fixture_finish(&fixture);
		return false;
	}
	int completion[2];
	if (pipe2(completion, O_CLOEXEC | O_NONBLOCK) < 0) {
		fixture_finish(&fixture);
		return false;
	}
	pid_t child = fork();
	if (child == 0) {
		close(completion[0]);
		unsigned char result = exercise_client(fixture.socket) ? 1 : 0;
		ssize_t written;
		do {
			written = write(completion[1], &result, sizeof(result));
		} while (written < 0 && errno == EINTR);
		close(completion[1]);
		/* The fork inherited server descriptors; only the parent owns their
		 * teardown. _exit avoids copied Wayland/libc cleanup in the child. */
		_exit(result == 1 && written == 1 ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	close(completion[1]);
	fixture.done_fd = completion[0];
	if (child < 0) {
		fixture_finish(&fixture);
		return false;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(fixture.server.display);
	fixture.client_done = wl_event_loop_add_fd(loop, fixture.done_fd,
		WL_EVENT_READABLE, on_client_done, &fixture);
	fixture.deadline = wl_event_loop_add_timer(loop, on_deadline, &fixture);
	if (!fixture.client_done || !fixture.deadline ||
			wl_event_source_timer_update(fixture.deadline, 10000) < 0) {
		fixture.failed = true;
	} else {
		wl_display_run(fixture.server.display);
	}
	if (!fixture.client_succeeded || fixture.failed || fixture.timed_out) {
		kill(child, SIGKILL);
	}
	int status = 0;
	pid_t waited;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	bool passed = !fixture.failed && fixture.client_succeeded && waited == child &&
		WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS &&
		fixture.buffered_commits == 1 && fixture.detached_commits == 1 &&
		fixture.destroyed_surfaces == 1 && !fixture.surface &&
		fixture.created_outputs == 2;
	return fixture_finish(&fixture) && passed;
}

static void print_json_string(const char *value) {
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (*p == '"' || *p == '\\') {
			putchar('\\');
			putchar(*p);
		} else if (*p < 0x20) {
			printf("\\u%04x", *p);
		} else {
			putchar(*p);
		}
	}
	putchar('"');
}

int main(int argc, char **argv) {
	bool serve = argc == 2 && strcmp(argv[1], "--serve") == 0;
	if (argc != 1 && !serve) {
		fprintf(stderr, "usage: seaside-headless-fixture [--serve]\n");
		return EXIT_FAILURE;
	}
	const char *configured_runtime = getenv("XDG_RUNTIME_DIR");
	char *runtime_root = strdup(configured_runtime ? configured_runtime : "/tmp");
	if (!runtime_root) {
		return EXIT_FAILURE;
	}
	bool passed = true;
	if (serve) {
		struct fixture fixture;
		passed = fixture_init(&fixture, runtime_root);
		if (passed) {
			fputs("{\"event\":\"ready\",\"socket\":", stdout);
			print_json_string(fixture.socket);
			fputs(",\"runtime_dir\":", stdout);
			print_json_string(fixture.runtime_dir);
			fputs("}\n", stdout);
			fflush(stdout);
			wl_display_run(fixture.server.display);
			passed = !fixture.failed;
		}
		passed = fixture_finish(&fixture) && passed;
	} else {
		unsigned completed_rounds = 0;
		for (unsigned i = 0; i < SELF_TEST_ROUNDS; i++) {
			if (!self_test_round(runtime_root)) {
				passed = false;
				break;
			}
			completed_rounds++;
		}
		printf("{\"event\":\"self-test\",\"status\":\"%s\",\"rounds\":%u}\n",
			passed ? "pass" : "fail", completed_rounds);
	}
	free(runtime_root);
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
