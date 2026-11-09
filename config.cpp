// The settings both processes share, kept as key=value lines in CONFIG_PATH.

#include "engram.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>

const char *const EFFECT_KEYS[FX_COUNT] = {
	"stay_put", "stairs", "cascade", "wave", "chomp", "vortex", "scatter", "cube_out",
	"cube_in",  "hinge",  "card",    "blinds", "spin", "shrink", "fade",
};

const char *const EFFECT_NAMES[FX_COUNT] = {
	"Stay Put", "Stairs", "Cascade", "Wave", "Chomp", "Vortex", "Scatter", "Cube (Outside)",
	"Cube (Inside)", "Hinge", "Card Flip", "Blinds", "Spin", "Shrink", "Page Fade",
};

const char *const EFFECT_HINTS[FX_COUNT] = {
	"Icons hold still and fade out while the page slides away.",
	"Columns leave one after another, left to right, like steps.",
	"Icons leave one by one in reading order.",
	"A ripple runs across the rows as the page moves.",
	"The page splits down the middle and the halves slide apart.",
	"Icons swirl into the middle of the screen and vanish.",
	"Icons fly apart in every direction and fade.",
	"Pages are the faces of a turning cube, seen from outside.",
	"Pages are the walls of a turning cube, seen from inside.",
	"The page swings away on its top or bottom edge.",
	"The page flips over like a card to show the next one.",
	"Each icon flips in place like a slat of a blind.",
	"Every icon turns one full circle per page.",
	"Icons shrink to nothing as their page leaves.",
	"The page fades out as it leaves and in as it arrives.",
};

int str_len(const char *s) {
	int n = 0;
	while (s && s[n])
		n++;
	return n;
}

bool str_eq(const char *a, const char *b) {
	if (!a || !b)
		return false;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

bool str_starts(const char *s, const char *prefix) {
	if (!s || !prefix)
		return false;
	while (*prefix)
		if (*s++ != *prefix++)
			return false;
	return true;
}

void log_line(const char *fmt, ...) {
	SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;

	char line[256];
	va_list args;
	va_start(args, fmt);
	sceClibVsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	sceIoWrite(fd, line, sceClibStrnlen(line, sizeof(line)));
	sceIoClose(fd);
}

bool file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

void write_marker(const char *path) {
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0)
		sceIoClose(fd);
}

void config_defaults(Config *cfg) {
	sceClibMemset(cfg, 0, sizeof(*cfg));
	cfg->enabled = 1;
	cfg->random = 0;
	cfg->intensity = INTENSITY_100;
	cfg->tilt = 1;
	cfg->fade_out = FADE_OUT_DURING;
	cfg->fx[FX_CUBE_OUT] = 1;
	cfg->depth = 1;
	cfg->depth_style = DEPTH_BOTH;
	cfg->depth_strength = DEPTH_NORMAL;
	cfg->shadows = 1;
	cfg->wallpaper = 1;
	cfg->shadow_depth = SHADOW_DEPTH_DEFAULT;
	cfg->shadow_layer = LAYER_UNDER_BACK;
}

int *config_field(Config *cfg, const char *key) {
	if (str_eq(key, KEY_ENABLED))
		return &cfg->enabled;
	if (str_eq(key, KEY_RANDOM))
		return &cfg->random;
	if (str_eq(key, KEY_INTENSITY))
		return &cfg->intensity;
	if (str_eq(key, KEY_TILT))
		return &cfg->tilt;
	if (str_eq(key, KEY_FADE_OUT))
		return &cfg->fade_out;
	if (str_eq(key, KEY_DEPTH))
		return &cfg->depth;
	if (str_eq(key, KEY_DEPTH_STYLE))
		return &cfg->depth_style;
	if (str_eq(key, KEY_DEPTH_STRENGTH))
		return &cfg->depth_strength;
	if (str_eq(key, KEY_SHADOWS))
		return &cfg->shadows;
	if (str_eq(key, KEY_WALLPAPER))
		return &cfg->wallpaper;
	if (str_eq(key, KEY_SHADOW_DEPTH))
		return &cfg->shadow_depth;
	if (str_eq(key, KEY_SHADOW_LAYER))
		return &cfg->shadow_layer;
	for (int i = 0; i < FX_COUNT; i++)
		if (str_eq(key, EFFECT_KEYS[i]))
			return &cfg->fx[i];
	return NULL;
}

