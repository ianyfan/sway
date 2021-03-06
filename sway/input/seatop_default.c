#define _POSIX_C_SOURCE 200809L
#include <float.h>
#include <libevdev/libevdev.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/tree/view.h"
#include "log.h"

struct seatop_default_event {
	struct sway_node *previous_node;
	uint32_t pressed_buttons[SWAY_CURSOR_PRESSED_BUTTONS_CAP];
	size_t pressed_button_count;
};

/*-----------------------------------------\
 * Functions shared by multiple callbacks  /
 *---------------------------------------*/

/**
 * Determine if the edge of the given container is on the edge of the
 * workspace/output.
 */
static bool edge_is_external(struct sway_container *cont, enum wlr_edges edge) {
	enum sway_container_layout layout = L_NONE;
	switch (edge) {
	case WLR_EDGE_TOP:
	case WLR_EDGE_BOTTOM:
		layout = L_VERT;
		break;
	case WLR_EDGE_LEFT:
	case WLR_EDGE_RIGHT:
		layout = L_HORIZ;
		break;
	case WLR_EDGE_NONE:
		sway_assert(false, "Never reached");
		return false;
	}

	// Iterate the parents until we find one with the layout we want,
	// then check if the child has siblings between it and the edge.
	while (cont) {
		if (container_parent_layout(cont) == layout) {
			list_t *siblings = container_get_siblings(cont);
			int index = list_find(siblings, cont);
			if (index > 0 && (edge == WLR_EDGE_LEFT || edge == WLR_EDGE_TOP)) {
				return false;
			}
			if (index < siblings->length - 1 &&
					(edge == WLR_EDGE_RIGHT || edge == WLR_EDGE_BOTTOM)) {
				return false;
			}
		}
		cont = cont->parent;
	}
	return true;
}

static enum wlr_edges find_edge(struct sway_container *cont,
		struct sway_cursor *cursor) {
	if (!cont->view) {
		return WLR_EDGE_NONE;
	}
	if (cont->border == B_NONE || !cont->border_thickness ||
			cont->border == B_CSD) {
		return WLR_EDGE_NONE;
	}

	enum wlr_edges edge = 0;
	if (cursor->cursor->x < cont->x + cont->border_thickness) {
		edge |= WLR_EDGE_LEFT;
	}
	if (cursor->cursor->y < cont->y + cont->border_thickness) {
		edge |= WLR_EDGE_TOP;
	}
	if (cursor->cursor->x >= cont->x + cont->width - cont->border_thickness) {
		edge |= WLR_EDGE_RIGHT;
	}
	if (cursor->cursor->y >= cont->y + cont->height - cont->border_thickness) {
		edge |= WLR_EDGE_BOTTOM;
	}

	return edge;
}

/**
 * If the cursor is over a _resizable_ edge, return the edge.
 * Edges that can't be resized are edges of the workspace.
 */
static enum wlr_edges find_resize_edge(struct sway_container *cont,
		struct sway_cursor *cursor) {
	enum wlr_edges edge = find_edge(cont, cursor);
	if (edge && !container_is_floating(cont) && edge_is_external(cont, edge)) {
		return WLR_EDGE_NONE;
	}
	return edge;
}

/**
 * Return the mouse binding which matches modifier, click location, release,
 * and pressed button state, otherwise return null.
 */
static struct sway_binding* get_active_mouse_binding(
		struct seatop_default_event *e, list_t *bindings, uint32_t modifiers,
		bool release, bool on_titlebar, bool on_border, bool on_content,
		bool on_workspace, const char *identifier) {
	uint32_t click_region =
			((on_titlebar || on_workspace) ? BINDING_TITLEBAR : 0) |
			((on_border || on_workspace) ? BINDING_BORDER : 0) |
			((on_content || on_workspace) ? BINDING_CONTENTS : 0);

	struct sway_binding *current = NULL;
	for (int i = 0; i < bindings->length; ++i) {
		struct sway_binding *binding = bindings->items[i];
		if (modifiers ^ binding->modifiers ||
				e->pressed_button_count != (size_t)binding->keys->length ||
				release != (binding->flags & BINDING_RELEASE) ||
				!(click_region & binding->flags) ||
				(on_workspace &&
				 (click_region & binding->flags) != click_region) ||
				(strcmp(binding->input, identifier) != 0 &&
				 strcmp(binding->input, "*") != 0)) {
			continue;
		}

		bool match = true;
		for (size_t j = 0; j < e->pressed_button_count; j++) {
			uint32_t key = *(uint32_t *)binding->keys->items[j];
			if (key != e->pressed_buttons[j]) {
				match = false;
				break;
			}
		}
		if (!match) {
			continue;
		}

		if (!current || strcmp(current->input, "*") == 0) {
			current = binding;
			if (strcmp(current->input, identifier) == 0) {
				// If a binding is found for the exact input, quit searching
				break;
			}
		}
	}
	return current;
}

