// The SceShell half of Engram: animates the home screen's bubbles while the
// pages scroll (Scroll Animations), floats them in 3D as the Vita tilts (Motion
// Depth), and lays them out in 3D in place of the grid (Icon Placement).
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
// Engram doesn't touch anything, unless Icon Placement is on: then it keeps
// every bubble in its place in the layout, worked out each frame from where
// SceShell has it in the grid, and gives a page back to the grid while SceShell
// animates it (opening a folder, say) or moves it off the grid (edit mode).

#include <limits> // paf/std/vector uses it without including it

#define private public
#define protected public
#include <paf.h>
#undef private
#undef protected

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/motion.h>
#include <psp2/pvf.h>
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
#define MAX_TRACKS 128        // bubbles changed at once: several pages' worth
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

// Icon Placement. A page that takes on the layout at rest, in front of you,
// eases into it; one coming onto the screen is already in it. A change of
// layout eases the old one out and the new one in.
#define PLACE_STEP (1.0f / 15.0f) // per frame: a quarter of a second
#define PLACE_LOG_LIMIT 3
#define PLACE_SKIP_LOG_LIMIT 8

// Folders. Opening a folder makes a new widget on the home page, outside the
// pages' scroller, with a new id each time (0xF196E3B0, then 0xC5B052CE, in
// 1.1's log), and the folder's icons in an icon container (0xC6063E76) inside
// it, named "0", "1" and so on like the home screen's. So each frame Engram
// looks for visible icon containers on the home page outside the scroller,
// and checks which grid their icons are on before placing them.
#define FOLDER_SEARCH_DEPTH 8
#define MAX_FOLDER_CONTAINERS 4
#define FOLDER_ICON_MIN 96  // px: folder icons are square, 124 in the probe's tree, 138 on the home screen
#define FOLDER_ICON_MAX 160
#define FOLDER_LOG_LIMIT 1

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
	float place; // 0..1: how far it has eased into Icon Placement's layout
	// Meta alpha, which unlike the colour fades everything inside a widget too
	// (Paf keeps a running product of it down the tree): how app pages fade.
	float base_meta, meta; // the widget's own, and what Engram last set, valid while meta_touched
	float base_show;       // the widget's own show alpha, valid while show_touched
	uint32_t seen;
	bool modified;
	bool scale_touched, rot_touched, color_touched, meta_touched, show_touched;
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
static int motion_deadband_was = -1; // the deadband before Engram turned it off, or -1
static uint32_t motion_retry_at;
static SceUInt64 motion_last_us;
static float tilt_x, tilt_y;   // radians: about the Vita's x axis (top edge towards you) and y axis
static float shown_x, shown_y; // the tilt the icons show, smoothed
static int motion_logs;
static uint32_t motion_logged_at;
static uint32_t depth_hold_until;
static int busy_logs;
// A layout as it's shown: the one in cfg, once the one before has eased out.
struct PlaceView {
	const char *what;
	int layout, strength;
	float master; // 0..1: the layout eases in and out as a whole
	int logs;
};
static PlaceView home_view = {"home", LAYOUT_GRID, DEPTH_NORMAL, 0.0f, 0};
static PlaceView folder_view = {"folders", LAYOUT_GRID, DEPTH_NORMAL, 0.0f, 0};
static int place_skip_logs;
// Visible icon containers outside the pages: an open folder's. Found again
// every frame, and only used within it.
static ui::Widget *folder_containers[MAX_FOLDER_CONTAINERS];
static int folder_container_count;
static int folder_found_logs;
static int folder_logs;

// Edit mode (hold an empty spot on the home screen). SceShell zooms the pages
// out and adds a few buttons, such as a + to add a page, and the icons don't
// wobble. 1.1's log showed it as a new widget beside the pages' scroller (in
// 0xCBD8EDA5: 0x8D1267F4, then 0x475855E1, a new id each time) with no icons
// in it. An open folder is a new widget there too, but it holds the folder's
// icon container. While edit mode lasts, Scroll Animations and Icon Placement
// stand aside, and they come back after it.
#define EDIT_ENTER_FRAMES 6  // the widget has to be there this long to count
#define EDIT_EXIT_FRAMES 20  // and gone this long to end it
#define EDIT_LOG_LIMIT 12
#define EDIT_SEARCH_DEPTH 4  // how deep a widget beside the pages is searched for icons
static bool edit_mode;
static int edit_signal_frames, edit_calm_frames, edit_logs;

// Folder icons straightened since the folder opened (see straighten), so each
// is set once, not fought over every frame.
#define STRAIGHTENED_MAX 32
static ui::Widget *straightened[STRAIGHTENED_MAX]; // compared, never dereferenced
static int straightened_n;

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
// SceShell gives each home icon a small turn and size of its own, and changes
// them now and then: over every 1.1 log, the turn was about the x axis only,
// within 33 mrad either way, and the size 98% to 101%. Engram takes whatever
// SceShell writes as the icon's own. But SceShell can also write back a turn
// and size that include Engram's (closing a folder, say): 1.1's logs had icons
// resting at -248 mrad, Icon Placement's own turn for their slot, and once at
// -499, twice that, as the layout went on top each time. So a resting turn or
// size outside SceShell's range has Engram's taken off: the layout's, if what's
// there matches it (eased in any amount), and whatever's left is put upright at
// full size.
#define NATURAL_TURN_X 0.06f
#define NATURAL_TURN_YZ 0.02f
#define NATURAL_SCALE_MIN 0.95f
#define NATURAL_SCALE_MAX 1.03f
#define CLEAN_LOG_LIMIT 4
static int clean_logs;

static bool natural_scale(float v) {
	return v >= NATURAL_SCALE_MIN && v <= NATURAL_SCALE_MAX;
}

static void clean_base(Track *t) {
	float *r = t->base_rot, *sc = t->base_scale;
	bool turned = fabsf_(r[0]) > NATURAL_TURN_X || fabsf_(r[1]) > NATURAL_TURN_YZ || fabsf_(r[2]) > NATURAL_TURN_YZ;
	bool sized = !natural_scale(sc[0]) || !natural_scale(sc[1]);
	if (!turned && !sized)
		return;
	int was_rx = (int)(r[0] * 1000), was_ry = (int)(r[1] * 1000), was_s = (int)(sc[0] * 100);
	Placement pl;
	if (turned && home_view.layout != LAYOUT_GRID &&
	    place_icon(GRID_HOME, home_view.layout, home_view.strength, t->base_pos[0], t->base_pos[1], &pl)) {
		float len2 = pl.rx * pl.rx + pl.ry * pl.ry;
		if (len2 > 0.0001f) {
			float k = (r[0] * pl.rx + r[1] * pl.ry) / len2; // how many times the layout's turn is in there
			float ex = r[0] - k * pl.rx, ey = r[1] - k * pl.ry;
			if (k > 0.05f && k < 2.5f && fabsf_(ex) <= NATURAL_TURN_X && fabsf_(ey) <= NATURAL_TURN_YZ) {
				r[0] = ex;
				r[1] = ey;
				// The size only if it's out of range too: SceShell may have
				// written a size of its own and left Engram's turn.
				float ps = 1.0f + (pl.s - 1.0f) * k;
				if (sized && ps > 0.1f) {
					sc[0] /= ps;
					sc[1] /= ps;
				}
			}
		}
	}
	if (fabsf_(r[0]) > NATURAL_TURN_X)
		r[0] = 0.0f;
	if (fabsf_(r[1]) > NATURAL_TURN_YZ)
		r[1] = 0.0f;
	if (fabsf_(r[2]) > NATURAL_TURN_YZ)
		r[2] = 0.0f;
	if (!natural_scale(sc[0]) || !natural_scale(sc[1]))
		sc[0] = sc[1] = 1.0f;
	if (clean_logs < CLEAN_LOG_LIMIT) {
		clean_logs++;
		log_line("bubble %p rested at turn (%d,%d) mrad, scale %d%%: Engram's taken off, now (%d,%d), %d%%\n",
		         t->widget, was_rx, was_ry, was_s, (int)(r[0] * 1000), (int)(r[1] * 1000), (int)(sc[0] * 100));
	}
}

