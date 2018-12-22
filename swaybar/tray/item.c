#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/tray/host.h"
#include "swaybar/tray/icon.h"
#include "swaybar/tray/item.h"
#include "swaybar/tray/tray.h"
#include "background-image.h"
#include "cairo.h"
#include "list.h"
#include "log.h"

// TODO menu

static bool sni_ready(struct swaybar_sni *sni) {
	return sni->status && (sni->status[0] == 'N' ?
			sni->attention_icon_name || sni->attention_icon_pixmap :
			sni->icon_name || sni->icon_pixmap);
}

static int read_pixmap(sd_bus_message *msg, struct swaybar_sni *sni,
		const char *prop, list_t **dest) {
	int ret = sd_bus_message_enter_container(msg, 'a', "(iiay)");
	if (ret < 0) {
		wlr_log(WLR_DEBUG, "Failed to read property %s: %s", prop, strerror(-ret));
		return ret;
	}

	list_t *pixmaps = create_list();
	if (!pixmaps) {
		return -12; // -ENOMEM
	}

	while (!sd_bus_message_at_end(msg, 0)) {
		ret = sd_bus_message_enter_container(msg, 'r', "iiay");
		if (ret < 0) {
			wlr_log(WLR_DEBUG, "Failed to read property %s: %s", prop, strerror(-ret));
			goto error;
		}

		int size;
		ret = sd_bus_message_read(msg, "ii", NULL, &size);
		if (ret < 0) {
			wlr_log(WLR_DEBUG, "Failed to read property %s: %s", prop, strerror(-ret));
			goto error;
		}

		const void *pixels;
		size_t npixels;
		ret = sd_bus_message_read_array(msg, 'y', &pixels, &npixels);
		if (ret < 0) {
			wlr_log(WLR_DEBUG, "Failed to read property %s: %s", prop, strerror(-ret));
			goto error;
		}

		struct swaybar_pixmap *pixmap =
			malloc(sizeof(struct swaybar_pixmap) + npixels);
		pixmap->size = size;
		memcpy(pixmap->pixels, pixels, npixels);
		list_add(pixmaps, pixmap);

		sd_bus_message_exit_container(msg);
	}
	*dest = pixmaps;

	return ret;
error:
	list_free_items_and_destroy(pixmaps);
	return ret;
}

struct get_property_data {
	struct swaybar_sni *sni;
	const char *prop;
	const char *type;
	void *dest;
};

static int get_property_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct get_property_data *d = data;
	struct swaybar_sni *sni = d->sni;
	const char *prop = d->prop;
	const char *type = d->type;
	void *dest = d->dest;

	int ret;
	if (sd_bus_message_is_method_error(msg, NULL)) {
		sd_bus_error err = *sd_bus_message_get_error(msg);
		wlr_log(WLR_DEBUG, "Failed to get property %s: %s", prop, err.message);
		ret = -sd_bus_error_get_errno(&err);
		goto cleanup;
	}

	ret = sd_bus_message_enter_container(msg, 'v', type);
	if (ret < 0) {
		wlr_log(WLR_DEBUG, "Failed to read property %s: %s", prop, strerror(-ret));
		goto cleanup;
	}

	if (!type) {
		ret = read_pixmap(msg, sni, prop, dest);
		if (ret < 0) {
			goto cleanup;
		}
	} else {
		ret = sd_bus_message_read(msg, type, dest);
		if (ret < 0) {
			wlr_log(WLR_DEBUG, "Failed to read property %s: %s", prop,
					strerror(-ret));
			goto cleanup;
		} else if (*type == 's' || *type == 'o') {
			char **str = dest;
			*str = strdup(*str);
		}
	}
cleanup:
	free(data);
	return ret;
}

static void sni_get_property_async(struct swaybar_sni *sni, const char *prop,
		const char *type, void *dest) {
	struct get_property_data *data = malloc(sizeof(struct get_property_data));
	data->sni = sni;
	data->prop = prop;
	data->type = type;
	data->dest = dest;
	int ret = sd_bus_call_method_async(sni->tray->bus, NULL, sni->service,
			sni->path, "org.freedesktop.DBus.Properties", "Get",
			get_property_callback, data, "ss", sni->interface, prop);
	if (ret < 0) {
		wlr_log(WLR_DEBUG, "Failed to get property %s: %s", prop, strerror(-ret));
	}
}

static int handle_new_icon(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	wlr_log(WLR_DEBUG, "%s has new IconName", sni->watcher_id);

	free(sni->icon_name);
	sni->icon_name = NULL;
	sni_get_property_async(sni, "IconName", "s", &sni->icon_name);

	list_free_items_and_destroy(sni->icon_pixmap);
	sni->icon_pixmap = NULL;
	sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);

	return 0;
}

static int handle_new_attention_icon(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	wlr_log(WLR_DEBUG, "%s has new AttentionIconName", sni->watcher_id);

	free(sni->attention_icon_name);
	sni->attention_icon_name = NULL;
	sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);

	list_free_items_and_destroy(sni->attention_icon_pixmap);
	sni->attention_icon_pixmap = NULL;
	sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);

	return 0;
}

