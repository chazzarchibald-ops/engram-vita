// The Settings app (NPXS10015) half of Engram: adds Settings > System > Engram.
//
// The same recipe as HENkaku's settings page and Turbo Storage's
// (github.com/juanjsebgarcia/vita-turbo-storage, MIT): hook SceSettings'
// sceKernelLoadStartModule to wait for system_settings_core.suprx, then hook
//   - scePafMiscLoadXmlLayout, to splice our setting_list into the System page;
//   - SceRegMgr get/set/info in SceSystemSettingsCore, to back our rows with a
//     private /CONFIG/ENGRAM namespace that reads and writes CONFIG_PATH instead
//     of the registry;
//   - scePafToplevelGetText, which turns our title and hint ids into text.
// They are all import hooks that pass anything not ours straight through, so if
// something doesn't match on this firmware the page just doesn't appear.

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include "engram.h"

// taiHEN's TAI_CONTINUE doesn't compile as C++.
#define TAI_NEXT(this_func, hook, ...)                                                                        \
	({                                                                                                        \
		struct _tai_hook_user *cur = (struct _tai_hook_user *)(hook), *next;                                  \
		next = (struct _tai_hook_user *)cur->next;                                                            \
		(next == NULL) ? ((__typeof__(&this_func))cur->old)(__VA_ARGS__)                                      \
		               : ((__typeof__(&this_func))next->func)(__VA_ARGS__);                                   \
	})

#define CATEGORY "/CONFIG/ENGRAM"

#define LIB_SCELIBKERNEL 0xCAE9ACE6
#define NID_LOAD_START_MODULE 0x2DCC4AFA
#define LIB_SCEPAFMISC 0x3D643CE8
#define NID_LOAD_XML_LAYOUT 0x19FE55A8
#define LIB_SCEREGMGR 0xC436F916
#define NID_REG_GET_INT 0x16DDF3DC
#define NID_REG_SET_INT 0xD72EA399
#define NID_REG_GET_INFO 0x58421DD1
#define LIB_SCEPAFTOPLEVEL 0x4D9A9DD0
#define NID_GET_TEXT 0x19CEFDA7

#define PAGE_ORIG_MAX 16384 // leave the System page alone if it is ever bigger
#define GROUP_MAX 65536 // Font > Apps has a list of every font for each app

static Config cfg;

// The fonts in FONT_DIR, listed once as Settings starts. Item 0 of the Font
// list is the system's own font, item i is font_files[i - 1].
#define FONT_LIST_MAX 40
static char font_files[FONT_LIST_MAX][FONT_NAME_MAX];
static unsigned font_sizes[FONT_LIST_MAX];
static char place_names[FONT_LIST_MAX][FONT_NAME_MAX + 12]; // with " (too big)" past PLACE_FONT_MAX
static int font_count;

static void list_fonts(void) {
	font_count = 0;
	SceUID dir = sceIoDopen(FONT_DIR);
	if (dir < 0)
		return;
	SceIoDirent entry;
	while (font_count < FONT_LIST_MAX) {
		sceClibMemset(&entry, 0, sizeof(entry));
		if (sceIoDread(dir, &entry) <= 0)
			break;
		if (SCE_S_ISDIR(entry.d_stat.st_mode) || !font_name_ok(entry.d_name))
			continue;
		sceClibStrncpy(font_files[font_count], entry.d_name, FONT_NAME_MAX - 1);
		font_files[font_count][FONT_NAME_MAX - 1] = '\0';
		font_sizes[font_count] = (unsigned)entry.d_stat.st_size;
		font_count++;
	}
	sceIoDclose(dir);
	// In order by name, so the list doesn't shuffle between visits.
	for (int i = 1; i < font_count; i++)
		for (int j = i; j > 0; j--) {
			const char *a = font_files[j - 1], *b = font_files[j];
			int k = 0;
			while (a[k] && (a[k] | 0x20) == (b[k] | 0x20))
				k++;
			if ((a[k] | 0x20) <= (b[k] | 0x20))
				break;
			char t[FONT_NAME_MAX];
			sceClibMemcpy(t, font_files[j - 1], FONT_NAME_MAX);
			sceClibMemcpy(font_files[j - 1], font_files[j], FONT_NAME_MAX);
			sceClibMemcpy(font_files[j], t, FONT_NAME_MAX);
			unsigned ts = font_sizes[j - 1];
			font_sizes[j - 1] = font_sizes[j];
			font_sizes[j] = ts;
		}
	for (int i = 0; i < font_count; i++)
		sceClibSnprintf(place_names[i], sizeof(place_names[i]), "%s%s", font_files[i],
		                font_sizes[i] > PLACE_FONT_MAX ? " (too big)" : "");
	log_line("fonts: %d in %s\n", font_count, FONT_DIR);
}

static int font_index(void) {
	for (int i = 0; i < font_count; i++)
		if (str_eq(font_files[i], cfg.font_file))
			return i + 1;
	return 0;
}

// An app's list in Font > Apps: 0 Same as Live Area, 1 System Font, then
// font_files[i - 2].
static int app_font_index(int app) {
	const char *f = cfg.app_font[app];
	if (!f[0])
		return 0;
	if (str_eq(f, FONT_APP_SYSTEM))
		return 1;
	for (int i = 0; i < font_count; i++)
		if (str_eq(font_files[i], f))
			return i + 2;
	return 0;
}

// ---- the page ----

static char group_xml[GROUP_MAX];
static int group_len;
static bool group_full; // something didn't fit: the page is rebuilt without Font > Apps
static char page_buf[PAGE_ORIG_MAX + GROUP_MAX];

