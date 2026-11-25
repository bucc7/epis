/* metal.c - Metal renderer for rectangles and glyphs */
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "objc.h"

/* the parts of the Metal API that its Objective-C headers declare */
#define PIXEL_R8 10
#define PIXEL_BGRA8 80
#define BLEND_ONE 1
#define BLEND_SRC_ALPHA 4
#define BLEND_ONE_MINUS_SRC_ALPHA 5
#define LOAD_CLEAR 2
#define STORE_STORE 1
#define PRIMITIVE_TRIANGLE 3

struct mtl_origin {
	nsuint x, y, z;
};

struct mtl_size {
	nsuint w, h, d;
};

struct mtl_region {
	struct mtl_origin origin;
	struct mtl_size size;
};

struct mtl_clear {
	double r, g, b, a;
};

struct mtl_scissor {
	nsuint x, y, w, h;
};

extern id MTLCreateSystemDefaultDevice(void);

static const char shader_source[] = {
#include "ui.metal.inc"
	, 0
};

static struct {
	id dev, queue, pipe, atlas, vb, last, pool, drawable, layer;
	size_t vb_cap;
} mtl;

static id indexed(id array, nsuint i)
{
	return MSG(id, id, SEL, nsuint)(array,
		sel("objectAtIndexedSubscript:"), i);
}

static int make_pipeline(void)
{
	id err = NULL, lib, desc, att, vs, fs;

	lib = MSG(id, id, SEL, id, id, id *)(mtl.dev,
		sel("newLibraryWithSource:options:error:"),
		nsstr(shader_source), NULL, &err);
	if (!lib) {
		warnx("metal: %s", utf8(get(err, "localizedDescription")));
		return -1;
	}
	vs = MSG(id, id, SEL, id)(lib, sel("newFunctionWithName:"),
				  nsstr("ui_vertex"));
	fs = MSG(id, id, SEL, id)(lib, sel("newFunctionWithName:"),
				  nsstr("ui_fragment"));
	desc = new_object("MTLRenderPipelineDescriptor");
	set_id(desc, "setVertexFunction:", vs);
	set_id(desc, "setFragmentFunction:", fs);
	att = indexed(get(desc, "colorAttachments"), 0);
	set_int(att, "setPixelFormat:", PIXEL_BGRA8);
	set_bool(att, "setBlendingEnabled:", 1);
	set_int(att, "setSourceRGBBlendFactor:", BLEND_SRC_ALPHA);
	set_int(att, "setDestinationRGBBlendFactor:",
		BLEND_ONE_MINUS_SRC_ALPHA);
	set_int(att, "setSourceAlphaBlendFactor:", BLEND_ONE);
	set_int(att, "setDestinationAlphaBlendFactor:",
		BLEND_ONE_MINUS_SRC_ALPHA);
	mtl.pipe = MSG(id, id, SEL, id, id *)(mtl.dev,
		sel("newRenderPipelineStateWithDescriptor:error:"), desc,
		&err);
	if (!mtl.pipe)
		warnx("metal: %s", utf8(get(err, "localizedDescription")));
	call(desc, "release");
	call(vs, "release");
	call(fs, "release");
	call(lib, "release");
	return mtl.pipe ? 0 : -1;
}

static void make_atlas(void)
{
	id desc = MSG(id, id, SEL, nsuint, nsuint, nsuint, BOOL)(
		cls("MTLTextureDescriptor"),
		sel("texture2DDescriptorWithPixelFormat:width:height:"
		    "mipmapped:"), PIXEL_R8, ATLAS, ATLAS, 0);

	mtl.atlas = MSG(id, id, SEL, id)(mtl.dev,
		sel("newTextureWithDescriptor:"), desc);
}

/* create the device, the pipeline and the glyph atlas texture */
int r_init(void)
{
	id pool = new_object("NSAutoreleasePool");
	CGColorSpaceRef srgb;
	int r = -1;

	mtl.dev = MTLCreateSystemDefaultDevice();
	if (!mtl.dev) {
		warnx("metal: no suitable device");
		goto out;
	}
	mtl.layer = plat_metal_layer();
	set_id(mtl.layer, "setDevice:", mtl.dev);
	set_int(mtl.layer, "setPixelFormat:", PIXEL_BGRA8);
	srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	MSG(void, id, SEL, CGColorSpaceRef)(mtl.layer, sel("setColorspace:"),
					    srgb);
	CGColorSpaceRelease(srgb);
	mtl.queue = get(mtl.dev, "newCommandQueue");
	make_atlas();
	if (mtl.queue && mtl.atlas)
		r = make_pipeline();
out:
	call(pool, "drain");
	return r;
}

/* wait until the GPU is done with the buffers of the previous frame */
static void wait_idle(void)
{
	if (!mtl.last)
		return;
	call(mtl.last, "waitUntilCompleted");
	call(mtl.last, "release");
	mtl.last = NULL;
}

void r_quit(void)
{
	if (!mtl.dev)
		return;
	wait_idle();
	if (mtl.vb)
		call(mtl.vb, "release");
	call(mtl.atlas, "release");
	call(mtl.pipe, "release");
	call(mtl.queue, "release");
	call(mtl.dev, "release");
	free(scene.data);
	free(scene.b);
}

/* copy a region of the glyph atlas into the texture */
void r_atlas(const uint8_t *pix, int w, int h)
{
	struct mtl_region r = { { 0, 0, 0 }, { 0, 0, 1 } };

	r.size.w = w < ATLAS ? w : ATLAS;
	r.size.h = h < ATLAS ? h : ATLAS;
	MSG(void, id, SEL, struct mtl_region, nsuint, const void *, nsuint)(
		mtl.atlas,
		sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), r, 0,
		pix, w);
}

