/*
 * plugin_loader.c — Dynamic Shared Object (.so) Plugin Screen Loader for FreeToon.
 *
 * Allows marketplace applications to provide their own compiled LVGL detail
 * screen (.so plugin) without requiring any app-specific code in toonui.
 */

#include "plugin_loader.h"
#include "screens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#define MAX_CACHED_PLUGINS 8
static struct {
    char id[64];
    void * handle;
    lv_obj_t * (*create_fn)(void);
} s_plugins[MAX_CACHED_PLUGINS];
static int s_plugin_count = 0;

int plugin_screen_open(const integration_meta_t * meta) {
    if (!meta || !meta->id[0]) return 0;

    /* Check if already loaded in memory to prevent dlclose-on-thread crashes */
    for (int i = 0; i < s_plugin_count; i++) {
        if (strcmp(s_plugins[i].id, meta->id) == 0 && s_plugins[i].create_fn) {
            lv_obj_t * scr = s_plugins[i].create_fn();
            if (scr) {
                ui_push(scr);
                return 1;
            }
            return 0;
        }
    }

    struct utsname uts;
    const char * arch = "toon2";
    if (uname(&uts) == 0 && strncmp(uts.machine, "armv5", 5) == 0) {
        arch = "toon1";
    }

    char path[256];
    struct stat st;

    /* 1. Try arch-specific naming: <id>_ui.<arch>.so (e.g. sonos_ui.toon2.so) */
    snprintf(path, sizeof path, "/mnt/data/integrations/%s/%s_ui.%s.so",
             meta->id, meta->id, arch);
    if (stat(path, &st) != 0) {
        /* 2. Try generic <id>_ui.so */
        snprintf(path, sizeof path, "/mnt/data/integrations/%s/%s_ui.so",
                 meta->id, meta->id);
    }
    if (stat(path, &st) != 0) {
        /* 3. Try ui.so */
        snprintf(path, sizeof path, "/mnt/data/integrations/%s/ui.so",
                 meta->id);
    }
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[plugin] no UI library found for '%s'\n", meta->id);
        return 0;
    }

    fprintf(stderr, "[plugin] loading %s ...\n", path);
    void * h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[plugin] dlopen failed for %s: %s\n", path, dlerror());
        return 0;
    }

    /* Look for entry function */
    lv_obj_t * (*create_fn)(void) = (lv_obj_t * (*)(void))dlsym(h, "screen_create");
    if (!create_fn) {
        char alt_sym[64];
        snprintf(alt_sym, sizeof alt_sym, "%s_screen_create", meta->id);
        create_fn = (lv_obj_t * (*)(void))dlsym(h, alt_sym);
    }
    if (!create_fn) {
        fprintf(stderr, "[plugin] no screen_create symbol in %s: %s\n", path, dlerror());
        dlclose(h);
        return 0;
    }

    lv_obj_t * scr = create_fn();
    if (!scr) {
        fprintf(stderr, "[plugin] %s screen_create returned NULL\n", meta->id);
        dlclose(h);
        return 0;
    }

    if (s_plugin_count < MAX_CACHED_PLUGINS) {
        snprintf(s_plugins[s_plugin_count].id, sizeof(s_plugins[s_plugin_count].id), "%s", meta->id);
        s_plugins[s_plugin_count].handle = h;
        s_plugins[s_plugin_count].create_fn = create_fn;
        s_plugin_count++;
    }

    ui_push(scr);
    return 1;
}
