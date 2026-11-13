// Font: the font Paf draws the system's text with, swapped for one of yours.
//
// Paf, the UI library SceShell and the system apps are built on, keeps six
// font slots (ScePaf at 0xE03A3714, 0xE03A3604 and 0xE03A3BC6): Japanese,
// Latin, Hangul and Chinese, then the music symbols and one more. Each opens
// the system's shared default font, a font from the system's list, a file,
// or a font in memory that the app hands Paf in Framework::InitParam. Engram
// uses that last way: it reads the chosen font into its own memory and puts
// it in the four language slots of the InitParam the app passes to
// paf::Framework's constructor, so Paf opens it itself, with the app's own
// font settings. Only Paf's text changes: a game drawing its own text isn't
// touched. The music and button symbols keep the system's fonts.
//
// The hook is on the app's own import of the constructor (SceShell's, or the
// Settings app's), which belongs to that app alone. 1.1's first font builds
// hooked ScePaf's imports of the ScePvf calls instead, and ScePaf's code is
// shared by every app that uses it: the patch made in SceShell reached the
// Settings app too, whose Paf then jumped into Engram's code in SceShell's
// memory and crashed (C2-12828-1) before opening a single font. Nothing in a
// module apps share is patched now.
//
// Paf loads after Engram starts, in SceShell (the home screen, LiveArea,
// quick menu, lock screen and notifications) and in the Settings app alike,
// so the constructor's import is hooked once the app's main module has
// loaded Paf with sceSysmoduleLoadModuleInternalWithArg (the app's own
// import again).
//
// Each app picks its font as it starts (Font > Apps, or the Home Screen's),
// so a change reaches an app the next time it opens; SceShell's needs a
// restart. Engram runs in the other system apps only for this, when it's
// listed under their *NPXS sections in ur0:tai/config.txt.

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
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

#define LIB_SCESYSMODULE 0x03FCF19D
#define NID_LOAD_INTERNAL_WITH_ARG 0xC3C26339
#define LIB_SCEPAFTOPLEVEL 0x4D9A9DD0
#define NID_FRAMEWORK_CTOR 0xE2860A99 // paf::Framework::Framework(InitParam const&)

// Framework::InitParam, ScePaf 3.65: from 0x24, an address and a size for a
// font in memory for each language slot, Japanese, Latin, Hangul, Chinese
// (Framework::Framework copies them to the slots at 0xE0176458 + 4 and + 8).
#define INIT_FONT_MEMORY 0x24
#define FONT_SLOTS 4
#define FONT_FILE_MAX (12 * 1024 * 1024)

static char font_path[128];
static const char *font_host = TAI_MAIN_MODULE; // the module whose imports are hooked
static char font_who[32] = "?";                  // its name, for the log
static SceUID load_hook = -1, ctor_hook = -1, font_block = -1;
static tai_hook_ref_t load_ref, ctor_ref;
static void *font_data;
static SceSize font_size;
static int font_tried;

static bool contains(const char *s, const char *part) {
	int n = str_len(part);
	for (; s && *s; s++) {
		int i = 0;
		while (i < n && s[i] && (s[i] | 0x20) == (part[i] | 0x20))
			i++;
		if (i == n)
			return true;
	}
	return false;
}

// The chosen font, read into a memory block of Engram's own. It stays for as
// long as the app runs: Paf reads its glyphs from it.
static bool load_font(void) {
	if (font_tried)
		return font_data != NULL;
	font_tried = 1;
	SceIoStat st;
	if (sceIoGetstat(font_path, &st) < 0 || st.st_size <= 0 || st.st_size > FONT_FILE_MAX) {
		log_line("font: %s: %s is missing, empty or over %d MB\n", font_who, font_path, FONT_FILE_MAX >> 20);
		return false;
	}
	// SceShell has little memory to spare (a 3.4 MB place font crashed it), so
	// the Home Screen font gets the places' limit there. Apps have far more:
	// M PLUS Rounded (3.4 MB) worked in Photos.
	if (font_host && str_eq(font_host, "SceShell") && st.st_size > PLACE_FONT_MAX) {
		log_line("font: %s: %s is %u KB, more than the %u KB the home screen can have: system font\n", font_who,
		         font_path, (unsigned)(st.st_size / 1024), (unsigned)(PLACE_FONT_MAX / 1024));
		return false;
	}
	SceSize size = (SceSize)st.st_size;
	font_block = sceKernelAllocMemBlock("EngramFont", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (size + 0xFFF) & ~0xFFFu,
	                                    NULL);
	void *base = NULL;
	if (font_block < 0 || sceKernelGetMemBlockBase(font_block, &base) < 0) {
		log_line("font: %s: no memory for %s (%u bytes): 0x%08X\n", font_who, font_path, (unsigned)size, font_block);
		font_block = -1;
		return false;
	}
	SceUID fd = sceIoOpen(font_path, SCE_O_RDONLY, 0);
	SceSize got = 0;
	if (fd >= 0) {
		while (got < size) {
			int n = sceIoRead(fd, (char *)base + got, size - got);
			if (n <= 0)
				break;
			got += n;
		}
		sceIoClose(fd);
	}
	if (got != size) {
		log_line("font: %s: read %u of %u bytes of %s\n", font_who, (unsigned)got, (unsigned)size, font_path);
		sceKernelFreeMemBlock(font_block);
		font_block = -1;
		return false;
	}
	font_data = base;
	font_size = size;
	SceKernelFreeMemorySizeInfo mem;
	sceClibMemset(&mem, 0, sizeof(mem));
	mem.size = sizeof(mem);
	if (sceKernelGetFreeMemorySize(&mem) >= 0)
		log_line("font: %s: %s loaded (%u KB), %d KB free after\n", font_who, font_path, (unsigned)(size / 1024),
		         mem.size_user / 1024);
	return true;
}

