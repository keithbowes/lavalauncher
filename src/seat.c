/*
 * LavaLauncher - A simple launcher panel for Wayland
 *
 * Copyright (C) 2020 - 2021 Leon Henrik Plickat
 * Copyright (C) 2020 Nicolai Dagestad
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L

#include<stdio.h>
#include<stdlib.h>
#include<stdbool.h>
#include<unistd.h>
#include<string.h>
#include<errno.h>
#include<sys/mman.h>
#include<assert.h>

#include<wayland-client.h>
#include<wayland-cursor.h>

#include"wlr-layer-shell-unstable-v1-protocol.h"
#include"xdg-output-unstable-v1-protocol.h"
#include"xdg-shell-protocol.h"

#include"lavalauncher.h"
#include"util.h"
#include"seat.h"
#include"bar.h"
#include"item.h"
#include"output.h"

/* No-Op function. */
static void noop () {}

/**************
 *            *
 *  Keyboard  *
 *            *
 **************/
#define CHECK_MOD(A, B) \
	if ( 1 == xkb_state_mod_name_is_active(seat->keyboard.state, A, XKB_STATE_MODS_EFFECTIVE) ) \
	{ \
		seat->keyboard.modifiers |= B; \
	}
static void keyboard_handle_modifiers (void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;

	log_message(3, "[input] Received modifiers.\n");

	xkb_state_update_mask(seat->keyboard.state, depressed, latched, locked, 0, 0, group);
	seat->keyboard.modifiers = 0;
	CHECK_MOD(XKB_MOD_NAME_ALT, ALT);
	CHECK_MOD(XKB_MOD_NAME_CAPS, CAPS);
	CHECK_MOD(XKB_MOD_NAME_CTRL, CONTROL);
	CHECK_MOD(XKB_MOD_NAME_LOGO, LOGO);
	CHECK_MOD(XKB_MOD_NAME_NUM, NUM);
	CHECK_MOD(XKB_MOD_NAME_SHIFT, SHIFT);
}
#undef CHECK_MOD

static void keyboard_handle_keymap (void *data, struct wl_keyboard *keyboard,
		uint32_t format, int32_t fd, uint32_t size)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;

	log_message(3, "[input] Received keymap.\n");

	char *str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if ( str != MAP_FAILED )
	{
		DESTROY_NULL(seat->keyboard.keymap, xkb_keymap_unref);
		DESTROY_NULL(seat->keyboard.state, xkb_state_unref);

		if ( NULL == (seat->keyboard.keymap = xkb_keymap_new_from_string(
						seat->keyboard.context, str,
						XKB_KEYMAP_FORMAT_TEXT_V1,
						XKB_KEYMAP_COMPILE_NO_FLAGS)) )
			log_message(0, "Error: Failed to get xkb keymap.\n");
		else if ( NULL == (seat->keyboard.state = xkb_state_new(
						seat->keyboard.keymap)) )
			log_message(0, "Error: Failed to get xkb state.\n");

		munmap(str, size);
	}

	close(fd);
}

/* These are the handlers for keyboard events. We only need the modifier status
 * and ignore everything else.
 */
static const struct wl_keyboard_listener keyboard_listener  = {
	.enter       = noop,
	.keymap      = keyboard_handle_keymap,
	.key         = noop,
	.leave       = noop,
	.modifiers   = keyboard_handle_modifiers,
	.repeat_info = noop
};

static void seat_release_keyboard (struct Lava_seat *seat)
{
	DESTROY_NULL(seat->keyboard.wl_keyboard, wl_keyboard_release);
	DESTROY_NULL(seat->keyboard.context, xkb_context_unref);
	DESTROY_NULL(seat->keyboard.keymap, xkb_keymap_unref);
	DESTROY_NULL(seat->keyboard.state, xkb_state_unref);
	seat->keyboard.modifiers = 0;
}

