#ifndef _SWAY_LAYERS_H
#define _SWAY_LAYERS_H
#include <stdbool.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_layer_shell_v1.h>

struct sway_layer_surface {
	struct wlr_layer_surface_v1 *layer_surface;
	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
	struct wl_listener output_destroy;
	struct wl_listener new_popup;

	bool configured;
	struct wlr_box geo;
};

struct sway_layer_popup {
	struct sway_layer_surface *parent;
	struct wlr_xdg_popup *wlr_popup;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
};

struct sway_output;
void arrange_layers(struct sway_output *output);

struct sway_layer_surface *layer_from_wlr_layer_surface_v1(
	struct wlr_layer_surface_v1 *layer_surface);

#endif