static void adopt_changes(Track *t) {
	ui::Widget *w = t->widget;
	bool first = !t->modified;
	bool adopted = false;

	if (first) {
		t->scale_touched = t->rot_touched = t->color_touched = t->meta_touched = t->show_touched = false;
		if (!at_rest_scale(scale_of(w)))
			diag(t, DIAG_FIRST, "still animating when its page started moving");
	}

	if (first || !same3(pos_of(w), t->last_pos))
		copy4(t->base_pos, pos_of(w));
	if (first || !same3(scale_of(w), t->last_scale)) {
		if (!first)
			diag(t, DIAG_SCALE, "scale changed by SceShell mid-scroll");
		copy4(t->base_scale, scale_of(w));
		adopted = true;
	}
	if (first || !same3(rot_of(w), t->last_rot)) {
		if (!first)
			diag(t, DIAG_ROT, "rotation changed by SceShell mid-scroll");
		copy4(t->base_rot, rot_of(w));
		adopted = true;
	}
	if (adopted)
		clean_base(t);
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
	// the test console, likely folders) rest at 98% scale. Only if they're still
	// what Engram set, as with position: if SceShell has changed one since, it's
	// SceShell's. (Putting back a size recorded while SceShell was still zooming
	// the home screen left it a little small, behind App Page Animation.)
	if (!(busy & BUSY_SCALE) && t->scale_touched && same3(scale_of(w), t->last_scale) &&
	    !same3(scale_of(w), t->base_scale))
		set_scale(w, t->base_scale);
	if (!(busy & BUSY_ROT) && t->rot_touched && same3(rot_of(w), t->last_rot) && !same3(rot_of(w), t->base_rot))
		set_rot(w, t->base_rot);
	// Always, whatever the bubble reads as: see set_alpha.
	// Alpha and meta alpha are put back unless SceShell is animating that very
	// property (setting it would cut its animation short; it sets its own end
	// value). Skipping them for any fade at all left an app page at 1% meta
	// alpha for good: there, invisible, and still taking the taps.
	if (t->color_touched && !w->FindAnim(ui::Widget::ANIM_COLOR)) {
		set_alpha(w, 1.0f);
		if (restore_logged < RESTORE_LOG_LIMIT) {
			restore_logged++;
			log_line("restored alpha of %p from %d%%, visible=%d\n", w, (int)(t->alpha * 100), w->IsVisible() ? 1 : 0);
		}
	}

	if (t->meta_touched && !w->FindAnim(ui::Widget::ANIM_META_ALPHA))
		w->SetMetaAlpha(t->base_meta);
	if (t->show_touched && !w->FindAnim(ui::Widget::ANIM_SHOW_ALPHA))
		w->SetShowAlpha(t->base_show);

	t->modified = false;
	t->scale_touched = t->rot_touched = t->color_touched = t->meta_touched = t->show_touched = false;
	t->place = 0.0f;
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

enum PlaceMode { PLACE_NONE, PLACE_EASE, PLACE_INSTANT };

static inline float ease(float x) {
	return x * x * (3.0f - 2.0f * x);
}

// `view` and `grid`: the layout to place the bubble in, with place_mode.
static void animate(ui::Widget *w, float d, const int *effects, int effect_count, bool fading, bool tilting,
                    int place_mode, PlaceView *view = NULL, int grid = GRID_HOME, bool upright = false) {
	Track *t = find_track(w);
	if (!t)
		t = add_track(w);
	if (!t)
		return;
	t->seen = frame;

	// A page that is only tilting waits for SceShell to finish animating its
	// bubbles. Scrolling pages don't: Scroll Animations has always moved them
	// regardless, and works.
	// (A page in Icon Placement's layout was checked as a whole: page_placeable.)
	if (tilting && effect_count == 0 && !fading && place_mode == PLACE_NONE) {
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
	// Folder icons rest upright at full size, whatever they read as. A folder's
	// icons vanish as it closes, before Engram can put them back, and come back
	// still turned when it opens again; taking that as their resting state
	// added the layout's turn again every time.
	if (upright) {
		for (int i = 0; i < 3; i++) {
			t->base_rot[i] = 0.0f;
			t->base_scale[i] = 1.0f;
		}
	}
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
	c.span = PAGE_H;
	c.across = PAGE_W;
	c.eye = PERSPECTIVE;
	c.seed = (uint32_t)((int)c.bx * 7919 + (int)c.by * 104729);
	c.tilt = cfg.tilt != 0;

	// Icon Placement first: the effects and Motion Depth work from where it
	// puts the icon, as they would from the grid.
	Placement pl = {c.bx, c.by, 1.0f, 0.0f, 0.0f};
	float k = 0.0f;
	if (place_mode != PLACE_NONE && view) {
		if (!t->modified)
			t->place = place_mode == PLACE_INSTANT ? 1.0f : 0.0f;
		else if (t->place < 1.0f)
			t->place = t->place + PLACE_STEP > 1.0f ? 1.0f : t->place + PLACE_STEP;
		if (place_icon(grid, view->layout, view->strength, c.bx, c.by, &pl)) {
			k = ease(t->place) * ease(view->master);
			if (view->logs < PLACE_LOG_LIMIT && k > 0.999f) {
				view->logs++;
				log_line("placement %s %d/%d, grid %d: (%d,%d) -> (%d,%d) scale %d%% turn (%d,%d) mrad\n",
				         view->what, view->layout, view->strength, grid, (int)c.bx, (int)c.by, (int)pl.x, (int)pl.y,
				         (int)(pl.s * 100),
				         (int)(pl.rx * 1000), (int)(pl.ry * 1000));
			}
		}
	} else {
		t->place = 0.0f;
	}
	c.bx += (pl.x - c.bx) * k;
	c.by += (pl.y - c.by) * k;
	float place_scale = 1.0f + (pl.s - 1.0f) * k;

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
	float scale[4] = {t->base_scale[0] * x.sx * place_scale, t->base_scale[1] * x.sy * place_scale,
	                  t->base_scale[2], t->base_scale[3]};
	float rot[4] = {t->base_rot[0] + x.rx + pl.rx * k, t->base_rot[1] + x.ry + pl.ry * k, t->base_rot[2] + x.rz,
	                t->base_rot[3]};

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

// `busy`: properties SceShell is animating, left to it (see restore).
static void settle(ui::Widget *w, uint32_t busy = 0) {
	Track *t = find_track(w);
	if (!t)
		return;
	t->seen = frame;
	if (t->modified)
		restore(t, busy);
}

// Where SceShell has the bubble: its base while Engram has it moved.
static const float *resting_pos(ui::Widget *w) {
	Track *t = find_track(w);
	if (t && t->modified && same3(pos_of(w), t->last_pos))
		return t->base_pos;
	return pos_of(w);
}

static float inverse_ease(float y) {
	float lo = 0.0f, hi = 1.0f;
	for (int i = 0; i < 16; i++) {
		float mid = 0.5f * (lo + hi);
		if (mid * mid * (3.0f - 2.0f * mid) < y)
			lo = mid;
		else
			hi = mid;
	}
	return 0.5f * (lo + hi);
}

// A bubble SceShell has put where the layout had it, or partway there (closing
// a folder, it can put back a place it saw while the layout was easing in), or
// one left in the layout with no track: it reads as off the grid, and the
// layout would never come back to it. If it sits on the way from a grid slot
// to that slot's place in the layout, it's Engram's: the track is set up to
// say so, from that slot, eased in that far, and the layout carries on from
// there. Nothing on screen moves.
static int recover_logs;

static bool recover_placed(ui::Widget *w, const PlaceView *view, int grid) {
	if (view->layout == LAYOUT_GRID || edit_mode || view->master <= 0.0f)
		return false;
	const float *pos = pos_of(w), *scale = scale_of(w), *rot = rot_of(w);
	for (int slot = 0; slot < 10; slot++) {
		float gx, gy;
		Placement pl;
		if (!grid_slot_pos(grid, slot, &gx, &gy) || !place_icon(grid, view->layout, view->strength, gx, gy, &pl))
			continue;
		float dx = pl.x - gx, dy = pl.y - gy, len2 = dx * dx + dy * dy;
		if (len2 < 4.0f)
			continue; // the layout hardly moves this slot: it's on the grid anyway
		float k = ((pos[0] - gx) * dx + (pos[1] - gy) * dy) / len2;
		float ex = pos[0] - (gx + k * dx), ey = pos[1] - (gy + k * dy);
		if (k < 0.02f || k > 1.05f || ex * ex + ey * ey > 1.5f * 1.5f)
			continue;
		Track *t = find_track(w);
		if (!t)
			t = add_track(w);
		if (!t)
			return false;
		t->seen = frame;
		t->base_pos[0] = gx;
		t->base_pos[1] = gy;
		t->base_pos[2] = pos[2];
		t->base_pos[3] = pos[3];
		float ps = 1.0f + (pl.s - 1.0f) * k;
		copy4(t->base_scale, scale);
		copy4(t->base_rot, rot);
		t->base_scale[0] /= ps;
		t->base_scale[1] /= ps;
		t->base_rot[0] -= pl.rx * k;
		t->base_rot[1] -= pl.ry * k;
		if (grid == GRID_HOME)
			clean_base(t);
		copy4(t->last_pos, pos);
		copy4(t->last_scale, scale);
		copy4(t->last_rot, rot);
		t->modified = true;
		t->scale_touched = t->rot_touched = true;
		t->color_touched = false;
		float m = ease(view->master);
		float want = m > 0.01f ? k / m : 1.0f;
		t->place = inverse_ease(want > 1.0f ? 1.0f : want);
		if (recover_logs < PLACE_SKIP_LOG_LIMIT) {
			recover_logs++;
			log_line("placement: bubble %p was %d%% of the way into the layout from slot %d: taken up from there\n",
			         w, (int)(k * 100), slot);
		}
		return true;
	}
	return false;
}

// Whether a page's bubbles can take Icon Placement's layout this frame: none
// of them being moved, scaled or turned by SceShell (opening or closing a
// folder, bringing the icons in), and all of them on the Vita's grid (not
// dragged around in edit mode). Otherwise the page goes back to the grid until
// it's settled, so SceShell's own animations and edit mode work as they always
// have. Alpha doesn't count: SceShell fades in pages it has just built, and
// Scroll Animations has always moved bubbles through that.
static bool page_placeable(ui::Widget *page, ui::Widget *container, int children) {
	for (int b = 0; b < children; b++) {
		ui::Widget *w = container->GetChild(b);
		if (!is_bubble(w))
			continue;
		uint32_t busy = busy_mask(w) & (BUSY_POS | BUSY_SCALE | BUSY_ROT);
		const float *rest = resting_pos(w);
		bool on_grid = grid_slot(GRID_HOME, rest[0], rest[1]) >= 0;
		if (!busy && !on_grid && recover_placed(w, &home_view, GRID_HOME))
			on_grid = true;
		if (!busy && on_grid)
			continue;
		if (place_skip_logs < PLACE_SKIP_LOG_LIMIT) {
			place_skip_logs++;
			log_line("placement: page %p back on the grid at frame %u: bubble %p at (%d,%d) %s\n", page,
			         (unsigned)frame, w, (int)rest[0], (int)rest[1],
			         busy ? "is being animated" : "is off the grid");
		}
		return false;
	}
	return true;
}

// ---- Folders ----

static bool is_folder_icon(ui::Widget *w) {
	if (!looks_like_object(w))
		return false;
	const float *size = size_of(w);
	return (int)size[0] == (int)size[1] && size[0] >= FOLDER_ICON_MIN && size[0] <= FOLDER_ICON_MAX;
}

// The grid an open folder's icons are on, if they're all on one and SceShell
// isn't moving any of them (opening or closing the folder): GRID_FOLDER or
// GRID_HOME. -1 if not, or if none are showing.
static int folder_grid(ui::Widget *container, int children) {
	int shown = 0;
	bool on[GRID_COUNT] = {true, true};
	bool busy = false;
	for (int b = 0; b < children; b++) {
		ui::Widget *w = container->GetChild(b);
		if (!is_folder_icon(w) || !w->IsVisible())
			continue;
		shown++;
		if (busy_mask(w) & (BUSY_POS | BUSY_SCALE | BUSY_ROT))
			busy = true;
		const float *rest = resting_pos(w);
		for (int g = 0; g < GRID_COUNT; g++) {
			if (on[g] && grid_slot(g, rest[0], rest[1]) < 0 && !(!find_track(w) && recover_placed(w, &folder_view, g)))
				on[g] = false;
		}
	}
	if (shown == 0)
		return -1;
	int grid = busy ? -1 : (on[GRID_FOLDER] ? GRID_FOLDER : (on[GRID_HOME] ? GRID_HOME : -1));
	// What an open folder is made of, the first few times one is seen settled.
	if (!busy && folder_logs < FOLDER_LOG_LIMIT) {
		folder_logs++;
		const float *size = size_of(container);
		log_line("folder: %08X %dx%d has %d icons showing, grid %d:\n", (unsigned)id_of(container), (int)size[0],
		         (int)size[1], shown, grid);
		for (int b = 0; b < children && b < 16; b++) {
			ui::Widget *w = container->GetChild(b);
			if (!looks_like_object(w))
				continue;
			const float *pos = pos_of(w), *sz = size_of(w);
			log_line("  %08X size %dx%d pos (%d,%d) visible %d\n", (unsigned)id_of(w), (int)sz[0], (int)sz[1],
			         (int)pos[0], (int)pos[1], w->IsVisible() ? 1 : 0);
		}
	}
	return grid;
}

// A folder's icons are made afresh each time it opens, and they take on the
// turn and scale of the folder's own icon on the home screen, which Icon
// Placement may have turned (1.1's log). Folder icons rest upright at full
// size, so when they aren't in the folders' layout they're set that way.
static void straighten(ui::Widget *w, uint32_t busy) {
	if (busy & (BUSY_POS | BUSY_SCALE | BUSY_ROT))
		return; // still opening: once it's settled
	for (int i = 0; i < straightened_n; i++)
		if (straightened[i] == w)
			return;
	if (straightened_n < STRAIGHTENED_MAX)
		straightened[straightened_n++] = w;
	const float *rot = rot_of(w), *scale = scale_of(w);
	if (!(busy & BUSY_ROT) && (fabsf_(rot[0]) > 0.001f || fabsf_(rot[1]) > 0.001f || fabsf_(rot[2]) > 0.001f)) {
		float r[4] = {0.0f, 0.0f, 0.0f, rot[3]};
		set_rot(w, r);
	}
	if (!(busy & BUSY_SCALE) && (fabsf_(scale[0] - 1.0f) > 0.001f || fabsf_(scale[1] - 1.0f) > 0.001f)) {
		float v[4] = {1.0f, 1.0f, scale[2], scale[3]};
		set_scale(w, v);
	}
}

// Places an open folder's icons in the folders' layout, or puts them back.
static void sync_folder_container(ui::Widget *container, bool run) {
	int children = container->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return;
	int grid = run ? folder_grid(container, children) : -1;
	for (int b = 0; b < children; b++) {
		ui::Widget *w = container->GetChild(b);
		if (!is_folder_icon(w))
			continue;
		if (grid >= 0 && w->IsVisible()) {
			animate(w, 0.0f, NULL, 0, false, false, PLACE_EASE, &folder_view, grid, true);
		} else {
			uint32_t busy = busy_mask(w);
			if (find_track(w))
				settle(w, busy);
			if (w->IsVisible())
				straighten(w, busy);
		}
	}
}

// Hidden widgets are skipped with everything in them: a closed folder view's
// icons may still read as visible themselves.
static void find_folder_containers(ui::Widget *w, ui::Widget *parent, int depth) {
	if (!looks_like_object(w) || depth > FOLDER_SEARCH_DEPTH || w == scroll_box || !w->IsVisible() ||
	    folder_container_count >= MAX_FOLDER_CONTAINERS)
		return;
	if (id_of(w) == BUBBLE_CONTAINER) {
		folder_containers[folder_container_count++] = w;
		if (folder_found_logs < FOLDER_LOG_LIMIT) {
			folder_found_logs++;
			const float *size = size_of(w);
			log_line("folder: icon container %p %dx%d in %08X, %d children, at frame %u\n", w, (int)size[0],
			         (int)size[1], parent ? (unsigned)id_of(parent) : 0u, w->GetChildrenNum(), (unsigned)frame);
		}
		return;
	}
	int children = w->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return;
	for (int i = 0; i < children; i++)
		find_folder_containers(w->GetChild(i), w, depth + 1);
}

static void sync_folders(bool run) {
	for (int i = 0; i < folder_container_count; i++)
		sync_folder_container(folder_containers[i], run);
}

// Eases a view towards the layout wanted. A new layout or strength waits for
// the old one to ease out. True while there's a layout to show.
static bool view_update(PlaceView *v, int layout, int strength) {
	bool want = layout != LAYOUT_GRID && !stop_requested;
	if (v->master <= 0.0f && want && (v->layout != layout || v->strength != strength)) {
		v->layout = layout;
		v->strength = strength;
		v->logs = 0;
		log_line("placement %s: layout %d, strength %d\n", v->what, layout, strength);
	}
	float target = want && v->layout == layout && v->strength == strength ? 1.0f : 0.0f;
	if (v->master < target)
		v->master = v->master + PLACE_STEP > target ? target : v->master + PLACE_STEP;
	else if (v->master > target)
		v->master = v->master - PLACE_STEP < target ? target : v->master - PLACE_STEP;
	return want || v->master > 0.0f;
}

// ---- Edit mode ----

// Icon containers inside a widget: how many, and the widest. A folder's is
// 1200 x 700; the home pages' are 960 x 512, and edit mode may show the pages
// zoomed out, so containers alone don't mean a folder.
static void count_containers(ui::Widget *w, int depth, int *count, float *widest) {
	if (!looks_like_object(w) || depth > EDIT_SEARCH_DEPTH)
		return;
	if (id_of(w) == BUBBLE_CONTAINER) {
		(*count)++;
		float width = size_of(w)[0];
		if (width > *widest)
			*widest = width;
		return;
	}
	int children = w->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return;
	for (int i = 0; i < children; i++)
		count_containers(w->GetChild(i), depth + 1, count, widest);
}

#define FOLDER_CONTAINER_WIDTH 1100.0f // wider than this is a folder's
#define BESIDE_LOG_LIMIT 8
static ui::Widget *beside_seen[8]; // compared, never dereferenced
static int beside_logs;
static bool holder_logged;

// A visible widget beside the pages' scroller that isn't an open folder.
static bool edit_signal(uint32_t *id_out) {
	ui::Widget *holder = *(ui::Widget **)((char *)scroll_box + WIDGET_PARENT_OFFSET);
	if (!looks_like_object(holder))
		return false;
	int children = holder->GetChildrenNum();
	if (!holder_logged) {
		holder_logged = true;
		log_line("edit mode: watching %08X beside the scroll box, %d children\n", (unsigned)id_of(holder), children);
	}
	if (children < 0 || children > MAX_CHILDREN)
		return false;
	bool found = false;
	ui::Widget *seen_now[8];
	int seen_n = 0;
	for (int i = 0; i < children; i++) {
		ui::Widget *c = holder->GetChild(i);
		if (c == scroll_box || !looks_like_object(c) || !c->IsVisible())
			continue;
		int count = 0;
		float widest = 0.0f;
		count_containers(c, 0, &count, &widest);
		bool folder = widest > FOLDER_CONTAINER_WIDTH;
		// What each new one is, once.
		bool known = false;
		for (int k = 0; k < 8; k++)
			known = known || beside_seen[k] == c;
		if (!known && beside_logs < BESIDE_LOG_LIMIT) {
			beside_logs++;
			const float *size = size_of(c);
			log_line("beside the pages at frame %u: %08X %dx%d, %d children, %d icon containers, widest %d%s\n",
			         (unsigned)frame, (unsigned)id_of(c), (int)size[0], (int)size[1], c->GetChildrenNum(), count,
			         (int)widest, folder ? ": a folder" : "");
		}
		if (seen_n < 8)
			seen_now[seen_n++] = c;
		if (!folder && !found) {
			*id_out = id_of(c);
			found = true;
		}
	}
	for (int k = 0; k < 8; k++)
		beside_seen[k] = k < seen_n ? seen_now[k] : NULL;
	return found;
}

// Edit mode zooms the home screen out. Which widget it scales isn't known
// yet, so these are all checked: the home page, the widget holding the pages'
// scroller, the scroller, the plane the pages sit on, and the page in front of
// you. The scales are logged whenever they change, to find out.
#define EDIT_ZOOM 0.98f // smaller than this is zoomed out
#define ZOOM_LOG_LIMIT 30
static int zoom_logs;
static int zoom_last[5] = {-1, -1, -1, -1, -1}; // as last logged
static uint32_t zoom_logged_at;

static bool zoomed_out(ui::Widget *plane, float plane_y, int page_count, int *which) {
	ui::Widget *page = NULL;
	float best = PAGE_H;
	for (int i = 0; i < page_count; i++) {
		ui::Widget *p = plane->GetChild(i);
		if (!looks_like_object(p))
			continue;
		float d = fabsf_(plane_y + pos_of(p)[1]);
		if (d < best) {
			best = d;
			page = p;
		}
	}
	ui::Widget *holder = *(ui::Widget **)((char *)scroll_box + WIDGET_PARENT_OFFSET);
	ui::Widget *ws[5] = {home_scene, holder, scroll_box, plane, page};
	static const char *const NAMES[5] = {"home page", "holder", "scroller", "plane", "page"};
	bool zoomed = false, changed = false;
	int now[5];
	for (int i = 0; i < 5; i++) {
		now[i] = looks_like_object(ws[i]) ? (int)(scale_of(ws[i])[0] * 100.0f + 0.5f) : -1;
		changed = changed || now[i] != zoom_last[i];
		if (!zoomed && now[i] >= 0 && now[i] < (int)(EDIT_ZOOM * 100.0f)) {
			zoomed = true;
			*which = i;
		}
	}
	// At most twice a second, so a zoom animation isn't logged frame by frame
	// (each line is a file write).
	if (changed && zoom_logs < ZOOM_LOG_LIMIT && frame - zoom_logged_at >= 30) {
		zoom_logs++;
		zoom_logged_at = frame;
		for (int i = 0; i < 5; i++)
			zoom_last[i] = now[i];
		log_line("scales at frame %u: %s %d%%, %s %d%%, %s %d%%, %s %d%%, %s %d%%\n", (unsigned)frame, NAMES[0], now[0],
		         NAMES[1], now[1], NAMES[2], now[2], NAMES[3], now[3], NAMES[4], now[4]);
	}
	return zoomed;
}

static void update_edit_mode(ui::Widget *plane, float plane_y, int page_count) {
	uint32_t id = 0;
	edit_signal(&id); // logs what appears beside the pages: 1526E816 (60 x 80) is there as edit mode ends
	int which = -1;
	bool signal = zoomed_out(plane, plane_y, page_count, &which);
	id = (uint32_t)which;
	if (signal) {
		edit_calm_frames = 0;
		if (!edit_mode && ++edit_signal_frames >= EDIT_ENTER_FRAMES) {
			edit_mode = true;
			if (edit_logs < EDIT_LOG_LIMIT) {
				edit_logs++;
				log_line("edit mode on at frame %u: zoomed out (%d)\n", (unsigned)frame, (int)id);
			}
		}
	} else {
		edit_signal_frames = 0;
		if (edit_mode && ++edit_calm_frames >= EDIT_EXIT_FRAMES) {
			edit_mode = false;
			if (edit_logs < EDIT_LOG_LIMIT) {
				edit_logs++;
				log_line("edit mode off at frame %u\n", (unsigned)frame);
			}
		}
	}
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
	if (motion_sampling && motion_we_started) {
		if (motion_deadband_was >= 0)
			sceMotionSetDeadband(motion_deadband_was);
		sceMotionStopSampling();
	}
	motion_deadband_was = -1;
	motion_sampling = motion_we_started = false;
	motion_last_us = 0;
	tilt_x = tilt_y = shown_x = shown_y = 0.0f;
}

// Brings the tilt up to date. Returns false if there is none to show. SceShell
// already loads SceMotion and Paf imports it, so sampling may be running; if it
// isn't, Engram starts it, and stops it again when Motion Depth goes off.
//
// Started by Engram, sampling comes with SceMotion's deadband on (its default),
// which reads slow turns as no turn at all: on a console where nothing else
// had started it (GitHub issue #1), gently tilting the Vita did nothing, and
// only fast turns registered. So Engram turns the deadband off while it's the
// one sampling, and back to how it was when it stops.
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
		int deadband = sceMotionGetDeadband();
		if (motion_we_started && deadband == 1) {
			motion_deadband_was = deadband;
			sceMotionSetDeadband(0);
		}
		log_line("motion: sampling %s, deadband %d%s\n", ret == 0 ? "started" : "was already running", deadband,
		         motion_deadband_was == 1 ? ", turned off" : "");
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

// ---- Font probe ----
//
// For per-place fonts (planned): which font family the text in each part of
// SceShell uses. Paf picks a text's fonts by its TextStyle family: family 0
// uses the four language slots (Japanese, Latin, Hangul, Chinese) that Font
// fills; a family above 0 adds a slot of its own (ScePaf 0xE03A3CBE: kind
// family + 0xFFFE, so families 2-9 open music_arib.pvf and the TTFs in
// vs0:data/external/font/pvf). If each place's text can be moved to a family
// of its own, that family's font can be a different one. This walks every
// SceShell plugin's scenes every few seconds and logs each ui::Text the first
// time it's seen: plugin, scene, parents, where it is, its point size and its
// family, and each widget type a plugin has. Nothing is changed. Off in
// releases (1.5): its findings are in Place fonts below. The helpers just
// below are shared with Place fonts and are always built.
#define ENGRAM_FONT_PROBE 0
#define FP_DEPTH 28

extern "C" const char *paf_text_type_name(const void *self) asm("_ZNK3paf2ui4Text11GetTypeNameEv");
extern "C" int paf_text_length(ui::Widget *self, unsigned *len) asm("_ZN3paf2ui4Text9GetLengthERj");
extern "C" int paf_text_style_int(ui::Widget *self, int attr, unsigned index, int *out)
	asm("_ZN3paf2ui4Text17GetStyleAttributeENS_5graph18TextStyleAttributeEjRi");
extern "C" int paf_text_style_v2(ui::Widget *self, int attr, unsigned index, float *out)
	asm("_ZN3paf2ui4Text17GetStyleAttributeENS_5graph18TextStyleAttributeEjRNS_4math2v2E");

static uint32_t fp_text_fn;  // where Text::GetTypeName is, as a vtable holds it
static int fp_slot = -1;     // its index in a widget's vtable, once a Text shows it

// An import stub once resolved: movw r12, lo / movt r12, hi / bx r12.
static uint32_t stub_target(const void *stub) {
	const uint32_t *w = (const uint32_t *)((uintptr_t)stub & ~(uintptr_t)1);
	if ((w[0] & 0x0FF00000) != 0x03000000 || (w[1] & 0x0FF00000) != 0x03400000)
		return 0;
	uint32_t lo = ((w[0] >> 4) & 0xF000) | (w[0] & 0xFFF), hi = ((w[1] >> 4) & 0xF000) | (w[1] & 0xFFF);
	return (hi << 16) | lo;
}

static const uint32_t *vtable_of(ui::Widget *w) {
	const uint32_t *vt = *(const uint32_t *const *)w;
	return looks_like_object(vt) ? vt : NULL;
}

static bool fp_is_text(ui::Widget *w) {
	const uint32_t *vt = vtable_of(w);
	if (!vt || !fp_text_fn)
		return false;
	if (fp_slot >= 0)
		return vt[fp_slot] == fp_text_fn;
	for (int i = 0; i < 40; i++) // well inside any widget's vtable: GetTypeName comes early
		if (vt[i] == fp_text_fn) {
			fp_slot = i;
			log_line("fonts: Text::GetTypeName is vtable slot %d\n", i);
			return true;
		}
	return false;
}

static const char *fp_type_name(ui::Widget *w) {
	const uint32_t *vt = vtable_of(w);
	if (!vt || fp_slot < 0)
		return NULL;
	uint32_t fn = vt[fp_slot];
	if (fn < 0x80000000u || fn >= 0xF0000000u)
		return NULL;
	const char *name = ((const char *(*)(const void *))fn)(w);
	return ((uintptr_t)name >= 0x80000000u && (uintptr_t)name < 0xF0000000u) ? name : NULL;
}

#if ENGRAM_FONT_PROBE
#define FP_EARLY 1800    // for the first 30 s, a walk every FP_EARLY_EVERY frames: the boot warning
#define FP_EARLY_EVERY 6
#define FP_FIRST 120
#define FP_EVERY 300     // then every 5 s or so
#define FP_WALKS 600     // about 25 minutes in all
#define FP_SCENES 64
#define FP_SEEN 768      // text widgets remembered, so each is logged once
#define FP_LINES 700     // lines logged in all
#define FP_TYPES 160     // plugin + type pairs remembered
#define FP_WIDGETS 6000  // widgets visited in one walk at most

static const char *const FP_PLUGINS[] = {
	"topmenu_plugin", "livearea_plugin", "impose_plugin", "indicator_plugin", "power_manage_plugin",
	"coldboot_plugin", "theme_plugin", "applauncher_plugin", "game_plugin", "ime_plugin", "impose_net_plugin",
	"download_plugin", "location_plugin", "mail_resident_plugin", "musiccore_plugin", "telephony_plugin",
	"sysupchk_plugin", "patch_chk_plugin", "photoimport_dialog_plugin", "auth_reset_plugin",
	"dbrecovery_plugin", "gamecard_installer_plugin", "idu_update_plugin", "import_savedata_plugin",
	"online_storage_plugin", "tel_initial_check_plugin",
};
#define FP_PLUGIN_COUNT (int)(sizeof(FP_PLUGINS) / sizeof(FP_PLUGINS[0]))

static ui::Widget *fp_seen[FP_SEEN];
static int fp_seen_count, fp_lines, fp_walks, fp_widgets;
static struct {
	short plugin;
	const char *type;
} fp_types[FP_TYPES];
static int fp_type_count;
static struct {
	void *scene;
	short plugin;
	char visible;
} fp_scenes[FP_SCENES];
static int fp_scene_count;
static char fp_buf[16384]; // one walk's lines, written in one go
static int fp_len;

static void fp_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fp_add(const char *fmt, ...) {
	if (fp_lines >= FP_LINES || fp_len > (int)sizeof(fp_buf) - 256)
		return;
	fp_lines++;
	va_list args;
	va_start(args, fmt);
	fp_len += sceClibVsnprintf(fp_buf + fp_len, sizeof(fp_buf) - fp_len, fmt, args);
	va_end(args);
}

static void fp_flush(void) {
	if (fp_len <= 0)
		return;
	SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0) {
		sceIoWrite(fd, fp_buf, fp_len);
		sceIoClose(fd);
	}
	fp_len = 0;
}

static void fp_note_type(int plugin, ui::Widget *w) {
	const char *type = fp_type_name(w);
	if (!type)
		return;
	for (int i = 0; i < fp_type_count; i++)
		if (fp_types[i].plugin == plugin && fp_types[i].type == type)
			return;
	if (fp_type_count >= FP_TYPES)
		return;
	fp_types[fp_type_count].plugin = (short)plugin;
	fp_types[fp_type_count].type = type;
	fp_type_count++;
	char name[40];
	int n = 0;
	while (n < 39 && type[n] >= 0x20 && type[n] < 0x7F) {
		name[n] = type[n];
		n++;
	}
	name[n] = '\0';
	fp_add("fontprobe: %s has %s (first %08X)\n", FP_PLUGINS[plugin], name, (unsigned)id_of(w));
}

static void fp_note_text(int plugin, ui::Scene *scene, ui::Widget *w) {
	for (int i = 0; i < fp_seen_count; i++)
		if (fp_seen[i] == w)
			return;
	if (fp_seen_count >= FP_SEEN)
		return;
	fp_seen[fp_seen_count++] = w;
	unsigned len = 0;
	int family = -1;
	float point[2] __attribute__((aligned(8))) = {-1, -1};
	int r1 = paf_text_length(w, &len);
	int r2 = paf_text_style_int(w, 0 /* TextStyleAttribute_Family */, 0, &family);
	int r3 = paf_text_style_v2(w, 1 /* TextStyleAttribute_Point */, 0, point);
	uint32_t up[3] = {0, 0, 0};
	ui::Widget *p = w;
	for (int k = 0; k < 3; k++) {
		p = *(ui::Widget **)((char *)p + WIDGET_PARENT_OFFSET);
		if (!looks_like_object(p))
			break;
		up[k] = id_of(p);
	}
	const float *pos = pos_of(w), *size = size_of(w);
	fp_add("fontprobe: %s scene %08X%s text %08X < %08X < %08X < %08X at (%d,%d) %dx%d, %u chars, "
	       "family %d, %d pt (%08X %08X %08X)\n",
	       FP_PLUGINS[plugin], (unsigned)id_of(scene), scene->IsVisible() ? "" : " (hidden)", (unsigned)id_of(w),
	       (unsigned)up[0], (unsigned)up[1], (unsigned)up[2], (int)pos[0], (int)pos[1], (int)size[0], (int)size[1],
	       len, family, (int)point[1], (unsigned)r1, (unsigned)r2, (unsigned)r3);
}

static void fp_walk(int plugin, ui::Scene *scene, ui::Widget *w, int depth) {
	if (!looks_like_object(w) || depth > FP_DEPTH || fp_widgets >= FP_WIDGETS)
		return;
	fp_widgets++;
	if (fp_is_text(w))
		fp_note_text(plugin, scene, w);
	fp_note_type(plugin, w);
	int children = w->GetChildrenNum();
	if (children < 0 || children > 256)
		return;
	for (int i = 0; i < children; i++)
		fp_walk(plugin, scene, w->GetChild(i), depth + 1);
}

static void font_probe(void) {
	if (fp_walks >= FP_WALKS)
		return;
	if (frame < FP_EARLY ? frame % FP_EARLY_EVERY != 0 : (frame - FP_FIRST) % FP_EVERY != 0)
		return;
	fp_walks++;
	if (!fp_text_fn) {
		fp_text_fn = stub_target((const void *)paf_text_type_name);
		fp_add("fontprobe: Text::GetTypeName at 0x%08X\n", (unsigned)fp_text_fn);
	}
	fp_widgets = 0;
	for (int pl = 0; pl < FP_PLUGIN_COUNT; pl++) {
		Plugin *plugin = Plugin::Find(FP_PLUGINS[pl]);
		if (!looks_like_object(plugin))
			continue;
		const uint32_t *words = (const uint32_t *)&plugin->draw_order;
		DrawOrderElement *pages = (DrawOrderElement *)words[0];
		uint32_t count = words[1];
		if (!looks_like_object(pages) || count > MAX_PAGES)
			continue;
		for (uint32_t i = 0; i < count; i++) {
			ui::Scene *scene = pages[i].page;
			if (!looks_like_object(scene))
				continue;
			// Scenes as they come and go, to place the boot warning.
			char visible = scene->IsVisible() ? 1 : 0;
			int k = 0;
			while (k < fp_scene_count && fp_scenes[k].scene != scene)
				k++;
			if (k == fp_scene_count && k < FP_SCENES) {
				fp_scenes[k].scene = scene;
				fp_scenes[k].plugin = (short)pl;
				fp_scenes[k].visible = visible;
				fp_scene_count++;
				fp_add("fontprobe: %s scene %08X there at frame %u%s\n", FP_PLUGINS[pl], (unsigned)id_of(scene),
				       (unsigned)frame, visible ? ", showing" : ", hidden");
			} else if (k < fp_scene_count && fp_scenes[k].visible != visible) {
				fp_scenes[k].visible = visible;
				fp_add("fontprobe: %s scene %08X %s at frame %u\n", FP_PLUGINS[pl], (unsigned)id_of(scene),
				       visible ? "shows" : "hides", (unsigned)frame);
			}
			fp_walk(pl, scene, scene, 0);
		}
	}
	if (fp_walks == 1 || fp_len > 0)
		fp_add("fontprobe: walk %d at frame %u: %d widgets, %d text widgets so far\n", fp_walks, (unsigned)frame,
		       fp_widgets, fp_seen_count);
	fp_flush();
}
#endif

// ---- Place fonts ----
//
// Font > Lock Screen and Status Bar: a font of their own for one part of
// SceShell. Paf draws a character from a text's family font first, when its
// family is above 0 and that font has the character, and from the four
// language slots otherwise (ScePaf 0xE03A461C), so a letter the place's font
// lacks comes from the Home Screen font. Each place's text widgets move to a
// family of its own that nothing else uses (before Engram, the font cache
// only ever held the four language slots, kinds 0x30000-0x30003), and once
// Paf has opened that family's font (kind family + 0xFFFE in its font cache,
// a std::map), the place's font goes in that font object instead, opened
// with Paf's own font library. It's all SceShell's own memory: nothing apps
// share is patched. Paf keeps at most four unused font objects and closes a
// font when it drops one, so each new font object for a family gets a fresh
// copy of the font. The 1.1 lock screen test (PressStart2P on family 5)
// worked on the console. The place's text is checked every PL_EVERY frames,
// as SceShell replaces it (the clock each minute).
//
// ScePaf loads at a different address each boot (Text::GetTypeName was at
// 0xE032F61B one boot and 0xE033A22B the next), so the first test, which read
// the dump's 0xE0161D60, crashed SceShell. Paf's font library and font cache
// are read from Paf's own code, the movw/movt pairs that load them, after
// checking Paf's segment sizes match the 3.65 dump's.
#define ENGRAM_PLACE_FONTS 1
#if ENGRAM_PLACE_FONTS
#define PL_EVERY 2 // the quick menu makes its texts as it opens
#define PL_EARLY 120
#define PL_LOGS 12 // per place
// Offsets into ScePaf's code (the dump's 0xE0208C20 base) of the movw/movt
// pairs that load its font library pointer and its font cache.
#define PAF_TEXT_SIZE 0x300D00
#define PAF_DATA_SIZE 0x166D0
#define PAF_LOADS_LIB 0x19B18E   // 0xE03A3DAE: movw/movt r0, the lib pointer
#define PAF_LOADS_CACHE 0x19B24A // 0xE03A3E6A: movw/movt r2, the cache

static uint32_t paf_pvf_lib, paf_font_cache; // found at run time, 0 if not

// A Thumb-2 movw/movt pair into one register: its 32-bit value, or 0.
static uint32_t thumb_movw_movt(uint32_t at, int reg) {
	const uint16_t *h = (const uint16_t *)at;
	if ((h[0] & 0xFBF0) != 0xF240 || (h[2] & 0xFBF0) != 0xF2C0 || (h[1] & 0x8000) || (h[3] & 0x8000))
		return 0;
	if (((h[1] >> 8) & 0xF) != reg || ((h[3] >> 8) & 0xF) != reg)
		return 0;
	uint32_t lo = ((h[0] & 0xF) << 12) | (((h[0] >> 10) & 1) << 11) | (((h[1] >> 12) & 7) << 8) | (h[1] & 0xFF);
	uint32_t hi = ((h[2] & 0xF) << 12) | (((h[2] >> 10) & 1) << 11) | (((h[3] >> 12) & 7) << 8) | (h[3] & 0xFF);
	return (hi << 16) | lo;
}

static bool find_paf_font_data(void) {
	tai_module_info_t tinfo;
	tinfo.size = sizeof(tinfo);
	if (taiGetModuleInfo("ScePaf", &tinfo) < 0)
		return false;
	SceKernelModuleInfo info;
	sceClibMemset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetModuleInfo(tinfo.modid, &info) < 0)
		return false;
	uint32_t text = (uint32_t)info.segments[0].vaddr, data = (uint32_t)info.segments[1].vaddr;
	log_line("place fonts: ScePaf text %08X (+0x%X), data %08X (+0x%X)\n", (unsigned)text,
	         (unsigned)info.segments[0].memsz, (unsigned)data, (unsigned)info.segments[1].memsz);
	if (info.segments[0].memsz != PAF_TEXT_SIZE || info.segments[1].memsz != PAF_DATA_SIZE)
		return false;
	uint32_t lib = thumb_movw_movt(text + PAF_LOADS_LIB, 0), cache = thumb_movw_movt(text + PAF_LOADS_CACHE, 2);
	log_line("place fonts: Paf's font library pointer %08X, font cache %08X\n", (unsigned)lib, (unsigned)cache);
	uint32_t end = data + PAF_DATA_SIZE;
	if (lib < data || lib + 4 > end || cache < data || cache + 8 > end)
		return false;
	paf_pvf_lib = lib;
	paf_font_cache = cache;
	return true;
}

extern "C" int paf_text_set_style_int(ui::Widget *self, int attr, unsigned index, unsigned len, int val)
	asm("_ZN3paf2ui4Text17SetStyleAttributeENS_5graph18TextStyleAttributeEjji");
extern "C" int paf_text_invalidate(ui::Widget *self) asm("_ZN3paf2ui4Text10InvalidateEv");

struct PlaceDef {
	const char *plugin;
	uint32_t scene; // 0: not found yet
	int family;
	const char *what;
	uint32_t root; // only this widget and what's under it, or 0 for the whole scene
};
static const PlaceDef PLACE_DEFS[PLACE_COUNT] = {
	{"power_manage_plugin", 0xA3C0A317, 5, "lock screen"}, // clock, date
	// The status bar and the notifications, one scene. Limiting the font to
	// the bar (3A375CEF) didn't change how the notifications looked, so the
	// user chose to keep both on it (2026-11-13).
	{"indicator_plugin", 0xFFD7E3F0, 6, "status bar", 0},
	{"coldboot_plugin", 0x9910C239, 7, "boot warning"},    // one text, 720x48, up by frame 6
	{"power_manage_plugin", 0xB4093C4B, 8, "quick menu"},  // its texts are made as it first opens
};
static struct {
	void *data;
	SceSize size;
	uint32_t obj; // the family's font object last given the font
	int on, logs, retags;
} places[PLACE_COUNT];
static int places_ready; // 0 not tried, 1 running, -1 off
static unsigned early_frames; // Paf's frames before on_frame starts (see early_frame)
static int pt_dumps;

// SceShell's free memory, in KB: logged as the place fonts load.
static int free_kb(void) {
	SceKernelFreeMemorySizeInfo info;
	sceClibMemset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetFreeMemorySize(&info) < 0)
		return -1;
	return info.size_user / 1024;
}