// Turbo Storage leaves a System page over 16 KB alone (TS_PAGE_ORIG_MAX in
// its storageturbo_settings.c), and Engram's rows with Font > Apps are about
// 23 KB. When Engram's hook runs before Turbo Storage's, it adds these
// compact rows instead: everything but Font > Apps, about 9 KB, and a note.
#define COMPACT_MAX 12288
#define COMPACT_ROWS_MAX 10240 // with the 5.4 KB System page, under Turbo Storage's 16 KB
static char compact_xml[COMPACT_MAX];
static int compact_len;
static bool turbo_storage; // storageturbo.suprx is loaded here

static void put(const char *text) {
	int n = str_len(text);
	if (group_len + n >= GROUP_MAX) {
		group_full = true;
		return;
	}
	sceClibMemcpy(group_xml + group_len, text, n);
	group_len += n;
	group_xml[group_len] = '\0';
}

static void put_toggle(const char *key) {
	char row[320];
	sceClibSnprintf(row, sizeof(row),
	                "<toggle_switch id=\"eg_%s\" title=\"eg_t_%s\" icon=\"tex_spanner\" hint=\"eg_h_%s\" "
	                "key=\"" CATEGORY "/%s\"/>",
	                key, key, key, key);
	put(row);
}

// A list row whose items are titled by text_for() from LISTS below.
static void put_list(const char *key, const char *item_prefix, int count) {
	char row[320];
	sceClibSnprintf(row, sizeof(row),
	                "<list id=\"eg_%s\" title=\"eg_t_%s\" icon=\"tex_spanner\" hint=\"eg_h_%s\" key=\"" CATEGORY
	                "/%s\">",
	                key, key, key, key);
	put(row);
	for (int i = 0; i < (int)count; i++) {
		char item[96];
		sceClibSnprintf(item, sizeof(item), "<list_item id=\"eg_i_%s%d\" title=\"eg_i_%s%d\" value=\"%d\"/>",
		                item_prefix, i, item_prefix, i, i);
		put(item);
	}
	put("</list>");
}

static void open_page(const char *id) {
	char row[160];
	sceClibSnprintf(row, sizeof(row), "<setting_list id=\"engram_%s\" title=\"eg_p_%s\" icon=\"tex_spanner\">", id, id);
	put(row);
}

// Engram
//   Engram                switch: off while ur0:data/engram/disabled exists (the crash guard's file)
//   Live Area
//     Scroll Animations   switch, Effects (one switch each), Randomize, Fade Out, Intensity, 3D Icon Tilt
//     Motion Depth        switch, Style, Strength (Shadows, Wallpaper Parallax: shelved)
//     Icon Placement      Layout, Strength, Folders, Folder Layout, Folder Strength
//     App Page Animation  switch, Effect, Fade Out, Fade In, Intensity
//   Fonts                 Live Area, Lock Screen, Status Bar, Boot Warning, Quick Menu, Apps (a font for each system app), a note on fonts
//   Credits
// More categories (a Lock Screen one, say) go next to Live Area.
static void build_group(bool app_fonts, bool apps_note = false, bool place_fonts = true) {
	group_len = 0;
	group_full = false;
	put("<setting_list id=\"engram_settings\" title=\"eg_p_title\" icon=\"tex_spanner\">");

	put_toggle(KEY_ENGRAM_ON);

	open_page("livearea");

	open_page("scroll");
	put_toggle(KEY_ENABLED);
	open_page("effects");
	for (int i = 0; i < FX_COUNT; i++)
		put_toggle(EFFECT_KEYS[i]);
	put("</setting_list>");
	put_toggle(KEY_RANDOM);
	put_list(KEY_FADE_OUT, "fade", FADE_OUT_COUNT);
	put_list(KEY_INTENSITY, "int", INTENSITY_COUNT);
	put_toggle(KEY_TILT);
	put("</setting_list>");

	open_page("depth");
	put_toggle(KEY_DEPTH);
	put_list(KEY_DEPTH_STYLE, "dstyle", DEPTH_STYLE_COUNT);
	put_list(KEY_DEPTH_STRENGTH, "dstr", DEPTH_STRENGTH_COUNT);
#if !ENGRAM_SHADOWS_SHELVED
	put_toggle(KEY_SHADOWS);
	put_list(KEY_SHADOW_LAYER, "slay", SHADOW_LAYER_COUNT);
#endif
#if !ENGRAM_PARALLAX_SHELVED
	put_toggle(KEY_WALLPAPER);
	put_list(KEY_WALL_LAYER, "wlay", WALL_LAYER_COUNT);
#endif
	put("</setting_list>");

	open_page("place");
	put_list(KEY_LAYOUT, "lay", LAYOUT_COUNT);
	put_list(KEY_LAYOUT_STRENGTH, "lstr", DEPTH_STRENGTH_COUNT);
	put_toggle(KEY_FOLDER_PLACE);
	put_list(KEY_FOLDER_LAYOUT, "flay", FOLDER_LAYOUT_COUNT);
	put_list(KEY_FOLDER_STRENGTH, "fstr", DEPTH_STRENGTH_COUNT);
	put("</setting_list>");

	open_page("apppage");
	put_toggle(KEY_APP_ENABLED);
	put_list(KEY_APP_EFFECT, "aeff", APP_EFFECT_COUNT);
	put_list(KEY_APP_FADE_OUT, "afade", FADE_OUT_COUNT);
	put_list(KEY_APP_FADE_IN, "afin", FADE_OUT_COUNT);
	put_list(KEY_APP_INTENSITY, "aint", INTENSITY_COUNT);
	put("</setting_list>");

	put("</setting_list>"); // Live Area

	// Font: the Live Area's list, one for each app, then a note on which
	// fonts to use and where.
	open_page("font");
	put_list(KEY_FONT, "font", font_count + 1);
	// Lock Screen, Status Bar...: Same as Live Area, then the fonts.
	for (int p = 0; place_fonts && p < PLACES_SHOWN; p++) {
		char prefix[16];
		sceClibSnprintf(prefix, sizeof(prefix), "fpl%dx", p);
		put_list(PLACE_KEYS[p], prefix, font_count + 1);
	}
	if (app_fonts) {
		open_page("fontapps");
		for (int a = 0; a < FONT_APP_COUNT; a++) {
			char key[24], prefix[16];
			sceClibSnprintf(key, sizeof(key), KEY_APP_FONT "%s", FONT_APPS[a].ids[0]);
			sceClibSnprintf(prefix, sizeof(prefix), "fapp%dx", a);
			put_list(key, prefix, font_count + 2);
		}
		put("</setting_list>");
	}
	if (apps_note)
		put("<label id=\"eg_apps_note\" title=\"eg_an_title\" description=\"eg_an_text\"/>");
	put("<label id=\"eg_font_note\" title=\"eg_fn_title\" description=\"eg_fn_text\"/>");
	put("</setting_list>");

	// Credits: one plain text row. The text is its description, the smaller
	// second line Settings rows can have, so it is all drawn at one smaller size
	// (three label rows each fitted their own line, at different sizes).
	put("<spacer id=\"eg_spacer\" height=\"24\"/>");
	put("<label id=\"eg_credits\" title=\"eg_c_title\" description=\"eg_c_text\"/>");
	put("</setting_list>");
}

