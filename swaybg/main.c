#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "background-image.h"
#include "cairo.h"
#include "log.h"
#include "pool-buffer.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct swaybg_output_config {
	const char *name;
	const char *image_path;
	cairo_surface_t *image;
	enum background_mode mode;
	uint32_t color;

	struct wl_list link;
};

struct swaybg_output {
	struct swaybg_state *state;
	struct swaybg_output_config *config;
	struct wl_list link;

	struct wl_output *wl_output;
	int32_t scale;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	struct wl_region *input_region;

	struct pool_buffer buffers[2];
	struct pool_buffer *current_buffer;
};

struct swaybg_state {
	struct wl_list output_configs; // swaybg_output_config::link

	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_list outputs; // swaybg_output::link
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zxdg_output_manager_v1 *xdg_output_manager;

	bool running;
} state;

static void noop() {
	// This space intentionally left blank
}

static void draw_frame(struct swaybg_output *output) {
	int buffer_width = output->width * output->scale,
		buffer_height = output->height * output->scale;
	output->current_buffer = get_next_buffer(output->state->shm,
			output->buffers, buffer_width, buffer_height);
	if (!output->current_buffer) {
		return;
	}
	cairo_t *cairo = output->current_buffer->cairo;
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	cairo_set_source_u32(cairo, output->config->color);
	cairo_paint(cairo);
	if (output->config->mode == BACKGROUND_MODE_SOLID_COLOR) {
		cairo_set_source_u32(cairo, output->config->color);
		cairo_paint(cairo);
	}

	if (output->config->image) {
		render_background_image(cairo, output->config->image,
				output->config->mode, buffer_width, buffer_height);
	}

	wl_surface_set_buffer_scale(output->surface, output->scale);
	wl_surface_attach(output->surface, output->current_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(output->surface);
}

static void render_frame(struct swaybg_state *state) {
	struct swaybg_output *output;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->height > 0 && output->width > 0) {
			draw_frame(output);
		}
	}
}

static void load_config_image(struct swaybg_output_config *output_config) {
	output_config->image = load_background_image(output_config->image_path);
	if (!output_config->image) {
		sway_log(SWAY_ERROR, "Failed to load background image '%s'",
				output_config->image_path);
		state.running = false;
	}
}

void reload_images(int signum) {
	struct swaybg_output_config *output_config;
	wl_list_for_each(output_config, &state.output_configs, link) {
		if (output_config->image) {
			cairo_surface_destroy(output_config->image);
			load_config_image(output_config);
		}
	}
	render_frame(&state);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct swaybg_output *output = data;
	output->height = height;
	output->width = width;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_frame(output->state);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct swaybg_output *output = data;

	wl_list_remove(&output->link);

	wl_region_destroy(output->input_region);
	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wl_surface_destroy(output->surface);
	wl_output_destroy(output->wl_output);

	free(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void create_output_surface(struct swaybg_output *output) {
	struct swaybg_state *state = output->state;

	output->surface = wl_compositor_create_surface(state->compositor);
	assert(output->surface);

	// Empty input region
	output->input_region = wl_compositor_create_region(state->compositor);
	assert(output->input_region);
	wl_surface_set_input_region(output->surface, output->input_region);

	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state->layer_shell, output->surface, output->wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	assert(output->layer_surface);

	zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(output->layer_surface,
			&layer_surface_listener, output);
	wl_surface_commit(output->surface);
}

static void xdg_output_handle_name(void *data,
		struct zxdg_output_v1 *xdg_output, const char *name) {
	zxdg_output_v1_destroy(xdg_output);

	struct swaybg_output *output = data;
	struct swaybg_state *state = output->state;
	struct swaybg_output_config *output_config = NULL, *c;
	wl_list_for_each(c, &state->output_configs, link) {
		if (strcmp(c->name, name) == 0) {
			output_config = c;
			break;
		} else if (strcmp(c->name, "*") == 0) {
			output_config = c;
		}
	}

	if (output_config) {
		if (!output_config->image && output_config->image_path) {
			load_config_image(output_config);
		}
		output->config = output_config;
		create_output_surface(output);
	} else {
		// remove unwanted outputs
		wl_list_remove(&output->link);
		wl_output_destroy(output->wl_output);
		free(output);
		if (wl_list_length(&state->outputs) == 0) {
			// do not terminate in case a matching output is later added
			sway_log(SWAY_INFO, "Warning: no current outputs configured to be shown on\n");
		}
	}
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.description = noop,
	.done = noop,
	.logical_position = noop,
	.logical_size = noop,
	.name = xdg_output_handle_name,
};

static void setup_output(struct swaybg_output *output) {
	struct zxdg_output_manager_v1 *output_manager =
		output->state->xdg_output_manager;
	struct zxdg_output_v1 *xdg_output =
		zxdg_output_manager_v1_get_xdg_output(output_manager, output->wl_output);
	zxdg_output_v1_add_listener(xdg_output, &xdg_output_listener, output);
}

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	struct swaybg_output *output = data;
	output->scale = scale;
	render_frame(output->state);
}

static const struct wl_output_listener output_listener = {
	.done = noop,
	.geometry = noop,
	.mode = noop,
	.scale = output_scale,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaybg_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor =
			wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaybg_output *output = calloc(1, sizeof(struct swaybg_output));
		output->state = state;
		output->wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 3);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		if (state->running) {
			setup_output(output);
		}
		wl_list_insert(&state->outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell =
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, 2);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = noop,
};


