// re3 WASM rendering diagnostic (Phase 4, task 10/11).
//
// This is deliberately NOT part of re3/GTA III -- it links directly against librw
// (the same rendering library re3 itself uses) through librw's own lightweight
// "skeleton" windowing harness (vendor/librw/skeleton/), the same one librw's own
// imguitest tool uses. It exercises exactly the rendering path GTA III's own model/
// sprite/HUD rendering goes through -- the Im2D immediate-mode pipeline
// (rw::im2d::*, backed by vendor/librw/src/gl/gl3immed.cpp's shader, VBO, and IBO) --
// without needing re3's engine, game state, or any GTA III assets at all. See
// docs/WEBGL_COMPATIBILITY.md for how this fits into the overall rendering trace.
//
// Every stage below is verified two ways: rw's own error/assert paths (a broken
// shader or raster would abort long before Draw() runs), and by reading back actual
// framebuffer pixels after rendering and checking they're what the test scene
// expects -- not just "no crash", but "the right thing landed on screen". Results
// are printed as PASS/FAIL lines (visible in web/rendertest.html's log panel and any
// terminal capturing stdout), which is what makes this a *diagnostic* mode rather
// than just a demo.

#include <rw.h>
#include "skeleton.h"
#include <cstdio>
#include <cstring>

using namespace rw;
using namespace sk;

rw::EngineOpenParams engineOpenParams;

namespace {

struct {
	rw::Camera *camera;
	rw::Texture *checkerTexture;
	int passCount;
	int failCount;
	bool ranOnce;
} Scene;

void report(bool ok, const char *stage, const char *detail) {
	if(ok) Scene.passCount++; else Scene.failCount++;
	printf("[rendertest] %s: %s%s%s\n", ok ? "PASS" : "FAIL", stage,
	       detail ? " -- " : "", detail ? detail : "");
}

// ---------------------------------------------------------------------------
// A small procedurally-generated checkerboard texture. No external asset files
// are loaded -- this test scene must run with nothing but the WASM module itself,
// consistent with Phase 3's "no GTA III assets required yet" boundary.
// ---------------------------------------------------------------------------
rw::Texture *makeCheckerTexture(int size, int checks) {
	rw::Image *img = rw::Image::create(size, size, 32);
	if(img == nil){
		report(false, "checker texture", "Image::create failed");
		return nil;
	}
	img->allocate();
	for(int y = 0; y < size; y++){
		uint8 *row = img->pixels + y * img->stride;
		for(int x = 0; x < size; x++){
			bool on = ((x * checks) / size + (y * checks) / size) % 2 == 0;
			uint8 *px = row + x * 4;
			px[0] = on ? 255 : 30;   // R
			px[1] = on ? 210 : 40;  // G
			px[2] = on ? 60  : 200; // B
			px[3] = 255;
		}
	}
	rw::Raster *raster = rw::Raster::createFromImage(img);
	img->destroy();
	if(raster == nil){
		report(false, "checker texture", "Raster::createFromImage failed");
		return nil;
	}
	rw::Texture *tex = rw::Texture::create(raster);
	tex->setFilter(rw::Texture::NEAREST);
	tex->setAddressU(rw::Texture::WRAP);
	tex->setAddressV(rw::Texture::WRAP);
	report(tex != nil, "checker texture created", nil);
	return tex;
}

// ---------------------------------------------------------------------------
// Im2D helpers -- screen-space quads/triangles through rw::im2d, the same
// immediate-mode path re3 uses for 2D sprites/HUD (CSprite2d etc, via re3's
// fakerw shim over this same librw API).
// ---------------------------------------------------------------------------

void drawFlatTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                       float z, RGBA color) {
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::VERTEXALPHA, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 0);
	rw::SetRenderState(rw::ZWRITEENABLE, 0);

	RWDEVICE::Im2DVertex v[3];
	float camZ = Scene.camera->nearPlane;
	float xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
	for(int i = 0; i < 3; i++){
		v[i].setScreenX(xs[i]);
		v[i].setScreenY(ys[i]);
		v[i].setScreenZ(z);
		v[i].setCameraZ(camZ);
		v[i].setColor(color.red, color.green, color.blue, color.alpha);
		v[i].setU(0.0f, 1.0f);
		v[i].setV(0.0f, 1.0f);
	}
	rw::im2d::RenderPrimitive(rw::PRIMTYPETRILIST, v, 3);
}