static const char CLOSE_LIST[] = "</setting_list>";

static const char *mem_find(const char *hay, int hay_len, const char *needle, int needle_len) {
	for (int i = 0; i + needle_len <= hay_len; i++) {
		int j = 0;
		while (j < needle_len && hay[i + j] == needle[j])
			j++;
		if (j == needle_len)
			return hay + i;
	}
	return NULL;
}

// The </setting_list> that closes the list opening with `open`, counting nesting.
static const char *list_close(const char *s, int size, const char *open) {
	static const char OPEN[] = "<setting_list";
	const char *end = s + size;
	const char *p = mem_find(s, size, open, str_len(open));
	int depth = 0;
	if (!p)
		return NULL;
	while (p < end) {
		const char *o = mem_find(p, (int)(end - p), OPEN, sizeof(OPEN) - 1);
		const char *c = mem_find(p, (int)(end - p), CLOSE_LIST, sizeof(CLOSE_LIST) - 1);
		if (!c)
			return NULL;
		if (o && o < c) {
			depth++;
			p = o + sizeof(OPEN) - 1;
		} else {
			if (--depth == 0)
				return c;
			p = c + sizeof(CLOSE_LIST) - 1;
		}
	}
	return NULL;
}

// Straight after System Information if it's there, else last in the System list.
static const char *insert_point(const char *s, int size) {
	const char *system_end = list_close(s, size, "<setting_list id=\"id_system\"");
	const char *about_end = list_close(s, size, "<setting_list id=\"id_about_system\"");
	if (!system_end)
		return NULL;
	if (about_end && about_end < system_end)
		return about_end + sizeof(CLOSE_LIST) - 1;
	return system_end;
}

// ---- text ----

typedef unsigned short u16;
// 511 characters a text: the Engram switch's info is about 330, and the first
// 255-character buffer cut it off mid-path ("ur0:d", user report).
#define TEXT_MAX 512
static u16 text_ring[8][TEXT_MAX];
static int text_next;

static const u16 *to_u16(const char *text) {
	u16 *out = text_ring[text_next];
	text_next = (text_next + 1) % 8;
	int i = 0;
	for (; text[i] && i < TEXT_MAX - 1; i++)
		out[i] = (unsigned char)text[i];
	out[i] = 0;
	return out;
}

static const char *const INTENSITY_NAMES[INTENSITY_COUNT] = {"Subtle (50%)", "Gentle (75%)", "Normal (100%)",
                                                             "Strong (125%)", "Extreme (150%)"};
static const char *const FADE_OUT_NAMES[FADE_OUT_COUNT] = {"Off", "During the Swipe", "As the Next Page Lands"};
static const char *const FADE_IN_NAMES[FADE_OUT_COUNT] = {"Off", "During the Swipe", "As It Lands"};
static const char *const DEPTH_STYLE_NAMES[DEPTH_STYLE_COUNT] = {"Float", "Turn to Face You", "Float and Turn"};
static const char *const DEPTH_STRENGTH_NAMES[DEPTH_STRENGTH_COUNT] = {"Subtle", "Normal", "Strong", "Extreme"};
static const char *const WALL_LAYER_NAMES[WALL_LAYER_COUNT] = {"A: Inside Each Page", "B: Each Page",
                                                              "C: All the Pages"};
static const char *const LAYOUT_NAMES[LAYOUT_COUNT] = {"Off (Vita Grid)", "Open Book", "Sphere", "Inverted Sphere"};
static const char *const FOLDER_LAYOUT_NAMES[FOLDER_LAYOUT_COUNT] = {"Open Book", "Sphere", "Inverted Sphere"};
static const char *const SHADOW_LAYER_NAMES[SHADOW_LAYER_COUNT] = {
	"A: Under, Pushed Back", "B: Under, Pulled Forward", "C: Over, Pushed Back",
	"D: Over, Pulled Forward", "E: Under, Far Back",      "F: Over, Far Back",
};

