// Hoptics-MC screens. MIT licence, see LICENSE.
#include "hx_ui.h"
#include "map/hx_tiles.h"
#include "src/misc/cache/instance/lv_image_cache.h" // lv_image_cache_drop: the wallpaper reuses its buffer
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <strings.h>

extern "C" {
LV_FONT_DECLARE(hx_emoji_12)
LV_FONT_DECLARE(hx_emoji_14)
LV_FONT_DECLARE(hx_emoji_16)
LV_FONT_DECLARE(hx_text_10)
LV_FONT_DECLARE(hx_text_20)
LV_FONT_DECLARE(hx_text_28)
}

namespace hx
{

bool ConvKey::operator==(const ConvKey &o) const
{
    return dm == o.dm && (dm ? memcmp(key, o.key, 6) == 0 : channel == o.channel);
}

const char *nodeTypeName(NodeType t)
{
    switch (t) {
    case NodeType::Chat:
        return "Chat";
    case NodeType::Repeater:
        return "Repeater";
    case NodeType::Room:
        return "Room";
    case NodeType::Sensor:
        return "Sensor";
    default:
        return "Node";
    }
}

const char *pktTypeName(PktType t)
{
    static const char *n[] = {"REQ", "RESP", "TEXT", "ACK", "ADVERT", "GROUP", "GDATA", "ANON", "PATH", "TRACE", "MULTI", "CTRL", "RAW", "OTHER"};
    unsigned i = (unsigned)t;
    return i < sizeof(n) / sizeof(n[0]) ? n[i] : "OTHER";
}

namespace
{
// labels refreshed on a timer: setting the same text again still makes LVGL lay out and redraw them, so only real
// changes go through
void setText(lv_obj_t *l, const char *t)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t) != 0)
        lv_label_set_text(l, t);
}
void setColor(lv_obj_t *l, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(l, c, 0);
}
void setTextFmt(lv_obj_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void setTextFmt(lv_obj_t *l, const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    setText(l, b);
}

// ------------------------------------------------------------------ palette (the Hoptics themes)
lv_color_t C_BG, C_BAR, C_CARD, C_CARD2, C_LINE, C_TEXT, C_MUTED, C_DIM, C_CYAN, C_GREEN, C_AMBER, C_RED, C_VIOLET, C_PINK,
    C_BLUE, C_ME, C_ON_ME, C_ME_META, C_ON_ACCENT;
bool T_BORDERS = false;
bool T_GLASS = false;   // the Glass themes: Midnight's colours, rounder cards with a coloured edge
bool T_RAINBOW = false; // Glass rainbow: each card's edge the next colour round the wheel
int32_t T_RADIUS = 10;  // cards' corners
lv_color_t C_EDGE;      // the Glass themes' card edge
int rainbowAt = 0;
// the edge for a card, tile or row; in Glass rainbow, a colour of the rainbow: the next one for cards and tiles (built
// once), and by position for list rows (rebuilt as the list changes, so a row's colour stays put)
lv_color_t edgeColor(int at = -1)
{
    if (!T_RAINBOW)
        return C_EDGE;
    // red, orange, yellow, green, cyan, indigo, violet: each 60% over the card colour, so they glow without glaring
    static const uint32_t R[7] = {0xB84A55, 0xA8653A, 0x9E8A2C, 0x2F8F62, 0x2591AD, 0x5F63B8, 0x8A58B0};
    return lv_color_hex(R[(at >= 0 ? at : rainbowAt++) % 7]);
}

void palette(int t)
{
    T_BORDERS = false;
    T_GLASS = false;
    T_RAINBOW = false;
    T_RADIUS = 10;
    rainbowAt = 0;
    auto set = [](uint32_t bg, uint32_t bar, uint32_t card, uint32_t card2, uint32_t line, uint32_t text, uint32_t muted,
                  uint32_t dim, uint32_t cyan, uint32_t green, uint32_t amber, uint32_t red, uint32_t violet, uint32_t pink,
                  uint32_t blue, uint32_t me, uint32_t onMe, uint32_t meMeta, uint32_t onAccent) {
        C_BG = lv_color_hex(bg), C_BAR = lv_color_hex(bar), C_CARD = lv_color_hex(card), C_CARD2 = lv_color_hex(card2);
        C_LINE = lv_color_hex(line), C_TEXT = lv_color_hex(text), C_MUTED = lv_color_hex(muted), C_DIM = lv_color_hex(dim);
        C_CYAN = lv_color_hex(cyan), C_GREEN = lv_color_hex(green), C_AMBER = lv_color_hex(amber), C_RED = lv_color_hex(red);
        C_VIOLET = lv_color_hex(violet), C_PINK = lv_color_hex(pink), C_BLUE = lv_color_hex(blue), C_ME = lv_color_hex(me);
        C_ON_ME = lv_color_hex(onMe), C_ME_META = lv_color_hex(meMeta), C_ON_ACCENT = lv_color_hex(onAccent);
    };
    switch (t) {
    case 1: // Daylight
        set(0xF2F4F7, 0xFFFFFF, 0xFFFFFF, 0xE3E8EE, 0xB8C2CE, 0x05090F, 0x2E3A48, 0x5A6675, 0x0369A1, 0x047857, 0xB45309,
            0xB91C1C, 0x6D28D9, 0xBE185D, 0x1D4ED8, 0x0369A1, 0xFFFFFF, 0xD6EBFA, 0xFFFFFF);
        T_BORDERS = true;
        break;
    case 2: // Night red
        set(0x000000, 0x070000, 0x110202, 0x1C0303, 0x380808, 0xE23B3B, 0xA82A2A, 0x6E1B1B, 0xFF3B30, 0xD93025, 0xFF6A3D,
            0xFF2020, 0xB8322A, 0xFF5A4A, 0xC8322A, 0x450909, 0xFF5050, 0xB03030, 0x000000);
        break;
    case 3: // Amber
        set(0x050400, 0x0A0800, 0x120E02, 0x1C1604, 0x3A2C08, 0xFFC24A, 0xC08A20, 0x7A5A14, 0xFFB000, 0xE8DC6A, 0xFFD166,
            0xFF6B35, 0xE0A030, 0xFF8C42, 0xFFC857, 0x5A3E00, 0xFFE0A0, 0xD0A040, 0x000000);
        break;
    case 4: // the Glass family: Midnight's dark colours, rounder cards and a thin coloured edge that catches the eye like
    case 5: // a glass rim, in the theme's own accent colour. Solid colours only, so they draw as fast as the others
    case 6:
    case 7:
    case 8:
    case 9: {
        struct Glass {
            uint32_t accent, edge, me, meMeta;
        };
        // accent (buttons, selection, focus), edge (the accent 60% over the card colour), my chat bubble and its text
        static const Glass G[6] = {
            {0x22D3EE, 0x2591AD, 0x0E7490, 0xA5E9F5}, // Glass: light blue
            {0xA78BFA, 0x6C5EA4, 0x5B3FA8, 0xDDD3FF}, // Glass violet
            {0x34D399, 0x27896A, 0x0F6E52, 0xB6F2DA}, // Glass green
            {0xF472B6, 0x9A4F7C, 0x8E2F64, 0xFFD0E8}, // Glass rose
            {0xFBBF24, 0x9E7D24, 0x7A5A0E, 0xFFE7A8}, // Glass gold
            {0x22D3EE, 0x2591AD, 0x0E7490, 0xA5E9F5}, // Glass rainbow (edges go round the rainbow)
        };
        const Glass &g = G[t - 4];
        set(0x0A0F14, 0x0E151C, 0x131B24, 0x1B2531, 0x243141, 0xE6EDF3, 0x8695A5, 0x55636F, g.accent, 0x34D399, 0xFBBF24,
            0xF87171, 0xA78BFA, 0xF472B6, 0x60A5FA, g.me, 0xFFFFFF, g.meMeta, 0x0A0F14);
        C_EDGE = lv_color_hex(g.edge);
        T_BORDERS = true;
        T_GLASS = true;
        T_RAINBOW = t == 9;
        T_RADIUS = 16;
        break;
    }
    default: // Midnight
        set(0x0A0F14, 0x0E151C, 0x131B24, 0x1B2531, 0x243141, 0xE6EDF3, 0x8695A5, 0x55636F, 0x22D3EE, 0x34D399, 0xFBBF24,
            0xF87171, 0xA78BFA, 0xF472B6, 0x60A5FA, 0x0E7490, 0xE6EDF3, 0xA5E9F5, 0x0A0F14);
        break;
    }
}

// 12, 14 and 16 px carry emoji (Noto Emoji, hoptics/ui/fonts) and fall back to Montserrat for everything else, so
// messages, names and lists show 😀 👍 ❤️ instead of boxes
const lv_font_t *F10 = &hx_text_10, *F12 = &hx_emoji_12, *F14 = &hx_emoji_14, *F16 = &hx_emoji_16, *F20 = &hx_text_20,
                *F28 = &hx_text_28;

int32_t W(void) { return lv_display_get_horizontal_resolution(lv_display_get_default()); }
int32_t H(void) { return lv_display_get_vertical_resolution(lv_display_get_default()); }
bool LAND = true;
constexpr int32_t PAD = 6;
int32_t STATUS_H = 20, RAIL_W = 46, NAV_H = 40;

constexpr lv_obj_flag_t F_STOP = LV_OBJ_FLAG_USER_1;   // reachable with keys
constexpr lv_obj_flag_t F_SUBMIT = LV_OBJ_FLAG_USER_2; // a text field whose Enter does something (LV_EVENT_READY)

lv_style_t stFocus;
bool styleReady = false;

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
void column(lv_obj_t *o, int32_t gap)
{
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
}
void row(lv_obj_t *o, int32_t gap)
{
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
}
lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, const char *t = "")
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    return l;
}
void oneLine(lv_obj_t *l)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
}
void wrap(lv_obj_t *l)
{
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
}
void stop(lv_obj_t *o)
{
    lv_obj_add_flag(o, F_STOP);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_style(o, &stFocus, LV_STATE_FOCUS_KEY);
}
// list rows in the Glass theme: rounder, with the light blue edge
void rowEdge(lv_obj_t *r)
{
    if (!T_GLASS)
        return;
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, edgeColor((int)lv_obj_get_index(r)), 0);
}

lv_obj_t *card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *c = plain(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, T_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    if (T_BORDERS) {
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, T_GLASS ? edgeColor() : C_LINE, 0);
    }
    column(c, 4);
    if (title) {
        lv_obj_t *t = label(c, F10, C_MUTED, title);
        lv_obj_set_width(t, LV_PCT(100));
        oneLine(t);
    }
    return c;
}
lv_obj_t *pill(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = label(parent, F10, color, text);
    lv_obj_set_style_bg_color(l, color, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_20, 0);
    lv_obj_set_style_radius(l, 4, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    return l;
}
lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t color, int32_t w, int32_t h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, color, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = label(b, F14, color, text);
    lv_obj_center(l);
    stop(b);
    return b;
}
void setButtonText(lv_obj_t *b, const char *t)
{
    if (lv_obj_t *l = lv_obj_get_child(b, 0))
        lv_label_set_text(l, t);
}
void textHooks(lv_obj_t *ta); // the on-screen keyboard's hooks (after `self`, below)
void styleInput(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_textarea_class))
        textHooks(o);
    lv_obj_set_style_bg_color(o, C_CARD2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, C_LINE, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, C_CYAN, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(o, C_TEXT, 0);
    lv_obj_set_style_text_font(o, F14, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_ver(o, 6, 0);
    lv_obj_set_style_pad_hor(o, 8, 0);
}
lv_obj_t *dropdown(lv_obj_t *parent, const char *opts)
{
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_obj_set_width(d, LV_PCT(100));
    styleInput(d);
    lv_obj_t *list = lv_dropdown_get_list(d);
    lv_obj_set_style_bg_color(list, C_CARD2, 0);
    lv_obj_set_style_text_color(list, C_TEXT, 0);
    lv_obj_set_style_text_font(list, F14, 0);
    lv_obj_set_style_border_color(list, C_LINE, 0);
    lv_obj_set_style_bg_color(list, C_CYAN, (lv_style_selector_t)LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, C_ON_ACCENT, (lv_style_selector_t)LV_PART_SELECTED | LV_STATE_CHECKED);
    stop(d);
    return d;
}
lv_obj_t *toggle(lv_obj_t *parent, const char *text, lv_obj_t **sw)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_size(r, LV_PCT(100), 30);
    row(r, 6);
    lv_obj_t *l = label(r, F14, C_TEXT, text);
    lv_obj_set_flex_grow(l, 1);
    oneLine(l);
    *sw = lv_switch_create(r);
    lv_obj_set_size(*sw, 40, 22);
    lv_obj_set_style_bg_color(*sw, C_CARD2, 0);
    lv_obj_set_style_bg_color(*sw, C_CYAN, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    stop(*sw);
    return r;
}
lv_obj_t *fieldRow(lv_obj_t *parent, const char *text)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    column(r, 3);
    wrap(label(r, F12, C_MUTED, text));
    return r;
}