/**
 * Remove a button (and duplicates) from the sorted list of currently pressed
 * buttons.
 */
static void state_erase_button(struct seatop_default_event *e,
		uint32_t button) {
	size_t j = 0;
	for (size_t i = 0; i < e->pressed_button_count; ++i) {
		if (i > j) {
			e->pressed_buttons[j] = e->pressed_buttons[i];
		}
		if (e->pressed_buttons[i] != button) {
			++j;
		}
	}
	while (e->pressed_button_count > j) {
		--e->pressed_button_count;
		e->pressed_buttons[e->pressed_button_count] = 0;
	}
}

/**
 * Add a button to the sorted list of currently pressed buttons, if there
 * is space.
 */
static void state_add_button(struct seatop_default_event *e, uint32_t button) {
	if (e->pressed_button_count >= SWAY_CURSOR_PRESSED_BUTTONS_CAP) {
		return;
	}
	size_t i = 0;
	while (i < e->pressed_button_count && e->pressed_buttons[i] < button) {
		++i;
	}
	size_t j = e->pressed_button_count;
	while (j > i) {
		e->pressed_buttons[j] = e->pressed_buttons[j - 1];
		--j;
	}
	e->pressed_buttons[i] = button;
	e->pressed_button_count++;
}

static void cursor_do_rebase(struct sway_cursor *cursor, uint32_t time_msec,
		struct sway_node *node, struct wlr_surface *surface,
		double sx, double sy) {
	// Handle cursor image
	if (surface) {
		// Reset cursor if switching between clients
		struct wl_client *client = wl_resource_get_client(surface->resource);
		if (client != cursor->image_client) {
			cursor_set_image(cursor, "left_ptr", client);
		}
	} else if (node && node->type == N_CONTAINER) {
		// Try a node's resize edge
		enum wlr_edges edge = find_resize_edge(node->sway_container, cursor);
		if (edge == WLR_EDGE_NONE) {
			cursor_set_image(cursor, "left_ptr", NULL);
		} else if (container_is_floating(node->sway_container)) {
			cursor_set_image(cursor, wlr_xcursor_get_resize_name(edge), NULL);
		} else {
			if (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
				cursor_set_image(cursor, "col-resize", NULL);
			} else {
				cursor_set_image(cursor, "row-resize", NULL);
			}
		}
	} else {
		cursor_set_image(cursor, "left_ptr", NULL);
	}

	// Send pointer enter/leave
	struct wlr_seat *wlr_seat = cursor->seat->wlr_seat;
	if (surface) {
		if (seat_is_input_allowed(cursor->seat, surface)) {
			wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(wlr_seat, time_msec, sx, sy);
		}
	} else {
		wlr_seat_pointer_clear_focus(wlr_seat);
	}
}

