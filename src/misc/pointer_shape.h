/*
 * kmscon - Mouse pointer shapes (OSC 22)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Mouse pointer shapes
 * Implements the OSC 22 pointer-shape protocol as specified by kitty (set,
 * push, pop and query, with comma-separated fallback names), accepting both
 * CSS names and the X11 cursor-font names xterm uses. Shape images are read
 * from Xcursor themes without depending on libXcursor.
 */

#ifndef KMSCON_POINTER_SHAPE_H
#define KMSCON_POINTER_SHAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shapes are identified by their index into the table of CSS names. */
#define POINTER_SHAPE_NONE (-1)
/* upper bound on pointer_shape_count(), for fixed-size per-shape arrays */
#define POINTER_SHAPE_MAX 64
#define POINTER_SHAPE_STACK_MAX 16

int pointer_shape_count(void);
/* CSS name of a shape index, or NULL if out of range */
const char *pointer_shape_name(int shape);
/* index of a CSS or X11 name, or POINTER_SHAPE_NONE if unknown */
int pointer_shape_lookup(const char *name, size_t len);
/* the CSS names used as defaults: "text" normally, "default" while grabbed */
int pointer_shape_text(void);
int pointer_shape_arrow(void);

struct pointer_shape_stack {
	int shapes[POINTER_SHAPE_STACK_MAX];
	unsigned int depth;
};

/* Whether a shape can be drawn. */
typedef bool (*pointer_shape_supported_cb)(int shape, void *data);

void pointer_shape_stack_reset(struct pointer_shape_stack *stack);
/* top of the stack, or POINTER_SHAPE_NONE if empty */
int pointer_shape_stack_top(const struct pointer_shape_stack *stack);
/* the shape to draw: the top of the stack, else text or arrow if grabbed */
int pointer_shape_effective(const struct pointer_shape_stack *stack, bool grabbed);

/*
 * Handle the payload of an OSC 22 sequence (the text after "22;").
 * Returns true if the stack changed. For a query, *reply is set to a
 * malloc'ed, NUL-terminated response to send back to the application,
 * which the caller must free. *reply is NULL otherwise.
 */
bool pointer_shape_osc(struct pointer_shape_stack *stack, const char *payload, bool grabbed,
		       pointer_shape_supported_cb supported, void *data, char **reply);

/* Xcursor images */

struct pointer_image {
	unsigned int width;
	unsigned int height;
	int hot_x;
	int hot_y;
	/* premultiplied ARGB, row-major, width * height */
	uint32_t *pixels;
};

void pointer_image_free(struct pointer_image *img);

/*
 * Parse an Xcursor file held in memory, taking the image whose nominal size
 * is closest to @size among those no larger than @max_dim in either
 * dimension. Only the first frame of an animated cursor is used.
 * Returns 0 on success or a negative error code.
 */
int pointer_image_parse(const uint8_t *data, size_t len, unsigned int size, unsigned int max_dim,
			struct pointer_image *out);

/*
 * Find @shape in the Xcursor theme @theme, trying the CSS name and then its
 * X11 aliases, and following the theme's Inherits= chain. Searches
 * $XCURSOR_PATH, or the usual icon directories if it is unset.
 * Returns 0 on success or a negative error code (-ENOENT if not found).
 */
int pointer_image_load(const char *theme, int shape, unsigned int size, unsigned int max_dim,
		       struct pointer_image *out);
/* Like pointer_image_load, but only reports whether the theme has the shape. */
bool pointer_image_exists(const char *theme, int shape);

/*
 * Rotate an image, and its hotspot, clockwise by @quarter_turns * 90 degrees.
 * Returns 0 on success or -ENOMEM.
 */
int pointer_image_rotate(struct pointer_image *img, unsigned int quarter_turns);

#endif /* KMSCON_POINTER_SHAPE_H */
