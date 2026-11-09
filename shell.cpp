// The SceShell half of Engram: animates the home screen's bubbles while the
// pages scroll (Scroll Animations), and floats them in 3D as the Vita tilts
// (Motion Depth).
//
// The probe and move test (docs/theme-engine-design.md) found that:
//   - page_top holds a scroll box (0x0E61B084) whose first child is a plane that
//     moves as you scroll. The plane's children are the pages, 512 px apart, so
//     a page's offset from the screen is plane.y + page.y.
//   - each page's bubbles (138 x 138) live in a container named 0xC6063E76.
//   - SetPos on a bubble works, but SceShell recreates bubbles as you scroll and
//     animates them itself when folders open and close.
// So Engram never stores anything about a bubble beyond the current gesture.
// Each frame it looks the bubbles up again, remembers what each one looked like
// before it touched it, notices when SceShell has changed a bubble since (and
// adopts that as the new resting state), and puts every bubble back exactly when
// its page comes to rest, or the Vita has stopped turning. With both still,
// Engram doesn't touch anything.

#include <limits> // paf/std/vector uses it without including it

#define private public
#define protected public
#include <paf.h>
#undef private
#undef protected

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/motion.h>
#include <stdarg.h>
#include <taihen.h>

#include "effects.h"
#include "engram.h"

using namespace paf;

#define NID_SCESHELL_365 0x5549BF1F

// SceShell calls this on its main thread once Paf is up; shelltheme starts the
// same way. Registering from there, rather than from our own thread as soon as
// Paf's stubs resolve, keeps the registration off a list Paf may not have set up.
#define LIB_SCEAVCONFIG 0x79E0F03F
#define NID_SCEAVCONFIGREGISTERCALLBACK 0xFB5E3E74

// taiHEN's TAI_CONTINUE doesn't compile as C++.
#define TAI_NEXT(this_func, hook, ...)                                                                        \
	({                                                                                                        \
		struct _tai_hook_user *cur = (struct _tai_hook_user *)(hook), *next;                                  \
		next = (struct _tai_hook_user *)cur->next;                                                            \
		(next == NULL) ? ((__typeof__(&this_func))cur->old)(__VA_ARGS__)                                      \
		               : ((__typeof__(&this_func))next->func)(__VA_ARGS__);                                   \
	})

#define HOME_PAGE 0x5E9535B3        // sha1("page_top"), topmenu_plugin's home screen
#define SCROLL_BOX 0x0E61B084       // holds the plane the pages sit on
#define BUBBLE_CONTAINER 0xC6063E76 // the parent of one page's bubbles
#define BUBBLE_SIZE 138

#define MAX_PAGES 64          // pages in a plugin's draw order
#define MAX_HOME_PAGES 512    // pages on the home screen (LiveArea NoLimits raises the limit)
#define MAX_CHILDREN 64
#define MAX_DEPTH 16
#define MAX_TRACKS 96         // bubbles changed at once: about three pages' worth
#define EVICT_FRAMES 30       // forget a bubble not seen in the tree for this long

#define STABLE_US (30 * 1000 * 1000)
#define CONFIG_POLL_US (1000 * 1000)

#define MIN_ALPHA 0.01f
#define REST_EPSILON 0.5f
#define SAME_EPSILON 0.01f

// Motion Depth. The gyroscope's turn is summed into a tilt that leaks back to
// zero, so the icons respond to turning the Vita and drift home when it's held
// still at any angle, as iOS's parallax does.
#define MOTION_TAU 1.5f      // seconds for a held tilt to drift back to the middle
#define MOTION_SMOOTH 10.0f  // per second: how quickly the icons follow the tilt
#define GYRO_DEADBAND 0.02f  // rad/s of turn ignored as noise
#define MAX_TILT 0.45f       // radians
#define DEPTH_REST 0.003f    // a tilt smaller than this puts the icons back exactly
#define MOTION_LOG_LIMIT 6
#define DEPTH_HOLD_FRAMES 90 // after the home page appears, while SceShell brings its icons in
#define BUSY_LOG_LIMIT 8
#define VISIBLE_LOG_LIMIT 8

// Shadows. Each page gets one layer of Engram's own, a sibling of the bubble
// container placed just behind it, holding one shadow per bubble. Nothing is
// added to the container itself, in case SceShell counts its children. Engram
// keeps no pointers to these widgets: each frame it finds them again by id
// under the pages that are on screen, so a page SceShell frees takes its
// shadows with it and Engram never sees them again.
#define SHADOW_LAYER_ID 0x45475348   // Engram's own ids, made up ("EGSH", "EGSS")
#define SHADOW_ID 0x45475353
#define STYLE_PLAIN_PLANE 0x03C69492 // topmenu_plugin's untextured white style_plane
#define SHADOW_TEX 64                // the shadow texture, drawn once and kept for good
#define SHADOW_SIZE 132.0f           // a bubble is 138, its round icon about 128
#define SHADOW_SOLID 0.88f           // solid out to here, as a share of the radius
#define SHADOW_ALPHA 0.5f
#define SHADOW_DROP 10.0f            // px below the icon at rest, as if lit from above
#define SHADOW_PARALLAX BG_PARALLAX  // the shadow lies on the wallpaper, so it slides with it
#define SHADOW_CREATE_PER_FRAME 4
#define SHADOW_FAIL_LIMIT 4
#define SHADOW_FADE_STEP 0.05f       // per frame: shadows fade in over a third of a second
#define SHADOW_LOG_LIMIT 6
#define ALPHA_CACHE 64

// Wallpaper Parallax. A theme's wallpaper is a plane behind each page's bubbles
// (0xF1B811BF, beside the bubble container). It's zoomed a little so that it can
// slide the other way without showing its edges. The Vita's own wave background
// isn't a widget: Paf's graphics code draws it, so it doesn't move.
#define BG_ID 0xF1B811BF
#define BG_ZOOM 1.1f
#define BG_PARALLAX 0.5f   // the wallpaper slides the other way, this much of the icons' float
#define BG_LOG_LIMIT 4

#define DIAG_PER_KIND 4 // log lines about SceShell's own changes, per kind, per boot

// What Engram knows about a bubble during one gesture.
//
// The base is what SceShell had when the gesture started, and anything SceShell
// writes during it becomes the new base. On console SceShell didn't write scale,
// rotation or alpha mid-scroll, but a test build that handed a property back to
// SceShell instead broke Spin and the 3D tilt, because some bubbles (likely
// folders) rest at 98% scale and were never taken.
//
// When a page settles, position, scale and rotation go back to their base, and
// alpha goes back to fully opaque. Alpha is forced rather than restored: an
// early build restored an alpha captured mid-fade and left icons half
// transparent. Engram also never takes alpha all the way to 0 (see MIN_ALPHA).
struct Track {
	ui::Widget *widget;
	float base_pos[4], base_scale[4], base_rot[4];
	float last_pos[4], last_scale[4], last_rot[4];
	float alpha; // what Engram last set, valid while color_touched
	uint32_t seen;
	bool modified;
	bool scale_touched, rot_touched, color_touched;
};

static Track tracks[MAX_TRACKS];
static int track_count;

static Config cfg;          // the main thread's copy
static Config pending_cfg;  // handed over by the poll thread
static volatile int cfg_pending;

static SceUID poll_thread = -1;
static volatile int poll_running;
static volatile int frame_registered;
static volatile int register_started;
static SceUID avconfig_hook = -1;
static tai_hook_ref_t avconfig_ref;
static volatile int stop_requested;
static volatile SceUInt64 first_frame_at;

static uint32_t frame;
static ui::Scene *home_scene;
static ui::Widget *scroll_box;
static bool was_moving;
static int random_effect = -1;
static uint32_t rng_state;
static bool rot_logged;
static bool color_logged;
static ui::Widget *origin_page; // the page this gesture started on; compared, never dereferenced
static int gestures_logged;
static bool tree_logged;
static int visible_last = -1, visible_logs;

