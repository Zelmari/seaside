#ifndef SEASIDE_LAYER_SCENE_H
#define SEASIDE_LAYER_SCENE_H

#include "scene_bridge.h"

struct wlr_scene_tree;
struct wlr_xdg_popup;
struct seaside_layer_scene;

struct seaside_layer_scene *seaside_layer_scene_create(struct wl_display *display,
                                                       struct wlr_scene_tree *windows,
                                                       struct wlr_scene_tree *drag_icons);
void seaside_layer_scene_destroy(struct seaside_layer_scene *layers);
void seaside_layer_scene_set_handler(struct seaside_layer_scene *layers,
                                     seaside_layer_output_fn focused_output,
                                     seaside_layer_changed_fn changed,
                                     seaside_layer_hidden_fn hidden, void *data);
bool seaside_layer_scene_add_output(struct seaside_layer_scene *layers, struct wlr_output *output,
                                    struct seaside_scene_rect bounds);
void seaside_layer_scene_update_output(struct seaside_layer_scene *layers,
                                       struct wlr_output *output, struct seaside_scene_rect bounds);
void seaside_layer_scene_remove_output(struct seaside_layer_scene *layers,
                                       struct wlr_output *output);
struct seaside_scene_rect seaside_layer_scene_work_area(struct seaside_layer_scene *layers,
                                                        struct wlr_output *output, bool fullscreen);
bool seaside_layer_scene_add_popup(struct seaside_layer_scene *layers, struct wlr_xdg_popup *popup);
bool seaside_layer_scene_at(struct seaside_layer_scene *layers, bool above_windows, double x,
                            double y, struct seaside_scene_target *target);
bool seaside_layer_scene_position(struct seaside_layer_scene *layers, double x, double y,
                                  struct seaside_scene_target *target);
void seaside_layer_scene_select(struct seaside_layer_scene *layers, struct wlr_surface *surface);
struct wlr_surface *seaside_layer_scene_keyboard_focus(struct seaside_layer_scene *layers,
                                                       struct wlr_output *focused_output);
struct wlr_output *seaside_layer_scene_visible_output(struct seaside_layer_scene *layers,
                                                      struct wlr_surface *surface);
bool seaside_scene_surface_intersects(struct wlr_scene_tree *tree, struct wlr_surface *surface,
                                      struct seaside_scene_rect viewport);

#endif