static void clamp_config(Config *cfg) {
	cfg->enabled = cfg->enabled ? 1 : 0;
	cfg->random = cfg->random ? 1 : 0;
	cfg->tilt = cfg->tilt ? 1 : 0;
	if (cfg->fade_out < 0 || cfg->fade_out >= FADE_OUT_COUNT)
		cfg->fade_out = FADE_OUT_DURING;
	if (cfg->intensity < 0 || cfg->intensity >= INTENSITY_COUNT)
		cfg->intensity = INTENSITY_100;
	for (int i = 0; i < FX_COUNT; i++)
		cfg->fx[i] = cfg->fx[i] ? 1 : 0;
	cfg->depth = cfg->depth ? 1 : 0;
	cfg->shadows = cfg->shadows ? 1 : 0;
	cfg->wallpaper = cfg->wallpaper ? 1 : 0;
	if (cfg->shadow_depth < 0 || cfg->shadow_depth > SHADOW_DEPTH_MAX)
		cfg->shadow_depth = SHADOW_DEPTH_DEFAULT;
	if (cfg->shadow_layer < 0 || cfg->shadow_layer >= SHADOW_LAYER_COUNT)
		cfg->shadow_layer = LAYER_UNDER_BACK;
	if (cfg->depth_style < 0 || cfg->depth_style >= DEPTH_STYLE_COUNT)
		cfg->depth_style = DEPTH_BOTH;
	if (cfg->depth_strength < 0 || cfg->depth_strength >= DEPTH_STRENGTH_COUNT)
		cfg->depth_strength = DEPTH_NORMAL;
}

int config_load(Config *cfg) {
	config_defaults(cfg);

	SceUID fd = sceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;
	char text[1024];
	int size = sceIoRead(fd, text, sizeof(text) - 1);
	sceIoClose(fd);
	if (size < 0)
		return size;
	text[size] = '\0';

	char *line = text;
	while (*line) {
		char *end = line;
		while (*end && *end != '\n' && *end != '\r')
			end++;
		char saved = *end;
		*end = '\0';

		char *eq = line;
		while (*eq && *eq != '=')
			eq++;
		if (*eq == '=' && line[0] != '#') {
			*eq = '\0';
			int *field = config_field(cfg, line);
			if (field) {
				int value = 0, sign = 1;
				const char *v = eq + 1;
				if (*v == '-') {
					sign = -1;
					v++;
				}
				while (*v >= '0' && *v <= '9')
					value = value * 10 + (*v++ - '0');
				*field = value * sign;
			}
		}

		line = saved ? end + 1 : end;
	}

	clamp_config(cfg);
	return 0;
}

int config_save(const Config *cfg) {
	char text[1024];
	int len = sceClibSnprintf(text, sizeof(text),
	                          "# Engram " ENGRAM_VERSION " settings. Change them in Settings > System > Engram.\n"
	                          KEY_ENABLED "=%d\n" KEY_RANDOM "=%d\n" KEY_INTENSITY "=%d\n" KEY_TILT "=%d\n"
	                          KEY_FADE_OUT "=%d\n" KEY_DEPTH "=%d\n" KEY_DEPTH_STYLE "=%d\n" KEY_DEPTH_STRENGTH "=%d\n"
	                          KEY_SHADOWS "=%d\n" KEY_WALLPAPER "=%d\n" KEY_SHADOW_DEPTH "=%d\n" KEY_SHADOW_LAYER "=%d\n",
	                          cfg->enabled, cfg->random, cfg->intensity, cfg->tilt, cfg->fade_out, cfg->depth,
	                          cfg->depth_style, cfg->depth_strength, cfg->shadows,
	                          cfg->wallpaper, cfg->shadow_depth, cfg->shadow_layer);
	for (int i = 0; i < FX_COUNT && len < (int)sizeof(text); i++)
		len += sceClibSnprintf(text + len, sizeof(text) - len, "%s=%d\n", EFFECT_KEYS[i], cfg->fx[i]);
	if (len >= (int)sizeof(text))
		return -1;

	sceIoMkdir(DATA_DIR, 0777);
	SceUID fd = sceIoOpen(CONFIG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int written = sceIoWrite(fd, text, len);
	sceIoClose(fd);
	return written == len ? 0 : -1;
}