void drawQuad(float x, float y, float w, float h, float z, RGBA color,
              rw::Raster *texture, bool depthTest, bool depthWrite) {
	rw::SetRenderStatePtr(rw::TEXTURERASTER, texture);
	if(texture != nil){
		rw::SetRenderState(rw::TEXTUREADDRESS, rw::Texture::WRAP);
		rw::SetRenderState(rw::TEXTUREFILTER, rw::Texture::NEAREST);
	}
	rw::SetRenderState(rw::VERTEXALPHA, color.alpha < 255);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
	rw::SetRenderState(rw::ZTESTENABLE, depthTest);
	rw::SetRenderState(rw::ZWRITEENABLE, depthWrite);

	RWDEVICE::Im2DVertex v[4];
	float camZ = Scene.camera->nearPlane;
	float xs[4] = { x, x + w, x, x + w };
	float ys[4] = { y, y, y + h, y + h };
	float us[4] = { 0, 1, 0, 1 };
	float vs[4] = { 0, 0, 1, 1 };
	for(int i = 0; i < 4; i++){
		v[i].setScreenX(xs[i]);
		v[i].setScreenY(ys[i]);
		v[i].setScreenZ(z);
		v[i].setCameraZ(camZ);
		v[i].setColor(color.red, color.green, color.blue, color.alpha);
		v[i].setU(us[i], 1.0f);
		v[i].setV(vs[i], 1.0f);
	}
	uint16 idx[6] = { 0, 1, 2, 2, 1, 3 };
	rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, v, 4, idx, 6);
}

// ---------------------------------------------------------------------------
// Pixel readback validation -- reads the actual rendered framebuffer back (via
// Raster::toImage(), which goes through the exact gl3Caps.gles glReadPixels/FBO
// path fixed for WebGL2 -- see docs/WEBGL_COMPATIBILITY.md) and checks real pixel
// colors, not just "the draw call didn't crash".
// ---------------------------------------------------------------------------

struct Px { int x, y; uint8 r, g, b; };

bool sampleMatches(rw::Image *img, int x, int y, uint8 r, uint8 g, uint8 b, int tolerance) {
	// Raster::toImage() doesn't always return 32bpp RGBA -- notably, the on-screen
	// Raster::CAMERA framebuffer reads back as 24bpp RGB (see gl3::rasterLock()'s
	// `case Raster::CAMERA:`, which calls glReadPixels(..., GL_RGB, ...) and
	// asserts bpp==3), while our procedural checker texture is 32bpp RGBA (created
	// via Image::create(size, size, 32)). Index by the image's *actual* bpp rather
	// than assuming 4, or reads silently walk into the wrong pixel entirely.
	if(x < 0 || y < 0 || x >= img->width || y >= img->height) return false;
	int bpp = img->depth / 8;
	uint8 *p = img->pixels + y * img->stride + x * bpp;
	return abs((int)p[0] - r) <= tolerance && abs((int)p[1] - g) <= tolerance && abs((int)p[2] - b) <= tolerance;
}

void describePixel(char *out, rw::Image *img, int x, int y) {
	if(x < 0 || y < 0 || x >= img->width || y >= img->height){
		sprintf(out, "(%d,%d) out of bounds", x, y);
		return;
	}
	int bpp = img->depth / 8;
	uint8 *p = img->pixels + y * img->stride + x * bpp;
	if(bpp >= 4)
		sprintf(out, "(%d,%d)=rgba(%d,%d,%d,%d)", x, y, p[0], p[1], p[2], p[3]);
	else
		sprintf(out, "(%d,%d)=rgb(%d,%d,%d)", x, y, p[0], p[1], p[2]);
}

void validateFramebuffer(int width, int height) {
	(void)width; (void)height;
	rw::Image *img = Scene.camera->frameBuffer->toImage();
	if(img == nil){
		report(false, "framebuffer readback", "Raster::toImage() returned null");
		return;
	}
	char detail[128];

	// Background: sample a corner far from anything we drew.
	bool bgOk = sampleMatches(img, 4, 4, 20, 24, 32, 12);
	describePixel(detail, img, 4, 4);
	report(bgOk, "clear screen", detail);

	// Triangle region (left third): centroid of the triangle we drew.
	bool triOk = sampleMatches(img, 160, 260, 220, 40, 40, 24);
	describePixel(detail, img, 160, 260);
	report(triOk, "2D triangle", detail);

	// Textured quad region (middle third): two sample points a checker-cell apart
	// should differ from each other, and neither should equal flat background --
	// proves real texture data (not a solid fallback color) reached the GPU.
	// The quad is drawn at screen (360,100)-(560,300) with UV (0,0)-(1,1) mapped
	// linearly across it, sampling a 64px/8-cell checker texture (see
	// makeCheckerTexture). (380,120) -> local(20,20) -> UV(0.1,0.1) -> texel
	// (6,6) -> cell(0,0), even -> "on". (500,120) -> local(140,20) -> UV(0.7,0.1)
	// -> texel(44,6) -> cell(5,0), odd -> "off". These are deliberately chosen to
	// land on different-parity cells -- don't move them without recomputing.
	bool quadA = !sampleMatches(img, 380, 120, 20, 24, 32, 12);
	bool quadB = !sampleMatches(img, 500, 120, 20, 24, 32, 12);
	int bpp = img->depth / 8;
	uint8 *pa = img->pixels + 120 * img->stride + 380 * bpp;
	uint8 *pb = img->pixels + 120 * img->stride + 500 * bpp;
	bool quadVaries = abs((int)pa[0] - (int)pb[0]) > 40 || abs((int)pa[1] - (int)pb[1]) > 40 || abs((int)pa[2] - (int)pb[2]) > 40;
	sprintf(detail, "(380,120)=rgb(%d,%d,%d) (500,120)=rgb(%d,%d,%d)", pa[0], pa[1], pa[2], pb[0], pb[1], pb[2]);
	report(quadA && quadB && quadVaries, "textured quad (checker pattern visible)", detail);

	// Depth test region (right third): draw order was near(blue)-then-far(orange)
	// with ZTESTENABLE -- if depth testing genuinely rejects the far quad, blue
	// should still be on top. If this comes back orange, depth testing isn't
	// actually rejecting occluded fragments under WebGL2.
	bool depthOk = sampleMatches(img, 800, 260, 60, 90, 230, 30);
	describePixel(detail, img, 800, 260);
	report(depthOk, "depth test (near quad occludes far quad)", detail);

	img->destroy();
}

} // namespace

