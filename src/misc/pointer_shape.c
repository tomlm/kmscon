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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pointer_shape.h"

#define XCURSOR_MAGIC 0x72756358 /* "Xcur" little-endian */
#define XCURSOR_IMAGE_TYPE 0xfffd0002
#define XCURSOR_IMAGE_HEADER 36
#define XCURSOR_MAX_DIM 0x7fff
#define XCURSOR_MAX_TOC 0x10000
#define XCURSOR_MAX_FILE (8 * 1024 * 1024)
#define XCURSOR_DEFAULT_PATH "~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps"
#define THEME_MAX_DEPTH 8

/*
 * Each shape is its CSS name followed by the file names Xcursor themes use
 * for it, most specific first. Older themes only ship the X11 cursor-font
 * names; newer ones have the CSS names, usually as symlinks. The X11 names
 * double as the aliases accepted in OSC 22, the first shape listing a name
 * winning, so the order of the table matters where a name appears twice.
 */
static const char *const shapes[][6] = {
	{"default", "left_ptr", "arrow", "top_left_arrow"},
	{"text", "xterm", "ibeam"},
	{"pointer", "hand2", "hand1", "hand", "pointing_hand"},
	{"help", "question_arrow", "whats_this", "left_ptr_help"},
	{"progress", "left_ptr_watch", "half-busy"},
	{"wait", "watch", "clock"},
	{"crosshair", "cross", "tcross"},
	{"cell", "plus"},
	{"vertical-text"},
	{"alias", "dnd-link", "link"},
	{"copy", "dnd-copy"},
	{"move", "fleur", "dnd-move", "size_all"},
	{"no-drop", "dnd-no-drop", "circle", "forbidden"},
	{"not-allowed", "crossed_circle", "circle", "forbidden"},
	{"grab", "openhand", "hand1"},
	{"grabbing", "closedhand", "fleur"},
	{"context-menu", "left_ptr"},
	{"all-scroll", "fleur"},
	{"e-resize", "right_side"},
	{"n-resize", "top_side"},
	{"ne-resize", "top_right_corner"},
	{"nw-resize", "top_left_corner"},
	{"s-resize", "bottom_side"},
	{"se-resize", "bottom_right_corner"},
	{"sw-resize", "bottom_left_corner"},
	{"w-resize", "left_side"},
	{"ew-resize", "sb_h_double_arrow", "h_double_arrow", "size_hor"},
	{"ns-resize", "sb_v_double_arrow", "v_double_arrow", "size_ver"},
	{"nesw-resize", "fd_double_arrow", "size_bdiag"},
	{"nwse-resize", "bd_double_arrow", "size_fdiag"},
	{"col-resize", "sb_h_double_arrow", "split_h"},
	{"row-resize", "sb_v_double_arrow", "split_v"},
	{"zoom-in", "zoom_in"},
	{"zoom-out", "zoom_out"},
};

#define SHAPE_COUNT ((int)(sizeof(shapes) / sizeof(shapes[0])))
#define SHAPE_ALIASES ((int)(sizeof(shapes[0]) / sizeof(shapes[0][0])))

_Static_assert(SHAPE_COUNT <= POINTER_SHAPE_MAX, "POINTER_SHAPE_MAX too small");

int pointer_shape_count(void)
{
	return SHAPE_COUNT;
}

const char *pointer_shape_name(int shape)
{
	if (shape < 0 || shape >= SHAPE_COUNT)
		return NULL;
	return shapes[shape][0];
}

static bool name_equals(const char *name, size_t len, const char *str)
{
	return strlen(str) == len && !memcmp(name, str, len);
}

int pointer_shape_lookup(const char *name, size_t len)
{
	int i, j;

	for (i = 0; i < SHAPE_COUNT; i++)
		if (name_equals(name, len, shapes[i][0]))
			return i;

	for (i = 0; i < SHAPE_COUNT; i++)
		for (j = 1; j < SHAPE_ALIASES && shapes[i][j]; j++)
			if (name_equals(name, len, shapes[i][j]))
				return i;

	return POINTER_SHAPE_NONE;
}

int pointer_shape_arrow(void)
{
	return 0;
}

int pointer_shape_text(void)
{
	return 1;
}

void pointer_shape_stack_reset(struct pointer_shape_stack *stack)
{
	stack->depth = 0;
}

int pointer_shape_stack_top(const struct pointer_shape_stack *stack)
{
	if (!stack->depth)
		return POINTER_SHAPE_NONE;
	return stack->shapes[stack->depth - 1];
}

