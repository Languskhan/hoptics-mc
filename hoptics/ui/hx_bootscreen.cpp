// Hoptics-MC start-up screen: the Hoptics boot art (a different picture each start), the node-and-arcs logo, and a
// line and a bar that follow MeshCore's start-up steps. Shown by the board as soon as the panel is up, before
// MeshCore's slow steps (formatting storage on the first start after installing takes up to a minute); App::begin()
// replaces it. Nothing animates: during start-up nothing else runs, so each step redraws it once. MIT licence.
#include "hx_ui.h"
#include <cstdio>

#if __has_include("hx_boot_bg.h") // made by tools/png_to_lvgl.py from assets/ for the panel's orientation
#include "hx_boot_bg.h"
#define HX_HAS_BOOT_BG 1
#endif

namespace hx
{
namespace
{
lv_obj_t *bootScr = nullptr, *bootLine = nullptr, *bootBar = nullptr, *bootNote = nullptr;
// the Midnight theme's colours (the user's theme isn't loaded yet)
const lv_color_t BG = lv_color_hex(0x0A0F14), CARD2 = lv_color_hex(0x1B2531), LINE = lv_color_hex(0x243141),
                 TEXT = lv_color_hex(0xE6EDF3), MUTED = lv_color_hex(0x8695A5), DIM = lv_color_hex(0x55636F),
                 CYAN = lv_color_hex(0x22D3EE), GREEN = lv_color_hex(0x34D399), VIOLET = lv_color_hex(0xA78BFA),
                 AMBER = lv_color_hex(0xFBBF24);
// what happens next, after each step (MeshCore's companion setup(), see hx_boot.cpp)
const char *const NEXT[] = {"Starting the radio...",
                            "Starting the radio...",
                            "Opening storage...",
                            "Loading contacts and channels...",
                            "Starting Bluetooth...",
                            "Looking for GPS and sensors...",
                            "Opening the screens...",
                            "Ready",
                            "Ready"};

lv_obj_t *text(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    return l;
}
} // namespace

extern "C" {
LV_FONT_DECLARE(hx_emoji_12)
}

const uint8_t *bootArt(int i, int32_t &w, int32_t &h)
{
#ifdef HX_HAS_BOOT_BG
    if (i < 0 || i >= HOPTICS_BOOT_BG_COUNT)
        return nullptr;
    w = HOPTICS_BOOT_BG_W;
    h = HOPTICS_BOOT_BG_H;
    return hoptics_boot_bgs[i];
#else
    (void)i, (void)w, (void)h;
    return nullptr;
#endif
}

void bootShow(const char *board, const char *version, int art)
{
    lv_display_t *d = lv_display_get_default();
    if (!d || bootScr)
        return;
    const int32_t w = lv_display_get_horizontal_resolution(d), h = lv_display_get_vertical_resolution(d);
    const bool land = w > h;
    bootScr = lv_obj_create(nullptr);
    lv_obj_remove_flag(bootScr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bootScr, BG, 0);
    lv_obj_set_style_bg_grad_color(bootScr, CARD2, 0);
    lv_obj_set_style_bg_grad_dir(bootScr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(bootScr, 0, 0);
    lv_obj_set_style_border_width(bootScr, 0, 0);
#ifdef HX_HAS_BOOT_BG
    if (art >= 0 && w == HOPTICS_BOOT_BG_W && h == HOPTICS_BOOT_BG_H) { // drawn straight from flash
        static lv_image_dsc_t bg;
        bg.header.magic = LV_IMAGE_HEADER_MAGIC;
        bg.header.cf = LV_COLOR_FORMAT_RGB565;
        bg.header.flags = 0;
        bg.header.w = HOPTICS_BOOT_BG_W;
        bg.header.h = HOPTICS_BOOT_BG_H;
        bg.header.stride = HOPTICS_BOOT_BG_W * 2;
        bg.data_size = HOPTICS_BOOT_BG_W * HOPTICS_BOOT_BG_H * 2;
        bg.data = hoptics_boot_bgs[art % HOPTICS_BOOT_BG_COUNT];
        lv_obj_t *im = lv_image_create(bootScr);
        lv_image_set_src(im, &bg);
        lv_obj_set_pos(im, 0, 0);
    }
#else
    (void)art;
#endif
    // the logo: a node with three radio arcs (higher and smaller on a landscape panel)
    const int32_t cx = w / 2, cy = land ? 62 : h / 2 - 48;
    const int32_t radii[3] = {land ? 20 : 26, land ? 34 : 44, land ? 48 : 62};
    const lv_color_t arcC[3] = {CYAN, GREEN, VIOLET};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *a = lv_arc_create(bootScr);
        lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(a, radii[i] * 2, radii[i] * 2);
        lv_obj_set_pos(a, cx - radii[i], cy - radii[i]);
        lv_arc_set_bg_angles(a, 0, 360);
        lv_arc_set_angles(a, 220, 320);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, arcC[i], LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    }
    lv_obj_t *dot = lv_obj_create(bootScr);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_pos(dot, cx - 8, cy - 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, CYAN, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(dot, CYAN, 0);
    lv_obj_set_style_shadow_width(dot, 18, 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_60, 0);

    // HOPTICS MC
    lv_obj_t *title = text(bootScr, &lv_font_montserrat_28, TEXT, "HOPTICS #22D3EE MC#");
    lv_label_set_recolor(title, true);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, land ? -6 : 30);
    lv_obj_t *tag = text(bootScr, &hx_emoji_12, MUTED, "mesh, made visible");
    lv_obj_align_to(tag, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    // what it's doing and how far along, then the version: one column up from the bottom, so a line that
    // wraps on a narrow panel pushes the others up instead of running into them
    lv_obj_t *col = lv_obj_create(bootScr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, w - 16, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 4, 0);
    bootBar = lv_bar_create(col);
    lv_obj_set_size(bootBar, land ? 140 : 150, 4);
    lv_bar_set_range(bootBar, 0, 8);
    lv_obj_set_style_bg_color(bootBar, LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bootBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bootBar, CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bootBar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bootBar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_margin_bottom(bootBar, 4, 0);
    bootLine = text(col, &hx_emoji_12, TEXT, NEXT[0]);
    bootNote = text(col, &lv_font_montserrat_10, MUTED, "");
    lv_obj_set_width(bootNote, LV_PCT(100));
    lv_obj_set_style_text_align(bootNote, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(bootNote, LV_OBJ_FLAG_HIDDEN);
    char b[80];
    snprintf(b, sizeof(b), land ? "%s  -  %s" : "%s\n%s", version ? version : "", board ? board : "");
    lv_obj_t *ver = text(col, &lv_font_montserrat_10, DIM, b);
    lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(ver, 6, 0);

    lv_obj_t *old = lv_screen_active();
    lv_screen_load(bootScr);
    if (old && old != bootScr)
        lv_obj_delete(old); // the display's empty default screen
    lv_refr_now(d);
}

void bootStep(int step)
{
    if (!bootScr || step < 0 || step > 8)
        return;
    lv_label_set_text(bootLine, NEXT[step]);
    lv_label_set_text(bootNote, "The first start after installing takes up to a minute.");
    if (step == 2) // the next step formats storage on a first start
        lv_obj_remove_flag(bootNote, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(bootNote, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(bootBar, step, LV_ANIM_OFF);
    lv_refr_now(nullptr);
}

void bootError(const char *what, const char *todo)
{
    if (!bootScr)
        return;
    lv_obj_set_style_text_color(bootLine, AMBER, 0);
    lv_label_set_text(bootLine, what ? what : "Start-up stopped");
    lv_obj_set_style_text_color(bootNote, TEXT, 0);
    lv_label_set_text(bootNote, todo ? todo : "");
    lv_obj_remove_flag(bootNote, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(bootBar, AMBER, LV_PART_INDICATOR);
    lv_refr_now(nullptr);
}

// the UI's own screen is up: the start-up screen goes (App::begin)
void bootDone(void)
{
    if (!bootScr)
        return;
    if (lv_screen_active() != bootScr)
        lv_obj_delete(bootScr);
    bootScr = bootLine = bootBar = bootNote = nullptr;
}
} // namespace hx