// The app's InitParam gets the chosen font in its four language slots for the
// constructor's run, then its own values back.
static void *framework_patched(void *self, void *param) {
	uint32_t *slots = (uint32_t *)((char *)param + INIT_FONT_MEMORY);
	uint32_t saved[FONT_SLOTS * 2];
	bool swap = param && load_font();
	if (param)
		sceClibMemcpy(saved, slots, sizeof(saved));
	if (swap)
		for (int i = 0; i < FONT_SLOTS; i++) {
			slots[i * 2] = (uint32_t)font_data;
			slots[i * 2 + 1] = font_size;
		}
	void *ret = TAI_NEXT(framework_patched, ctor_ref, self, param);
	if (swap)
		sceClibMemcpy(slots, saved, sizeof(saved));
	if (param)
		log_line("font: %s: Paf started with %s (the app's own font memory: %08X %08X %08X %08X)\n", font_who,
		         swap ? "your font" : "the system font", saved[0], saved[2], saved[4], saved[6]);
	if (font_host && str_eq(font_host, "SceShell"))
		shell_framework_ready();
	return ret;
}

static void hook_framework(void) {
	if (ctor_hook >= 0)
		return;
	ctor_hook = taiHookFunctionImport(&ctor_ref, font_host, LIB_SCEPAFTOPLEVEL, NID_FRAMEWORK_CTOR,
	                                  (const void *)framework_patched);
	log_line("font: %s: Paf is loaded, Framework hook 0x%08X, using %s\n", font_who, ctor_hook, font_path);
}

static int load_internal_patched(SceSysmoduleInternalModuleId id, SceSize args, void *argp,
                                 const SceSysmoduleOpt *option) {
	int ret = TAI_NEXT(load_internal_patched, load_ref, id, args, argp, option);
	if (ret >= 0 && id == SCE_SYSMODULE_INTERNAL_PAF) {
		hook_framework();
		if (font_host && str_eq(font_host, "SceShell"))
			shell_paf_loaded();
	}
	return ret;
}

bool font_name_ok(const char *name) {
	int n = str_len(name);
	if (n <= 4 || n >= FONT_NAME_MAX)
		return false;
	for (int i = 0; i < n; i++)
		if (name[i] == '/' || name[i] == '\\' || name[i] == ':')
			return false;
	return contains(name + n - 4, ".ttf") || contains(name + n - 4, ".otf") || contains(name + n - 4, ".pvf");
}

void font_start(const char *file, const char *host) {
	if (!file || !file[0] || !font_name_ok(file))
		return;
	font_host = host;
	tai_module_info_t info;
	info.size = sizeof(info);
	if (host) {
		sceClibStrncpy(font_who, host, sizeof(font_who) - 1);
	} else if (taiGetModuleInfo(TAI_MAIN_MODULE, &info) >= 0) {
		sceClibStrncpy(font_who, info.name, sizeof(font_who) - 1);
		info.size = sizeof(info);
	}
	sceClibSnprintf(font_path, sizeof(font_path), FONT_DIR "/%s", file);
	if (!file_exists(font_path)) {
		log_line("font: %s: %s is missing, keeping the system font\n", font_who, font_path);
		return;
	}
	if (taiGetModuleInfo("ScePaf", &info) >= 0) {
		// Too late for this app's Framework if it's already running: hooked
		// anyway, for an app that makes it later.
		hook_framework();
		return;
	}
	load_hook = taiHookFunctionImport(&load_ref, host, LIB_SCESYSMODULE, NID_LOAD_INTERNAL_WITH_ARG,
	                                  (const void *)load_internal_patched);
	log_line("font: %s: waiting for Paf, hook 0x%08X\n", font_who, load_hook);
}

void app_font_start(void) {
	char title[16];
	sceClibMemset(title, 0, sizeof(title));
	if (sceAppMgrAppParamGetString(0, 12, title, sizeof(title) - 1) < 0)
		return;
	int app = font_app_index(title);
	if (app < 0 || file_exists(DISABLED_PATH))
		return;
	static Config cfg; // big: not on module_start's stack
	config_load(&cfg);
	const char *file = font_for_app(&cfg, app);
	if (!file[0])
		return; // the system font: nothing to do
	log_line("\n== Engram " ENGRAM_VERSION " in %s (%s)\n", FONT_APPS[app].name, title);
	font_start(file, TAI_MAIN_MODULE);
}

static void release(SceUID *uid, tai_hook_ref_t ref) {
	if (*uid >= 0)
		taiHookRelease(*uid, ref);
	*uid = -1;
}

// The font's memory stays: Paf may still be drawing with it.
void font_stop(void) {
	release(&ctor_hook, ctor_ref);
	release(&load_hook, load_ref);
}
