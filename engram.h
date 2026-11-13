// Engram: tweaks for the PS Vita, set in Settings > System > Engram. So far they
// are all in its Live Area category: Cylinder-style scroll animations, Motion
// Depth, ShadowBoard-style icons that float in 3D as you tilt the Vita, and Icon
// Placement, 3D layouts for the icons in place of the Vita's grid.
//
// One engram.suprx is listed under two sections of the taiHEN config:
//   *main       SceShell: moves the home screen's bubbles (shell.cpp)
//   *NPXS10015  the Settings app: adds Settings > System > Engram (settings.cpp)
// The two processes share nothing but the config file below.

#pragma once

#include <stdint.h>

#define ENGRAM_VERSION "1.5"

// Shadows and Wallpaper Parallax are shelved: neither worked on the console
// (1.2 to 1.6, see README). Their code stays; set this to 0 to bring them back.
#define ENGRAM_SHADOWS_SHELVED 1
// Wallpaper Parallax's second try (1.1 test builds, the Wallpaper Layer test)
// is shelved too, at the user's request. Its code stays; set this to 0 to
// bring back the Wallpaper Parallax and Wallpaper Layer rows and what they do.
#define ENGRAM_PARALLAX_SHELVED 1

#define DATA_DIR "ur0:data/engram"
#define CONFIG_PATH DATA_DIR "/config.txt"
#define LOG_PATH DATA_DIR "/log.txt"
#define BOOTING_PATH DATA_DIR "/booting"   // exists from boot until SceShell has run Engram for 30 s
#define DISABLED_PATH DATA_DIR "/disabled" // keeps the animations off until deleted
#define FONT_DIR DATA_DIR "/fonts"         // fonts for Engram's Font category: .ttf, .otf or .pvf
#define FONT_NAME_MAX 64

// Application order matters when effects are combined: the ones that move icons
// around the page come first, then the ones that turn the whole page in 3D, then
// the ones that only spin, shrink or fade.
enum Effect {
	FX_STAY_PUT,
	FX_STAIRS,
	FX_CASCADE,
	FX_WAVE,
	FX_CHOMP,
	FX_VORTEX,
	FX_SCATTER,
	FX_CUBE_OUT,
	FX_CUBE_IN,
	FX_HINGE,
	FX_CARD,
	FX_BLINDS,
	FX_SPIN,
	FX_SHRINK,
	FX_FADE,
	FX_COUNT
};

enum Intensity { INTENSITY_50, INTENSITY_75, INTENSITY_100, INTENSITY_125, INTENSITY_150, INTENSITY_COUNT };

enum FadeOut {
	FADE_OUT_OFF,
	FADE_OUT_DURING, // gone by 60% of the way
	FADE_OUT_LANDING, // fully visible until 60% of the way, gone as the next page lands
	FADE_OUT_COUNT
};

// Motion Depth: what the icons do as you tilt the Vita.
enum DepthStyle {
	DEPTH_FLOAT, // drift against the wallpaper, as if raised above it
	DEPTH_TURN,  // turn to keep facing you
	DEPTH_BOTH,
	DEPTH_STYLE_COUNT
};

enum DepthStrength { DEPTH_SUBTLE, DEPTH_NORMAL, DEPTH_STRONG, DEPTH_EXTREME, DEPTH_STRENGTH_COUNT };

// Wallpaper Parallax, second try: which widget carries a theme's wallpaper
// isn't known, so it's chosen in Settings while testing. Each moves that widget
// the other way as you tilt, zoomed a little so its edges don't show, and moves
// what's inside it back, so the icons stay where Motion Depth puts them.
enum WallLayer {
	WALL_PAGE_INSIDE, // A: each icon page's one child, with the icon container inside moved back
	WALL_PAGE,        // B: each icon page, with its child moved back
	WALL_HOME,        // C: the widget holding the pages' scroll box, with the scroll box moved back
	WALL_LAYER_COUNT
};

// Icon Placement: where the icons rest. Its strength is a DepthStrength.
enum Layout {
	LAYOUT_GRID,          // the Vita's own grid: Icon Placement is off
	LAYOUT_BOOK,          // an open book, 5 icons on each page
	LAYOUT_SPHERE,        // the near side of a ball, like the Apple Watch's app grid
	LAYOUT_SPHERE_INSIDE, // the inside of a ball
	LAYOUT_COUNT
};
// Folders choose from the layouts but the grid: folder_layout + 1 is a Layout.
#define FOLDER_LAYOUT_COUNT (LAYOUT_COUNT - 1)