static void seat_bind_keyboard (struct Lava_seat *seat)
{
	log_message(2, "[seat] Binding keyboard.\n");

	seat->keyboard.wl_keyboard = wl_seat_get_keyboard(seat->wl_seat);
	wl_keyboard_add_listener(seat->keyboard.wl_keyboard, &keyboard_listener, seat);

	/* Set up xkbcommon stuff. */
	if ( NULL == (seat->keyboard.context = xkb_context_new(0)) )
	{
		log_message(0, "Error: Failed to setup xkb context.\n");
		seat_release_keyboard(seat);
		return;
	}

	struct xkb_rule_names rules = {
		.rules   = NULL,
		.model   = NULL,
		.layout  = NULL,
		.variant = NULL,
		.options = NULL
	};
	if ( NULL == (seat->keyboard.keymap = xkb_keymap_new_from_names(
					seat->keyboard.context, &rules, 0)) )
	{
		log_message(0, "Error: Failed to setup xkb keymap.\n");
		seat_release_keyboard(seat);
		return;
	}

	if ( NULL == (seat->keyboard.state = xkb_state_new(seat->keyboard.keymap)) )
	{
		log_message(0, "Error: Failed to setup xkb state.\n");
		seat_release_keyboard(seat);
		return;
	}
}

static void seat_init_keyboard (struct Lava_seat *seat)
{
	seat->keyboard.wl_keyboard = NULL;
	seat->keyboard.context     = NULL;
	seat->keyboard.keymap      = NULL;
	seat->keyboard.state       = NULL;
	seat->keyboard.modifiers   = 0;
}

/*****************
 *               *
 *  Touchpoints  *
 *               *
 *****************/
static bool create_touchpoint (struct Lava_seat *seat, int32_t id,
          struct Lava_bar_instance *instance, struct Lava_item_instance *item_instance)
{
	log_message(1, "[seat] Creating touchpoint.\n");

	TRY_NEW(struct Lava_touchpoint, touchpoint, false);

	touchpoint->id            = id;
	touchpoint->instance      = instance;
	touchpoint->item_instance = item_instance;

	item_instance->active_indicator++;
	item_instance->dirty = true;

	bar_instance_schedule_frame(instance);

	return true;
}

void destroy_touchpoint (struct Lava_touchpoint *touchpoint)
{
	counter_safe_subtract(&(touchpoint->item_instance->active_indicator), 1);
	touchpoint->item_instance->dirty = true;
	wl_list_remove(&touchpoint->link);
	bar_instance_schedule_frame(touchpoint->instance);
	free(touchpoint);
}

static void destroy_all_touchpoints (struct Lava_seat *seat)
{
	struct Lava_touchpoint *tp, *temp;
	wl_list_for_each_safe(tp, temp, &seat->touch.touchpoints, link)
		destroy_touchpoint(tp);
}

static struct Lava_touchpoint *touchpoint_from_id (struct Lava_seat *seat, int32_t id)
{
	struct Lava_touchpoint *touchpoint;
	wl_list_for_each(touchpoint, &seat->touch.touchpoints, link)
		if ( touchpoint->id == id )
			return touchpoint;
	return NULL;
}

/***********
 *         *
 *  Touch  *
 *         *
 ***********/
static void touch_handle_up (void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;
	struct Lava_touchpoint *touchpoint = touchpoint_from_id(seat, id);
	if ( touchpoint == NULL )
		return;

	log_message(1, "[input] Touch up.\n");

	item_interaction(touchpoint->item_instance->item,
			touchpoint->instance, seat,
			INTERACTION_TOUCH,
			seat->keyboard.modifiers, 0);
	destroy_touchpoint(touchpoint);
}

static void touch_handle_down (void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time,
		struct wl_surface *surface, int32_t id,
		wl_fixed_t fx, wl_fixed_t fy)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;
	int32_t x = wl_fixed_to_int(fx), y = wl_fixed_to_int(fy);

	log_message(1, "[input] Touch down: x=%d y=%d\n", x, y);

	struct Lava_bar_instance  *instance      = bar_instance_from_surface(surface);
	struct Lava_item_instance *item_instance = bar_instance_get_item_instance_from_coords(instance, x, y);
	if ( item_instance == NULL )
		return;
	if (! create_touchpoint(seat, id, instance, item_instance))
		log_message(0, "ERROR: could not create touchpoint\n");
}

static void touch_handle_motion (void *data, struct wl_touch *wl_touch,
		uint32_t time, int32_t id, wl_fixed_t fx, wl_fixed_t fy)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;
	struct Lava_touchpoint *touchpoint = touchpoint_from_id(seat, id);
	if ( touchpoint == NULL )
		return;

	log_message(2, "[input] Touch move\n");

	/* If the item under the touch point is not the same we first touched,
	 * we simply abort the touch operation.
	 */
	struct Lava_item_instance *item_instance = bar_instance_get_item_instance_from_coords(
		touchpoint->instance, wl_fixed_to_int(fx), wl_fixed_to_int(fy));
	if ( item_instance == NULL )
	{
		destroy_touchpoint(touchpoint);
		return;
	}
	if ( item_instance != touchpoint->item_instance )
		destroy_touchpoint(touchpoint); // TODO [item-rework] move touchpoint instead of killing it
}