// a node's colour: the same for a key everywhere on the screen
lv_color_t keyColor(const uint8_t *k)
{
    uint32_t h = (uint32_t)k[0] << 16 | (uint32_t)k[1] << 8 | k[2];
    h *= 2654435761u;
    return lv_color_hsv_to_rgb((h >> 8) % 360, 55, 88);
}
lv_color_t typeColor(NodeType t)
{
    switch (t) {
    case NodeType::Repeater:
        return C_GREEN;
    case NodeType::Room:
        return C_VIOLET;
    case NodeType::Sensor:
        return C_AMBER;
    default:
        return C_CYAN;
    }
}
lv_color_t pktColor(PktType t)
{
    switch (t) {
    case PktType::Text:
    case PktType::GroupText:
        return C_CYAN;
    case PktType::Advert:
        return C_GREEN;
    case PktType::Ack:
        return C_DIM;
    case PktType::Path:
    case PktType::Trace:
        return C_PINK;
    case PktType::Req:
    case PktType::Resp:
    case PktType::AnonReq:
        return C_AMBER;
    default:
        return C_BLUE;
    }
}
lv_obj_t *badge(lv_obj_t *parent, const uint8_t *key, const char *name, int32_t size)
{
    lv_obj_t *b = plain(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, 8, 0);
    lv_color_t c = keyColor(key);
    lv_obj_set_style_bg_color(b, c, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    // initials: the first letter of the first two words ("Folsom Ridge" -> FR), or two letters of a single word
    char ini[3] = {0};
    int n = 0;
    bool wordStart = true;
    for (const char *p = name; *p && n < 2; p++) {
        bool alnum = isalnum((unsigned char)*p);
        if (alnum && wordStart)
            ini[n++] = (char)toupper((unsigned char)*p);
        wordStart = !alnum;
    }
    if (n == 1)
        for (const char *p = name; *p; p++)
            if (isalnum((unsigned char)*p) && p != name && isalnum((unsigned char)p[-1])) {
                ini[1] = (char)toupper((unsigned char)*p);
                break;
            }
    if (!ini[0])
        snprintf(ini, sizeof(ini), "%02X", key[0]);
    lv_obj_t *l = label(b, F14, lv_color_luminance(c) > 140 ? lv_color_hex(0x05090F) : lv_color_white(), ini);
    lv_obj_center(l);
    return b;
}

std::string ago(uint32_t now, uint32_t t)
{
    if (!t || !now || t > now + 60)
        return "-";
    uint32_t d = now > t ? now - t : 0;
    char b[16];
    if (d < 60)
        snprintf(b, sizeof(b), "%us", (unsigned)d);
    else if (d < 3600)
        snprintf(b, sizeof(b), "%um", (unsigned)(d / 60));
    else if (d < 86400)
        snprintf(b, sizeof(b), "%uh", (unsigned)(d / 3600));
    else
        snprintf(b, sizeof(b), "%ud", (unsigned)(d / 86400));
    return b;
}
std::string route(int8_t hops)
{
    if (hops < 0)
        return "flood";
    if (hops == 0)
        return "direct";
    char b[12];
    snprintf(b, sizeof(b), "%d hop%s", hops, hops == 1 ? "" : "s");
    return b;
}

// LoRa presets offered on the screen. Frequency, bandwidth and spreading factor must match the
// mesh; coding rate does not (the receiver reads it from each packet), so a CR 8 entry still
// talks to CR 5 radios and only makes this radio's own packets sturdier.
// Verified Nov 25 2026 against MeshCore's own suggested-settings list (api.meshcore.nz config:
// USA, USA - Southern California, EU/UK (Narrow), Australia, New Zealand (Narrow)) and each
// mesh's own page (Florida statewide, Gulf Coast: CR 8; PhillyMesh: 919.500/500/SF10/CR5 from
// Oct 2 2026; SMF, Sacramento-Foothills: sacmesh.org/guides/smf-radio-settings, 909.875/62.5/SF9/CR5).
// 918.000 is MeshCore's US off-grid frequency, the only one a companion may repeat on.
// Left out until a mesh's own page confirms it: Arizona 908.205/SF10.
struct Preset {
    const char *name;
    float freq, bw;
    uint8_t sf, cr;
};
const Preset PRESETS[] = {
    {"USA / Canada (recommended)", 910.525f, 62.5f, 7, 5},
    {"USA: Southern California", 927.875f, 62.5f, 7, 5},
    {"USA: Sacramento / Foothills (SMF)", 909.875f, 62.5f, 9, 5},
    {"USA: Florida / Gulf Coast (CR 8)", 910.525f, 62.5f, 7, 8},
    {"USA: Philadelphia (500 kHz)", 919.500f, 500.0f, 10, 5},
    {"USA off-grid 918.000 (companions repeat)", 918.000f, 62.5f, 7, 5},
    {"EU / UK narrow", 869.618f, 62.5f, 8, 8},
    {"Australia", 915.800f, 250.0f, 10, 5},
    {"New Zealand (narrow)", 917.375f, 62.5f, 7, 5},
};
constexpr int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// each country's LoRa band and power limit, from Meshtastic 2.8.3's region table (src/mesh/RadioInterface.cpp,
// which cites each country's rules). MeshCore itself has no such table: the radio warns when a setting is
// outside the band picked here; it never blocks one (the rules are the owner's to follow).
struct Band {
    const char *name;
    float lo, hi;  // MHz
    float duty;    // per cent of the time a radio may transmit
    int8_t dbm;    // power limit
};
const Band BANDS[] = {
    {"Not chosen", 0, 0, 100, 0},
    {"USA, Canada and the Americas 915", 902.0f, 928.0f, 100, 30},
    {"Brazil", 902.0f, 907.5f, 100, 30},
    {"Europe 868", 869.4f, 869.65f, 10, 27},
    {"Europe 866", 865.6f, 867.6f, 2.5f, 27},
    {"Europe 433", 433.0f, 434.0f, 10, 10},
    {"Russia", 868.7f, 869.2f, 100, 20},
    {"Ukraine 433", 433.0f, 434.7f, 10, 10},
    {"Kazakhstan 863", 863.0f, 868.0f, 100, 30},
    {"Kazakhstan 433", 433.075f, 434.775f, 100, 10},
    {"India", 865.0f, 867.0f, 100, 30},
    {"Nepal", 865.0f, 868.0f, 100, 30},
    {"China", 470.0f, 510.0f, 100, 19},
    {"Japan", 920.5f, 923.5f, 100, 13},
    {"Korea", 920.0f, 923.0f, 100, 23},
    {"Taiwan", 920.0f, 925.0f, 100, 27},
    {"Thailand", 920.0f, 925.0f, 10, 27},
    {"Malaysia 919", 919.0f, 924.0f, 100, 27},
    {"Malaysia 433", 433.0f, 435.0f, 100, 20},
    {"Singapore", 917.0f, 925.0f, 100, 20},
    {"Philippines 915", 915.0f, 918.0f, 100, 24},
    {"Philippines 868", 868.0f, 869.4f, 100, 14},
    {"Philippines 433", 433.0f, 434.7f, 100, 10},
    {"Australia and New Zealand 915", 915.0f, 928.0f, 100, 30},
    {"Australia and New Zealand 433", 433.05f, 434.79f, 100, 14},
    {"New Zealand 865", 864.0f, 868.0f, 100, 36},
    {"Ham 70 cm, ITU Region 1 (licence)", 430.0f, 440.0f, 100, 30},
    {"Ham 70 cm, ITU Region 2 (licence)", 420.0f, 450.0f, 100, 30},
    {"Ham 70 cm, ITU Region 3 (licence)", 430.0f, 450.0f, 100, 30},
    {"Ham 1.25 m, ITU Region 2 (licence)", 220.0f, 225.0f, 100, 30},
};
constexpr int N_BANDS = sizeof(BANDS) / sizeof(BANDS[0]);
const int8_t TX_LEVELS[] = {2, 5, 8, 11, 14, 17, 20, 22};
const float BWS[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
constexpr int N_BWS = sizeof(BWS) / sizeof(BWS[0]);

App *self = nullptr; // the screens are a singleton; LVGL callbacks reach it here

void textHooks(lv_obj_t *ta)
{
    lv_obj_add_event_cb(
        ta, [](lv_event_t *e) { if (self->kbNeeded()) self->kbShow(lv_event_get_target_obj(e)); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        ta, [](lv_event_t *e) { if (self->kbFor(lv_event_get_target_obj(e))) self->kbHide(); }, LV_EVENT_DELETE, nullptr);
}
} // namespace

// ================================================================= setup
App::App(Model &model) : m(model)
{
    self = this;
}

void App::begin(void)
{
    if (!styleReady) {
        lv_style_init(&stFocus);
        styleReady = true;
    }
    if (convs.empty())
        loadChats();
    applyTheme(m.uiPref("theme", 0));
    bootDone(); // the start-up screen is no longer showing
    m.setBacklight((uint8_t)m.uiPref("bright", 200));
    lastInput = lv_tick_get();
}

void App::applyTheme(int t)
{
    palette(t);
    lv_style_reset(&stFocus);
    lv_style_set_outline_width(&stFocus, 2);
    lv_style_set_outline_color(&stFocus, C_CYAN);
    lv_style_set_outline_pad(&stFocus, 1);
    lv_obj_t *old = scr;
    int keepTab = tab;
    build();
    lv_screen_load(scr);
    if (old)
        lv_obj_delete(old);
    selectTab(keepTab);
}

void App::build(void)
{
    LAND = W() > H();
    RAIL_W = LAND ? 46 : 0;
    NAV_H = LAND ? 0 : 42;
    STATUS_H = 22;
    if (!group) {
        group = lv_group_create();
        lv_group_set_wrap(group, true);
        // the keys (or a tap) moving to anything but a text field closes the on-screen keyboard
        lv_group_set_focus_cb(group, [](lv_group_t *g) {
            lv_obj_t *f = lv_group_get_focused(g);
            if (self->kb && (!f || !lv_obj_check_type(f, &lv_textarea_class)))
                self->kbHide();
        });
    }
    kbHide(); // a rebuilt screen (theme change) starts without it
    lv_group_remove_all_objs(group);
    rmFree(); // a repeater being managed: its widgets go with the old screen

    scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(scr, wallpaper(), 0); // none: the plain background
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lockObj = lockTime = lockDate = lockInfo = bandInfo = nullptr; // went with the old screen

    // status bar
    status = plain(scr);
    lv_obj_set_size(status, W(), STATUS_H);
    lv_obj_set_style_bg_color(status, C_BAR, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(status, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(status, 1, 0);
    lv_obj_set_style_border_color(status, C_LINE, 0);
    lv_obj_set_style_pad_hor(status, 6, 0);
    row(status, 6);
    lv_obj_t *dot = plain(status);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, C_CYAN, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    stName = label(status, F12, C_TEXT, "Hoptics");
    lv_obj_set_flex_grow(stName, 1);
    oneLine(stName);
    stRight = label(status, F12, C_MUTED, "");

    // navigation: rail (landscape) or tabs (portrait)
    nav = plain(scr);
    if (LAND) {
        lv_obj_set_size(nav, RAIL_W, H() - STATUS_H);
        lv_obj_set_pos(nav, 0, STATUS_H);
        column(nav, 0);
    } else {
        lv_obj_set_size(nav, W(), NAV_H);
        lv_obj_set_pos(nav, 0, H() - NAV_H);
        row(nav, 0);
    }
    lv_obj_set_style_bg_color(nav, C_BAR, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    static const char *icons[T_COUNT] = {LV_SYMBOL_HOME, LV_SYMBOL_GPS, LV_SYMBOL_SHUFFLE, LV_SYMBOL_LIST, LV_SYMBOL_ENVELOPE, LV_SYMBOL_SETTINGS};
    static const char *names[T_COUNT] = {"Home", "Map", "Live", "Nodes", "Chats", "More"};
    for (int i = 0; i < T_COUNT; i++) {
        lv_obj_t *b = lv_button_create(nav);
        lv_obj_remove_style_all(b);
        if (LAND)
            lv_obj_set_size(b, RAIL_W, (H() - STATUS_H) / T_COUNT);
        else
            lv_obj_set_size(b, W() / T_COUNT, NAV_H);
        lv_obj_set_style_bg_color(b, C_CARD, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(b, C_CYAN, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(b, LAND ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_TOP, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(b, 3, LV_STATE_CHECKED);
        // the keys' cursor on the menu (setRail): a full frame, so it reads apart from the open tab's edge
        lv_obj_set_style_bg_color(b, C_CARD2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_FULL, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(b, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(b, C_CYAN, LV_STATE_FOCUS_KEY);
        lv_obj_t *ic = label(b, F16, C_MUTED, icons[i]);
        lv_obj_set_style_text_color(ic, C_CYAN, LV_STATE_CHECKED);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, LAND ? 3 : 5);
        lv_obj_t *nm = label(b, F10, C_DIM, names[i]);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, LAND ? -2 : -3);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(
            b, [](lv_event_t *e) { self->navClicked((int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e))); },
            LV_EVENT_CLICKED, nullptr);
        navBtn[i] = b;
    }
    navBadge = label(navBtn[T_CHATS], F10, C_ON_ACCENT, "");
    lv_obj_set_style_bg_color(navBadge, C_RED, 0);
    lv_obj_set_style_bg_opa(navBadge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(navBadge, 7, 0);
    lv_obj_set_style_pad_hor(navBadge, 4, 0);
    lv_obj_align(navBadge, LV_ALIGN_TOP_MID, 10, 1);
    lv_obj_add_flag(navBadge, LV_OBJ_FLAG_HIDDEN);

    setRows.clear(); // the settings rows belong to the screens being built now
    setScope = setPin = nullptr;
    // pages
    content = plain(scr);
    lv_obj_set_size(content, W() - RAIL_W, H() - STATUS_H - NAV_H);
    lv_obj_set_pos(content, RAIL_W, STATUS_H);
    for (int i = 0; i < T_COUNT; i++)
        pages[i] = makePage();
    buildHome(pages[T_HOME]);
    buildMap(pages[T_MAP]);
    buildLive(pages[T_LIVE]);
    buildNodes(pages[T_NODES]);
    buildChats(pages[T_CHATS]);
    buildMore(pages[T_MORE]);
    buildChatOverlay();
    buildStatOverlay();
    buildNodeOverlay(); // after the stat overlay: a node opened from a stat list draws on top of it
    buildRemoteOverlay(); // and managing a repeater, opened from its node page, on top of that

    // toast
    toastBox = plain(scr);
    lv_obj_set_size(toastBox, W() - RAIL_W - 16, LV_SIZE_CONTENT);
    lv_obj_align(toastBox, LV_ALIGN_TOP_MID, RAIL_W / 2, STATUS_H + 4);
    lv_obj_set_style_bg_color(toastBox, C_CARD2, 0);
    lv_obj_set_style_bg_opa(toastBox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toastBox, C_CYAN, 0);
    lv_obj_set_style_border_width(toastBox, 1, 0);
    lv_obj_set_style_radius(toastBox, 8, 0);
    lv_obj_set_style_pad_all(toastBox, 8, 0);
    toastLbl = label(toastBox, F14, C_TEXT, "");
    wrap(toastLbl);
    lv_obj_add_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
    // a tap on a toast hides it; on a new message's toast it also opens that conversation, ready to reply
    lv_obj_add_flag(toastBox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        toastBox,
        [](lv_event_t *) {
            self->toastUntil = 0;
            lv_obj_add_flag(self->toastBox, LV_OBJ_FLAG_HIDDEN);
            if (!self->toastIsMsg)
                return;
            self->toastIsMsg = false;
            if (self->askBox && !lv_obj_has_flag(self->askBox, LV_OBJ_FLAG_HIDDEN))
                return; // the radio question is answered first
            lv_obj_add_flag(self->replyBox, LV_OBJ_FLAG_HIDDEN);
            self->selectTab(T_CHATS);
            self->openChat(self->toastConv);
        },
        LV_EVENT_CLICKED, nullptr);
    dirtyLive = dirtyContacts = dirtyChats = true;
    contactsSeen = 0xffffffff;
    statOpen = -1;
    railFocus = false;
    refreshStatus();
    refreshHome();
}

lv_obj_t *App::makePage(void)
{
    lv_obj_t *p = plain(content);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_all(p, PAD, 0);
    lv_obj_set_style_width(p, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(p, C_CYAN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(p, LV_OPA_50, LV_PART_SCROLLBAR);
    column(p, PAD);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

lv_obj_t *App::overlay(const char *title, lv_obj_t **titleOut, lv_event_cb_t back)
{
    lv_obj_t *ov = plain(scr);
    lv_obj_set_size(ov, W() - RAIL_W, H() - STATUS_H - NAV_H);
    lv_obj_set_pos(ov, RAIL_W, STATUS_H);
    lv_obj_set_style_bg_color(ov, C_BG, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    column(ov, 0);
    lv_obj_t *hdr = plain(ov);
    lv_obj_set_size(hdr, LV_PCT(100), 32);
    lv_obj_set_style_bg_color(hdr, C_BAR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, C_LINE, 0);
    lv_obj_set_style_pad_hor(hdr, 2, 0);
    row(hdr, 4);
    lv_obj_t *b = lv_button_create(hdr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 36, 30);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, C_CYAN, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_t *bl = label(b, F16, C_CYAN, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, back, LV_EVENT_CLICKED, nullptr);
    stop(b);
    lv_obj_t *t = label(hdr, F16, C_TEXT, title);
    lv_obj_set_flex_grow(t, 1);
    oneLine(t);
    if (titleOut)
        *titleOut = t;
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    return ov;
}

// ================================================================= home
void App::buildHome(lv_obj_t *p)
{
    lv_obj_t *hero = card(p, nullptr);
    stop(hero);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(hero, C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(hero, [](lv_event_t *) { self->openStat(S_RADIO); }, LV_EVENT_CLICKED, nullptr);
    hName = label(hero, F20, C_TEXT, "");
    oneLine(hName);
    lv_obj_set_width(hName, LV_PCT(100));
    hSub = label(hero, F12, C_MUTED, "");
    wrap(hSub);

    lv_obj_t *grid = plain(p);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, PAD, 0);
    lv_obj_set_style_pad_column(grid, PAD, 0);
    static const char *caps[4] = {"HEARD 1H", "REPEATERS", "PACKETS / MIN", "NOISE FLOOR"};
    lv_color_t cols[4] = {C_CYAN, C_GREEN, C_VIOLET, C_AMBER};
    int32_t tw = (W() - RAIL_W - 2 * PAD - PAD) / 2;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = plain(grid);
        lv_obj_set_size(t, tw, 52);
        lv_obj_set_style_bg_color(t, C_CARD, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(t, T_GLASS ? 12 : 10, 0);
        lv_obj_set_style_border_side(t, T_BORDERS ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(t, T_GLASS ? 1 : T_BORDERS ? 2 : 3, 0);
        lv_obj_set_style_border_color(t, T_GLASS ? edgeColor() : cols[i], 0);
        lv_obj_t *c = label(t, F10, C_MUTED, caps[i]);
        lv_obj_set_pos(c, 8, 4);
        hTile[i] = label(t, F20, cols[i], "-");
        lv_obj_set_pos(hTile[i], 8, 20);
        hTileSub[i] = label(t, F10, C_MUTED, "");
        lv_obj_align(hTileSub[i], LV_ALIGN_BOTTOM_RIGHT, -6, -5);
        lv_obj_t *more = label(t, F10, C_DIM, LV_SYMBOL_RIGHT); // opens: details
        lv_obj_align(more, LV_ALIGN_TOP_RIGHT, -6, 5);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(t, C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_user_data(t, (void *)(intptr_t)i); // S_HEARD, S_REPEATERS, S_PACKETS, S_NOISE in tile order
        lv_obj_add_event_cb(
            t, [](lv_event_t *e) { self->openStat((int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e))); },
            LV_EVENT_CLICKED, nullptr);
        stop(t);
    }

    lv_obj_t *ca = card(p, "ACTIVITY  -  packets / min, last 30 min");
    hChart = lv_chart_create(ca);
    lv_obj_set_size(hChart, LV_PCT(100), 50);
    lv_chart_set_type(hChart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(hChart, 30);
    lv_chart_set_div_line_count(hChart, 2, 0);
    lv_obj_set_style_bg_opa(hChart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hChart, 0, 0);
    lv_obj_set_style_pad_all(hChart, 0, 0);
    lv_obj_set_style_line_color(hChart, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_pad_column(hChart, 1, LV_PART_MAIN);
    hSer = lv_chart_add_series(hChart, C_VIOLET, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_remove_flag(hChart, LV_OBJ_FLAG_CLICKABLE); // the card takes the tap
    lv_obj_add_flag(ca, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ca, C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ca, [](lv_event_t *) { self->openStat(S_PACKETS); }, LV_EVENT_CLICKED, nullptr);
    stop(ca);
    lv_obj_t *ac = plain(p);
    lv_obj_set_size(ac, LV_PCT(100), 34);
    row(ac, PAD);
    lv_obj_t *b1 = button(ac, LV_SYMBOL_GPS " Advert", C_CYAN, (W() - RAIL_W - 3 * PAD) / 2, 32);
    lv_obj_add_event_cb(
        b1, [](lv_event_t *) { self->toast(self->m.sendAdvert(true) ? LV_SYMBOL_GPS " Advert sent to the mesh" : "Couldn't send the advert"); },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *b2 = button(ac, LV_SYMBOL_WIFI " Zero-hop", C_GREEN, (W() - RAIL_W - 3 * PAD) / 2, 32);
    lv_obj_add_event_cb(
        b2, [](lv_event_t *) { self->toast(self->m.sendAdvert(false) ? LV_SYMBOL_WIFI " Zero-hop advert sent (neighbours only)" : "Couldn't send the advert"); },
        LV_EVENT_CLICKED, nullptr);
}

void App::refreshHome(void)
{
    char b[96];
    setText(hName, m.nodeName());
    Radio r = m.radio();
    static const char *roles[] = {"Companion", "Repeater", "Room server", "Observer", "Repeater + observer", "Field station"};
    snprintf(b, sizeof(b), "%s  -  %.3f MHz  SF%u  BW%g  CR%u  %d dBm", roles[(int)m.role()], r.freq, r.sf, r.bw, r.cr, r.txPower);
    setText(hSub, b);
    uint32_t now = m.now();
    size_t heard = 0, reps = 0;
    Contact c;
    for (size_t i = 0; i < m.contactCount(); i++) {
        if (!m.contactAt(i, c))
            continue;
        if (c.type == NodeType::Repeater)
            reps++;
        if (now && c.lastHeard && now - c.lastHeard <= 3600)
            heard++;
    }
    setTextFmt(hTile[0], "%u", (unsigned)heard);
    setTextFmt(hTileSub[0], "of %u", (unsigned)m.contactCount());
    setTextFmt(hTile[1], "%u", (unsigned)reps);
    setText(hTileSub[1], "known");
    setTextFmt(hTile[2], "%u", (unsigned)perMin[29]);
    Stats s = m.stats();
    setTextFmt(hTileSub[2], "%u rx", (unsigned)s.rx);
    if (s.noiseFloor)
        setTextFmt(hTile[3], "%d", (int)s.noiseFloor);
    else
        setText(hTile[3], "-");
    setText(hTileSub[3], "dBm");
    const int32_t *ys = lv_chart_get_series_y_array(hChart, hSer);
    bool same = ys != nullptr;
    for (int i = 0; same && i < 30; i++)
        same = ys[i] == (int32_t)perMin[i];
    if (same)
        return; // the chart already shows these numbers: no redraw
    uint16_t top = 4;
    for (int i = 0; i < 30; i++)
        top = std::max<uint16_t>(top, perMin[i]);
    lv_chart_set_axis_range(hChart, LV_CHART_AXIS_PRIMARY_Y, 0, top);
    for (int i = 0; i < 30; i++)
        lv_chart_set_value_by_id(hChart, hSer, i, perMin[i]);
    lv_chart_refresh(hChart);
}

// ================================================================= status bar
void App::refreshStatus(void)
{
    setText(stName, m.nodeName());
    char b[64];
    size_t n = 0;
    b[0] = 0;
    if (m.repeatEnabled())
        n += snprintf(b + n, sizeof(b) - n, LV_SYMBOL_LOOP " ");
    if (m.bluetoothEnabled())
        n += snprintf(b + n, sizeof(b) - n, "%s ", m.appConnected() ? LV_SYMBOL_BLUETOOTH "+" : LV_SYMBOL_BLUETOOTH);
    int bp = m.batteryPercent();
    if (bp == 101)
        n += snprintf(b + n, sizeof(b) - n, LV_SYMBOL_USB " ");
    else if (bp >= 0)
        n += snprintf(b + n, sizeof(b) - n, "%d%% ", bp);
    uint32_t now = m.now();
    if (now > 1600000000) {
        struct tm tmv;
        localTime(now, tmv);
        snprintf(b + n, sizeof(b) - n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
    setText(stRight, b);
    // unread badge on Chats
    unsigned unread = 0;
    for (auto &c : convs)
        unread += c.unread;
    if (unread) {
        setTextFmt(navBadge, "%u", unread);
        lv_obj_remove_flag(navBadge, LV_OBJ_FLAG_HIDDEN);
    } else
        lv_obj_add_flag(navBadge, LV_OBJ_FLAG_HIDDEN);
}

// ================================================================= live
void App::buildLive(lv_obj_t *p)
{
    lv_obj_set_style_pad_row(p, 4, 0);
    liveEmpty = label(p, F14, C_MUTED, "Listening...\nEvery packet the radio hears shows up here.");
    wrap(liveEmpty);
    lv_obj_set_style_text_align(liveEmpty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(liveEmpty, 50, 0);
    liveList = p;
}

void App::onPacket(const Packet &pk)
{
    if (pk.viaNet) { // the live feed: on the map only (the Live tab is what this radio hears)
        mapPacket(pk);
        if (tab == T_MAP)
            refreshMapStrip();
        return;
    }
    live[liveHead] = pk;
    live[liveHead].seq = ++pktSeq;
    liveHead = (liveHead + 1) % kLive;
    if (liveCount < kLive)
        liveCount++;
    if (!pk.mine)
        perMin[29]++;
    dirtyLive = true;
    mapPacket(pk);
    if (tab == T_MAP)
        refreshMapStrip();
}

void App::refreshLive(void)
{
    dirtyLive = false;
    // rows are rebuilt from the ring (cheap: at most kLive small rows, only while the tab is open)
    lv_obj_t *keepFocus = lv_group_get_focused(group);
    bool hadFocus = keepFocus && lv_obj_get_parent(keepFocus) == liveList;
    while (lv_obj_get_child_count(liveList) > 1)
        lv_obj_delete(lv_obj_get_child(liveList, 1));
    if (!liveCount) {
        lv_obj_remove_flag(liveEmpty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(liveEmpty, LV_OBJ_FLAG_HIDDEN);
    uint32_t now = m.now();
    char b[64];
    for (int i = 0; i < liveCount; i++) {
        const Packet &pk = live[(liveHead + kLive - 1 - i) % kLive];
        lv_obj_t *r = plain(liveList);
        lv_obj_set_size(r, LV_PCT(100), 38);
        lv_obj_set_style_bg_color(r, C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 8, 0);
        rowEdge(r);
        if (pk.mine) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(r, 3, 0);
            lv_obj_set_style_border_color(r, C_CYAN, 0);
        }
        lv_obj_t *t = pill(r, pktTypeName(pk.type), pktColor(pk.type));
        lv_obj_set_pos(t, 6, 4);
        lv_obj_t *who = label(r, F12, C_TEXT, pk.from[0] ? pk.from : (pk.mine ? "this radio" : ""));
        lv_obj_set_pos(who, 58, 3);
        lv_obj_set_width(who, W() - RAIL_W - 2 * PAD - 58 - 70);
        oneLine(who);
        // route: F/D, hops, and the path hashes it came through
        size_t n = snprintf(b, sizeof(b), "%s  %u hop%s", pk.direct ? "direct" : "flood", pk.pathLen, pk.pathLen == 1 ? "" : "s");
        int shown = std::min<int>(pk.pathLen, (int)sizeof(pk.path) / std::max<int>(1, pk.hashSize));
        for (int h = 0; h < shown && n < sizeof(b) - 8; h++) {
            n += snprintf(b + n, sizeof(b) - n, h ? ">" : "  ");
            for (int k = 0; k < pk.hashSize && n < sizeof(b) - 3; k++)
                n += snprintf(b + n, sizeof(b) - n, "%02x", pk.path[h * pk.hashSize + k]);
        }
        lv_obj_t *rt = label(r, F10, C_MUTED, b);
        lv_obj_set_pos(rt, 6, 21);
        lv_obj_set_width(rt, W() - RAIL_W - 2 * PAD - 6 - 70);
        oneLine(rt);
        if (pk.mine)
            snprintf(b, sizeof(b), "sent\n%s", ago(now, pk.time).c_str());
        else
            snprintf(b, sizeof(b), "%d dBm\n%+.1f dB", pk.rssi, pk.snr4 / 4.0f);
        lv_obj_t *sg = label(r, F10, pk.mine ? C_MUTED : (pk.snr4 >= 20 ? C_GREEN : pk.snr4 >= -20 ? C_AMBER : C_RED), b);
        lv_obj_set_style_text_align(sg, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(sg, LV_ALIGN_RIGHT_MID, -6, 0);
        stop(r); // keys scroll through the list
    }
    if (hadFocus && tab == T_LIVE)
        refocus();
}

// ================================================================= nodes (contacts)
void App::buildNodes(lv_obj_t *p)
{
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_t *chips = plain(p);
    lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(chips, 4, 0);
    lv_obj_set_style_pad_row(chips, 4, 0);
    static const char *names[5] = {"All", "Chat", "Repeaters", "Rooms", "Sensors"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_button_create(chips);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 24);
        lv_obj_set_style_pad_hor(b, 9, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, C_CARD2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, C_CYAN, LV_STATE_CHECKED);
        lv_obj_t *l = label(b, F12, C_TEXT, names[i]);
        lv_obj_set_style_text_color(l, C_ON_ACCENT, LV_STATE_CHECKED);
        lv_obj_center(l);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                self->nodeFilter = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                self->dirtyContacts = true;
            },
            LV_EVENT_CLICKED, nullptr);
        stop(b);
        nodeChips[i] = b;
    }
    nodeList = plain(p);
    lv_obj_set_size(nodeList, LV_PCT(100), LV_SIZE_CONTENT);
    column(nodeList, 4);
}

namespace
{
std::vector<std::array<uint8_t, 6>> rowKeys; // node rows -> contact key
}

void App::refreshNodes(void)
{
    dirtyContacts = false;
    contactsSeen = m.contactsVersion();
    for (int i = 0; i < 5; i++) {
        if (i == nodeFilter)
            lv_obj_add_state(nodeChips[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(nodeChips[i], LV_STATE_CHECKED);
        lv_obj_t *l = lv_obj_get_child(nodeChips[i], 0);
        lv_obj_set_style_text_color(l, i == nodeFilter ? C_ON_ACCENT : C_TEXT, 0);
    }
    lv_obj_clean(nodeList);
    rowKeys.clear();
    std::vector<Contact> list;
    list.reserve(m.contactCount());
    Contact c;
    static const NodeType want[5] = {NodeType::Unknown, NodeType::Chat, NodeType::Repeater, NodeType::Room, NodeType::Sensor};
    for (size_t i = 0; i < m.contactCount(); i++)
        if (m.contactAt(i, c) && (nodeFilter == 0 || c.type == want[nodeFilter]))
            list.push_back(c);
    std::sort(list.begin(), list.end(), [](const Contact &a, const Contact &b) {
        if (a.favourite != b.favourite)
            return a.favourite;
        return std::max(a.lastHeard, a.lastAdvert) > std::max(b.lastHeard, b.lastAdvert);
    });
    if (list.size() > 120)
        list.resize(120); // the newest 120: enough to scroll through, and kind to memory
    if (list.empty()) {
        lv_obj_t *l = label(nodeList, F14, C_MUTED, "No nodes here yet.\nThey appear as their adverts are heard. Tap Home > Advert to announce yourself.");
        wrap(l);
        lv_obj_set_style_pad_top(l, 30, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    uint32_t now = m.now();
    for (auto &n : list) {
        lv_obj_t *r = plain(nodeList);
        lv_obj_set_size(r, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(r, C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(r, 8, 0);
        rowEdge(r);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *bd = badge(r, n.key, n.name, 28);
        lv_obj_align(bd, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_t *nm = label(r, F14, C_TEXT, n.name[0] ? n.name : "(no name)");
        lv_obj_set_pos(nm, 42, 3);
        lv_obj_set_width(nm, W() - RAIL_W - 2 * PAD - 42 - 46);
        oneLine(nm);
        lv_obj_t *tp = pill(r, nodeTypeName(n.type), typeColor(n.type));
        lv_obj_set_pos(tp, 42, 22);
        lv_obj_t *rt = label(r, F10, C_MUTED, route(n.pathLen).c_str());
        lv_obj_align_to(rt, tp, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        lv_obj_t *ag = label(r, F12, C_MUTED, ago(now, n.lastHeard ? n.lastHeard : n.lastAdvert).c_str());
        lv_obj_align(ag, LV_ALIGN_RIGHT_MID, -8, 0);
        rowKeys.push_back({});
        memcpy(rowKeys.back().data(), n.key, 6);
        lv_obj_set_user_data(r, (void *)(intptr_t)(rowKeys.size() - 1));
        lv_obj_add_event_cb(
            r,
            [](lv_event_t *e) {
                size_t i = (size_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                if (i < rowKeys.size())
                    self->openNode(rowKeys[i].data());
            },
            LV_EVENT_CLICKED, nullptr);
        stop(r);
    }
    if (tab == T_NODES && !topOverlay())
        refocus();
}

// node detail
void App::buildNodeOverlay(void)
{
    nodeOv = overlay("Node", nullptr, [](lv_event_t *) { self->closeNode(); });
    nodeOvBody = plain(nodeOv);
    lv_obj_set_width(nodeOvBody, LV_PCT(100));
    lv_obj_set_flex_grow(nodeOvBody, 1);
    lv_obj_add_flag(nodeOvBody, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nodeOvBody, LV_DIR_VER);
    lv_obj_set_style_pad_all(nodeOvBody, PAD, 0);
    column(nodeOvBody, PAD);
}

void App::openNode(const uint8_t key[6])
{
    Contact c;
    if (!m.findContact(key, c))
        return;
    memcpy(nodeOvKey, key, 6);
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(nodeOv, 0), 1), c.name[0] ? c.name : "Node");
    lv_obj_clean(nodeOvBody);
    lv_obj_t *hero = card(nodeOvBody, nullptr);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 10, 0);
    badge(hero, c.key, c.name, 40);
    lv_obj_t *col = plain(hero);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    column(col, 2);
    lv_obj_t *nm = label(col, F16, C_TEXT, c.name);
    oneLine(nm);
    pill(col, nodeTypeName(c.type), typeColor(c.type));
    stop(hero);

    lv_obj_t *info = card(nodeOvBody, "DETAILS");
    char b[160];
    uint32_t now = m.now();
    snprintf(b, sizeof(b), "Route: %s\nLast heard: %s ago\nLast advert: %s ago\nKey: %02x%02x%02x%02x%02x%02x...", route(c.pathLen).c_str(),
             ago(now, c.lastHeard).c_str(), ago(now, c.lastAdvert).c_str(), c.key[0], c.key[1], c.key[2], c.key[3], c.key[4], c.key[5]);
    lv_obj_t *il = label(info, F12, C_TEXT, b);
    wrap(il);
    if (c.hasPos) {
        int32_t la, lo;
        if (m.gpsFix(la, lo)) {
            // distance and bearing from us (spherical, good to a few metres at mesh ranges)
            double p1 = la / 1e6 * M_PI / 180, p2 = c.lat / 1e6 * M_PI / 180, dl = (c.lon - lo) / 1e6 * M_PI / 180;
            double d = 6371.0 * acos(std::min(1.0, sin(p1) * sin(p2) + cos(p1) * cos(p2) * cos(dl)));
            double brg = fmod(atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * 180 / M_PI + 360, 360);
            static const char *dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
            snprintf(b, sizeof(b), "Position: %.4f, %.4f\n%.1f km %s of you", c.lat / 1e6, c.lon / 1e6, d, dirs[(int)((brg + 22.5) / 45) % 8]);
        } else
            snprintf(b, sizeof(b), "Position: %.4f, %.4f", c.lat / 1e6, c.lon / 1e6);
        lv_obj_t *pl = label(info, F12, C_TEXT, b);
        wrap(pl);
    }
    stop(info);

    lv_obj_t *acts = plain(nodeOvBody);
    lv_obj_set_size(acts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(acts, PAD, 0);
    lv_obj_set_style_pad_row(acts, PAD, 0);
    int32_t bw = (W() - RAIL_W - 3 * PAD) / 2;
    if (c.type == NodeType::Chat || c.type == NodeType::Room) {
        lv_obj_t *dm = button(acts, LV_SYMBOL_ENVELOPE " Message", C_CYAN, bw, 32);
        lv_obj_add_event_cb(
            dm,
            [](lv_event_t *) {
                ConvKey k;
                k.dm = true;
                memcpy(k.key, self->nodeOvKey, 6);
                self->closeNode();
                self->openChat(k);
            },
            LV_EVENT_CLICKED, nullptr);
    }
    if (c.type == NodeType::Repeater || c.type == NodeType::Room) {
        lv_obj_t *mg = button(acts, LV_SYMBOL_SETTINGS " Manage", C_GREEN, bw, 32);
        lv_obj_add_event_cb(mg, [](lv_event_t *) { self->openRemote(self->nodeOvKey); }, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *rp = button(acts, LV_SYMBOL_REFRESH " Reset path", C_AMBER, bw, 32);
    lv_obj_add_event_cb(
        rp, [](lv_event_t *) { self->toast(self->m.resetPath(self->nodeOvKey) ? "Path reset: the next message floods to find a new route" : "Couldn't reset the path"); },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_remove_flag(nodeOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(nodeOv);
    lv_obj_move_foreground(toastBox);
    lv_obj_scroll_to_y(nodeOvBody, 0, LV_ANIM_OFF);
    refocus(hero);
}

void App::closeNode(void)
{
    lv_obj_add_flag(nodeOv, LV_OBJ_FLAG_HIDDEN);
    refocus();
}

// ================================================================= chats
App::Conv &App::conv(const ConvKey &k)
{
    for (auto &c : convs)
        if (c.key == k)
            return c;
    convs.push_back(Conv{});
    Conv &c = convs.back();
    c.key = k;
    c.title = convTitle(k);
    if (convs.size() > 24) { // drop the quietest
        auto it = std::min_element(convs.begin(), convs.end() - 1, [](const Conv &a, const Conv &b) { return a.last < b.last; });
        convs.erase(it);
        return convs.back();
    }
    return c;
}

std::string App::convTitle(const ConvKey &k) const
{
    if (k.dm) {
        Contact c;
        if (m.findContact(k.key, c) && c.name[0])
            return c.name;
        char b[16];
        snprintf(b, sizeof(b), "%02x%02x%02x", k.key[0], k.key[1], k.key[2]);
        return b;
    }
    Channel ch;
    for (size_t i = 0; i < m.channelCount(); i++)
        if (m.channelAt(i, ch) && ch.index == k.channel)
            return std::string("#") + (ch.name[0] ? ch.name : "channel");
    return "#channel";
}

void App::buildChats(lv_obj_t *p)
{
    lv_obj_set_style_pad_row(p, 4, 0);
    chatList = p;
}

namespace
{
std::vector<ConvKey> rowConvs;
}

void App::refreshChats(void)
{
    dirtyChats = false;
    lv_obj_clean(chatList);
    rowConvs.clear();
    // every channel, then direct chats that have messages (newest first)
    std::vector<ConvKey> keys;
    Channel ch;
    for (size_t i = 0; i < m.channelCount(); i++)
        if (m.channelAt(i, ch) && ch.name[0]) {
            ConvKey k;
            k.channel = ch.index;
            keys.push_back(k);
        }
    std::vector<const Conv *> dms;
    for (auto &c : convs)
        if (c.key.dm)
            dms.push_back(&c);
    std::sort(dms.begin(), dms.end(), [](const Conv *a, const Conv *b) { return a->last > b->last; });
    for (auto *c : dms)
        keys.push_back(c->key);
    if (keys.empty()) {
        lv_obj_t *l = label(chatList, F14, C_MUTED, "No channels yet. Add them in the MeshCore app, or start a direct chat from Nodes.");
        wrap(l);
        lv_obj_set_style_pad_top(l, 30, 0);
        return;
    }
    uint32_t now = m.now();
    for (auto &k : keys) {
        const Conv *cv = nullptr;
        for (auto &c : convs)
            if (c.key == k)
                cv = &c;
        lv_obj_t *r = plain(chatList);
        lv_obj_set_size(r, LV_PCT(100), 42);
        lv_obj_set_style_bg_color(r, C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(r, 8, 0);
        rowEdge(r);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        std::string title = cv ? cv->title : convTitle(k);
        if (k.dm)
            lv_obj_align(badge(r, k.key, title.c_str(), 28), LV_ALIGN_LEFT_MID, 6, 0);
        else {
            lv_obj_t *hs = label(r, F20, C_CYAN, "#");
            lv_obj_align(hs, LV_ALIGN_LEFT_MID, 13, 0);
        }
        lv_obj_t *t = label(r, F14, C_TEXT, k.dm ? title.c_str() : title.c_str() + 1);
        lv_obj_set_pos(t, 42, 3);
        lv_obj_set_width(t, W() - RAIL_W - 2 * PAD - 42 - 50);
        oneLine(t);
        std::string last = "no messages yet";
        if (cv && !cv->lines.empty()) {
            const Line &l = cv->lines.back();
            last = (l.mine ? std::string("you: ") : (k.dm ? "" : l.from + ": ")) + l.text;
        }
        lv_obj_t *s = label(r, F12, C_MUTED, last.c_str());
        lv_obj_set_pos(s, 42, 22);
        lv_obj_set_width(s, W() - RAIL_W - 2 * PAD - 42 - 50);
        oneLine(s);
        if (cv && cv->unread) {
            lv_obj_t *u = label(r, F10, C_ON_ACCENT, std::to_string(cv->unread).c_str());
            lv_obj_set_style_bg_color(u, C_CYAN, 0);
            lv_obj_set_style_bg_opa(u, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(u, 8, 0);
            lv_obj_set_style_pad_hor(u, 5, 0);
            lv_obj_align(u, LV_ALIGN_RIGHT_MID, -8, 0);
        } else if (cv && cv->last) {
            lv_obj_t *a = label(r, F12, C_MUTED, ago(now, cv->last).c_str());
            lv_obj_align(a, LV_ALIGN_RIGHT_MID, -8, 0);
        }
        rowConvs.push_back(k);
        lv_obj_set_user_data(r, (void *)(intptr_t)(rowConvs.size() - 1));
        lv_obj_add_event_cb(
            r,
            [](lv_event_t *e) {
                size_t i = (size_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                if (i < rowConvs.size())
                    self->openChat(rowConvs[i]);
            },
            LV_EVENT_CLICKED, nullptr);
        stop(r);
    }
    if (tab == T_CHATS && !topOverlay())
        refocus();
}

void App::buildChatOverlay(void)
{
    chatOv = overlay("Chat", &chatTitle, [](lv_event_t *) { self->closeChat(); });
    chatMsgs = plain(chatOv);
    lv_obj_set_width(chatMsgs, LV_PCT(100));
    lv_obj_set_flex_grow(chatMsgs, 1);
    lv_obj_add_flag(chatMsgs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chatMsgs, LV_DIR_VER);
    lv_obj_set_style_pad_all(chatMsgs, PAD, 0);
    lv_obj_set_style_width(chatMsgs, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(chatMsgs, C_CYAN, LV_PART_SCROLLBAR);
    column(chatMsgs, 6);
    lv_obj_t *in = plain(chatOv);
    lv_obj_set_size(in, LV_PCT(100), 40);
    lv_obj_set_style_pad_all(in, 4, 0);
    lv_obj_set_style_bg_color(in, C_BAR, 0);
    lv_obj_set_style_bg_opa(in, LV_OPA_COVER, 0);
    row(in, 4);
    chatInput = lv_textarea_create(in);
    lv_textarea_set_one_line(chatInput, true);
    lv_textarea_set_max_length(chatInput, 140); // MeshCore's text limit
    lv_textarea_set_placeholder_text(chatInput, "Message");
    lv_obj_set_height(chatInput, 32);
    lv_obj_set_flex_grow(chatInput, 1);
    styleInput(chatInput);
    lv_obj_set_style_pad_ver(chatInput, 5, 0);
    stop(chatInput);
    lv_obj_add_event_cb(chatInput, [](lv_event_t *) { self->sendFromInput(); }, LV_EVENT_READY, nullptr); // on-screen OK
    lv_obj_t *send = button(in, LV_SYMBOL_UPLOAD, C_CYAN, 40, 32);
    lv_obj_add_event_cb(send, [](lv_event_t *) { self->sendFromInput(); }, LV_EVENT_CLICKED, nullptr);
    buildReplyBox();
}

// "Reply to NAME": opened by a tap (or OK) on someone's message in a channel
void App::buildReplyBox(void)
{
    replyBox = plain(scr);
    lv_obj_set_size(replyBox, W(), H());
    lv_obj_set_style_bg_color(replyBox, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(replyBox, LV_OPA_60, 0);
    lv_obj_add_flag(replyBox, LV_OBJ_FLAG_CLICKABLE); // a tap outside the card closes it
    lv_obj_add_flag(replyBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(
        replyBox, [](lv_event_t *e) { if (lv_event_get_target(e) == self->replyBox) self->closeReply(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cd = card(replyBox, nullptr);
    lv_obj_set_width(cd, std::min<int32_t>(W() - 24, 280));
    lv_obj_center(cd);
    replyTitle = label(cd, F14, C_TEXT, "");
    oneLine(replyTitle);
    lv_obj_set_width(replyTitle, LV_PCT(100));
    lv_obj_t *ch = button(cd, "# Reply on the channel", C_CYAN, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        ch,
        [](lv_event_t *) {
            // MeshCore's mention, as the phone apps write it: @[Name]
            std::string t = "@[" + self->replyName + "] " + lv_textarea_get_text(self->chatInput);
            self->closeReply();
            lv_textarea_set_text(self->chatInput, t.c_str());
            self->refocus(self->chatInput);
        },
        LV_EVENT_CLICKED, nullptr);
    replyDm = button(cd, LV_SYMBOL_ENVELOPE " Message privately", C_GREEN, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        replyDm,
        [](lv_event_t *) {
            ConvKey k;
            k.dm = true;
            memcpy(k.key, self->replyKey, 6);
            self->closeReply();
            self->openChat(k);
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cx = button(cd, "Cancel", C_MUTED, LV_PCT(100), 30);
    lv_obj_add_event_cb(cx, [](lv_event_t *) { self->closeReply(); }, LV_EVENT_CLICKED, nullptr);
}

bool App::chatContactByName(const std::string &name, uint8_t key[6]) const
{
    Contact c;
    for (size_t i = 0; i < m.contactCount(); i++)
        if (m.contactAt(i, c) && c.type == NodeType::Chat && name == c.name) {
            memcpy(key, c.key, 6);
            return true;
        }
    return false;
}

void App::askReply(uint32_t seq)
{
    const Conv &c = conv(chatKey);
    const Line *l = nullptr;
    for (auto &x : c.lines)
        if (x.seq == seq)
            l = &x;
    if (!l || l->mine || l->from.empty())
        return;
    replyName = l->from;
    lv_label_set_text_fmt(replyTitle, "Reply to %s", replyName.c_str());
    // privately only when we know them (their key is in our contacts)
    if (chatContactByName(replyName, replyKey))
        lv_obj_remove_flag(replyDm, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(replyDm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(replyBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(replyBox);
    refocus();
}

void App::closeReply(void)
{
    lv_obj_add_flag(replyBox, LV_OBJ_FLAG_HIDDEN);
    refocus(chatInput);
}

void App::appendBubble(const Line &l)
{
    lv_obj_t *line = plain(chatMsgs);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, l.mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *b = plain(line);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    int32_t maxW = (W() - RAIL_W) * 4 / 5;
    lv_obj_set_style_max_width(b, maxW, 0);
    lv_obj_set_style_bg_color(b, l.mine ? C_ME : C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_pad_hor(b, 9, 0);
    lv_obj_set_style_pad_ver(b, 5, 0);
    column(b, 1);
    const bool big = m.uiPref("chattext", 0) == 1; // Large chat text
    if (!l.mine && !chatKey.dm) {
        // someone's message on a channel: tap it (or OK on it) to reply to them
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(b, C_CARD, LV_STATE_PRESSED);
        stop(b);
        lv_obj_set_user_data(b, (void *)(uintptr_t)l.seq);
        lv_obj_add_event_cb(
            b, [](lv_event_t *e) { self->askReply((uint32_t)(uintptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e))); },
            LV_EVENT_CLICKED, nullptr);
        lv_obj_t *w = label(b, big ? F14 : F12, C_CYAN, l.from.c_str());
        oneLine(w);
        lv_obj_set_style_max_width(w, maxW - 18, 0);
    }
    lv_obj_t *t = label(b, big ? F16 : F14, l.mine ? C_ON_ME : C_TEXT, l.text.c_str());
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(t, maxW - 18, 0);
    char meta[48];
    std::string when = "now";
    uint32_t now = m.now();
    if (l.time && now > 1600000000 && l.time > 1600000000) {
        struct tm tmv;
        localTime(l.time, tmv);
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        when = hm;
    } else if (l.time)
        when = ago(now, l.time);
    if (l.mine && l.state == 4 && l.hops > 0)
        snprintf(meta, sizeof(meta), "%s  " LV_SYMBOL_OK " heard by %d observer%s", when.c_str(), l.hops, l.hops == 1 ? "" : "s");
    else if (l.mine)
        snprintf(meta, sizeof(meta), "%s  %s", when.c_str(),
                 l.state == 2   ? LV_SYMBOL_OK " delivered"
                 : l.state == 3 ? LV_SYMBOL_CLOSE " no reply"
                 : l.state == 4 ? LV_SYMBOL_OK " sent"
                 : l.state == 1 ? "sending..."
                                : "");
    else if (l.hops == -2)
        snprintf(meta, sizeof(meta), "%s  " LV_SYMBOL_WIFI " via internet", when.c_str());
    else if (l.hops >= 0)
        snprintf(meta, sizeof(meta), "%s  %s", when.c_str(), route(l.hops).c_str());
    else
        snprintf(meta, sizeof(meta), "%s", when.c_str());
    label(b, big ? F12 : F10, l.mine ? C_ME_META : (l.state == 3 ? C_RED : C_DIM), meta);
}

void App::renderChat(void)
{
    lv_obj_clean(chatMsgs);
    Conv &c = conv(chatKey);
    if (c.lines.empty()) {
        lv_obj_t *l = label(chatMsgs, F12, C_DIM, chatKey.dm ? "Say hello!" : "No messages on this channel yet.");
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 30, 0);
    }
    for (auto &l : c.lines)
        appendBubble(l);
    lv_obj_update_layout(chatMsgs);
    lv_obj_scroll_to_y(chatMsgs, LV_COORD_MAX, LV_ANIM_OFF);
    if (chatOpen && topOverlay() == chatOv)
        refocus(chatInput); // the new bubbles join the keys' stops
}

void App::openChat(const ConvKey &k)
{
    if (!(k == chatKey))
        lv_textarea_set_text(chatInput, ""); // a half-typed message stays with its own chat
    chatKey = k;
    Conv &c = conv(k);
    c.title = convTitle(k);
    if (c.unread) {
        c.unread = 0;
        chatsChanged();
    }
    lv_label_set_text(chatTitle, c.title.c_str());
    renderChat();
    chatOpen = true;
    lv_obj_remove_flag(chatOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(chatOv);
    lv_obj_move_foreground(toastBox);
    dirtyChats = true;
    refreshStatus();
    refocus(chatInput); // a keyboard types straight into the message
}

void App::closeChat(void)
{
    chatOpen = false;
    lv_obj_add_flag(chatOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(chatMsgs);
    dirtyChats = true;
    refocus();
}

void App::sendFromInput(void)
{
    const char *t = lv_textarea_get_text(chatInput);
    if (!t || !t[0])
        return;
    const uint32_t id = sendLine(chatKey, t);
    lv_textarea_set_text(chatInput, "");
    renderChat();
    if (!id)
        toast("Couldn't send: no route and the radio is busy, try again");
}

// a message from the web page (channels only, as the page sees only channels): JSON for the page
std::string App::webSend(uint8_t channel, const char *text)
{
    ConvKey k;
    k.channel = channel;
    Channel ch;
    bool known = false;
    for (size_t i = 0; i < m.channelCount() && !known; i++)
        known = m.channelAt(i, ch) && ch.index == channel;
    if (!known)
        return "{\"error\":\"No such channel on this node\"}";
    const uint32_t id = sendLine(k, text);
    if (chatOpen && chatKey == k)
        renderChat(); // open on the screen too
    dirtyChats = true;
    return id ? "{\"ok\":true}" : "{\"error\":\"The radio is busy, try again\"}";
}

uint32_t App::sendLine(const ConvKey &key, const char *t)
{
    std::string text(t);
    uint32_t id = m.sendText(key, text.c_str());
    Conv &c = conv(key);
    Line l;
    l.text = text;
    l.mine = true;
    l.time = m.now();
    l.id = id;
    l.state = !id ? 3 : (key.dm ? 1 : 4); // channels have no ACK: "sent" is as far as it goes
    l.seq = ++lineSeq;
    c.lines.push_back(l);
    if (c.lines.size() > 60)
        c.lines.erase(c.lines.begin());
    c.last = l.time;
    chatsChanged();
    return id;
}

void App::onMessage(const Message &msg)
{
    Conv &c = conv(msg.conv);
    c.title = convTitle(msg.conv);
    Line l;
    l.from = msg.from;
    l.text = msg.text ? msg.text : "";
    l.time = msg.time ? msg.time : m.now();
    l.hops = msg.viaNet ? -2 : msg.pathLen;
    l.seq = ++lineSeq;
    c.lines.push_back(l);
    if (c.lines.size() > 60)
        c.lines.erase(c.lines.begin());
    c.last = m.now() ? m.now() : l.time;
    chatsChanged();
    bool visible = chatOpen && chatKey == msg.conv && awake;
    // a sound for messages heard on the radio (not the internet feed's copies), as set under More > Sound
    const int tone = m.uiPref("snd", 1);
    if (tone > 0 && !msg.viaNet && (msg.conv.dm || m.uiPref("snd_all", 1)))
        m.playSound(tone, m.uiPref("vol", 60));
    if (visible) {
        appendBubble(l);
        lv_obj_update_layout(chatMsgs);
        lv_obj_scroll_to_y(chatMsgs, LV_COORD_MAX, LV_ANIM_ON);
        if (topOverlay() == chatOv)
            refocus(lv_group_get_focused(group)); // the new bubble joins the keys' stops, the focus stays put
    } else {
        c.unread++;
        char b[120];
        snprintf(b, sizeof(b), "%s %s%s%s: %s", msg.viaNet ? LV_SYMBOL_WIFI : LV_SYMBOL_ENVELOPE, msg.conv.dm ? "" : c.title.c_str(),
                 msg.conv.dm ? "" : " ", msg.from, l.text.c_str());
        toast(b);
        toastIsMsg = true; // a tap on it opens the conversation
        toastConv = msg.conv;
        wake();
    }
    lastIn = msg.conv; // R (outside a text field) opens it
    haveLastIn = true;
    dirtyChats = true;
    refreshStatus();
}

// a channel message we sent, heard by observers on the live feed
void App::onHeard(uint32_t id, uint8_t observers)
{
    for (auto &c : convs)
        for (auto &l : c.lines)
            if (l.mine && l.id == id) {
                l.hops = (int8_t)observers;
                chatsChanged();
                if (chatOpen && c.key == chatKey)
                    renderChat();
                return;
            }
}

void App::onDelivered(uint32_t id, bool ok)
{
    if (!id)
        return;
    for (auto &c : convs)
        for (auto &l : c.lines)
            if (l.mine && l.id == id) {
                l.state = ok ? 2 : 3;
                chatsChanged();
                if (chatOpen && c.key == chatKey)
                    renderChat();
                return;
            }
}

// ---- chat history on flash. Plain bytes: "HXC1", then per conversation its key, title, unread count, last time and
// lines (newest 40), each line: from, text, time, mine, state, hops. Strings are a 2-byte length then the bytes.
namespace
{
constexpr size_t KEEP_LINES = 40, KEEP_BYTES = 96 * 1024;
void put(std::string &o, const void *p, size_t n) { o.append((const char *)p, n); }
template <typename T> void putv(std::string &o, T v) { put(o, &v, sizeof(v)); }
void putStr(std::string &o, const std::string &s)
{
    uint16_t n = (uint16_t)std::min<size_t>(s.size(), 512);
    putv(o, n);
    o.append(s, 0, n);
}
struct Reader {
    const std::string &s;
    size_t at = 0;
    bool ok = true;
    explicit Reader(const std::string &src) : s(src) {}
    template <typename T> T get()
    {
        T v{};
        if (at + sizeof(T) > s.size())
            ok = false;
        else
            memcpy(&v, s.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string str()
    {
        uint16_t n = get<uint16_t>();
        if (!ok || at + n > s.size()) {
            ok = false;
            return "";
        }
        std::string r = s.substr(at, n);
        at += n;
        return r;
    }
};
} // namespace

void App::chatsChanged(void)
{
    if (!chatsChangedAt)
        chatsChangedAt = lv_tick_get() | 1; // never 0
}

void App::saveChats(void)
{
    chatsChangedAt = 0;
    std::vector<const Conv *> order;
    for (auto &c : convs)
        order.push_back(&c);
    std::sort(order.begin(), order.end(), [](const Conv *a, const Conv *b) { return a->last > b->last; }); // newest first
    std::string o = "HXC1";
    for (const Conv *c : order) {
        if (o.size() > KEEP_BYTES)
            break; // the quietest conversations go first
        putv<uint8_t>(o, c->key.dm);
        putv<uint8_t>(o, c->key.channel);
        put(o, c->key.key, 6);
        putStr(o, c->title);
        putv<uint16_t>(o, c->unread);
        putv<uint32_t>(o, c->last);
        const size_t from = c->lines.size() > KEEP_LINES ? c->lines.size() - KEEP_LINES : 0;
        putv<uint16_t>(o, (uint16_t)(c->lines.size() - from));
        for (size_t i = from; i < c->lines.size(); i++) {
            const Line &l = c->lines[i];
            putStr(o, l.from);
            putStr(o, l.text);
            putv<uint32_t>(o, l.time);
            putv<uint8_t>(o, l.mine);
            putv<uint8_t>(o, l.state == 1 ? 4 : l.state); // still waiting for an ACK: "sent" is all we'll know
            putv<int8_t>(o, l.hops);
        }
    }
    m.saveData("chats", o);
}

void App::loadChats(void)
{
    std::string s;
    if (!m.loadData("chats", s) || s.compare(0, 4, "HXC1") != 0)
        return;
    Reader r(s);
    r.at = 4;
    std::vector<Conv> got;
    while (r.ok && r.at < s.size() && got.size() < 24) {
        Conv c;
        c.key.dm = r.get<uint8_t>() != 0;
        c.key.channel = r.get<uint8_t>();
        for (auto &b : c.key.key)
            b = r.get<uint8_t>();
        c.title = r.str();
        c.unread = r.get<uint16_t>();
        c.last = r.get<uint32_t>();
        uint16_t n = r.get<uint16_t>();
        for (uint16_t i = 0; i < n && r.ok; i++) {
            Line l;
            l.from = r.str();
            l.text = r.str();
            l.time = r.get<uint32_t>();
            l.mine = r.get<uint8_t>() != 0;
            l.state = r.get<uint8_t>();
            l.hops = r.get<int8_t>();
            l.seq = ++lineSeq;
            c.lines.push_back(l);
        }
        if (r.ok)
            got.push_back(c);
    }
    convs = got; // a damaged file keeps what read cleanly before the damage
    dirtyChats = true;
}

// ================================================================= more (settings)
void App::buildMore(lv_obj_t *p)
{
    // the hub: four quick switches, then the six categories, two across; each category is a page of cards below it
    static const char *const NAMES[M_COUNT] = {"Radio", "Messages", "Connect", "Display", "Location", "System"};
    static const char *const ICONS[M_COUNT] = {LV_SYMBOL_CHARGE, LV_SYMBOL_ENVELOPE, LV_SYMBOL_WIFI, LV_SYMBOL_IMAGE, LV_SYMBOL_GPS,
                                               LV_SYMBOL_SETTINGS};
    const lv_color_t HUE[M_COUNT] = {C_AMBER, C_CYAN, C_BLUE, C_VIOLET, C_GREEN, C_MUTED};
    moreOpen = -1;
    moreHub = plain(p);
    lv_obj_set_size(moreHub, LV_PCT(100), LV_SIZE_CONTENT);
    column(moreHub, PAD);
    lv_obj_t *qr = plain(moreHub);
    lv_obj_set_size(qr, LV_PCT(100), LV_SIZE_CONTENT);
    row(qr, PAD);
    static const char *const QN[4] = {"Wi-Fi", "Bluetooth", "GPS", "Sound"};
    static const char *const QI[4] = {LV_SYMBOL_WIFI, LV_SYMBOL_BLUETOOTH, LV_SYMBOL_GPS, LV_SYMBOL_BELL};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = plain(qr);
        lv_obj_set_height(t, 40);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(t, T_RADIUS, 0);
        lv_obj_set_style_bg_color(t, C_CARD, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, C_CYAN, LV_STATE_CHECKED); // on: washed in the accent
        lv_obj_set_style_bg_opa(t, LV_OPA_30, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, C_LINE, 0);
        lv_obj_set_style_border_color(t, C_CYAN, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(t, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_t *ic = label(t, F14, C_MUTED, QI[i]);
        lv_obj_set_style_text_color(ic, C_CYAN, LV_STATE_CHECKED);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_t *nm = label(t, F10, C_DIM, QN[i]);
        lv_obj_set_style_text_color(nm, C_TEXT, LV_STATE_CHECKED);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -3);
        lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(nm, LV_OBJ_FLAG_EVENT_BUBBLE);
        stop(t);
        lv_obj_set_user_data(t, (void *)(intptr_t)i);
        auto quick = [](lv_event_t *e) {
                const int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                static const int8_t PAGE[4] = {M_CONNECT, M_CONNECT, M_LOCATION, M_SCREEN};
                if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
                    return self->openMore(PAGE[i]); // hold: its settings
                if (lv_obj_has_flag(self->moreHub, LV_OBJ_FLAG_HIDDEN))
                    return; // the release after a hold
                if (i == 3) { // sound: off, or back to the tone it had
                    const int now = self->m.uiPref("snd", 1);
                    if (now > 0)
                        self->m.setUiPref("snd_was", now);
                    const int v = now > 0 ? 0 : std::max(1, self->m.uiPref("snd_was", 1));
                    self->writeSetting("ui:snd", v);
                    if (v)
                        self->m.playSound(v, self->m.uiPref("vol", 60));
                    self->toast(v ? LV_SYMBOL_BELL " Sound on" : "Sound off");
                    return self->refreshSettings();
                }
                lv_obj_t *sw = self->quickSw[i]; // the page's own switch does the work, so both always agree
                if (!sw)
                    return;
                if (lv_obj_has_state(sw, LV_STATE_CHECKED))
                    lv_obj_remove_state(sw, LV_STATE_CHECKED);
                else
                    lv_obj_add_state(sw, LV_STATE_CHECKED);
                lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
                if (i == 2)
                    self->toast(lv_obj_has_state(sw, LV_STATE_CHECKED) ? LV_SYMBOL_GPS " GPS on" : "GPS off");
                self->refreshMoreHub();
            };
        lv_obj_add_event_cb(t, quick, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(t, quick, LV_EVENT_LONG_PRESSED, nullptr);
        quickTile[i] = t;
        quickSw[i] = nullptr;
    }
    lv_obj_t *grid = plain(moreHub);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, PAD, 0);
    lv_obj_set_style_pad_column(grid, PAD, 0);
    const int32_t tw = LAND ? (W() - RAIL_W - 3 * PAD) / 2 : LV_PCT(100); // two across on the wide screens
    for (int s = 0; s < M_COUNT; s++) {
        lv_obj_t *t = plain(grid);
        lv_obj_set_size(t, tw, LV_SIZE_CONTENT);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(t, C_CARD, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(t, T_RADIUS, 0);
        lv_obj_set_style_pad_all(t, 6, 0);
        lv_obj_set_style_border_side(t, LV_BORDER_SIDE_LEFT, 0); // each category's own colour down its edge
        lv_obj_set_style_border_width(t, 3, 0);
        lv_obj_set_style_border_color(t, HUE[s], 0);
        column(t, 1);
        lv_obj_t *hd = plain(t);
        lv_obj_set_size(hd, LV_PCT(100), LV_SIZE_CONTENT);
        row(hd, 5);
        lv_obj_t *ic = label(hd, F14, HUE[s], ICONS[s]);
        (void)ic;
        lv_obj_t *nm = label(hd, F14, C_TEXT, NAMES[s]);
        lv_obj_set_flex_grow(nm, 1);
        oneLine(nm);
        label(hd, F10, C_DIM, LV_SYMBOL_RIGHT);
        for (int k = 0; k < 2; k++) {
            moreSum[s][k] = label(t, F10, k ? C_DIM : C_MUTED, "");
            lv_obj_set_width(moreSum[s][k], LV_PCT(100));
            oneLine(moreSum[s][k]);
        }
        stop(t);
        lv_obj_set_user_data(t, (void *)(intptr_t)s);
        lv_obj_add_event_cb(
            t, [](lv_event_t *e) { self->openMore((int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e))); },
            LV_EVENT_CLICKED, nullptr);
        moreTile[s] = t;
    }
    for (int s = 0; s < M_COUNT; s++) {
        lv_obj_t *sec = plain(p);
        lv_obj_set_size(sec, LV_PCT(100), LV_SIZE_CONTENT);
        column(sec, PAD);
        lv_obj_add_flag(sec, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *bk = button(sec, "", HUE[s], LV_PCT(100), 30); // the page's title, and the way back
        lv_obj_t *bl = lv_obj_get_child(bk, 0);
        lv_label_set_text_fmt(bl, LV_SYMBOL_LEFT "   %s", NAMES[s]);
        lv_obj_align(bl, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_add_event_cb(bk, [](lv_event_t *) { self->openMore(-1); }, LV_EVENT_CLICKED, nullptr);
        moreSec[s] = sec;
    }

    // "Apply radio change?": a box over the screen, so a slip of a finger can't move the radio off the mesh
    askBox = plain(scr);
    lv_obj_set_size(askBox, W(), H());
    lv_obj_set_style_bg_color(askBox, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(askBox, LV_OPA_60, 0);
    lv_obj_add_flag(askBox, LV_OBJ_FLAG_CLICKABLE); // nothing under it takes a tap
    lv_obj_add_flag(askBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ab = card(askBox, "APPLY RADIO CHANGE?");
    lv_obj_set_width(ab, std::min<int32_t>(W() - 24, 300));
    lv_obj_center(ab);
    askText = label(ab, F12, C_TEXT, "");
    wrap(askText);
    lv_obj_t *abr = plain(ab);
    lv_obj_set_size(abr, LV_PCT(100), LV_SIZE_CONTENT);
    row(abr, PAD);
    lv_obj_t *ac = button(abr, "Cancel", C_MUTED, LV_PCT(48), 30);
    lv_obj_set_flex_grow(ac, 1);
    lv_obj_add_event_cb(
        ac,
        [](lv_event_t *) {
            lv_obj_add_flag(self->askBox, LV_OBJ_FLAG_HIDDEN);
            self->refreshSettings(); // the lists show the radio as it still is
            self->refocus(self->setPreset);
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *aa = button(abr, LV_SYMBOL_OK " Apply", C_AMBER, LV_PCT(48), 30);
    lv_obj_set_flex_grow(aa, 1);
    lv_obj_add_event_cb(aa, [](lv_event_t *) { self->askApply(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cd = card(moreSec[M_RADIO], "THIS RADIO");
    lv_obj_t *r = fieldRow(cd, "Name others see");
    setName = lv_textarea_create(r);
    lv_textarea_set_one_line(setName, true);
    lv_textarea_set_max_length(setName, 31);
    lv_obj_set_width(setName, LV_PCT(100));
    styleInput(setName);
    stop(setName);
    lv_obj_t *sv = button(cd, LV_SYMBOL_SAVE " Save name", C_CYAN, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        sv,
        [](lv_event_t *) {
            const char *n = lv_textarea_get_text(self->setName);
            if (!n[0])
                return self->toast("Enter a name first");
            self->toast(self->m.setNodeName(n) ? LV_SYMBOL_OK " Name saved. Send an advert so others see it." : "Couldn't save the name");
            self->refreshStatus();
            self->refreshHome();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cr = card(moreSec[M_RADIO], "RADIO");
    static int16_t BANDV[N_BANDS];
    static std::string bandOpts;
    if (bandOpts.empty())
        for (int i = 0; i < N_BANDS; i++) {
            BANDV[i] = (int16_t)i;
            bandOpts += (i ? "\n" : "") + std::string(BANDS[i].name);
        }
    addSetting(cr, "ui:band", "Your country's radio band", bandOpts.c_str(), BANDV, N_BANDS);
    bandInfo = label(cr, F10, C_DIM, "");
    wrap(bandInfo);
    r = fieldRow(cr, "Preset (everyone on your mesh must match)");
    std::string opts;
    for (int i = 0; i < N_PRESETS; i++)
        opts += std::string(PRESETS[i].name) + "\n";
    opts += "Custom (set below)";
    setPreset = dropdown(r, opts.c_str());
    r = fieldRow(cr, "Transmit power");
    setTx = dropdown(r, "2 dBm\n5 dBm\n8 dBm\n11 dBm\n14 dBm\n17 dBm\n20 dBm\n22 dBm (max)");
    auto radioChanged = [](lv_event_t *) {
        if (self->loadingSettings)
            return;
        Radio rd = self->m.radio();
        uint32_t pi = lv_dropdown_get_selected(self->setPreset);
        if (pi < (uint32_t)N_PRESETS) {
            rd.freq = PRESETS[pi].freq, rd.bw = PRESETS[pi].bw, rd.sf = PRESETS[pi].sf, rd.cr = PRESETS[pi].cr;
        }
        rd.txPower = TX_LEVELS[std::min<uint32_t>(lv_dropdown_get_selected(self->setTx), sizeof(TX_LEVELS) - 1)];
        self->askRadioChange(rd); // applied only after "Apply"; Cancel puts the lists back
    };
    lv_obj_add_event_cb(setPreset, radioChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(setTx, radioChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    toggle(cr, LV_SYMBOL_LOOP " Repeat for others", &setRepeat);
    lv_obj_add_event_cb(
        setRepeat,
        [](lv_event_t *) {
            if (self->loadingSettings)
                return;
            bool on = lv_obj_has_state(self->setRepeat, LV_STATE_CHECKED);
            if (self->m.setRepeat(on))
                self->toast(on ? LV_SYMBOL_LOOP " Repeating on: this radio relays packets for the mesh" : "Repeating off");
            else { // the radio explains why (its own toast); put the switch back
                self->loadingSettings = true;
                if (on)
                    lv_obj_remove_state(self->setRepeat, LV_STATE_CHECKED);
                else
                    lv_obj_add_state(self->setRepeat, LV_STATE_CHECKED);
                self->loadingSettings = false;
            }
            self->refreshStatus();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *rh = label(cr, F10, C_DIM,
                         "Repeating makes this radio relay other nodes' packets, like a repeater, while it is still your "
                         "companion. It uses more battery and airtime.");
    wrap(rh);
    static const int16_t HASH[] = {0, 1, 2};
    addSetting(cr, "hash_mode", "Path hash size (bytes per repeater in a route)", "1 byte (works with every repeater)\n2 bytes (recommended)\n3 bytes",
               HASH, 3,
               "More bytes tell repeaters apart in long routes and on the map. Repeaters on MeshCore 1.14 or newer pass any size.");
    addSetting(cr, "rx_boost", LV_SYMBOL_DOWNLOAD " Boosted receive", nullptr, nullptr, 0, "Hears weaker signals for a little more power.");
    addSetting(cr, "cad", LV_SYMBOL_EYE_OPEN " Listen before sending", nullptr, nullptr, 0,
               "Waits for a quiet channel first: fewer collisions on a busy mesh.");

    lv_obj_t *cc = card(moreSec[M_RADIO], "CUSTOM RADIO");
    lv_obj_t *ch = label(cc, F10, C_DIM,
                         "For a local mesh with its own settings. Frequency, bandwidth and SF must match the mesh "
                         "exactly; coding rate can differ. Type the MHz, or use Left / Right to step 25 kHz.");
    wrap(ch);
    r = fieldRow(cc, "Frequency (MHz)");
    custFreq = lv_textarea_create(r);
    lv_textarea_set_one_line(custFreq, true);
    lv_textarea_set_max_length(custFreq, 9);
    lv_textarea_set_accepted_chars(custFreq, "0123456789.");
    lv_obj_set_width(custFreq, LV_PCT(100));
    styleInput(custFreq);
    stop(custFreq);
    r = fieldRow(cc, "Bandwidth (kHz)");
    custBw = dropdown(r, "7.8\n10.4\n15.6\n20.8\n31.25\n41.7\n62.5\n125\n250\n500");
    r = fieldRow(cc, "Spreading factor");
    custSf = dropdown(r, "SF5\nSF6\nSF7\nSF8\nSF9\nSF10\nSF11\nSF12");
    r = fieldRow(cc, "Coding rate");
    custCr = dropdown(r, "4/5\n4/6\n4/7\n4/8");
    custNote = label(cc, F10, C_MUTED, "");
    wrap(custNote);
    lv_obj_t *ap = button(cc, LV_SYMBOL_OK " Apply custom settings", C_CYAN, LV_PCT(100), 30);
    lv_obj_add_event_cb(ap, [](lv_event_t *) { self->applyCustom(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cm = card(moreSec[M_MESSAGES], "MESSAGES AND CONTACTS");
    addSetting(cm, "multi_acks", LV_SYMBOL_OK " Send receipts twice", nullptr, nullptr, 0,
               "Direct messages show as delivered more reliably on a weak or busy mesh, for a little more airtime.");
    static const int16_t ADDM[] = {0, 1, 2};
    addSetting(cm, "add_mode", "Add nodes I hear to my contacts", "Automatically\nOnly the types below\nOnly when I add them", ADDM, 3);
    addSetting(cm, "add_chat", "Chat nodes (people)", nullptr, nullptr, 0);
    addSetting(cm, "add_rep", "Repeaters", nullptr, nullptr, 0);
    addSetting(cm, "add_room", "Room servers", nullptr, nullptr, 0);
    addSetting(cm, "add_sensor", "Sensors", nullptr, nullptr, 0);
    static const int16_t HOPS[] = {0, 1, 2, 3, 4, 6};
    addSetting(cm, "add_hops", "Only from nodes this close", "Any distance\nDirect neighbours only\nUp to 1 hop away\nUp to 2 hops\nUp to 3 hops\nUp to 5 hops",
               HOPS, 6);
    static const int16_t FULL[] = {0, 1};
    addSetting(cm, "add_full", "When the contact list is full", "Keep it as it is\nReplace the oldest (never favourites)", FULL, 2);

    lv_obj_t *cg = card(moreSec[M_LOCATION], "POSITION");
    addSetting(cg, "gps", LV_SYMBOL_GPS " GPS", nullptr, nullptr, 0);
    static const int16_t GPSI[] = {1, 10, 60, 300, 900};
    addSetting(cg, "gps_int", "Update my position", "Every second\nEvery 10 seconds\nEvery minute\nEvery 5 minutes\nEvery 15 minutes", GPSI, 5);
    if (m.gpsCanSleep()) {
        static const int16_t GSL[] = {0, 10, 30, 60};
        addSetting(cg, "ui:gps_sleep", "GPS power", "Always on\nWake every 10 minutes\nWake every 30 minutes\nWake every hour", GSL, 4,
                   "Between fixes the GPS is switched off, which saves a lot of battery. Each wake takes up to 3 minutes "
                   "to find you again; your last position stays in the meantime.");
    }
    addSetting(cg, "adv_loc", LV_SYMBOL_UPLOAD " Share my position in adverts", nullptr, nullptr, 0,
               "Puts you on other people's maps. Off keeps your location to yourself.");

    lv_obj_t *cp = card(moreSec[M_MESSAGES], "PRIVACY: WHO CAN ASK THIS RADIO FOR...");
    static const int16_t TEL[] = {0, 1, 2};
    addSetting(cp, "tel_base", "Battery and status", "Nobody\nFavourites only\nAnyone", TEL, 3);
    addSetting(cp, "tel_loc", "My location", "Nobody\nFavourites only\nAnyone", TEL, 3);
    addSetting(cp, "tel_env", "Sensor readings", "Nobody\nFavourites only\nAnyone", TEL, 3);

    lv_obj_t *cb = card(moreSec[M_CONNECT], "PHONE APP");
    toggle(cb, LV_SYMBOL_BLUETOOTH " Bluetooth", &setBt);
    lv_obj_add_event_cb(
        setBt,
        [](lv_event_t *) {
            if (self->loadingSettings)
                return;
            bool on = lv_obj_has_state(self->setBt, LV_STATE_CHECKED);
            self->toast(self->m.setBluetooth(on) ? (on ? LV_SYMBOL_BLUETOOTH " Bluetooth on" : "Bluetooth off") : "Couldn't change Bluetooth");
            self->refreshSettings();
            self->refreshStatus();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    setBtPin = label(cb, F16, C_BLUE, "");
    lv_obj_t *bh = label(cb, F10, C_DIM, "Pair the MeshCore app with this PIN. The screen keeps working while the app is connected.");
    wrap(bh);
    r = fieldRow(cb, "Fixed PIN (six digits; empty = a new PIN each start). Takes effect after a restart.");
    setPin = lv_textarea_create(r);
    lv_textarea_set_one_line(setPin, true);
    lv_textarea_set_max_length(setPin, 6);
    lv_textarea_set_accepted_chars(setPin, "0123456789");
    lv_obj_set_width(setPin, LV_PCT(100));
    styleInput(setPin);
    stop(setPin);
    lv_obj_t *ps = button(cb, LV_SYMBOL_SAVE " Save PIN", C_BLUE, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        ps,
        [](lv_event_t *) {
            const char *t = lv_textarea_get_text(self->setPin);
            int32_t pin = t[0] ? atol(t) : 0;
            if (t[0] && strlen(t) != 6)
                return self->toast("A PIN is six digits (or leave it empty)");
            self->toast(self->m.setSetting("ble_pin", pin) ? (pin ? LV_SYMBOL_OK " PIN saved: it's used from the next restart"
                                                                   : LV_SYMBOL_OK " A new PIN will be made each start")
                                                         : "Couldn't save the PIN");
        },
        LV_EVENT_CLICKED, nullptr);

    if (m.wifiSupported()) {
        lv_obj_t *cw = card(moreSec[M_CONNECT], "WI-FI");
        toggle(cw, LV_SYMBOL_WIFI " Wi-Fi", &wfSwitch);
        lv_obj_add_event_cb(
            wfSwitch,
            [](lv_event_t *) {
                if (self->loadingSettings)
                    return;
                const bool on = lv_obj_has_state(self->wfSwitch, LV_STATE_CHECKED);
                self->m.setWifi(on);
                if (on && !self->m.wifi().ssid[0]) { // nothing saved yet: look for networks to pick from
                    self->m.wifiScan();
                    self->toast(LV_SYMBOL_WIFI " Pick your network below");
                }
                self->refreshWifi();
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        wfStatus = label(cw, F12, C_MUTED, "");
        wrap(wfStatus);
        lv_obj_t *fb = button(cw, LV_SYMBOL_REFRESH " Find networks", C_CYAN, LV_PCT(100), 30);
        lv_obj_add_event_cb(
            fb,
            [](lv_event_t *) {
                self->m.wifiScan();
                self->wfListSeen = -3;
                self->refreshWifi();
            },
            LV_EVENT_CLICKED, nullptr);
        wfList = plain(cw);
        lv_obj_set_size(wfList, LV_PCT(100), LV_SIZE_CONTENT);
        column(wfList, 3);
        r = fieldRow(cw, "Network name");
        wfSsid = lv_textarea_create(r);
        lv_textarea_set_one_line(wfSsid, true);
        lv_textarea_set_max_length(wfSsid, 32);
        lv_obj_set_width(wfSsid, LV_PCT(100));
        styleInput(wfSsid);
        stop(wfSsid);
        r = fieldRow(cw, "Password (empty for an open network)");
        wfPass = lv_textarea_create(r);
        lv_textarea_set_one_line(wfPass, true);
        lv_textarea_set_max_length(wfPass, 64);
        lv_textarea_set_password_mode(wfPass, true);
        lv_obj_set_width(wfPass, LV_PCT(100));
        styleInput(wfPass);
        stop(wfPass);
        lv_obj_add_flag(wfPass, F_SUBMIT);
        lv_obj_add_event_cb(wfPass, [](lv_event_t *) { self->wifiConnectFromFields(); }, LV_EVENT_READY, nullptr);
        lv_obj_t *cb2 = button(cw, LV_SYMBOL_OK " Connect", C_GREEN, LV_PCT(100), 30);
        lv_obj_add_event_cb(cb2, [](lv_event_t *) { self->wifiConnectFromFields(); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *fg = button(cw, LV_SYMBOL_TRASH " Forget this network", C_MUTED, LV_PCT(100), 30);
        lv_obj_add_event_cb(
            fg,
            [](lv_event_t *) {
                self->m.wifiForget();
                lv_textarea_set_text(self->wfSsid, "");
                self->toast("Network forgotten, Wi-Fi off");
                self->refreshWifi();
            },
            LV_EVENT_CLICKED, nullptr);
        wrap(label(cw, F10, C_DIM,
                   "For the street map and the clock. Bluetooth stays on for the phone app. The password is kept on this radio."));

        lv_obj_t *cmp = card(moreSec[M_LOCATION], "MAP");
        toggle(cmp, LV_SYMBOL_IMAGE " Street map", &mapTilesSw);
        lv_obj_add_event_cb(
            mapTilesSw,
            [](lv_event_t *) {
                if (self->loadingSettings)
                    return;
                const bool on = lv_obj_has_state(self->mapTilesSw, LV_STATE_CHECKED);
                self->m.setUiPref("maptiles", on ? 1 : 0);
                hoptics::TileMap::get().setEnabled(on);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        mapSaved = label(cmp, F12, C_MUTED, "");
        lv_obj_t *clr = button(cmp, LV_SYMBOL_TRASH " Clear the saved map", C_AMBER, LV_PCT(100), 30);
        lv_obj_add_event_cb(
            clr,
            [](lv_event_t *) {
                hoptics::TileMap::get().clearSaved();
                self->toast("Saved map tiles cleared");
            },
            LV_EVENT_CLICKED, nullptr);
        wrap(label(cmp, F10, C_DIM,
                   "OpenStreetMap, downloaded over Wi-Fi only for what you look at, and kept on this radio (up to 1.5 MB) so "
                   "places you've seen still show without Wi-Fi. Map data (c) OpenStreetMap contributors."));

        if (m.feedSupported()) {
            lv_obj_t *cf = card(moreSec[M_CONNECT], "LIVE FEED");
            toggle(cf, LV_SYMBOL_DOWNLOAD " Hear it online", &feedSw);
            lv_obj_add_event_cb(
                feedSw,
                [](lv_event_t *) {
                    if (self->loadingSettings)
                        return;
                    const bool on = lv_obj_has_state(self->feedSw, LV_STATE_CHECKED);
                    self->m.setFeed(on);
                    if (on && self->m.wifi().state == WifiStatus::OFF)
                        self->toast(LV_SYMBOL_WIFI " It needs Wi-Fi: turn that on too");
                    self->refreshWifi();
                },
                LV_EVENT_VALUE_CHANGED, nullptr);
            feedStatus = label(cf, F12, C_MUTED, "");
            wrap(feedStatus);
            r = fieldRow(cf, "Server (a CoreScope site)");
            feedUrl = lv_textarea_create(r);
            lv_textarea_set_one_line(feedUrl, true);
            lv_textarea_set_max_length(feedUrl, 95);
            lv_obj_set_width(feedUrl, LV_PCT(100));
            styleInput(feedUrl);
            stop(feedUrl);
            lv_obj_add_flag(feedUrl, F_SUBMIT);
            lv_obj_add_event_cb(feedUrl, [](lv_event_t *) { self->feedServerFromField(); }, LV_EVENT_READY, nullptr);
            lv_obj_t *fs = button(cf, LV_SYMBOL_OK " Use this server", C_CYAN, LV_PCT(100), 30);
            lv_obj_add_event_cb(fs, [](lv_event_t *) { self->feedServerFromField(); }, LV_EVENT_CLICKED, nullptr);
            wrap(label(cf, F10, C_DIM,
                       "What the region's observers hear, from the server's live map. Your radio decodes it with its own "
                       "channel keys, so you see your channels even when you can't hear the mesh (marked " LV_SYMBOL_WIFI
                       " via internet), the map draws their routes, and your channel messages show how many observers heard "
                       "them. Receive only: sending still goes out over the radio."));
        }
        if (m.webSupported()) {
            lv_obj_t *cw = card(moreSec[M_CONNECT], "WEB PAGE");
            toggle(cw, LV_SYMBOL_EYE_OPEN " Dashboard on Wi-Fi", &webSw);
            lv_obj_add_event_cb(
                webSw,
                [](lv_event_t *) {
                    if (self->loadingSettings)
                        return;
                    const bool on = lv_obj_has_state(self->webSw, LV_STATE_CHECKED);
                    self->m.setWeb(on);
                    if (on && self->m.wifi().state == WifiStatus::OFF)
                        self->toast(LV_SYMBOL_WIFI " It needs Wi-Fi: turn that on too");
                    self->refreshWifi();
                },
                LV_EVENT_VALUE_CHANGED, nullptr);
            webStatus = label(cw, F12, C_MUTED, "");
            wrap(webStatus);
            r = fieldRow(cw, "PIN to sign in");
            webPin = label(r, F28, C_CYAN, "");
            lv_obj_t *np = button(cw, LV_SYMBOL_REFRESH " New PIN (signs every browser out)", C_AMBER, LV_PCT(100), 30);
            lv_obj_add_event_cb(
                np,
                [](lv_event_t *) {
                    self->m.newWebPin();
                    self->refreshWifi();
                    self->toast("New PIN: browsers sign in again");
                },
                LV_EVENT_CLICKED, nullptr);
            wrap(label(cw, F10, C_DIM,
                       "Open the address on a phone or computer on the same Wi-Fi and sign in with the PIN: live traffic, "
                       "a map with live routes, channel messages, charts and a line-of-sight tool. The page fetches the internet "
                       "feed itself, so it costs this radio nothing. Direct messages never go to the page."));
        }
    }

    lv_obj_t *cs = card(moreSec[M_SCREEN], "SCREEN");
    r = fieldRow(cs, "Theme");
    setTheme = dropdown(r, "Midnight\nDaylight\nNight red\nAmber\nGlass\nGlass violet\nGlass green\nGlass rose\nGlass gold\nGlass rainbow");
    lv_obj_add_event_cb(
        setTheme,
        [](lv_event_t *) {
            if (self->loadingSettings)
                return;
            int t = (int)lv_dropdown_get_selected(self->setTheme);
            self->m.setUiPref("theme", t);
            self->applyTheme(t); // rebuilds every screen
            self->selectTab(T_MORE);
            self->openMore(M_SCREEN); // back where the theme was picked
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    r = fieldRow(cs, "Brightness");
    setBright = lv_slider_create(r);
    lv_slider_set_range(setBright, 20, 255);
    lv_obj_set_width(setBright, LV_PCT(94));
    lv_obj_set_style_bg_color(setBright, C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(setBright, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(setBright, C_CYAN, LV_PART_KNOB);
    stop(setBright);
    lv_obj_add_event_cb(
        setBright,
        [](lv_event_t *) {
            int v = lv_slider_get_value(self->setBright);
            self->m.setBacklight((uint8_t)v);
            self->m.setUiPref("bright", v);
        },
        LV_EVENT_VALUE_CHANGED, nullptr);

    if (m.hasSound()) {
        lv_obj_t *cv = card(moreSec[M_SCREEN], "SOUND");
        static const int16_t SND[] = {0, 1, 2, 3}, SALL[] = {0, 1};
        addSetting(cv, "ui:snd", LV_SYMBOL_BELL " Message sound", "Off\nChirp\nChime\nPing", SND, 4);
        addSetting(cv, "ui:snd_all", "Play it for", "Direct messages only\nAll messages", SALL, 2,
                   "Messages from the internet feed stay quiet.");
        lv_obj_t *vr = fieldRow(cv, LV_SYMBOL_VOLUME_MAX " Volume");
        setVol = lv_slider_create(vr);
        lv_obj_set_width(setVol, LV_PCT(100));
        lv_slider_set_range(setVol, 0, 100);
        lv_obj_set_style_bg_color(setVol, C_LINE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(setVol, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(setVol, C_CYAN, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(setVol, C_CYAN, LV_PART_KNOB);
        stop(setVol);
        // saved on every change; heard when it settles: on release of a drag, or at each Left / Right key step
        lv_obj_add_event_cb(
            setVol,
            [](lv_event_t *e) {
                const lv_event_code_t c = lv_event_get_code(e);
                if (c != LV_EVENT_VALUE_CHANGED && c != LV_EVENT_RELEASED)
                    return;
                const int v = lv_slider_get_value(self->setVol);
                self->m.setUiPref("vol", v);
                if (c == LV_EVENT_RELEASED || !lv_slider_is_dragged(self->setVol))
                    self->m.playSound(std::max(1, self->m.uiPref("snd", 1)), v);
            },
            LV_EVENT_ALL, nullptr);
    }

    static const int16_t TOUT[] = {30, 60, 120, 300, 600, 0};
    addSetting(cs, "ui:timeout", "Screen off after", "30 seconds\n1 minute\n2 minutes\n5 minutes\n10 minutes\nNever", TOUT, 6);
    addSetting(cs, "ui:lock", LV_SYMBOL_EYE_CLOSE " Lock screen", nullptr, nullptr, 0,
               "When the screen comes back on: the clock, new messages and battery, until a key or a long press.");
    static const int16_t WALL[] = {0, 1, 2, 3}, CHT[] = {0, 1};
    addSetting(cs, "ui:wall", "Wallpaper", "None\nRidgelines\nConstellation\nAurora", WALL, 4,
               "Behind the pages and the lock screen, dimmed to suit the theme.");
    addSetting(cs, "ui:chattext", "Chat text", "Normal\nLarge", CHT, 2);
    if (m.hasKeyLight()) {
        static const int16_t KBL[] = {0, 1, 2};
        addSetting(cs, "ui:kblight", "Keyboard light", "Off\nWhile typing\nWhile the screen is on", KBL, 3,
                   "Alt+B on the keyboard also turns it on and off.");
    }
    // every offset in use, in minutes (half and quarter hours included), with places that use it
    static const struct {
        int16_t min;
        const char *where;
    } TZ[] = {{-720, ""}, {-660, " (Samoa US)"}, {-600, " (Hawaii)"}, {-570, " (Marquesas)"}, {-540, " (Alaska)"}, {-480, " (Pacific)"},
              {-420, " (Mountain)"}, {-360, " (Central)"}, {-300, " (Eastern)"}, {-240, " (Atlantic)"}, {-210, " (Newfoundland)"},
              {-180, " (Brazil, Argentina)"}, {-120, ""}, {-60, " (Azores)"}, {0, " (UK, Portugal)"}, {60, " (Central Europe)"},
              {120, " (Eastern Europe)"}, {180, " (Moscow, Turkey)"}, {210, " (Iran)"}, {240, " (Gulf)"}, {270, " (Afghanistan)"},
              {300, " (Pakistan)"}, {330, " (India)"}, {345, " (Nepal)"}, {360, " (Bangladesh)"}, {390, " (Myanmar)"},
              {420, " (Thailand)"}, {480, " (China, Perth)"}, {525, " (Eucla)"}, {540, " (Japan, Korea)"}, {570, " (Adelaide)"},
              {600, " (Sydney)"}, {630, " (Lord Howe)"}, {660, ""}, {720, " (New Zealand)"}, {765, " (Chatham)"}, {780, " (Tonga)"},
              {840, " (Kiribati)"}};
    constexpr int N_TZ = sizeof(TZ) / sizeof(TZ[0]);
    static int16_t TZV[N_TZ];
    static std::string tzOpts;
    if (tzOpts.empty())
        for (int i = 0; i < N_TZ; i++) {
            TZV[i] = TZ[i].min;
            const int a = TZ[i].min < 0 ? -TZ[i].min : TZ[i].min;
            char b[40];
            if (a % 60)
                snprintf(b, sizeof(b), "%sUTC%c%d:%02d%s", i ? "\n" : "", TZ[i].min < 0 ? '-' : '+', a / 60, a % 60, TZ[i].where);
            else
                snprintf(b, sizeof(b), "%sUTC%+d%s", i ? "\n" : "", TZ[i].min / 60, TZ[i].where);
            tzOpts += b;
        }
    lv_obj_t *ck = card(moreSec[M_SYSTEM], "CLOCK");
    addSetting(ck, "tzm", "Time zone (for the clock)", tzOpts.c_str(), TZV, N_TZ,
               "Summer time isn't automatic: pick one hour more while it is on (e.g. Pacific summer = UTC-7).");

    lv_obj_t *cz = card(moreSec[M_RADIO], "REGION");
    lv_obj_t *zh = label(cz, F10, C_DIM,
                         "Only if your mesh uses regions: adverts and channel messages then flood only within that region. "
                         "Leave it empty otherwise (Sacramento's SMF: leave it empty).");
    wrap(zh);
    r = fieldRow(cz, "Region name (empty = everywhere)");
    setScope = lv_textarea_create(r);
    lv_textarea_set_one_line(setScope, true);
    lv_textarea_set_max_length(setScope, 29);
    lv_obj_set_width(setScope, LV_PCT(100));
    styleInput(setScope);
    stop(setScope);
    lv_obj_t *zs = button(cz, LV_SYMBOL_SAVE " Save region", C_CYAN, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        zs,
        [](lv_event_t *) {
            const char *t = lv_textarea_get_text(self->setScope);
            self->toast(self->m.setSettingText("scope", t) ? (t[0] ? LV_SYMBOL_OK " Region saved" : LV_SYMBOL_OK " Region cleared: flooding everywhere")
                                                          : "Couldn't save the region");
            self->refreshSettings();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cdv = card(moreSec[M_SYSTEM], "DEVICE");
    lv_obj_t *rb = button(cdv, LV_SYMBOL_REFRESH " Restart the radio", C_AMBER, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        rb,
        [](lv_event_t *) {
            self->toast("Restarting...");
            self->saveChats();
            lv_refr_now(nullptr);
            if (!self->m.setSetting("reboot", 1))
                self->toast("This radio can't restart from here");
        },
        LV_EVENT_CLICKED, nullptr);
    {
        int32_t v;
        if (!m.getSetting("reboot", v))
            lv_obj_add_flag(rb, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *cl = card(moreSec[M_SYSTEM], "DEVICE LOG");
    lv_obj_t *lb = button(cl, LV_SYMBOL_UPLOAD " Send the log to USB", C_CYAN, LV_PCT(100), 30);
    lv_obj_add_event_cb(
        lb,
        [](lv_event_t *) {
            self->toast(LV_SYMBOL_UPLOAD " Sending the log to USB...");
            lv_refr_now(nullptr);
            if (!self->m.sendLog())
                self->toast("This radio keeps no log");
            else
                self->toast(LV_SYMBOL_OK " Log sent: now press Download logs on the install page");
        },
        LV_EVENT_CLICKED, nullptr);
    wrap(label(cl, F10, C_DIM,
               "Something went wrong? Plug the radio into a computer, open the Hoptics install page, Connect, then Logs & "
               "Console. Press this button, then Download logs, and send the file. The radio keeps its last three "
               "sessions, including a crash if there was one. No messages, names, positions, network names or "
               "passwords go in it."));

    lv_obj_t *ca = card(moreSec[M_SYSTEM], "ABOUT");
    setAbout = label(ca, F12, C_TEXT, "");
    wrap(setAbout);
    stop(ca);

    // the quick switches drive the pages' own switches; one a radio lacks is not shown
    quickSw[0] = m.wifiSupported() ? wfSwitch : nullptr;
    quickSw[1] = setBt;
    for (auto &sr : setRows)
        if (!strcmp(sr.key, "gps")) {
            int32_t v;
            quickSw[2] = readSetting("gps", v) ? sr.ctl : nullptr;
        }
    for (int i = 0; i < 4; i++)
        if (i == 3 ? !m.hasSound() : !quickSw[i])
            lv_obj_add_flag(quickTile[i], LV_OBJ_FLAG_HIDDEN);
}

// -1 = the six categories; otherwise that category's page. The page starts at its top, the keys' cursor on its first
// control; back on the hub the cursor returns to the category just left.
void App::openMore(int s)
{
    const int was = moreOpen;
    moreOpen = s >= 0 && s < M_COUNT ? s : -1;
    if (moreOpen < 0)
        lv_obj_remove_flag(moreHub, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(moreHub, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < M_COUNT; i++)
        if (i == moreOpen)
            lv_obj_remove_flag(moreSec[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(moreSec[i], LV_OBJ_FLAG_HIDDEN);
    if (tab != T_MORE)
        return;
    if (moreOpen < 0)
        refreshMoreHub();
    lv_obj_update_layout(pages[T_MORE]);
    lv_obj_scroll_to_y(pages[T_MORE], 0, LV_ANIM_OFF);
    refocus(moreOpen < 0 && was >= 0 ? moreTile[was] : nullptr);
}

// what each category is set to, in a line or two, so most of the time there's no need to open it
void App::refreshMoreHub(void)
{
    if (!moreHub)
        return;
    char a[64], b[64];
    int32_t v = 0;
    // Radio: the preset (or the custom numbers) and the power
    const Radio r = m.radio();
    const char *pn = nullptr;
    for (int i = 0; i < N_PRESETS; i++)
        if (fabsf(PRESETS[i].freq - r.freq) < 0.002f && fabsf(PRESETS[i].bw - r.bw) < 0.01f && PRESETS[i].sf == r.sf && PRESETS[i].cr == r.cr)
            pn = PRESETS[i].name;
    setText(moreSum[M_RADIO][0], pn ? pn : "Custom radio");
    snprintf(a, sizeof(a), "%.3f MHz \xC2\xB7 SF%u \xC2\xB7 %d dBm", r.freq, (unsigned)r.sf, (int)r.txPower);
    setText(moreSum[M_RADIO][1], a);
    // Messages: who gets added, and who may ask where this radio is
    static const char *const ADD[3] = {"Adds everyone heard", "Adds the types picked", "Adds only by hand"};
    setText(moreSum[M_MESSAGES][0], readSetting("add_mode", v) && v >= 0 && v < 3 ? ADD[v] : "Contacts");
    static const char *const WHO[3] = {"nobody", "favourites", "anyone"};
    if (readSetting("tel_loc", v) && v >= 0 && v < 3)
        snprintf(b, sizeof(b), "Location asked by %s", WHO[v]);
    else
        snprintf(b, sizeof(b), "Privacy");
    setText(moreSum[M_MESSAGES][1], b);
    // Connect: the links, then what Wi-Fi is doing
    const WifiStatus w = m.wifi();
    const bool wifiOn = m.wifiSupported() && w.state != WifiStatus::OFF;
    if (m.wifiSupported())
        snprintf(a, sizeof(a), "Wi-Fi %s \xC2\xB7 Bluetooth %s", wifiOn ? "on" : "off", m.bluetoothEnabled() ? "on" : "off");
    else
        snprintf(a, sizeof(a), "Bluetooth %s", m.bluetoothEnabled() ? "on" : "off");
    setText(moreSum[M_CONNECT][0], a);
    static const char *const WST[5] = {"Offline", "Joining...", "Online", "Network not found", "Couldn't join"};
    setText(moreSum[M_CONNECT][1], !m.wifiSupported() ? "Phone app" : w.state < 5 ? WST[w.state] : "");
    // Screen & sound: the theme, then the sound (or the screen timeout on a radio with no speaker)
    static const char *const THEME[10] = {"Midnight", "Daylight", "Night red", "Amber", "Glass", "Glass violet", "Glass green", "Glass rose", "Glass gold", "Glass rainbow"};
    const int th = m.uiPref("theme", 0);
    snprintf(a, sizeof(a), "%s theme \xC2\xB7 %d%%", THEME[th >= 0 && th < 10 ? th : 0], (m.uiPref("bright", 200) * 100 + 127) / 255);
    setText(moreSum[M_SCREEN][0], a);
    if (m.hasSound()) {
        static const char *const TONE[4] = {"Off", "Chirp", "Chime", "Ping"};
        const int sn = m.uiPref("snd", 1);
        if (sn > 0 && sn < 4)
            snprintf(b, sizeof(b), "Sound: %s \xC2\xB7 %d%%", TONE[sn], m.uiPref("vol", 60));
        else
            snprintf(b, sizeof(b), "Sound off");
    } else {
        const int to = m.uiPref("timeout", 60);
        if (to <= 0)
            snprintf(b, sizeof(b), "Screen stays on");
        else if (to < 60)
            snprintf(b, sizeof(b), "Screen off after %ds", to);
        else
            snprintf(b, sizeof(b), "Screen off after %d min", to / 60);
    }
    setText(moreSum[M_SCREEN][1], b);
    // Location: GPS, and whether adverts carry the position
    if (readSetting("gps", v))
        snprintf(a, sizeof(a), "GPS %s", v ? "on" : "off");
    else
        snprintf(a, sizeof(a), "No GPS");
    setText(moreSum[M_LOCATION][0], a);
    const bool share = readSetting("adv_loc", v) && v;
    snprintf(b, sizeof(b), "%s%s", share ? "Shared in adverts" : "Kept to myself", mapTiles() ? " \xC2\xB7 street map" : "");
    setText(moreSum[M_LOCATION][1], b);
    // System: the version and how long it has been up
    const Stats st = m.stats();
    snprintf(a, sizeof(a), "Hoptics-MC %s", m.firmwareVersion());
    setText(moreSum[M_SYSTEM][0], a);
    snprintf(b, sizeof(b), "Up %luh %02lum", (unsigned long)(st.uptimeS / 3600), (unsigned long)(st.uptimeS / 60 % 60));
    setText(moreSum[M_SYSTEM][1], b);
    // the quick switches show what they would flip
    for (int i = 0; i < 4; i++) {
        bool on = false;
        if (i == 3)
            on = m.uiPref("snd", 1) > 0;
        else if (quickSw[i])
            on = lv_obj_has_state(quickSw[i], LV_STATE_CHECKED);
        if (on)
            lv_obj_add_state(quickTile[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(quickTile[i], LV_STATE_CHECKED);
    }
}

// a preset or power picked: asked first, with the new numbers and any warning about the country's band
void App::askRadioChange(const Radio &rd)
{
    askRadio = rd;
    const std::string warn = bandWarning(rd);
    setTextFmt(askText, "%.3f MHz, %g kHz, SF%u, CR 4/%u, %d dBm.\nEveryone on your mesh must use the same settings, or you "
                        "won't hear each other.%s%s",
               rd.freq, rd.bw, (unsigned)rd.sf, (unsigned)rd.cr, (int)rd.txPower, warn.empty() ? "" : "\n\n", warn.c_str());
    lv_obj_remove_flag(askBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(askBox);
    refocus();
}

void App::askApply(void)
{
    lv_obj_add_flag(askBox, LV_OBJ_FLAG_HIDDEN);
    const std::string warn = bandWarning(askRadio);
    toast(!m.setRadio(askRadio) ? "Couldn't change the radio settings" : warn.empty() ? LV_SYMBOL_OK " Radio settings saved" : warn.c_str());
    refreshSettings();
    refreshHome();
    refocus(setPreset);
}

// a keyed setting as a switch (opts == nullptr) or a dropdown; it saves as soon as it changes
void App::addSetting(lv_obj_t *parent, const char *key, const char *labelText, const char *opts, const int16_t *vals, int nvals, const char *help)
{
    SetRow sr;
    sr.key = key;
    sr.vals = opts ? vals : nullptr;
    sr.nvals = nvals;
    sr.row = plain(parent); // the control and its help, shown or hidden together
    lv_obj_set_size(sr.row, LV_PCT(100), LV_SIZE_CONTENT);
    column(sr.row, 2);
    if (opts)
        sr.ctl = dropdown(fieldRow(sr.row, labelText), opts);
    else
        toggle(sr.row, labelText, &sr.ctl);
    if (help)
        wrap(label(sr.row, F10, C_DIM, help));
    lv_obj_set_user_data(sr.ctl, (void *)(intptr_t)setRows.size());
    lv_obj_add_event_cb(
        sr.ctl,
        [](lv_event_t *e) {
            if (self->loadingSettings)
                return;
            size_t i = (size_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
            if (i >= self->setRows.size())
                return;
            SetRow &r = self->setRows[i];
            int32_t v = r.vals ? r.vals[std::min<int>((int)lv_dropdown_get_selected(r.ctl), r.nvals - 1)] : (lv_obj_has_state(r.ctl, LV_STATE_CHECKED) ? 1 : 0);
            bool ok = self->writeSetting(r.key, v);
            if (ok && !strcmp(r.key, "ui:band")) {
                const std::string warn = self->bandWarning(self->m.radio());
                self->refreshSettings();
                return self->toast(warn.empty() ? LV_SYMBOL_OK " Saved: the radio fits that band" : warn.c_str());
            }
            if (ok && !strcmp(r.key, "ui:snd") && v > 0)
                self->m.playSound(v, self->m.uiPref("vol", 60)); // hear the one picked
            if (ok && !strcmp(r.key, "ui:wall")) {
                const lv_image_dsc_t *wp = self->wallpaper();
                lv_obj_set_style_bg_image_src(self->scr, wp, 0);
                lv_obj_invalidate(self->scr);
                if (!wp && self->wpBuf) { // off: nothing shows the old picture now
                    lv_image_cache_drop(&self->wpDsc);
                    free(self->wpBuf);
                    self->wpBuf = nullptr;
                    self->wpPic = -1;
                }
            }
            self->toast(ok ? LV_SYMBOL_OK " Saved" : "Couldn't change that setting");
            self->refreshSettings(); // puts a refused change back, and shows or hides the rows that depend on it
            self->refreshStatus();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    setRows.push_back(sr);
}

bool App::readSetting(const char *key, int32_t &v) const
{
    if (!strcmp(key, "tzm")) { // the time zone in minutes: MeshCore's whole hours plus Hoptics' extra minutes
        int32_t h = 0;
        if (!m.getSetting("tz", h))
            return false;
        const int extra = m.uiPref("tz_min", 0);
        v = h * 60 + (h < 0 ? -extra : extra);
        return true;
    }
    if (!strncmp(key, "ui:", 3)) {
        const char *k = key + 3; // the defaults used where the pref is read
        v = m.uiPref(k, !strcmp(k, "timeout") ? 60 : (!strcmp(k, "kblight") || !strcmp(k, "snd") || !strcmp(k, "snd_all")) ? 1 : 0);
        return true;
    }
    return m.getSetting(key, v);
}

bool App::writeSetting(const char *key, int32_t v)
{
    if (!strcmp(key, "tzm")) {
        if (!m.setSetting("tz", v / 60)) // truncates toward zero: -210 is -3 h and 30 min
            return false;
        m.setUiPref("tz_min", (v < 0 ? -v : v) % 60);
        return true;
    }
    if (!strncmp(key, "ui:", 3)) {
        m.setUiPref(key + 3, v);
        return true;
    }
    return m.setSetting(key, v);
}

// the clock: the radio's time zone when it has one, the system's otherwise (the PC harness)
void App::localTime(uint32_t t, struct tm &out) const
{
    int32_t tz = 0;
    time_t tt = t;
    if (m.getSetting("tz", tz)) {
        const int extra = m.uiPref("tz_min", 0); // India, Nepal, Newfoundland...: the minutes past the hour
        tt += (time_t)tz * 3600 + (tz < 0 ? -extra : extra) * 60;
        gmtime_r(&tt, &out);
    } else
        localtime_r(&tt, &out);
}

// the custom card shows whatever the radio is on now, so a small change is a small edit
void App::loadCustom(void)
{
    Radio r = m.radio();
    char b[16];
    snprintf(b, sizeof(b), "%.3f", (double)r.freq);
    lv_textarea_set_text(custFreq, b);
    uint32_t bi = 6;
    for (int i = 0; i < N_BWS; i++)
        if (fabsf(BWS[i] - r.bw) < 0.2f)
            bi = i;
    lv_dropdown_set_selected(custBw, bi);
    lv_dropdown_set_selected(custSf, (uint32_t)std::min(std::max((int)r.sf, 5), 12) - 5);
    lv_dropdown_set_selected(custCr, (uint32_t)std::min(std::max((int)r.cr, 5), 8) - 5);
    lv_label_set_text_fmt(custNote, "Now: %.3f MHz  BW %g  SF%u  CR%u", (double)r.freq, (double)r.bw, (unsigned)r.sf, (unsigned)r.cr);
}

// empty when the radio fits the country band picked in settings (or none is picked); a warning otherwise
std::string App::bandWarning(const Radio &rd) const
{
    const int b = m.uiPref("band", 0);
    if (b <= 0 || b >= N_BANDS)
        return "";
    const Band &k = BANDS[b];
    const float half = rd.bw / 2000.0f; // the signal's edges must fit in the band too
    char w[120] = "";
    if (rd.freq - half < k.lo || rd.freq + half > k.hi)
        snprintf(w, sizeof(w), "Note: outside the %s band (%.3f-%.3f MHz). Check local rules.", k.name, k.lo, k.hi);
    else if (rd.txPower > k.dbm)
        snprintf(w, sizeof(w), "Note: above %s's limit of %d dBm. Check local rules.", k.name, k.dbm);
    return w;
}

void App::applyCustom(void)
{
    const char *t = lv_textarea_get_text(custFreq);
    char *end = nullptr;
    float f = strtof(t, &end);
    if (!t[0] || end == t || *end || f < 150.0f || f > 2500.0f)
        return toast("Enter a frequency in MHz, like 910.525");
    Radio rd = m.radio();
    rd.freq = f;
    rd.bw = BWS[std::min<uint32_t>(lv_dropdown_get_selected(custBw), N_BWS - 1)];
    rd.sf = (uint8_t)(5 + lv_dropdown_get_selected(custSf));
    rd.cr = (uint8_t)(5 + lv_dropdown_get_selected(custCr));
    if (!m.setRadio(rd))
        return toast("Couldn't use those radio settings");
    const std::string warn = bandWarning(rd);
    toast(warn.empty() ? LV_SYMBOL_OK " Custom radio settings saved" : warn.c_str());
    refreshSettings();
    refreshHome();
}

// Left / Right on the custom frequency box step it 25 kHz (for keypads without digits)
static bool stepFreq(lv_obj_t *ta, bool up)
{
    float f = strtof(lv_textarea_get_text(ta), nullptr);
    if (f < 150.0f || f > 2500.0f)
        f = 910.525f;
    f += up ? 0.025f : -0.025f;
    char b[16];
    snprintf(b, sizeof(b), "%.3f", (double)f);
    lv_textarea_set_text(ta, b);
    return true;
}

void App::feedServerFromField(void)
{
    std::string u = lv_textarea_get_text(feedUrl);
    if (!m.setFeedServer(u.c_str()))
        return toast("That address doesn't look right (example: scope.sacmesh.org)");
    toast(LV_SYMBOL_OK " Live feed server saved");
    refreshWifi();
}

void App::wifiConnectFromFields(void)
{
    std::string ssid = lv_textarea_get_text(wfSsid), pass = lv_textarea_get_text(wfPass);
    if (ssid.empty())
        return toast("Pick a network, or type its name");
    if (!m.wifiJoin(ssid.c_str(), pass.c_str()))
        return toast("Couldn't save that network");
    lv_textarea_set_text(wfPass, "");
    std::string t = LV_SYMBOL_WIFI " Connecting to " + ssid + "...";
    toast(t.c_str());
    refreshWifi();
}

// Wi-Fi's state and the networks found, while Settings is on screen
void App::refreshWifi(void)
{
    if (!wfStatus)
        return;
    const WifiStatus w = m.wifi();
    loadingSettings = true;
    if (w.state != WifiStatus::OFF)
        lv_obj_add_state(wfSwitch, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(wfSwitch, LV_STATE_CHECKED);
    loadingSettings = false;
    char b[128];
    lv_color_t c = C_MUTED;
    switch (w.state) {
    case WifiStatus::CONNECTED:
        snprintf(b, sizeof(b), LV_SYMBOL_OK " Connected to %s\n%s, %d dBm%s", w.ssid, w.ip, w.rssi,
                 w.clockFromInternet ? ", clock set from the internet" : "");
        c = C_GREEN;
        break;
    case WifiStatus::CONNECTING:
        if (w.reason == 15 || w.reason == 202 || w.reason == 204 || w.reason == 2) {
            snprintf(b, sizeof(b), "%s turned down the password. Check it and Connect again.", w.ssid);
            c = C_RED;
        } else if (w.reason == 201)
            snprintf(b, sizeof(b), "Can't find %s. Out of range, or a 5 GHz-only network (this radio uses 2.4 GHz). Still trying.", w.ssid);
        else if (w.reason)
            snprintf(b, sizeof(b), "Connecting to %s... (the last try failed, code %u; still trying)", w.ssid, (unsigned)w.reason);
        else
            snprintf(b, sizeof(b), "Connecting to %s...", w.ssid);
        if (w.reason != 15 && w.reason != 202 && w.reason != 204 && w.reason != 2)
            c = C_AMBER;
        break;
    case WifiStatus::NO_NETWORK:
        snprintf(b, sizeof(b), "Can't find %s. Out of range, or a 5 GHz-only network (this radio uses 2.4 GHz).", w.ssid);
        c = C_RED;
        break;
    case WifiStatus::FAILED:
        snprintf(b, sizeof(b), "%s didn't let this radio in: check the password.", w.ssid);
        c = C_RED;
        break;
    default:
        snprintf(b, sizeof(b), w.ssid[0] ? "Off. Saved network: %s" : "Off. No network saved yet.", w.ssid);
        break;
    }
    setText(wfStatus, b);
    setColor(wfStatus, c);

    if (feedStatus) {
        const FeedStatus f = m.feed();
        loadingSettings = true;
        if (f.state != FeedStatus::OFF)
            lv_obj_add_state(feedSw, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(feedSw, LV_STATE_CHECKED);
        loadingSettings = false;
        c = C_MUTED;
        switch (f.state) {
        case FeedStatus::LIVE:
            snprintf(b, sizeof(b), LV_SYMBOL_OK " Live: %lu packets, %lu messages your radio missed", (unsigned long)f.packets,
                     (unsigned long)f.messages);
            c = C_GREEN;
            break;
        case FeedStatus::CONNECTING:
            snprintf(b, sizeof(b), "Connecting to %s...", f.server);
            c = C_AMBER;
            break;
        case FeedStatus::NO_WIFI:
            snprintf(b, sizeof(b), "Waiting for Wi-Fi");
            c = C_AMBER;
            break;
        case FeedStatus::FAILED:
            snprintf(b, sizeof(b), "Not connected: %s. It keeps trying.", f.error[0] ? f.error : "no answer");
            c = C_RED;
            break;
        default:
            snprintf(b, sizeof(b), "Off");
            break;
        }
        setText(feedStatus, b);
        setColor(feedStatus, c);
    }

    if (webStatus) {
        const WebStatus w = m.web();
        loadingSettings = true;
        if (w.on)
            lv_obj_add_state(webSw, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(webSw, LV_STATE_CHECKED);
        loadingSettings = false;
        if (!w.on)
            snprintf(b, sizeof(b), "Off");
        else if (!w.running)
            snprintf(b, sizeof(b), "Waiting for Wi-Fi");
        else
            snprintf(b, sizeof(b), "Open %s\nor %s%s", w.addr, w.host, w.sessions ? (w.sessions == 1 ? "\n1 browser signed in" : "\nBrowsers signed in") : "");
        setText(webStatus, b);
        setColor(webStatus, !w.on ? C_MUTED : w.running ? C_GREEN : C_AMBER);
        setText(webPin, w.on ? w.pin : "");
    }

    WifiNet nets[10];
    const int n = m.wifiScanResults(nets, 10);
    if (n != wfListSeen) { // rebuilt only when what was found changes
        wfListSeen = n;
        lv_obj_clean(wfList);
        if (n == -1)
            label(wfList, F12, C_AMBER, "Looking for networks...");
        else if (n == 0)
            label(wfList, F12, C_MUTED, "No 2.4 GHz networks found.");
        for (int i = 0; i < n; i++) {
            lv_obj_t *nr = lv_button_create(wfList);
            lv_obj_remove_style_all(nr);
            lv_obj_set_size(nr, LV_PCT(100), 28);
            lv_obj_set_style_bg_color(nr, C_CARD2, 0);
            lv_obj_set_style_bg_opa(nr, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(nr, 6, 0);
            lv_obj_set_style_pad_hor(nr, 8, 0);
            row(nr, 6);
            lv_obj_t *l = label(nr, F14, C_TEXT, nets[i].ssid);
            lv_obj_set_flex_grow(l, 1);
            oneLine(l);
            char sig[24];
            snprintf(sig, sizeof(sig), "%s%d dBm", nets[i].open ? "open  " : LV_SYMBOL_EYE_CLOSE "  ", nets[i].rssi);
            label(nr, F10, nets[i].rssi > -65 ? C_GREEN : nets[i].rssi > -78 ? C_AMBER : C_RED, sig);
            stop(nr);
            lv_obj_add_event_cb(
                nr,
                [](lv_event_t *e) {
                    lv_obj_t *nm = lv_obj_get_child(lv_event_get_target_obj(e), 0);
                    lv_textarea_set_text(self->wfSsid, lv_label_get_text(nm));
                    lv_obj_t *sig = lv_obj_get_child(lv_event_get_target_obj(e), 1);
                    if (!strncmp(lv_label_get_text(sig), "open", 4))
                        return self->wifiConnectFromFields(); // no password to type
                    lv_group_focus_obj(self->wfPass);
                    if (self->kbNeeded())
                        self->kbShow(self->wfPass);
                    self->toast("Now its password");
                },
                LV_EVENT_CLICKED, nullptr);
        }
        if (tab == T_MORE && !topOverlay())
            refocus(lv_group_get_focused(group));
    }
    if (mapSaved) {
        const hoptics::MapStatus st = hoptics::TileMap::get().status();
        snprintf(b, sizeof(b), "Saved on this radio: %u tiles, %u KB", (unsigned)st.savedTiles, (unsigned)st.savedKB);
        setText(mapSaved, b);
    }
}

void App::refreshSettings(void)
{
    loadingSettings = true;
    if (bandInfo) {
        const int b = m.uiPref("band", 0);
        if (b > 0 && b < N_BANDS)
        {
            char duty[64] = "";
            if (BANDS[b].duty < 100)
                snprintf(duty, sizeof(duty), ", transmitting at most %g %% of the time", BANDS[b].duty);
            setTextFmt(bandInfo, "%.3f-%.3f MHz, up to %d dBm%s. The radio warns when a setting is outside it.", BANDS[b].lo,
                       BANDS[b].hi, BANDS[b].dbm, duty);
        }
        else
            setText(bandInfo, "Pick yours and the radio warns when a frequency or power is outside the local rules.");
    }
    lv_textarea_set_text(setName, m.nodeName());
    Radio r = m.radio();
    uint32_t pi = N_PRESETS;
    for (int i = 0; i < N_PRESETS; i++)
        if (fabsf(PRESETS[i].freq - r.freq) < 0.002f && fabsf(PRESETS[i].bw - r.bw) < 0.01f && PRESETS[i].sf == r.sf && PRESETS[i].cr == r.cr)
            pi = i;
    lv_dropdown_set_selected(setPreset, pi);
    loadCustom();
    uint32_t ti = sizeof(TX_LEVELS) - 1;
    for (uint32_t i = 0; i < sizeof(TX_LEVELS); i++)
        if (TX_LEVELS[i] <= r.txPower)
            ti = i;
    lv_dropdown_set_selected(setTx, ti);
    if (m.repeatEnabled())
        lv_obj_add_state(setRepeat, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(setRepeat, LV_STATE_CHECKED);
    if (m.bluetoothEnabled()) {
        lv_obj_add_state(setBt, LV_STATE_CHECKED);
        if (m.bluetoothPin())
            lv_label_set_text_fmt(setBtPin, LV_SYMBOL_BLUETOOTH "  PIN  %06u", (unsigned)m.bluetoothPin());
        else
            lv_label_set_text(setBtPin, LV_SYMBOL_BLUETOOTH "  PIN shown when the app pairs");
    } else {
        lv_obj_remove_state(setBt, LV_STATE_CHECKED);
        lv_label_set_text(setBtPin, "Bluetooth is off");
    }
    lv_dropdown_set_selected(setTheme, (uint32_t)m.uiPref("theme", 0));
    if (wfSsid) {
        lv_textarea_set_text(wfSsid, m.wifi().ssid);
        if (mapTiles())
            lv_obj_add_state(mapTilesSw, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(mapTilesSw, LV_STATE_CHECKED);
        wfListSeen = -3; // the list is drawn afresh
    }
    if (feedUrl)
        lv_textarea_set_text(feedUrl, m.feed().server);
    lv_slider_set_value(setBright, m.uiPref("bright", 200), LV_ANIM_OFF);
    if (setVol)
        lv_slider_set_value(setVol, m.uiPref("vol", 60), LV_ANIM_OFF);
    int32_t addMode = 0;
    const bool pick = readSetting("add_mode", addMode) && addMode == 1;
    for (auto &sr : setRows) {
        int32_t v;
        bool show = readSetting(sr.key, v);
        if (show && !strncmp(sr.key, "add_", 4) && strcmp(sr.key, "add_mode") && strcmp(sr.key, "add_hops") && strcmp(sr.key, "add_full"))
            show = pick; // the type switches only mean something in "only the types below"
        if (!show) {
            lv_obj_add_flag(sr.row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(sr.row, LV_OBJ_FLAG_HIDDEN);
        if (!sr.vals) {
            if (v)
                lv_obj_add_state(sr.ctl, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(sr.ctl, LV_STATE_CHECKED);
        } else {
            int best = 0; // the option with this value, or the nearest below it
            for (int i = 0; i < sr.nvals; i++)
                if (sr.vals[i] == v || (sr.vals[i] < v && sr.vals[i] > sr.vals[best]))
                    best = i;
            lv_dropdown_set_selected(sr.ctl, (uint32_t)best);
        }
    }
    if (setScope) {
        char sc[32] = "";
        bool has = m.getSettingText("scope", sc, sizeof(sc));
        lv_textarea_set_text(setScope, sc[0] == '#' ? sc + 1 : sc);
        lv_obj_t *crd = lv_obj_get_parent(lv_obj_get_parent(setScope));
        if (has)
            lv_obj_remove_flag(crd, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(crd, LV_OBJ_FLAG_HIDDEN);
    }
    if (setPin) {
        int32_t pin = 0;
        bool has = m.getSetting("ble_pin", pin);
        char pb[8] = "";
        if (pin)
            snprintf(pb, sizeof(pb), "%06ld", (long)pin);
        lv_textarea_set_text(setPin, pb);
        lv_obj_t *row = lv_obj_get_parent(setPin);
        if (has)
            lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    uint8_t pk[32];
    m.pubKey(pk);
    char b[200];
    Stats s = m.stats();
    snprintf(b, sizeof(b), "Hoptics-MC %s\n%s\nKey: %02x%02x%02x%02x%02x%02x%02x%02x...\nUp %luh %02lum  -  rx %lu  tx %lu", m.firmwareVersion(),
             m.boardName(), pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7], (unsigned long)(s.uptimeS / 3600),
             (unsigned long)(s.uptimeS / 60 % 60), (unsigned long)s.rx, (unsigned long)s.tx);
    lv_label_set_text(setAbout, b);
    loadingSettings = false;
    refreshWifi();
    refreshMoreHub();
}

// ================================================================= navigation, keys, power
void App::selectTab(int t)
{
    if (t < 0 || t >= T_COUNT)
        return;
    lv_obj_add_flag(replyBox, LV_OBJ_FLAG_HIDDEN);
    if (chatOpen)
        closeChat();
    if (!lv_obj_has_flag(nodeOv, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(nodeOv, LV_OBJ_FLAG_HIDDEN);
    if (statOpen >= 0) {
        lv_obj_add_flag(statOv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(statBody);
        statOpen = -1;
    }
    tab = t;
    for (int i = 0; i < T_COUNT; i++) {
        if (i == t) {
            lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(navBtn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_state(navBtn[i], LV_STATE_CHECKED);
        }
    }
    switch (t) {
    case T_HOME:
        refreshHome();
        break;
    case T_MAP:
        refreshMapStrip();
        lv_obj_invalidate(mapObj);
        break;
    case T_LIVE:
        refreshLive();
        break;
    case T_NODES:
        refreshNodes();
        break;
    case T_CHATS:
        refreshChats();
        break;
    case T_MORE:
        openMore(-1); // More opens on its six categories
        refreshSettings();
        break;
    }
    lv_obj_scroll_to_y(pages[t], 0, LV_ANIM_OFF);
    refocus();
    if (railFocus)
        setRail(true); // the cursor stays on the menu, now on this tab
}

lv_obj_t *App::topOverlay(void) const
{
    if (askBox && !lv_obj_has_flag(askBox, LV_OBJ_FLAG_HIDDEN))
        return askBox;
    if (replyBox && !lv_obj_has_flag(replyBox, LV_OBJ_FLAG_HIDDEN))
        return replyBox;
    if (chatOpen)
        return chatOv;
    if (rm)
        return rmOv;
    if (nodeOv && !lv_obj_has_flag(nodeOv, LV_OBJ_FLAG_HIDDEN))
        return nodeOv;
    if (statOpen >= 0)
        return statOv;
    return nullptr;
}

namespace
{
void collect(lv_obj_t *o, std::vector<lv_obj_t *> &out)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return;
    if (lv_obj_has_flag(o, F_STOP))
        out.push_back(o);
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        collect(lv_obj_get_child(o, i), out);
}
} // namespace

// the focus ring follows the keys: only the controls on the page (or overlay) on top take part
void App::refocus(lv_obj_t *prefer)
{
    std::vector<lv_obj_t *> stops;
    lv_obj_t *ov = topOverlay();
    collect(ov ? ov : pages[tab], stops);
    if (kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN) &&
        std::find(stops.begin(), stops.end(), lv_keyboard_get_textarea(kb)) == stops.end())
        kbHide(); // its field went with the page or overlay that closed
    lv_group_focus_freeze(group, true); // adding must not focus (and scroll to) anything
    lv_group_remove_all_objs(group);
    for (auto *o : stops)
        lv_group_add_obj(group, o);
    lv_group_focus_freeze(group, false);
    lv_obj_t *target = prefer && std::find(stops.begin(), stops.end(), prefer) != stops.end() ? prefer : (stops.empty() ? nullptr : stops[0]);
    if (!target)
        return;
    if (keysUsed || prefer) {
        lv_group_focus_obj(target);
        if (keysUsed)
            lv_obj_add_state(target, LV_STATE_FOCUS_KEY);
    } else
        lv_group_focus_freeze(group, false);
}

// ================================================================= map
namespace
{
// a link between two hops the mesh has used: learned from the paths of the packets heard
struct MapLink {
    uint8_t a[6], b[6]; // contact key prefixes; b all zero = this radio
    bool toMe = false;
    uint16_t n = 0;     // packets seen over it
    uint32_t last = 0;  // lv tick
    int8_t snr4 = 0;    // last packet's SNR (links to this radio)
};
// one packet on its way across the map
struct Flight {
    int32_t lat[10], lon[10];
    uint8_t n = 0;
    uint32_t start = 0; // lv tick
    lv_color_t color;
};
// a node a packet passed through recently (glow, "active" count)
struct Hot {
    uint8_t key[6];
    uint32_t last;
};
std::vector<MapLink> mapLinks;
std::vector<Hot> mapHot;
Flight flights[8];
int flightNext = 0;
constexpr uint32_t SEG_MS = 320, FADE_MS = 1400, RIPPLE_MS = 700;
bool mapDragged = false;

bool zeroKey(const uint8_t *k)
{
    for (int i = 0; i < 6; i++)
        if (k[i])
            return false;
    return true;
}

void markHot(const uint8_t key[6])
{
    uint32_t t = lv_tick_get();
    for (auto &h : mapHot)
        if (!memcmp(h.key, key, 6)) {
            h.last = t;
            return;
        }
    if (mapHot.size() >= 64) // drop the stalest
        mapHot.erase(std::min_element(mapHot.begin(), mapHot.end(), [](const Hot &a, const Hot &b) { return a.last < b.last; }));
    Hot h;
    memcpy(h.key, key, 6);
    h.last = t;
    mapHot.push_back(h);
}

uint32_t hotAge(const uint8_t key[6]) // ms since a packet went through it, or ~0
{
    for (auto &h : mapHot)
        if (!memcmp(h.key, key, 6))
            return lv_tick_get() - h.last;
    return 0xffffffff;
}

void learnLink(const uint8_t a[6], const uint8_t *b, int8_t snr4)
{
    static const uint8_t me[6] = {};
    const uint8_t *bb = b ? b : me;
    for (auto &l : mapLinks)
        if ((!memcmp(l.a, a, 6) && !memcmp(l.b, bb, 6)) || (b && !memcmp(l.a, bb, 6) && !memcmp(l.b, a, 6))) {
            l.n++;
            l.last = lv_tick_get();
            if (!b)
                l.snr4 = snr4;
            return;
        }
    if (mapLinks.size() >= 160)
        mapLinks.erase(std::min_element(mapLinks.begin(), mapLinks.end(), [](const MapLink &x, const MapLink &y) { return x.last < y.last; }));
    MapLink l;
    memcpy(l.a, a, 6);
    memcpy(l.b, bb, 6);
    l.toMe = !b;
    l.n = 1;
    l.last = lv_tick_get();
    l.snr4 = snr4;
    mapLinks.push_back(l);
}

// a "nice" round distance near x km: 1, 2 or 5 times a power of ten
float niceKm(float x)
{
    float p = powf(10.0f, floorf(log10f(std::max(x, 0.01f))));
    float m = x / p;
    return (m < 1.5f ? 1 : m < 3.5f ? 2 : m < 7.5f ? 5 : 10) * p;
}
std::string kmText(float km)
{
    char b[16];
    if (km < 1)
        snprintf(b, sizeof(b), "%d m", (int)(km * 1000 + 0.5f));
    else
        snprintf(b, sizeof(b), "%g km", km);
    return b;
}

void dot(lv_layer_t *layer, int32_t x, int32_t y, int32_t r, lv_color_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = c;
    d.bg_opa = opa;
    lv_area_t a = {x - r, y - r, x + r, y + r};
    lv_draw_rect(layer, &d, &a);
}
void ring(lv_layer_t *layer, int32_t x, int32_t y, int32_t r, int32_t w, lv_color_t c, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = x;
    d.center.y = y;
    d.radius = (uint16_t)std::max<int32_t>(1, r);
    d.width = w;
    d.start_angle = 0;
    d.end_angle = 360;
    d.color = c;
    d.opa = opa;
    lv_draw_arc(layer, &d);
}
void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w, lv_color_t c, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1, d.p1.y = y1, d.p2.x = x2, d.p2.y = y2;
    d.width = w;
    d.color = c;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}
void text(lv_layer_t *layer, const char *t, int32_t x, int32_t y, const lv_font_t *f, lv_color_t c, lv_opa_t opa,
          lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1; // copied: the text is on the stack
    d.font = f;
    d.color = c;
    d.opa = opa;
    d.align = align;
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t w = sz.x + 2;
    lv_area_t a;
    a.y1 = y, a.y2 = y + lv_font_get_line_height(f);
    if (align == LV_TEXT_ALIGN_RIGHT)
        a.x1 = x - w, a.x2 = x;
    else if (align == LV_TEXT_ALIGN_CENTER)
        a.x1 = x - w / 2, a.x2 = x + w / 2;
    else
        a.x1 = x, a.x2 = x + w;
    lv_draw_label(layer, &d, &a);
}
// a darkened version of the page colour, for the map's background
lv_color_t mapBg(void) { return lv_color_mix(lv_color_black(), C_BG, 60); }
} // namespace

void App::buildMap(lv_obj_t *p)
{
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(p, LV_LAYOUT_NONE);
    mapObj = plain(p);
    lv_obj_set_size(mapObj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(mapObj, mapBg(), 0);
    lv_obj_set_style_bg_opa(mapObj, LV_OPA_COVER, 0);
    lv_obj_add_flag(mapObj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mapObj, [](lv_event_t *e) { self->mapDraw(e); }, LV_EVENT_DRAW_MAIN_END, nullptr);
    // touch: drag pans, a tap on a dot opens that node
    lv_obj_add_event_cb(
        mapObj,
        [](lv_event_t *) {
            lv_indev_t *in = lv_indev_active();
            if (!in)
                return;
            lv_point_t v;
            lv_indev_get_vect(in, &v);
            if (!v.x && !v.y)
                return;
            mapDragged = true;
            self->mapCX -= v.x;
            self->mapCY -= v.y;
            self->mapScaleChanged();
            lv_obj_invalidate(self->mapObj);
        },
        LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(mapObj, [](lv_event_t *) { mapDragged = false; }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(
        mapObj,
        [](lv_event_t *) {
            if (mapDragged)
                return;
            lv_indev_t *in = lv_indev_active();
            if (!in)
                return;
            lv_point_t pt;
            lv_indev_get_point(in, &pt);
            self->mapTap(pt);
        },
        LV_EVENT_SHORT_CLICKED, nullptr);

    // zoom buttons for touch (the keys use Up / Down / OK)
    static const char *zl[3] = {"+", "-", LV_SYMBOL_GPS};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 28, 28);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_color(b, C_CARD, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(b, C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, C_LINE, 0);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -6, 6 + i * 33);
        lv_obj_t *l = label(b, i == 2 ? F12 : F16, C_TEXT, zl[i]);
        lv_obj_center(l);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                if (i == 2)
                    self->mapCentre();
                else
                    self->mapZoom(i == 0 ? 0.625f : 1.6f);
            },
            LV_EVENT_CLICKED, nullptr);
    }
    // the strip along the bottom: how busy the mesh is, and the last route
    mapStrip = label(p, F10, C_TEXT, "");
    lv_obj_set_width(mapStrip, LV_PCT(100));
    lv_label_set_long_mode(mapStrip, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(mapStrip, lv_font_get_line_height(F10) + 6); // one line: the end of a long route is cut with "..."
    lv_obj_set_style_bg_color(mapStrip, C_BAR, 0);
    lv_obj_set_style_bg_opa(mapStrip, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(mapStrip, 6, 0);
    lv_obj_set_style_pad_ver(mapStrip, 3, 0);
    lv_obj_align(mapStrip, LV_ALIGN_BOTTOM_MID, 0, 0);
    mapEmpty = label(p, F12, C_MUTED,
                     "No positions yet.\nNodes appear here when their adverts carry a location. Set yours in the phone app, or let "
                     "the GPS find it.");
    lv_obj_set_width(mapEmpty, LV_PCT(80));
    lv_label_set_long_mode(mapEmpty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(mapEmpty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(mapEmpty);
    lv_obj_add_flag(mapEmpty, LV_OBJ_FLAG_HIDDEN);

    if (!mapTimer) // ~30 frames a second while something is moving on the map, nothing otherwise
        mapTimer = lv_timer_create(
            [](lv_timer_t *) {
                if (self->tab != T_MAP || !self->awake || self->topOverlay() || !self->mapObj)
                    return;
                uint32_t t = lv_tick_get();
                for (auto &f : flights)
                    if (f.n && t - f.start < (f.n - 1) * SEG_MS + FADE_MS) {
                        lv_obj_invalidate(self->mapObj);
                        return;
                    }
            },
            33, nullptr);
}

// screen position of a place, relative to the map's top-left corner
namespace
{
constexpr int MAP_MIN_Z = 3, MAP_MAX_Z = 17;
// Web Mercator: a position's world pixel at zoom z (the world is 256 << z pixels across)
void geoToWorld(double lat, double lon, int z, double &x, double &y)
{
    const double n = 256.0 * (double)(1u << z);
    lat = std::max(-85.0511, std::min(85.0511, lat));
    const double s = sin(lat * M_PI / 180);
    x = (lon + 180.0) / 360.0 * n;
    y = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}
double worldToLat(double y, int z)
{
    const double n = 256.0 * (double)(1u << z);
    return atan(sinh(M_PI * (1 - 2 * y / n))) * 180 / M_PI;
}
} // namespace

bool App::mapXY(int32_t lat, int32_t lon, int32_t &x, int32_t &y) const
{
    if (!mapObj)
        return false;
    const int32_t w = lv_obj_get_width(mapObj), h = lv_obj_get_height(mapObj);
    double wx, wy;
    geoToWorld(lat / 1e6, lon / 1e6, mapZ, wx, wy);
    const double dx = wx - mapCX, dy = wy - mapCY;
    if (fabs(dx) > 4000 || fabs(dy) > 4000)
        return false;
    x = w / 2 + (int32_t)lround(dx);
    y = h / 2 + (int32_t)lround(dy);
    return true;
}

// km per pixel at the view's centre (rings, scale bar, labels)
void App::mapScaleChanged(void)
{
    mapKmPx = (float)(156.543034 * cos(worldToLat(mapCY, mapZ) * M_PI / 180) / (double)(1u << mapZ));
}

bool App::mapTiles(void) const { return m.uiPref("maptiles", 1) != 0; }

// centre on this radio (or the middle of the mesh) and zoom so the nodes heard lately fit
void App::mapFit(void)
{
    lv_obj_update_layout(mapObj);
    const int32_t w = lv_obj_get_width(mapObj), h = lv_obj_get_height(mapObj);
    const uint32_t now = m.now();
    std::vector<std::pair<int32_t, int32_t>> pts;
    Contact c;
    for (size_t i = 0; i < m.contactCount(); i++)
        if (m.contactAt(i, c) && c.hasPos && (!now || !c.lastHeard || now - c.lastHeard < 7 * 86400))
            pts.push_back({c.lat, c.lon});
    int32_t la, lo;
    const bool me = m.gpsFix(la, lo);
    if (pts.empty() && !me) {
        mapFitted = false;
        return;
    }
    if (!me) { // the middle: the median, so one far-off node doesn't drag the view away
        std::vector<int32_t> a, b;
        for (auto &p : pts)
            a.push_back(p.first), b.push_back(p.second);
        std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
        std::nth_element(b.begin(), b.begin() + b.size() / 2, b.end());
        la = a[a.size() / 2], lo = b[b.size() / 2];
    }
    const double cosl = cos(la / 1e6 * M_PI / 180);
    std::vector<double> d;
    for (auto &p : pts) {
        double dx = (p.second - lo) / 1e6 * 111.32 * cosl, dy = (p.first - la) / 1e6 * 110.57;
        double k = sqrt(dx * dx + dy * dy);
        if (k < 250) // further than this is another mesh (or a wrong position): left out of the fit
            d.push_back(k);
    }
    double reach = 2;
    if (!d.empty()) {
        std::sort(d.begin(), d.end());
        reach = std::max(1.0, d[std::min<size_t>(d.size() - 1, (size_t)(d.size() * 0.9))]); // 90%: room for the bulk
    }
    // the closest whole zoom level that still fits that reach (the street map's tiles come in whole levels)
    const double radiusPx = std::max<int32_t>(20, std::min(w, h) / 2 - 14);
    const double z = log2(radiusPx * 156.543034 * std::max(0.05, cosl) / (reach * 1.08));
    mapZ = std::max(MAP_MIN_Z, std::min(MAP_MAX_Z, (int)floor(z)));
    geoToWorld(la / 1e6, lo / 1e6, mapZ, mapCX, mapCY);
    mapScaleChanged();
    mapFitted = true;
    lv_obj_invalidate(mapObj);
}

// the centre button: on this radio when it knows where it is; otherwise it says so (and shows the middle of the mesh)
void App::mapCentre(void)
{
    int32_t la, lo;
    if (!m.gpsFix(la, lo))
        toast(LV_SYMBOL_GPS " No position yet: the GPS has no fix (it needs open sky). Showing the middle of the mesh.");
    mapFit();
}

// one zoom level in (factor < 1) or out, about the centre
void App::mapZoom(float factor)
{
    const int z = std::max(MAP_MIN_Z, std::min(MAP_MAX_Z, mapZ + (factor < 1 ? 1 : -1)));
    if (z == mapZ)
        return;
    const double k = z > mapZ ? 2.0 : 0.5;
    mapCX *= k;
    mapCY *= k;
    mapZ = z;
    mapScaleChanged();
    lv_obj_invalidate(mapObj);
}

// a heard (or sent) packet: learn its links, and send a flight along its route
void App::mapPacket(const Packet &pk)
{
    Contact c;
    // resolve each path hash to the most recently heard contact whose key starts with it
    auto byHash = [&](const uint8_t *h, int hs, Contact &out) {
        bool found = false;
        uint32_t best = 0;
        Contact x;
        for (size_t i = 0; i < m.contactCount(); i++)
            if (m.contactAt(i, x) && !memcmp(x.key, h, hs) && (!found || x.lastHeard > best)) {
                out = x, best = x.lastHeard, found = true;
            }
        return found;
    };
    Flight f;
    f.color = pktColor(pk.type);
    auto add = [&](int32_t la, int32_t lo) {
        if (f.n < 10 && (f.n == 0 || f.lat[f.n - 1] != la || f.lon[f.n - 1] != lo))
            f.lat[f.n] = la, f.lon[f.n] = lo, f.n++;
    };
    int32_t myLa, myLo;
    const bool me = m.gpsFix(myLa, myLo);
    if (pk.mine) {
        if (me)
            add(myLa, myLo);
    } else if (pk.hasFromKey && m.findContact(pk.fromKey, c) && c.hasPos) {
        add(c.lat, c.lon);
        markHot(c.key);
    } else if (pk.hasPos)
        add(pk.lat, pk.lon);
    const int hs = std::max<int>(1, pk.hashSize);
    const int hops = std::min<int>(pk.pathLen, (int)sizeof(pk.path) / hs);
    Contact prev;
    bool havePrev = false;
    std::string route;
    for (int h = 0; h < hops; h++) {
        Contact r;
        if (!byHash(pk.path + h * hs, hs, r)) {
            havePrev = false;
            continue;
        }
        markHot(r.key);
        if (havePrev)
            learnLink(prev.key, r.key, 0);
        prev = r, havePrev = true;
        if (r.hasPos)
            add(r.lat, r.lon);
        if (!route.empty())
            route += " > ";
        route += r.name[0] ? r.name : "?";
    }
    if (pk.viaNet) { // from the live feed: the route ends at the observer that heard it, not here
        Contact o;
        if (!zeroKey(pk.obsKey) && m.findContact(pk.obsKey, o)) {
            if (havePrev && hops == pk.pathLen)
                learnLink(prev.key, o.key, 0);
            markHot(o.key);
            if (o.hasPos)
                add(o.lat, o.lon);
        }
        std::string who = pk.mine ? std::string("You") : pk.from[0] ? std::string(pk.from) : std::string(pktTypeName(pk.type));
        mapLast = std::string(LV_SYMBOL_WIFI " ") + who + (route.empty() ? "" : " > " + route) + " > " + (pk.via[0] ? pk.via : "an observer");
    } else if (!pk.mine) {
        if (havePrev && hops == pk.pathLen)
            learnLink(prev.key, nullptr, pk.snr4); // the last hop is the one this radio heard
        if (me)
            add(myLa, myLo);
        char b[48];
        std::string who = pk.from[0] ? std::string(pk.from) : std::string(pktTypeName(pk.type));
        snprintf(b, sizeof(b), "  %d dBm %+.1f dB", pk.rssi, pk.snr4 / 4.0f);
        mapLast = who + (route.empty() ? (pk.pathLen ? "" : " (direct)") : " > " + route) + " > you" + b;
    }
    if (!f.n)
        return;
    f.start = lv_tick_get();
    flights[flightNext] = f;
    flightNext = (flightNext + 1) % 8;
    if (tab == T_MAP && mapObj)
        lv_obj_invalidate(mapObj);
}

void App::refreshMapStrip(void)
{
    if (!mapStrip)
        return;
    unsigned active = 0;
    Contact c;
    for (auto &h : mapHot)
        if (lv_tick_get() - h.last < 15 * 60 * 1000 && m.findContact(h.key, c) && c.type == NodeType::Repeater)
            active++;
    char b[160];
    snprintf(b, sizeof(b), "%u/min  -  %u repeater%s active  -  %s", (unsigned)perMin[29], active, active == 1 ? "" : "s",
             mapLast.empty() ? "waiting for a packet" : mapLast.c_str());
    setText(mapStrip, b);
    // nothing to place yet?
    bool any = false;
    int32_t la, lo;
    if (m.gpsFix(la, lo))
        any = true;
    for (size_t i = 0; !any && i < m.contactCount(); i++)
        any = m.contactAt(i, c) && c.hasPos;
    if (any)
        lv_obj_add_flag(mapEmpty, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(mapEmpty, LV_OBJ_FLAG_HIDDEN);
    if (any && !mapFitted)
        mapFit();
}

void App::mapTap(lv_point_t pt)
{
    lv_area_t a;
    lv_obj_get_coords(mapObj, &a);
    int32_t best = 18 * 18;
    bool found = false;
    uint8_t key[6];
    Contact c;
    for (size_t i = 0; i < m.contactCount(); i++) {
        int32_t x, y;
        if (!m.contactAt(i, c) || !c.hasPos || !mapXY(c.lat, c.lon, x, y))
            continue;
        int32_t dx = a.x1 + x - pt.x, dy = a.y1 + y - pt.y;
        if (dx * dx + dy * dy < best)
            best = dx * dx + dy * dy, found = true, memcpy(key, c.key, 6);
    }
    if (found)
        openNode(key);
}

void App::mapDraw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(mapObj, &a);
    const int32_t ox = a.x1, oy = a.y1, w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    const uint32_t t = lv_tick_get(), now = m.now();
    int32_t myLa, myLo, mx = w / 2, my = h / 2;
    const bool me = m.gpsFix(myLa, myLo) && mapXY(myLa, myLo, mx, my);
    if (!mapFitted)
        return;

    // the street map underneath, when it's on: the tile engine fills a buffer the size of the view (hx_tiles.cpp)
    if (mapTiles()) {
        hoptics::TileMap &tm = hoptics::TileMap::get();
        if (uint16_t *buf = tm.buffer((int16_t)w, (int16_t)h)) {
            static lv_image_dsc_t img;
            if (img.data != (const uint8_t *)buf || (int32_t)img.header.w != w || (int32_t)img.header.h != h) {
                std::fill_n(buf, (size_t)w * h, lv_color_to_u16(mapBg())); // a new buffer: blank until tiles arrive
                img.header.magic = LV_IMAGE_HEADER_MAGIC;
                img.header.cf = LV_COLOR_FORMAT_RGB565;
                img.header.flags = 0;
                img.header.w = (uint32_t)w;
                img.header.h = (uint32_t)h;
                img.header.stride = (uint32_t)w * 2;
                img.data_size = (uint32_t)w * h * 2;
                img.data = (const uint8_t *)buf;
            }
            hoptics::MapView v;
            v.z = (uint8_t)mapZ;
            v.ox = (int32_t)lround(mapCX) - w / 2;
            v.oy = (int32_t)lround(mapCY) - h / 2;
            v.w = (int16_t)w;
            v.h = (int16_t)h;
            const int th = m.uiPref("theme", 0); // the Glass family (4 and up) keeps the dark tiles
            static const hoptics::MapStyle styles[4] = {hoptics::MapStyle::Dark, hoptics::MapStyle::Light,
                                                        hoptics::MapStyle::Red, hoptics::MapStyle::Amber};
            v.style = th >= 0 && th < 4 ? styles[th] : hoptics::MapStyle::Dark;
            v.bg = lv_color_to_u16(mapBg());
            tm.request(v); // nothing to do when the view is unchanged
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = &img;
            lv_area_t ia = {ox, oy, ox + w - 1, oy + h - 1};
            lv_draw_image(layer, &d, &ia);
        }
    }

    // range rings around this radio (or the middle), every "nice" distance
    const float step = niceKm(mapKmPx * std::min(w, h) / 2 / 3);
    const int32_t rpx = (int32_t)(step / mapKmPx);
    if (rpx >= 12)
        for (int i = 1; i <= 8; i++) {
            int32_t r = rpx * i;
            if (r > w + h)
                break;
            ring(layer, ox + mx, oy + my, r, 1, C_LINE, LV_OPA_70);
            text(layer, kmText(step * i).c_str(), ox + mx + (int32_t)(r * 0.707f) + 2, oy + my - (int32_t)(r * 0.707f) - 12, F10, C_DIM, LV_OPA_60);
        }
    line(layer, ox + mx, oy, ox + mx, oy + h, 1, C_LINE, LV_OPA_30);
    line(layer, ox, oy + my, ox + w, oy + my, 1, C_LINE, LV_OPA_30);

    // gather the placed nodes once
    struct P {
        int32_t x, y;
        Contact c;
    };
    std::vector<P> placed;
    Contact c;
    for (size_t i = 0; i < m.contactCount(); i++)
        if (m.contactAt(i, c) && c.hasPos) {
            P p;
            if (mapXY(c.lat, c.lon, p.x, p.y) && p.x > -40 && p.y > -40 && p.x < w + 40 && p.y < h + 40) {
                p.c = c;
                placed.push_back(p);
            }
        }
    auto where = [&](const uint8_t key[6], int32_t &x, int32_t &y) {
        for (auto &p : placed)
            if (!memcmp(p.c.key, key, 6)) {
                x = p.x, y = p.y;
                return true;
            }
        return false;
    };

    // links the mesh has used: thicker = busier, links to this radio coloured by signal
    for (auto &l : mapLinks) {
        int32_t x1, y1, x2 = mx, y2 = my;
        if (!where(l.a, x1, y1))
            continue;
        if (l.toMe ? !me : !where(l.b, x2, y2))
            continue;
        uint32_t age = t - l.last;
        lv_opa_t opa = age < 5 * 60000 ? LV_OPA_70 : age < 30 * 60000 ? LV_OPA_40 : LV_OPA_20;
        lv_color_t col = l.toMe ? (l.snr4 >= 20 ? C_GREEN : l.snr4 >= -20 ? C_AMBER : C_RED) : C_CYAN;
        line(layer, ox + x1, oy + y1, ox + x2, oy + y2, l.n >= 20 ? 3 : l.n >= 5 ? 2 : 1, col, opa);
    }

    // packets in flight: the route so far fades, the head is a bright comet, a ripple where it lands
    for (auto &f : flights) {
        if (!f.n)
            continue;
        uint32_t el = t - f.start, travel = (f.n - 1) * SEG_MS;
        if (el >= travel + FADE_MS) {
            f.n = 0;
            continue;
        }
        int32_t px[10], py[10];
        bool ok = true;
        for (int i = 0; i < f.n && ok; i++)
            ok = mapXY(f.lat[i], f.lon[i], px[i], py[i]);
        if (!ok)
            continue;
        uint32_t seg = std::min<uint32_t>(el / SEG_MS, f.n - 1);
        for (uint32_t i = 0; i < seg && i + 1 < f.n; i++) {
            uint32_t since = el - (i + 1) * SEG_MS; // ms since this leg was finished
            lv_opa_t o = (lv_opa_t)(since >= FADE_MS ? 0 : 200 - 200 * since / FADE_MS);
            if (o > 8)
                line(layer, ox + px[i], oy + py[i], ox + px[i + 1], oy + py[i + 1], 2, f.color, o);
        }
        if (seg + 1 < f.n) {
            float fr = (el % SEG_MS) / (float)SEG_MS;
            int32_t hx = px[seg] + (int32_t)((px[seg + 1] - px[seg]) * fr), hy = py[seg] + (int32_t)((py[seg + 1] - py[seg]) * fr);
            // the tail: three steps back along the leg, fading
            for (int k = 3; k >= 1; k--) {
                float tf = std::max(0.0f, fr - 0.12f * k);
                int32_t tx = px[seg] + (int32_t)((px[seg + 1] - px[seg]) * tf), ty = py[seg] + (int32_t)((py[seg + 1] - py[seg]) * tf);
                line(layer, ox + tx, oy + ty, ox + hx, oy + hy, 2 + (3 - k), f.color, (lv_opa_t)(90 + 40 * (3 - k)));
            }
            line(layer, ox + px[seg], oy + py[seg], ox + hx, oy + hy, 2, f.color, LV_OPA_60);
            dot(layer, ox + hx, oy + hy, 7, f.color, LV_OPA_30);
            dot(layer, ox + hx, oy + hy, 3, lv_color_white(), LV_OPA_COVER);
        } else {
            uint32_t r = el - travel;
            if (r < RIPPLE_MS)
                ring(layer, ox + px[f.n - 1], oy + py[f.n - 1], 4 + (int32_t)(r * 18 / RIPPLE_MS), 2, f.color,
                     (lv_opa_t)(220 - 220 * r / RIPPLE_MS));
        }
    }

    // the nodes: colour by type, faded by how long ago they were heard; a glow where a packet just passed
    int labelled = 0;
    for (auto &p : placed)
        if (p.c.type == NodeType::Repeater || p.c.type == NodeType::Room)
            labelled++;
    const bool allLabels = labelled <= 12 && mapKmPx < 1.0f;
    struct Lab {
        int32_t x, y;
        uint32_t hot;
        lv_opa_t o;
        const char *name;
    };
    std::vector<Lab> labs;
    for (auto &p : placed) {
        uint32_t ago = now && p.c.lastHeard && now >= p.c.lastHeard ? now - p.c.lastHeard : 86400;
        lv_opa_t o = ago < 900 ? LV_OPA_COVER : ago < 3600 ? LV_OPA_80 : ago < 86400 ? LV_OPA_50 : LV_OPA_30;
        lv_color_t col = typeColor(p.c.type);
        const bool rep = p.c.type == NodeType::Repeater || p.c.type == NodeType::Room;
        uint32_t hot = hotAge(p.c.key);
        if (hot < 8000) {
            dot(layer, ox + p.x, oy + p.y, 10, col, (lv_opa_t)(70 - 70 * hot / 8000));
            o = LV_OPA_COVER;
        }
        dot(layer, ox + p.x, oy + p.y, rep ? 5 : 4, mapBg(), LV_OPA_COVER); // a dark rim keeps dots apart
        dot(layer, ox + p.x, oy + p.y, rep ? 4 : 3, col, o);
        if (rep && (allLabels || hot < 20000))
            labs.push_back({p.x, p.y, hot, o, p.c.name});
    }
    // names: the ones a packet just went through first, and none drawn over another (or over "you", "N", the buttons)
    std::sort(labs.begin(), labs.end(), [](const Lab &a, const Lab &b) { return a.hot < b.hot; });
    std::vector<lv_area_t> taken;
    taken.push_back({0, 0, 24, 34});      // N
    taken.push_back({w - 40, 0, w, 106}); // zoom buttons
    if (me)
        taken.push_back({mx + 6, my, mx + 30, my + 14});
    const int32_t lh = lv_font_get_line_height(F10);
    for (auto &l : labs) {
        char nm[18];
        snprintf(nm, sizeof(nm), "%s", l.name);
        lv_point_t sz;
        lv_text_get_size(&sz, nm, F10, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        // right of the dot, else left of it
        lv_area_t r = {l.x + 7, l.y - 7, l.x + 7 + sz.x, l.y - 7 + lh};
        auto clash = [&](const lv_area_t &q) {
            for (auto &t2 : taken)
                if (q.x1 < t2.x2 && q.x2 > t2.x1 && q.y1 < t2.y2 && q.y2 > t2.y1)
                    return true;
            return false;
        };
        if (clash(r))
            r = {l.x - 7 - sz.x, l.y - 7, l.x - 7, l.y - 7 + lh};
        if (clash(r))
            continue;
        taken.push_back(r);
        text(layer, nm, ox + r.x1, oy + r.y1, F10, l.hot < 20000 ? C_TEXT : C_MUTED, l.o);
    }

    // this radio
    if (me) {
        dot(layer, ox + mx, oy + my, 9, C_CYAN, LV_OPA_20);
        dot(layer, ox + mx, oy + my, 5, lv_color_white(), LV_OPA_COVER);
        dot(layer, ox + mx, oy + my, 4, C_CYAN, LV_OPA_COVER);
        text(layer, "you", ox + mx + 8, oy + my + 2, F10, C_CYAN, LV_OPA_COVER);
    }

    // north and the scale bar
    text(layer, "N", ox + 8, oy + 4, F12, C_MUTED, LV_OPA_COVER);
    line(layer, ox + 12, oy + 20, ox + 12, oy + 30, 2, C_MUTED, LV_OPA_COVER);
    const float bar = niceKm(mapKmPx * 60);
    const int32_t bpx = (int32_t)(bar / mapKmPx), by = oy + h - lv_obj_get_height(mapStrip) - 10;
    line(layer, ox + 8, by, ox + 8 + bpx, by, 2, C_TEXT, LV_OPA_80);
    line(layer, ox + 8, by - 3, ox + 8, by + 3, 2, C_TEXT, LV_OPA_80);
    line(layer, ox + 8 + bpx, by - 3, ox + 8 + bpx, by + 3, 2, C_TEXT, LV_OPA_80);
    text(layer, kmText(bar).c_str(), ox + 12 + bpx, by - 7, F10, C_TEXT, LV_OPA_80);

    if (mapTiles()) {
        // the credit OpenStreetMap asks for, and what the street map is waiting for
        static const char *credit = "(c) OpenStreetMap";
        lv_point_t cs;
        lv_text_get_size(&cs, credit, F10, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        text(layer, credit, ox + w - cs.x - 6, by - 7, F10, C_TEXT, LV_OPA_70);
        const hoptics::MapStatus st = hoptics::TileMap::get().status();
        const char *note = nullptr;
        if (st.pending && st.problem == hoptics::MapStatus::BUSY)
            note = LV_SYMBOL_WARNING " Map server busy, trying again";
        else if (st.pending && st.problem == hoptics::MapStatus::NO_LINK)
            note = LV_SYMBOL_WARNING " Can't reach the map server";
        else if (st.pending && !st.online)
            note = LV_SYMBOL_WIFI " Street map: needs Wi-Fi for this area";
        else if (st.pending)
            note = LV_SYMBOL_DOWNLOAD " Loading the street map...";
        if (note)
            text(layer, note, ox + 24, oy + 6, F10, C_AMBER, LV_OPA_COVER);
    }
}

// ================================================================= stat details
namespace
{
std::vector<std::array<uint8_t, 6>> statKeys; // node rows on a stat detail -> contact key

// a label: value line inside a card
void kv(lv_obj_t *parent, const char *k, const char *v, lv_color_t vc)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    row(r, 6);
    lv_obj_t *kl = label(r, F12, C_MUTED, k);
    lv_obj_set_flex_grow(kl, 1);
    oneLine(kl);
    label(r, F12, vc, v);
}

// the headline number of a detail: big value, a unit and a line saying what it means
lv_obj_t *headline(lv_obj_t *parent, const char *value, const char *unit, const char *note, lv_color_t c)
{
    lv_obj_t *h = card(parent, nullptr);
    lv_obj_t *top = plain(h);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(top, 6, 0);
    label(top, F28, c, value);
    lv_obj_t *u = label(top, F14, C_MUTED, unit);
    lv_obj_set_style_pad_bottom(u, 4, 0);
    if (note && *note)
        wrap(label(h, F12, C_TEXT, note));
    stop(h);
    return h;
}

// "30 min ago ... now" under a 30-minute chart
void timeAxis(lv_obj_t *parent)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(r, F10, C_DIM, "30 min ago");
    label(r, F10, C_DIM, "now");
}

lv_obj_t *bigChart(lv_obj_t *parent, lv_chart_type_t type, int32_t h)
{
    lv_obj_t *ch = lv_chart_create(parent);
    lv_obj_set_size(ch, LV_PCT(100), h);
    lv_chart_set_type(ch, type);
    lv_chart_set_point_count(ch, 30);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_line_color(ch, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ch, 2, LV_PART_MAIN);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
    return ch;
}

double kmBetween(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2)
{
    double p1 = la1 / 1e6 * M_PI / 180, p2 = la2 / 1e6 * M_PI / 180, dl = (lo2 - lo1) / 1e6 * M_PI / 180;
    return 6371.0 * acos(std::min(1.0, sin(p1) * sin(p2) + cos(p1) * cos(p2) * cos(dl)));
}

const char *noiseRating(int16_t nf, lv_color_t &c, const char *&note)
{
    if (nf <= -115) {
        c = C_GREEN;
        note = "Very quiet. Weak, far-off packets have the best chance of getting through.";
        return "Very quiet";
    }
    if (nf <= -105) {
        c = C_GREEN;
        note = "Quiet. A normal reading for a radio away from other electronics.";
        return "Quiet";
    }
    if (nf <= -95) {
        c = C_AMBER;
        note = "Some noise. Weak packets may be missed; a charger, computer or LED light nearby can do this.";
        return "Some noise";
    }
    c = C_RED;
    note = "Noisy. Something close is drowning out weak signals: move the radio or its antenna away from other electronics.";
    return "Noisy";
}
} // namespace

void App::buildStatOverlay(void)
{
    statOv = overlay("", &statTitle, [](lv_event_t *) { self->closeStat(); });
    statBody = plain(statOv);
    lv_obj_set_width(statBody, LV_PCT(100));
    lv_obj_set_flex_grow(statBody, 1);
    lv_obj_add_flag(statBody, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(statBody, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(statBody, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(statBody, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(statBody, C_CYAN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(statBody, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_all(statBody, PAD, 0);
    column(statBody, PAD);
}

void App::openStat(int s)
{
    if (s < 0 || s >= S_COUNT)
        return;
    statOpen = s;
    static const char *titles[S_COUNT] = {"Heard in the last hour", "Repeaters", "Packets per minute", "Noise floor", "This radio"};
    lv_label_set_text(statTitle, titles[s]);
    lv_obj_remove_flag(statOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(statOv);
    lv_obj_move_foreground(toastBox);
    lv_obj_scroll_to_y(statBody, 0, LV_ANIM_OFF);
    renderStat();
}

void App::closeStat(void)
{
    if (statOpen < 0)
        return;
    lv_obj_add_flag(statOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(statBody); // the detail is rebuilt each time it opens: nothing kept while it is closed
    statKeys.clear();
    statOpen = -1;
    refreshHome();
    refocus();
}

// (re)draws the open detail from the current numbers; keeps the scroll position and the keys' place
void App::renderStat(void)
{
    if (statOpen < 0)
        return;
    statDrawn = lv_tick_get();
    int32_t sy = lv_obj_get_scroll_y(statBody);
    int focusAt = -1;
    {
        std::vector<lv_obj_t *> stops;
        collect(statOv, stops);
        lv_obj_t *f = lv_group_get_focused(group);
        for (size_t i = 0; i < stops.size(); i++)
            if (stops[i] == f)
                focusAt = (int)i;
    }
    const bool hadRing = focusAt >= 0 && lv_obj_has_state(lv_group_get_focused(group), LV_STATE_FOCUS_KEY);
    lv_obj_clean(statBody);
    statKeys.clear();
    char b[256], v[40];
    const uint32_t now = m.now();
    const Stats st = m.stats();
    int32_t myLa = 0, myLo = 0;
    const bool haveFix = m.gpsFix(myLa, myLo);

    // a contact row that opens the node (same look as the Nodes tab), with distance when both ends have a position
    auto nodeRow = [&](const Contact &n) {
        lv_obj_t *r = plain(statBody);
        lv_obj_set_size(r, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(r, C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(r, 8, 0);
        rowEdge(r);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *bd = badge(r, n.key, n.name, 28);
        lv_obj_align(bd, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_t *nm = label(r, F14, C_TEXT, n.name[0] ? n.name : "(no name)");
        lv_obj_set_pos(nm, 42, 3);
        lv_obj_set_width(nm, W() - RAIL_W - 2 * PAD - 42 - 62);
        oneLine(nm);
        lv_obj_t *tp = pill(r, nodeTypeName(n.type), typeColor(n.type));
        lv_obj_set_pos(tp, 42, 22);
        lv_obj_t *rt = label(r, F10, C_MUTED, route(n.pathLen).c_str());
        lv_obj_align_to(rt, tp, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        std::string right = ago(now, n.lastHeard ? n.lastHeard : n.lastAdvert) + " ago";
        if (haveFix && n.hasPos) {
            snprintf(b, sizeof(b), "\n%.1f km", kmBetween(myLa, myLo, n.lat, n.lon));
            right += b;
        }
        lv_obj_t *ag = label(r, F12, C_MUTED, right.c_str());
        lv_obj_set_style_text_align(ag, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(ag, LV_ALIGN_RIGHT_MID, -8, 0);
        statKeys.push_back({});
        memcpy(statKeys.back().data(), n.key, 6);
        lv_obj_set_user_data(r, (void *)(intptr_t)(statKeys.size() - 1));
        lv_obj_add_event_cb(
            r,
            [](lv_event_t *e) {
                size_t i = (size_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                if (i < statKeys.size())
                    self->openNode(statKeys[i].data());
            },
            LV_EVENT_CLICKED, nullptr);
        stop(r);
    };
    auto byHeard = [](const Contact &a, const Contact &c) {
        return std::max(a.lastHeard, a.lastAdvert) > std::max(c.lastHeard, c.lastAdvert);
    };
    auto empty = [&](const char *text) {
        lv_obj_t *l = label(statBody, F14, C_MUTED, text);
        wrap(l);
        lv_obj_set_style_pad_top(l, 16, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    };

    switch (statOpen) {
    case S_HEARD: {
        std::vector<Contact> list;
        unsigned byType[5] = {};
        Contact c;
        for (size_t i = 0; i < m.contactCount(); i++)
            if (m.contactAt(i, c) && now && c.lastHeard && now - c.lastHeard <= 3600) {
                list.push_back(c);
                byType[std::min<int>((int)c.type, 4)]++;
            }
        std::sort(list.begin(), list.end(), byHeard);
        snprintf(v, sizeof(v), "%u", (unsigned)list.size());
        snprintf(b, sizeof(b), "of %u known", (unsigned)m.contactCount());
        lv_obj_t *h = headline(statBody, v, b, nullptr, C_CYAN);
        snprintf(b, sizeof(b), "%u chat  -  %u repeater%s  -  %u room%s  -  %u sensor%s", byType[1], byType[2], byType[2] == 1 ? "" : "s",
                 byType[3], byType[3] == 1 ? "" : "s", byType[4], byType[4] == 1 ? "" : "s");
        wrap(label(h, F12, C_MUTED, b));
        if (!now)
            empty("The clock isn't set yet, so \"the last hour\" can't be worked out. It sets itself from GPS or the phone app.");
        else if (list.empty())
            empty("Nobody heard in the last hour yet.\nNodes show up here as their adverts and messages arrive.");
        if (list.size() > 80)
            list.resize(80);
        for (auto &n : list)
            nodeRow(n);
        break;
    }
    case S_REPEATERS: {
        std::vector<Contact> list;
        unsigned h1 = 0, h24 = 0;
        double nearest = -1;
        Contact c, near;
        for (size_t i = 0; i < m.contactCount(); i++) {
            if (!m.contactAt(i, c) || c.type != NodeType::Repeater)
                continue;
            list.push_back(c);
            if (now && c.lastHeard && now - c.lastHeard <= 3600)
                h1++;
            if (now && c.lastHeard && now - c.lastHeard <= 86400)
                h24++;
            if (haveFix && c.hasPos) {
                double d = kmBetween(myLa, myLo, c.lat, c.lon);
                if (nearest < 0 || d < nearest)
                    nearest = d, near = c;
            }
        }
        std::sort(list.begin(), list.end(), byHeard);
        snprintf(v, sizeof(v), "%u", (unsigned)list.size());
        lv_obj_t *h = headline(statBody, v, "known", nullptr, C_GREEN);
        snprintf(b, sizeof(b), "%u heard in the last hour, %u in the last day", h1, h24);
        wrap(label(h, F12, C_TEXT, b));
        if (nearest >= 0) {
            snprintf(b, sizeof(b), "Nearest: %s, %.1f km", near.name, nearest);
            wrap(label(h, F12, C_MUTED, b));
        } else if (!haveFix)
            wrap(label(h, F12, C_MUTED, "Distances show once this radio has a GPS fix."));
        if (list.empty())
            empty("No repeaters known yet.\nThey appear as their adverts are heard.");
        if (list.size() > 80)
            list.resize(80);
        for (auto &n : list)
            nodeRow(n);
        break;
    }
    case S_PACKETS: {
        unsigned sum = 0, peak = 0;
        for (int i = 0; i < 30; i++)
            sum += perMin[i], peak = std::max<unsigned>(peak, perMin[i]);
        snprintf(v, sizeof(v), "%u", (unsigned)perMin[29]);
        lv_obj_t *h = headline(statBody, v, "this minute", nullptr, C_VIOLET);
        snprintf(b, sizeof(b), "Average %.1f a minute, busiest %u (last 30 minutes)", sum / 30.0, peak);
        wrap(label(h, F12, C_TEXT, b));
        lv_obj_t *ch = bigChart(h, LV_CHART_TYPE_BAR, 90);
        lv_chart_series_t *ser = lv_chart_add_series(ch, C_VIOLET, LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, std::max<unsigned>(4, peak));
        for (int i = 0; i < 30; i++)
            lv_chart_set_value_by_id(ch, ser, i, perMin[i]);
        lv_chart_refresh(ch);
        timeAxis(h);

        lv_obj_t *tot = card(statBody, "SINCE THE RADIO STARTED");
        snprintf(v, sizeof(v), "%u", (unsigned)st.rx);
        kv(tot, "Received", v, C_TEXT);
        if (st.rx) {
            snprintf(v, sizeof(v), "%u%% / %u%%", (unsigned)(st.rxFlood * 100 / st.rx), (unsigned)(st.rxDirect * 100 / st.rx));
            kv(tot, "Flood / direct", v, C_TEXT);
        }
        snprintf(v, sizeof(v), "%u", (unsigned)st.dups);
        kv(tot, "Duplicates (already heard)", v, C_TEXT);
        snprintf(v, sizeof(v), "%u", (unsigned)st.tx);
        kv(tot, "Sent", v, C_TEXT);
        if (st.uptimeS)
            snprintf(v, sizeof(v), "%.0f s (%.2f%%)", st.airtimeTxMs / 1000.0, st.airtimeTxMs / 10.0 / st.uptimeS);
        else
            snprintf(v, sizeof(v), "%.0f s", st.airtimeTxMs / 1000.0);
        kv(tot, "Time transmitting", v, C_TEXT);
        stop(tot);

        // what the last packets were
        unsigned cnt[(int)PktType::Other + 1] = {};
        int heard = 0;
        for (int i = 0; i < liveCount; i++) {
            const Packet &pk = live[(liveHead + kLive - 1 - i) % kLive];
            if (pk.mine)
                continue;
            cnt[std::min<int>((int)pk.type, (int)PktType::Other)]++;
            heard++;
        }
        if (heard) {
            snprintf(b, sizeof(b), "THE LAST %d PACKETS HEARD, BY TYPE", heard);
            lv_obj_t *ty = card(statBody, b);
            std::vector<std::pair<unsigned, int>> order;
            for (int t = 0; t <= (int)PktType::Other; t++)
                if (cnt[t])
                    order.push_back({cnt[t], t});
            std::sort(order.begin(), order.end(), [](const std::pair<unsigned, int> &a, const std::pair<unsigned, int> &c) { return a.first > c.first; });
            for (auto &o : order) {
                lv_obj_t *r = plain(ty);
                lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
                row(r, 6);
                pill(r, pktTypeName((PktType)o.second), pktColor((PktType)o.second));
                lv_obj_t *bar = plain(r);
                lv_obj_set_flex_grow(bar, 1);
                lv_obj_set_height(bar, 8);
                lv_obj_t *fill = plain(bar);
                lv_obj_set_size(fill, LV_PCT(std::max<int>(2, (int)(o.first * 100 / order[0].first))), 8);
                lv_obj_set_style_radius(fill, 4, 0);
                lv_obj_set_style_bg_color(fill, pktColor((PktType)o.second), 0);
                lv_obj_set_style_bg_opa(fill, LV_OPA_70, 0);
                snprintf(v, sizeof(v), "%u", o.first);
                label(r, F12, C_TEXT, v);
            }
            stop(ty);
        }
        break;
    }
    case S_NOISE: {
        lv_color_t c = C_MUTED;
        const char *note = "";
        if (st.noiseFloor) {
            const char *rating = noiseRating(st.noiseFloor, c, note);
            snprintf(v, sizeof(v), "%d", (int)st.noiseFloor);
            snprintf(b, sizeof(b), "dBm  -  %s", rating);
            headline(statBody, v, b, note, c);
        } else
            headline(statBody, "-", "dBm", "No reading yet: the radio measures the background between packets.", C_MUTED);

        int16_t lo = 0, hi = 0;
        int known = 0;
        for (int i = 0; i < 30; i++)
            if (noiseHist[i]) {
                lo = known ? std::min(lo, noiseHist[i]) : noiseHist[i];
                hi = known ? std::max(hi, noiseHist[i]) : noiseHist[i];
                known++;
            }
        if (known >= 2) {
            snprintf(b, sizeof(b), "LAST 30 MINUTES  -  %d to %d dBm", (int)lo, (int)hi);
            lv_obj_t *hc = card(statBody, b);
            lv_obj_t *ch = bigChart(hc, LV_CHART_TYPE_LINE, 70);
            lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR); // no dots on the line
            lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
            lv_chart_series_t *ser = lv_chart_add_series(ch, c, LV_CHART_AXIS_PRIMARY_Y);
            lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, lo - 3, hi + 3);
            for (int i = 0; i < 30; i++)
                lv_chart_set_value_by_id(ch, ser, i, noiseHist[i] ? noiseHist[i] : LV_CHART_POINT_NONE);
            lv_chart_refresh(ch);
            timeAxis(hc);
            stop(hc);
        }

        lv_obj_t *lp = card(statBody, "THE LAST PACKET HEARD");
        if (st.lastRssi) {
            snprintf(v, sizeof(v), "%d dBm", (int)st.lastRssi);
            kv(lp, "Signal (RSSI)", v, C_TEXT);
            snprintf(v, sizeof(v), "%+.1f dB", st.lastSnr4 / 4.0f);
            kv(lp, "Signal to noise (SNR)", v, st.lastSnr4 >= 20 ? C_GREEN : st.lastSnr4 >= -20 ? C_AMBER : C_RED);
            if (st.noiseFloor) {
                snprintf(v, sizeof(v), "%d dB", (int)(st.lastRssi - st.noiseFloor));
                kv(lp, "Above the noise floor", v, C_TEXT);
            }
        } else
            wrap(label(lp, F12, C_MUTED, "Nothing heard yet."));
        stop(lp);
        lv_obj_t *ex = card(statBody, "WHAT THIS IS");
        Radio r = m.radio();
        snprintf(b, sizeof(b), "The background radio noise on %.3f MHz when nobody is transmitting. Lower is better. At SF%u a packet still decodes "
                               "when it is a little below this, so a quiet spot hears much farther.",
                 r.freq, r.sf);
        wrap(label(ex, F12, C_TEXT, b));
        stop(ex);
        break;
    }
    case S_RADIO: {
        Radio r = m.radio();
        const char *preset = "custom";
        for (int i = 0; i < N_PRESETS; i++)
            if (fabsf(PRESETS[i].freq - r.freq) < 0.002f && fabsf(PRESETS[i].bw - r.bw) < 0.01f && PRESETS[i].sf == r.sf && PRESETS[i].cr == r.cr)
                preset = PRESETS[i].name;
        lv_obj_t *rc = card(statBody, "RADIO");
        wrap(label(rc, F14, C_CYAN, preset)); // its own line: preset names are long
        snprintf(v, sizeof(v), "%.3f MHz", r.freq);
        kv(rc, "Frequency", v, C_TEXT);
        snprintf(v, sizeof(v), "%g kHz", r.bw);
        kv(rc, "Bandwidth", v, C_TEXT);
        snprintf(v, sizeof(v), "SF%u", r.sf);
        kv(rc, "Spreading factor", v, C_TEXT);
        snprintf(v, sizeof(v), "4/%u", r.cr);
        kv(rc, "Coding rate", v, C_TEXT);
        snprintf(v, sizeof(v), "%d dBm", r.txPower);
        kv(rc, "Transmit power", v, C_TEXT);
        stop(rc);

        lv_obj_t *nc = card(statBody, "THIS NODE");
        static const char *roles[] = {"Companion", "Repeater", "Room server", "Observer", "Repeater + observer", "Field station"};
        kv(nc, "Role", roles[(int)m.role()], C_TEXT);
        kv(nc, "Board", m.boardName(), C_TEXT);
        kv(nc, "Firmware", m.firmwareVersion(), C_TEXT);
        uint32_t up = st.uptimeS;
        if (up >= 86400)
            snprintf(v, sizeof(v), "%ud %uh", (unsigned)(up / 86400), (unsigned)(up % 86400 / 3600));
        else
            snprintf(v, sizeof(v), "%uh %um", (unsigned)(up / 3600), (unsigned)(up % 3600 / 60));
        kv(nc, "Up", v, C_TEXT);
        int bp = m.batteryPercent();
        if (bp == 101)
            snprintf(v, sizeof(v), "USB power");
        else if (bp >= 0)
            snprintf(v, sizeof(v), "%d%%  %.2f V", bp, m.batteryMv() / 1000.0);
        else
            snprintf(v, sizeof(v), "-");
        kv(nc, "Battery", v, C_TEXT);
        kv(nc, "Bluetooth", m.bluetoothEnabled() ? (m.appConnected() ? "on, app connected" : "on") : "off", C_TEXT);
        kv(nc, "Repeats packets", m.repeatEnabled() ? "yes" : "no", C_TEXT);
        {
            bool on = false, fix = false;
            const int sats = m.gpsSatellites(on, fix);
            if (sats < 0)
                snprintf(v, sizeof(v), "none on this board");
            else if (!on)
                snprintf(v, sizeof(v), "off (More: GPS)");
            else if (!fix)
                snprintf(v, sizeof(v), "searching, %d satellite%s", sats, sats == 1 ? "" : "s");
            else
                snprintf(v, sizeof(v), "fix, %d satellites", sats);
            kv(nc, "GPS", v, fix ? C_GREEN : C_TEXT);
        }
        if (haveFix)
            snprintf(v, sizeof(v), "%.4f, %.4f", myLa / 1e6, myLo / 1e6);
        else
            snprintf(v, sizeof(v), "not known yet");
        kv(nc, "Position", v, C_TEXT);
        stop(nc);
        break;
    }
    }

    // put the reader back where they were
    lv_obj_update_layout(statBody);
    lv_obj_scroll_to_y(statBody, sy, LV_ANIM_OFF);
    std::vector<lv_obj_t *> stops;
    collect(statOv, stops);
    if (focusAt >= 0 && focusAt < (int)stops.size()) {
        refocus(stops[focusAt]);
        if (!hadRing)
            lv_obj_remove_state(stops[focusAt], LV_STATE_FOCUS_KEY);
    } else {
        // just opened: the keys start on the first thing in the detail, not its back arrow (Back closes it anyway)
        lv_obj_t *first = nullptr;
        for (auto *o : stops)
            if (lv_obj_get_parent(o) == statBody) {
                first = o;
                break;
            }
        refocus(first);
    }
}

// the keys' cursor on the menu, or back on the page
void App::setRail(bool on)
{
    railFocus = on;
    for (int i = 0; i < T_COUNT; i++)
        lv_obj_remove_state(navBtn[i], LV_STATE_FOCUS_KEY);
    if (on) {
        if (lv_obj_t *f = lv_group_get_focused(group))
            lv_obj_remove_state(f, LV_STATE_FOCUS_KEY);
        lv_obj_add_state(navBtn[tab], LV_STATE_FOCUS_KEY);
    }
}

// ---- on-screen keyboard (boards without a keyboard)
int32_t App::kbHeight(void) const { return H() * (LAND ? 46 : 42) / 100; }
bool App::kbFor(lv_obj_t *ta) const { return kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN) && lv_keyboard_get_textarea(kb) == ta; }

void App::kbShow(lv_obj_t *ta)
{
    if (!kb) {
        kb = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(kb, W(), kbHeight());
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(kb, C_BAR, 0);
        lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(kb, 1, 0);
        lv_obj_set_style_border_color(kb, C_LINE, 0);
        lv_obj_set_style_pad_all(kb, 3, 0);
        lv_obj_set_style_pad_gap(kb, 3, 0);
        lv_obj_set_style_bg_color(kb, C_CARD2, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(kb, C_CYAN, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(kb, C_TEXT, LV_PART_ITEMS);
        lv_obj_set_style_text_font(kb, F14, LV_PART_ITEMS);
        lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
        lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
        lv_obj_add_event_cb(
            kb,
            [](lv_event_t *e) {
                // OK and close: hide after the keyboard has passed the event on to the text field
                const lv_event_code_t c = lv_event_get_code(e);
                if (c == LV_EVENT_CANCEL || (c == LV_EVENT_READY && lv_keyboard_get_textarea(self->kb) != self->chatInput))
                    lv_async_call([](void *) { self->kbHide(); }, nullptr);
            },
            LV_EVENT_ALL, nullptr);
    }
    const char *acc = lv_textarea_get_accepted_chars(ta);
    lv_keyboard_set_mode(kb, acc && !strchr(acc, 'a') ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
    // lift what's on screen just enough that the field sits above the keyboard
    if (kbLifted)
        lv_obj_set_style_translate_y(kbLifted, 0, 0);
    kbLifted = topOverlay() ? topOverlay() : content;
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
    lv_obj_update_layout(scr);
    lv_area_t a;
    lv_obj_get_coords(ta, &a);
    const int32_t over = a.y2 + 4 - (H() - kbHeight()); // (its own height isn't laid out yet the first time)
    lv_obj_set_style_translate_y(kbLifted, over > 0 ? -over : 0, 0);
}

void App::kbHide(void)
{
    if (!kb)
        return;
    lv_keyboard_set_textarea(kb, nullptr);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (kbLifted)
        lv_obj_set_style_translate_y(kbLifted, 0, 0);
    kbLifted = nullptr;
}

// d-pad / trackball: the control nearest to `from` in a key's direction, among the ones the keys can reach now.
// Left / Right only go along the same row (something overlapping it vertically); Up / Down prefer the same column
// but take the closest thing below or above when nothing lines up. nullptr: nothing lies that way.
lv_obj_t *App::nearest(lv_obj_t *from, uint32_t k) const
{
    if (!from)
        return nullptr;
    lv_area_t a;
    lv_obj_get_coords(from, &a);
    const bool across = k == K_LEFT || k == K_RIGHT;
    const int32_t dir = (k == K_RIGHT || k == K_DOWN) ? 1 : -1;
    const int32_t ax = (a.x1 + a.x2) / 2, ay = (a.y1 + a.y2) / 2;
    lv_obj_t *best = nullptr;
    int64_t bestScore = 0;
    for (uint32_t i = 0; i < lv_group_get_obj_count(group); i++) {
        lv_obj_t *o = lv_group_get_obj_by_index(group, i);
        if (o == from)
            continue;
        lv_area_t b;
        lv_obj_get_coords(o, &b);
        const int32_t bx = (b.x1 + b.x2) / 2, by = (b.y1 + b.y2) / 2;
        if ((across ? bx - ax : by - ay) * dir <= 4)
            continue; // not that way
        const bool lined = across ? (b.y1 < a.y2 && b.y2 > a.y1) : (b.x1 < a.x2 && b.x2 > a.x1);
        if (across && !lined)
            continue;
        int32_t gap = across ? (dir > 0 ? b.x1 - a.x2 : a.x1 - b.x2) : (dir > 0 ? b.y1 - a.y2 : a.y1 - b.y2);
        if (gap < 0)
            gap = 0;
        const int32_t side = across ? std::abs(by - ay) : std::abs(bx - ax);
        const int64_t score = (int64_t)gap * 4 + side + (lined ? 0 : 100000);
        if (!best || score < bestScore) {
            best = o;
            bestScore = score;
        }
    }
    return best;
}

void App::goBack(void)
{
    if (kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN))
        return kbHide(); // Back closes the keyboard first
    if (topOverlay() == replyBox)
        closeReply();
    else if (chatOpen)
        closeChat();
    else if (rm)
        closeRemote();
    else if (topOverlay() == nodeOv)
        closeNode();
    else if (topOverlay() == statOv)
        closeStat();
    else if (topOverlay() == askBox)
        lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(askBox, 0), 2), 0), LV_EVENT_CLICKED, nullptr); // Cancel
    else if (tab == T_MORE && moreOpen >= 0)
        openMore(-1);
    else if (tab != T_HOME)
        selectTab(T_HOME);
}

// ================================================================= wallpaper and lock screen
// a boot picture mixed 75 % with the theme's background (once, into PSRAM), so text on the pages stays readable on
// every theme; drawn as the screen's background, so it costs a copy where the screen is redrawn, nothing more
const lv_image_dsc_t *App::wallpaper(void)
{
    const int pic = m.uiPref("wall", 0) - 1, theme = m.uiPref("theme", 0);
    int32_t w = 0, h = 0;
    const uint8_t *src = pic >= 0 ? bootArt(pic, w, h) : nullptr;
    if (!src || w != W() || h != H())
        return nullptr; // its buffer is freed where nothing shows it any more (the setting's change)
    if (wpBuf && wpPic == pic && wpTheme == theme)
        return &wpDsc;
    if (!wpBuf && !(wpBuf = (uint16_t *)malloc((size_t)w * h * 2)))
        return nullptr;
    const uint16_t bg = lv_color_to_u16(C_BG);
    const int bR = bg >> 11, bG = (bg >> 5) & 63, bB = bg & 31;
    for (int32_t i = 0; i < w * h; i++) {
        const uint16_t c = (uint16_t)(src[2 * i] | src[2 * i + 1] << 8); // LVGL's RGB565, byte by byte (flash may be unaligned)
        const int r = ((c >> 11) * 75 + bR * 25) / 100, g = (((c >> 5) & 63) * 75 + bG * 25) / 100, b = ((c & 31) * 75 + bB * 25) / 100;
        wpBuf[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
    lv_image_cache_drop(&wpDsc); // same address, new pixels
    wpDsc = {};
    wpDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    wpDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    wpDsc.header.w = (uint32_t)w;
    wpDsc.header.h = (uint32_t)h;
    wpDsc.header.stride = (uint32_t)w * 2;
    wpDsc.data_size = (uint32_t)(w * h * 2);
    wpDsc.data = (const uint8_t *)wpBuf;
    wpPic = pic;
    wpTheme = theme;
    return &wpDsc;
}

void App::lock(void)
{
    if (lockObj || !scr)
        return;
    kbHide();
    lockObj = plain(scr);
    lv_obj_set_size(lockObj, W(), H());
    lv_obj_set_style_bg_color(lockObj, C_BG, 0);
    lv_obj_set_style_bg_opa(lockObj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(lockObj, wallpaper(), 0); // none: plain (a gradient bands in 16-bit colour)
    lv_obj_add_flag(lockObj, LV_OBJ_FLAG_CLICKABLE); // takes every tap: nothing behind it can be pressed
    lockTime = label(lockObj, &lv_font_montserrat_48, C_TEXT, "");
    lv_obj_align(lockTime, LV_ALIGN_CENTER, 0, -40);
    lockDate = label(lockObj, F16, C_MUTED, "");
    lv_obj_align(lockDate, LV_ALIGN_CENTER, 0, 2);
    lockInfo = label(lockObj, F14, C_CYAN, "");
    lv_obj_set_width(lockInfo, W() - 24);
    lv_obj_set_style_text_align(lockInfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lockInfo, LV_ALIGN_CENTER, 0, 34);
    lv_obj_t *hint = label(lockObj, F12, C_DIM, m.hasKeyboard() ? "Press a key or hold the screen to unlock" : "Hold the screen to unlock");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(lockObj, [](lv_event_t *) { self->unlock(); }, LV_EVENT_LONG_PRESSED, nullptr);
    refreshLock();
}

void App::unlock(void)
{
    if (!lockObj)
        return;
    lv_obj_delete(lockObj);
    lockObj = lockTime = lockDate = lockInfo = nullptr;
    lastSlow = 0; // the page behind catches up at once
}

void App::refreshLock(void)
{
    static const char *const DAY[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MON[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    const uint32_t now = m.now();
    if (now > 1600000000) {
        struct tm tv;
        localTime(now, tv);
        setTextFmt(lockTime, "%02d:%02d", tv.tm_hour, tv.tm_min);
        setTextFmt(lockDate, "%s %d %s", DAY[tv.tm_wday % 7], tv.tm_mday, MON[tv.tm_mon % 12]);
    } else {
        setText(lockTime, "--:--");
        setText(lockDate, "Waiting for the time");
    }
    int unread = 0;
    for (const Conv &c : convs)
        unread += c.unread;
    char msg[40] = "No new messages", bat[24] = "";
    if (unread)
        snprintf(msg, sizeof(msg), LV_SYMBOL_ENVELOPE " %d new message%s", unread, unread == 1 ? "" : "s");
    const int bp = m.batteryPercent();
    if (bp > 100)
        snprintf(bat, sizeof(bat), "   " LV_SYMBOL_CHARGE " charging");
    else if (bp >= 0)
        snprintf(bat, sizeof(bat), "   " LV_SYMBOL_BATTERY_FULL " %d%%", bp);
    setTextFmt(lockInfo, "%s%s", msg, bat);
    if (lv_obj_get_index(lockObj) != (int32_t)lv_obj_get_child_count(scr) - 1)
        lv_obj_move_foreground(lockObj); // above anything opened since
}

void App::touched(void)
{
    wake();
    if (railFocus)
        setRail(false);
    if (keysUsed) {
        keysUsed = false;
        if (lv_obj_t *f = lv_group_get_focused(group))
            lv_obj_remove_state(f, LV_STATE_FOCUS_KEY);
    }
}

void App::wake(void)
{
    lastInput = lv_tick_get();
    if (!awake) {
        awake = true;
        m.setBacklight((uint8_t)m.uiPref("bright", 200));
    }
}

// the keyboard light: off, on for 10 s after each key typed, or on with the screen; as bright as the screen allows
void App::keyLightTick(uint32_t t)
{
    if (!m.hasKeyLight())
        return;
    const int mode = m.uiPref("kblight", 1);
    const bool on = awake && (mode == 2 || (mode == 1 && lastTyped && t - lastTyped < 10000));
    const int level = on ? std::max(30, m.uiPref("bright", 200) / 2) : 0;
    if (level != keyLight) { // only when it changes (it is a message to the keyboard's own chip)
        keyLight = level;
        m.setKeyLight((uint8_t)level);
    }
}

void App::key(uint32_t k)
{
    if ((k >= 0x20 && k < 0x7F) || k == K_BKSP)
        lastTyped = lv_tick_get() | 1; // typing (the keyboard light follows it)
    bool wasAsleep = !awake;
    wake();
    if (wasAsleep)
        return; // the key that wakes the screen only wakes it
    if (lockObj)
        return unlock(); // the next one opens the lock screen
    if (k >= K_GOTO && k < K_GOTO + T_COUNT)
        return selectTab((int)(k - K_GOTO));
    if (k == K_ADVERT || k == K_ADVERT_FLOOD) {
        const bool flood = k == K_ADVERT_FLOOD;
        return toast(m.sendAdvert(flood) ? (flood ? LV_SYMBOL_GPS " Advert sent to the mesh"
                                                  : LV_SYMBOL_WIFI " Zero-hop advert sent (neighbours only)")
                                         : "Couldn't send the advert");
    }
    if (k == K_QUICK) {
        ConvKey pub; // channel 0: Public
        selectTab(T_CHATS);
        return openChat(pub);
    }
    if (railFocus) {
        // on the menu: along it moves between tabs (each opens as the cursor reaches it), into the page enters it
        const uint32_t prevK = LAND ? K_UP : K_LEFT, nextK = LAND ? K_DOWN : K_RIGHT, inK = LAND ? K_RIGHT : K_UP;
        if (k == prevK || k == nextK)
            return selectTab((tab + (k == nextK ? 1 : T_COUNT - 1)) % T_COUNT);
        if (k == inK || k == K_ENTER || k == '\r' || k == '\n' || k == K_TAB) {
            setRail(false);
            refocus();
            if (lv_obj_t *n = lv_group_get_focused(group)) {
                lv_obj_add_state(n, LV_STATE_FOCUS_KEY);
                lv_obj_scroll_to_view_recursive(n, LV_ANIM_ON);
            }
            return;
        }
        if (k == K_BACK || k == K_BKSP || k == 0x08 || k == K_HOME) {
            if (tab != T_HOME)
                return selectTab(T_HOME);
            return;
        }
        setRail(false); // anything else: off the menu, and the key does what it does on the page
    }
    if (tab == T_MAP && !topOverlay()) { // the map: Up / Down zoom, OK fits the mesh back on screen
        if (k == K_UP || k == K_DOWN || k == K_ENTER || k == '\r' || k == '\n') {
            keysUsed = true;
            if (k == K_UP)
                mapZoom(0.625f);
            else if (k == K_DOWN)
                mapZoom(1.6f);
            else
                mapCentre();
            return;
        }
    }
    lv_obj_t *f = lv_group_get_focused(group);
    if (!keysUsed) {
        // the first arrow or Enter after touching only shows where the focus is
        keysUsed = true;
        bool nav = k == K_UP || k == K_DOWN || k == K_ENTER || k == '\r';
        if (!f && lv_group_get_obj_count(group)) {
            lv_group_focus_next(group);
            f = lv_group_get_focused(group);
        }
        if (f) {
            lv_obj_add_state(f, LV_STATE_FOCUS_KEY);
            lv_obj_scroll_to_view_recursive(f, LV_ANIM_ON);
        }
        if (nav)
            return;
    }
    const bool inText = f && lv_obj_check_type(f, &lv_textarea_class);
    const bool ddOpen = f && lv_obj_check_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f);
    auto move = [&](bool next) {
        if (f)
            lv_obj_remove_state(f, LV_STATE_FOCUS_KEY);
        next ? lv_group_focus_next(group) : lv_group_focus_prev(group);
        if (lv_obj_t *n = lv_group_get_focused(group))
            lv_obj_add_state(n, LV_STATE_FOCUS_KEY);
    };
    auto jump = [&](lv_obj_t *n) {
        if (f)
            lv_obj_remove_state(f, LV_STATE_FOCUS_KEY);
        lv_group_focus_obj(n); // scrolls it into view (LV_OBJ_FLAG_SCROLL_ON_FOCUS)
        lv_obj_add_state(n, LV_STATE_FOCUS_KEY);
    };
    if (ddOpen) { // an open list takes the arrows and Enter
        if (k == K_UP || k == K_DOWN) {
            lv_obj_t *list = lv_dropdown_get_list(f);
            (void)list;
            uint32_t sel = lv_dropdown_get_selected(f), cnt = lv_dropdown_get_option_count(f);
            sel = k == K_UP ? (sel + cnt - 1) % cnt : (sel + 1) % cnt;
            lv_dropdown_set_selected(f, sel);
        } else if (k == K_ENTER || k == '\r' || k == '\n') {
            lv_dropdown_close(f);
            lv_obj_send_event(f, LV_EVENT_VALUE_CHANGED, nullptr);
        } else if (k == K_BACK)
            lv_dropdown_close(f);
        return;
    }
    switch (k) {
    case K_UP:
    case K_DOWN:
        if (inText && chatOpen && k == K_UP && f == chatInput && !lv_textarea_get_text(f)[0] && !chatKey.dm) {
            // nothing typed yet: Up steps onto the messages above (OK on one replies to its sender)
            lv_obj_t *n = nearest(f, k);
            if (n && lv_obj_get_parent(n) && lv_obj_get_parent(lv_obj_get_parent(n)) == chatMsgs)
                return jump(n);
        }
        if (inText && chatOpen) { // typing a message: Up / Down scroll the conversation above
            lv_obj_scroll_by_bounded(chatMsgs, 0, k == K_UP ? 40 : -40, LV_ANIM_ON);
            return;
        }
        if (f == chatMsgs && k == K_UP) {
            lv_obj_scroll_by_bounded(chatMsgs, 0, 40, LV_ANIM_ON);
            return;
        }
        if (lv_obj_t *n = nearest(f, k)) // the thing above / below on the screen
            return jump(n);
        return move(k == K_DOWN); // nothing that way: round to the other end
    case K_TAB:
        return move(true);
    case K_LEFT:
    case K_RIGHT:
        if (f && f == custFreq) {
            stepFreq(f, k == K_RIGHT);
            return;
        }
        if (inText) { // the cursor moves through the text; past either end, the keys leave the field
            const uint32_t pos = lv_textarea_get_cursor_pos(f), len = (uint32_t)strlen(lv_textarea_get_text(f));
            if (k == K_LEFT ? pos > 0 : pos < len) {
                k == K_LEFT ? lv_textarea_cursor_left(f) : lv_textarea_cursor_right(f);
                return;
            }
        }
        if (f && lv_obj_check_type(f, &lv_slider_class)) {
            lv_slider_set_value(f, lv_slider_get_value(f) + (k == K_RIGHT ? 20 : -20), LV_ANIM_OFF);
            lv_obj_send_event(f, LV_EVENT_VALUE_CHANGED, nullptr);
            return;
        }
        if (lv_obj_t *n = nearest(f, k)) // the thing beside it on the same row
            return jump(n);
        if (topOverlay()) {
            if (k == K_LEFT)
                goBack();
            return;
        }
        if (tab == T_MORE && moreOpen >= 0) { // in a settings page: Left goes back to the six, Right stays
            if (k == K_LEFT)
                openMore(-1);
            return;
        }
        if (LAND && k == K_LEFT) { // landscape: Left steps out of the page onto the menu rail
            keysUsed = true;
            return setRail(true);
        }
        return selectTab((tab + (k == K_RIGHT ? 1 : T_COUNT - 1)) % T_COUNT);
    case K_BACK:
        return goBack();
    case K_HOME:
        return selectTab(T_HOME);
    case K_BKSP:
    case 0x08:
        if (inText)
            lv_textarea_delete_char(f);
        else
            goBack();
        return;
    case K_ENTER:
    case '\r':
    case '\n':
        if (inText) {
            if (f == chatInput)
                sendFromInput();
            else if (lv_obj_has_flag(f, F_SUBMIT))
                lv_obj_send_event(f, LV_EVENT_READY, nullptr);
            else
                move(true);
            return;
        }
        if (!f)
            return;
        if (lv_obj_check_type(f, &lv_switch_class)) {
            if (lv_obj_has_state(f, LV_STATE_CHECKED))
                lv_obj_remove_state(f, LV_STATE_CHECKED);
            else
                lv_obj_add_state(f, LV_STATE_CHECKED);
            lv_obj_send_event(f, LV_EVENT_VALUE_CHANGED, nullptr);
        } else if (lv_obj_check_type(f, &lv_dropdown_class))
            lv_dropdown_open(f);
        else
            lv_obj_send_event(f, LV_EVENT_CLICKED, nullptr);
        return;
    default:
        break;
    }
    if (k >= 0x20 && k < 0x110000) {
        if (topOverlay() == replyBox)
            return; // pick one of its buttons first
        if (!inText && chatOpen) { // typing anywhere in a chat goes to the message
            lv_group_focus_obj(chatInput);
            lv_obj_add_state(chatInput, LV_STATE_FOCUS_KEY);
            f = chatInput;
        } else if (!inText && rmTypingTarget()) { // managing a repeater: to its command line (or password)
            f = rmTypingTarget();
            lv_group_focus_obj(f);
            lv_obj_add_state(f, LV_STATE_FOCUS_KEY);
        } else if (!inText) {
            if ((k == 'r' || k == 'R') && haveLastIn) { // R: open the conversation the last message came in on
                selectTab(T_CHATS);
                openChat(lastIn);
            }
            return;
        }
        char u[5] = {0};
        if (k < 0x80)
            u[0] = (char)k;
        else if (k < 0x800)
            u[0] = (char)(0xC0 | (k >> 6)), u[1] = (char)(0x80 | (k & 0x3F));
        else
            u[0] = (char)(0xE0 | (k >> 12)), u[1] = (char)(0x80 | ((k >> 6) & 0x3F)), u[2] = (char)(0x80 | (k & 0x3F));
        lv_textarea_add_text(f, u);
    }
}

void App::toast(const char *text)
{
    lv_label_set_text(toastLbl, text);
    toastIsMsg = false;
    lv_obj_remove_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toastBox);
    toastUntil = lv_tick_get() + 3500;
}

void App::loop(uint32_t ms)
{
    (void)ms;
    uint32_t t = lv_tick_get();
    if (toastUntil && (int32_t)(t - toastUntil) >= 0) {
        toastUntil = 0;
        lv_obj_add_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
    }
    // packets-per-minute history
    uint32_t now = m.now();
    uint32_t minute = (now ? now : t / 1000) / 60;
    if (!minuteStart)
        minuteStart = minute;
    while (minuteStart < minute) {
        memmove(perMin, perMin + 1, sizeof(perMin) - sizeof(perMin[0]));
        perMin[29] = 0;
        memmove(noiseHist, noiseHist + 1, sizeof(noiseHist) - sizeof(noiseHist[0]));
        noiseHist[29] = 0;
        minuteStart++;
        if (minute - minuteStart > 30)
            minuteStart = minute;
    }
    // screen timeout: dim, then off (any key or touch wakes it)
    uint32_t timeout = (uint32_t)m.uiPref("timeout", 60) * 1000;
    if (awake && timeout && t - lastInput > timeout) {
        awake = false;
        m.setBacklight(0);
        if (m.uiPref("lock", 0))
            lock(); // ready before the screen comes back on
    }
    keyLightTick(t);
    rmTick(); // answers a repeater being managed never sent
    if (chatsChangedAt && ((!awake) || t - chatsChangedAt >= 120000))
        saveChats();
    if (!awake)
        return;
    if (lockObj) { // nothing behind it needs drawing
        if (t - lastSlow >= 2000) {
            lastSlow = t;
            if (int16_t nf = m.stats().noiseFloor)
                noiseHist[29] = nf;
            refreshLock();
        }
        return;
    }
    if (tab == T_MAP && !topOverlay() && mapTiles() && hoptics::TileMap::get().takeDirty())
        lv_obj_invalidate(mapObj); // new street-map tiles are in the buffer
    if (t - lastTick >= 250) {
        lastTick = t;
        if (tab == T_LIVE && dirtyLive && !topOverlay())
            refreshLive();
        if (tab == T_NODES && !topOverlay() && (dirtyContacts || m.contactsVersion() != contactsSeen))
            refreshNodes();
        if (tab == T_CHATS && dirtyChats && !topOverlay())
            refreshChats();
    }
    if (t - lastSlow >= 2000) {
        lastSlow = t;
        if (int16_t nf = m.stats().noiseFloor)
            noiseHist[29] = nf; // this minute's latest reading
        if (statOpen >= 0 && topOverlay() == statOv && t - statDrawn >= 5000)
            renderStat();
        refreshStatus();
        if (tab == T_HOME && !topOverlay())
            refreshHome();
        if (tab == T_MORE && !topOverlay()) {
            refreshWifi();
            if (moreOpen < 0)
                refreshMoreHub(); // the summaries keep up (uptime, Wi-Fi joining)
        }
        if (tab == T_MAP && !topOverlay()) {
            refreshMapStrip();
            lv_obj_invalidate(mapObj); // ages, fades and new positions
        }
        if (tab == T_LIVE)
            dirtyLive = true; // the "ago" times move
    }
}

// ================================================================= the web page's data (hoptics/net/hx_web)
namespace
{
// text from the radio is strangers' text: quotes, backslashes, control characters and <>& are escaped, so it can
// only ever be a string in the page (which also shows it with textContent)
void jsStr(std::string &o, const char *s)
{
    o += '"';
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20 || c == '<' || c == '>' || c == '&') {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else
            o += (char)c;
    }
    o += '"';
}
void jsHex(std::string &o, const uint8_t *b, size_t n)
{
    static const char *H = "0123456789abcdef";
    o += '"';
    for (size_t i = 0; i < n; i++) {
        o += H[b[i] >> 4];
        o += H[b[i] & 15];
    }
    o += '"';
}
void jsf(std::string &o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jsf(std::string &o, const char *fmt, ...)
{
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    o += b;
}
} // namespace

void App::webJson(std::string &o, uint32_t contactsHave, uint32_t packetsHave, uint32_t msgsHave, uint32_t histHave)
{
    o.reserve(6144);
    const uint32_t now = m.now();
    const Stats s = m.stats();
    const Radio r = m.radio();
    jsf(o, "{\"v\":1,\"now\":%lu,\"up\":%lu,\"node\":{\"name\":", (unsigned long)now, (unsigned long)s.uptimeS);
    jsStr(o, m.nodeName());
    uint8_t pk[32];
    m.pubKey(pk);
    o += ",\"key\":";
    jsHex(o, pk, 6);
    o += ",\"fw\":";
    jsStr(o, m.firmwareVersion());
    o += ",\"board\":";
    jsStr(o, m.boardName());
    int32_t lat, lon;
    if (m.gpsFix(lat, lon))
        jsf(o, ",\"lat\":%.6f,\"lon\":%.6f", lat / 1e6, lon / 1e6);
    jsf(o, ",\"batt\":{\"mv\":%u,\"pct\":%d}}", (unsigned)m.batteryMv(), m.batteryPercent());
    jsf(o, ",\"radio\":{\"freq\":%.3f,\"bw\":%g,\"sf\":%u,\"cr\":%u,\"tx\":%d}", r.freq, r.bw, r.sf, r.cr, r.txPower);
    jsf(o, ",\"stats\":{\"rx\":%lu,\"tx\":%lu,\"flood\":%lu,\"direct\":%lu,\"dups\":%lu,\"airTxS\":%lu,\"noise\":%d,\"lastRssi\":%d,\"lastSnr\":%.2f}",
        (unsigned long)s.rx, (unsigned long)s.tx, (unsigned long)s.rxFlood, (unsigned long)s.rxDirect, (unsigned long)s.dups,
        (unsigned long)(s.airtimeTxMs / 1000), s.noiseFloor, s.lastRssi, s.lastSnr4 / 4.0);
    o += ",\"perMin\":[";
    for (int i = 0; i < 30; i++)
        jsf(o, i ? ",%u" : "%u", perMin[i]);
    o += ']';
    // contacts: only when the list changed since the page last had it
    const uint32_t cv = m.contactsVersion();
    if (cv != contactsHave) {
        jsf(o, ",\"cv\":%lu,\"contacts\":[", (unsigned long)cv);
        Contact c;
        bool first = true;
        for (size_t i = 0; i < m.contactCount(); i++) {
            if (!m.contactAt(i, c))
                continue;
            o += first ? "{\"k\":" : ",{\"k\":";
            first = false;
            jsHex(o, c.key, 6);
            o += ",\"n\":";
            jsStr(o, c.name);
            jsf(o, ",\"t\":%d,\"h\":%d,\"last\":%lu", (int)c.type, c.pathLen, (unsigned long)c.lastHeard);
            if (c.hasPos)
                jsf(o, ",\"lat\":%.6f,\"lon\":%.6f", c.lat / 1e6, c.lon / 1e6);
            if (c.favourite)
                o += ",\"fav\":true";
            o += '}';
        }
        o += ']';
    }
    o += ",\"channels\":[";
    bool first = true;
    for (size_t i = 0; i < m.channelCount(); i++) {
        Channel ch;
        if (!m.channelAt(i, ch))
            continue;
        jsf(o, first ? "{\"i\":%u,\"n\":" : ",{\"i\":%u,\"n\":", ch.index);
        first = false;
        jsStr(o, ch.name);
        char k[33];
        if (m.channelWebKey(i, k))
            jsf(o, ",\"k\":\"%s\"", k);
        o += '}';
    }
    o += "],\"packets\":[";
    first = true;
    for (int i = 0; i < liveCount; i++) { // oldest first
        const Packet &p = live[(liveHead + kLive - liveCount + i) % kLive];
        if (p.seq <= packetsHave)
            continue;
        jsf(o, first ? "{\"s\":%lu,\"t\":%lu,\"ty\":\"%s\",\"fl\":%s,\"h\":%u,\"hs\":%u,\"len\":%u" : ",{\"s\":%lu,\"t\":%lu,\"ty\":\"%s\",\"fl\":%s,\"h\":%u,\"hs\":%u,\"len\":%u",
            (unsigned long)p.seq, (unsigned long)p.time, pktTypeName(p.type), p.direct ? "false" : "true", p.pathLen, p.hashSize, p.len);
        first = false;
        if (p.hasHash) {
            o += ",\"id\":";
            jsHex(o, p.hash, 8);
        }
        o += ",\"p\":[";
        const int hs = std::max<int>(1, p.hashSize), shown = std::min<int>(p.pathLen, (int)sizeof(p.path) / hs);
        for (int h = 0; h < shown; h++) {
            if (h)
                o += ',';
            jsHex(o, p.path + h * hs, hs);
        }
        o += ']';
        if (!p.mine)
            jsf(o, ",\"rssi\":%d,\"snr\":%.2f", p.rssi, p.snr4 / 4.0);
        else
            o += ",\"me\":true";
        if (p.from[0]) {
            o += ",\"f\":";
            jsStr(o, p.from);
        }
        if (p.hasFromKey) {
            o += ",\"k\":";
            jsHex(o, p.fromKey, 6);
        }
        if (p.hasPos)
            jsf(o, ",\"lat\":%.6f,\"lon\":%.6f", p.lat / 1e6, p.lon / 1e6);
        o += '}';
    }
    // channel messages only: direct messages never leave the radio
    o += "],\"msgs\":[";
    first = true;
    for (const Conv &c : convs) {
        if (c.key.dm)
            continue;
        for (const Line &l : c.lines) {
            if (l.seq <= msgsHave)
                continue;
            jsf(o, first ? "{\"s\":%lu,\"t\":%lu,\"c\":%u,\"f\":" : ",{\"s\":%lu,\"t\":%lu,\"c\":%u,\"f\":", (unsigned long)l.seq,
                (unsigned long)l.time, c.key.channel);
            first = false;
            jsStr(o, l.mine ? m.nodeName() : l.from.c_str());
            o += ",\"x\":";
            jsStr(o, l.text.c_str());
            if (l.mine)
                o += ",\"me\":true";
            else if (l.hops == -2)
                o += ",\"net\":true";
            else if (l.hops >= 0)
                jsf(o, ",\"h\":%d", l.hops);
            o += '}';
        }
    }
    o += ']';
    m.webExtras(o, histHave);
    o += '}';
}

// ================================================================= managing a repeater or room server
#include "hx_ui_remote.inc"

} // namespace hx
