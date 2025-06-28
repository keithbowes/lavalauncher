/*
 * LavaLauncher - A simple launcher panel for Wayland
 *
 * Copyright (C) 2020 Leon Henrik Plickat
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

#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<stdbool.h>
#include<unistd.h>
#include<string.h>
#include<errno.h>
#include<sys/wait.h>
#include<linux/input-event-codes.h>

#include"lavalauncher.h"
#include"item.h"
#include"seat.h"
#include"util.h"
#include"bar.h"
#include"output.h"
#include"foreign-toplevel-management.h"
#include"types/image_t.h"

/* Helper macro to reduce error handling boiler plate code. */
#define TRY(A) \
	{ \
		if (A)\
			return true; \
		goto error; \
	}

/*******************
 *                 *
 *  Item commands  *
 *                 *
 *******************/
/* We need to fork two times for UNIXy resons. */
static void item_command_exec_second_fork (struct Lava_bar_instance *instance, const char *cmd)
{
	errno = 0;
	int ret = fork();
	if ( ret == 0 )
	{
		/* Prepare environment variables. */
		setenvf("LAVALAUNCHER_OUTPUT_NAME",  "%s", instance->output->name);
		setenvf("LAVALAUNCHER_OUTPUT_SCALE", "%d", instance->output->scale);

		/* execl() only returns on error; On success it replaces this process. */
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		log_message(0, "ERROR: execl: %s\n", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	else if ( ret < 0 ) /* Yes, fork can fail. */
	{
		log_message(0, "ERROR: fork: %s\n", strerror(errno));
		_exit(EXIT_FAILURE);
	}
}

/* We need to fork two times for UNIXy resons. */
static void item_command_exec_first_fork (struct Lava_bar_instance *instance, const char *cmd)
{
	errno = 0;
	int ret = fork();
	if ( ret == 0 )
	{
		setsid();

		/* Restore signals. */
		sigset_t mask;
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, NULL);

		item_command_exec_second_fork(instance, cmd);
		_exit(EXIT_SUCCESS);
	}
	else if ( ret < 0 ) /* Yes, fork can fail. */
		log_message(0, "ERROR: fork: %s\n", strerror(errno));
	else
		waitpid(ret, NULL, 0);
}

static void execute_item_command (struct Lava_item_command *cmd, struct Lava_bar_instance *instance)
{
	const char *command = cmd->command;
	if ( command == NULL )
		return;
	log_message(1, "[item] Executing command: %s\n", command);
	item_command_exec_first_fork(instance, command);
}

/** Tries to find a matching command and returns it, otherwise returns NULL. */
static struct Lava_item_command *find_item_command (struct Lava_item *item,
		enum Interaction_type type, uint32_t modifiers, uint32_t special,
		bool allow_universal)
{
	struct Lava_item_command *cmd;
	wl_list_for_each(cmd, &item->commands, link)
		if ( (cmd->type == type && cmd->modifiers == modifiers && cmd->special == special)
				|| (allow_universal && cmd->type == INTERACTION_UNIVERSAL && type != INTERACTION_MOUSE_SCROLL) )
			return cmd;
	return NULL;
}

/** Tries to find a matching command and overwrite it, otherwise create a new one. */
static bool item_add_command (struct Lava_item *item, const char *command,
		enum Interaction_type type, uint32_t modifiers, uint32_t special)
{
	struct Lava_item_command *cmd = find_item_command(item, type, modifiers,
			special, false);

	if ( cmd == NULL )
	{
		cmd = calloc(1, sizeof(struct Lava_item_command));
		if ( cmd == NULL )
		{
			log_message(0, "ERROR: Can not allocte.\n");
			return false;
		}

		cmd->type      = type;
		cmd->modifiers = modifiers;
		cmd->special   = special;
		cmd->command   = NULL;

		wl_list_insert(&item->commands, &cmd->link);
	}

	/* Parse meta action, if any. */
	if ( *command == '@' )
	{
		struct
		{
			const char *name;
			enum Meta_action action;
		} actions[] = {
			// TODO: @maximize-toplevel @unmaximize-toplevel @toggle-maximize-toplevel,
			//       -> same for fullscreen and minimize
			//       @close-all try to close /all/ toplevels with matching app_id
			{ .name = "@toplevel-activate", .action = META_ACTION_TOPLEVEL_ACTIVATE },
			{ .name = "@toplevel-close",    .action = META_ACTION_TOPLEVEL_CLOSE    },
			{ .name = "@reload",            .action = META_ACTION_RELOAD            },
			{ .name = "@exit",              .action = META_ACTION_EXIT              },
		};

		FOR_ARRAY(actions, i) if (string_starts_with(command, actions[i].name))
		{
			cmd->action = actions[i].action;

			/* Check if command only contains the meta action. */
			if ( strlen(command) == strlen(actions[i].name) )
			{
				free_if_set(cmd->command);
				cmd->command = NULL;
			}
			else
				set_string(&cmd->command, (char *)command + strlen(actions[i].name));

			return true;
		}

		/* If we could not match any meta action despite the command
		 * string starting with @, it may be part of the command itself,
		 * so just fall through here.
		 */
	}

	cmd->action = META_ACTION_NONE;
	set_string(&cmd->command, (char *)command);

	return true;
}

static void destroy_item_command (struct Lava_item_command *cmd)
{
	wl_list_remove(&cmd->link);
	free_if_set(cmd->command);
	free(cmd);
}

static void destroy_all_item_commands (struct Lava_item *item)
{
	struct Lava_item_command *cmd, *tmp;
	wl_list_for_each_safe(cmd, tmp, &item->commands, link)
		destroy_item_command(cmd);
}

/**************************
 *                        *
 *  Button configuration  *
 *                        *
 **************************/
static bool button_set_image_path (struct Lava_item *button, const char *path)
{
	DESTROY(button->img, image_t_destroy);
	if ( NULL == (button->img = image_t_create_from_file(path)) )
		return false;
	return true;
}

static bool button_set_toplevel_app_id (struct Lava_item *button, const char *app_id)
{
	free_if_set(button->associated_app_id);
	if ( strcmp(app_id, "none") == 0 )
		return true;
	button->associated_app_id = strdup(app_id);
	context.need_foreign_toplevel = true;
	return true;
}

static bool parse_bind_token_buffer (char *buffer, int *index,enum Interaction_type *type,
		uint32_t *modifiers, uint32_t *special, bool *type_defined)
{
	buffer[*index] = '\0';

	const struct
	{
		char *name;
		enum Interaction_type type;
		bool modifier;
		uint32_t value;
	} tokens[] = {
		/* Mouse buttons (basically everything from linux/input-event-codes.h that a mouse-like device can emit) */
		{ .name = "mouse-mouse",    .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_MOUSE   },
		{ .name = "mouse-left",     .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_LEFT    },
		{ .name = "mouse-right",    .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_RIGHT   },
		{ .name = "mouse-middle",   .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_MIDDLE  },
		{ .name = "mouse-side",     .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_SIDE    },
		{ .name = "mouse-extra",    .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_EXTRA   },
		{ .name = "mouse-forward",  .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_FORWARD },
		{ .name = "mouse-backward", .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_BACK    },
		{ .name = "mouse-task",     .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_TASK    },
		{ .name = "mouse-misc",     .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_MISC    },
		{ .name = "mouse-1",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_1       },
		{ .name = "mouse-2",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_2       },
		{ .name = "mouse-3",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_3       },
		{ .name = "mouse-4",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_4       },
		{ .name = "mouse-5",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_5       },
		{ .name = "mouse-6",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_6       },
		{ .name = "mouse-7",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_7       },
		{ .name = "mouse-8",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_8       },
		{ .name = "mouse-9",        .type = INTERACTION_MOUSE_BUTTON, .modifier = false, .value = BTN_9       },

		/* Scroll */
		{ .name = "scroll-up",   .type = INTERACTION_MOUSE_SCROLL, .modifier = false, .value = 1 },
		{ .name = "scroll-down", .type = INTERACTION_MOUSE_SCROLL, .modifier = false, .value = 0 },

		/* Touch */
		{ .name = "touch", .type = INTERACTION_TOUCH, .modifier = false, .value = 0 },

		/* Modifiers */
		{ .name = "alt",      .type = INTERACTION_UNIVERSAL, .modifier = true, .value = ALT     },
		{ .name = "capslock", .type = INTERACTION_UNIVERSAL, .modifier = true, .value = CAPS    },
		{ .name = "control",  .type = INTERACTION_UNIVERSAL, .modifier = true, .value = CONTROL },
		{ .name = "logo",     .type = INTERACTION_UNIVERSAL, .modifier = true, .value = LOGO    },
		{ .name = "numlock",  .type = INTERACTION_UNIVERSAL, .modifier = true, .value = NUM     },
		{ .name = "shift",    .type = INTERACTION_UNIVERSAL, .modifier = true, .value = SHIFT   }
	};

	FOR_ARRAY(tokens, i) if (! strcmp(tokens[i].name, buffer))
	{
		if (tokens[i].modifier)
		{
			*modifiers |= tokens[i].value;
			context.need_keyboard = true;
		}
		else
		{
			if (*type_defined)
			{
				log_message(0, "ERROR: A command can only have a single interaction type.\n");
				return false;
			}
			*type_defined = true;

			*type = tokens[i].type;
			*special = tokens[i].value;
			switch (tokens[i].type)
			{
				case INTERACTION_MOUSE_BUTTON:
				case INTERACTION_MOUSE_SCROLL:
					context.need_pointer = true;
					break;

				case INTERACTION_TOUCH:
					context.need_touch = true;
					break;

				default:
					break;
			}
		}

		*index = 0;
		return true;
	}

	log_message(0, "ERROR: Unrecognized interaction type / modifier \"%s\".\n", buffer);
	return false;
}

static bool parse_token_buffer_add_char (char *buffer, int size, int *index, char ch)
{
	if ( *index > size - 2 )
		return false;
	buffer[*index] = ch;
	(*index)++;
	return true;
}

static bool button_item_command_from_string (struct Lava_item *button,
		const char *_bind, const char *command)
{
	/* We can safely skip what we know is already there. */
	char *bind = (char *)_bind + strlen("command");

	const int buffer_size = 20;
	char buffer[buffer_size];
	int buffer_index = 0;

	bool type_defined = false;
	enum Interaction_type type = INTERACTION_UNIVERSAL;
	uint32_t modifiers = 0, special = 0;
	bool start = false, stop = false;
	char *ch = (char *)bind;
	for (;;)
	{
		if ( *ch == '\0' )
		{
			if ( !start || !stop )
				goto error;

			/* If the type is still universal, no bind has been specified. */
			if ( type == INTERACTION_UNIVERSAL )
			{
				log_message(0, "ERROR: No interaction type defined.\n", bind);
				goto error;
			}

			return item_add_command(button, (char *)command, type,
					modifiers, special);
		}
		else if ( *ch == '[' )
		{
			if ( start || stop )
				goto error;
			start = true;
		}
		else if ( *ch == ']' )
		{
			if ( !start || stop )
				goto error;
			if (! parse_bind_token_buffer(buffer, &buffer_index,
						&type, &modifiers, &special, &type_defined))
				goto error;
			stop = true;
		}
		else if ( *ch == '+' )
		{
			if (! parse_bind_token_buffer(buffer, &buffer_index,
						&type, &modifiers, &special, &type_defined))
				goto error;
		}
		else
		{
			if ( !start || stop )
				goto error;
			if (! parse_token_buffer_add_char(buffer, buffer_size, &buffer_index, *ch))
				goto error;
		}

		ch++;
	}

error:
	log_message(0, "ERROR: Unable to parse command bind string: %s\n", bind);
	return false;
}

static bool button_item_universal_command (struct Lava_item *button, const char *command)
{
	/* Interaction type is universal, meaning the button can be activated
	 * by both the pointer and touch.
	 */
	context.need_pointer = true;
	context.need_touch = true;
	return item_add_command(button, command, INTERACTION_UNIVERSAL, 0, 0);
}

static bool button_set_variable (struct Lava_item *button, const char *variable,
		const char *value, uint32_t line)
{
	if ( strcmp("image-path", variable) == 0 )
		TRY(button_set_image_path(button, value))
	else if ( strcmp("toplevel-app-id", variable) == 0 )
		TRY(button_set_toplevel_app_id(button, value))
	else if ( strcmp("command", variable) == 0 ) /* Generic/universal command */
		TRY(button_item_universal_command(button, value))
	else if (string_starts_with(variable, "command"))  /* Command with special bind */
		TRY(button_item_command_from_string(button, variable, value))

	log_message(0, "ERROR: Unrecognized button setting \"%s\".\n", variable);
error:
	log_message(0, "INFO: The error is on line %d in \"%s\".\n",
			line, context.config_path);
	return false;
}

/**************************
 *                        *
 *  Spacer configuration  *
 *                        *
 **************************/
static bool spacer_set_length (struct Lava_item *spacer, const char *length)
{
	int len = atoi(length);
	if ( len <= 0 )
	{
		log_message(0, "ERROR: Spacer size must be greater than 0.\n");
		return false;
	}
	spacer->spacer_length = (uint32_t)len;
	return true;
}

static bool spacer_set_variable (struct Lava_item *spacer,
		const char *variable, const char *value, uint32_t line)
{
	if (! strcmp("length", variable))
		TRY(spacer_set_length(spacer, value))

	log_message(0, "ERROR: Unrecognized spacer setting \"%s\".\n", variable);
error:
	log_message(0, "INFO: The error is on line %d in \"%s\".\n",
			line, context.config_path);
	return false;
}

bool item_set_variable (struct Lava_item *item, const char *variable,
		const char *value, uint32_t line)
{
	switch (item->type)
	{
		case TYPE_BUTTON:
			return button_set_variable(item, variable, value, line);

		case TYPE_SPACER:
			return spacer_set_variable(item, variable, value, line);

		default:
			return false;
	}
}

/**********
 *        *
 *  Item  *
 *        *
 **********/
void item_interaction (struct Lava_item *item, struct Lava_bar_instance *instance,
		struct Lava_seat *seat, enum Interaction_type type,
		uint32_t modifiers, uint32_t special)
{
	if ( item->type != TYPE_BUTTON )
		return;

	log_message(1, "[item] Interaction: type=%d mod=%d spec=%d\n",
			type, modifiers, special);

	struct Lava_item_command *cmd = find_item_command(item, type, modifiers, special, true);
	if ( cmd == NULL )
		return;

	struct Lava_toplevel *toplevel;
	switch (cmd->action)
	{
		case META_ACTION_NONE:
			execute_item_command(cmd, instance);
			break;

		case META_ACTION_TOPLEVEL_ACTIVATE:
			toplevel = find_toplevel_with_app_id(item->associated_app_id);
			if ( toplevel == NULL )
				execute_item_command(cmd, instance);
			else
			{
				log_message(2, "[item] Activating toplevel: app-id=%s\n", item->associated_app_id);
				zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, seat->wl_seat);
			}
			break;

		case META_ACTION_TOPLEVEL_CLOSE:
			toplevel = find_toplevel_with_app_id(item->associated_app_id);
			if ( toplevel == NULL )
				execute_item_command(cmd, instance);
			else
			{
				log_message(2, "[item] Closing toplevel: app-id=%s\n", item->associated_app_id);
				zwlr_foreign_toplevel_handle_v1_close(toplevel->handle);
			}
			break;

		case META_ACTION_RELOAD:
			execute_item_command(cmd, instance);
			log_message(2, "[item] Triggering reload.\n");
			context.loop   = false;
			context.reload = true;
			break;

		case META_ACTION_EXIT:
			execute_item_command(cmd, instance);
			log_message(2, "[item] Triggering exit.\n");
			context.loop   = false;
			context.reload = false;
			break;
	}
}