int pointer_shape_effective(const struct pointer_shape_stack *stack, bool grabbed)
{
	int top = pointer_shape_stack_top(stack);

	if (top != POINTER_SHAPE_NONE)
		return top;
	return grabbed ? pointer_shape_arrow() : pointer_shape_text();
}

/* Find the next comma-separated name, with surrounding blanks trimmed. */
static const char *next_name(const char **pos, size_t *len)
{
	const char *start = *pos, *end;

	if (!*start)
		return NULL;

	end = strchr(start, ',');
	if (!end)
		end = start + strlen(start);
	*pos = *end ? end + 1 : end;

	while (start < end && (*start == ' ' || *start == '\t'))
		start++;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;

	*len = end - start;
	return start;
}

static int first_supported(const char *names, pointer_shape_supported_cb supported, void *data)
{
	const char *name;
	size_t len;
	int shape;

	while ((name = next_name(&names, &len))) {
		shape = pointer_shape_lookup(name, len);
		if (shape != POINTER_SHAPE_NONE && (!supported || supported(shape, data)))
			return shape;
	}
	return POINTER_SHAPE_NONE;
}

static char *query_reply(const struct pointer_shape_stack *stack, const char *names, bool grabbed,
			 pointer_shape_supported_cb supported, void *data)
{
	const char *name, *answer, *pos;
	size_t len, count = 1, size, used;
	bool first = true;
	char *reply;
	int shape;

	for (pos = names; *pos; pos++)
		if (*pos == ',')
			count++;

	/* every answer is at most the longest shape name plus a comma */
	size = sizeof("\e]22;\e\\") + count * 16;
	reply = malloc(size);
	if (!reply)
		return NULL;

	used = sprintf(reply, "\e]22;");
	pos = names;
	while ((name = next_name(&pos, &len))) {
		if (!first)
			reply[used++] = ',';
		first = false;

		if (name_equals(name, len, "__current__")) {
			answer = pointer_shape_name(pointer_shape_effective(stack, grabbed));
		} else if (name_equals(name, len, "__default__")) {
			answer = pointer_shape_name(pointer_shape_text());
		} else if (name_equals(name, len, "__grabbed__")) {
			answer = pointer_shape_name(pointer_shape_arrow());
		} else {
			shape = pointer_shape_lookup(name, len);
			answer = shape != POINTER_SHAPE_NONE &&
						 (!supported || supported(shape, data))
					 ? "1"
					 : "0";
		}
		used += snprintf(reply + used, size - used, "%s", answer);
	}
	snprintf(reply + used, size - used, "\e\\");
	return reply;
}

bool pointer_shape_osc(struct pointer_shape_stack *stack, const char *payload, bool grabbed,
		       pointer_shape_supported_cb supported, void *data, char **reply)
{
	char op = '=';
	int shape;

	*reply = NULL;

	if (*payload == '=' || *payload == '>' || *payload == '<' || *payload == '?')
		op = *payload++;

	switch (op) {
	case '?':
		*reply = query_reply(stack, payload, grabbed, supported, data);
		return false;
	case '<':
		if (!stack->depth)
			return false;
		stack->depth--;
		return true;
	case '>':
		shape = first_supported(payload, supported, data);
		if (shape == POINTER_SHAPE_NONE)
			return false;
		if (stack->depth == POINTER_SHAPE_STACK_MAX) {
			memmove(stack->shapes, stack->shapes + 1,
				(POINTER_SHAPE_STACK_MAX - 1) * sizeof(*stack->shapes));
			stack->depth--;
		}
		stack->shapes[stack->depth++] = shape;
		return true;
	default:
		/* an empty set restores the terminal's own pointer */
		if (!*payload) {
			if (!stack->depth)
				return false;
			stack->depth = 0;
			return true;
		}
		shape = first_supported(payload, supported, data);
		if (shape == POINTER_SHAPE_NONE)
			return false;
		if (!stack->depth) {
			stack->shapes[stack->depth++] = shape;
			return true;
		}
		if (stack->shapes[stack->depth - 1] == shape)
			return false;
		stack->shapes[stack->depth - 1] = shape;
		return true;
	}
}

