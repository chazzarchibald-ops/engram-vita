// The animations. Each one is a pure function of the page's scroll progress, so
// it runs the same whether the page is dragged, flicked or snapping back, and
// every one is the identity at p = 0, which is where the page comes to rest.
//
// The effects are Engram's own maths, modelled on what Cylinder's effects do on
// iOS, turned 90 degrees because the Vita's home screen scrolls vertically.

#include "effects.h"

#define PI 3.14159265f
#define HALF_PI 1.57079633f
#define TWO_PI 6.28318531f

// The bubble grid's extent, for effects that depend on where an icon sits.
#define GRID_LEFT -336.0f
#define GRID_RIGHT 336.0f
#define GRID_TOP 158.0f
#define GRID_BOTTOM -150.0f

static inline float fabsf_(float x) {
	return x < 0 ? -x : x;
}
static inline float signf(float x) {
	return x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f);
}
static inline float clampf(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}
static inline float smooth(float x) {
	return x * x * (3.0f - 2.0f * x);
}

// No libm: a range-reduced Taylor series, accurate to about 1e-6.
float fsin(float x) {
	int turns = (int)(x / TWO_PI);
	x -= turns * TWO_PI;
	if (x > PI)
		x -= TWO_PI;
	else if (x < -PI)
		x += TWO_PI;
	if (x > HALF_PI)
		x = PI - x;
	else if (x < -HALF_PI)
		x = -PI - x;
	float x2 = x * x;
	return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
}

float fcos(float x) {
	return fsin(x + HALF_PI);
}

static uint32_t hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7FEB352D;
	x ^= x >> 15;
	x *= 0x846CA68B;
	x ^= x >> 16;
	return x;
}

static float unit(uint32_t h) {
	return (h & 0xFFFF) / 65535.0f;
}

void xform_identity(Xform *t, const IconCtx *c) {
	t->x = c->bx;
	t->y = c->by + c->d;
	t->sx = t->sy = 1.0f;
	t->rx = t->ry = t->rz = 0.0f;
	t->alpha = 1.0f;
}

// Icons leave in turn: k = 0 leads, k = 1 trails. Each icon's own progress q
// runs 0..1 as |p| does, but delayed by k, and arrives at 1 when |p| does.
static void stagger(Xform *t, const IconCtx *c, float k) {
	const float spread = 0.5f;
	float q = smooth(clampf((fabsf_(c->p) - k * spread) / (1.0f - spread), 0.0f, 1.0f));
	t->y += signf(c->p) * q * PAGE_H - c->d;
}

// Turns the page about a horizontal axis through (pivot_y, pivot_z) by phi, then
// projects it back onto the screen.
static void turn_page(Xform *t, const IconCtx *c, float pivot_y, float pivot_z, float phi) {
	float s = fsin(phi), co = fcos(phi);
	float yr = (t->y - c->d) - pivot_y;
	float zr = -pivot_z;
	float y2 = yr * co - zr * s;
	float z2 = yr * s + zr * co;
	float depth = pivot_z + z2; // towards the viewer is positive

	float view = PERSPECTIVE - depth;
	if (view < 200.0f)
		view = 200.0f;
	float scale = PERSPECTIVE / view;

	t->x *= scale;
	t->y = (pivot_y + y2) * scale;
	t->sx *= scale;
	t->sy *= scale;
	if (c->tilt)
		t->rx += phi;
	else
		t->sy *= fabsf_(co);
	// Only a page turned past edge-on is hidden (Card Flip's back). Cylinder's
	// cubes don't fade, and an edge-on icon is already invisible: turned on its
	// side with 3D Icon Tilt, or flattened to nothing without it.
	if (co < 0.0f)
		t->alpha = 0.0f;
}