static void touch_handle_cancel (void *raw, struct wl_touch *touch)
{
	/* The cancel event means that the compositor has decided to take over
	 * the touch-input, possibly for gestures, and that therefore we should
	 * stop caring about all active touchpoints.
	 */

	struct Lava_seat *seat = (struct Lava_seat *)raw;
	destroy_all_touchpoints(seat);
}

/* These are the handlers for touch events. We only want to interact with an
 * item, if both touch-down and touch-up were over the same item. To
 * achieve this, each touch event is stored in the wl_list, inside seat->touch.
 * This ways we can follow each of them without needing any extra logic.
 */
static const struct wl_touch_listener touch_listener = {
	.cancel      = touch_handle_cancel,
	.down        = touch_handle_down,
	.frame       = noop,
	.motion      = touch_handle_motion,
	.orientation = noop,
	.shape       = noop,
	.up          = touch_handle_up
};

static void seat_release_touch (struct Lava_seat *seat)
{
	destroy_all_touchpoints(seat);
	DESTROY_NULL(seat->touch.wl_touch, wl_touch_release);
}

static void seat_bind_touch (struct Lava_seat *seat)
{
	log_message(2, "[seat] Binding touch.\n");
	seat->touch.wl_touch = wl_seat_get_touch(seat->wl_seat);
	wl_touch_add_listener(seat->touch.wl_touch, &touch_listener, seat);
}

static void seat_init_touch (struct Lava_seat *seat)
{
	seat->touch.wl_touch = NULL;
	wl_list_init(&seat->touch.touchpoints);
}

/************
 *          *
 *  Cursor  *
 *          *
 ************/
static void seat_pointer_unset_cursor (struct Lava_seat *seat)
{
	DESTROY_NULL(seat->pointer.cursor.theme, wl_cursor_theme_destroy);
	DESTROY_NULL(seat->pointer.cursor.surface, wl_surface_destroy);

	 /* These just points back to the theme. */
	seat->pointer.cursor.wl_cursor = NULL;
	seat->pointer.cursor.image = NULL;

	seat->pointer.cursor.type = CURSOR_NONE;
}

static void seat_pointer_set_cursor (struct Lava_seat *seat, uint32_t serial,
		enum Lava_cursor_type type)
{
	if ( type == seat->pointer.cursor.type )
		return;
	seat_pointer_unset_cursor(seat);
	if ( type == CURSOR_NONE )
		return;

	seat->pointer.cursor.type = type;

	struct wl_pointer *pointer = seat->pointer.wl_pointer;

	const int32_t scale = (int32_t)seat->pointer.instance->output->scale;
	const int32_t cursor_size = seat->pointer.instance->config->cursor_size;

	seat->pointer.cursor.theme = wl_cursor_theme_load(NULL, cursor_size * scale, context.shm);
	if ( seat->pointer.cursor.theme == NULL )
	{
		log_message(0, "ERROR: Could not load cursor theme.\n");
		return;
	}

	const char *name = type == CURSOR_DEFAULT
			? str_orelse(seat->pointer.instance->config->cursor_name_default, "default")
			: str_orelse(seat->pointer.instance->config->cursor_name_hover, "pointer");

	seat->pointer.cursor.wl_cursor = wl_cursor_theme_get_cursor(seat->pointer.cursor.theme, name);
	if ( seat->pointer.cursor.wl_cursor == NULL )
	{
		log_message(0, "WARNING: Could not get cursor \"%s\".\n"
				"         This cursor is likely missing from your cursor theme.\n",
				name);
		seat_pointer_unset_cursor(seat);
		return;
	}

	seat->pointer.cursor.image = seat->pointer.cursor.wl_cursor->images[0];
	assert(seat->pointer.cursor.image); // TODO Propably not needed; A non-fatal fail would be better here anyway