/*----------------------------------\
 * Functions used by handle_button  /
 *--------------------------------*/

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wlr_button_state state) {
	struct seatop_default_event *e = seat->seatop_data;
	struct sway_cursor *cursor = seat->cursor;

	// Determine what's under the cursor
	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct sway_node *node = node_at_coords(seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);
	struct sway_container *cont = node && node->type == N_CONTAINER ?
		node->sway_container : NULL;
	bool is_floating = cont && container_is_floating(cont);
	bool is_floating_or_child = cont && container_is_floating_or_child(cont);
	bool is_fullscreen_or_child = cont && container_is_fullscreen_or_child(cont);
	enum wlr_edges edge = cont ? find_edge(cont, cursor) : WLR_EDGE_NONE;
	enum wlr_edges resize_edge = edge ?
		find_resize_edge(cont, cursor) : WLR_EDGE_NONE;
	bool on_border = edge != WLR_EDGE_NONE;
	bool on_contents = cont && !on_border && surface;
	bool on_workspace = node && node->type == N_WORKSPACE;
	bool on_titlebar = cont && !on_border && !surface;

	// Handle mouse bindings
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
	uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

	char *device_identifier = device ? input_device_get_identifier(device)
		: strdup("*");
	struct sway_binding *binding = NULL;
	if (state == WLR_BUTTON_PRESSED) {
		state_add_button(e, button);
		binding = get_active_mouse_binding(e,
			config->current_mode->mouse_bindings, modifiers, false,
			on_titlebar, on_border, on_contents, on_workspace,
			device_identifier);
	} else {
		binding = get_active_mouse_binding(e,
			config->current_mode->mouse_bindings, modifiers, true,
			on_titlebar, on_border, on_contents, on_workspace,
			device_identifier);
		state_erase_button(e, button);
	}
	free(device_identifier);
	if (binding) {
		seat_execute_command(seat, binding);
		return;
	}

	// Handle clicking an empty workspace
	if (node && node->type == N_WORKSPACE) {
		seat_set_focus(seat, node);
		return;
	}

	// Handle clicking a layer surface
	if (surface && wlr_surface_is_layer_surface(surface)) {
		struct wlr_layer_surface_v1 *layer =
			wlr_layer_surface_v1_from_wlr_surface(surface);
		if (layer->current.keyboard_interactive) {
			seat_set_focus_layer(seat, layer);
		}
		seat_pointer_notify_button(seat, time_msec, button, state);
		return;
	}

	// Handle tiling resize via border
	if (cont && resize_edge && button == BTN_LEFT &&
			state == WLR_BUTTON_PRESSED && !is_floating) {
		seat_set_focus_container(seat, cont);
		seatop_begin_resize_tiling(seat, cont, edge);
		return;
	}

	// Handle tiling resize via mod
	bool mod_pressed = keyboard &&
		(wlr_keyboard_get_modifiers(keyboard) & config->floating_mod);
	if (cont && !is_floating_or_child && mod_pressed &&
			state == WLR_BUTTON_PRESSED) {
		uint32_t btn_resize = config->floating_mod_inverse ?
			BTN_LEFT : BTN_RIGHT;
		if (button == btn_resize) {
			edge = 0;
			edge |= cursor->cursor->x > cont->x + cont->width / 2 ?
				WLR_EDGE_RIGHT : WLR_EDGE_LEFT;
			edge |= cursor->cursor->y > cont->y + cont->height / 2 ?
				WLR_EDGE_BOTTOM : WLR_EDGE_TOP;

			const char *image = NULL;
			if (edge == (WLR_EDGE_LEFT | WLR_EDGE_TOP)) {
				image = "nw-resize";
			} else if (edge == (WLR_EDGE_TOP | WLR_EDGE_RIGHT)) {
				image = "ne-resize";
			} else if (edge == (WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM)) {
				image = "se-resize";
			} else if (edge == (WLR_EDGE_BOTTOM | WLR_EDGE_LEFT)) {
				image = "sw-resize";
			}
			cursor_set_image(seat->cursor, image, NULL);
			seat_set_focus_container(seat, cont);
			seatop_begin_resize_tiling(seat, cont, edge);
			return;
		}
	}

	// Handle beginning floating move
	if (cont && is_floating_or_child && !is_fullscreen_or_child &&
			state == WLR_BUTTON_PRESSED) {
		uint32_t btn_move = config->floating_mod_inverse ? BTN_RIGHT : BTN_LEFT;
		if (button == btn_move && state == WLR_BUTTON_PRESSED &&
				(mod_pressed || on_titlebar)) {
			while (cont->parent) {
				cont = cont->parent;
			}
			seat_set_focus_container(seat, cont);
			seatop_begin_move_floating(seat, cont);
			return;
		}
	}

	// Handle beginning floating resize
	if (cont && is_floating_or_child && !is_fullscreen_or_child &&
			state == WLR_BUTTON_PRESSED) {
		// Via border
		if (button == BTN_LEFT && resize_edge != WLR_EDGE_NONE) {
			seatop_begin_resize_floating(seat, cont, resize_edge);
			return;
		}

		// Via mod+click
		uint32_t btn_resize = config->floating_mod_inverse ?
			BTN_LEFT : BTN_RIGHT;
		if (mod_pressed && button == btn_resize) {
			struct sway_container *floater = cont;
			while (floater->parent) {
				floater = floater->parent;
			}
			edge = 0;
			edge |= cursor->cursor->x > floater->x + floater->width / 2 ?
				WLR_EDGE_RIGHT : WLR_EDGE_LEFT;
			edge |= cursor->cursor->y > floater->y + floater->height / 2 ?
				WLR_EDGE_BOTTOM : WLR_EDGE_TOP;
			seatop_begin_resize_floating(seat, floater, edge);
			return;
		}
	}

	// Handle moving a tiling container
	if (config->tiling_drag && (mod_pressed || on_titlebar) &&
			state == WLR_BUTTON_PRESSED && !is_floating_or_child &&
			cont && cont->fullscreen_mode == FULLSCREEN_NONE) {
		struct sway_container *focus = seat_get_focused_container(seat);
		bool focused = focus == cont || container_has_ancestor(focus, cont);
		if (on_titlebar && !focused) {
			node = seat_get_focus_inactive(seat, &cont->node);
			seat_set_focus(seat, node);
		}

		// If moving a container by it's title bar, use a threshold for the drag
		if (!mod_pressed && config->tiling_drag_threshold > 0) {
			seatop_begin_move_tiling_threshold(seat, cont);
		} else {
			seatop_begin_move_tiling(seat, cont);
		}
		return;
	}

	// Handle mousedown on a container surface
	if (surface && cont && state == WLR_BUTTON_PRESSED) {
		seat_set_focus_container(seat, cont);
		seatop_begin_down(seat, cont, time_msec, sx, sy);
		seat_pointer_notify_button(seat, time_msec, button, WLR_BUTTON_PRESSED);
		return;
	}

	// Handle clicking a container surface or decorations
	if (cont) {
		node = seat_get_focus_inactive(seat, &cont->node);
		seat_set_focus(seat, node);
		seat_pointer_notify_button(seat, time_msec, button, state);
		return;
	}

	seat_pointer_notify_button(seat, time_msec, button, state);
}