static bool place_load(int p, const char *file) {
	// A font another place already has is shared, not loaded again.
	for (int q = 0; q < p; q++)
		if (places[q].on && str_eq(cfg.place_font[q], file)) {
			places[p].data = places[q].data;
			places[p].size = places[q].size;
			return true;
		}
	char path[128];
	sceClibSnprintf(path, sizeof(path), FONT_DIR "/%s", file);
	SceIoStat st;
	if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0)
		return false;
	if (st.st_size > PLACE_FONT_MAX) {
		log_line("place fonts: %s is %u KB, more than the %u KB a place can have: left out\n", file,
		         (unsigned)(st.st_size / 1024), (unsigned)(PLACE_FONT_MAX / 1024));
		return false;
	}
	SceSize size = (SceSize)st.st_size;
	SceUID block = sceKernelAllocMemBlock("EngramPlaceFont", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
	                                      (size + 0xFFF) & ~0xFFFu, NULL);
	void *data = NULL;
	if (block < 0 || sceKernelGetMemBlockBase(block, &data) < 0)
		return false;
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	SceSize got = 0;
	if (fd >= 0) {
		while (got < size) {
			int n = sceIoRead(fd, (char *)data + got, size - got);
			if (n <= 0)
				break;
			got += n;
		}
		sceIoClose(fd);
	}
	if (got != size) {
		sceKernelFreeMemBlock(block);
		return false;
	}
	places[p].data = data;
	places[p].size = size;
	return true;
}