static bool motion_sampling, motion_we_started;
static uint32_t motion_retry_at;
static SceUInt64 motion_last_us;
static float tilt_x, tilt_y;   // radians: about the Vita's x axis (top edge towards you) and y axis
static float shown_x, shown_y; // the tilt the icons show, smoothed
static int motion_logs;
static uint32_t motion_logged_at;
static uint32_t depth_hold_until;
static int busy_logs;

static graph::Surface *shadow_tex;
static bool shadow_tex_failed;
static int shadow_failures;
static bool shadows_made;  // some page has a shadow layer, so turning Shadows off has to hide it
static float shadow_master; // 0..1: all shadows fade in and out together
static int shadow_logs;
static float wall_master; // 0..1: the wallpaper's zoom and slide fade in and out together
static int bg_logs;
static uint32_t shadow_pixels[SHADOW_TEX * SHADOW_TEX];
static struct {
	ui::Widget *widget; // compared, never dereferenced
	float alpha;
} alpha_cache[ALPHA_CACHE];
static int alpha_cache_next;

// What Paf's intrusive_ptr<Surface> is: one pointer. Engram builds its own so it
// never calls AddRef or Release behind its own back.
struct SurfaceRef {
	graph::Surface *ptr;
};

// Called by symbol, with Engram's own types, so nothing depends on the headers'
// vtable order or class sizes. The stubs are in paf_stubs.S.
extern "C" void *paf_surface_ctor(void *self, graph::SurfacePool *pool, int width, int height, int mode, int order,
                                  int num_page, int num_level, bool unk)
	asm("_ZN3paf5graph7SurfaceC1EPNS0_11SurfacePoolEii9ImageMode10ImageOrderiib");
extern "C" int paf_surface_copy(void *self, int page, const void *src, int order, int line_bytes, int level)
	asm("_ZN3paf5graph7Surface4CopyEiPKv10ImageOrderii");
extern "C" int paf_surface_add_ref(void *self) asm("_ZN3paf5graph7Surface6AddRefEv");
extern "C" graph::SurfacePool *paf_surface_pool_get(int index) asm("_ZN3paf5graph11SurfacePool7GetPoolEi");
extern "C" int paf_plane_set_texture(ui::Widget *self, const SurfaceRef *surf, int obj, unsigned index)
	asm("_ZN3paf2ui5Plane10SetTextureERKNS_13intrusive_ptrINS_5graph7SurfaceEEEij");
extern "C" void *paf_new(unsigned size) asm("_Znwj");
// Moves self to just before target among their parent's children, there and
// then. The public SetBehind only queues the same move (Paf's code posts
// {parent, self, target} to a request list), and in 1.2 and 1.3 the shadows
// stayed on top. The header has this one private, so it's called by symbol.
extern "C" int paf_widget_set_behind_now(ui::Widget *self, ui::Widget *target)
	asm("_ZN3paf2ui6Widget14SetBehind_coreEPS1_");
// Moves self to the end of its parent's children, so it's drawn last.
extern "C" int paf_widget_set_forward_most_now(ui::Widget *self) asm("_ZN3paf2ui6Widget19SetForwardMost_coreEv");
#define WIDGET_PARENT_OFFSET 0x168 // where Paf's own code reads a widget's parent
#define SURFACE_ALLOC 0x400 // more than a Surface needs, whatever this firmware's layout

// ---- helpers ----

static bool looks_like_object(const void *ptr) {
	uintptr_t value = (uintptr_t)ptr;
	return value >= 0x80000000 && value < 0xF0000000 && (value & 3) == 0;
}

static uint32_t id_of(ui::Widget *widget) {
	return widget->GetName().GetIDHash();
}

static float *pos_of(ui::Widget *w) {
	return (float *)&w->m_pos;
}
static float *scale_of(ui::Widget *w) {
	return (float *)&w->m_scale;
}
static float *rot_of(ui::Widget *w) {
	return (float *)&w->m_rot;
}
static float *size_of(ui::Widget *w) {
	return (float *)&w->m_size;
}

static inline float fabsf_(float x) {
	return x < 0 ? -x : x;
}

static bool same3(const float *a, const float *b) {
	return fabsf_(a[0] - b[0]) < SAME_EPSILON && fabsf_(a[1] - b[1]) < SAME_EPSILON && fabsf_(a[2] - b[2]) < SAME_EPSILON;
}

static void copy4(float *dst, const float *src) {
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
	dst[3] = src[3];
}

