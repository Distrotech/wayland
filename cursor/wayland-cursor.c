/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "xcursor.h"
#include "wayland-cursor.h"
#include "wayland-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "os-compatibility.h"

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

struct shm_pool {
	struct wl_shm_pool *pool;
	int fd;
	unsigned int size;
	unsigned int used;
	char *data;
};

static struct shm_pool *
shm_pool_create(struct wl_shm *shm, int size)
{
	struct shm_pool *pool;

	pool = malloc(sizeof *pool);
	if (!pool)
		return NULL;

	pool->fd = os_create_anonymous_file(size);
	if (pool->fd < 0)
		goto err_free;

	pool->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  pool->fd, 0);

	if (pool->data == MAP_FAILED)
		goto err_close;

	pool->pool = wl_shm_create_pool(shm, pool->fd, size);
	pool->size = size;
	pool->used = 0;

	return pool;

err_close:
	close(pool->fd);
err_free:
	free(pool);
	return NULL;
}

static int
shm_pool_resize(struct shm_pool *pool, int size)
{
	if (ftruncate(pool->fd, size) < 0)
		return 0;

	wl_shm_pool_resize(pool->pool, size);

	munmap(pool->data, pool->size);

	pool->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  pool->fd, 0);
	pool->size = size;

	return 1;
}

static int
shm_pool_allocate(struct shm_pool *pool, int size)
{
	int offset;

	if (pool->used + size > pool->size)
		if (!shm_pool_resize(pool, 2 * pool->size + size))
			return -1;

	offset = pool->used;
	pool->used += size;

	return offset;
}

static void
shm_pool_destroy(struct shm_pool *pool)
{
	munmap(pool->data, pool->size);
	wl_shm_pool_destroy(pool->pool);
	close(pool->fd);
	free(pool);
}


struct wl_cursor_theme {
	unsigned int cursor_count;
	struct wl_cursor **cursors;
	struct wl_shm *shm;
	struct shm_pool *pool;
	char *name;
	int size;
};

struct cursor_image {
	struct wl_cursor_image image;
	struct wl_cursor_theme *theme;
	struct wl_buffer *buffer;
	int offset; /* data offset of this image in the shm pool */
};

struct cursor {
	struct wl_cursor cursor;
	uint32_t total_delay; /* length of the animation in ms */
};

/** Get an shm buffer for a cursor image
 *
 * \param image The cursor image
 * \return An shm buffer for the cursor image. The user should not destroy
 * the returned buffer.
 */
WL_EXPORT struct wl_buffer *
wl_cursor_image_get_buffer(struct wl_cursor_image *_img)
{
	struct cursor_image *image = (struct cursor_image *) _img;
	struct wl_cursor_theme *theme = image->theme;

	if (!image->buffer) {
		image->buffer =
			wl_shm_pool_create_buffer(theme->pool->pool,
						  image->offset,
						  _img->width, _img->height,
						  _img->width * 4,
						  WL_SHM_FORMAT_ARGB8888);
	};

	return image->buffer;
}

static void
wl_cursor_image_destroy(struct wl_cursor_image *_img)
{
	struct cursor_image *image = (struct cursor_image *) _img;

	if (image->buffer)
		wl_buffer_destroy(image->buffer);

	free(image);
}

static void
wl_cursor_destroy(struct wl_cursor *cursor)
{
	unsigned int i;

	for (i = 0; i < cursor->image_count; i++)
		wl_cursor_image_destroy(cursor->images[i]);

	free(cursor->name);
	free(cursor);
}

#include "cursor-data.h"

static struct wl_cursor *
wl_cursor_create_from_data(struct cursor_metadata *metadata,
			   struct wl_cursor_theme *theme)
{
	struct cursor *cursor;
	struct cursor_image *image;
	int size;

	cursor = malloc(sizeof *cursor);
	if (!cursor)
		return NULL;

	cursor->cursor.image_count = 1;
	cursor->cursor.images = malloc(sizeof *cursor->cursor.images);
	if (!cursor->cursor.images) {
		free(cursor);
		return NULL;
	}

	cursor->cursor.name = strdup(metadata->name);
	cursor->total_delay = 0;

	image = malloc(sizeof *image);
	if (!image) {
		free(cursor->cursor.name);
		free(cursor->cursor.images);
		free(cursor);
		return NULL;
	}

	cursor->cursor.images[0] = (struct wl_cursor_image *) image;
	image->theme = theme;
	image->buffer = NULL;
	image->image.width = metadata->width;
	image->image.height = metadata->height;
	image->image.hotspot_x = metadata->hotspot_x;
	image->image.hotspot_y = metadata->hotspot_y;
	image->image.delay = 0;

	size = metadata->width * metadata->height * sizeof(uint32_t);
	image->offset = shm_pool_allocate(theme->pool, size);
	memcpy(theme->pool->data + image->offset,
	       cursor_data + metadata->offset, size);

	return &cursor->cursor;
}

static void
load_default_theme(struct wl_cursor_theme *theme)
{
	uint32_t i;

	free(theme->name);
	theme->name = strdup("default");

	theme->cursor_count = ARRAY_LENGTH(cursor_metadata);;
	theme->cursors = malloc(theme->cursor_count * sizeof(*theme->cursors));

	for (i = 0; i < theme->cursor_count; ++i)
		theme->cursors[i] =
			wl_cursor_create_from_data(&cursor_metadata[i], theme);
}

