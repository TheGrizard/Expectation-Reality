/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the app's half of the contract with fm1-base (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "EXPECTATION REALITY"
#define APP_SPLASH_RGB RGB(255, 0, 168)
#define APP_SPLASH                       /* our own first screen (app_splash): the grid, the name knocked out of it */
void app_splash(void);
#define APP_OVERLOAD                     /* a block near its limit: the engine sheds at once (app_overload) */
void app_overload(void);
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h) */
#define APP_CANVAS_PX (240u * 60u)       /* the screen drawn in bands of 60 rows: the pool's RAM goes to the sound */

/* the store: the settings (the groove playing, the saved ones, MIDI) and the panel's calibration */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

/* the host simulator's checks (host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