/* start a frame, returns -1 when nothing can be drawn */
int r_begin(uint32_t clear)
{
	CGSize size;
	int w, h;

	wait_idle();
	plat_size(&w, &h);
	if (w <= 0 || h <= 0)
		return -1;
	mtl.pool = new_object("NSAutoreleasePool");
	size.width = w;
	size.height = h;
	MSG(void, id, SEL, CGSize)(mtl.layer, sel("setDrawableSize:"), size);
	/* only a screenshot needs to read the drawable back */
	set_bool(mtl.layer, "setFramebufferOnly:", !scene.shot);
	mtl.drawable = get(mtl.layer, "nextDrawable");
	if (!mtl.drawable) {
		call(mtl.pool, "drain");
		return -1;
	}
	scene_reset(w, h, clear);
	return 0;
}

static id buffer_new(size_t size)
{
	/* option 0 is shared, CPU visible storage */
	return MSG(id, id, SEL, nsuint, nsuint)(mtl.dev,
		sel("newBufferWithLength:options:"), size, 0);
}

static void *buffer_ptr(id b)
{
	return MSG(void *, id, SEL)(b, sel("contents"));
}

/* copy the rendered texture into a CPU visible buffer */
static id shot_record(id cb, id tex)
{
	nsuint row = (nsuint)scene.w * 4, size = row * scene.h;
	struct mtl_origin o = { 0, 0, 0 };
	struct mtl_size s = { 0, 0, 1 };
	id buf = buffer_new(size), blit = get(cb, "blitCommandEncoder");

	s.w = scene.w;
	s.h = scene.h;
	MSG(void, id, SEL, id, nsuint, nsuint, struct mtl_origin,
	    struct mtl_size, id, nsuint, nsuint, nsuint)(blit,
		sel("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:"
		    "sourceSize:toBuffer:destinationOffset:"
		    "destinationBytesPerRow:destinationBytesPerImage:"), tex,
		0, 0, o, s, buf, 0, row, size);
	call(blit, "endEncoding");
	return buf;
}

static void draw_batches(id enc)
{
	float screen[2];
	size_t i;

	screen[0] = scene.w;
	screen[1] = scene.h;
	set_id(enc, "setRenderPipelineState:", mtl.pipe);
	MSG(void, id, SEL, id, nsuint, nsuint)(enc,
		sel("setVertexBuffer:offset:atIndex:"), mtl.vb, 0, 0);
	MSG(void, id, SEL, const void *, nsuint, nsuint)(enc,
		sel("setVertexBytes:length:atIndex:"), screen, sizeof(screen),
		1);
	MSG(void, id, SEL, id, nsuint)(enc, sel("setFragmentTexture:atIndex:"),
				       mtl.atlas, 0);
	for (i = 0; i < scene.nb; i++) {
		struct batch *b = &scene.b[i];
		struct mtl_scissor sc;

		if (!b->w || !b->h)
			continue;
		sc.x = b->x;
		sc.y = b->y;
		sc.w = b->w;
		sc.h = b->h;
		MSG(void, id, SEL, struct mtl_scissor)(enc,
			sel("setScissorRect:"), sc);
		MSG(void, id, SEL, nsuint, nsuint, nsuint, nsuint, nsuint)(
			enc, sel("drawPrimitives:vertexStart:vertexCount:"
				 "instanceCount:baseInstance:"),
			PRIMITIVE_TRIANGLE, 0, 6, b->count, b->first);
	}
}

/* record and submit the frame, then present it */
void r_end(void)
{
	size_t need = scene.n * sizeof(struct inst);
	id tex = get(mtl.drawable, "texture");
	id cb = get(mtl.queue, "commandBuffer");
	id pass = get(cls("MTLRenderPassDescriptor"), "renderPassDescriptor");
	id att = indexed(get(pass, "colorAttachments"), 0), enc, shot = NULL;
	struct mtl_clear cc;
	float c[4];

	if (need > mtl.vb_cap) {
		if (mtl.vb)
			call(mtl.vb, "release");
		mtl.vb = buffer_new(need * 2);
		mtl.vb_cap = mtl.vb ? need * 2 : 0;
	}
	if (need && need <= mtl.vb_cap)
		memcpy(buffer_ptr(mtl.vb), scene.data, need);
	scene_unpack(scene.clear, c);
	cc.r = c[0];
	cc.g = c[1];
	cc.b = c[2];
	cc.a = c[3];
	set_id(att, "setTexture:", tex);
	set_int(att, "setLoadAction:", LOAD_CLEAR);
	set_int(att, "setStoreAction:", STORE_STORE);
	MSG(void, id, SEL, struct mtl_clear)(att, sel("setClearColor:"), cc);
	enc = MSG(id, id, SEL, id)(cb,
		sel("renderCommandEncoderWithDescriptor:"), pass);
	if (need && need <= mtl.vb_cap)
		draw_batches(enc);
	call(enc, "endEncoding");
	if (scene.shot)
		shot = shot_record(cb, tex);
	set_id(cb, "presentDrawable:", mtl.drawable);
	call(cb, "commit");
	mtl.last = get(cb, "retain");
	if (shot) {
		wait_idle();
		scene_shot(buffer_ptr(shot), 1);
		call(shot, "release");
	}
	call(mtl.pool, "drain");
}