	seat->pointer.cursor.surface = wl_compositor_create_surface(context.compositor);
	wl_surface_set_buffer_scale(seat->pointer.cursor.surface, scale);
	wl_surface_attach(seat->pointer.cursor.surface,
			wl_cursor_image_get_buffer(seat->pointer.cursor.image),
			0, 0);
	wl_surface_damage_buffer(seat->pointer.cursor.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(seat->pointer.cursor.surface);
	// TODO how is the thee buffer cleaned up? Investigate wayland-cursor.h

	wl_pointer_set_cursor(pointer, serial, seat->pointer.cursor.surface,
			(int32_t)seat->pointer.cursor.image->hotspot_x / scale,
			(int32_t)seat->pointer.cursor.image->hotspot_y / scale);
}

/*************
 *           *
 *  Pointer  *
 *           *
 *************/
#define CONTINUOUS_SCROLL_THRESHHOLD 10000
#define CONTINUOUS_SCROLL_TIMEOUT    1000

static void pointer_handle_leave (void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface)
{
	log_message(1, "[input] Pointer left surface.\n");

	struct Lava_seat *seat = (struct Lava_seat *)data;

	seat_pointer_unset_cursor(seat);

	/* Clean up indicators. */
	if ( seat->pointer.item_instance != NULL )
	{
		counter_safe_subtract(&(seat->pointer.item_instance->indicator), 1);
		counter_safe_subtract(&(seat->pointer.item_instance->active_indicator), (uint32_t)seat->pointer.click);
		seat->pointer.item_instance->dirty = true;
	}

	struct Lava_bar_instance *instance = seat->pointer.instance;

	seat->pointer.x             = 0;
	seat->pointer.y             = 0;
	seat->pointer.instance      = NULL;
	seat->pointer.item_instance = NULL;
	seat->pointer.click         = 0;

	bar_instance_pointer_leave(instance); // TODO [item-rework] instance->hover will be an int instead of bool
	bar_instance_schedule_frame(instance);
}

static void pointer_process_motion (struct Lava_seat *seat)
{
	struct Lava_item_instance *old_instance = seat->pointer.item_instance;
	struct Lava_item_instance *new_instance = bar_instance_get_item_instance_from_coords(
			seat->pointer.instance, (int32_t)seat->pointer.x, (int32_t)seat->pointer.y);
	seat->pointer.item_instance = new_instance;

	/* Remove indicator from previous item_instance, if exists. */
	bool need_frame = false;
	if ( old_instance != NULL )
	{
		if ( new_instance == old_instance )
			return;

		counter_safe_subtract(&(old_instance->indicator), 1);
		counter_safe_subtract(&(old_instance->active_indicator), (uint32_t)seat->pointer.click);
		old_instance->dirty = true;

		need_frame = true;
	}

	if ( new_instance == NULL )
		seat_pointer_set_cursor(seat, seat->pointer.serial, CURSOR_DEFAULT);
	else
	{
		seat_pointer_set_cursor(seat, seat->pointer.serial,
				new_instance->item->type == TYPE_BUTTON ? CURSOR_POINTER : CURSOR_DEFAULT);

		new_instance->indicator++;
		new_instance->active_indicator += (uint32_t)seat->pointer.click;
		new_instance->dirty = true;

		need_frame = true;
	}

	if (need_frame)
		bar_instance_schedule_frame(seat->pointer.instance);
}

static void pointer_handle_enter (void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t x, wl_fixed_t y)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;
	seat->pointer.serial = serial;

	seat->pointer.instance = bar_instance_from_surface(surface);
	if ( seat->pointer.instance == NULL )
	{
		/* Should be unreachable, but handling this is a good idea for
		 * debugging.
		 */
		log_message(0, "ERROR: Pointer entered unexpected surface.\n");
		return;
	}

	bar_instance_pointer_enter(seat->pointer.instance);

	seat->pointer.x = (uint32_t)wl_fixed_to_int(x);
	seat->pointer.y = (uint32_t)wl_fixed_to_int(y);
	log_message(1, "[input] Pointer entered surface: x=%d y=%d\n",
				seat->pointer.x, seat->pointer.y);
	pointer_process_motion(seat);
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;
	seat->pointer.x = (uint32_t)wl_fixed_to_int(x);
	seat->pointer.y = (uint32_t)wl_fixed_to_int(y);
	pointer_process_motion(seat);
}