// For testing: how the shadows are layered against the icons. Paf draws in
// child order and also depth-tests, and which way its depth runs isn't known,
// so each arrangement is a draw order and a depth offset (in shadow_depth
// units). Pick the one that puts the shadows behind.
enum ShadowLayer {
	LAYER_UNDER_BACK,     // A: drawn before the icons, pushed back
	LAYER_UNDER_FORWARD,  // B: drawn before, pulled forward
	LAYER_OVER_BACK,      // C: drawn after the icons, pushed back
	LAYER_OVER_FORWARD,   // D: drawn after, pulled forward
	LAYER_UNDER_FAR_BACK, // E: drawn before, pushed back 8 times as far
	LAYER_OVER_FAR_BACK,  // F: drawn after, pushed back 8 times as far
	SHADOW_LAYER_COUNT
};

struct Config {
	int enabled;   // Live Area > Scroll Animations' switch
	int random;    // each scroll uses one of the chosen effects, picked at random
	int intensity; // enum Intensity
	int tilt;      // 3D effects turn the icons themselves, not only their positions
	int fade_out;  // enum FadeOut: when the page you swipe away from fades out
	int fx[FX_COUNT];
	int depth;          // Live Area > Motion Depth's switch
	int depth_style;    // enum DepthStyle
	int depth_strength; // enum DepthStrength
	int shadows;        // a soft shadow under each icon, left on the wallpaper as the icons float
	int wallpaper;      // the wallpaper slides the other way as you tilt
	int shadow_depth;   // px the shadows sit behind the icons, for the depth test (not in Settings)
	int shadow_layer;   // enum ShadowLayer
	int layout;          // Live Area > Icon Placement: enum Layout
	int layout_strength; // enum DepthStrength
	int folder_place;    // open folders get a layout too
	int folder_layout;   // a Layout, less 1
	int folder_strength; // enum DepthStrength
	int app_enabled;     // Live Area > App Page Animation's switch
	int app_effect;      // an index into APP_EFFECT_CHOICES: one effect at a time
	int app_intensity;   // enum Intensity
	int app_fade_out;    // enum FadeOut: when the page you swipe away from fades out
	int app_fade_in;     // enum FadeOut's values: when the page you swipe to fades in
	int wall_layer;      // enum WallLayer: which widget Wallpaper Parallax moves (for testing)
	char font_file[FONT_NAME_MAX]; // Font > Home Screen: a file in FONT_DIR, or empty for the system font
	// Font > Apps, one per FONT_APPS entry: FONT_APP_SAME, FONT_APP_SYSTEM or a file in FONT_DIR.
	char app_font[17][FONT_NAME_MAX];
	// Font > Lock Screen, Status Bar...: one per PLACE_KEYS entry, a file in
	// FONT_DIR or empty for Same as Home Screen.
	char place_font[4][FONT_NAME_MAX];
};

// Keys in the config file, and in the Settings page's /CONFIG/ENGRAM namespace.
#define KEY_ENABLED "enabled"
#define KEY_RANDOM "random"
#define KEY_INTENSITY "intensity"
#define KEY_TILT "tilt"
#define KEY_FADE_OUT "fade_out"
#define KEY_DEPTH "depth"
#define KEY_DEPTH_STYLE "depth_style"
#define KEY_DEPTH_STRENGTH "depth_strength"
#define KEY_SHADOWS "shadows"
#define KEY_WALLPAPER "wallpaper"
#define KEY_SHADOW_DEPTH "shadow_depth"
#define KEY_SHADOW_LAYER "shadow_layer"
#define KEY_LAYOUT "layout"
#define KEY_LAYOUT_STRENGTH "layout_strength"
#define KEY_FOLDER_PLACE "folder_place"
#define KEY_FOLDER_LAYOUT "folder_layout"
#define KEY_FOLDER_STRENGTH "folder_strength"
#define KEY_APP_ENABLED "app_enabled"
#define KEY_APP_EFFECT "app_effect"
#define KEY_APP_INTENSITY "app_intensity"
#define KEY_APP_FADE_OUT "app_fade_out"
#define KEY_APP_FADE_IN "app_fade_in"
#define KEY_WALL_LAYER "wall_layer"
#define KEY_FONT_FILE "font_file" // a string, not an int: the font's file name
#define KEY_FONT "font"           // Settings' Home Screen font list: its index into the fonts found
#define KEY_ENGRAM_ON "engram_on" // Settings' top switch: on while DISABLED_PATH doesn't exist (not in the config)
#define KEY_APP_FONT "font_"      // + an app's first title id: its font, a string in the config file
#define SHADOW_DEPTH_DEFAULT 16
#define SHADOW_DEPTH_MAX 400
extern const char *const EFFECT_KEYS[FX_COUNT];
extern const char *const EFFECT_NAMES[FX_COUNT];
extern const char *const EFFECT_HINTS[FX_COUNT];
// App Page Animation's Effect list, one at a time. Each page moves as one
// card, so the effects that move icons one by one across a page (Stairs,
// Cascade, Chomp) aren't offered, nor Stay Put: holding two whole pages still
// in the middle of the screen covered one with the other, and the screen no
// longer followed the swipe (1.1 test build). Vortex, which shrinks both pages
// into the middle of the screen while spinning them, Scatter, which flings them
// off in random directions, Card Flip and Blinds, which flip both pages in the
// middle of the screen, Hinge, which never landed cleanly, and Cube (Outside),
// Wave and Page Fade were taken out too: glitchy on whole pages (and a page's
// peelable background never fades). Random picks one per swipe.
#define APP_EFFECT_RANDOM -1
#define APP_EFFECT_COUNT 4
extern const int APP_EFFECT_CHOICES[APP_EFFECT_COUNT]; // an enum Effect, or APP_EFFECT_RANDOM
extern const char *const APP_EFFECT_NAMES[APP_EFFECT_COUNT];

