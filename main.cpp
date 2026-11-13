// Engram's entry points. The same module runs in two processes; see engram.h.

#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include "engram.h"

// GCC emits calls to these even with -fno-builtin; there is no libc to provide them.
extern "C" void *memset(void *dst, int value, size_t size) {
	return sceClibMemset(dst, value, size);
}
extern "C" void *memcpy(void *dst, const void *src, size_t size) {
	return sceClibMemcpy(dst, src, size);
}

enum Host { HOST_NONE, HOST_SHELL, HOST_SETTINGS, HOST_APP };
static Host host = HOST_NONE;

static bool module_loaded(const char *name) {
	tai_module_info_t info;
	info.size = sizeof(info);
	return taiGetModuleInfo(name, &info) >= 0;
}

extern "C" {

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	sceIoMkdir(DATA_DIR, 0777);

	if (module_loaded("SceShell")) {
		host = HOST_SHELL;
		shell_start();
	} else if (module_loaded("SceSettings")) {
		host = HOST_SETTINGS;
		settings_start();
	} else {
		host = HOST_APP; // another system app: Font > Apps only
		app_font_start();
	}
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	if (host == HOST_SHELL)
		shell_stop();
	else if (host == HOST_SETTINGS)
		settings_stop();
	else if (host == HOST_APP)
		font_stop();
	return SCE_KERNEL_STOP_SUCCESS;
}

}