int main(int argc, char **argv) {
	sway_log_init(SWAY_INFO, NULL);

	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"output", required_argument, NULL, 'o'},
		{"image", required_argument, NULL, 'i'},
		{"mode", required_argument, NULL, 'm'},
		{"color", required_argument, NULL, 'c'},
		{0}
	};

	wl_list_init(&state.output_configs);

	int n_duplicate_configs = 0;
	int n_options = 0;
	int last_option, c = 'o';
	while (true) {
		last_option = c;
		c = getopt_long(argc, argv, "o:i:m:c:h", long_options, NULL);
		if (c == -1) {
			break;
		} else if (c == ':') {
			return EXIT_FAILURE;
		} else if (c == 'h' || c == '?') {
			printf("Usage: %s [options]\n", argv[0]);
			// TODO
			return EXIT_SUCCESS;
		}

		if (c == 'o') {
			if (last_option == 'o' && wl_list_length(&state.output_configs) > 0) {
				break;
			}

			bool config_exists = false;
			struct swaybg_output_config *output_config;
			wl_list_for_each(output_config, &state.output_configs, link) {
				if (strcmp(output_config->name, optarg) == 0) { // move to end
					config_exists = true;
					wl_list_remove(&output_config->link); // TODO is this safe?
					break;
				}
			}
			if (!config_exists) {
				output_config = calloc(1, sizeof(struct swaybg_output_config));
				output_config->name = optarg;
			}
			wl_list_insert(&state.output_configs, &output_config->link);
			n_duplicate_configs = n_options == 0 ? n_duplicate_configs + 1 : 1;
			continue;
		} else if (wl_list_length(&state.output_configs) == 0) {
			struct swaybg_output_config *output_config =
				calloc(1, sizeof(struct swaybg_output_config));
			output_config->name = "*";
			wl_list_insert(&state.output_configs, &output_config->link);
			n_duplicate_configs = 1;
		}

		n_options++;
		int i = 0;
		struct swaybg_output_config *output_config;
		wl_list_for_each(output_config, &state.output_configs, link) {
			if (i++ == n_duplicate_configs) {
				break;
			}

			switch (c) {
			case 'i': output_config->image_path = optarg; break;
			case 'm': {
				output_config->mode = parse_background_mode(optarg);
				if (output_config->mode == BACKGROUND_MODE_INVALID) {
					// invalid mode
					return 1;
				}
				break;
			}
			case 'c': output_config->color = parse_color(optarg); break;
			}
		}
	}

	if (last_option == 'o') {
		sway_log(SWAY_ERROR, "No options provided for last output, terminating");
		return 1;
	}

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		sway_log(SWAY_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return 2;
	}

	wl_list_init(&state.outputs);

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);
	if (state.compositor == NULL || state.shm == NULL ||
			state.layer_shell == NULL || state.xdg_output_manager == NULL) {
		sway_log(SWAY_ERROR, "Missing a required Wayland interface");
		return 2;
	}

	struct swaybg_output *output;
	wl_list_for_each(output, &state.outputs, link) {
		setup_output(output);
	}

	struct sigaction sa = { .sa_handler = reload_images };
	sigaction(SIGUSR1, &sa, NULL);

	state.running = true;
	while (state.running && wl_display_dispatch(state.display) != -1) {
		// This space intentionally left blank
	}

	// TODO cleanup and handle signals

	return EXIT_SUCCESS;
}