void pointer_image_free(struct pointer_image *img)
{
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

static uint32_t read32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int clamp_hot(uint32_t hot, uint32_t dim)
{
	return hot < dim ? (int)hot : (int)dim - 1;
}

int pointer_image_parse(const uint8_t *data, size_t len, unsigned int size, unsigned int max_dim,
			struct pointer_image *out)
{
	uint32_t header, ntoc, i, pos, chunk, width, height, nominal, best_pos = 0;
	uint32_t best_diff = UINT32_MAX, diff, x, y;
	const uint8_t *src;

	memset(out, 0, sizeof(*out));

	if (len < 16 || read32(data) != XCURSOR_MAGIC)
		return -EINVAL;

	header = read32(data + 4);
	ntoc = read32(data + 12);
	if (header < 16 || header > len || ntoc > XCURSOR_MAX_TOC || (len - header) / 12 < ntoc)
		return -EINVAL;

	for (i = 0; i < ntoc; i++) {
		const uint8_t *toc = data + header + i * 12;

		if (read32(toc) != XCURSOR_IMAGE_TYPE)
			continue;

		pos = read32(toc + 8);
		if (pos > len || len - pos < XCURSOR_IMAGE_HEADER)
			continue;

		chunk = read32(data + pos);
		width = read32(data + pos + 16);
		height = read32(data + pos + 20);
		if (chunk < XCURSOR_IMAGE_HEADER || chunk > len - pos || !width || !height ||
		    width > XCURSOR_MAX_DIM || height > XCURSOR_MAX_DIM || width > max_dim ||
		    height > max_dim || (uint64_t)width * height * 4 > len - pos - chunk)
			continue;

		/* the first frame of each size comes first; keep it on ties */
		nominal = read32(toc + 4);
		diff = nominal > size ? nominal - size : size - nominal;
		if (diff < best_diff) {
			best_diff = diff;
			best_pos = pos;
		}
	}

	if (best_diff == UINT32_MAX)
		return -ENOENT;

	chunk = read32(data + best_pos);
	width = read32(data + best_pos + 16);
	height = read32(data + best_pos + 20);

	out->pixels = malloc((size_t)width * height * sizeof(*out->pixels));
	if (!out->pixels)
		return -ENOMEM;

	out->width = width;
	out->height = height;
	out->hot_x = clamp_hot(read32(data + best_pos + 24), width);
	out->hot_y = clamp_hot(read32(data + best_pos + 28), height);

	src = data + best_pos + chunk;
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			out->pixels[y * width + x] = read32(src + (y * width + x) * 4);

	return 0;
}

/* Expand a leading "~" in an XCURSOR_PATH entry. */
static int dir_path(char *buf, size_t size, const char *dir, size_t len)
{
	const char *home;
	int ret;

	if (len && dir[0] == '~') {
		home = getenv("HOME");
		if (!home)
			return -ENOENT;
		ret = snprintf(buf, size, "%s%.*s", home, (int)len - 1, dir + 1);
	} else {
		ret = snprintf(buf, size, "%.*s", (int)len, dir);
	}
	return ret < 0 || (size_t)ret >= size ? -ENAMETOOLONG : 0;
}

static const char *search_path(void)
{
	const char *path = getenv("XCURSOR_PATH");

	return path && *path ? path : XCURSOR_DEFAULT_PATH;
}

/* Iterate over the directories of the search path. */
static bool next_dir(const char **pos, char *buf, size_t size)
{
	const char *start, *end;

	while (**pos) {
		start = *pos;
		end = strchr(start, ':');
		if (!end)
			end = start + strlen(start);
		*pos = *end ? end + 1 : end;
		if (end > start && !dir_path(buf, size, start, end - start))
			return true;
	}
	return false;
}

/* Find the first of a shape's file names in one theme, ignoring Inherits=. */
static bool find_in_theme(const char *theme, int shape, char *out, size_t size)
{
	char dir[PATH_MAX];
	const char *pos;
	int i, ret;

	for (i = 0; i < SHAPE_ALIASES && shapes[shape][i]; i++) {
		pos = search_path();
		while (next_dir(&pos, dir, sizeof(dir))) {
			ret = snprintf(out, size, "%s/%s/cursors/%s", dir, theme, shapes[shape][i]);
			if (ret > 0 && (size_t)ret < size && !access(out, R_OK))
				return true;
		}
	}
	return false;
}

/* Read the Inherits= line of the first index.theme found for a theme. */
static bool theme_inherits(const char *theme, char *out, size_t size)
{
	char dir[PATH_MAX], file[PATH_MAX], line[512];
	const char *pos = search_path(), *value;
	bool found = false;
	size_t len;
	FILE *f;
	int ret;

	while (next_dir(&pos, dir, sizeof(dir))) {
		ret = snprintf(file, sizeof(file), "%s/%s/index.theme", dir, theme);
		if (ret < 0 || (size_t)ret >= sizeof(file))
			continue;
		f = fopen(file, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "Inherits", 8))
				continue;
			value = line + 8;
			while (*value == ' ' || *value == '\t')
				value++;
			if (*value++ != '=')
				continue;
			len = strcspn(value, "\r\n");
			if (len >= size)
				len = size - 1;
			memcpy(out, value, len);
			out[len] = 0;
			found = true;
			break;
		}
		fclose(f);
		return found;
	}
	return false;
}

