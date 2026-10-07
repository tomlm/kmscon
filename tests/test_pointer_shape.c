#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pointer_shape.h"

static int failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

static int shape(const char *name)
{
	return pointer_shape_lookup(name, strlen(name));
}

/* everything but zoom-in */
static bool supported(int s, void *data)
{
	return s != shape("zoom-in");
}

static void test_lookup(void)
{
	CHECK(shape("default") == pointer_shape_arrow());
	CHECK(shape("text") == pointer_shape_text());
	CHECK(shape("left_ptr") == pointer_shape_arrow());
	CHECK(shape("xterm") == pointer_shape_text());
	CHECK(shape("hand2") == shape("pointer"));
	CHECK(shape("sb_h_double_arrow") == shape("ew-resize"));
	CHECK(shape("top_left_corner") == shape("nw-resize"));
	CHECK(shape("watch") == shape("wait"));
	CHECK(shape("bogus") == POINTER_SHAPE_NONE);
	CHECK(shape("") == POINTER_SHAPE_NONE);
	CHECK(pointer_shape_lookup("pointer,text", 7) == shape("pointer"));
	CHECK(!strcmp(pointer_shape_name(shape("ns-resize")), "ns-resize"));
	CHECK(pointer_shape_name(POINTER_SHAPE_NONE) == NULL);
	CHECK(pointer_shape_count() <= POINTER_SHAPE_MAX);
}

static bool osc(struct pointer_shape_stack *st, const char *payload, bool grabbed, char **reply)
{
	return pointer_shape_osc(st, payload, grabbed, supported, NULL, reply);
}

static void test_stack(void)
{
	struct pointer_shape_stack st = {0};
	char *reply;
	int i;

	CHECK(pointer_shape_effective(&st, false) == pointer_shape_text());
	CHECK(pointer_shape_effective(&st, true) == pointer_shape_arrow());

	/* set, with fallbacks */
	CHECK(osc(&st, "zoom-in, bogus ,pointer", false, &reply));
	CHECK(!reply);
	CHECK(st.depth == 1 && pointer_shape_stack_top(&st) == shape("pointer"));
	CHECK(pointer_shape_effective(&st, true) == shape("pointer"));

	/* set replaces the top, and again is no change */
	CHECK(osc(&st, "=hand2", false, &reply) == false);
	CHECK(osc(&st, "=ew-resize", false, &reply));
	CHECK(st.depth == 1 && pointer_shape_stack_top(&st) == shape("ew-resize"));

	/* nothing supported: no change */
	CHECK(!osc(&st, "zoom-in,bogus", false, &reply));
	CHECK(pointer_shape_stack_top(&st) == shape("ew-resize"));

	/* push and pop */
	CHECK(osc(&st, ">wait", false, &reply));
	CHECK(st.depth == 2 && pointer_shape_stack_top(&st) == shape("wait"));
	CHECK(!osc(&st, ">zoom-in", false, &reply));
	CHECK(osc(&st, "<", false, &reply));
	CHECK(pointer_shape_stack_top(&st) == shape("ew-resize"));
	CHECK(osc(&st, "<", false, &reply));
	CHECK(st.depth == 0);
	CHECK(!osc(&st, "<", false, &reply));

	/* overflow drops the oldest entry */
	CHECK(osc(&st, ">crosshair", false, &reply));
	for (i = 1; i < POINTER_SHAPE_STACK_MAX; i++)
		CHECK(osc(&st, ">pointer", false, &reply));
	CHECK(st.depth == POINTER_SHAPE_STACK_MAX && st.shapes[0] == shape("crosshair"));
	CHECK(osc(&st, ">text", false, &reply));
	CHECK(st.depth == POINTER_SHAPE_STACK_MAX && st.shapes[0] == shape("pointer"));
	CHECK(pointer_shape_stack_top(&st) == shape("text"));

	/* an empty set resets everything */
	CHECK(osc(&st, "", false, &reply));
	CHECK(st.depth == 0);
	CHECK(!osc(&st, "=", false, &reply));
}

static void test_query(void)
{
	struct pointer_shape_stack st = {0};
	char *reply;

	CHECK(!osc(&st, "?pointer,zoom-in,bogus,left_ptr,__current__,__default__,__grabbed__", true,
		   &reply));
	CHECK(reply && !strcmp(reply, "\e]22;1,0,0,1,default,text,default\e\\"));
	free(reply);

	osc(&st, "move", false, &reply);
	CHECK(!osc(&st, "?__current__", false, &reply));
	CHECK(reply && !strcmp(reply, "\e]22;move\e\\"));
	free(reply);

	CHECK(!osc(&st, "?", false, &reply));
	CHECK(reply && !strcmp(reply, "\e]22;\e\\"));
	free(reply);
	CHECK(st.depth == 1);
}

static void put32(uint8_t **p, uint32_t v)
{
	(*p)[0] = v;
	(*p)[1] = v >> 8;
	(*p)[2] = v >> 16;
	(*p)[3] = v >> 24;
	*p += 4;
}

/*
 * An Xcursor file with images of nominal sizes 24 (two frames, 2x2) and
 * 32 (3x3). Each pixel holds its frame tag in the top byte and its index.
 */