static int handle_new_status(sd_bus_message *msg, void *data, sd_bus_error *error) {
	char *status;
	int ret = sd_bus_message_read(msg, "s", &status);
	if (ret < 0) {
		wlr_log(WLR_DEBUG, "Failed to read new status message: %s", strerror(-ret));
	} else {
		struct swaybar_sni *sni = data;
		free(sni->status);
		sni->status = strdup(status);
		wlr_log(WLR_DEBUG, "%s has new Status '%s'", sni->watcher_id, status);
	}
	return ret;
}

static void sni_match_signal(struct swaybar_sni *sni, char *signal,
		sd_bus_message_handler_t callback) {
	int ret = sd_bus_match_signal(sni->tray->bus, NULL, sni->service, sni->path,
			sni->interface, signal, callback, sni);
	if (ret < 0) {
		wlr_log(WLR_DEBUG, "Failed to subscribe to signal %s: %s", signal,
				strerror(-ret));
	}
}

struct swaybar_sni *create_sni(char *id, struct swaybar_tray *tray) {
	struct swaybar_sni *sni = calloc(1, sizeof(struct swaybar_sni));
	if (!sni) {
		return NULL;
	}
	sni->tray = tray;
	sni->watcher_id = strdup(id);
	char *path_ptr = strchr(id, '/');
	if (!path_ptr) {
		sni->service = strdup(id);
		sni->path = strdup("/StatusNotifierItem");
		sni->interface = "org.freedesktop.StatusNotifierItem";
	} else {
		sni->service = strndup(id, path_ptr - id);
		sni->path = strdup(path_ptr);
		sni->interface = "org.kde.StatusNotifierItem";
	}

	// Ignored: Category, Id, Title, WindowId, OverlayIconName,
	//          OverlayIconPixmap, AttentionMovieName, ToolTip
	sni_get_property_async(sni, "Status", "s", &sni->status);
	sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
	sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);
	sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
	sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);
	sni_get_property_async(sni, "ItemIsMenu", "b", &sni->item_is_menu);
	sni_get_property_async(sni, "Menu", "o", &sni->menu);

	sni_match_signal(sni, "NewIcon", handle_new_icon);
	sni_match_signal(sni, "NewAttentionIcon", handle_new_attention_icon);
	sni_match_signal(sni, "NewStatus", handle_new_status);

	return sni;
}

void destroy_sni(struct swaybar_sni *sni) {
	if (!sni) {
		return;
	}

	free(sni->watcher_id);
	free(sni->service);
	free(sni->path);
	free(sni->status);
	free(sni->icon_name);
	free(sni->icon_pixmap);
	free(sni->attention_icon_name);
	free(sni->menu);
	free(sni);
}

uint32_t render_sni(cairo_t *cairo, struct swaybar_output *output, double *x,
		struct swaybar_sni *sni) {
	uint32_t height = output->height * output->scale;
	int padding = output->bar->config->tray_padding;
	int ideal_size = height - 2*padding;
	if ((ideal_size < sni->min_size || ideal_size > sni->max_size) && sni_ready(sni)) {
		bool icon_found = false;
		char *icon_name = sni->status[0] == 'N' ?
			sni->attention_icon_name : sni->icon_name;
		if (icon_name) {
			char *icon_path = find_icon(sni->tray->themes, sni->tray->basedirs,
					icon_name, ideal_size, output->bar->config->icon_theme,
					&sni->min_size, &sni->max_size);
			if (icon_path) {
				cairo_surface_destroy(sni->icon);
				sni->icon = load_background_image(icon_path);
				free(icon_path);
				icon_found = true;
			}
		}
		if (!icon_found) {
			list_t *pixmaps = sni->status[0] == 'N' ?
				sni->attention_icon_pixmap : sni->icon_pixmap;
			if (pixmaps) {
				unsigned smallest_error = -1; // UINT_MAX
				int idx = -1;
				for (int i = 0; i < pixmaps->length; ++i) {
					struct swaybar_pixmap *pixmap = pixmaps->items[i];
					unsigned error = (ideal_size - pixmap->size) *
						(ideal_size < pixmap->size ? -1 : 1);
					if (error < smallest_error) {
						smallest_error = error;
						idx = i;
					}
				}
				struct swaybar_pixmap *pixmap = pixmaps->items[idx];
				cairo_surface_destroy(sni->icon);
				sni->icon = cairo_image_surface_create_for_data(pixmap->pixels,
						CAIRO_FORMAT_ARGB32, pixmap->size, pixmap->size,
						cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pixmap->size));
			}
		}
	}

	if (!sni->icon) {
		// TODO fallback
		return 0;
	}

	cairo_surface_t *icon;
	int actual_size = cairo_image_surface_get_height(sni->icon);
	int icon_size = actual_size < ideal_size ?
		actual_size * (ideal_size / actual_size) : ideal_size;
	icon = cairo_image_surface_scale(sni->icon, icon_size, icon_size);

	int padded_size = icon_size + 2*padding;
	*x -= padded_size;
	int y = floor((height - padded_size) / 2.0);

	cairo_operator_t op = cairo_get_operator(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	cairo_set_source_surface(cairo, icon, *x + padding, y + padding);
	cairo_rectangle(cairo, *x, y, padded_size, padded_size);
	cairo_fill(cairo);
	cairo_set_operator(cairo, op);

	cairo_surface_destroy(icon);

	return output->height;
}
