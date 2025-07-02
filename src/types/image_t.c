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

#include<stdio.h>
#include<stdlib.h>
#include<stdbool.h>
#include<stdint.h>
#include<unistd.h>
#include<string.h>
#include<errno.h>
#include<cairo/cairo.h>

#if SVG_SUPPORT
#include<librsvg-2.0/librsvg/rsvg.h>
#endif

#include"util.h"
#include"lavalauncher.h"
#include"types/image_t.h"

/* Returns: -1 On error
 *           0 If the file is not a PNG file
 *           1 If the file is a PNG file
 */
static int is_png_file (const char *path)
{
	const size_t buffer_size = 8;

	FILE *file;
	if ( NULL == (file = fopen(path, "r")) )
	{
		log_message(0, "ERROR: Can not open file: %s\n"
				"ERROR: fopen: %s\n", path, strerror(errno));
		return -1;
	}

	unsigned char buffer[buffer_size];
	size_t ret = fread(buffer, sizeof(unsigned char), buffer_size, file);
	fclose(file);

	if ( ret == 0 )
	{
		log_message(0, "ERROR: fread() failed when trying to fetch file magic.\n");
		return -1;
	}

	unsigned char png_magic[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	for (int i = 0; i < 8; i++) if ( buffer[i] != png_magic[i] )
		return 0;

	return 1;
}

static bool load_image (image_t *image, const char *path)
{
	if (access(path, F_OK))
	{
		log_message(0, "ERROR: File does not exist: %s\n", path);
		return false;
	}
	if (access(path, R_OK))
	{
		log_message(0, "ERROR: File can not be read: %s\n"
				"INFO: Check the files permissions, owner and group.\n",
				path);
		return false;
	}
	if ( errno != 0 )
	{
		log_message(0, "ERROR: access: %s\n", strerror(errno));
		return false;
	}

	/* PNG */
	int ret;
	ret = is_png_file(path);
	if ( ret == 1 )
	{
		if ( NULL == (image->cairo_surface = cairo_image_surface_create_from_png(path)) )
		{
			log_message(0, "ERROR: Failed loading image: %s\n"
					"ERROR: cairo_image_surface_create_from_png: %s\n",
					path, strerror(errno));
			return false;
		}
		else
			return true;
	}
	else if ( ret == -1 )
		return false;

#if SVG_SUPPORT
	/* SVG */
	GError *gerror = NULL;
	if ( NULL != (image->rsvg_handle = rsvg_handle_new_from_file(path, &gerror)) )
		return true;
	else if ( gerror->domain != 123 )
	{
		/* The domain 123 is an XML parse error. Receiving it means that
		 * the file is likely not an SVG image, a case which must be
		 * handled differently than other errors.
		 */
		log_message(0, "ERROR: Failed to load image: %s\n"
				"ERROR: rsvg_handle_new_from_file: %d: %s\n",
				path, gerror->domain, gerror->message);
		return false;
	}
#endif

	log_message(0, "ERROR: Unsupported file type: %s\n"
#if SVG_SUPPORT
			"INFO: LavaLauncher supports PNG and SVG images.\n",
#else
			"INFO: LavaLauncher supports PNG images.\n"
			"INFO: LavaLauncher has been compiled without SVG support.\n",
#endif
			path);

	return false;
}

image_t *image_t_create_from_file (const char *path)
{
	TRY_NEW(image_t, image, NULL);

	image->cairo_surface = NULL;
	image->references    = 1;
#if SVG_SUPPORT
	image->rsvg_handle   = NULL;
#endif

	if (load_image(image, path))
		return image;
	
	free(image);
	return NULL;
}

image_t *image_t_reference (image_t *image)
{
	image->references++;
	return image;
}

void image_t_destroy (image_t *image)
{
	if ( --image->references > 0 )
		return;

	if ( image->cairo_surface != NULL )
		cairo_surface_destroy(image->cairo_surface);

#if SVG_SUPPORT
	if ( image->rsvg_handle != NULL )
		g_object_unref(image->rsvg_handle);
#endif

	free(image);
}

void image_t_draw_to_cairo (cairo_t *cairo, image_t *image,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t scale)
{
	x *= scale, y *= scale, width *= scale, height *= scale;
	cairo_save(cairo);
	cairo_translate(cairo, x, y);

	if ( image->cairo_surface != NULL )
	{
		int sw = cairo_image_surface_get_width(image->cairo_surface);
		int sh = cairo_image_surface_get_height(image->cairo_surface);

		cairo_scale(cairo, (float)width / (float)sw, (float)height / (float)sh);
		cairo_set_source_surface(cairo, image->cairo_surface, 0, 0);
		cairo_paint(cairo);
	}
#if SVG_SUPPORT
	else if ( image->rsvg_handle != NULL )
	{
		// TODO maybe set DPI?
		gboolean has_viewbox;
		RsvgLength rsvg_width, rsvg_height;
		RsvgRectangle viewbox = {.x=0, .y=0, .width=48, .height=48};	/* Sensible defaults in case viewBox is missing from file. */

		rsvg_handle_get_intrinsic_dimensions(image->rsvg_handle,
				NULL, &rsvg_width, NULL, &rsvg_height,
				&has_viewbox, &viewbox);
		const char *filename = g_filename_from_uri(rsvg_handle_get_base_uri(image->rsvg_handle), NULL, NULL);
		if ( ! has_viewbox )
		{
			log_message(1, "[bar] Constructing viewBox for SVG image %s.\n", filename);
			if ( rsvg_width.length == 0 )
				log_message(0, "INFO: SVG image %s has a width of zero, using default.\n", filename);
			else if (rsvg_width.unit == RSVG_UNIT_PX)
				viewbox.width = rsvg_width.length;
			else
			{
				gdouble rsvg_width_px;
				if (rsvg_handle_get_intrinsic_size_in_pixels(image->rsvg_handle, &rsvg_width_px, NULL))
					viewbox.width = rsvg_width_px;
			}
			if ( rsvg_height.length == 0 )
				log_message(0, "INFO: SVG image %s has a height of zero, using default.\n", filename);
			else if (rsvg_height.unit == RSVG_UNIT_PX)
				viewbox.height = rsvg_height.length;
			else
			{
				gdouble rsvg_height_px;
				if (rsvg_handle_get_intrinsic_size_in_pixels(image->rsvg_handle, NULL, &rsvg_height_px))
					viewbox.height = rsvg_height_px;
			}
			log_message(1, "[bar] Constructed viewBox of SVG image %s: width=%.0f height=%.0f.\n", filename, viewbox.width, viewbox.height);
		}
		if ( viewbox.width == 0 || viewbox.height == 0 )
			log_message(0, "ERROR: Viewbox of SVG image %s has a width/height of zero.\n", filename);
		cairo_scale(cairo, (float)width / viewbox.width,
				(float)width / viewbox.height);
		GError *gerror = NULL;
		rsvg_handle_render_document(image->rsvg_handle, cairo,
				&viewbox, &gerror);
		// TODO check value of gerror
	}
#endif

	cairo_restore(cairo);
}