static struct wl_cursor *
wl_cursor_create_from_xcursor_images(XcursorImages *images,
				     struct wl_cursor_theme *theme)
{
	struct cursor *cursor;
	struct cursor_image *image;
	int i, size;

	cursor = malloc(sizeof *cursor);
	if (!cursor)
		return NULL;

	cursor->cursor.image_count = images->nimage;
	cursor->cursor.images =
		malloc(images->nimage * sizeof cursor->cursor.images[0]);
	if (!cursor->cursor.images) {
		free(cursor);
		return NULL;
	}

	cursor->cursor.name = strdup(images->name);
	cursor->total_delay = 0;

	for (i = 0; i < images->nimage; i++) {
		image = malloc(sizeof *image);
		cursor->cursor.images[i] = (struct wl_cursor_image *) image;

		image->theme = theme;
		image->buffer = NULL;

		image->image.width = images->images[i]->width;
		image->image.height = images->images[i]->height;
		image->image.hotspot_x = images->images[i]->xhot;
		image->image.hotspot_y = images->images[i]->yhot;
		image->image.delay = images->images[i]->delay;
		cursor->total_delay += image->image.delay;

		/* copy pixels to shm pool */
		size = image->image.width * image->image.height * 4;
		image->offset = shm_pool_allocate(theme->pool, size);
		memcpy(theme->pool->data + image->offset,
		       images->images[i]->pixels, size);
	}

	return &cursor->cursor;
}

static void
load_callback(XcursorImages *images, void *data)
{
	struct wl_cursor_theme *theme = data;
	struct wl_cursor *cursor;

	if (wl_cursor_theme_get_cursor(theme, images->name)) {
		XcursorImagesDestroy(images);
		return;
	}

	cursor = wl_cursor_create_from_xcursor_images(images, theme);

	if (cursor) {
		theme->cursor_count++;
		theme->cursors =
			realloc(theme->cursors,
				theme->cursor_count * sizeof theme->cursors[0]);

		theme->cursors[theme->cursor_count - 1] = cursor;
	}

	XcursorImagesDestroy(images);
}

/** Load a cursor theme to memory shared with the compositor
 *
 * \param name The name of the cursor theme to load. If %NULL, the default
 * theme will be loaded.
 * \param size Desired size of the cursor images.
 * \param shm The compositor's shm interface.
 *
 * \return An object representing the theme that should be destroyed with
 * wl_cursor_theme_destroy() or %NULL on error. If no theme with the given
 * name exists, a default theme will be loaded.
 */
WL_EXPORT struct wl_cursor_theme *
wl_cursor_theme_load(const char *name, int size, struct wl_shm *shm)
{
	struct wl_cursor_theme *theme;

	theme = malloc(sizeof *theme);
	if (!theme)
		return NULL;

	if (!name)
		name = "default";

	theme->name = strdup(name);
	theme->size = size;
	theme->cursor_count = 0;
	theme->cursors = NULL;

	theme->pool =
		shm_pool_create(shm, size * size * 4);
	if (!theme->pool) {
		free(theme->name);
		free(theme);
		return NULL;
	}

	xcursor_load_theme(name, size, load_callback, theme);

	if (theme->cursor_count == 0)
		load_default_theme(theme);

	return theme;
}

/** Destroys a cursor theme object
 *
 * \param theme The cursor theme to be destroyed
 */
WL_EXPORT void
wl_cursor_theme_destroy(struct wl_cursor_theme *theme)
{
	unsigned int i;

	for (i = 0; i < theme->cursor_count; i++)
		wl_cursor_destroy(theme->cursors[i]);

	shm_pool_destroy(theme->pool);

	free(theme->cursors);
	free(theme);
}

/** Get the cursor for a given name from a cursor theme
 *
 * \param theme The cursor theme
 * \param name Name of the desired cursor
 * \return The theme's cursor of the given name or %NULL if there is no
 * such cursor
 */
WL_EXPORT struct wl_cursor *
wl_cursor_theme_get_cursor(struct wl_cursor_theme *theme,
			   const char *name)
{
	unsigned int i;

	for (i = 0; i < theme->cursor_count; i++) {
		if (strcmp(name, theme->cursors[i]->name) == 0)
			return theme->cursors[i];
	}

	return NULL;
}

/** Find the frame for a given elapsed time in a cursor animation
 *
 * \param cursor The cursor
 * \param time Elapsed time since the beginning of the animation
 *
 * \return The index of the image that should be displayed for the
 * given time in the cursor animation.
 */
WL_EXPORT int
wl_cursor_frame(struct wl_cursor *_cursor, uint32_t time)
{
	struct cursor *cursor = (struct cursor *) _cursor;
	uint32_t t;
	int i;

	if (cursor->cursor.image_count == 1)
		return 0;

	i = 0;
	t = time % cursor->total_delay;

	while (t - cursor->cursor.images[i]->delay < t)
		t -= cursor->cursor.images[i++]->delay;

	return i;
}