// The items of each list row, by the prefix of their ids (eg_i_<prefix><n>).
static const struct {
	const char *prefix;
	const char *const *names;
	int count;
} LISTS[] = {
	{"fade", FADE_OUT_NAMES, FADE_OUT_COUNT},
	{"int", INTENSITY_NAMES, INTENSITY_COUNT},
	{"dstyle", DEPTH_STYLE_NAMES, DEPTH_STYLE_COUNT},
	{"dstr", DEPTH_STRENGTH_NAMES, DEPTH_STRENGTH_COUNT},
	{"slay", SHADOW_LAYER_NAMES, SHADOW_LAYER_COUNT},
	{"lay", LAYOUT_NAMES, LAYOUT_COUNT},
	// Every list needs its own prefix, even with the same names: two lists
	// with the same item ids crashed the Settings app (1.1's first build).
	{"lstr", DEPTH_STRENGTH_NAMES, DEPTH_STRENGTH_COUNT},
	{"flay", FOLDER_LAYOUT_NAMES, FOLDER_LAYOUT_COUNT},
	{"fstr", DEPTH_STRENGTH_NAMES, DEPTH_STRENGTH_COUNT},
	{"aint", INTENSITY_NAMES, INTENSITY_COUNT},
	{"aeff", APP_EFFECT_NAMES, APP_EFFECT_COUNT},
	{"afade", FADE_OUT_NAMES, FADE_OUT_COUNT},
	{"afin", FADE_IN_NAMES, FADE_OUT_COUNT},
	{"wlay", WALL_LAYER_NAMES, WALL_LAYER_COUNT},
};

static const char *page_title(const char *id) {
	if (str_eq(id, "title")) // the Engram page's header, and its row in System
		return "Engram " ENGRAM_VERSION;
	if (str_eq(id, "livearea"))
		return "Live Area";
	if (str_eq(id, "scroll"))
		return "Scroll Animations";
	if (str_eq(id, "effects"))
		return "Effects";
	if (str_eq(id, "depth"))
		return "Motion Depth";
	if (str_eq(id, "place"))
		return "Icon Placement";
	if (str_eq(id, "font"))
		return "Fonts";
	if (str_eq(id, "fontapps"))
		return "Apps";
	if (str_eq(id, "apppage"))
		return "App Page Animation";
	return NULL;
}

static const char *title_for(const char *key) {
	if (str_eq(key, KEY_ENGRAM_ON))
		return "Engram";
	if (str_eq(key, KEY_ENABLED))
		return "Scroll Animations";
	if (str_eq(key, KEY_RANDOM))
		return "Randomize";
	if (str_eq(key, KEY_INTENSITY))
		return "Intensity";
	if (str_eq(key, KEY_TILT))
		return "3D Icon Tilt";
	if (str_eq(key, KEY_FADE_OUT))
		return "Fade Out"; // the longer "Fade Out Previous Page" overlapped the chosen value
	if (str_eq(key, KEY_DEPTH))
		return "Motion Depth";
	if (str_eq(key, KEY_DEPTH_STYLE))
		return "Style";
	if (str_eq(key, KEY_DEPTH_STRENGTH) || str_eq(key, KEY_LAYOUT_STRENGTH))
		return "Strength";
	if (str_eq(key, KEY_LAYOUT))
		return "Layout";
	if (str_eq(key, KEY_APP_ENABLED))
		return "App Page Animation";
	if (str_eq(key, KEY_WALL_LAYER))
		return "Wallpaper Layer";
	if (str_eq(key, KEY_FONT))
		return "Live Area";
	if (font_app_key(key) >= 0)
		return FONT_APPS[font_app_key(key)].name;
	if (place_key(key) >= 0)
		return PLACE_NAMES[place_key(key)];
	if (str_eq(key, KEY_APP_EFFECT))
		return "Effect";
	if (str_eq(key, KEY_APP_INTENSITY))
		return "Intensity";
	if (str_eq(key, KEY_APP_FADE_OUT))
		return "Fade Out";
	if (str_eq(key, KEY_APP_FADE_IN))
		return "Fade In";
	if (str_eq(key, KEY_FOLDER_PLACE))
		return "Folders";
	if (str_eq(key, KEY_FOLDER_LAYOUT))
		return "Folder Layout";
	if (str_eq(key, KEY_FOLDER_STRENGTH))
		return "Folder Strength";
	if (str_eq(key, KEY_SHADOWS))
		return "Shadows";
	if (str_eq(key, KEY_WALLPAPER))
		return "Wallpaper Parallax";
	if (str_eq(key, KEY_SHADOW_LAYER))
		return "Shadow Layering";
	for (int i = 0; i < FX_COUNT; i++)
		if (str_eq(key, EFFECT_KEYS[i]))
			return EFFECT_NAMES[i];
	return NULL;
}

#define PLACE_SIZE_NOTE "Fonts marked too big (over 1 MB) are left out: the Live Area hasn't the memory."
#define CRASH_GUARD_HINT "Engram turned itself off after two boots in a row that didn't finish. Turn this on, then restart, to try again."