/*----------------------------------\
 * Functions used by handle_motion  /
 *--------------------------------*/

static void check_focus_follows_mouse(struct sway_seat *seat,
		struct seatop_default_event *e, struct sway_node *hovered_node) {
	struct sway_node *focus = seat_get_focus(seat);

	// If a workspace node is hovered (eg. in the gap area), only set focus if
	// the workspace is on a different output to the previous focus.
	if (focus && hovered_node->type == N_WORKSPACE) {
		struct sway_output *focused_output = node_get_output(focus);
		struct sway_output *hovered_output = node_get_output(hovered_node);
		if (hovered_output != focused_output) {
			seat_set_focus(seat, seat_get_focus_inactive(seat, hovered_node));
		}
		return;
	}

	if (node_is_view(hovered_node)) {
		if (hovered_node != e->previous_node ||
				config->focus_follows_mouse == FOLLOWS_ALWAYS) {
			seat_set_focus(seat, hovered_node);
		} else {
			// Focusing a tab which contains a split child
			struct sway_node *next_focus =
				seat_get_focus_inactive(seat, &root->node);
			if (next_focus && node_is_view(next_focus) &&
					view_is_visible(next_focus->sway_container->view)) {
				seat_set_focus(seat, next_focus);
			}
		}
	}
}

static void handle_motion(struct sway_seat *seat, uint32_t time_msec,
		double dx, double dy) {
	struct seatop_default_event *e = seat->seatop_data;
	struct sway_cursor *cursor = seat->cursor;

	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct sway_node *node = node_at_coords(seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);

	if (node && config->focus_follows_mouse != FOLLOWS_NO) {
		check_focus_follows_mouse(seat, e, node);
	}

	cursor_do_rebase(cursor, time_msec, node, surface, sx, sy);

	struct sway_drag_icon *drag_icon;
	wl_list_for_each(drag_icon, &root->drag_icons, link) {
		if (drag_icon->seat == seat) {
			drag_icon_update_position(drag_icon);
		}
	}

	e->previous_node = node;
}

/*--------------------------------\
 * Functions used by handle_axis  /
 *------------------------------*/

static uint32_t wl_axis_to_button(struct wlr_event_pointer_axis *event) {
	switch (event->orientation) {
	case WLR_AXIS_ORIENTATION_VERTICAL:
		return event->delta < 0 ? SWAY_SCROLL_UP : SWAY_SCROLL_DOWN;
	case WLR_AXIS_ORIENTATION_HORIZONTAL:
		return event->delta < 0 ? SWAY_SCROLL_LEFT : SWAY_SCROLL_RIGHT;
	default:
		sway_log(SWAY_DEBUG, "Unknown axis orientation");
		return 0;
	}
}