// The font cache's nodes: parent/root +0, left +4, right +8, kind +0xC,
// font object +0x10, nil +0x19 (ScePaf 0xE03A3E8E, 0xE0461C34). A font object
// is {vtable, ScePvfFontId +4, opened +8}.
static uint32_t pt_find(uint32_t kind) {
	uint32_t head = *(const uint32_t *)paf_font_cache;
	if (!looks_like_object((void *)head))
		return 0;
	uint32_t node = *(const uint32_t *)head;
	for (int steps = 0; steps < 64 && looks_like_object((void *)node) && !*(const int8_t *)(node + 0x19); steps++) {
		uint32_t key = *(const uint32_t *)(node + 0xC);
		if (kind == key)
			return *(const uint32_t *)(node + 0x10);
		node = *(const uint32_t *)(node + (kind < key ? 4 : 8));
	}
	return 0;
}

static void pt_dump(const char *when) {
	if (pt_dumps >= 8)
		return;
	pt_dumps++;
	uint32_t head = *(const uint32_t *)paf_font_cache, size = *(const uint32_t *)(paf_font_cache + 4);
	if (!looks_like_object((void *)head))
		return;
	char line[256];
	int len = sceClibSnprintf(line, sizeof(line), "place fonts: font cache %s: %u entries:", when, (unsigned)size);
	uint32_t stack[32];
	int top = 0, shown = 0;
	uint32_t root = *(const uint32_t *)head;
	if (looks_like_object((void *)root) && !*(const int8_t *)(root + 0x19))
		stack[top++] = root;
	while (top > 0 && shown < 12 && len < (int)sizeof(line) - 40) {
		uint32_t node = stack[--top];
		uint32_t obj = *(const uint32_t *)(node + 0x10);
		len += sceClibSnprintf(line + len, sizeof(line) - len, " %X=%08X", *(const unsigned *)(node + 0xC),
		                       looks_like_object((void *)obj) ? *(const unsigned *)(obj + 4) : 0);
		shown++;
		for (int side = 4; side <= 8; side += 4) {
			uint32_t next = *(const uint32_t *)(node + side);
			if (top < 32 && looks_like_object((void *)next) && !*(const int8_t *)(next + 0x19))
				stack[top++] = next;
		}
	}
	log_line("%s\n", line);
}

