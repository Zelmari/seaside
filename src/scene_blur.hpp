#ifndef SEASIDE_SCENE_BLUR_H
#define SEASIDE_SCENE_BLUR_H

struct wlr_box;
struct wlr_output;
struct wlr_scene_tree;
struct seaside_scene_blur;

bool seaside_scene_blur_update(struct seaside_scene_blur **blur, struct wlr_scene_tree *stack,
                               struct wlr_scene_tree *window, struct wlr_scene_tree *content,
                               struct wlr_output *output, const struct wlr_box *viewport,
                               double strength, int passes);
void seaside_scene_blur_remove_output(struct seaside_scene_blur *blur, struct wlr_output *output);
void seaside_scene_blur_destroy(struct seaside_scene_blur **blur);

#endif