static uint32_t next_random(void) {
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static void set_pos(ui::Widget *w, const float *v) {
	w->SetPos(math::v4(v[0], v[1], v[2], v[3]));
}
static void set_scale(ui::Widget *w, const float *v) {
	w->SetScale(math::v4(v[0], v[1], v[2], v[3]));
}
static void set_rot(ui::Widget *w, const float *v) {
	w->SetRot(math::v4(v[0], v[1], v[2], v[3]));
}
// White at the given alpha. On console, the header's m_color never changed after
// SetColor (it always read 0xFF alpha), so Engram never reads a bubble's colour:
// it remembers the alpha it set itself, and bubbles are assumed untinted.
static void set_alpha(ui::Widget *w, float a) {
	uint32_t before = w->m_color;
	w->SetColor(math::v4(1.0f, 1.0f, 1.0f, a));
	if (!color_logged) {
		color_logged = true;
		log_line("first SetColor: alpha %d%%, header m_color 0x%08X -> 0x%08X\n", (int)(a * 100), (unsigned)before,
		         (unsigned)w->m_color);
	}
}

// ---- finding the home screen ----

static ui::Scene *find_home_scene(void) {
	Plugin *plugin = Plugin::Find("topmenu_plugin");
	if (!looks_like_object(plugin))
		return NULL;

	// The probe confirmed the firmware stores this vector as (data, size, capacity).
	const uint32_t *words = (const uint32_t *)&plugin->draw_order;
	DrawOrderElement *pages = (DrawOrderElement *)words[0];
	uint32_t count = words[1];
	if (!looks_like_object(pages) || count > MAX_PAGES)
		return NULL;
	for (uint32_t i = 0; i < count; i++) {
		ui::Scene *scene = pages[i].page;
		if (looks_like_object(scene) && id_of(scene) == HOME_PAGE)
			return scene;
	}
	return NULL;
}

static ui::Widget *find_widget(ui::Widget *widget, uint32_t id, int depth) {
	if (!looks_like_object(widget) || depth > MAX_DEPTH)
		return NULL;
	if (id_of(widget) == id)
		return widget;

	int children = widget->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return NULL;
	for (int i = 0; i < children; i++) {
		ui::Widget *found = find_widget(widget->GetChild(i), id, depth + 1);
		if (found)
			return found;
	}
	return NULL;
}

// page -> its one child -> the bubble container among that child's children.
static ui::Widget *bubble_container(ui::Widget *page) {
	if (page->GetChildrenNum() < 1)
		return NULL;
	ui::Widget *inner = page->GetChild(0);
	if (!looks_like_object(inner))
		return NULL;
	int children = inner->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return NULL;
	for (int i = 0; i < children; i++) {
		ui::Widget *child = inner->GetChild(i);
		if (looks_like_object(child) && id_of(child) == BUBBLE_CONTAINER)
			return child;
	}
	return NULL;
}

static bool is_bubble(ui::Widget *w) {
	if (!looks_like_object(w))
		return false;
	const float *size = size_of(w);
	return (int)size[0] == BUBBLE_SIZE && (int)size[1] == BUBBLE_SIZE;
}

// ---- remembering bubbles ----

static Track *find_track(ui::Widget *w) {
	for (int i = 0; i < track_count; i++)
		if (tracks[i].widget == w)
			return &tracks[i];
	return NULL;
}

static Track *add_track(ui::Widget *w) {
	if (track_count >= MAX_TRACKS)
		return NULL;
	Track *t = &tracks[track_count++];
	sceClibMemset(t, 0, sizeof(*t));
	t->widget = w;
	return t;
}

#define RESTORE_LOG_LIMIT 6
static int restore_logged;

enum DiagKind { DIAG_FIRST, DIAG_SCALE, DIAG_ROT, DIAG_KINDS };
static int diag_counts[DIAG_KINDS];

static void diag(Track *t, int kind, const char *what) {
	if (diag_counts[kind] >= DIAG_PER_KIND)
		return;
	diag_counts[kind]++;
	ui::Widget *w = t->widget;
	const float *s = scale_of(w), *r = rot_of(w);
	log_line("bubble %p %s: scale=(%d,%d)%% rot=(%d,%d) mrad\n", w, what, (int)(s[0] * 100), (int)(s[1] * 100),
	         (int)(r[0] * 1000), (int)(r[2] * 1000));
}

static bool at_rest_scale(const float *s) {
	return fabsf_(s[0] - 1.0f) < SAME_EPSILON && fabsf_(s[1] - 1.0f) < SAME_EPSILON;
}

// Brings the track up to date with the bubble before Engram changes it: on the
// first frame of a gesture, and whenever SceShell has written something other
// than what Engram last set, its value becomes the new base.
static void adopt_changes(Track *t) {
	ui::Widget *w = t->widget;
	bool first = !t->modified;

	if (first) {
		t->scale_touched = t->rot_touched = t->color_touched = false;
		if (!at_rest_scale(scale_of(w)))
			diag(t, DIAG_FIRST, "still animating when its page started moving");
	}

	if (first || !same3(pos_of(w), t->last_pos))
		copy4(t->base_pos, pos_of(w));
	if (first || !same3(scale_of(w), t->last_scale)) {
		if (!first)
			diag(t, DIAG_SCALE, "scale changed by SceShell mid-scroll");
		copy4(t->base_scale, scale_of(w));
	}
	if (first || !same3(rot_of(w), t->last_rot)) {
		if (!first)
			diag(t, DIAG_ROT, "rotation changed by SceShell mid-scroll");
		copy4(t->base_rot, rot_of(w));
	}
}

static void remember_applied(Track *t) {
	ui::Widget *w = t->widget;
	copy4(t->last_pos, pos_of(w));
	copy4(t->last_scale, scale_of(w));
	copy4(t->last_rot, rot_of(w));
	t->modified = true;
}

// Which of SceShell's own animations are running on a bubble. Setting a property
// without a timer replaces the animation of that property: in 1.1, Motion Depth
// moved the first page's bubbles while SceShell was bringing them in after boot,
// which left most of them invisible and one stranded near the top.
enum Busy { BUSY_POS = 1, BUSY_SCALE = 2, BUSY_ROT = 4, BUSY_COLOR = 8 };

static uint32_t busy_mask(ui::Widget *w) {
	uint32_t mask = 0;
	if (w->FindAnim(ui::Widget::ANIM_MOVE))
		mask |= BUSY_POS;
	if (w->FindAnim(ui::Widget::ANIM_SCALE))
		mask |= BUSY_SCALE;
	if (w->FindAnim(ui::Widget::ANIM_ROT) || w->FindAnim(ui::Widget::ANIM_QUAT))
		mask |= BUSY_ROT;
	if (w->FindAnim(ui::Widget::ANIM_COLOR) || w->FindAnim(ui::Widget::ANIM_SHOW_ALPHA) ||
	    w->FindAnim(ui::Widget::ANIM_META_ALPHA))
		mask |= BUSY_COLOR;
	return mask;
}

// Puts the bubble back how it was. Position goes back to its base unless SceShell
// has moved it since (a folder, say). Scale and rotation Engram touched go back
// to their base, and alpha it touched goes back to fully opaque. Properties in
// `busy` are being animated by SceShell, so they're left to it.
static void restore(Track *t, uint32_t busy = 0) {
	ui::Widget *w = t->widget;
	if (!(busy & BUSY_POS) && same3(pos_of(w), t->last_pos) && !same3(pos_of(w), t->base_pos))
		set_pos(w, t->base_pos);

	// Scale and rotation go back to SceShell's own values: some bubbles (four on
	// the test console, likely folders) rest at 98% scale, and the logs showed
	// SceShell never changes either mid-scroll.
	if (!(busy & BUSY_SCALE) && t->scale_touched && !same3(scale_of(w), t->base_scale))
		set_scale(w, t->base_scale);
	if (!(busy & BUSY_ROT) && t->rot_touched && !same3(rot_of(w), t->base_rot))
		set_rot(w, t->base_rot);
	// Always, whatever the bubble reads as: see set_alpha.
	if (!(busy & BUSY_COLOR) && t->color_touched) {
		set_alpha(w, 1.0f);
		if (restore_logged < RESTORE_LOG_LIMIT) {
			restore_logged++;
			log_line("restored alpha of %p from %d%%, visible=%d\n", w, (int)(t->alpha * 100), w->IsVisible() ? 1 : 0);
		}
	}

	t->modified = false;
	t->scale_touched = t->rot_touched = t->color_touched = false;
}

static void restore_all_seen(void) {
	for (int i = 0; i < track_count; i++)
		if (tracks[i].modified && tracks[i].seen == frame)
			restore(&tracks[i]);
}

// Forgets bubbles that weren't in the tree this frame. They may have been freed,
// so they are never touched again.
static void evict_tracks(void) {
	int kept = 0;
	for (int i = 0; i < track_count; i++) {
		if (frame - tracks[i].seen > EVICT_FRAMES)
			continue;
		if (kept != i)
			tracks[kept] = tracks[i];
		kept++;
	}
	track_count = kept;
}

static int modified_count(void) {
	int n = 0;
	for (int i = 0; i < track_count; i++)
		if (tracks[i].modified)
			n++;
	return n;
}

// ---- animating ----

// Logs one bubble's children once, to see what a bubble is built from (for
// shadows, say, a later build would need a layer behind the icon).
static void log_tree(ui::Widget *w, int depth) {
	if (!looks_like_object(w) || depth > 2)
		return;
	int children = w->GetChildrenNum();
	const float *pos = pos_of(w), *size = size_of(w);
	log_line("  depth %d: %08X size %dx%d pos (%d,%d,%d) %d children\n", depth, (unsigned)id_of(w), (int)size[0],
	         (int)size[1], (int)pos[0], (int)pos[1], (int)pos[2], children);
	if (children < 0 || children > 16)
		return;
	for (int i = 0; i < children; i++)
		log_tree(w->GetChild(i), depth + 1);
}

static void animate(ui::Widget *w, float d, const int *effects, int effect_count, bool fading, bool tilting) {
	Track *t = find_track(w);
	if (!t)
		t = add_track(w);
	if (!t)
		return;
	t->seen = frame;

	// A page that is only tilting waits for SceShell to finish animating its
	// bubbles. Scrolling pages don't: Scroll Animations has always moved them
	// regardless, and works.
	if (tilting && effect_count == 0 && !fading) {
		uint32_t busy = busy_mask(w);
		if (busy) {
			if (busy_logs < BUSY_LOG_LIMIT) {
				busy_logs++;
				log_line("bubble %p busy (0x%X) at frame %u: Motion Depth waits\n", w, (unsigned)busy, (unsigned)frame);
			}
			// Alpha comes back regardless: a fade left by Scroll Animations must
			// never stay, whatever SceShell is doing.
			if (t->modified)
				restore(t, busy & ~BUSY_COLOR);
			return;
		}
	}
	adopt_changes(t);
	if (!tree_logged) {
		tree_logged = true;
		log_line("a bubble's tree:\n");
		log_tree(w, 0);
	}

	IconCtx c;
	c.bx = t->base_pos[0];
	c.by = t->base_pos[1];
	c.d = d;
	c.p = d / PAGE_H;
	c.seed = (uint32_t)((int)c.bx * 7919 + (int)c.by * 104729);
	c.tilt = cfg.tilt != 0;

	Xform x;
	xform_identity(&x, &c);
	for (int i = 0; i < effect_count; i++)
		effect_apply(effects[i], &x, &c);
	effect_intensity(&x, &c, intensity_factor(cfg.intensity));
	if (fading) // after Intensity, so the page always fades out completely
		x.alpha *= fade_out_alpha(fabsf_(c.p), cfg.fade_out);
	if (tilting) // after Intensity too: it has its own Strength
		depth_apply(&x, shown_x, shown_y, cfg.depth_style, cfg.depth_strength);

	float pos[4] = {x.x, x.y - d, t->base_pos[2], t->base_pos[3]};
	float scale[4] = {t->base_scale[0] * x.sx, t->base_scale[1] * x.sy, t->base_scale[2], t->base_scale[3]};
	float rot[4] = {t->base_rot[0] + x.rx, t->base_rot[1] + x.ry, t->base_rot[2] + x.rz, t->base_rot[3]};

	if (!same3(pos_of(w), pos))
		set_pos(w, pos);
	if (!same3(scale_of(w), scale)) {
		set_scale(w, scale);
		t->scale_touched = true;
	}
	if (!same3(rot_of(w), rot)) {
		set_rot(w, rot);
		t->rot_touched = true;
		if (!rot_logged) {
			rot_logged = true;
			const float *now = rot_of(w);
			log_line("first SetRot: asked (%d,%d,%d) mrad, widget now (%d,%d,%d) mrad\n", (int)(rot[0] * 1000),
			         (int)(rot[1] * 1000), (int)(rot[2] * 1000), (int)(now[0] * 1000), (int)(now[1] * 1000),
			         (int)(now[2] * 1000));
		}
	}
	// Alpha is only touched when an effect fades, so SceShell's own fade-in of a
	// page it has just built is left alone. It's absolute, not relative to the
	// bubble's current alpha, which may be partway through that fade.
	if (x.alpha < 0.999f || t->color_touched) {
		// Never fully transparent, as a precaution. (Icons that vanished for good
		// after Card Flip, Blinds and Fade Out Previous Page were never restored
		// at all: Engram read their alpha from m_color, which never changes.)
		float a = x.alpha < MIN_ALPHA ? MIN_ALPHA : (x.alpha > 1.0f ? 1.0f : x.alpha);
		if (!t->color_touched || fabsf_(a - t->alpha) > 0.002f) {
			set_alpha(w, a);
			t->alpha = a;
		}
		t->color_touched = true;
	}
	remember_applied(t);
}

static void settle(ui::Widget *w) {
	Track *t = find_track(w);
	if (!t)
		return;
	t->seen = frame;
	if (t->modified)
		restore(t);
}

// Picks the effects for this frame. Returns how many.
static int choose_effects(int *effects, bool gesture_started) {
	int chosen[FX_COUNT];
	int count = 0;
	for (int i = 0; i < FX_COUNT; i++)
		if (cfg.fx[i])
			chosen[count++] = i;
	if (count == 0)
		return 0;

	if (!cfg.random) {
		for (int i = 0; i < count; i++)
			effects[i] = chosen[i];
		return count;
	}

	// One effect per gesture, so it doesn't change halfway through a scroll.
	bool still_chosen = random_effect >= 0 && cfg.fx[random_effect];
	if (gesture_started || !still_chosen)
		random_effect = chosen[next_random() % count];
	effects[0] = random_effect;
	return 1;
}

// ---- Shadows ----

static void log_shadow(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_shadow(const char *fmt, ...) {
	if (shadow_logs >= SHADOW_LOG_LIMIT)
		return;
	shadow_logs++;
	char line[200];
	va_list args;
	va_start(args, fmt);
	sceClibVsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	log_line("shadows: %s\n", line);
}

static void shadow_failed(const char *what) {
	shadow_failures++;
	log_line("shadows: %s failed (%d of %d)\n", what, shadow_failures, SHADOW_FAIL_LIMIT);
	if (shadow_failures >= SHADOW_FAIL_LIMIT)
		log_line("shadows: giving up for this boot\n");
}

// A round shadow with a crisp edge: solid out to SHADOW_SOLID of the radius,
// then a short fall-off so the circle isn't jagged. Every byte of a pixel gets
// the same value, so whichever byte this format keeps alpha in, tinting the
// plane black gives black at that alpha.
static void draw_shadow_pixels(void) {
	const float half = SHADOW_TEX * 0.5f;
	const float solid2 = SHADOW_SOLID * SHADOW_SOLID;
	for (int y = 0; y < SHADOW_TEX; y++) {
		for (int x = 0; x < SHADOW_TEX; x++) {
			float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
			float r2 = dx * dx + dy * dy; // 0 in the middle, 1 at the edge
			float t = (r2 - solid2) / (1.0f - solid2);
			t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
			float a = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep down to 0
			uint32_t v = (uint32_t)(a * 255.0f + 0.5f);
			shadow_pixels[y * SHADOW_TEX + x] = v | (v << 8) | (v << 16) | (v << 24);
		}
	}
}

static graph::Surface *shadow_texture(void) {
	if (shadow_tex || shadow_tex_failed)
		return shadow_tex;
	shadow_tex_failed = true; // until it works

	Plugin *plugin = Plugin::Find("topmenu_plugin");
	graph::SurfacePool *pool = looks_like_object(plugin) ? plugin->surface_pool : NULL;
	if (!looks_like_object(pool))
		pool = paf_surface_pool_get(0);
	if (!looks_like_object(pool)) {
		shadow_failed("finding a surface pool");
		return NULL;
	}

	draw_shadow_pixels();
	void *mem = paf_new(SURFACE_ALLOC);
	if (!looks_like_object(mem)) {
		shadow_failed("allocating the texture");
		return NULL;
	}
	sceClibMemset(mem, 0, SURFACE_ALLOC);
	paf_surface_ctor(mem, pool, SHADOW_TEX, SHADOW_TEX, ImageMode_RGBA8888, ImageOrder_Linear, 1, 1, false);
	// Engram's own reference, never given back: shadows come and go with their
	// pages, and the texture must outlive every one of them.
	paf_surface_add_ref(mem);
	int ret = paf_surface_copy(mem, 0, shadow_pixels, ImageOrder_Linear, SHADOW_TEX * 4, 0);
	log_line("shadows: texture %p from pool %p (topmenu_plugin's %p), copy 0x%08X\n", mem, pool,
	         looks_like_object(plugin) ? plugin->surface_pool : NULL, ret);
	shadow_tex = (graph::Surface *)mem;
	shadow_tex_failed = false;
	return shadow_tex;
}

// Alpha only when it changes: m_color can't be read back (see set_alpha). A new
// shadow is always set (`force`), since a freed one may have had its address.
static void shadow_alpha(ui::Widget *w, float a, bool force = false) {
	for (int i = 0; i < ALPHA_CACHE; i++) {
		if (alpha_cache[i].widget == w) {
			if (!force && fabsf_(alpha_cache[i].alpha - a) < 0.004f)
				return;
			alpha_cache[i].alpha = a;
			w->SetColor(math::v4(0.0f, 0.0f, 0.0f, a));
			return;
		}
	}
	alpha_cache[alpha_cache_next].widget = w;
	alpha_cache[alpha_cache_next].alpha = a;
	alpha_cache_next = (alpha_cache_next + 1) % ALPHA_CACHE;
	w->SetColor(math::v4(0.0f, 0.0f, 0.0f, a));
}

static ui::Widget *child_with_id(ui::Widget *parent, uint32_t id) {
	int children = parent->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return NULL;
	for (int i = 0; i < children; i++) {
		ui::Widget *child = parent->GetChild(i);
		if (looks_like_object(child) && id_of(child) == id)
			return child;
	}
	return NULL;
}

// An IDParam for a hash, filled in by hand. Its constructor and destructor
// would import Paf's shared empty string, a variable, which an early-loading
// plugin can't rely on; this one's string is an empty one of Engram's own.
struct IdHolder {
	uint32_t raw[(sizeof(IDParam) + 3) / 4];
};
static char empty_id[1];

static const IDParam &make_id(IdHolder *holder, uint32_t hash) {
	sceClibMemset(holder, 0, sizeof(*holder));
	IDParam *id = (IDParam *)holder;
	id->m_id.ptr = empty_id;
	id->m_id.count = 0;
	id->m_idhash = hash;
	return *id;
}

static ui::Widget *create_plane(ui::Widget *parent, uint32_t id, float size) {
	Plugin *plugin = Plugin::Find("topmenu_plugin");
	if (!looks_like_object(plugin))
		return NULL;
	IdHolder name, style;
	ui::Widget *w = plugin->CreateWidget(parent, "plane", make_id(&name, id), make_id(&style, STYLE_PLAIN_PLANE));
	if (!looks_like_object(w))
		return NULL;
	w->SetSize(math::v4(size, size, 0.0f, 0.0f));
	return w;
}

static int index_in(ui::Widget *parent, ui::Widget *w) {
	int children = parent->GetChildrenNum();
	for (int i = 0; i < children && i < MAX_CHILDREN; i++)
		if (parent->GetChild(i) == w)
			return i;
	return -1;
}

static bool layer_drawn_first(void) {
	int l = cfg.shadow_layer;
	return l == LAYER_UNDER_BACK || l == LAYER_UNDER_FORWARD || l == LAYER_UNDER_FAR_BACK;
}

// How far the shadows sit from their bubbles in z, by the arrangement. Paf's
// draw objects have a depth test (graph::DrawStyle::depth): the Cube effects'
// icons hide behind each other mid-turn. In 1.4 the layer was drawn first at
// the bubbles' own z, and the bubbles failed the test wherever a shadow was; in
// 1.2 and 1.3 it was drawn after them, and showed on top. Which way Paf's
// depth runs isn't known, hence the arrangements.
static float shadow_z(void) {
	float d = (float)cfg.shadow_depth;
	switch (cfg.shadow_layer) {
	case LAYER_UNDER_FORWARD:
	case LAYER_OVER_FORWARD:
		return d;
	case LAYER_UNDER_FAR_BACK:
	case LAYER_OVER_FAR_BACK:
		return -8.0f * d;
	default:
		return -d;
	}
}

static int arrange_logs;

// Puts the layer just before the container among their siblings (drawn under
// the bubbles, over the hidden wallpaper plane), or after everything, by the
// arrangement. SetBehind_core and SetForwardMost_core move it there and then;
// the public SetBehind only queues the move. Checked every frame, so changing
// the arrangement in Settings takes effect when you're back on the home screen.
static void arrange_layer(ui::Widget *inner, ui::Widget *layer, ui::Widget *container) {
	int before = index_in(inner, layer), target = index_in(inner, container);
	if (before < 0 || target < 0)
		return;
	bool first = layer_drawn_first();
	if (first == (before < target))
		return;
	int ret = first ? paf_widget_set_behind_now(layer, container) : paf_widget_set_forward_most_now(layer);
	int now = index_in(inner, layer), cont = index_in(inner, container);
	if (arrange_logs < 12) {
		arrange_logs++;
		log_line("shadows: arrangement %c, %s: 0x%08X, layer now child %d, container %d (were %d and %d of %d), "
		         "z %d\n",
		         'A' + cfg.shadow_layer, first ? "SetBehind_core" : "SetForwardMost_core", ret, now, cont, before,
		         target, inner->GetChildrenNum(), (int)shadow_z());
	}
	if (first != (now < cont))
		shadow_failed("arranging the shadows against the icons");
}

// The layer a page's shadows hang off, made if need be. NULL if there isn't one.
static ui::Widget *shadow_layer(ui::Widget *inner, ui::Widget *container, bool make, int *budget) {
	ui::Widget *layer = child_with_id(inner, SHADOW_LAYER_ID);
	if (layer) {
		if (shadow_failures < SHADOW_FAIL_LIMIT)
			arrange_layer(inner, layer, container);
		return layer;
	}
	if (!make || *budget <= 0 || shadow_failures >= SHADOW_FAIL_LIMIT || !shadow_texture())
		return layer;
	(*budget)--;
	// Size 0, so the plane itself draws nothing. It sits where the container does
	// (both fill the page from its centre), so a shadow and its bubble share
	// coordinates.
	layer = create_plane(inner, SHADOW_LAYER_ID, 0.0f);
	if (!layer) {
		shadow_failed("creating a page's shadow layer");
		return NULL;
	}
	layer->SetColor(math::v4(1.0f, 1.0f, 1.0f, 1.0f));
	shadows_made = true;
	arrange_layer(inner, layer, container);
	return layer;
}

// Brings one page's shadows up to date with its bubbles, after they've moved.
// float_x/y is how far Float moves an icon at the current tilt, and floated says
// whether this page's icons were moved by it this frame.
static void sync_shadows(ui::Widget *page, ui::Widget *container, float d, bool floated, float float_x, float float_y,
                         float wall, bool want, int *budget) {
	ui::Widget *inner = page->GetChild(0);
	if (!looks_like_object(inner))
		return;
	ui::Widget *layer = shadow_layer(inner, container, want, budget);
	if (!layer)
		return;

	// Shadows only make sense under icons at rest on the wallpaper, so they fade
	// out as soon as a page starts to scroll, and come back as it lands.
	float page_fade = 1.0f - fabsf_(d) / PAGE_H * 3.0f;
	if (page_fade < 0.0f)
		page_fade = 0.0f;
	float strength = want ? SHADOW_ALPHA * shadow_master * page_fade : 0.0f;

	int children = container->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return;
	int n = 0;
	for (int b = 0; b < children; b++) {
		ui::Widget *bubble = container->GetChild(b);
		if (!is_bubble(bubble))
			continue;
		int shadows = layer->GetChildrenNum();
		ui::Widget *shadow = n < shadows ? layer->GetChild(n) : NULL;
		if (!looks_like_object(shadow)) {
			if (!want || *budget <= 0 || shadow_failures >= SHADOW_FAIL_LIMIT)
				break;
			(*budget)--;
			shadow = create_plane(layer, SHADOW_ID, SHADOW_SIZE);
			if (!shadow) {
				shadow_failed("creating a shadow");
				break;
			}
			SurfaceRef ref = {shadow_tex};
			int ret = paf_plane_set_texture(shadow, &ref, 0, 0);
			shadow_alpha(shadow, 0.0f, true);
			if (n == 0)
				log_shadow("first shadow %p on page %p, SetTexture 0x%08X", shadow, page, ret);
		}
		n++;

		float a = strength;
		if (a > 0.0f && !bubble->IsVisible())
			a = 0.0f;
		Track *t = find_track(bubble);
		if (a > 0.0f && t && t->modified && t->color_touched)
			a *= t->alpha; // faded by Scroll Animations
		if (a > 0.0f && busy_mask(bubble))
			a = 0.0f; // SceShell is animating it: opening, launching, coming in
		shadow_alpha(shadow, a);
		if (a <= 0.0f)
			continue;

		// The bubble's position now, less the float Motion Depth gave it: the
		// shadow stays down on the wallpaper, and slides a little the other way.
		const float *bp = pos_of(bubble);
		float pos[4] = {bp[0] - SHADOW_PARALLAX * float_x * wall,
		                bp[1] - SHADOW_PARALLAX * float_y * wall - SHADOW_DROP, bp[2] + shadow_z(),
		                bp[3]};
		if (floated) {
			pos[0] -= float_x;
			pos[1] -= float_y;
		}
		if (!same3(pos_of(shadow), pos))
			set_pos(shadow, pos);
		const float *bs = scale_of(bubble);
		if (!same3(scale_of(shadow), bs))
			set_scale(shadow, bs);
	}
	// Shadows left over from bubbles that have gone.
	int shadows = layer->GetChildrenNum();
	for (int i = n; i < shadows && i < MAX_CHILDREN; i++) {
		ui::Widget *shadow = layer->GetChild(i);
		if (looks_like_object(shadow))
			shadow_alpha(shadow, 0.0f);
	}
}

// ---- Wallpaper Parallax ----

// Once per boot: what a page is built from, and what sits beside the pages, to
// find the wallpaper. 1.3 never logged one, so 0xF1B811BF was missing or hidden.
#define BUBBLE_CONTAINER_ID 0xC6063E76

static void log_layers(ui::Widget *w, int depth, int max_depth) {
	if (!looks_like_object(w) || depth > max_depth)
		return;
	int children = w->GetChildrenNum();
	const float *pos = pos_of(w), *size = size_of(w), *scale = scale_of(w);
	log_line("  %.*s%08X size %dx%d pos (%d,%d,%d) scale %d%% visible %d, %d children\n", depth * 2,
	         "                ", (unsigned)id_of(w), (int)size[0], (int)size[1], (int)pos[0], (int)pos[1],
	         (int)pos[2], (int)(scale[0] * 100), w->IsVisible() ? 1 : 0, children);
	if (children < 0 || children > 16 || id_of(w) == BUBBLE_CONTAINER_ID)
		return;
	for (int i = 0; i < children; i++)
		log_layers(w->GetChild(i), depth + 1, max_depth);
}

static ui::Widget *parent_of(ui::Widget *w) {
	ui::Widget *parent = looks_like_object(w) ? *(ui::Widget **)((char *)w + WIDGET_PARENT_OFFSET) : NULL;
	return looks_like_object(parent) ? parent : NULL;
}

static bool layers_logged;

// A custom theme's wallpapers aren't in the pages. 1.4's log showed, beside the
// pages' scroller, a widget (37677DEF on the test console) with one child per
// page, drawn before the pages. This finds it: going up from a page, the first
// earlier sibling of an ancestor with as many children as there are pages.
// Found again every frame, by walking up, so no pointer is kept.
static ui::Widget *wallpaper_list(ui::Widget *page, int page_count) {
	ui::Widget *below = parent_of(page); // the pages' plane
	for (int level = 0; level < 4 && below; level++) {
		ui::Widget *up = parent_of(below);
		if (!up)
			return NULL;
		int n = up->GetChildrenNum();
		if (n < 0 || n > MAX_CHILDREN)
			return NULL;
		for (int i = 0; i < n; i++) {
			ui::Widget *sibling = up->GetChild(i);
			if (sibling == below)
				break; // only ones drawn before the pages
			if (looks_like_object(sibling) && sibling->GetChildrenNum() == page_count)
				return sibling;
		}
		below = up;
	}
	return NULL;
}

static int wall_find_logs;

// The page's own wallpaper: its entry in the list above, if it's a big one (not
// a page dot, say), else the plane inside the page that 1.3 used.
static ui::Widget *theme_wallpaper(ui::Widget *page, int index, int page_count) {
	ui::Widget *list = wallpaper_list(page, page_count);
	ui::Widget *w = list ? list->GetChild(index) : NULL;
	if (!looks_like_object(w))
		return NULL;
	const float *size = size_of(w);
	bool big = size[0] >= 480.0f && size[1] >= 256.0f;
	if (wall_find_logs < 3) {
		wall_find_logs++;
		log_line("wallpaper: page %d's entry in %08X is %p %08X size %dx%d pos (%d,%d) visible %d, %d children%s\n",
		         index, (unsigned)id_of(list), w, (unsigned)id_of(w), (int)size[0], (int)size[1],
		         (int)pos_of(w)[0], (int)pos_of(w)[1], w->IsVisible() ? 1 : 0, w->GetChildrenNum(),
		         big ? "" : ": too small to be a wallpaper");
		if (wall_find_logs == 1) {
			log_line("wallpaper: %08X's children:\n", (unsigned)id_of(list));
			int n = list->GetChildrenNum();
			for (int i = 0; i < n && i < 16; i++)
				log_layers(list->GetChild(i), 1, 3);
		}
	}
	return big ? w : NULL;
}

static ui::Widget *page_background(ui::Widget *page, int index, int page_count) {
	ui::Widget *inner = page->GetChild(0);
	if (!looks_like_object(inner))
		return NULL;
	ui::Widget *bg = child_with_id(inner, BG_ID);
	if (bg && !bg->IsVisible())
		bg = NULL;
	if (!bg)
		bg = theme_wallpaper(page, index, page_count);
	if (!layers_logged) {
		layers_logged = true;
		log_line("wallpaper: using %p %08X for page %d; a page:\n", bg, bg ? (unsigned)id_of(bg) : 0u, index);
		log_layers(page, 0, 3);
		ui::Widget *up = parent_of(page); // the scroll plane
		for (int level = 1; level <= 3 && up; level++) {
			up = parent_of(up);
			if (!up)
				break;
			int n = up->GetChildrenNum();
			log_line("wallpaper: %d levels above the pages, %08X has %d children:\n", level + 1,
			         (unsigned)id_of(up), n);
			for (int i = 0; i < n && i < 16; i++)
				log_layers(up->GetChild(i), 1, 1);
		}
	}
	return bg;
}

// Zooms the wallpaper and slides it the other way from the icons' float, by
// wall_master of the full amount. Its resting state is tracked like a bubble's,
// so settle() puts it back.
static void move_wallpaper(ui::Widget *bg, float float_x, float float_y) {
	Track *t = find_track(bg);
	if (!t)
		t = add_track(bg);
	if (!t)
		return;
	t->seen = frame;
	uint32_t busy = busy_mask(bg);
	if (busy) {
		if (t->modified)
			restore(t, busy & ~BUSY_COLOR);
		return;
	}
	adopt_changes(t);
	if (bg_logs < BG_LOG_LIMIT && !t->modified) {
		bg_logs++;
		const float *s = size_of(bg);
		log_line("wallpaper %p: size %dx%d, visible %d\n", bg, (int)s[0], (int)s[1], bg->IsVisible() ? 1 : 0);
	}

	const float *size = size_of(bg);
	float zoom = 1.0f + (BG_ZOOM - 1.0f) * wall_master;
	float mx = size[0] * (zoom - 1.0f) * 0.5f * 0.95f, my = size[1] * (zoom - 1.0f) * 0.5f * 0.95f;
	float sx = -BG_PARALLAX * float_x * wall_master, sy = -BG_PARALLAX * float_y * wall_master;
	sx = sx < -mx ? -mx : (sx > mx ? mx : sx);
	sy = sy < -my ? -my : (sy > my ? my : sy);

	float pos[4] = {t->base_pos[0] + sx, t->base_pos[1] + sy, t->base_pos[2], t->base_pos[3]};
	float scale[4] = {t->base_scale[0] * zoom, t->base_scale[1] * zoom, t->base_scale[2], t->base_scale[3]};
	if (!same3(pos_of(bg), pos))
		set_pos(bg, pos);
	if (!same3(scale_of(bg), scale)) {
		set_scale(bg, scale);
		t->scale_touched = true;
	}
	remember_applied(t);
}

// ---- Motion Depth: the Vita's tilt ----

static inline float clamp_tilt(float v) {
	return v < -MAX_TILT ? -MAX_TILT : (v > MAX_TILT ? MAX_TILT : v);
}

static void motion_off(void) {
	if (motion_sampling && motion_we_started)
		sceMotionStopSampling();
	motion_sampling = motion_we_started = false;
	motion_last_us = 0;
	tilt_x = tilt_y = shown_x = shown_y = 0.0f;
}

// Brings the tilt up to date. Returns false if there is none to show. SceShell
// already loads SceMotion and Paf imports it, so sampling may be running; if it
// isn't, Engram starts it, and stops it again when Motion Depth goes off.
static bool motion_update(void) {
	if (!motion_sampling) {
		if (frame < motion_retry_at)
			return false;
		int ret = sceMotionStartSampling();
		if (ret != 0 && ret != (int)SCE_MOTION_ERROR_ALREADY_SAMPLING) {
			if (motion_logs < MOTION_LOG_LIMIT) {
				motion_logs++;
				log_line("motion: start sampling failed 0x%08X\n", ret);
			}
			motion_retry_at = frame + 600;
			return false;
		}
		motion_sampling = true;
		motion_we_started = ret == 0;
		log_line("motion: sampling %s\n", ret == 0 ? "started" : "was already running");
	}

	SceMotionState state;
	sceClibMemset(&state, 0, sizeof(state));
	int ret = sceMotionGetState(&state);
	if (ret < 0) {
		if (motion_logs < MOTION_LOG_LIMIT) {
			motion_logs++;
			log_line("motion: get state failed 0x%08X\n", ret);
		}
		if (ret == (int)SCE_MOTION_ERROR_NOT_SAMPLING)
			motion_sampling = motion_we_started = false; // something stopped it: start it again
		motion_retry_at = frame + 60;
		motion_last_us = 0;
		return false;
	}

	SceUInt64 now = sceKernelGetProcessTimeWide();
	float dt = motion_last_us ? (float)(now - motion_last_us) / 1000000.0f : 0.0f;
	motion_last_us = now;
	if (dt > 0.1f)
		dt = 0.1f;

	float gx = state.angularVelocity.x, gy = state.angularVelocity.y;
	if (fabsf_(gx) < GYRO_DEADBAND)
		gx = 0.0f;
	if (fabsf_(gy) < GYRO_DEADBAND)
		gy = 0.0f;
	float leak = dt / MOTION_TAU;
	if (leak > 1.0f)
		leak = 1.0f;
	tilt_x = clamp_tilt(tilt_x + gx * dt - tilt_x * leak);
	tilt_y = clamp_tilt(tilt_y + gy * dt - tilt_y * leak);
	float follow = dt * MOTION_SMOOTH;
	if (follow > 1.0f)
		follow = 1.0f;
	shown_x += (tilt_x - shown_x) * follow;
	shown_y += (tilt_y - shown_y) * follow;

	// A few samples of a real turn, to check the gyro's units and axes.
	if (motion_logs < MOTION_LOG_LIMIT && (fabsf_(gx) > 1.0f || fabsf_(gy) > 1.0f) && frame - motion_logged_at > 60) {
		motion_logs++;
		motion_logged_at = frame;
		log_line("motion: gyro (%d,%d,%d) mrad/s, accel (%d,%d,%d) mg, tilt (%d,%d) mrad\n",
		         (int)(state.angularVelocity.x * 1000), (int)(state.angularVelocity.y * 1000),
		         (int)(state.angularVelocity.z * 1000), (int)(state.acceleration.x * 1000),
		         (int)(state.acceleration.y * 1000), (int)(state.acceleration.z * 1000), (int)(tilt_x * 1000),
		         (int)(tilt_y * 1000));
	}
	return true;
}

static void forget_home(void) {
	home_scene = NULL;
	scroll_box = NULL;
	origin_page = NULL;
	track_count = 0; // the widgets may be gone: never touch them again
}

// Paf calls this on its main thread every frame, the only place widgets are safe to touch.
static void on_frame(void *arg) {
	frame++;
	if (!first_frame_at)
		first_frame_at = sceKernelGetProcessTimeWide();

	if (cfg_pending) {
		cfg = pending_cfg;
		__sync_synchronize();
		cfg_pending = 0;
	}

	ui::Scene *scene = find_home_scene();
	if (!scene) {
		forget_home();
		motion_off();
		return;
	}
	if (scene != home_scene) {
		forget_home();
		home_scene = scene;
		log_line("home page %p\n", scene);
	}
	// The page exists a while before SceShell fills it, so keep looking twice a second.
	if (!scroll_box) {
		if (frame % 30 != 0)
			return;
		scroll_box = find_widget(scene, SCROLL_BOX, 0);
		if (!scroll_box) {
			if (frame == 3600)
				log_line("still no scroll box 0x%08X after 3600 frames\n", SCROLL_BOX);
			return;
		}
		log_line("scroll box %p found at frame %u\n", scroll_box, (unsigned)frame);
		depth_hold_until = frame + DEPTH_HOLD_FRAMES;
	}
	if (scroll_box->GetChildrenNum() < 1)
		return;
	ui::Widget *plane = scroll_box->GetChild(0);
	if (!looks_like_object(plane))
		return;
	int page_count = plane->GetChildrenNum();
	if (page_count < 0 || page_count > MAX_HOME_PAGES)
		return;

	float plane_y = pos_of(plane)[1];
	float nearest = (float)(int)(plane_y / PAGE_H + (plane_y < 0 ? -0.5f : 0.5f)) * PAGE_H;
	bool moving = fabsf_(plane_y - nearest) > REST_EPSILON;

	int effects[FX_COUNT];
	int effect_count = 0;
	bool active = cfg.enabled && !stop_requested;
	if (active && moving)
		effect_count = choose_effects(effects, !was_moving);
	bool gesture_started = moving && !was_moving;
	was_moving = moving;

	// The page you swipe away from is the one nearest the screen as the scroll
	// starts. Only it fades out: the page coming in stays fully visible.
	bool fade_on = active && moving && cfg.fade_out;
	if (!moving)
		origin_page = NULL;
	else if (gesture_started) {
		float best = PAGE_H;
		for (int i = 0; i < page_count; i++) {
			ui::Widget *page = plane->GetChild(i);
			if (!looks_like_object(page))
				continue;
			float d = fabsf_(plane_y + pos_of(page)[1]);
			if (d < best) {
				best = d;
				origin_page = page;
			}
		}
	}

	int animated = 0, pages_on_screen = 0, containers = 0;
	bool any_modified = modified_count() > 0;
	bool depth_moving = false;
	if (cfg.depth && !stop_requested) {
		if (motion_update() && frame >= depth_hold_until)
			depth_moving = fabsf_(shown_x) > DEPTH_REST || fabsf_(shown_y) > DEPTH_REST;
	} else {
		motion_off();
	}
	// Whether SceShell hides the home page while an app is in front isn't known yet.
	{
		int visible = scene->IsVisible() ? 1 : 0;
		if (visible != visible_last && visible_logs < VISIBLE_LOG_LIMIT) {
			visible_logs++;
			log_line("home page visible=%d at frame %u\n", visible, (unsigned)frame);
		}
		if (visible && visible_last != 1)
			depth_hold_until = frame + DEPTH_HOLD_FRAMES; // SceShell brings the icons back in
		visible_last = visible;
	}
	{
		bool want = !ENGRAM_SHADOWS_SHELVED && cfg.shadows && !stop_requested && shadow_failures < SHADOW_FAIL_LIMIT;
		float target = want && frame >= depth_hold_until && visible_last == 1 ? 1.0f : 0.0f;
		if (shadow_master < target)
			shadow_master = shadow_master + SHADOW_FADE_STEP > target ? target : shadow_master + SHADOW_FADE_STEP;
		else if (shadow_master > target)
			shadow_master = shadow_master - SHADOW_FADE_STEP < target ? target : shadow_master - SHADOW_FADE_STEP;
	}
	{
		float target = !ENGRAM_SHADOWS_SHELVED && cfg.wallpaper && cfg.depth && !stop_requested && frame >= depth_hold_until && visible_last == 1
		                   ? 1.0f
		                   : 0.0f;
		if (wall_master < target)
			wall_master = wall_master + SHADOW_FADE_STEP > target ? target : wall_master + SHADOW_FADE_STEP;
		else if (wall_master > target)
			wall_master = wall_master - SHADOW_FADE_STEP < target ? target : wall_master - SHADOW_FADE_STEP;
	}
	bool wall_run = wall_master > 0.0f;
	bool shadows_want = !ENGRAM_SHADOWS_SHELVED && cfg.shadows && !stop_requested && shadow_failures < SHADOW_FAIL_LIMIT;
	bool shadows_run = shadows_want || shadows_made;
	float float_x = 0.0f, float_y = 0.0f;
	depth_float(shown_x, shown_y, cfg.depth_strength, &float_x, &float_y);
	bool floats = cfg.depth_style != DEPTH_TURN;
	int shadow_budget = SHADOW_CREATE_PER_FRAME;
	if (effect_count == 0 && !fade_on && !any_modified && !depth_moving && !shadows_run && !wall_run)
		goto done;

	for (int i = 0; i < page_count; i++) {
		ui::Widget *page = plane->GetChild(i);
		if (!looks_like_object(page))
			continue;
		float d = plane_y + pos_of(page)[1];
		bool on_screen = fabsf_(d) > REST_EPSILON && fabsf_(d) < PAGE_H - REST_EPSILON;
		bool fading = fade_on && page == origin_page;
		bool scrolling = (effect_count > 0 || fading) && on_screen;
		bool tilting = depth_moving && fabsf_(d) < PAGE_H - REST_EPSILON; // the page at rest too
		bool animating = scrolling || tilting;
		bool shadowed = shadows_run && fabsf_(d) < PAGE_H - REST_EPSILON;
		bool walled = wall_run && fabsf_(d) < PAGE_H - REST_EPSILON;
		if (on_screen)
			pages_on_screen++;
		if (!animating && !any_modified && !shadowed && !walled)
			continue;

		// The wallpaper first: it can be there when the bubble container isn't.
		float wall = 0.0f; // how far this page's shadows follow its wallpaper: only if it moves
		if (walled || any_modified) {
			ui::Widget *bg = page_background(page, i, page_count);
			if (bg && walled && bg->IsVisible()) {
				move_wallpaper(bg, float_x, float_y);
				wall = wall_master;
			} else if (bg) {
				settle(bg);
			}
		}

		ui::Widget *container = bubble_container(page);
		if (!container)
			continue;
		containers++;
		int children = container->GetChildrenNum();
		if (children < 0 || children > MAX_CHILDREN)
			continue;
		if (animating || any_modified) {
			for (int b = 0; b < children; b++) {
				ui::Widget *bubble = container->GetChild(b);
				if (!is_bubble(bubble))
					continue;
				if (animating) {
					animate(bubble, d, effects, scrolling ? effect_count : 0, fading && scrolling, tilting);
					animated++;
				} else {
					settle(bubble);
				}
			}
		}
		if (shadowed)
			sync_shadows(page, container, d, tilting && floats, float_x, float_y, wall, shadows_want, &shadow_budget);
	}

done:
	if (gesture_started && gestures_logged < 5) {
		gestures_logged++;
		log_line("scroll %d: plane y=%d, %d pages, %d on screen, %d containers, %d effects, %d bubbles animated\n",
		         gestures_logged, (int)plane_y, page_count, pages_on_screen, containers, effect_count, animated);
	}
	if (stop_requested) {
		restore_all_seen();
		motion_off();
		common::MainThreadCallList::Unregister(on_frame, arg);
		frame_registered = 0;
		return;
	}
	evict_tracks();
}

// ---- starting: register the frame callback from SceShell's main thread ----

static SceInt32 sceAVConfigRegisterCallback_patched(SceUID cbid, SceInt32 a2) {
	if (!__sync_lock_test_and_set(&register_started, 1)) {
		log_line("SceShell is up: registering the frame callback\n");
		common::MainThreadCallList::Register(on_frame, NULL);
		frame_registered = 1;
		log_line("frame callback registered\n");
	}
	return TAI_NEXT(sceAVConfigRegisterCallback_patched, avconfig_ref, cbid, a2);
}

// ---- background thread: reloads the config once a second ----

static int poll_thread_entry(SceSize args, void *argp) {
	bool booting_cleared = false;
	bool first_frame_logged = false;
	Config last = cfg;
	while (poll_running) {
		if (!first_frame_logged && first_frame_at) {
			first_frame_logged = true;
			log_line("first frame ran\n");
		}
		if (!booting_cleared && first_frame_at && sceKernelGetProcessTimeWide() - first_frame_at > STABLE_US) {
			sceIoRemove(BOOTING_PATH);
			booting_cleared = true;
			log_line("stable for 30 s, boot marker cleared\n");
		}

		Config now;
		config_load(&now);
		if (sceClibMemcmp(&now, &last, sizeof(now)) != 0 && !cfg_pending) {
			pending_cfg = now;
			__sync_synchronize();
			cfg_pending = 1;
			last = now;
			log_line("settings changed: scroll=%d random=%d intensity=%d tilt=%d fade=%d depth=%d style=%d strength=%d "
			         "shadows=%d wallpaper=%d layering=%c shadow_depth=%d\n",
			         now.enabled, now.random, now.intensity, now.tilt, now.fade_out, now.depth, now.depth_style,
			         now.depth_strength, now.shadows, now.wallpaper, 'A' + now.shadow_layer, now.shadow_depth);
		}
		sceKernelDelayThread(CONFIG_POLL_US);
	}
	return sceKernelExitThread(0);
}

int shell_start(void) {
	log_line("\n== Engram " ENGRAM_VERSION " in SceShell\n");

	tai_module_info_t info;
	info.size = sizeof(info);
	int ret = taiGetModuleInfo("SceShell", &info);
	if (ret < 0 || info.module_nid != NID_SCESHELL_365) {
		log_line("not retail 3.65 SceShell, staying off: ret=0x%08X nid=0x%08X\n", ret, info.module_nid);
		return 0;
	}

	if (file_exists(DISABLED_PATH)) {
		log_line("%s exists, staying off. Turn Engram on in Settings to try again.\n", DISABLED_PATH);
		return 0;
	}

	SceCtrlData pad;
	sceClibMemset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
	if (pad.buttons & SCE_CTRL_LTRIGGER) {
		log_line("L held at boot, staying off for this boot\n");
		return 0;
	}

	if (file_exists(BOOTING_PATH)) {
		write_marker(DISABLED_PATH);
		sceIoRemove(BOOTING_PATH);
		log_line("the last boot didn't get 30 s past starting Engram: wrote %s, staying off\n", DISABLED_PATH);
		return 0;
	}
	write_marker(BOOTING_PATH);

	config_load(&cfg);
	if (!file_exists(CONFIG_PATH))
		config_save(&cfg);
	rng_state = (uint32_t)sceKernelGetProcessTimeWide();

	avconfig_hook = taiHookFunctionImport(&avconfig_ref, "SceShell", LIB_SCEAVCONFIG, NID_SCEAVCONFIGREGISTERCALLBACK,
	                                      (const void *)sceAVConfigRegisterCallback_patched);
	log_line("start hook: 0x%08X\n", avconfig_hook);
	if (avconfig_hook < 0) {
		sceIoRemove(BOOTING_PATH);
		return 0;
	}

	poll_running = 1;
	poll_thread = sceKernelCreateThread("engram_poll", poll_thread_entry, 0xA0, 0x2000, 0, 0, NULL);
	if (poll_thread >= 0)
		sceKernelStartThread(poll_thread, 0, NULL);
	log_line("poll thread: 0x%08X\n", poll_thread);
	return 0;
}

void shell_stop(void) {
	stop_requested = 1;
	poll_running = 0;
	if (poll_thread >= 0) {
		sceKernelWaitThreadEnd(poll_thread, NULL, NULL);
		sceKernelDeleteThread(poll_thread);
		poll_thread = -1;
	}
	if (avconfig_hook >= 0) {
		taiHookRelease(avconfig_hook, avconfig_ref);
		avconfig_hook = -1;
	}
}
