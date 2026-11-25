/* render.c - batching of rectangles and glyphs shared by the renderers */
#include <stdlib.h>
#include <string.h>

#include "gui.h"

struct scene scene;

/* start collecting a new frame of w by h pixels */
void scene_reset(int w, int h, uint32_t clear)
{
	scene.w = w;
	scene.h = h;
	scene.clear = clear;
	scene.scale = plat_scale();
	scene.n = scene.nb = 0;
	r_scissor(0, 0, -1, -1);
}

void scene_unpack(uint32_t c, float *out)
{
	out[0] = (c >> 24 & 0xff) / 255.0f;
	out[1] = (c >> 16 & 0xff) / 255.0f;
	out[2] = (c >> 8 & 0xff) / 255.0f;
	out[3] = (c & 0xff) / 255.0f;
}

static void push(struct inst *in)
{
	struct batch *last = scene.nb ? &scene.b[scene.nb - 1] : NULL;

	if (!last || last->x != scene.sc.x || last->y != scene.sc.y ||
	    last->w != scene.sc.w || last->h != scene.sc.h) {
		if (scene.nb == scene.bcap) {
			scene.bcap = scene.bcap * 2 + 16;
			scene.b = xrealloc(scene.b, scene.bcap *
					   sizeof(*scene.b));
		}
		last = &scene.b[scene.nb++];
		*last = scene.sc;
		last->first = scene.n;
		last->count = 0;
	}
	if (scene.n == scene.cap) {
		scene.cap = scene.cap * 2 + 1024;
		scene.data = xrealloc(scene.data, scene.cap *
				      sizeof(*scene.data));
	}
	scene.data[scene.n++] = *in;
	last->count++;
}

/* draw a rounded rectangle in logical coordinates; border > 0 outlines */
void r_rect(float x, float y, float w, float h, uint32_t c, float radius,
	    float border)
{
	struct inst in = { 0 };
	float s = scene.scale;

	in.rect[0] = x * s;
	in.rect[1] = y * s;
	in.rect[2] = w * s;
	in.rect[3] = h * s;
	scene_unpack(c, in.color);
	in.params[0] = radius * s;
	in.params[1] = border * s;
	push(&in);
}

/* draw an atlas glyph at physical pixel coordinates */
void r_glyph(float x, float y, float w, float h, const float uv[4],
	     uint32_t c)
{
	struct inst in = { 0 };

	in.rect[0] = x;
	in.rect[1] = y;
	in.rect[2] = w;
	in.rect[3] = h;
	memcpy(in.uv, uv, sizeof(in.uv));
	scene_unpack(c, in.color);
	in.params[2] = 1;
	push(&in);
}

/* clip to a logical rectangle clamped to the frame; w < 0 removes it */
void r_scissor(float x, float y, float w, float h)
{
	float s = scene.scale;
	int x0, y0, x1, y1;

	if (w < 0) {
		scene.sc.x = scene.sc.y = 0;
		scene.sc.w = scene.w;
		scene.sc.h = scene.h;
		return;
	}
	x0 = x * s < 0 ? 0 : x * s;
	y0 = y * s < 0 ? 0 : y * s;
	x1 = (x + w) * s;
	y1 = (y + h) * s;
	x0 = x0 > scene.w ? scene.w : x0;
	y0 = y0 > scene.h ? scene.h : y0;
	x1 = x1 > scene.w ? scene.w : x1;
	y1 = y1 > scene.h ? scene.h : y1;
	scene.sc.x = x0;
	scene.sc.y = y0;
	scene.sc.w = x1 > x0 ? x1 - x0 : 0;
	scene.sc.h = y1 > y0 ? y1 - y0 : 0;
}

/* save the next frame to path */
void r_screenshot(const char *path)
{
	free(scene.shot);
	scene.shot = xstrdup(path);
}

/* write a captured frame of 32-bit pixels as a binary PPM */
void scene_shot(const uint8_t *p, int bgr)
{
	FILE *f = fopen(scene.shot, "wb");
	size_t i, n = (size_t)scene.w * scene.h;

	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", scene.w, scene.h);
		for (i = 0; i < n; i++, p += 4) {
			fputc(p[bgr ? 2 : 0], f);
			fputc(p[1], f);
			fputc(p[bgr ? 0 : 2], f);
		}
		fclose(f);
	} else {
		warnx("cannot write %s", scene.shot);
	}
	free(scene.shot);
	scene.shot = NULL;
}