static const char *hint_for(const char *key) {
	bool guard_tripped =
		(str_eq(key, KEY_ENABLED) || str_eq(key, KEY_DEPTH) || str_eq(key, KEY_LAYOUT) || str_eq(key, KEY_FOLDER_PLACE) ||
		 str_eq(key, KEY_APP_ENABLED)) &&
		file_exists(DISABLED_PATH);
	if (guard_tripped)
		return CRASH_GUARD_HINT;
	if (str_eq(key, KEY_ENGRAM_ON))
		return "As a failsafe, this turns off by itself if the Vita crashes because of Engram. Once you've changed "
		       "the setting you think caused the crash, turn it back on and restart the Vita. If you had a crash and "
		       "this turned off by itself, please send the log in ur0:data/engram to my GitHub: "
		       "github.com/chazzarchibald-ops/engram-vita/issues";
	if (str_eq(key, KEY_ENABLED))
		return "Animate the home screen's icons as you scroll between pages.";
	if (str_eq(key, KEY_RANDOM))
		return "Each scroll uses one of the effects you've turned on, picked at random.";
	if (str_eq(key, KEY_INTENSITY))
		return "How far the effects move, turn and fade the icons.";
	if (str_eq(key, KEY_TILT))
		return "The 3D effects turn the icons themselves. Turn off to flatten them instead.";
	if (str_eq(key, KEY_FADE_OUT))
		return "When the page you swipe away from fades out. The next page always stays fully visible.";
	if (str_eq(key, KEY_DEPTH))
		return "Tilt the Vita and the icons float above the wallpaper in 3D.";
	if (str_eq(key, KEY_DEPTH_STYLE))
		return "Float: icons drift against the wallpaper. Turn: icons turn to face you. Both look the most 3D.";
	if (str_eq(key, KEY_DEPTH_STRENGTH))
		return "How far the icons float and turn as you tilt the Vita.";
	if (str_eq(key, KEY_LAYOUT))
		return "Open Book: two pages of icons angled like a book held open. Sphere: icons on a big ball, bigger in "
		       "the middle. Inverted Sphere: the inside of the ball.";
	if (str_eq(key, KEY_LAYOUT_STRENGTH))
		return "How far the pages are angled, or how round the ball is.";
	if (str_eq(key, KEY_APP_ENABLED))
		return "Animate an app's page as you swipe left and right between the home screen and the apps you've opened.";
	if (str_eq(key, KEY_FONT))
		return "The font for the Live Area and its app pages, and for the lock screen, status bar and notifications, "
		       "boot warning, quick menu and apps left on Same as Live Area. Restart the Vita to apply. " PLACE_SIZE_NOTE;
	if (font_app_key(key) >= 0)
		return "This app's font. Same as Live Area follows the Live Area font. Applies the next time the app "
		       "starts.";
	if (place_key(key) == PLACE_LOCK)
		return "The font for the lock screen's clock and date. Letters it doesn't have come from the Live Area "
		       "font. Restart the Vita to apply. " PLACE_SIZE_NOTE;
	if (place_key(key) == PLACE_STATUS)
		return "The font for the status bar at the top of the screen (the clock and the battery) and for the "
		       "notifications. Letters it doesn't have come from the Live Area font. Restart the Vita to apply. "
		       PLACE_SIZE_NOTE;
	if (place_key(key) == PLACE_WARNING)
		return "The font for the warning the Vita shows as it starts, before the lock screen. Letters it doesn't "
		       "have come from the Live Area font. Restart the Vita to see it. " PLACE_SIZE_NOTE;
	if (place_key(key) == PLACE_QUICK)
		return "The font for the quick menu you open by holding the PS button. Letters it doesn't have come from "
		       "the Live Area font. Restart the Vita to apply. " PLACE_SIZE_NOTE;
	if (str_eq(key, KEY_WALL_LAYER))
		return "For testing: which part of the home screen carries your wallpaper. Try each one while tilting the "
		       "Vita and note which one moves the wallpaper.";
	if (str_eq(key, KEY_APP_EFFECT))
		return "How the pages move as you swipe between them. Random picks a different effect for each swipe.";
	if (str_eq(key, KEY_APP_INTENSITY))
		return "How far the effect moves, turns and fades the pages.";
	if (str_eq(key, KEY_APP_FADE_OUT))
		return "When the page you swipe away from fades out. An app page's background you can peel off doesn't fade.";
	if (str_eq(key, KEY_APP_FADE_IN))
		return "When the page you swipe to fades in, the home screen as well as an app's page.";
	if (str_eq(key, KEY_FOLDER_PLACE))
		return "Lay out the icons in an open folder too, with a layout of their own.";
	if (str_eq(key, KEY_FOLDER_LAYOUT))
		return "The layout for the icons in an open folder.";
	if (str_eq(key, KEY_FOLDER_STRENGTH))
		return "How far a folder's pages are angled, or how round its ball is.";
	if (str_eq(key, KEY_SHADOWS))
		return "A shadow under each icon. It stays on the wallpaper as the icons float, like ShadowBoard.";
	if (str_eq(key, KEY_WALLPAPER))
		return "The wallpaper slides the other way as you tilt, as on iOS. Works with a theme's wallpaper.";
	if (str_eq(key, KEY_SHADOW_LAYER))
		return "For testing: how the shadows are layered against the icons. Try each one and note which puts "
		       "the shadows behind.";
	for (int i = 0; i < FX_COUNT; i++)
		if (str_eq(key, EFFECT_KEYS[i]))
			return EFFECT_HINTS[i];
	return NULL;
}

