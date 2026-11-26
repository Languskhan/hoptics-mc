// Hoptics-MC LVGL configuration (LVGL 9.6). Anything not set here keeps LVGL's default. MIT licence.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
// memory: on the boards, PSRAM first and internal RAM as the fallback (hoptics/hal/hx_lvgl_mem.cpp); the C library's
// malloc would keep LVGL's small objects in the internal RAM that Bluetooth and WiFi need. The PC harness uses malloc.
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#endif
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 15 // redraws and touch reads every 15 ms: quick to answer
#define LV_DPI_DEF 130
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_PERF_MONITOR 0
#define LV_USE_SYSMON 0
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW 1
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (16 * 1024)
#define LV_CACHE_DEF_SIZE 0

// fonts (LVGL's built-in Montserrat, SIL OFL 1.1, with the LV_SYMBOL icons)
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1 // the lock screen's clock
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// widgets in use
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_TEXTAREA 1
#define LV_USE_DROPDOWN 1
#define LV_USE_SWITCH 1
#define LV_USE_SLIDER 1
#define LV_USE_CHART 1
#define LV_USE_KEYBOARD 1 /* on-screen typing on boards without a keyboard */
#define LV_USE_CALENDAR 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_MSGBOX 0
#define LV_USE_MENU 0
#define LV_USE_LIST 0
#define LV_USE_ANIMIMG 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_LED 0
#define LV_USE_SCALE 0
#define LV_USE_TABLE 0
#define LV_USE_BUTTONMATRIX 1 /* the textarea needs it */
#define LV_USE_FLEX 1
#define LV_USE_GRID 0
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_LIBINPUT 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