// Text that isn't a ui::Text: a widget's draw objects that are graph::TextObj,
// like a Button's label (draw object 1, OBJ_LABEL), reached through
// Widget::GetDrawObj, vtable slot 62. That's the headers' order, which matched
// the console at GetTypeName's slot 26, and Quick Menu Plus calls the same
// slot (0xF8, its get_style_obj) on the quick menu's buttons. An object
// counts as a TextObj when its vtable holds TextObj::GetDrawObjType. The
// quick menu's toggles and power buttons kept the Home Screen font without
// this (user report, 2026-11-13); the power buttons changed once their
// labels were reached.
extern "C" int paf_textobj_style_int(void *self, int attr, unsigned index, int *out)
	asm("_ZN3paf5graph7TextObj17GetStyleAttributeENS0_18TextStyleAttributeEjRi");
extern "C" int paf_textobj_set_style_int(void *self, int attr, unsigned index, unsigned len, int val)
	asm("_ZN3paf5graph7TextObj17SetStyleAttributeENS0_18TextStyleAttributeEjji");
extern "C" int paf_textobj_invalidate(void *self) asm("_ZN3paf5graph7TextObj10InvalidateEv");
extern "C" int paf_textobj_string(void *self, const wchar_t **buf, unsigned *len)
	asm("_ZN3paf5graph7TextObj15GetStringBufferERPKwRj");
extern "C" int paf_textobj_type(const void *self) asm("_ZNK3paf5graph7TextObj14GetDrawObjTypeEv");
#define WIDGET_GET_DRAW_OBJ 62
static uint32_t textobj_type_fn;

// Every TextObj a widget draws, among its draw objects 0-7. Widget::GetDrawObj
// (ScePaf 0xE0368412 in the dump, the one every widget class's vtable has at
// slot 62: text, button, composite_button, check_box, plane_button) looks the
// number up in the widget's own list of draw objects (+0x24C, 12-byte entries
// whose id is bits 3-10 of +8) and returns NULL if it isn't there, so asking
// for any number is safe.
#define DRAW_OBJ_IDS 8
static int widget_text_objs(ui::Widget *w, void **out, int max) {
	const uint32_t *vt = vtable_of(w);
	if (!vt)
		return 0;
	uint32_t fn = vt[WIDGET_GET_DRAW_OBJ];
	if (fn < 0x80000000u || fn >= 0xF0000000u)
		return 0;
	if (!textobj_type_fn)
		textobj_type_fn = stub_target((const void *)paf_textobj_type);
	if (!textobj_type_fn)
		return 0;
	int n = 0;
	for (int id = 0; id < DRAW_OBJ_IDS && n < max; id++) {
		void *obj = ((void *(*)(ui::Widget *, int))fn)(w, id);
		if (!looks_like_object(obj))
			continue;
		const uint32_t *ovt = *(const uint32_t *const *)obj;
		if (!looks_like_object(ovt))
			continue;
		for (int i = 0; i < 40; i++)
			if (ovt[i] == textobj_type_fn) {
				out[n++] = obj;
				break;
			}
	}
	return n;
}

// The label's first characters, for the log.
static void label_text(void *label, char *out, int size, unsigned *len_out) {
	const wchar_t *buf = NULL;
	unsigned len = 0;
	paf_textobj_string(label, &buf, &len);
	int n = 0;
	for (unsigned i = 0; buf && i < len && n < size - 1; i++)
		out[n++] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '?';
	out[n] = '\0';
	*len_out = len;
}

static void place_retag(int p, ui::Widget *w, int depth) {
	if (!looks_like_object(w) || depth > FP_DEPTH)
		return;
	if (!fp_is_text(w)) {
		void *objs[4];
		int count = widget_text_objs(w, objs, 4);
		// Button-like widgets with no text of their own, once each, to find
		// the labels this still misses.
		static uint32_t other_ids[16];
		static int other_count;
		const char *type = count ? NULL : fp_type_name(w);
		if (type && other_count < 16) {
			bool buttonish = false;
			for (const char *t = type; *t && !buttonish; t++)
				buttonish = str_starts(t, "button") || str_starts(t, "check");
			uint32_t id = id_of(w);
			bool seen = false;
			for (int k = 0; k < other_count && !seen; k++)
				seen = other_ids[k] == id;
			if (buttonish && !seen) {
				other_ids[other_count++] = id;
				char name[24];
				int n = 0;
				while (n < 23 && type[n] >= 0x20 && type[n] < 0x7F) {
					name[n] = type[n];
					n++;
				}
				name[n] = '\0';
				log_line("place fonts: %s has a %s %08X with no text objects (%d children)\n", PLACE_DEFS[p].what,
				         name, (unsigned)id, w->GetChildrenNum());
			}
		}
		for (int k = 0; k < count; k++) {
			void *label = objs[k];
			char text[28];
			unsigned len = 0;
			int family = -1;
			label_text(label, text, sizeof(text), &len);
			paf_textobj_style_int(label, 0, 0, &family);
			if (len > 0 && family != PLACE_DEFS[p].family) {
				int r = paf_textobj_set_style_int(label, 0 /* Family */, 0, len, PLACE_DEFS[p].family);
				paf_textobj_invalidate(label);
				places[p].retags++;
				if (places[p].logs < PL_LOGS * 3) {
					places[p].logs++;
					log_line("place fonts: %s widget %08X's text \"%s\": family %d -> %d (0x%08X) at frame %u\n",
					         PLACE_DEFS[p].what, (unsigned)id_of(w), text, family, PLACE_DEFS[p].family,
					         (unsigned)r, (unsigned)frame);
				}
			}
		}
	}
	if (fp_is_text(w)) {
		unsigned len = 0;
		int family = -1;
		paf_text_length(w, &len);
		paf_text_style_int(w, 0, 0, &family);
		if (len > 0 && family != PLACE_DEFS[p].family) {
			int r = paf_text_set_style_int(w, 0 /* Family */, 0, len, PLACE_DEFS[p].family);
			paf_text_invalidate(w);
			places[p].retags++;
			if (places[p].logs < PL_LOGS) {
				places[p].logs++;
				log_line("place fonts: %s text %08X, %u chars: family %d -> %d (0x%08X) at %sframe %u\n",
				         PLACE_DEFS[p].what, (unsigned)id_of(w), len, family, PLACE_DEFS[p].family, (unsigned)r,
				         frame ? "" : "early ", frame ? (unsigned)frame : early_frames);
			}
		}
	}
	int children = w->GetChildrenNum();
	if (children < 0 || children > 256)
		return;
	for (int i = 0; i < children; i++)
		place_retag(p, w->GetChild(i), depth + 1);
}

// After a swap, every text of the place is laid out again. Paf measures a
// text's letters with whatever font the family has when it lays it out, and
// the family's font object only appears once a text needs it, so the first
// layout used the system's font for that family: with Lobster swapped in
// two frames later, the boot warning showed only half its text (user report,
// 2026-11-13).
static int place_relayout(int p, ui::Widget *w, int depth) {
	if (!looks_like_object(w) || depth > FP_DEPTH)
		return 0;
	int done = 0;
	if (fp_is_text(w)) {
		int family = -1;
		paf_text_style_int(w, 0, 0, &family);
		if (family == PLACE_DEFS[p].family) {
			paf_text_invalidate(w);
			done++;
		}
	} else {
		void *objs[4];
		int count = widget_text_objs(w, objs, 4);
		for (int k = 0; k < count; k++) {
			int family = -1;
			paf_textobj_style_int(objs[k], 0, 0, &family);
			if (family == PLACE_DEFS[p].family) {
				paf_textobj_invalidate(objs[k]);
				done++;
			}
		}
	}
	int children = w->GetChildrenNum();
	if (children < 0 || children > 256)
		return done;
	for (int i = 0; i < children; i++)
		done += place_relayout(p, w->GetChild(i), depth + 1);
	return done;
}

static ui::Scene *place_scene(int p) {
	Plugin *plugin = Plugin::Find(PLACE_DEFS[p].plugin);
	if (!looks_like_object(plugin))
		return NULL;
	const uint32_t *words = (const uint32_t *)&plugin->draw_order;
	DrawOrderElement *pages = (DrawOrderElement *)words[0];
	uint32_t count = words[1];
	if (!looks_like_object(pages) || count > MAX_PAGES)
		return NULL;
	for (uint32_t i = 0; i < count; i++) {
		ui::Scene *scene = pages[i].page;
		if (looks_like_object(scene) && id_of(scene) == PLACE_DEFS[p].scene)
			return scene;
	}
	return NULL;
}