// eg_i_<prefix><digit>
static const char *item_text(const char *id) {
	// A place's list (Lock Screen...): fpl<place>x<item>, 0 Same as Live Area.
	if (str_starts(id, "fpl")) {
		const char *n = id + 3;
		int place = 0, value = 0, digits = 0;
		while (*n >= '0' && *n <= '9' && digits < 2) {
			place = place * 10 + (*n++ - '0');
			digits++;
		}
		if (digits > 0 && *n == 'x' && place < PLACE_COUNT) {
			n++;
			digits = 0;
			while (n[digits] >= '0' && n[digits] <= '9' && digits < 3)
				value = value * 10 + (n[digits++] - '0');
			if (digits > 0 && !n[digits] && value <= font_count)
				return value == 0 ? "Same as Live Area" : place_names[value - 1];
		}
	}
	// An app's list in Font > Apps: fapp<app>x<item>.
	if (str_starts(id, "fapp")) {
		const char *n = id + 4;
		int app = 0, value = 0, digits = 0;
		while (*n >= '0' && *n <= '9' && digits < 3) {
			app = app * 10 + (*n++ - '0');
			digits++;
		}
		if (digits > 0 && *n == 'x' && app < FONT_APP_COUNT) {
			n++;
			digits = 0;
			while (n[digits] >= '0' && n[digits] <= '9' && digits < 3)
				value = value * 10 + (n[digits++] - '0');
			if (digits > 0 && !n[digits] && value <= font_count + 1)
				return value == 0 ? "Same as Live Area" : value == 1 ? "System Font" : font_files[value - 2];
		}
	}
	// The Live Area list's items: the fonts found, not a fixed list.
	if (str_starts(id, "font")) {
		const char *n = id + 4;
		int value = 0, digits = 0;
		while (n[digits] >= '0' && n[digits] <= '9' && digits < 3)
			value = value * 10 + (n[digits++] - '0');
		if (digits > 0 && !n[digits] && value <= font_count)
			return value == 0 ? "System Font" : place_names[value - 1]; // "(too big)" past 1 MB here too
	}
	for (unsigned l = 0; l < sizeof(LISTS) / sizeof(LISTS[0]); l++) {
		if (!str_starts(id, LISTS[l].prefix))
			continue;
		const char *n = id + str_len(LISTS[l].prefix);
		int value = 0, digits = 0;
		while (n[digits] >= '0' && n[digits] <= '9' && digits < 3)
			value = value * 10 + (n[digits++] - '0');
		if (digits > 0 && !n[digits] && value < LISTS[l].count)
			return LISTS[l].names[value];
	}
	return NULL;
}

static const char *text_for(const char *id) {
	if (!str_starts(id, "eg_"))
		return NULL;
	if (str_eq(id, "eg_c_title"))
		return "Credits";
	if (str_eq(id, "eg_fn_title"))
		return "About Fonts";
	if (str_eq(id, "eg_an_title"))
		return "Apps Is Hidden";
	if (str_eq(id, "eg_an_text"))
		return "Turbo Storage leaves the System page alone if it's over 16 KB, and Fonts > Apps would make it "
		       "bigger. To have both, move ur0:tai/engram.suprx up to straight after ur0:tai/henkaku.suprx under "
		       "*NPXS10015 in ur0:tai/config.txt, then restart the Vita.";
	if (str_eq(id, "eg_fn_text"))
		return "Put .ttf or .otf fonts in ur0:data/engram/fonts. Use a font with your language's letters, or a "
		       "multi-language one if you use more than one (English and Japanese, say): missing letters show "
		       "as blanks or boxes. An app's own font needs Engram listed under that app in ur0:tai/config.txt.";
	if (str_eq(id, "eg_c_text"))
		return "Engram by badmanwazzy37. Scroll Animations are a faithful port of Cylinder for iOS by Reed "
		       "Weichler (rweichler). Motion Depth is inspired by ShadowBoard for iOS.";
	if (str_starts(id, "eg_p_"))
		return page_title(id + 5);
	if (str_starts(id, "eg_t_"))
		return title_for(id + 5);
	if (str_starts(id, "eg_h_"))
		return hint_for(id + 5);
	if (str_starts(id, "eg_i_"))
		return item_text(id + 5);
	return NULL;
}

// ---- hooks ----

static SceUID load_hook = -1, xml_hook = -1, get_hook = -1, set_hook = -1, info_hook = -1, text_hook = -1;
static tai_hook_ref_t load_ref, xml_ref, get_ref, set_ref, info_ref, text_ref;

// Test builds: how the Settings app quits. 1.1's Font crash left no dump,
// so Settings may be quitting on its own; this logs the code and the module
// that asked, from sceKernelExitProcess.
#define ENGRAM_EXIT_TRACE 0
#if ENGRAM_EXIT_TRACE
#define NID_EXIT_PROCESS 0x7595D9AA
static SceUID exit_hook = -1;
static tai_hook_ref_t exit_ref;
static int module_logs;

static void log_caller(const char *what, unsigned addr) {
	SceUID ids[96];
	SceSize count = 96;
	if (sceKernelGetModuleList(0xFF, ids, &count) < 0) {
		log_line("exit trace: %s 0x%08X, no module list\n", what, addr);
		return;
	}
	for (int i = 0; i < (int)count; i++) {
		SceKernelModuleInfo info;
		sceClibMemset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(ids[i], &info) < 0)
			continue;
		for (int k = 0; k < 4; k++) {
			unsigned start = (unsigned)info.segments[k].vaddr, size = info.segments[k].memsz;
			if (size && addr >= start && addr < start + size) {
				log_line("exit trace: %s 0x%08X = %s seg%d+0x%X\n", what, addr, info.module_name, k, addr - start);
				return;
			}
		}
	}
	log_line("exit trace: %s 0x%08X, in no module\n", what, addr);
}

static int exit_process_patched(int res) {
	log_line("exit trace: Settings quits with 0x%08X\n", res);
	log_caller("called from", (unsigned)__builtin_return_address(0));
	return TAI_NEXT(exit_process_patched, exit_ref, res);
}
#endif
static int ui_hooked;

static bool is_ours(const char *category) {
	return str_eq(category, CATEGORY);
}

