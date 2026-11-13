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
	t->y += signf(c->p) * q * c->span - c->d;
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

	float eye = c->eye > 0.0f ? c->eye : PERSPECTIVE;
	float view = eye - depth;
	if (view < 200.0f)
		view = 200.0f;
	float scale = eye / view;

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
		t->x += signf(c->bx) * ap * c->across * 0.5f;
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
		float reach = (0.6f + 0.6f * unit(h >> 16)) * c->across * ap;
		t->x += fcos(dir) * reach;
		t->y += fsin(dir) * reach - c->d;
		t->rz += (unit(hash(h)) - 0.5f) * 2.0f * PI * p;
		t->alpha *= 1.0f - ap;
		break;
	}

	case FX_CUBE_OUT:
		turn_page(t, c, 0.0f, -c->span * 0.5f, -p * HALF_PI);
		break;

	case FX_CUBE_IN:
		turn_page(t, c, 0.0f, c->span * 0.5f, p * HALF_PI);
		break;

	case FX_HINGE:
		turn_page(t, c, signf(p) * c->span * 0.5f, 0.0f, p * HALF_PI);
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

// ap runs from 1 down to 0 as the page comes in.
float fade_in_alpha(float ap, int mode) {
	if (mode == FADE_OUT_DURING)
		return 1.0f - smooth(clampf((ap - (1.0f - FADE_SPLIT)) / FADE_SPLIT, 0.0f, 1.0f));
	if (mode == FADE_OUT_LANDING)
		return 1.0f - smooth(clampf(ap / FADE_SPLIT, 0.0f, 1.0f));
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

// ---- Icon Placement ----

// The grids icons rest on. A grid's slots are in reading order, and its frame
// (centre and reach) lets every layout fit any grid: the layouts are designed
// on the home screen's grid and scaled to the others.
struct Grid {
	const float (*slots)[2];
	int count;
	float cx, cy; // the middle of the grid
	float reach;  // how far the furthest slot is from the middle, across
};

// The home screen's 3-4-3 grid of 138 px bubbles (docs/theme-engine-design.md).
static const float HOME_SLOTS[10][2] = {
	{-229.0f, 158.0f}, {0.0f, 158.0f},    {229.0f, 158.0f},                    // top
	{-336.0f, 4.0f},   {-112.0f, 4.0f},   {112.0f, 4.0f},   {336.0f, 4.0f},   // middle
	{-229.0f, -150.0f}, {0.0f, -150.0f},  {229.0f, -150.0f},                   // bottom
};
// An open folder's 3-4-3 grid of 124 px slots, from the probe's tree (under
// 0x37677DEF). Not yet seen with a folder open: shell.cpp checks the icons are
// really on it, or on the home grid, before placing them.
static const float FOLDER_SLOTS[10][2] = {
	{-200.0f, 124.0f}, {0.0f, 124.0f},    {200.0f, 124.0f},
	{-300.0f, -16.0f}, {-100.0f, -16.0f}, {100.0f, -16.0f}, {300.0f, -16.0f},
	{-200.0f, -156.0f}, {0.0f, -156.0f},  {200.0f, -156.0f},
};
static const Grid GRIDS[GRID_COUNT] = {
	{HOME_SLOTS, 10, 0.0f, 4.0f, 336.0f},
	{FOLDER_SLOTS, 10, 0.0f, -16.0f, 300.0f},
};
#define HOME_REACH 336.0f    // the layouts' own scale: the home grid's
#define SLOT_TOLERANCE 20.0f // px an icon can be from its slot and still count as on it

static const Grid *grid_of(int grid) {
	return grid >= 0 && grid < GRID_COUNT ? &GRIDS[grid] : 0;
}

int grid_slot(int grid, float bx, float by) {
	const Grid *g = grid_of(grid);
	if (!g)
		return -1;
	for (int i = 0; i < g->count; i++) {
		float dx = bx - g->slots[i][0], dy = by - g->slots[i][1];
		if (dx * dx + dy * dy <= SLOT_TOLERANCE * SLOT_TOLERANCE)
			return i;
	}
	return -1;
}

bool grid_slot_pos(int grid, int slot, float *x, float *y) {
	const Grid *g = grid_of(grid);
	if (!g || slot < 0 || slot >= g->count)
		return false;
	*x = g->slots[slot][0];
	*y = g->slots[slot][1];
	return true;
}

static float fsqrt(float x) {
	if (x <= 0.0f)
		return 0.0f;
	float r = x > 1.0f ? x : 1.0f;
	for (int i = 0; i < 12; i++)
		r = 0.5f * (r + x / r);
	return r;
}

// Open Book: two pages of 5 icons, either side of a spine down the middle, the
// way you'd hold a book open to read it. Each page is laid out like the five on
// a die: two on top, one in the middle, two on the bottom. The spine is
// furthest away and each page comes towards you at its outer edge, so the icons
// nearest the spine are smaller, and every icon is turned to lie flat on its
// page. The grid's slots fill the book in reading order: the left page's five,
// then the right page's. In home grid units, centred on the home grid.
#define BOOK_EYE 700.0f       // a closer eye than the scroll effects', for more depth
#define BOOK_INNER 130.0f     // px from the spine to the inner column, laid flat
#define BOOK_MIDDLE 235.0f    // to the middle icon
#define BOOK_OUTER 340.0f     // and to the outer column
static const float BOOK_ROWS[3] = {154.0f, 0.0f, -154.0f}; // from the grid's middle
static const float BOOK_ANGLE[DEPTH_STRENGTH_COUNT] = {0.30f, 0.45f, 0.60f, 0.80f}; // each page's turn, radians
// A page's five places in reading order, as (row, column): column 0 is the
// page's left, 1 its middle, 2 its right.
static const int BOOK_PLACES[5][2] = {{0, 0}, {0, 2}, {1, 1}, {2, 0}, {2, 2}};

static void place_book(int strength, int slot, Placement *out) {
	float angle = BOOK_ANGLE[strength];
	float side = slot < 5 ? -1.0f : 1.0f;
	int row = BOOK_PLACES[slot % 5][0], col = BOOK_PLACES[slot % 5][1];
	bool outer = side < 0.0f ? col == 0 : col == 2;
	float u = col == 1 ? BOOK_MIDDLE : (outer ? BOOK_OUTER : BOOK_INNER);
	float s = fsin(angle), co = fcos(angle);
	// Laid on its page and turned with it: the outer edge sits on the screen's
	// plane, and the rest goes back from there.
	float z = (u - BOOK_OUTER) * s;
	float scale = BOOK_EYE / (BOOK_EYE - z);
	out->x = side * u * co * scale;
	out->y = BOOK_ROWS[row] * scale;
	out->s = scale;
	out->rx = 0.0f;
	out->ry = -side * angle; // the left page's right edge goes away from you, the right page's left edge
}

// Sphere: the page is the near side of a big ball, seen up close, like the
// Apple Watch's app grid. Each icon keeps its place on the grid, but that place
// is measured over the ball's surface from the middle, so icons further out
// shrink as the surface curves away, and turn to lie on it. Inverted, the page
// is the inside of the ball: the middle is furthest away and the icons get
// bigger towards the edges. In home grid units, from the grid's middle.
static const float BALL_RADIUS[DEPTH_STRENGTH_COUNT] = {900.0f, 620.0f, 460.0f, 360.0f};
static const float BALL_ZOOM[DEPTH_STRENGTH_COUNT] = {1.04f, 1.08f, 1.11f, 1.14f}; // the middle comes this close
#define BALL_EYE 0.6f      // the eye's distance from the surface, as a share of the radius
#define BOWL_BACK 0.75f    // how far back the inverted sphere's middle sits, as a share of its edge's depth

// How far from the middle an icon r from it on the grid is drawn, and its scale.
static float ball_project(int strength, bool inside, float r, float *scale_out) {
	float radius = BALL_RADIUS[strength];
	float eye = BALL_EYE * radius;
	float theta = r / radius; // how far round the ball the icon is
	float rho = radius * fsin(theta); // its distance from the middle, seen from in front
	float depth = radius * (1.0f - fcos(theta)); // how far the surface has curved away (or, inside, towards you)
	float scale;
	if (!inside) {
		scale = BALL_ZOOM[strength] * eye / (eye + depth);
	} else {
		float back = BOWL_BACK * radius * (1.0f - fcos(HOME_REACH / radius));
		scale = eye / (eye + back - depth);
	}
	*scale_out = scale;
	return rho * scale;
}

static void place_ball(int strength, bool inside, float dx, float dy, Placement *out) {
	float r = fsqrt(dx * dx + dy * dy);
	float scale;
	float drawn = ball_project(strength, inside, r, &scale);
	float k = r > 0.5f ? drawn / r : scale;
	float wide = 1.0f;
	if (!inside) {
		// As wide as the inverted sphere: the ball draws its outer icons in, so
		// they're spread back out sideways, most at the edges, least in the
		// middle, until the furthest slots land where the inverted sphere's do.
		float s_in, s_out;
		float stretch = ball_project(strength, true, HOME_REACH, &s_in) / ball_project(strength, false, HOME_REACH, &s_out);
		float f = dx / HOME_REACH;
		wide = 1.0f + (stretch - 1.0f) * f * f;
	}
	out->x = dx * k * wide;
	out->y = dy * k;
	out->s = scale;
	// Turned to lie on the surface: on the outside, an icon right of the middle
	// turns its right edge away and one above tips its top away; inside, the
	// other way.
	float theta = r / BALL_RADIUS[strength];
	float tx = r > 0.5f ? theta * dx / r : 0.0f;
	float ty = r > 0.5f ? theta * dy / r : 0.0f;
	float sign = inside ? -1.0f : 1.0f;
	out->rx = -sign * ty;
	out->ry = sign * tx;
}

bool place_icon(int grid, int layout, int strength, float bx, float by, Placement *out) {
	const Grid *g = grid_of(grid);
	if (!g)
		return false;
	if (strength < 0 || strength >= DEPTH_STRENGTH_COUNT)
		strength = DEPTH_NORMAL;
	// To home grid units, from the grid's middle, and back afterwards.
	float unit = g->reach / HOME_REACH;
	float dx = (bx - g->cx) / unit, dy = (by - g->cy) / unit;
	Placement p;
	switch (layout) {
	case LAYOUT_BOOK: {
		int slot = grid_slot(grid, bx, by);
		if (slot < 0)
			return false;
		place_book(strength, slot, &p);
		break;
	}
	case LAYOUT_SPHERE:
		place_ball(strength, false, dx, dy, &p);
		break;
	case LAYOUT_SPHERE_INSIDE:
		place_ball(strength, true, dx, dy, &p);
		break;
	default:
		return false;
	}
	out->x = g->cx + p.x * unit;
	out->y = g->cy + p.y * unit;
	out->s = p.s;
	out->rx = p.rx;
	out->ry = p.ry;
	return true;
}
