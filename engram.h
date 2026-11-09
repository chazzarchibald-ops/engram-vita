// Engram: tweaks for the PS Vita, set in Settings > System > Engram. So far they
// are all in its Live Area category: Cylinder-style scroll animations, and Motion
// Depth, ShadowBoard-style icons that float in 3D as you tilt the Vita.
//
// One engram.suprx is listed under two sections of the taiHEN config:
//   *main       SceShell: moves the home screen's bubbles (shell.cpp)
//   *NPXS10015  the Settings app: adds Settings > System > Engram (settings.cpp)
// The two processes share nothing but the config file below.

#pragma once

#include <stdint.h>

#define ENGRAM_VERSION "1.0"

// Shadows and Wallpaper Parallax are shelved: neither worked on the console
// (1.2 to 1.6, see README). Their code stays; set this to 0 to bring them back.
#define ENGRAM_SHADOWS_SHELVED 1

#define DATA_DIR "ur0:data/engram"
#define CONFIG_PATH DATA_DIR "/config.txt"
#define LOG_PATH DATA_DIR "/log.txt"
#define BOOTING_PATH DATA_DIR "/booting"   // exists from boot until SceShell has run Engram for 30 s
#define DISABLED_PATH DATA_DIR "/disabled" // keeps the animations off until deleted

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
#define SHADOW_DEPTH_DEFAULT 16
#define SHADOW_DEPTH_MAX 400
extern const char *const EFFECT_KEYS[FX_COUNT];
extern const char *const EFFECT_NAMES[FX_COUNT];
extern const char *const EFFECT_HINTS[FX_COUNT];

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

// settings.cpp
void settings_start(void);
void settings_stop(void);