// scePafMiscLoadXmlLayout(out, buf, size, a4)
static int load_xml_patched(void *out, void *buf, int size, void *a4) {
	static const char SYSTEM_PAGE[] = "plugin=\"console_info_plugin\"";
	const char *s = (const char *)buf;

	if (s && size > 0 && mem_find(s, size, SYSTEM_PAGE, sizeof(SYSTEM_PAGE) - 1)) {
		if (size > PAGE_ORIG_MAX) {
			log_line("system page is %d bytes, more than %d: left alone\n", size, PAGE_ORIG_MAX);
		} else {
			config_load(&cfg);
			const char *at = insert_point(s, size);
			// Turbo Storage is loaded but its rows aren't on the page yet:
			// Engram's hook runs before its, so the page has to stay small.
			// Checked here, not as Settings starts: listed after Engram under
			// *NPXS10015, Turbo Storage loads after Engram starts.
			tai_module_info_t ts;
			ts.size = sizeof(ts);
			turbo_storage = taiGetModuleInfo("storageturbo", &ts) >= 0;
			bool before_turbo = turbo_storage && !mem_find(s, size, "turbostorage_settings", 21);
			const char *rows = before_turbo ? compact_xml : group_xml;
			int rows_len = before_turbo ? compact_len : group_len;
			log_line("system page (%d bytes): insert point %d, Turbo Storage %s, %s rows (%d bytes)\n", size,
			         at ? (int)(at - s) : -1, !turbo_storage ? "not loaded" : before_turbo ? "after Engram" : "first",
			         before_turbo ? "compact" : "full", rows_len);
			if (at) {
				int head = (int)(at - s);
				sceClibMemcpy(page_buf, s, head);
				sceClibMemcpy(page_buf + head, rows, rows_len);
				sceClibMemcpy(page_buf + head + rows_len, at, size - head);
				buf = page_buf;
				size += rows_len;
			}
		}
	}
	return TAI_NEXT(load_xml_patched, xml_ref, out, buf, size, a4);
}

static int reg_get_int_patched(const char *category, const char *name, int *value) {
	if (is_ours(category) && value && str_eq(name, KEY_ENGRAM_ON)) {
		*value = file_exists(DISABLED_PATH) ? 0 : 1;
		return 0;
	}
	if (is_ours(category) && value && str_eq(name, KEY_FONT)) {
		*value = font_index();
		return 0;
	}
	if (is_ours(category) && value && place_key(name) >= 0) {
		const char *f = cfg.place_font[place_key(name)];
		*value = 0;
		for (int i = 0; f[0] && i < font_count; i++)
			if (str_eq(font_files[i], f))
				*value = i + 1;
		return 0;
	}
	if (is_ours(category) && value && font_app_key(name) >= 0) {
		*value = app_font_index(font_app_key(name));
		return 0;
	}
	if (is_ours(category) && value) {
		int *field = config_field(&cfg, name);
		*value = field ? *field : 0;
		if ((field == &cfg.enabled || field == &cfg.depth || field == &cfg.layout || field == &cfg.folder_place ||
		     field == &cfg.app_enabled) &&
		    file_exists(DISABLED_PATH))
			*value = 0;
		return 0;
	}
	return TAI_NEXT(reg_get_int_patched, get_ref, category, name, value);
}

static int reg_set_int_patched(const char *category, const char *name, int value) {
	if (is_ours(category) && str_eq(name, KEY_ENGRAM_ON)) {
		int ret = 0;
		if (value)
			ret = sceIoRemove(DISABLED_PATH);
		else
			write_marker(DISABLED_PATH);
		log_line("set Engram %s: %s (0x%08X)\n", value ? "on" : "off",
		         value ? "removed " DISABLED_PATH : "wrote " DISABLED_PATH, (unsigned)ret);
		return 0;
	}
	if (is_ours(category) && str_eq(name, KEY_FONT)) {
		if (value <= 0 || value > font_count)
			cfg.font_file[0] = '\0';
		else
			sceClibStrncpy(cfg.font_file, font_files[value - 1], FONT_NAME_MAX - 1);
		int ret = config_save(&cfg);
		log_line("set font = %s: save 0x%08X\n", cfg.font_file[0] ? cfg.font_file : "(system)", ret);
		return 0;
	}
	if (is_ours(category) && place_key(name) >= 0) {
		int place = place_key(name);
		char *f = cfg.place_font[place];
		if (value <= 0 || value > font_count)
			f[0] = '\0';
		else
			sceClibStrncpy(f, font_files[value - 1], FONT_NAME_MAX - 1);
		f[FONT_NAME_MAX - 1] = '\0';
		int ret = config_save(&cfg);
		log_line("set %s font = %s: save 0x%08X\n", PLACE_NAMES[place], f[0] ? f : "(same as home)", ret);
		return 0;
	}
	if (is_ours(category) && font_app_key(name) >= 0) {
		int app = font_app_key(name);
		char *f = cfg.app_font[app];
		if (value <= 0 || value > font_count + 1)
			f[0] = '\0';
		else if (value == 1)
			sceClibStrncpy(f, FONT_APP_SYSTEM, FONT_NAME_MAX - 1);
		else
			sceClibStrncpy(f, font_files[value - 2], FONT_NAME_MAX - 1);
		f[FONT_NAME_MAX - 1] = '\0';
		int ret = config_save(&cfg);
		log_line("set %s font = %s: save 0x%08X\n", FONT_APPS[app].name, f[0] ? f : "(same as home)", ret);
		return 0;
	}
	if (is_ours(category)) {
		int *field = config_field(&cfg, name);
		if (field) {
			*field = value;
			if ((field == &cfg.enabled || field == &cfg.depth || field == &cfg.layout || field == &cfg.folder_place ||
			     field == &cfg.app_enabled) &&
			    value)
				sceIoRemove(DISABLED_PATH); // a fresh chance after the crash guard tripped
			int ret = config_save(&cfg);
			log_line("set %s = %d: save 0x%08X\n", name, value, ret);
		}
		return 0;
	}
	return TAI_NEXT(reg_set_int_patched, set_ref, category, name, value);
}