void Draw(float timeDelta);

void Init(void) {
	sk::globals.windowtitle = "re3 WASM rendering diagnostic";
	sk::globals.width = 960;
	sk::globals.height = 540;
	sk::globals.quit = 0;
}

bool attachPlugins(void) {
	rw::registerMeshPlugin();
	rw::registerNativeDataPlugin();
	return true;
}

bool InitRW(void) {
	if(!sk::InitRW())
		return false;

	Scene.camera = sk::CameraCreate(sk::globals.width, sk::globals.height, 1);
	if(Scene.camera == nil){
		report(false, "camera creation", nil);
		return false;
	}
	report(true, "RW engine + GL3/WebGL2 device + camera initialized", nil);

	Scene.checkerTexture = makeCheckerTexture(64, 8);

	// Run the diagnostic scene once immediately, synchronously, rather than only
	// from the IDLE-driven main loop below: emscripten_set_main_loop's default
	// timing is requestAnimationFrame, which browsers suspend entirely for a
	// backgrounded/hidden tab (see docs/BROWSER_RUNTIME.md's Phase 2 notes on the
	// same issue) -- a diagnostic mode shouldn't depend on the tab being focused
	// (or on the browser ever scheduling a frame) to produce a result. The ongoing
	// per-frame Draw() call from IDLE still runs afterwards for live viewing.
	Draw(0.0f);

	return true;
}

void Draw(float timeDelta) {
	(void)timeDelta;

	rw::RGBA clearcol = rw::makeRGBA(20, 24, 32, 255);
	Scene.camera->clear(&clearcol, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	Scene.camera->beginUpdate();

	// 1. Solid-color triangle (left third).
	drawFlatTriangle(40, 440, 280, 440, 160, 100, 0.0f, rw::makeRGBA(220, 40, 40, 255));

	// 2. Textured quad (middle third).
	drawQuad(360, 100, 200, 200, 0.0f, rw::makeRGBA(255, 255, 255, 255),
	         Scene.checkerTexture ? Scene.checkerTexture->raster : nil, false, false);

	// 3. Depth-tested overlapping geometry (right third): near quad (blue) drawn
	// first with a small screenZ, far quad (orange) drawn second with a larger
	// screenZ, both with ZTESTENABLE+ZWRITEENABLE. With correct depth testing
	// (GL_LEQUAL, see vendor/librw/src/gl/gl3device.cpp resetRenderState()), the
	// far quad's fragments must fail against the near quad's already-written
	// depth, so blue should remain on top despite being drawn first.
	drawQuad(680, 130, 160, 160, -0.5f, rw::makeRGBA(60, 90, 230, 255), nil, true, true);
	drawQuad(720, 170, 160, 160,  0.5f, rw::makeRGBA(230, 140, 40, 255), nil, true, true);

	Scene.camera->endUpdate();

	if(!Scene.ranOnce){
		validateFramebuffer(sk::globals.width, sk::globals.height);
		printf("[rendertest] ==== %d passed, %d failed ====\n", Scene.passCount, Scene.failCount);
		Scene.ranOnce = true;
	}

	Scene.camera->showRaster(0);
}

void KeyDown(int key) {
	if(key == sk::KEY_ESC)
		sk::globals.quit = 1;
}

sk::EventStatus AppEventHandler(sk::Event e, void *param) {
	using namespace sk;
	Rect *r;

	switch(e){
	case INITIALIZE:
		Init();
		return EVENTPROCESSED;
	case RWINITIALIZE:
		return ::InitRW() ? EVENTPROCESSED : EVENTERROR;
	case PLUGINATTACH:
		return attachPlugins() ? EVENTPROCESSED : EVENTERROR;
	case KEYDOWN:
		KeyDown(*(int*)param);
		return EVENTPROCESSED;
	case RESIZE:
		r = (Rect*)param;
		if(r->w == 0) r->w = 1;
		if(r->h == 0) r->h = 1;
		sk::globals.width = r->w;
		sk::globals.height = r->h;
		if(Scene.camera)
			sk::CameraSize(Scene.camera, r);
		break;
	case IDLE:
		Draw(*(float*)param);
		return EVENTPROCESSED;
	default:
		break;
	}
	return sk::EVENTNOTPROCESSED;
}