static void handle_axis(struct sway_seat *seat,
		struct wlr_event_pointer_axis *event) {
	struct sway_input_device *input_device =
		event->device ? event->device->data : NULL;
	struct input_config *ic =
		input_device ? input_device_get_config(input_device) : NULL;
	struct sway_cursor *cursor = seat->cursor;
	struct seatop_default_event *e = seat->seatop_data;

	// Determine what's under the cursor
	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct sway_node *node = node_at_coords(seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);
	struct sway_container *cont = node && node->type == N_CONTAINER ?
		node->sway_container : NULL;
	enum wlr_edges edge = cont ? find_edge(cont, cursor) : WLR_EDGE_NONE;
	bool on_border = edge != WLR_EDGE_NONE;
	bool on_titlebar = cont && !on_border && !surface;
	bool on_titlebar_border = cont && on_border &&
		cursor->cursor->y < cont->content_y;
	bool on_contents = cont && !on_border && surface;
	bool on_workspace = node && node->type == N_WORKSPACE;
	float scroll_factor =
		(ic == NULL || ic->scroll_factor == FLT_MIN) ? 1.0f : ic->scroll_factor;

	bool handled = false;

	// Gather information needed for mouse bindings
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
	uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
	struct wlr_input_device *device =
		input_device ? input_device->wlr_device : NULL;
	char *dev_id = device ? input_device_get_identifier(device) : strdup("*");
	uint32_t button = wl_axis_to_button(event);

	// Handle mouse bindings - x11 mouse buttons 4-7 - press event
	struct sway_binding *binding = NULL;
	state_add_button(e, button);
	binding = get_active_mouse_binding(e, config->current_mode->mouse_bindings,
			modifiers, false, on_titlebar, on_border, on_contents, on_workspace,
			dev_id);
	if (binding) {
		seat_execute_command(seat, binding);
		handled = true;
	}

	// Scrolling on a tabbed or stacked title bar (handled as press event)
	if (!handled && (on_titlebar || on_titlebar_border)) {
		enum sway_container_layout layout = container_parent_layout(cont);
		if (layout == L_TABBED || layout == L_STACKED) {
			struct sway_node *tabcontainer = node_get_parent(node);
			struct sway_node *active =
				seat_get_active_tiling_child(seat, tabcontainer);
			list_t *siblings = container_get_siblings(cont);
			int desired = list_find(siblings, active->sway_container) +
				round(scroll_factor * event->delta_discrete);
			if (desired < 0) {
				desired = 0;
			} else if (desired >= siblings->length) {
				desired = siblings->length - 1;
			}
			struct sway_node *old_focus = seat_get_focus(seat);
			struct sway_container *new_sibling_con = siblings->items[desired];
			struct sway_node *new_sibling = &new_sibling_con->node;
			struct sway_node *new_focus =
				seat_get_focus_inactive(seat, new_sibling);
			if (node_has_ancestor(old_focus, tabcontainer)) {
				seat_set_focus(seat, new_focus);
			} else {
				// Scrolling when focus is not in the tabbed container at all
				seat_set_raw_focus(seat, new_sibling);
				seat_set_raw_focus(seat, new_focus);
				seat_set_raw_focus(seat, old_focus);
			}
			handled = true;
		}
	}

	// Handle mouse bindings - x11 mouse buttons 4-7 - release event
	binding = get_active_mouse_binding(e, config->current_mode->mouse_bindings,
			modifiers, true, on_titlebar, on_border, on_contents, on_workspace,
			dev_id);
	state_erase_button(e, button);
	if (binding) {
		seat_execute_command(seat, binding);
		handled = true;
	}
	free(dev_id);

	if (!handled) {
		wlr_seat_pointer_notify_axis(cursor->seat->wlr_seat, event->time_msec,
			event->orientation, scroll_factor * event->delta,
			round(scroll_factor * event->delta_discrete), event->source);
	}
}

/*----------------------------------\
 * Functions used by handle_rebase  /
 *--------------------------------*/

static void handle_rebase(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_default_event *e = seat->seatop_data;
	struct sway_cursor *cursor = seat->cursor;
	struct wlr_surface *surface = NULL;
	double sx = 0.0, sy = 0.0;
	e->previous_node = node_at_coords(seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);
	cursor_do_rebase(cursor, time_msec, e->previous_node, surface, sx, sy);
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.motion = handle_motion,
	.axis = handle_axis,
	.rebase = handle_rebase,
	.allow_set_cursor = true,
};

void seatop_begin_default(struct sway_seat *seat) {
	seatop_end(seat);

	struct seatop_default_event *e =
		calloc(1, sizeof(struct seatop_default_event));
	sway_assert(e, "Unable to allocate seatop_default_event");
	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	seatop_rebase(seat, 0);
}
