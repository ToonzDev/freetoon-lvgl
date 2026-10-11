#ifndef UPDATE_MODAL_H
#define UPDATE_MODAL_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the About / Update modal.
 * If parent is NULL, attaches to lv_scr_act(). */
void update_modal_open(lv_obj_t * parent);

/* Close the About / Update modal if currently visible. */
void update_modal_close(void);

/* Check if the modal is currently open. */
bool update_modal_is_open(void);

/* Refresh the modal status & buttons if open.
 * Can be called from screen tick / timer loops. */
void update_modal_tick(void);

/* True if the currently running build already matches latest_version. */
bool update_modal_already_installed(void);

/* Minimize / dismissal state for the home screen banner. */
bool update_modal_is_minimized(void);
void update_modal_set_minimized(bool min);

/* Version skipping state for the home screen banner. */
const char * update_modal_get_skipped_version(void);
void update_modal_set_skipped_version(const char * ver);

#ifdef __cplusplus
}
#endif

#endif /* UPDATE_MODAL_H */