static int reg_get_info_patched(const char *category, void *info, int unk) {
	if (is_ours(category)) {
		if (info)
			*(int *)((char *)info + 8) = 0x00040000; // an int key
		return 0;
	}
	return TAI_NEXT(reg_get_info_patched, info_ref, category, info, unk);
}

// scePafToplevelGetText(arg, char **id): the UTF-16 text for an id.
static void *get_text_patched(void *arg, char **id) {
	if (id && *id) {
		const char *text = text_for(*id);
		if (text)
			return (void *)to_u16(text);
	}
	return TAI_NEXT(get_text_patched, text_ref, arg, id);
}

// As SceSettings imports it, this takes six arguments, the last an int *status.
//
// The page hook goes in once the core module has loaded, where it always
// has: the other Settings plugins' hooks keep their order (HENkaku's first,
// then Tweaks', which puts its entry above HENkaku's). taiHEN keeps the first
// hook on a function at the head of its chain and puts each later one
// straight after the head (taiHEN patches.c, hooks_add_hook). A 1.1 test
// build took the head with a pass-through hook so Engram's would run last;
// that moved HENkaku's after Tweaks' and Tweaks' entry disappeared. Where
// Engram runs among them follows its line under *NPXS10015 in
// ur0:tai/config.txt: listed straight after henkaku.suprx, its hook runs
// after Turbo Storage's. See load_xml_patched for when it doesn't.
static SceUID load_start_module_patched(const char *path, SceSize args, void *argp, int flags, void *option,
                                        int *status) {
	SceUID ret = TAI_NEXT(load_start_module_patched, load_ref, path, args, argp, flags, option, status);
#if ENGRAM_EXIT_TRACE
	if (module_logs < 24) {
		module_logs++;
		log_line("Settings loaded %s: 0x%08X\n", path ? path : "(none)", ret);
	}
#endif
	if (!ui_hooked && path && mem_find(path, str_len(path), "system_settings_core", 20)) {
		ui_hooked = 1;
		xml_hook = taiHookFunctionImport(&xml_ref, "SceSettings", LIB_SCEPAFMISC, NID_LOAD_XML_LAYOUT,
		                                 (const void *)load_xml_patched);
		get_hook = taiHookFunctionImport(&get_ref, "SceSystemSettingsCore", LIB_SCEREGMGR, NID_REG_GET_INT,
		                                 (const void *)reg_get_int_patched);
		set_hook = taiHookFunctionImport(&set_ref, "SceSystemSettingsCore", LIB_SCEREGMGR, NID_REG_SET_INT,
		                                 (const void *)reg_set_int_patched);
		info_hook = taiHookFunctionImport(&info_ref, "SceSystemSettingsCore", LIB_SCEREGMGR, NID_REG_GET_INFO,
		                                  (const void *)reg_get_info_patched);
		text_hook = taiHookFunctionImport(&text_ref, "SceSystemSettingsCore", LIB_SCEPAFTOPLEVEL, NID_GET_TEXT,
		                                  (const void *)get_text_patched);
		log_line("settings core loaded: xml=0x%08X get=0x%08X set=0x%08X info=0x%08X text=0x%08X\n", xml_hook, get_hook,
		         set_hook, info_hook, text_hook);
	}
	return ret;
}

void settings_start(void) {
	log_line("\n== Engram " ENGRAM_VERSION " in Settings\n");
	config_load(&cfg);
	sceIoMkdir(FONT_DIR, 0777);
	list_fonts();
	build_group(false, true);
	if (group_len > COMPACT_ROWS_MAX || group_full)
		build_group(false, true, false);
	compact_len = group_len < COMPACT_MAX ? group_len : 0;
	sceClibMemcpy(compact_xml, group_xml, compact_len);
	build_group(true);
	if (group_full) {
		log_line("the page didn't fit in %d bytes with Font > Apps: built without it\n", GROUP_MAX);
		build_group(false);
	}
	log_line("compact rows %d bytes\n", compact_len);
	// Not while the crash guard has Engram off: a font that crashed SceShell
	// stays out of Settings too, which still opens to turn Engram back on.
	if (!file_exists(DISABLED_PATH))
		font_start(font_for_app(&cfg, font_app_index("NPXS10015")), "SceSettings");
	load_hook = taiHookFunctionImport(&load_ref, "SceSettings", LIB_SCELIBKERNEL, NID_LOAD_START_MODULE,
	                                  (const void *)load_start_module_patched);
	log_line("page is %d bytes; load hook 0x%08X\n", group_len, load_hook);
#if ENGRAM_EXIT_TRACE
	exit_hook = taiHookFunctionExport(&exit_ref, "SceLibKernel", TAI_ANY_LIBRARY, NID_EXIT_PROCESS,
	                                  (const void *)exit_process_patched);
	log_line("exit trace: hook 0x%08X\n", exit_hook);
#endif
}

static void release(SceUID *uid, tai_hook_ref_t ref) {
	if (*uid >= 0)
		taiHookRelease(*uid, ref);
	*uid = -1;
}

void settings_stop(void) {
	font_stop();
#if ENGRAM_EXIT_TRACE
	release(&exit_hook, exit_ref);
#endif
	release(&text_hook, text_ref);
	release(&info_hook, info_ref);
	release(&set_hook, set_ref);
	release(&get_hook, get_ref);
	release(&xml_hook, xml_ref);
	release(&load_hook, load_ref);
}