// Font > Apps: the system apps that can have a font of their own. An app is
// matched by any of its title ids (a few run under two); its config key is
// KEY_APP_FONT plus the first. Engram has to be loaded for the app (under its
// *NPXS section in ur0:tai/config.txt) to change its font. Panoramic Camera
// (NPXS10095) isn't one: with Engram loaded it stopped opening, with error
// C1-2569-2, before Engram logged anything (1.1 test, 2026-11-12).
#define FONT_APP_COUNT 17
struct FontApp {
	const char *name;
	const char *ids[2];
};
extern const FontApp FONT_APPS[FONT_APP_COUNT];
// Font's places: parts of SceShell with a font of their own (shell.cpp,
// Place fonts). The boot warning's isn't shown in Settings until Engram
// knows where it is.
// The biggest font a place can have. SceShell has little memory to spare:
// with a 3.4 MB status bar font (M PLUS Rounded) loaded beside three others,
// it crashed during boot twice (1.1 test, 2026-11-13).
#define PLACE_FONT_MAX (1024 * 1024)
#define PLACE_COUNT 4
#define PLACES_SHOWN 4
enum PlaceId { PLACE_LOCK, PLACE_STATUS, PLACE_WARNING, PLACE_QUICK };
extern const char *const PLACE_KEYS[PLACE_COUNT];  // "font_lock"...: config keys, file names
extern const char *const PLACE_NAMES[PLACE_COUNT]; // "Lock Screen"...
int place_key(const char *key);                    // the place a key is for, or -1
#define FONT_APP_SAME ""         // follow the Home Screen font
#define FONT_APP_SYSTEM "system" // the system's own font
static_assert(sizeof(((Config *)0)->app_font) / FONT_NAME_MAX == FONT_APP_COUNT, "Config::app_font is one per app");
static_assert(sizeof(((Config *)0)->place_font) / FONT_NAME_MAX == PLACE_COUNT, "Config::place_font is one per place");
int font_app_index(const char *title_id); // -1 if it isn't one of FONT_APPS
int font_app_key(const char *key);         // the app a KEY_APP_FONT key is for, or -1
// The font file app (an index into FONT_APPS, or -1) uses: its own choice,
// else the Home Screen's. "" means the system font.
const char *font_for_app(const Config *cfg, int app);

void config_defaults(Config *cfg);
// Reads CONFIG_PATH over the defaults. Returns 0 if the file was read.
int config_load(Config *cfg);
int config_save(const Config *cfg);
// The value behind a key, or a pointer to it; NULL if the key isn't one of ours.
int *config_field(Config *cfg, const char *key);

// String helpers: there is no libc.
int str_len(const char *s);
bool str_eq(const char *a, const char *b);
bool str_starts(const char *s, const char *prefix);

void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool file_exists(const char *path);
void write_marker(const char *path);

// shell.cpp
int shell_start(void);
void shell_stop(void);
void shell_paf_loaded(void);      // font.cpp tells SceShell's half when Paf has loaded,
void shell_framework_ready(void); // and when its Framework is made

// settings.cpp
void settings_start(void);
void settings_stop(void);

// font.cpp: the Font category. Swaps the font Paf opens in this process for
// file (in FONT_DIR; "" keeps the system font); host is the process's main
// module, which loads Paf after Engram starts, or NULL for the main module.
void font_start(const char *file, const char *host);
void font_stop(void);
// Engram in one of FONT_APPS' apps: only the font.
void app_font_start(void);
bool font_name_ok(const char *name); // a plain file name ending .ttf, .otf or .pvf
