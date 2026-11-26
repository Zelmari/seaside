#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vmwgfx_drm.h>

int main(int argc, char **argv) {
    const char *layouts[] = {"single", "dual", "triple"};
    unsigned count = 0;
    if (argc == 2) {
        for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
            if (strcmp(argv[1], layouts[i]) == 0) {
                count = i + 1;
                break;
            }
        }
    }
    if (count == 0) {
        return 2;
    }
    struct drm_vmw_rect outputs[] = {{0, 0, 1280, 800}, {1280, 0, 800, 600}, {2080, 0, 640, 480}};
    struct drm_vmw_update_layout_arg layout = {
        .num_outputs = count, .pad64 = 0, .rects = reinterpret_cast<uintptr_t>(outputs)};
    const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        perror("open vmwgfx");
        return 1;
    }
    const unsigned long request =
        DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UPDATE_LAYOUT, struct drm_vmw_update_layout_arg);
    if (ioctl(fd, request, &layout) < 0) {
        perror("update vmwgfx layout");
        close(fd);
        return 1;
    }
    return close(fd) != 0;
}