bool create_item (enum Item_type type)
{
	log_message(2, "[item] Creating item.\n");

	TRY_NEW(struct Lava_item, item, false);

	context.last_item = item;

	item->img               = NULL;
	item->type              = type;
	item->associated_app_id = NULL;

	wl_list_init(&item->commands);
	wl_list_insert(&context.items, &item->link);

	return true;
}

static void destroy_item (struct Lava_item *item)
{
	wl_list_remove(&item->link);
	destroy_all_item_commands(item);
	DESTROY(item->img, image_t_destroy);
	free_if_set(item->associated_app_id);
	free(item);
}

void destroy_all_items (void)
{
	log_message(1, "[items] Destroying all items.\n");
	struct Lava_item *item, *temp;
	wl_list_for_each_safe(item, temp, &context.items, link)
		destroy_item(item);
}

/*******************
 *                 *
 *  Item instance  *
 *                 *
 *******************/
void item_instance_next_frame (struct Lava_item_instance *instance)
{
	if ( instance->item->type != TYPE_BUTTON )
	{
		instance->dirty = false;
		return;
	}

	// TODO store previous buffer, so that items do not need to be re-rendered
	//      when bar unhides.
	if (instance->bar_instance->hidden)
	{
		wl_surface_attach(instance->wl_surface, NULL, 0, 0);
		wl_surface_commit(instance->wl_surface);
		return;
	}

	log_message(2, "[item] Render item frame: global_name=%d\n",
			instance->bar_instance->output->global_name);

	struct Lava_bar_configuration *config = instance->bar_instance->config;
	const uint32_t scale = instance->bar_instance->output->scale;
	const uint32_t icon_padding = config->icon_padding;
	const uint32_t indicator_padding = config->indicator_padding;

	if (! next_buffer(&instance->current_buffer, context.shm, instance->buffers,
			instance->w * scale, instance->h * scale))
		return;

	cairo_t *cairo = instance->current_buffer->cairo;
	clear_cairo_buffer(cairo);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	/* Active indicator: Shown when mouse button is pressed down on button 
	 * or button is touched.
	 */
	if ( instance->active_indicator > 0 )
	{
		rounded_rectangle(cairo, indicator_padding, indicator_padding,
				instance->w - (2 * indicator_padding),
				instance->h - (2 * indicator_padding),
				&config->radii, scale);
		colour_t_set_cairo_source(cairo, &config->indicator_active_colour);
		cairo_fill(cairo);
	}
	/* Hover indicator: Shown when cursor hovers over button. */
	else if ( instance->indicator > 0 )
	{
		rounded_rectangle(cairo, indicator_padding, indicator_padding,
				instance->w - (2 * indicator_padding),
				instance->h - (2 * indicator_padding),
				&config->radii, scale);
		colour_t_set_cairo_source(cairo, &config->indicator_hover_colour);
		cairo_fill(cairo);
	}

	// TODO draw indicators for toplevel state
	// if ( instance->item_instances[i].toplevel_activated_indicator > 0 )
	// {
	// 	// TODO
	// }
	// if ( instance->item_instances[i].toplevel_exists_indicator > 0 )
	// {
	// 	// TODO
	// }

	/* Draw the icon. */
	if ( instance->item->img != NULL )
		image_t_draw_to_cairo(cairo, instance->item->img,
				icon_padding, icon_padding,
				instance->w - (2 * icon_padding),
				instance->h - (2 * icon_padding), scale);

	instance->dirty = false;
	wl_surface_damage_buffer(instance->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_set_buffer_scale(instance->wl_surface, (int32_t)scale);
	wl_surface_attach(instance->wl_surface, instance->current_buffer->buffer, 0, 0);

	wl_surface_commit(instance->wl_surface);
}

void configure_item_instance (struct Lava_item_instance *instance,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	instance->dirty = true;

	instance->x = x;
	instance->y = y;
	instance->w = w;
	instance->h = h;

	wl_subsurface_set_position(instance->wl_subsurface, (int32_t)x, (int32_t)y);
	wl_surface_commit(instance->wl_surface);
}

void init_item_instance (struct Lava_item_instance *instance,
		struct Lava_bar_instance *bar_instance, struct Lava_item *item)
{
	// TODO surface listener to know when cursor enters it
	// -> replaces cursor movement handling code!
	instance->item = item;
	instance->active = true;
	instance->bar_instance = bar_instance;
	instance->indicator = 0;
	instance->active_indicator = 0;
	instance->toplevel_exists_indicator = 0;
	instance->toplevel_activated_indicator = 0;
	instance->wl_surface = wl_compositor_create_surface(context.compositor);
	instance->wl_subsurface = wl_subcompositor_get_subsurface(context.subcompositor,
			instance->wl_surface, bar_instance->wl_surface);

	/* We update and render subsurfaces synchronous to the parent surface.
	 * Also see update_bar_instance() and create_bar_instance().
	 */
	wl_subsurface_set_sync(instance->wl_subsurface);

	// TODO remove when using surface listeners
	struct wl_region *region = wl_compositor_create_region(context.compositor);
	wl_surface_set_input_region(instance->wl_surface, region);
	wl_region_destroy(region);

	wl_surface_commit(instance->wl_surface);
}

void finish_item_instance (struct Lava_item_instance *instance)
{
	DESTROY(instance->wl_subsurface, wl_subsurface_destroy);
	DESTROY(instance->wl_surface, wl_surface_destroy);
	finish_buffer(&instance->buffers[0]);
	finish_buffer(&instance->buffers[1]);
}