static void place_fonts(void) {
	// Every frame for the first 2 s, and before on_frame starts (early_frame).
	if (frame > PL_EARLY && frame % PL_EVERY)
		return;
	if (places_ready == 0) {
		bool any = false;
		for (int p = 0; p < PLACE_COUNT; p++)
			if (PLACE_DEFS[p].plugin && cfg.place_font[p][0])
				any = true;
		if (!any) {
			places_ready = -1;
			return;
		}
		if (!fp_text_fn)
			fp_text_fn = stub_target((const void *)paf_text_type_name);
		if (!find_paf_font_data()) {
			places_ready = -1;
			log_line("place fonts: Paf isn't the 3.65 build this was written for: off\n");
			return;
		}
		int free_before = free_kb();
		for (int p = 0; p < PLACE_COUNT; p++) {
			if (!PLACE_DEFS[p].plugin || !cfg.place_font[p][0])
				continue;
			places[p].on = place_load(p, cfg.place_font[p]);
			log_line("place fonts: %s font %s %s (%u bytes), family %d\n", PLACE_DEFS[p].what, cfg.place_font[p],
			         places[p].on ? "loaded" : "didn't load", (unsigned)places[p].size, PLACE_DEFS[p].family);
		}
		log_line("place fonts: SceShell's free memory %d KB before, %d KB after\n", free_before, free_kb());
		places_ready = 1;
		pt_dump("at the start");
	}
	if (places_ready < 0)
		return;
	for (int p = 0; p < PLACE_COUNT; p++) {
		if (!places[p].on)
			continue;
		ui::Widget *scene = place_scene(p);
		if (scene && PLACE_DEFS[p].root)
			scene = find_widget(scene, PLACE_DEFS[p].root, 0);
		if (scene)
			place_retag(p, scene, 0);
		uint32_t obj = pt_find((uint32_t)(PLACE_DEFS[p].family + 0xFFFE));
		if (obj && looks_like_object((void *)obj) && obj != places[p].obj) {
			void *lib = *(void *const *)paf_pvf_lib;
			ScePvfError err = 0;
			ScePvfFontId font =
				looks_like_object(lib) ? scePvfOpenUserMemory(lib, places[p].data, places[p].size, &err) : NULL;
			uint32_t was = *(const uint32_t *)(obj + 4);
			int relaid = 0;
			if (font && !err) {
				*(ScePvfFontId *)(obj + 4) = font;
				places[p].obj = obj;
				// The system's font Paf opened for this font object stays open.
				// Closing it here crashed SceShell at boot twice, straight after the
				// boot warning's swap (1.1 test, 2026-11-13): something in Paf still
				// uses it. One is left open per font object Paf makes for a family.
				if (scene)
					relaid = place_relayout(p, scene, 0);
			}
			log_line("place fonts: %s: family %d's font object %08X had font %08X: now %p (0x%08X) at %sframe %u, "
			         "%d texts laid out again\n",
			         PLACE_DEFS[p].what, PLACE_DEFS[p].family, (unsigned)obj, (unsigned)was, font, (unsigned)err,
			         frame ? "" : "early ", frame ? (unsigned)frame : early_frames, relaid);
			pt_dump("after a swap");
		}
	}
}
#endif

// ---- Boot warning probe ----
//
// The warning SceShell shows before the lock screen is Paf text from
// SceShell's scePafToplevelGetText, message 0x76A1B071 or 0x72411882 (as
// VitaTweaks' CustomWarning has it). This logs when it's asked for, beside
// the font probe's scenes, to find which part of SceShell draws it.
static SceUID gettext_hook = -1;
static tai_hook_ref_t gettext_ref;
static int warning_logs;

static void *shell_get_text_patched(void *a0, void *idp) {
	if (idp && warning_logs < 6) {
		uint32_t id = *(const uint32_t *)((const char *)idp + 0xC);
		if (id == 0x76A1B071 || id == 0x72411882) {
			warning_logs++;
			log_line("fontprobe: the boot warning's text %08X asked for at frame %u%s\n", (unsigned)id,
			         (unsigned)frame, frame ? "" : " (Engram's frame callback isn't running yet)");
		}
	}
	return TAI_NEXT(shell_get_text_patched, gettext_ref, a0, idp);
}

void shell_paf_loaded(void) {
	if (!ENGRAM_FONT_PROBE || gettext_hook >= 0)
		return;
	gettext_hook = taiHookFunctionImport(&gettext_ref, "SceShell", 0x4D9A9DD0, 0x19CEFDA7,
	                                     (const void *)shell_get_text_patched);
	log_line("fontprobe: SceShell's text hook 0x%08X\n", (unsigned)gettext_hook);
}

// ---- App page probe ----
//
// Off: its findings are in App Page Animation below. Set to 1 to look again.
#define ENGRAM_APP_PROBE 0
//
// For App Page Animation (planned): what an app's LiveArea page is made of,
// and what moves as you swipe between the apps' pages. The pages belong to
// livearea_plugin (shelltheme watched it); none were open when the 2026-11-05
// probe ran. This logs that plugin's pages as they open and close, each one's
// widget tree the first time it shows, and every widget holding children laid
// out side by side a screen apart (a pager, as the home screen has up and
// down), with where it and its children are whenever they move.
#define LA_EVERY 6           // frames between looks
#define LA_PAGES_MAX 8
#define LA_TREE_DEPTH 11
#define LA_TREE_LINES 420
#define LA_DUMPS 6
#define LA_LIST_LOGS 24
#define LA_MOVE_LOGS 160
#define LA_PAGERS 4
#define LA_PAGER_SPAN 800.0f // children this far apart sideways look like screens

#if ENGRAM_APP_PROBE
// Written in one go: one log_line per widget would stutter the screen.
static char dump_buf[49152];
static int dump_len;

static void dump_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dump_add(const char *fmt, ...) {
	int room = (int)sizeof(dump_buf) - dump_len;
	if (room <= 1)
		return;
	va_list args;
	va_start(args, fmt);
	int n = sceClibVsnprintf(dump_buf + dump_len, room, fmt, args);
	va_end(args);
	if (n > 0)
		dump_len += n < room ? n : room - 1;
}

static void dump_flush(void) {
	if (dump_len <= 0)
		return;
	SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0) {
		sceIoWrite(fd, dump_buf, dump_len);
		sceIoClose(fd);
	}
	dump_len = 0;
}

static uint32_t la_signature[2];
static int la_list_logs, la_dumps, la_move_logs, la_tree_lines;
static uint32_t la_dumped[LA_DUMPS]; // page id and child count, so a page is dumped again when it grows
static struct {
	uint32_t id;
	int x, child0, child1;
} la_pager_last[LA_PAGERS];

// The home screen's own icon pages (the scroll box) and the icons inside icon
// containers are left out: they're known, and there are hundreds.
static void la_tree(ui::Widget *w, int depth) {
	if (!looks_like_object(w) || depth > LA_TREE_DEPTH || la_tree_lines >= LA_TREE_LINES)
		return;
	if (w == scroll_box) {
		la_tree_lines++;
		dump_add("  %.*s(the home pages' scroll box)\n", depth * 2, "                      ");
		return;
	}
	la_tree_lines++;
	int children = w->GetChildrenNum();
	const float *pos = pos_of(w), *size = size_of(w), *scale = scale_of(w);
	dump_add("  %.*s%08X vt %08X %dx%d pos (%d,%d,%d) scale %d%% visible %d, %d children\n", depth * 2,
	         "                    ", (unsigned)id_of(w), *(const unsigned *)w, (int)size[0], (int)size[1], (int)pos[0],
	         (int)pos[1], (int)pos[2], (int)(scale[0] * 100), w->IsVisible() ? 1 : 0, children);
	if (children < 0 || children > MAX_CHILDREN || id_of(w) == BUBBLE_CONTAINER)
		return;
	for (int i = 0; i < children; i++)
		la_tree(w->GetChild(i), depth + 1);
}

// Widgets whose children sit a screen apart sideways.
static int la_find_pagers(ui::Widget *w, int depth, ui::Widget **out, int n) {
	if (!looks_like_object(w) || depth > LA_TREE_DEPTH || n >= LA_PAGERS || w == scroll_box)
		return n;
	int children = w->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return n;
	if (children >= 2) {
		float lo = 1e9f, hi = -1e9f;
		for (int i = 0; i < children; i++) {
			ui::Widget *c = w->GetChild(i);
			if (!looks_like_object(c))
				continue;
			float x = pos_of(c)[0];
			lo = x < lo ? x : lo;
			hi = x > hi ? x : hi;
		}
		if (hi - lo >= LA_PAGER_SPAN && n < LA_PAGERS)
			out[n++] = w;
	}
	for (int i = 0; i < children && n < LA_PAGERS; i++)
		n = la_find_pagers(w->GetChild(i), depth + 1, out, n);
	return n;
}

static void probe_plugin(int which, const char *name) {
	Plugin *plugin = Plugin::Find(name);
	if (!looks_like_object(plugin))
		return;
	const uint32_t *words = (const uint32_t *)&plugin->draw_order;
	DrawOrderElement *pages = (DrawOrderElement *)words[0];
	uint32_t count = words[1];
	if (count > LA_PAGES_MAX || (count > 0 && !looks_like_object(pages)))
		return;

	// The plugin's pages, as they open, close, show and hide.
	uint32_t sig = count * 2654435761u;
	for (uint32_t i = 0; i < count; i++) {
		ui::Scene *scene = pages[i].page;
		if (looks_like_object(scene))
			sig = (sig ^ (id_of(scene) + (scene->IsVisible() ? 1u : 0u))) * 16777619u;
	}
	if (sig != la_signature[which] && la_list_logs < LA_LIST_LOGS) {
		la_signature[which] = sig;
		la_list_logs++;
		dump_add("app pages at frame %u: %u in %s\n", (unsigned)frame, (unsigned)count, name);
		for (uint32_t i = 0; i < count; i++) {
			ui::Scene *scene = pages[i].page;
			if (!looks_like_object(scene))
				continue;
			const float *pos = pos_of(scene);
			dump_add("  page %08X visible %d pos (%d,%d) priority %d/%d, %d children\n", (unsigned)id_of(scene),
			         scene->IsVisible() ? 1 : 0, (int)pos[0], (int)pos[1], (int)pages[i].view_priority,
			         (int)pages[i].page_priority, scene->GetChildrenNum());
		}
	}

	for (uint32_t i = 0; i < count; i++) {
		ui::Scene *scene = pages[i].page;
		if (!looks_like_object(scene) || !scene->IsVisible())
			continue;
		uint32_t id = id_of(scene);
		uint32_t key = id + (uint32_t)scene->GetChildrenNum() * 0x9E3779B9u;
		bool dumped = false;
		for (int k = 0; k < la_dumps; k++)
			dumped = dumped || la_dumped[k] == key;
		// The home page only once it has grown past its usual 3 children (the
		// app pages seem to live in it: it gained a 4th as one opened).
		bool wanted = id != HOME_PAGE || scene->GetChildrenNum() > 3;
		if (!dumped && wanted && la_dumps < LA_DUMPS) {
			la_dumped[la_dumps++] = key;
			la_tree_lines = 0;
			dump_add("app page %08X's tree at frame %u (%d children):\n", (unsigned)id, (unsigned)frame,
			         scene->GetChildrenNum());
			la_tree(scene, 0);
		}
		// What moves sideways as you swipe.
		ui::Widget *pagers[LA_PAGERS];
		int n = la_find_pagers(scene, 0, pagers, 0);
		for (int k = 0; k < n && la_move_logs < LA_MOVE_LOGS; k++) {
			ui::Widget *pg = pagers[k];
			int x = (int)pos_of(pg)[0];
			ui::Widget *c0 = pg->GetChild(0), *c1 = pg->GetChild(1);
			int x0 = looks_like_object(c0) ? (int)pos_of(c0)[0] : 0, x1 = looks_like_object(c1) ? (int)pos_of(c1)[0] : 0;
			uint32_t pid = id_of(pg);
			int slot = -1;
			for (int j = 0; j < LA_PAGERS; j++)
				if (la_pager_last[j].id == pid)
					slot = j;
			if (slot < 0) {
				for (int j = 0; j < LA_PAGERS && slot < 0; j++)
					if (la_pager_last[j].id == 0)
						slot = j;
				if (slot < 0)
					continue;
				la_pager_last[slot].id = pid;
				la_pager_last[slot].x = x + 1; // log it the first time
			}
			if (la_pager_last[slot].x != x || la_pager_last[slot].child0 != x0 || la_pager_last[slot].child1 != x1) {
				la_move_logs++;
				dump_add("pager %08X at frame %u: x %d, %d children, first at x %d, second at x %d\n", (unsigned)pid,
				         (unsigned)frame, x, pg->GetChildrenNum(), x0, x1);
				la_pager_last[slot].x = x;
				la_pager_last[slot].child0 = x0;
				la_pager_last[slot].child1 = x1;
			}
		}
	}
}

// The app pages belong to livearea_plugin by shelltheme's watching, or sit in
// topmenu_plugin's tree by the probe's notes: both are looked at.
static void probe_livearea(void) {
	if (frame % LA_EVERY != 0)
		return;
	probe_plugin(0, "livearea_plugin");
	probe_plugin(1, "topmenu_plugin");
	dump_flush();
}
#endif