static size_t make_xcursor(uint8_t *buf)
{
	static const uint32_t sizes[] = {24, 24, 32};
	static const uint32_t dims[] = {2, 2, 3};
	uint8_t *p = buf, *pos_field[3];
	uint32_t i, j;

	put32(&p, 0x72756358);
	put32(&p, 16);
	put32(&p, 0x10000);
	put32(&p, 3);
	for (i = 0; i < 3; i++) {
		put32(&p, 0xfffd0002);
		put32(&p, sizes[i]);
		pos_field[i] = p;
		put32(&p, 0);
	}
	for (i = 0; i < 3; i++) {
		uint8_t *field = pos_field[i];

		put32(&field, p - buf);
		put32(&p, 36);
		put32(&p, 0xfffd0002);
		put32(&p, sizes[i]);
		put32(&p, 1);
		put32(&p, dims[i]);
		put32(&p, dims[i]);
		put32(&p, 1);
		put32(&p, i == 2 ? 7 : 0); /* out of range: clamped */
		put32(&p, 50);
		for (j = 0; j < dims[i] * dims[i]; j++)
			put32(&p, (i << 24) | j);
	}
	return p - buf;
}

static void test_parse(void)
{
	uint8_t buf[512];
	struct pointer_image img;
	size_t len = make_xcursor(buf);

	CHECK(!pointer_image_parse(buf, len, 30, 64, &img));
	CHECK(img.width == 3 && img.height == 3 && img.hot_x == 1 && img.hot_y == 2);
	CHECK(img.pixels[4] == ((2u << 24) | 4));
	pointer_image_free(&img);

	/* the first frame wins */
	CHECK(!pointer_image_parse(buf, len, 20, 64, &img));
	CHECK(img.width == 2 && img.pixels[3] == 3);
	pointer_image_free(&img);

	/* too large for the cursor plane */
	CHECK(!pointer_image_parse(buf, len, 32, 2, &img));
	CHECK(img.width == 2);
	pointer_image_free(&img);
	CHECK(pointer_image_parse(buf, len, 32, 1, &img) == -ENOENT);

	/* truncated and corrupt files */
	CHECK(pointer_image_parse(buf, len - 4, 30, 64, &img) == 0);
	CHECK(img.width == 2);
	pointer_image_free(&img);
	CHECK(pointer_image_parse(buf, 20, 30, 64, &img) == -EINVAL);
	buf[0] = 'Y';
	CHECK(pointer_image_parse(buf, len, 30, 64, &img) == -EINVAL);
}

static void test_rotate(void)
{
	uint32_t px[6] = {0, 1, 2, 3, 4, 5}; /* 3 wide, 2 high */
	struct pointer_image img;

	img.width = 3;
	img.height = 2;
	img.hot_x = 2;
	img.hot_y = 0;
	img.pixels = malloc(sizeof(px));
	memcpy(img.pixels, px, sizeof(px));

	/* clockwise: the left column becomes the top row */
	CHECK(!pointer_image_rotate(&img, 1));
	CHECK(img.width == 2 && img.height == 3);
	CHECK(img.pixels[0] == 3 && img.pixels[1] == 0 && img.pixels[4] == 5 && img.pixels[5] == 2);
	CHECK(img.hot_x == 1 && img.hot_y == 2);

	CHECK(!pointer_image_rotate(&img, 3));
	CHECK(img.width == 3 && !memcmp(img.pixels, px, sizeof(px)));
	CHECK(img.hot_x == 2 && img.hot_y == 0);

	CHECK(!pointer_image_rotate(&img, 2));
	CHECK(img.pixels[0] == 5 && img.hot_x == 0 && img.hot_y == 1);
	pointer_image_free(&img);
}

static int write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return -1;
	fwrite(data, 1, len, f);
	return fclose(f);
}

static void test_theme(void)
{
	char root[] = "/tmp/kmscon-test-cursors-XXXXXX";
	char path[512];
	uint8_t buf[512];
	struct pointer_image img;
	size_t len = make_xcursor(buf);
	static const char index[] = "[Icon Theme]\nName=child\nInherits = parent\n";

	if (!mkdtemp(root)) {
		perror("mkdtemp");
		failures++;
		return;
	}

	snprintf(path, sizeof(path), "%s/child", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/child/index.theme", root);
	write_file(path, index, sizeof(index) - 1);
	snprintf(path, sizeof(path), "%s/parent", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/parent/cursors", root);
	mkdir(path, 0755);
	/* only the X11 name, found through the alias table */
	snprintf(path, sizeof(path), "%s/parent/cursors/sb_h_double_arrow", root);
	write_file(path, buf, len);

	setenv("XCURSOR_PATH", root, 1);

	CHECK(pointer_image_exists("child", shape("ew-resize")));
	CHECK(pointer_image_exists("parent", shape("col-resize")));
	CHECK(!pointer_image_exists("child", shape("pointer")));
	CHECK(!pointer_image_exists("../child", shape("ew-resize")));
	CHECK(!pointer_image_load("child", shape("ew-resize"), 24, 64, &img));
	CHECK(img.width == 2);
	pointer_image_free(&img);
	CHECK(pointer_image_load("child", shape("pointer"), 24, 64, &img) == -ENOENT);

	unlink(path);
	snprintf(path, sizeof(path), "%s/parent/cursors", root);
	rmdir(path);
	snprintf(path, sizeof(path), "%s/parent", root);
	rmdir(path);
	snprintf(path, sizeof(path), "%s/child/index.theme", root);
	unlink(path);
	snprintf(path, sizeof(path), "%s/child", root);
	rmdir(path);
	rmdir(root);
}

int main(void)
{
	test_lookup();
	test_stack();
	test_query();
	test_parse();
	test_rotate();
	test_theme();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("all pointer shape tests passed\n");
	return EXIT_SUCCESS;
}
