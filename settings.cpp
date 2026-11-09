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

#include <psp2/io/fcntl.h>
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
#define GROUP_MAX 8192

static Config cfg;

// ---- the page ----

static char group_xml[GROUP_MAX];
static int group_len;
static char page_buf[PAGE_ORIG_MAX + GROUP_MAX];

static void put(const char *text) {
	int n = str_len(text);
	if (group_len + n >= GROUP_MAX)
		return;
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
	for (int i = 0; i < count; i++) {
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
//   Live Area
//     Scroll Animations   switch, Effects (one switch each), Randomize, Fade Out, Intensity, 3D Icon Tilt
//     Motion Depth        switch, Style, Strength, Shadows, Wallpaper Parallax
//   Credits
// More categories (a Lock Screen one, say) go next to Live Area.
static void build_group(void) {
	group_len = 0;
	put("<setting_list id=\"engram_settings\" title=\"eg_p_title\" icon=\"tex_spanner\">");

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
	put_toggle(KEY_WALLPAPER);
#endif
	put("</setting_list>");

	put("</setting_list>"); // Live Area

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
static u16 text_ring[8][256];
static int text_next;

static const u16 *to_u16(const char *text) {
	u16 *out = text_ring[text_next];
	text_next = (text_next + 1) % 8;
	int i = 0;
	for (; text[i] && i < 255; i++)
		out[i] = (unsigned char)text[i];
	out[i] = 0;
	return out;
}

static const char *const INTENSITY_NAMES[INTENSITY_COUNT] = {"Subtle (50%)", "Gentle (75%)", "Normal (100%)",
                                                             "Strong (125%)", "Extreme (150%)"};
static const char *const FADE_OUT_NAMES[FADE_OUT_COUNT] = {"Off", "During the Swipe", "As the Next Page Lands"};
static const char *const DEPTH_STYLE_NAMES[DEPTH_STYLE_COUNT] = {"Float", "Turn to Face You", "Float and Turn"};
static const char *const DEPTH_STRENGTH_NAMES[DEPTH_STRENGTH_COUNT] = {"Subtle", "Normal", "Strong", "Extreme"};
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
};

static const char *page_title(const char *id) {
	if (str_eq(id, "title"))
		return "Engram";
	if (str_eq(id, "livearea"))
		return "Live Area";
	if (str_eq(id, "scroll"))
		return "Scroll Animations";
	if (str_eq(id, "effects"))
		return "Effects";
	if (str_eq(id, "depth"))
		return "Motion Depth";
	return NULL;
}

static const char *title_for(const char *key) {
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
	if (str_eq(key, KEY_DEPTH_STRENGTH))
		return "Strength";
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

#define CRASH_GUARD_HINT "Engram turned itself off after a boot that didn't finish. Turn this on, then restart, to try again."

static const char *hint_for(const char *key) {
	bool guard_tripped = (str_eq(key, KEY_ENABLED) || str_eq(key, KEY_DEPTH)) && file_exists(DISABLED_PATH);
	if (guard_tripped)
		return CRASH_GUARD_HINT;
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
	for (unsigned l = 0; l < sizeof(LISTS) / sizeof(LISTS[0]); l++) {
		if (!str_starts(id, LISTS[l].prefix))
			continue;
		const char *n = id + str_len(LISTS[l].prefix);
		if (n[0] >= '0' && n[0] < '0' + LISTS[l].count && !n[1])
			return LISTS[l].names[n[0] - '0'];
	}
	return NULL;
}

static const char *text_for(const char *id) {
	if (!str_starts(id, "eg_"))
		return NULL;
	if (str_eq(id, "eg_c_title"))
		return "Credits";
	if (str_eq(id, "eg_c_text"))
		return "Scroll Animations are a faithful port of Cylinder for iOS by Reed Weichler (rweichler). "
		       "Motion Depth is inspired by ShadowBoard for iOS. PS Vita port by badmanwazzy37.";
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
			log_line("system page (%d bytes): insert point %d\n", size, at ? (int)(at - s) : -1);
			if (at) {
				int head = (int)(at - s);
				sceClibMemcpy(page_buf, s, head);
				sceClibMemcpy(page_buf + head, group_xml, group_len);
				sceClibMemcpy(page_buf + head + group_len, at, size - head);
				buf = page_buf;
				size += group_len;
			}
		}
	}
	return TAI_NEXT(load_xml_patched, xml_ref, out, buf, size, a4);
}

static int reg_get_int_patched(const char *category, const char *name, int *value) {
	if (is_ours(category) && value) {
		int *field = config_field(&cfg, name);
		*value = field ? *field : 0;
		if ((field == &cfg.enabled || field == &cfg.depth) && file_exists(DISABLED_PATH))
			*value = 0;
		return 0;
	}
	return TAI_NEXT(reg_get_int_patched, get_ref, category, name, value);
}

static int reg_set_int_patched(const char *category, const char *name, int value) {
	if (is_ours(category)) {
		int *field = config_field(&cfg, name);
		if (field) {
			*field = value;
			if ((field == &cfg.enabled || field == &cfg.depth) && value)
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
static SceUID load_start_module_patched(const char *path, SceSize args, void *argp, int flags, void *option,
                                        int *status) {
	SceUID ret = TAI_NEXT(load_start_module_patched, load_ref, path, args, argp, flags, option, status);
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
	build_group();
	load_hook = taiHookFunctionImport(&load_ref, "SceSettings", LIB_SCELIBKERNEL, NID_LOAD_START_MODULE,
	                                  (const void *)load_start_module_patched);
	log_line("page is %d bytes; load hook 0x%08X\n", group_len, load_hook);
}

static void release(SceUID *uid, tai_hook_ref_t ref) {
	if (*uid >= 0)
		taiHookRelease(*uid, ref);
	*uid = -1;
}

void settings_stop(void) {
	release(&text_hook, text_ref);
	release(&info_hook, info_ref);
	release(&set_hook, set_ref);
	release(&get_hook, get_ref);
	release(&xml_hook, xml_ref);
	release(&load_hook, load_ref);
}