// ---- App Page Animation ----
//
// From the probe (2026-11-12): the home screen and the apps' LiveArea pages
// are one strip that slides sideways, inside page_top. 0x9CC405A3 holds it as
// its first child, and the strip's children are the screens, 960 px apart:
// the home screen (0x8FCDE1B9, 960 wide) at x = 0, then one app page (840
// wide, the rest is room for the arrows) per app you've opened. Swiping moves
// the strip: at x = -960 the first app page is in front.
//
// Each page is turned, moved and faded as a whole, as one card, by the scroll
// effects run with a page's length along the swipe, and turned on their side
// on the way out; each goes back exactly as it was when the strip settles.
//
// This is how the second test build did it, which the user found the
// smoothest. Later builds tried moving only what's inside each page (SceShell
// reads the pages' positions to tell where the strip should stop), easing the
// effect in and out over time, landing the page early, a farther eye and
// turning pages out instead of fading them: each made the swipe look worse.
#define APP_STRIP_HOLDER 0x9CC405A3
#define APP_PAGE_STEP 960.0f  // the strip's screens are this far apart
#define APP_PAGE_MIN_W 800.0f // the screens: the home screen is 960 wide, app pages 840
// Inside an app page: 0x18411BF4, then 0xB34A295B, whose first child is the
// background you can peel off (a 3D page-turn, paf::ui::PageTurn, which
// ignores every alpha: fading never reached it).
#define APP_PAGE_INNER 0x18411BF4
#define APP_PAGE_BODY 0xB34A295B
#define APP_LOG_LIMIT 4
#define META_LOG_LIMIT 2

static bool app_was_moving;
static int app_random_effect = -1;
static int app_logs, meta_logs;
static ui::Widget *app_origin; // the page the swipe started from; compared, never dereferenced

// This swipe's effect, or -1 for none. Random keeps one for the whole swipe.
static int choose_app_effect(bool swipe_started) {
	int choice = cfg.app_effect;
	if (choice < 0 || choice >= APP_EFFECT_COUNT)
		return -1;
	int fx = APP_EFFECT_CHOICES[choice];
	if (fx != APP_EFFECT_RANDOM)
		return fx;
	if (swipe_started || app_random_effect < 0)
		app_random_effect = APP_EFFECT_CHOICES[next_random() % (APP_EFFECT_COUNT - 1)];
	return app_random_effect;
}

static ui::Widget *app_page_background(ui::Widget *page) {
	ui::Widget *inner = child_with_id(page, APP_PAGE_INNER);
	ui::Widget *body = inner ? child_with_id(inner, APP_PAGE_BODY) : NULL;
	if (!body || body->GetChildrenNum() < 1)
		return NULL;
	ui::Widget *bg = body->GetChild(0);
	return looks_like_object(bg) && size_of(bg)[0] >= APP_PAGE_MIN_W ? bg : NULL;
}

// `fade` multiplies the page's alpha: Fade Out or Fade In, 1 for neither.
static void animate_app_page(ui::Widget *w, float d, int effect, float fade) {
	Track *t = find_track(w);
	if (!t)
		t = add_track(w);
	if (!t)
		return;
	t->seen = frame;
	// SceShell opening or peeling a page off: left to it.
	uint32_t busy = busy_mask(w) & (BUSY_POS | BUSY_SCALE | BUSY_ROT);
	if (busy) {
		if (t->modified)
			restore(t, busy);
		return;
	}
	adopt_changes(t);
	// The strip's pages rest upright at full size: SceShell's own effects are on
	// widgets inside them. Taking a size SceShell was still zooming as the page's
	// own would put it back a little small.
	for (int i = 0; i < 3; i++) {
		t->base_rot[i] = 0.0f;
		t->base_scale[i] = 1.0f;
	}

	// In the effects' own terms the page scrolls up and down, its middle at
	// (0, d): run them so, then turn the result on its side.
	IconCtx c;
	c.bx = 0.0f;
	c.by = 0.0f;
	c.d = d;
	c.span = APP_PAGE_STEP;
	c.across = PAGE_H;
	c.eye = PERSPECTIVE;
	c.p = d / APP_PAGE_STEP;
	c.seed = id_of(w);
	c.tilt = true;
	Xform x;
	xform_identity(&x, &c);
	if (effect >= 0)
		effect_apply(effect, &x, &c);
	effect_intensity(&x, &c, intensity_factor(cfg.app_intensity));
	x.alpha *= fade; // after Intensity, so the page always fades out completely

	// Along the effects' scroll is across the screen here, and a turn about
	// their horizontal axis is one about the vertical: the far side going away
	// from you (y up, so swapping the axes mirrors the turn's direction).
	float pos[4] = {t->base_pos[0] + (x.y - d), t->base_pos[1] + x.x, t->base_pos[2], t->base_pos[3]};
	float scale[4] = {t->base_scale[0] * x.sy, t->base_scale[1] * x.sx, t->base_scale[2], t->base_scale[3]};
	float rot[4] = {t->base_rot[0], t->base_rot[1] - x.rx, t->base_rot[2] - x.rz, t->base_rot[3]};
	if (!same3(pos_of(w), pos))
		set_pos(w, pos);
	if (!same3(scale_of(w), scale)) {
		set_scale(w, scale);
		t->scale_touched = true;
	}
	if (!same3(rot_of(w), rot)) {
		set_rot(w, rot);
		t->rot_touched = true;
	}
	// Faded by its meta alpha, which reaches what's on the page (its colour
	// doesn't); the peelable background ignores both.
	if (x.alpha < 0.999f || t->meta_touched) {
		if (!t->meta_touched) {
			float own = w->GetMetaAlpha();
			t->base_meta = own > 0.05f && own <= 1.0f ? own : 1.0f;
			if (meta_logs < META_LOG_LIMIT) {
				meta_logs++;
				log_line("app pages: page %08X's own meta alpha %d%%\\n", (unsigned)id_of(w), (int)(own * 100));
			}
		}
		float a = x.alpha < MIN_ALPHA ? MIN_ALPHA : (x.alpha > 1.0f ? 1.0f : x.alpha);
		a *= t->base_meta;
		if (!t->meta_touched || fabsf_(a - t->meta) > 0.002f) {
			w->SetMetaAlpha(a);
			t->meta = a;
		}
		t->meta_touched = true;
	}
	remember_applied(t);
}

// Puts every page of the strip back, for when Engram stops looking (an app
// opening hides the home page, and Engram rests): a page left mid-effect,
// faded say, would stay so behind the app and when you come back.
static void app_pages_settle(ui::Scene *scene) {
	ui::Widget *holder = child_with_id(scene, APP_STRIP_HOLDER);
	ui::Widget *strip = holder && holder->GetChildrenNum() > 0 ? holder->GetChild(0) : NULL;
	int children = looks_like_object(strip) ? strip->GetChildrenNum() : 0;
	if (children < 0 || children > MAX_CHILDREN)
		return;
	for (int i = 0; i < children; i++) {
		ui::Widget *page = strip->GetChild(i);
		if (!looks_like_object(page) || size_of(page)[0] < APP_PAGE_MIN_W)
			continue;
		if (find_track(page))
			settle(page, busy_mask(page) & ~BUSY_COLOR);
		// Inner widgets a later test build moved, put back too.
		ui::Widget *inner = child_with_id(page, APP_PAGE_INNER);
		ui::Widget *body = inner ? child_with_id(inner, APP_PAGE_BODY) : NULL;
		ui::Widget *bg = app_page_background(page);
		ui::Widget *others[3] = {inner, body, bg};
		for (int k = 0; k < 3; k++)
			if (others[k] && find_track(others[k]))
				settle(others[k], busy_mask(others[k]) & ~BUSY_COLOR);
	}
	app_was_moving = false;
	app_origin = NULL;
}

static void animate_app_pages(ui::Scene *scene) {
	ui::Widget *holder = child_with_id(scene, APP_STRIP_HOLDER);
	if (!holder || holder->GetChildrenNum() < 1)
		return;
	ui::Widget *strip = holder->GetChild(0);
	if (!looks_like_object(strip))
		return;
	int children = strip->GetChildrenNum();
	if (children < 0 || children > MAX_CHILDREN)
		return;
	float sx = pos_of(strip)[0];
	float nearest = (float)(int)(sx / APP_PAGE_STEP + (sx < 0 ? -0.5f : 0.5f)) * APP_PAGE_STEP;
	bool moving = fabsf_(sx - nearest) > REST_EPSILON;
	bool started = moving && !app_was_moving;
	app_was_moving = moving;
	bool active = cfg.app_enabled && !stop_requested && !edit_mode && moving;
	int effect = active ? choose_app_effect(started) : -1;
	bool fade_on = active && cfg.app_fade_out != FADE_OUT_OFF;
	bool fade_in_on = active && cfg.app_fade_in != FADE_OUT_OFF;

	// The page you swipe away from is the one nearest the screen as the swipe
	// starts. It fades out; the others on the screen fade in.
	if (!moving) {
		app_origin = NULL;
	} else if (started) {
		float best = APP_PAGE_STEP;
		for (int i = 0; i < children; i++) {
			ui::Widget *page = strip->GetChild(i);
			if (!looks_like_object(page) || size_of(page)[0] < APP_PAGE_MIN_W)
				continue;
			float d = fabsf_(sx + resting_pos(page)[0]);
			if (d < best) {
				best = d;
				app_origin = page;
			}
		}
	}

	int animated = 0;
	for (int i = 0; i < children; i++) {
		ui::Widget *page = strip->GetChild(i);
		if (!looks_like_object(page) || size_of(page)[0] < APP_PAGE_MIN_W)
			continue;
		float d = sx + resting_pos(page)[0]; // where it is on the screen, 0 in front
		bool on_screen = fabsf_(d) > REST_EPSILON && fabsf_(d) < APP_PAGE_STEP - REST_EPSILON;
		float fade = 1.0f;
		bool fading = false;
		if (fade_on && page == app_origin) {
			fade = fade_out_alpha(fabsf_(d) / APP_PAGE_STEP, cfg.app_fade_out);
			fading = true;
		} else if (fade_in_on && page != app_origin) {
			fade = fade_in_alpha(fabsf_(d) / APP_PAGE_STEP, cfg.app_fade_in);
			fading = true;
		}
		if ((effect >= 0 || fading) && on_screen) {
			animate_app_page(page, d, effect, fade);
			animated++;
		} else if (find_track(page)) {
			settle(page, busy_mask(page));
		}
	}
	if (started && app_logs < APP_LOG_LIMIT) {
		app_logs++;
		log_line("app pages: swipe at frame %u, strip x %d, %d children, effect %d, fade out %d, in %d, %d pages\\n",
		         (unsigned)frame, (int)sx, children, effect, fade_on ? cfg.app_fade_out : 0,
		         fade_in_on ? cfg.app_fade_in : 0, animated);
	}
}

// ---- Wallpaper Parallax (second try) ----
//
// The first try moved widgets that weren't the wallpaper (1.2 to 1.6). App Page
// Animation then showed that moving the whole home screen moves the wallpaper
// with it, so it's drawn by a widget in there: which one is chosen in Settings
// (enum WallLayer) while testing. The layer moves the other way to Motion
// Depth's float, zoomed a little so its edges don't show, and what's inside it
// is moved back by as much, so the icons stay where Motion Depth puts them.
#define PAR_ZOOM 1.1f
#define PAR_AMOUNT 0.5f // of the icons' float
#define PAR_STEP 0.05f  // per frame: the zoom eases in and out over a third of a second
#define PAR_LOG_LIMIT 3
static float par_master;
static int par_logs;

static Track *track_for(ui::Widget *w) {
	Track *t = find_track(w);
	return t ? t : add_track(w);
}

static void apply_tracked(Track *t, const float *pos, const float *scale) {
	ui::Widget *w = t->widget;
	if (!same3(pos_of(w), pos))
		set_pos(w, pos);
	if (!same3(scale_of(w), scale)) {
		set_scale(w, scale);
		t->scale_touched = true;
	}
	remember_applied(t);
}

static void par_layer(ui::Widget *layer, ui::Widget *inside, float float_x, float float_y, float k) {
	if (!looks_like_object(layer) || !looks_like_object(inside))
		return;
	Track *tl = track_for(layer), *ti = track_for(inside);
	if (!tl || !ti)
		return;
	tl->seen = ti->seen = frame;
	uint32_t bl = busy_mask(layer) & (BUSY_POS | BUSY_SCALE | BUSY_ROT);
	uint32_t bi = busy_mask(inside) & (BUSY_POS | BUSY_SCALE | BUSY_ROT);
	if (bl || bi) {
		if (tl->modified)
			restore(tl, bl);
		if (ti->modified)
			restore(ti, bi);
		return;
	}
	adopt_changes(tl);
	adopt_changes(ti);
	float zoom = 1.0f + (PAR_ZOOM - 1.0f) * k;
	float ox = -float_x * PAR_AMOUNT * k, oy = -float_y * PAR_AMOUNT * k; // the other way to the icons
	float lp[4] = {tl->base_pos[0] + ox, tl->base_pos[1] + oy, tl->base_pos[2], tl->base_pos[3]};
	float ls[4] = {tl->base_scale[0] * zoom, tl->base_scale[1] * zoom, tl->base_scale[2], tl->base_scale[3]};
	// Inside the layer, positions are scaled by its zoom: so that what's inside
	// lands where it was on the screen, its offset is undone and divided out.
	const float *bp = ti->base_pos;
	float ip[4] = {bp[0] + (bp[0] * (1.0f - zoom) - ox) / zoom, bp[1] + (bp[1] * (1.0f - zoom) - oy) / zoom, bp[2],
	               bp[3]};
	float is[4] = {ti->base_scale[0] / zoom, ti->base_scale[1] / zoom, ti->base_scale[2], ti->base_scale[3]};
	apply_tracked(tl, lp, ls);
	apply_tracked(ti, ip, is);
	if (par_logs < PAR_LOG_LIMIT && k > 0.999f) {
		par_logs++;
		log_line("wallpaper: layer %c, %08X moved (%d,%d) at %d%%, %08X back\n", 'A' + cfg.wall_layer,
		         (unsigned)id_of(layer), (int)ox, (int)oy, (int)(zoom * 100), (unsigned)id_of(inside));
	}
}