static bool find_shape(const char *theme, int shape, char *out, size_t size, int depth)
{
	char inherits[512], name[256];
	const char *pos;
	size_t len;

	if (depth > THEME_MAX_DEPTH || !*theme || strchr(theme, '/'))
		return false;

	if (find_in_theme(theme, shape, out, size))
		return true;

	if (!theme_inherits(theme, inherits, sizeof(inherits)))
		return false;

	pos = inherits;
	while (*pos) {
		len = strcspn(pos, ",; \t");
		if (len && len < sizeof(name)) {
			memcpy(name, pos, len);
			name[len] = 0;
			if (strcmp(name, theme) && find_shape(name, shape, out, size, depth + 1))
				return true;
		}
		pos += len;
		if (*pos)
			pos++;
	}
	return false;
}

/* Find a shape in a theme, falling back to the "default" theme. */
static bool find_shape_file(const char *theme, int shape, char *out, size_t size)
{
	if (shape < 0 || shape >= SHAPE_COUNT)
		return false;
	if (!theme || !*theme)
		theme = "default";
	if (find_shape(theme, shape, out, size, 0))
		return true;
	return strcmp(theme, "default") && find_shape("default", shape, out, size, 0);
}

bool pointer_image_exists(const char *theme, int shape)
{
	char file[PATH_MAX];

	return find_shape_file(theme, shape, file, sizeof(file));
}

static int read_file(const char *file, uint8_t **data, size_t *len)
{
	struct stat st;
	ssize_t ret;
	size_t done = 0;
	int fd;

	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > XCURSOR_MAX_FILE) {
		close(fd);
		return -EINVAL;
	}

	*data = malloc(st.st_size);
	if (!*data) {
		close(fd);
		return -ENOMEM;
	}

	while (done < (size_t)st.st_size) {
		ret = read(fd, *data + done, st.st_size - done);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			break;
		done += ret;
	}
	close(fd);

	*len = done;
	return 0;
}

int pointer_image_load(const char *theme, int shape, unsigned int size, unsigned int max_dim,
		       struct pointer_image *out)
{
	char file[PATH_MAX];
	uint8_t *data = NULL;
	size_t len = 0;
	int ret;

	memset(out, 0, sizeof(*out));

	if (!find_shape_file(theme, shape, file, sizeof(file)))
		return -ENOENT;

	ret = read_file(file, &data, &len);
	if (ret)
		return ret;

	ret = pointer_image_parse(data, len, size, max_dim, out);
	free(data);
	return ret;
}

int pointer_image_rotate(struct pointer_image *img, unsigned int quarter_turns)
{
	unsigned int w = img->width, h = img->height, x, y, nx, ny, nw, nh;
	int hot_x, hot_y;
	uint32_t *pixels;

	quarter_turns %= 4;
	if (!quarter_turns || !img->pixels)
		return 0;

	nw = quarter_turns == 2 ? w : h;
	nh = quarter_turns == 2 ? h : w;

	pixels = malloc((size_t)w * h * sizeof(*pixels));
	if (!pixels)
		return -ENOMEM;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			switch (quarter_turns) {
			case 1:
				nx = h - 1 - y;
				ny = x;
				break;
			case 2:
				nx = w - 1 - x;
				ny = h - 1 - y;
				break;
			default:
				nx = y;
				ny = w - 1 - x;
				break;
			}
			pixels[ny * nw + nx] = img->pixels[y * w + x];
		}
	}

	switch (quarter_turns) {
	case 1:
		hot_x = h - 1 - img->hot_y;
		hot_y = img->hot_x;
		break;
	case 2:
		hot_x = w - 1 - img->hot_x;
		hot_y = h - 1 - img->hot_y;
		break;
	default:
		hot_x = img->hot_y;
		hot_y = w - 1 - img->hot_x;
		break;
	}

	free(img->pixels);
	img->pixels = pixels;
	img->width = nw;
	img->height = nh;
	img->hot_x = hot_x;
	img->hot_y = hot_y;
	return 0;
}
