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

#include<stdbool.h>
#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<ctype.h>
#include<cairo/cairo.h>

#include"lavalauncher.h"
#include"types/box_t.h"

void log_message (int level, const char *fmt, ...)
{
	if ( level > context.verbosity )
		return;

	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}


void free_if_set (void *ptr)
{
	if ( ptr != NULL )
		free(ptr);
}

void set_string (char **ptr, char *arg)
{
	free_if_set(*ptr);
	*ptr = (char *)strdup(arg);
}

char *get_formatted_buffer (const char *fmt, ...)
{
	/* First we determine the lenght of the formatted text. */
	va_list args;
	va_start(args, fmt);
	unsigned long length = (unsigned long)vsnprintf(NULL, 0, fmt, args) + 1; /* +1 for NULL terminator. */
	va_end(args);

	/* Then we allocate a buffer to hold the formatted text. */
	char *buffer = calloc(1, length);
	if ( buffer == NULL )
	{
		log_message(0, "ERROR: Could not allocate text buffer.\n");
		return NULL;
	}

	/* Then we put the formatted text into the buffer. */
	va_start(args, fmt);
	vsnprintf(buffer, length, fmt, args);
	va_end(args);

	return buffer;
}

const char *str_orelse (const char *str, const char *orelse)
{
	if ( str != NULL )
		return str;
	return  orelse;
}

void setenvf (const char *name, const char *fmt, ...)
{
	char buffer[64];
	memset(buffer, '\0', sizeof(buffer));

	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
	va_end(args);

	setenv(name, buffer, true);
}

bool string_starts_with(const char *str, const char *prefix)
{
	const size_t len_str = strlen(str);
	const size_t len_prefix = strlen(prefix);
	const size_t min_len = len_str > len_prefix ? len_prefix : len_str;
	return strncmp(prefix, str, min_len) == 0;
}

bool is_boolean_true (const char *str)
{
	return ( ! strcmp(str, "true") || ! strcmp(str, "yes") || ! strcmp(str, "on") || ! strcmp(str, "1") );
}

bool is_boolean_false (const char *str)
{
	return ( ! strcmp(str, "false") || ! strcmp(str, "no") || ! strcmp(str, "off") || ! strcmp(str, "0") );
}

bool set_boolean (bool *b, const char *value)
{
	if (is_boolean_true(value))
		*b = true;
	else if (is_boolean_false(value))
		*b = false;
	else
	{
		log_message(0, "ERROR: Not a boolean: %s\n", value);
		return false;
	}
	return true;
}

/* Return amount of whitespace separated tokens.
 * Example: "hello"  => 1
 *          "hell o" => 2
 */
uint32_t count_tokens (const char *arg)
{
	uint32_t args = 0;
	bool on_arg = false;
	for (const char *i = arg; *i != '\0'; i++)
	{
		if (isspace(*i))
		{
			if (on_arg)
				on_arg = false;
		}
		else if (! on_arg)
		{
			on_arg = true;
			args++;
		}
	}
	return args;
}

void counter_safe_subtract (uint32_t *counter, uint32_t subtract)
{
	if ( subtract > *counter )
		*counter = 0;
	else
		*counter -= subtract;
}

void clear_cairo_buffer (cairo_t *cairo)
{
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
}

void rounded_rectangle (cairo_t *cairo, uint32_t x, uint32_t y,
		uint32_t w, uint32_t h, uradii_t *_radii, uint32_t scale)
{
	const double degrees = 3.1415927 / 180.0;
	x *= scale, y *= scale, w *= scale, h *= scale;
	uradii_t radii = uradii_t_scale(_radii, scale);
	cairo_new_sub_path(cairo);
	cairo_arc(cairo, x + w - radii.top_right,    y     + radii.top_right,    radii.top_right,   -90 * degrees,   0 * degrees);
	cairo_arc(cairo, x + w - radii.bottom_right, y + h - radii.bottom_right, radii.bottom_right,  0 * degrees,  90 * degrees);
	cairo_arc(cairo, x     + radii.bottom_left,  y + h - radii.bottom_left,  radii.bottom_left,  90 * degrees, 180 * degrees);
	cairo_arc(cairo, x     + radii.top_left,     y     + radii.top_left,     radii.top_left,    180 * degrees, 270 * degrees);
	cairo_close_path(cairo);
}

