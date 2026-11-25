/* font.c - FreeType glyph cache packed into a texture atlas */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui.h"

#define ATLAS 2048
#define CACHE 8192

struct glyph {
	uint32_t key, cp;
	int used;
	short w, h, bx, by;
	float adv, uv[4];
};

static FT_Library ft;
static FT_Face face[3], fallback[8];
static int nfallback;
static uint8_t *atlas;
static int ax, ay, rowh, dirty, overflow;
static struct glyph cache[CACHE];
static float scale = 1;

static const char *ui_fonts[] = {
	"/usr/share/fonts/noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/liberation-fonts/LiberationSans-Regular.ttf",
	"/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
	NULL
};

static const char *mono_fonts[] = {
	"/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
	"/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
	"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
	"/usr/share/fonts/liberation-fonts/LiberationMono-Regular.ttf",
	"/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
	NULL
};

static const char *bold_fonts[] = {
	"/usr/share/fonts/noto/NotoSans-Bold.ttf",
	"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
	"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
	"/usr/share/fonts/liberation-fonts/LiberationSans-Bold.ttf",
	"/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
	NULL
};

static const char *fallback_fonts[] = {
	"/usr/share/fonts/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
	"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/noto/NotoColorEmoji.ttf",
	NULL
};

static FT_Face load(const char *want, const char **list)
{
	FT_Face f = NULL;
	int i;

	if (want && !FT_New_Face(ft, want, 0, &f))
		return f;
	if (want)
		warnx("cannot load font %s", want);
	for (i = 0; list[i]; i++)
		if (!access(list[i], R_OK) && !FT_New_Face(ft, list[i], 0, &f))
			return f;
	return NULL;
}

/* load the configured fonts for the given output scale */
int font_init(float s)
{
	int i;

	scale = s;
	if (!ft && FT_Init_FreeType(&ft))
		return -1;
	if (!atlas)
		atlas = xcalloc(ATLAS, ATLAS);
	if (!face[FONT_UI]) {
		face[FONT_UI] = load(cfg.font, ui_fonts);
		face[FONT_MONO] = load(cfg.font_mono, mono_fonts);
		face[FONT_BOLD] = load(NULL, bold_fonts);
		for (i = 0; fallback_fonts[i] && nfallback < 8; i++)
			if (!access(fallback_fonts[i], R_OK) &&
			    !FT_New_Face(ft, fallback_fonts[i], 0,
					 &fallback[nfallback]))
				nfallback++;
	}
	if (!face[FONT_UI]) {
		warnx("no usable font found, set 'font' in the configuration");
		return -1;
	}
	if (!face[FONT_MONO])
		face[FONT_MONO] = face[FONT_UI];
	if (!face[FONT_BOLD])
		face[FONT_BOLD] = face[FONT_UI];
	memset(cache, 0, sizeof(cache));
	memset(atlas, 0, ATLAS * ATLAS);
	ax = ay = rowh = 0;
	dirty = 1;
	return 0;
}

static FT_Face sized(FT_Face f, int size)
{
	FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(size * scale + 0.5f));
	return f;
}

static struct glyph *glyph(int font, int size, uint32_t cp)
{
	uint32_t key = cp * 2654435761u ^ (uint32_t)(font << 24 | size << 12);
	uint32_t h = key % CACHE, i;
	struct glyph *g;
	FT_Face f;
	FT_GlyphSlot sl;
	int x, y;

	for (i = 0; i < CACHE; i++) {
		g = &cache[(h + i) % CACHE];
		if (!g->used)
			break;
		if (g->key == key && g->cp == cp)
			return g;
	}
	if (i == CACHE)
		return NULL;
	f = sized(face[font], size);
	for (x = 0; !FT_Get_Char_Index(f, cp) && x < nfallback; x++)
		if (FT_Get_Char_Index(fallback[x], cp))
			f = sized(fallback[x], size);
	if (FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT))
		return NULL;
	sl = f->glyph;
	if (ax + (int)sl->bitmap.width + 1 >= ATLAS) {
		ax = 0;
		ay += rowh + 1;
		rowh = 0;
	}
	if (ay + (int)sl->bitmap.rows + 1 >= ATLAS) {
		overflow = 1;
		return NULL;
	}
	for (y = 0; y < (int)sl->bitmap.rows; y++)
		for (x = 0; x < (int)sl->bitmap.width; x++)
			atlas[(ay + y) * ATLAS + ax + x] =
			    sl->bitmap.buffer[y * sl->bitmap.pitch + x];
	g->used = 1;
	g->key = key;
	g->cp = cp;
	g->w = sl->bitmap.width;
	g->h = sl->bitmap.rows;
	g->bx = sl->bitmap_left;
	g->by = sl->bitmap_top;
	g->adv = sl->advance.x / 64.0f;
	g->uv[0] = (float)ax / ATLAS;
	g->uv[1] = (float)ay / ATLAS;
	g->uv[2] = (float)(ax + g->w) / ATLAS;
	g->uv[3] = (float)(ay + g->h) / ATLAS;
	ax += g->w + 1;
	if (g->h > rowh)
		rowh = g->h;
	dirty = 1;
	return g;
}

/* width of a UTF-8 string in logical pixels */
float font_width(int font, int size, const char *s, int len)
{
	float w = 0;
	int i = 0;

	while (i < len) {
		uint32_t cp;
		struct glyph *g;

		i += utf8_decode(s + i, &cp);
		if (cp == '\t')
			cp = ' ';
		g = glyph(font, size, cp);
		if (g)
			w += g->adv * (s[i - 1] == '\t' ? 4 : 1);
	}
	return w / scale;
}

float font_lineh(int font, int size)
{
	FT_Face f = sized(face[font], size);

	return (f->size->metrics.height >> 6) / scale;
}

/* draw text with its top left corner at logical (x, y) */
void font_draw(int font, int size, float x, float y, const char *s, int len,
	       uint32_t col)
{
	FT_Face f = sized(face[font], size);
	float px = x * scale, base = y * scale + (f->size->metrics.ascender >>
						   6);
	int i = 0;

	px = (float)(int)(px + 0.5f);
	base = (float)(int)(base + 0.5f);
	while (i < len) {
		uint32_t cp;
		struct glyph *g;
		int tab;

		i += utf8_decode(s + i, &cp);
		tab = cp == '\t';
		if (tab)
			cp = ' ';
		g = glyph(font, size, cp);
		if (!g)
			continue;
		if (g->w && g->h)
			r_glyph(px + g->bx, base - g->by, g->w, g->h, g->uv,
				col);
		px += g->adv * (tab ? 4 : 1);
	}
}

/* upload new glyphs; called once per frame after the GPU is idle */
void font_frame(void)
{
	if (overflow) {
		overflow = 0;
		font_init(scale);
		app.dirty = 1;
	}
	if (dirty) {
		r_atlas(atlas, ATLAS, ATLAS);
		dirty = 0;
	}
}
