#pragma once

#include "engram.h"

// The home screen's geometry, from the probe (docs/theme-engine-design.md).
// Paf's y axis points up, and positions are relative to the page's centre.
#define PAGE_W 960.0f
#define PAGE_H 512.0f
#define PERSPECTIVE 1000.0f // eye distance for the 3D effects

// What an effect does to one icon. x and y are in screen space: the page's
// centre is at (0, d) while it scrolls, so an untouched icon sits at (bx, by + d).
struct Xform {
	float x, y;
	float sx, sy; // scale, multiplied into the icon's own
	float rx, ry, rz; // radians, added to the icon's own rotation
	float alpha;  // multiplied into the icon's own alpha
};

struct IconCtx {
	float bx, by; // the icon's resting position on its page
	float p;      // page progress: 0 at rest, +1 a whole page above the screen, -1 below
	float d;      // the page's scroll offset in pixels, p * span
	float span;   // a page's length along the scroll: PAGE_H on the home screen
	float across; // and across it: PAGE_W
	float eye;    // the eye's distance for the 3D effects: PERSPECTIVE on the home screen
	uint32_t seed;
	bool tilt;
};

void xform_identity(Xform *t, const IconCtx *c);
void effect_apply(int effect, Xform *t, const IconCtx *c);
// Scales every change from the resting state by k (0.5 to 1.5).
void effect_intensity(Xform *t, const IconCtx *c, float k);
float intensity_factor(int intensity);
// Alpha for the page you swipe away from, by how far it has gone (|p|) and the
// Fade Out Previous Page mode (enum FadeOut).
float fade_out_alpha(float ap, int mode);
// The same for the page you swipe to, fading in: During the Swipe, fully in by
// 60% of the way; As It Lands, coming in over the last 60% of the way.
float fade_in_alpha(float ap, int mode);
// Motion Depth, after everything else. ax and ay are how far the Vita has turned
// about its own x axis (top edge towards you is positive) and y axis (right edge
// away from you is positive), in radians.
void depth_apply(Xform *t, float ax, float ay, int style, int strength);
// How far Float moves an icon for that tilt, in Paf's coordinates (y up),
// whatever the style: the shadows slide the other way.
void depth_float(float ax, float ay, int strength, float *dx, float *dy);
float fsin(float x);
float fcos(float x);

// Icon Placement: where the icons rest instead of the Vita's grid. A layout
// maps an icon's place in the grid to its place in the layout, with the scale
// and turn that make it look 3D. Scroll Animations and Motion Depth then work
// from there, as they would from the grid.
struct Placement {
	float x, y; // the icon's new resting position
	float s;    // scale, multiplied into the icon's own
	float rx, ry; // radians, added to the icon's own rotation
};
enum GridKind {
	GRID_HOME,   // a home screen page: 138 px bubbles, 3-4-3
	GRID_FOLDER, // an open folder's 124 px slots, 3-4-3, as the probe found them
	GRID_COUNT
};
// The grid slot (0 to 9 in reading order: 3 on top, 4 in the middle, 3 on the
// bottom) that (bx, by) is on, or -1 if it isn't on one.
int grid_slot(int grid, float bx, float by);
// Where a grid slot is. False if there's no such slot.
bool grid_slot_pos(int grid, int slot, float *x, float *y);
// Where the icon resting at (bx, by) on the grid goes in the layout (enum
// Layout), at the strength (enum DepthStrength). False if it can't be placed.
bool place_icon(int grid, int layout, int strength, float bx, float by, Placement *out);