void effect_apply(int effect, Xform *t, const IconCtx *c) {
	float p = c->p, ap = fabsf_(p);
	float xfrac = clampf((c->bx - GRID_LEFT) / (GRID_RIGHT - GRID_LEFT), 0.0f, 1.0f);
	float rowfrac = clampf((GRID_TOP - c->by) / (GRID_TOP - GRID_BOTTOM), 0.0f, 1.0f);

	switch (effect) {
	case FX_STAY_PUT: // hold still and cross-fade, so the pages never overlap at full strength
		t->y -= c->d;
		t->alpha *= 1.0f - ap;
		break;

	case FX_STAIRS:
		stagger(t, c, xfrac);
		break;

	case FX_CASCADE:
		stagger(t, c, clampf(0.65f * rowfrac + 0.35f * xfrac, 0.0f, 1.0f));
		break;

	case FX_WAVE:
		t->y += 70.0f * fsin(PI * ap) * fsin(TWO_PI * 1.25f * xfrac + 7.0f * ap);
		break;

	case FX_CHOMP:
		t->x += signf(c->bx) * ap * PAGE_W * 0.5f;
		break;

	case FX_VORTEX: {
		float angle = p * PI * 1.25f;
		float r = 1.0f - ap;
		float lx = t->x, ly = t->y - c->d;
		float s = fsin(angle), co = fcos(angle);
		t->x = (lx * co - ly * s) * r;
		t->y = (lx * s + ly * co) * r; // swirls about the screen's centre, not the page's
		t->sx *= r;
		t->sy *= r;
		t->rz += angle;
		break;
	}

	case FX_SCATTER: {
		uint32_t h = hash(c->seed);
		float dir = unit(h) * TWO_PI;
		float reach = (0.6f + 0.6f * unit(h >> 16)) * PAGE_W * ap;
		t->x += fcos(dir) * reach;
		t->y += fsin(dir) * reach - c->d;
		t->rz += (unit(hash(h)) - 0.5f) * 2.0f * PI * p;
		t->alpha *= 1.0f - ap;
		break;
	}

	case FX_CUBE_OUT:
		turn_page(t, c, 0.0f, -PAGE_H * 0.5f, -p * HALF_PI);
		break;

	case FX_CUBE_IN:
		turn_page(t, c, 0.0f, PAGE_H * 0.5f, p * HALF_PI);
		break;

	case FX_HINGE:
		turn_page(t, c, signf(p) * PAGE_H * 0.5f, 0.0f, p * HALF_PI);
		break;

	case FX_CARD:
		turn_page(t, c, 0.0f, 0.0f, p * PI);
		break;

	case FX_BLINDS: { // flips in place; past halfway the slat shows its back, so it's hidden
		float phi = p * PI;
		t->y -= c->d;
		if (c->tilt)
			t->rx += phi;
		else
			t->sy *= fabsf_(fcos(phi));
		if (ap > 0.5f)
			t->alpha = 0.0f;
		break;
	}

	case FX_SPIN:
		t->rz += p * TWO_PI;
		break;

	case FX_SHRINK:
		t->sx *= 1.0f - ap;
		t->sy *= 1.0f - ap;
		break;

	case FX_FADE:
		t->alpha *= 1.0f - ap;
		break;
	}
}

// During the Swipe: gone by 60% of the way, so the page has faded before it
// turns edge-on or leaves the screen. As the Next Page Lands: fully visible until
// 60% of the way, then gone exactly as the next page settles into place.
#define FADE_SPLIT 0.6f

float fade_out_alpha(float ap, int mode) {
	if (mode == FADE_OUT_DURING)
		return 1.0f - smooth(clampf(ap / FADE_SPLIT, 0.0f, 1.0f));
	if (mode == FADE_OUT_LANDING)
		return 1.0f - smooth(clampf((ap - FADE_SPLIT) / (1.0f - FADE_SPLIT), 0.0f, 1.0f));
	return 1.0f;
}

// Motion Depth treats the icons as a layer this far above the wallpaper. Seen
// from a fixed eye, a raised layer shifts towards whichever edge of the screen
// turns away from you, by height * tan(angle). Turned to face you, each icon
// turns back against the Vita's own turn.
#define DEPTH_PX 80.0f
#define DEPTH_MAX_TURN 0.6f

static float depth_factor(int strength) {
	static const float FACTORS[DEPTH_STRENGTH_COUNT] = {0.5f, 1.0f, 1.6f, 2.4f};
	return (strength >= 0 && strength < DEPTH_STRENGTH_COUNT) ? FACTORS[strength] : 1.0f;
}

void depth_float(float ax, float ay, int strength, float *dx, float *dy) {
	float k = depth_factor(strength);
	*dx = DEPTH_PX * k * fsin(ay) / fcos(ay);
	*dy = -DEPTH_PX * k * fsin(ax) / fcos(ax); // Paf's y points up, and the top edge came towards you
}

void depth_apply(Xform *t, float ax, float ay, int style, int strength) {
	float k = depth_factor(strength);
	if (style != DEPTH_TURN) {
		float dx, dy;
		depth_float(ax, ay, strength, &dx, &dy);
		t->x += dx;
		t->y += dy;
	}
	if (style != DEPTH_FLOAT) {
		t->rx -= clampf(ax * k, -DEPTH_MAX_TURN, DEPTH_MAX_TURN);
		t->ry -= clampf(ay * k, -DEPTH_MAX_TURN, DEPTH_MAX_TURN);
	}
}

float intensity_factor(int intensity) {
	static const float FACTORS[INTENSITY_COUNT] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
	if (intensity < 0 || intensity >= INTENSITY_COUNT)
		return 1.0f;
	return FACTORS[intensity];
}

void effect_intensity(Xform *t, const IconCtx *c, float k) {
	if (k == 1.0f)
		return;
	float rest_y = c->by + c->d;
	t->x = c->bx + (t->x - c->bx) * k;
	t->y = rest_y + (t->y - rest_y) * k;
	t->sx = 1.0f + (t->sx - 1.0f) * k;
	t->sy = 1.0f + (t->sy - 1.0f) * k;
	if (t->sx < 0.0f)
		t->sx = 0.0f;
	if (t->sy < 0.0f)
		t->sy = 0.0f;
	t->rx *= k;
	t->ry *= k;
	t->rz *= k;
	t->alpha = clampf(1.0f + (t->alpha - 1.0f) * k, 0.0f, 1.0f);
}