static void settle_tracked(ui::Widget *w) {
	if (looks_like_object(w) && find_track(w))
		settle(w, busy_mask(w));
}

static void forget_home(void) {
	home_scene = NULL;
	scroll_box = NULL;
	folder_container_count = 0;
	origin_page = NULL;
	edit_mode = false;
	edit_signal_frames = edit_calm_frames = 0;
	straightened_n = 0;
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

#if ENGRAM_APP_PROBE
	probe_livearea();
#endif
#if ENGRAM_FONT_PROBE
	font_probe();
#endif
#if ENGRAM_PLACE_FONTS
	place_fonts();
#endif

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
	// With an app in front, SceShell hides the home page (1.1's logs) but keeps
	// calling this every frame. Engram rests then, to save battery: Motion Depth
	// stops sampling the gyroscope (if Engram started it) and nothing else runs.
	// The icons are left as they are, out of sight, and Engram's record of them
	// is kept, so it picks up where it left off when the home page shows again.
	if (!stop_requested) {
		int visible = scene->IsVisible() ? 1 : 0;
		if (visible != visible_last && visible_logs < VISIBLE_LOG_LIMIT) {
			visible_logs++;
			log_line("home page %s at frame %u\n", visible ? "showing" : "hidden: Engram rests", (unsigned)frame);
		}
		if (!visible) {
			if (visible_last != 0) {
				motion_off();
				app_pages_settle(scene);
			}
			visible_last = 0;
			return;
		}
		if (visible_last == 0) {
			depth_hold_until = frame + DEPTH_HOLD_FRAMES; // SceShell brings the icons back in
			for (int i = 0; i < track_count; i++)
				tracks[i].seen = frame - 1; // not forgotten for having been out of sight (but not
				                            // "seen this frame" either, until found in the tree)
		}
		visible_last = 1;
	}
	animate_app_pages(scene);

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
	folder_container_count = 0;
	// Always: folder icons may need straightening with every tweak off.
	find_folder_containers(scene, NULL, 0);
	if (folder_container_count == 0)
		straightened_n = 0; // no folder open: the next one's icons are new
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

	if (!(cfg.enabled || cfg.layout != LAYOUT_GRID || cfg.folder_place) || stop_requested)
		edit_mode = false;
	else
		update_edit_mode(plane, plane_y, page_count);

	int effects[FX_COUNT];
	int effect_count = 0;
	bool active = cfg.enabled && !stop_requested && !edit_mode;
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
	// Icon Placement, on the home screen and in folders.
	// Both ease out in edit mode, and back in after it.
	bool place_run = view_update(&home_view, edit_mode ? (int)LAYOUT_GRID : cfg.layout, cfg.layout_strength);
	bool folder_run = view_update(
		&folder_view, cfg.folder_place && !edit_mode ? cfg.folder_layout + 1 : (int)LAYOUT_GRID, cfg.folder_strength);
	sync_folders(folder_run);
	bool shadows_want = !ENGRAM_SHADOWS_SHELVED && cfg.shadows && !stop_requested && shadow_failures < SHADOW_FAIL_LIMIT;
	bool shadows_run = shadows_want || shadows_made;
	float float_x = 0.0f, float_y = 0.0f;
	depth_float(shown_x, shown_y, cfg.depth_strength, &float_x, &float_y);
	bool floats = cfg.depth_style != DEPTH_TURN;
	int shadow_budget = SHADOW_CREATE_PER_FRAME;
	{
		bool want = !ENGRAM_PARALLAX_SHELVED && cfg.wallpaper && cfg.depth && floats && depth_moving &&
		            !stop_requested && !edit_mode;
		float target = want ? 1.0f : 0.0f;
		if (par_master < target)
			par_master = par_master + PAR_STEP > target ? target : par_master + PAR_STEP;
		else if (par_master > target)
			par_master = par_master - PAR_STEP < target ? target : par_master - PAR_STEP;
	}
	bool par_run = par_master > 0.0f;
	{
		ui::Widget *holder = *(ui::Widget **)((char *)scroll_box + WIDGET_PARENT_OFFSET);
		if (par_run && cfg.wall_layer == WALL_HOME) {
			par_layer(holder, scroll_box, float_x, float_y, par_master);
		} else {
			settle_tracked(holder);
			settle_tracked(scroll_box);
		}
	}
	if (effect_count == 0 && !fade_on && !any_modified && !depth_moving && !shadows_run && !wall_run && !place_run &&
	    !par_run)
		goto done;

	for (int i = 0; i < page_count; i++) {
		ui::Widget *page = plane->GetChild(i);
		if (!looks_like_object(page))
			continue;
		float d = plane_y + resting_pos(page)[1]; // resting: Wallpaper Parallax may have moved it
		bool on_screen = fabsf_(d) > REST_EPSILON && fabsf_(d) < PAGE_H - REST_EPSILON;
		bool fading = fade_on && page == origin_page;
		bool scrolling = (effect_count > 0 || fading) && on_screen;
		bool tilting = depth_moving && fabsf_(d) < PAGE_H - REST_EPSILON; // the page at rest too
		bool animating = scrolling || tilting;
		bool shadowed = shadows_run && fabsf_(d) < PAGE_H - REST_EPSILON;
		bool walled = wall_run && fabsf_(d) < PAGE_H - REST_EPSILON;
		if (on_screen)
			pages_on_screen++;
		if (!animating && !any_modified && !shadowed && !walled && !place_run && !par_run)
			continue;

		// The wallpaper first: it can be there when the bubble container isn't.
		float wall = 0.0f; // how far this page's shadows follow its wallpaper: only if it moves
		if (!ENGRAM_SHADOWS_SHELVED && (walled || any_modified)) {
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
		{
			ui::Widget *inner = page->GetChild(0);
			bool par_page = par_run && fabsf_(d) < PAGE_H - REST_EPSILON;
			if (par_page && cfg.wall_layer == WALL_PAGE_INSIDE) {
				par_layer(inner, container, float_x, float_y, par_master);
				settle_tracked(page);
			} else if (par_page && cfg.wall_layer == WALL_PAGE) {
				par_layer(page, inner, float_x, float_y, par_master);
				settle_tracked(container);
			} else {
				settle_tracked(page);
				settle_tracked(inner);
				settle_tracked(container);
			}
		}
		int children = container->GetChildrenNum();
		if (children < 0 || children > MAX_CHILDREN)
			continue;
		bool placing = place_run && page_placeable(page, container, children);
		// Eased into in front of you; already in place coming onto the screen.
		int place_mode = !placing ? PLACE_NONE : (fabsf_(d) > REST_EPSILON ? PLACE_INSTANT : PLACE_EASE);
		if (animating || any_modified || place_run) {
			for (int b = 0; b < children; b++) {
				ui::Widget *bubble = container->GetChild(b);
				if (!is_bubble(bubble))
					continue;
				if (animating || placing) {
					animate(bubble, d, effects, scrolling ? effect_count : 0, fading && scrolling, tilting,
					        place_mode, &home_view, GRID_HOME);
					animated++;
				} else {
					// While placing, a page given back may be mid-animation: leave
					// SceShell's animations be.
					settle(bubble, place_run ? busy_mask(bubble) : 0);
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

// ---- place fonts from Paf's first frame ----
//
// The boot warning (coldboot_plugin) is drawn before SceShell gets as far as
// sceAVConfigRegisterCallback, where on_frame starts, so it showed in the
// Home Screen font and changed to its own a moment later (user report,
// 2026-11-13). This callback goes in on SceShell's main thread as soon as Paf's
// Framework is made (font.cpp), runs only the place fonts, and takes itself
// out once on_frame is running.
static int early_registered;

static void early_frame(void *arg) {
	if (frame > 0 || stop_requested) {
		common::MainThreadCallList::Unregister(early_frame, arg);
		return;
	}
	early_frames++;
#if ENGRAM_PLACE_FONTS
	place_fonts();
#endif
}

void shell_framework_ready(void) {
	if (early_registered)
		return;
	early_registered = 1;
	common::MainThreadCallList::Register(early_frame, NULL);
	log_line("place fonts: checking from Paf's first frame\n");
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
			         "layout=%d layout_strength=%d\n",
			         now.enabled, now.random, now.intensity, now.tilt, now.fade_out, now.depth, now.depth_style,
			         now.depth_strength, now.layout, now.layout_strength);
		}
		sceKernelDelayThread(CONFIG_POLL_US);
	}
	return sceKernelExitThread(0);
}

// BOOTING_PATH holds how many boots in a row have started Engram without
// getting 30 s past it: one digit. 1.0's empty marker counts as one.
#define GUARD_SHORT_BOOTS 2

static int short_boots_before(void) {
	SceUID fd = sceIoOpen(BOOTING_PATH, SCE_O_RDONLY, 0);
	if (fd < 0)
		return 0;
	char c = 0;
	int n = sceIoRead(fd, &c, 1);
	sceIoClose(fd);
	return (n == 1 && c >= '1' && c <= '9') ? c - '0' : 1;
}

static void set_short_boots(int count) {
	SceUID fd = sceIoOpen(BOOTING_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	char c = (char)('0' + (count > 9 ? 9 : count));
	sceIoWrite(fd, &c, 1);
	sceIoClose(fd);
}

#define LOG_KEEP (256 * 1024)
#define LOG_OLD_PATH DATA_DIR "/log.old.txt"

static void rotate_log(void) {
	SceIoStat st;
	if (sceIoGetstat(LOG_PATH, &st) < 0 || st.st_size < LOG_KEEP)
		return;
	sceIoRemove(LOG_OLD_PATH);
	sceIoRename(LOG_PATH, LOG_OLD_PATH);
}

int shell_start(void) {
	rotate_log();
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

	// The crash guard: Engram turns itself off after GUARD_SHORT_BOOTS boots in
	// a row that didn't get 30 s past starting it. One wasn't enough: a restart
	// from the quick menu 20 s after boot, to apply a font, looked the same as
	// a crash and turned Engram off (1.1 test, 2026-11-13).
	int short_boots = short_boots_before();
	if (short_boots >= GUARD_SHORT_BOOTS) {
		write_marker(DISABLED_PATH);
		sceIoRemove(BOOTING_PATH);
		log_line("the last %d boots didn't get 30 s past starting Engram: wrote %s, staying off\n", short_boots,
		         DISABLED_PATH);
		return 0;
	}
	if (short_boots > 0)
		log_line("the last boot didn't get 30 s past starting Engram (a crash, or a quick restart): one more and "
		         "Engram turns itself off\n");
	set_short_boots(short_boots + 1);

	config_load(&cfg);
	if (!file_exists(CONFIG_PATH))
		config_save(&cfg);
	rng_state = (uint32_t)sceKernelGetProcessTimeWide();
	sceIoMkdir(FONT_DIR, 0777);
	font_start(cfg.font_file, "SceShell");

	avconfig_hook = taiHookFunctionImport(&avconfig_ref, "SceShell", LIB_SCEAVCONFIG, NID_SCEAVCONFIGREGISTERCALLBACK,
	                                      (const void *)sceAVConfigRegisterCallback_patched);
	log_line("start hook: 0x%08X\n", avconfig_hook);
	if (avconfig_hook < 0) {
		sceIoRemove(BOOTING_PATH);
		return 0;
	}

	poll_running = 1;
	poll_thread = sceKernelCreateThread("engram_poll", poll_thread_entry, 0xA0, 0x4000, 0, 0, NULL);
	if (poll_thread >= 0)
		sceKernelStartThread(poll_thread, 0, NULL);
	log_line("poll thread: 0x%08X\n", poll_thread);
	return 0;
}

void shell_stop(void) {
	font_stop();
	if (gettext_hook >= 0) {
		taiHookRelease(gettext_hook, gettext_ref);
		gettext_hook = -1;
	}
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
