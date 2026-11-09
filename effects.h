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
	float d;      // the page's scroll offset in pixels, p * PAGE_H
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
// Motion Depth, after everything else. ax and ay are how far the Vita has turned
// about its own x axis (top edge towards you is positive) and y axis (right edge
// away from you is positive), in radians.
void depth_apply(Xform *t, float ax, float ay, int style, int strength);
// How far Float moves an icon for that tilt, in Paf's coordinates (y up),
// whatever the style: the shadows slide the other way.
void depth_float(float ax, float ay, int strength, float *dx, float *dy);
float fsin(float x);
float fcos(float x);