static void pointer_handle_button (void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t button_state)
{
	struct Lava_seat *seat = data;
	if ( seat->pointer.instance == NULL )
	{
		log_message(0, "ERROR: Button press on unexpected surface.\n");
		return;
	}

	if ( button_state == WL_POINTER_BUTTON_STATE_PRESSED )
	{
		seat->pointer.click++;

		log_message(1, "[input] Button pressed: x=%d y=%d click=%d\n",
					seat->pointer.x, seat->pointer.y,
					seat->pointer.click);

		if ( seat->pointer.item_instance == NULL )
			return;

		seat->pointer.item_instance->active_indicator++;
		seat->pointer.item_instance->dirty = true;
	}
	else
	{
		seat->pointer.click--;

		log_message(1, "[input] Button released: x=%d y=%d click=%d\n",
					seat->pointer.x, seat->pointer.y,
					seat->pointer.click);

		if ( seat->pointer.item_instance == NULL )
			return;

		counter_safe_subtract(&(seat->pointer.item_instance->active_indicator), 1);
		seat->pointer.item_instance->dirty = true;

		item_interaction(seat->pointer.item_instance->item,
				seat->pointer.instance, seat,
				INTERACTION_MOUSE_BUTTON,
				seat->keyboard.modifiers, button);
	}

	bar_instance_schedule_frame(seat->pointer.instance);
}

static void pointer_handle_axis (void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value)
{
	/* We only handle up and down scrolling. */
	if ( axis != WL_POINTER_AXIS_VERTICAL_SCROLL )
		return;

	struct Lava_seat *seat = data;
	if ( seat->pointer.instance == NULL )
	{
		log_message(0, "ERROR: Scrolling on unexpected surface.\n");
		return;
	}

	if ( seat->pointer.discrete_steps == 0
			&& time - seat->pointer.last_update_time > CONTINUOUS_SCROLL_TIMEOUT )
		seat->pointer.value = 0;

	seat->pointer.value            += value;
	seat->pointer.last_update_time  = time;
}

static void pointer_handle_axis_discrete (void *data,
		struct wl_pointer *wl_pointer, uint32_t axis, int32_t steps)
{
	/* We only handle up and down scrolling. */
	if ( axis != WL_POINTER_AXIS_VERTICAL_SCROLL )
		return;

	struct Lava_seat *seat = data;
	if ( seat->pointer.instance == NULL )
	{
		log_message(0, "ERROR: Scrolling on unexpected surface.\n");
		return;
	}

	seat->pointer.discrete_steps += (uint32_t)abs(steps);
}

static void pointer_handle_frame (void *data, struct wl_pointer *wl_pointer)
{
	struct Lava_seat *seat = data;
	if ( seat->pointer.instance == NULL )
		return;

	if ( seat->pointer.item_instance == NULL )
		return;

	int value_change;
	uint32_t direction; /* 0 == down, 1 == up */
	if ( wl_fixed_to_int(seat->pointer.value) > 0 )
		direction = 0, value_change = -CONTINUOUS_SCROLL_THRESHHOLD;
	else
		direction = 1, value_change = CONTINUOUS_SCROLL_THRESHHOLD;

	if (seat->pointer.discrete_steps)
	{
		for (uint32_t i = 0; i < seat->pointer.discrete_steps; i++)
			item_interaction(seat->pointer.item_instance->item,
					seat->pointer.instance, seat,
					INTERACTION_MOUSE_SCROLL,
					seat->keyboard.modifiers, direction);

		seat->pointer.discrete_steps = 0;
		seat->pointer.value          = 0;
	}
	else while ( abs(seat->pointer.value) > CONTINUOUS_SCROLL_THRESHHOLD )
	{
		item_interaction(seat->pointer.item_instance->item,
				seat->pointer.instance, seat,
				INTERACTION_MOUSE_SCROLL,
				seat->keyboard.modifiers, direction);
		seat->pointer.value += value_change;
	}
}

/*
 * These are the listeners for pointer events. Only if a mouse button has been
 * pressed and released over the same bar item do we want that to interact with
 * the item. To achieve this, pointer_handle_enter() and pointer_handle_motion()
 * will update the cursor coordinates stored in the seat.
 * pointer_handle_button() will on press store the bar item under the pointer
 * in the seat. On release it will check whether the item under the pointer is
 * the one stored in the seat and interact with the item if this is the case.
 * pointer_handle_leave() will simply abort the pointer operation.
 *
 * All axis events (meaning scrolling actions) are handled in the frame event.
 * This is done, because pointing device like mice send a "click" (discrete axis
 * event) as well as a scroll value, while other devices like touchpads only
 * send a scroll value. For the second type of device, the value must be
 * converted into virtual "clicks". Because these devices scroll very fast, the
 * virtual "clicks" have a higher associated scroll value then physical
 * "clicks". Physical "clicks" must also be handled here to reset the scroll
 * value to avoid handling scrolling twice (Remember: Wayland makes no
 * guarantees regarding the order in which axis and axis_discrete events are
 * received).
 */
static const struct wl_pointer_listener pointer_listener = {
	.axis_discrete = pointer_handle_axis_discrete,
	.axis          = pointer_handle_axis,
	.axis_source   = noop,
	.axis_stop     = noop,
	.button        = pointer_handle_button,
	.enter         = pointer_handle_enter,
	.frame         = pointer_handle_frame,
	.leave         = pointer_handle_leave,
	.motion        = pointer_handle_motion
};

static void seat_release_pointer (struct Lava_seat *seat)
{
	if ( seat->pointer.item_instance != NULL )
	{
		counter_safe_subtract(&(seat->pointer.item_instance->indicator), 1);
		counter_safe_subtract(&(seat->pointer.item_instance->active_indicator), (uint32_t)seat->pointer.click);
		seat->pointer.item_instance->dirty = true;
		bar_instance_schedule_frame(seat->pointer.instance);
		seat->pointer.item_instance = NULL;
		seat->pointer.click = 0;
	}
	seat_pointer_unset_cursor(seat);
	DESTROY_NULL(seat->pointer.wl_pointer, wl_pointer_release);
}

static void seat_bind_pointer (struct Lava_seat *seat)
{
	log_message(2, "[seat] Binding pointer.\n");
	seat->pointer.wl_pointer = wl_seat_get_pointer(seat->wl_seat);
	wl_pointer_add_listener(seat->pointer.wl_pointer, &pointer_listener, seat);
}

static void seat_init_pointer (struct Lava_seat *seat)
{
	seat->pointer.wl_pointer       = NULL;
	seat->pointer.x                = 0;
	seat->pointer.y                = 0;
	seat->pointer.instance         = NULL;
	seat->pointer.item_instance    = NULL;
	seat->pointer.discrete_steps   = 0;
	seat->pointer.last_update_time = 0;
	seat->pointer.value            = wl_fixed_from_int(0);
	seat->pointer.click            = 0;

	seat->pointer.cursor.type      = CURSOR_NONE;
	seat->pointer.cursor.surface   = NULL;
	seat->pointer.cursor.theme     = NULL;
	seat->pointer.cursor.image     = NULL;
	seat->pointer.cursor.wl_cursor = NULL;
}

/**********
 *        *
 *  Seat  *
 *        *
 **********/
static void seat_handle_capabilities (void *data, struct wl_seat *wl_seat,
		uint32_t capabilities)
{
	struct Lava_seat *seat = (struct Lava_seat *)data;

	log_message(1, "[seat] Handling seat capabilities.\n");

	if ( capabilities & WL_SEAT_CAPABILITY_KEYBOARD && context.need_keyboard )
		seat_bind_keyboard(seat);
	else
		seat_release_keyboard(seat);

	if ( capabilities & WL_SEAT_CAPABILITY_POINTER && context.need_pointer )
		seat_bind_pointer(seat);
	else
		seat_release_pointer(seat);

	if ( capabilities & WL_SEAT_CAPABILITY_TOUCH && context.need_touch )
		seat_bind_touch(seat);
	else
		seat_release_touch(seat);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name         = noop
};

bool create_seat (struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	log_message(1, "[seat] Adding seat.\n");

	struct wl_seat *wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);

	TRY_NEW(struct Lava_seat, seat, false);

	wl_seat_add_listener(wl_seat, &seat_listener, seat);

	seat->wl_seat = wl_seat;
	seat->global_name = name;

	seat_init_touch(seat);
	seat_init_keyboard(seat);
	seat_init_pointer(seat);

	wl_list_insert(&context.seats, &seat->link);

	return true;
}

struct Lava_seat *get_seat_from_global_name (uint32_t name)
{
	struct Lava_seat *seat, *temp;
	wl_list_for_each_safe(seat, temp, &context.seats, link)
		if ( seat->global_name == name )
			return seat;
	return NULL;
}

void destroy_seat (struct Lava_seat *seat)
{
	log_message(1, "[seat] Destroying seat.\n");
	seat_release_keyboard(seat);
	seat_release_touch(seat);
	seat_release_pointer(seat);
	wl_seat_release(seat->wl_seat);
	wl_list_remove(&seat->link);
	free(seat);
}

