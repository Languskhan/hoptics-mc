// Hoptics-MC: managing a repeater or room server from this radio (MeshCore's remote admin). Included at the end of
// hx_ui.cpp, inside namespace hx, so it shares the screen helpers there. MIT licence, see LICENSE.
//
// Log in with its admin password (or the guest one, which shows its status), then four pages: Status (with its
// telemetry and the actions), Heard (the nodes it hears directly), Settings (every setting its command line has, read
// and written one command at a time) and Console (its command line). Nothing is kept once the screen closes, except
// a password the user asked this radio to keep.

namespace
{
enum RKind : uint8_t { RK_TEXT, RK_NUM, RK_ONOFF, RK_CHOICE };
struct RSet {
    const char *group; // a new card starts here
    const char *label, *key;
    uint8_t kind;
    const char *opts; // RK_CHOICE: the options as shown, "\n" between
    const char *vals; // RK_CHOICE: what each option is on its command line (nullptr: as shown)
    const char *help;
};
// the settings MeshCore's repeater and room server command line has ("get <key>" / "set <key> <value>"); a node
// whose firmware is older answers "??" for the newer ones, and their rows say so
const RSet RSETS[] = {
    {"IDENTITY AND LOCATION", "Name", "name", RK_TEXT, nullptr, nullptr, nullptr},
    {nullptr, "Latitude", "lat", RK_NUM, nullptr, nullptr, nullptr},
    {nullptr, "Longitude", "lon", RK_NUM, nullptr, nullptr, "Where it shows on maps. 0, 0 = not shown."},
    {nullptr, "Owner info", "owner.info", RK_TEXT, nullptr, nullptr, "Who runs it, for anyone who asks. | starts a new line."},
    {"RADIO", "Frequency, bandwidth, SF, CR", "radio", RK_TEXT, nullptr, nullptr,
     "e.g. 910.525,62.5,7,5. Saved now, used after a restart. Must match the mesh, or you lose it: try it first."},
    {nullptr, "Transmit power (dBm)", "tx", RK_NUM, nullptr, nullptr, nullptr},
    {nullptr, "Boosted receive gain", "radio.rxgain", RK_ONOFF, nullptr, nullptr, "SX126x radios: hears weaker signals."},
    {nullptr, "Listen before talk", "cad", RK_ONOFF, nullptr, nullptr, "Waits for a quiet channel before it sends (CAD)."},
    {"REPEATING", "Repeat packets", "repeat", RK_ONOFF, nullptr, nullptr, "Off: it still answers you, but relays nothing."},
    {nullptr, "Flood hop limit", "flood.max", RK_NUM, nullptr, nullptr, "Floods that have come further than this are not repeated (0-64)."},
    {nullptr, "Advert hop limit", "flood.max.advert", RK_NUM, nullptr, nullptr, "The same, for adverts."},
    {nullptr, "Loop detection", "loop.detect", RK_CHOICE, "off\nminimal\nmoderate\nstrict", nullptr,
     "Drops floods that have already passed through it."},
    {nullptr, "Path hash size", "path.hash.mode", RK_CHOICE, "1 byte\n2 bytes\n3 bytes", "0\n1\n2",
     "Bytes per repeater in the routes it starts."},
    {"ADVERTS", "Zero-hop advert every (minutes)", "advert.interval", RK_NUM, nullptr, nullptr, "0 = off, else 60-240."},
    {nullptr, "Flood advert every (hours)", "flood.advert.interval", RK_NUM, nullptr, nullptr, "0 = off, else 3-168."},
    {"TIMING", "Airtime factor", "af", RK_NUM, nullptr, nullptr, "Waits this many times its last packet's airtime before the next (0-9)."},
    {nullptr, "Receive delay", "rxdelay", RK_NUM, nullptr, nullptr, "Lets stronger copies of a flood go first (0-20)."},
    {nullptr, "Flood send delay", "txdelay", RK_NUM, nullptr, nullptr, "Random wait before repeating a flood (0-2)."},
    {nullptr, "Direct send delay", "direct.txdelay", RK_NUM, nullptr, nullptr, "The same, for direct packets (0-2)."},
    {"ACCESS", "Guest password", "guest.password", RK_TEXT, nullptr, nullptr, "Lets people see its status, not change it."},
    {nullptr, "Open to guests", "allow.read.only", RK_ONOFF, nullptr, nullptr,
     "Room servers: anyone can read the posts without the guest password."},
};
constexpr int N_RSETS = sizeof(RSETS) / sizeof(RSETS[0]);
const char *const NOT_HERE = "\x01"; // a value: its firmware doesn't have this setting
int rsetIndex(const char *key)
{
    for (int i = 0; i < N_RSETS; i++)
        if (!strcmp(RSETS[i].key, key))
            return i;
    return -1;
}

// the n-th line of a "\n" list, or -1
int optIndex(const char *list, const char *v)
{
    int i = 0;
    for (const char *p = list; *p; i++) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (strlen(v) == n && !strncmp(p, v, n))
            return i;
        if (!e)
            break;
        p = e + 1;
    }
    return -1;
}
std::string optAt(const char *list, int idx)
{
    const char *p = list;
    for (int i = 0; i < idx && p; i++) {
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    if (!p)
        return "";
    const char *e = strchr(p, '\n');
    return e ? std::string(p, e - p) : std::string(p);
}

std::string durText(uint32_t s)
{
    char b[24];
    if (s < 3600)
        snprintf(b, sizeof(b), "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
    else if (s < 86400)
        snprintf(b, sizeof(b), "%uh %um", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    else
        snprintf(b, sizeof(b), "%ud %uh", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24));
    return b;
}

lv_color_t snrColor(int8_t snr4) { return snr4 >= 20 ? C_GREEN : snr4 >= -20 ? C_AMBER : C_RED; }

// telemetry: MeshCore sends Cayenne LPP (channel, type, value). Channel 1 is the node itself.
void lppLines(const uint8_t *d, int len, std::vector<std::string> &out)
{
    struct T {
        uint8_t type, size;
        float div;
        bool sgn;
        const char *name, *unit;
    };
    static const T TYPES[] = {
        {0, 1, 1, false, "Digital in", ""},       {1, 1, 1, false, "Digital out", ""},
        {2, 2, 100, true, "Analog in", ""},       {3, 2, 100, true, "Analog out", ""},
        {100, 4, 1, false, "Sensor", ""},         {101, 2, 1, false, "Light", " lux"},
        {102, 1, 1, false, "Presence", ""},       {103, 2, 10, true, "Temperature", " \xC2\xB0" "C"},
        {104, 1, 2, false, "Humidity", " %"},     {115, 2, 10, false, "Pressure", " hPa"},
        {116, 2, 100, false, "Voltage", " V"},    {117, 2, 1000, false, "Current", " A"},
        {118, 4, 1, false, "Frequency", " Hz"},   {120, 1, 1, false, "Percent", " %"},
        {121, 2, 1, true, "Altitude", " m"},      {125, 2, 1, false, "Concentration", " ppm"},
        {128, 2, 1, false, "Power", " W"},        {130, 4, 1000, false, "Distance", " m"},
        {131, 4, 1000, false, "Energy", " kWh"},  {132, 2, 1, false, "Direction", "\xC2\xB0"},
        {133, 4, 1, false, "Time", ""},           {142, 1, 1, false, "Switch", ""},
    };
    int at = 0;
    while (at + 2 <= len) {
        const uint8_t ch = d[at], type = d[at + 1];
        at += 2;
        char b[64];
        const char *where = ch == 1 ? "" : " (sensor %u)";
        char w[16];
        snprintf(w, sizeof(w), where, (unsigned)ch);
        if (type == 136) { // GPS: 3 bytes each, lat/lon x 10000, altitude x 100
            if (at + 9 > len)
                break;
            auto s24 = [&](int o) -> int32_t { int32_t v = (int32_t)d[o] << 16 | d[o + 1] << 8 | d[o + 2]; return v & 0x800000 ? v - 0x1000000 : v; };
            snprintf(b, sizeof(b), "Position%s: %.4f, %.4f", w, s24(at) / 10000.0, s24(at + 3) / 10000.0);
            out.push_back(b);
            at += 9;
            continue;
        }
        if (type == 113 || type == 134) { // accelerometer, gyro: 3 x 2 bytes
            if (at + 6 > len)
                break;
            at += 6;
            continue;
        }
        const T *t = nullptr;
        for (auto &x : TYPES)
            if (x.type == type)
                t = &x;
        if (!t || at + t->size > len)
            break; // unknown type: the rest can't be read
        uint32_t raw = 0;
        for (int i = 0; i < t->size; i++)
            raw = raw << 8 | d[at + i];
        at += t->size;
        double v = raw;
        if (t->sgn && t->size == 2)
            v = (int16_t)raw;
        v /= t->div;
        const char *name = t->name;
        if (type == 116 && ch == 1)
            name = "Battery";
        else if (type == 103 && ch == 1)
            name = "Board temperature";
        snprintf(b, sizeof(b), t->div >= 100 ? "%s%s: %.2f%s" : t->div > 1 ? "%s%s: %.1f%s" : "%s%s: %.0f%s", name, w, v, t->unit);
        out.push_back(b);
    }
}

// a small button that fits a row of others
lv_obj_t *smallButton(lv_obj_t *parent, const char *text, lv_color_t c, int32_t w)
{
    lv_obj_t *b = button(parent, text, c, w, 28);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), F12, 0);
    return b;
}
// a one-line text field
lv_obj_t *field(lv_obj_t *parent, const char *placeholder, uint32_t maxLen)
{
    lv_obj_t *t = lv_textarea_create(parent);
    lv_textarea_set_one_line(t, true);
    lv_textarea_set_max_length(t, maxLen);
    lv_textarea_set_placeholder_text(t, placeholder);
    lv_obj_set_width(t, LV_PCT(100));
    styleInput(t);
    lv_obj_set_style_pad_ver(t, 5, 0);
    stop(t);
    return t;
}
} // namespace

struct App::Remote {
    uint8_t key[6] = {};
    char name[32] = {};
    NodeType type = NodeType::Repeater;
    int8_t hops = -1;
    enum State { LOGIN, LOGGING_IN, IN } state = LOGIN;
    uint8_t perms = 0; // low 2 bits: 0 guest, 1 read-only, 2 read-write, 3 admin
    int page = 0;      // 0 status, 1 heard, 2 settings, 3 console
    std::string note;  // the login form's line
    bool noteBad = false;
    std::string pw;    // as typed, to keep after a good login
    bool remember = true;
    // the binary request on its way (one at a time)
    enum Ask : uint8_t { A_NONE, A_LOGIN, A_STATUS, A_NEIGH, A_TELEM, A_ACL } asking = A_NONE;
    uint32_t askUntil = 0;
    bool hasStatus = false, statusFailed = false;
    RemoteStatus st;
    uint32_t stAt = 0; // lv tick of the answer
    bool hasNeigh = false, neighFailed = false;
    uint16_t nTotal = 0;
    std::vector<RemoteNeighbour> neigh;
    bool hasTelem = false, telemFailed = false;
    std::vector<std::string> telem;
    bool hasAcl = false, aclFailed = false;
    std::vector<RemoteClient> acl;
    // its command line: one command on its way at a time, the rest wait their turn (answers carry no tag)
    struct Cmd {
        std::string text;
        int row = -1;      // the settings row it reads (get) or writes (set); -1 none
        bool set = false;
        std::string value; // set: what it writes
    };
    std::deque<Cmd> queue;
    bool busy = false;
    Cmd cur;
    uint32_t cmdUntil = 0;
    std::vector<std::string> log; // the console: "> command" and the answers
    std::vector<std::string> vals; // settings as read: "" not yet, NOT_HERE not on its firmware
    int readsLeft = 0;
    uint32_t rebootArm = 0; // lv tick until which a second press of Restart restarts it
    int drawnState = -1, drawnPage = -1; // what the body shows now (a new page starts at its top)
    // the open page's widgets
    lv_obj_t *pass = nullptr, *noteLbl = nullptr, *conOut = nullptr, *conIn = nullptr, *readLbl = nullptr;
    lv_obj_t *tabBtn[4] = {};
    std::vector<lv_obj_t *> ctl; // settings: each row's control
};

static bool sameKey(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

void App::buildRemoteOverlay(void)
{
    rmOv = overlay("Manage", &rmTitle, [](lv_event_t *) { self->closeRemote(); });
    rmTabs = plain(rmOv);
    lv_obj_set_size(rmTabs, LV_PCT(100), 34);
    lv_obj_set_style_pad_hor(rmTabs, PAD, 0);
    lv_obj_set_style_pad_top(rmTabs, 4, 0);
    row(rmTabs, 4);
    rmBody = plain(rmOv);
    lv_obj_set_width(rmBody, LV_PCT(100));
    lv_obj_set_flex_grow(rmBody, 1);
    lv_obj_add_flag(rmBody, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(rmBody, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rmBody, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(rmBody, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(rmBody, C_CYAN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(rmBody, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_all(rmBody, PAD, 0);
    column(rmBody, PAD);
}

void App::openRemote(const uint8_t key[6])
{
    Contact c;
    if (!m.findContact(key, c))
        return;
    delete rm;
    rm = new Remote;
    memcpy(rm->key, key, 6);
    strncpy(rm->name, c.name[0] ? c.name : "Node", sizeof(rm->name) - 1);
    rm->type = c.type;
    rm->hops = c.pathLen;
    rm->vals.assign(N_RSETS, "");
    rm->ctl.assign(N_RSETS, nullptr);
    char t[48];
    snprintf(t, sizeof(t), "Manage %s", rm->name);
    lv_label_set_text(rmTitle, t);
    char pw[24];
    const bool kept = m.savedPassword(key, pw, sizeof(pw));
    if (kept)
        rm->pw = pw;
    lv_obj_remove_flag(rmOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(rmOv);
    lv_obj_move_foreground(toastBox);
    rmRender();
    if (kept)
        rmLogin(); // the kept password: straight in
}

void App::closeRemote(void)
{
    if (!rm)
        return;
    lv_obj_add_flag(rmOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(rmBody);
    lv_obj_clean(rmTabs);
    delete rm;
    rm = nullptr;
    refocus();
}

// ------------------------------------------------------------------ sending
void App::rmLogin(void)
{
    if (!rm || rm->asking == Remote::A_LOGIN)
        return;
    if (rm->pass)
        rm->pw = lv_textarea_get_text(rm->pass);
    uint32_t wait = m.remoteLogin(rm->key, rm->pw.c_str());
    if (!wait) {
        rm->note = "Couldn't send: the radio is busy. Try again.";
        rm->noteBad = true;
        return rmRender();
    }
    rm->state = Remote::LOGGING_IN;
    rm->asking = Remote::A_LOGIN;
    rm->askUntil = lv_tick_get() + wait;
    char b[96];
    snprintf(b, sizeof(b), "Logging in%s... (up to %u s)", rm->hops < 0 ? " over the whole mesh" : "", (unsigned)((wait + 999) / 1000));
    rm->note = b;
    rm->noteBad = false;
    rmRender();
}

void App::rmAsk(int what)
{
    if (!rm || rm->state != Remote::IN)
        return;
    if (rm->asking != Remote::A_NONE)
        return toast("Still waiting for its last answer");
    uint32_t wait = 0;
    switch (what) {
    case Remote::A_STATUS:
        wait = m.remoteStatus(rm->key);
        rm->statusFailed = false;
        break;
    case Remote::A_NEIGH:
        wait = m.remoteNeighbours(rm->key);
        rm->neighFailed = false;
        break;
    case Remote::A_TELEM:
        wait = m.remoteTelemetry(rm->key);
        rm->telemFailed = false;
        break;
    case Remote::A_ACL:
        wait = m.remoteAccessList(rm->key);
        rm->aclFailed = false;
        break;
    }
    if (!wait)
        return toast("Couldn't send: the radio is busy");
    rm->asking = (Remote::Ask)what;
    rm->askUntil = lv_tick_get() + wait;
    rmRender();
}

// queue a command for its command line; row/set: a settings row it reads or writes
void App::rmCommand(const std::string &text, int row, bool set, const std::string &value)
{
    if (!rm || rm->state != Remote::IN)
        return;
    Remote::Cmd c;
    c.text = text;
    c.row = row;
    c.set = set;
    c.value = value;
    rm->queue.push_back(c);
    rmPump();
}

void App::rmPump(void)
{
    if (!rm || rm->busy || rm->queue.empty())
        return;
    rm->cur = rm->queue.front();
    rm->queue.pop_front();
    uint32_t wait = m.remoteCommand(rm->key, rm->cur.text.c_str());
    rm->log.push_back("> " + rm->cur.text);
    if (!wait) {
        rm->log.push_back("! couldn't send (the radio is busy)");
        if (rm->cur.row >= 0 && !rm->cur.set && rm->readsLeft > 0)
            rm->readsLeft--;
        rmConsoleChanged();
        return rmPump();
    }
    rm->busy = true;
    rm->cmdUntil = lv_tick_get() + wait;
    rmConsoleChanged();
}

// its answer to the command on its way (or nothing: text == nullptr)
void App::rmAnswer(const char *text)
{
    if (!rm)
        return;
    Remote::Cmd c = rm->cur;
    rm->busy = false;
    rm->log.push_back(text ? text : "! no answer");
    if (rm->log.size() > 60)
        rm->log.erase(rm->log.begin(), rm->log.begin() + (rm->log.size() - 60));
    const bool err = !text || !strncmp(text, "??", 2) || !strncasecmp(text, "error", 5) || !strncasecmp(text, "err ", 4) ||
                     !strncmp(text, "Unknown", 7) || !strncmp(text, "unknown", 7);
    if (c.row >= 0 && c.row < N_RSETS) {
        const RSet &s = RSETS[c.row];
        if (!c.set) {
            if (rm->readsLeft > 0)
                rm->readsLeft--;
            if (text && !strncmp(text, "> ", 2))
                rm->vals[c.row] = text + 2;
            else if (text && err)
                rm->vals[c.row] = NOT_HERE;
            rmShowValue(c.row);
        } else {
            char b[96];
            if (text && !err) {
                rm->vals[c.row] = c.value;
                snprintf(b, sizeof(b), "%s: %s", s.label, text);
            } else
                snprintf(b, sizeof(b), "%s not saved: %s", s.label, text ? text : "no answer");
            toast(b);
            rmShowValue(c.row);
        }
    } else if (!c.text.empty() && rm->page != 3) {
        // an action from the Status page: say how it went
        char b[96];
        snprintf(b, sizeof(b), "%s", text ? text : "No answer from it");
        toast(b);
    }
    if (c.text == "password" || !strncmp(c.text.c_str(), "password ", 9)) {
        if (text && !strncmp(text, "password now", 12) && rm->remember && c.text.size() > 9) // a bare "password" names no new one
            m.savePassword(rm->key, c.text.c_str() + 9); // keep the new one, as the old one was kept
    }
    rmConsoleChanged();
    if (rm->readLbl)
        rmReadProgress();
    rmPump();
}

// ------------------------------------------------------------------ answers (Ui)
void App::onRemoteLogin(const uint8_t key[6], bool ok, uint8_t perms)
{
    if (!rm || !sameKey(key, rm->key) || rm->asking != Remote::A_LOGIN)
        return;
    rm->asking = Remote::A_NONE;
    if (!ok) {
        rm->state = Remote::LOGIN;
        rm->note = "It said no: wrong password.";
        rm->noteBad = true;
        return rmRender();
    }
    rm->state = Remote::IN;
    rm->perms = perms & 3;
    rm->note.clear();
    if (rm->remember && !rm->pw.empty())
        m.savePassword(rm->key, rm->pw.c_str());
    else if (!rm->remember)
        m.savePassword(rm->key, "");
    rm->page = 0;
    rmRender();
    rmAsk(Remote::A_STATUS);
}

void App::onRemoteStatus(const uint8_t key[6], const RemoteStatus &s)
{
    if (!rm || !sameKey(key, rm->key) || rm->asking != Remote::A_STATUS)
        return;
    rm->asking = Remote::A_NONE;
    rm->st = s;
    rm->hasStatus = true;
    rm->stAt = lv_tick_get();
    if (rm->page == 0)
        rmRender();
}

void App::onRemoteNeighbours(const uint8_t key[6], uint16_t total, const RemoteNeighbour *n, int count)
{
    if (!rm || !sameKey(key, rm->key) || rm->asking != Remote::A_NEIGH)
        return;
    rm->asking = Remote::A_NONE;
    rm->nTotal = total;
    rm->neigh.assign(n, n + count);
    rm->hasNeigh = true;
    if (rm->page == 1)
        rmRender();
}

void App::onRemoteTelemetry(const uint8_t key[6], const uint8_t *lpp, int len)
{
    if (!rm || !sameKey(key, rm->key) || rm->asking != Remote::A_TELEM)
        return;
    rm->asking = Remote::A_NONE;
    rm->telem.clear();
    lppLines(lpp, len, rm->telem);
    rm->hasTelem = true;
    if (rm->page == 0)
        rmRender();
}

void App::onRemoteAccessList(const uint8_t key[6], const RemoteClient *c, int count)
{
    if (!rm || !sameKey(key, rm->key) || rm->asking != Remote::A_ACL)
        return;
    rm->asking = Remote::A_NONE;
    rm->acl.assign(c, c + count);
    rm->hasAcl = true;
    if (rm->page == 2)
        rmRender();
}

void App::onRemoteReply(const uint8_t key[6], const char *text)
{
    if (!rm || !sameKey(key, rm->key))
        return;
    if (rm->busy)
        return rmAnswer(text);
    rm->log.push_back(text); // nothing asked: show it anyway
    if (rm->log.size() > 60) // the same cap as an answered line, so a chatty repeater can't grow it
        rm->log.erase(rm->log.begin(), rm->log.begin() + (rm->log.size() - 60));
    rmConsoleChanged();
}

// requests that never got an answer
void App::rmTick(void)
{
    if (!rm)
        return;
    const uint32_t t = lv_tick_get();
    if (rm->asking != Remote::A_NONE && (int32_t)(t - rm->askUntil) > 0) {
        const auto what = rm->asking;
        rm->asking = Remote::A_NONE;
        if (what == Remote::A_LOGIN) {
            rm->state = Remote::LOGIN;
            rm->note = "No answer. Either the password is wrong (it doesn't say), or it didn't hear us: "
                       "try again, or reset the path on the node's page.";
            rm->noteBad = true;
        } else if (what == Remote::A_STATUS)
            rm->statusFailed = true;
        else if (what == Remote::A_NEIGH)
            rm->neighFailed = true;
        else if (what == Remote::A_TELEM)
            rm->telemFailed = true;
        else if (what == Remote::A_ACL)
            rm->aclFailed = true;
        rmRender();
    }
    if (rm && rm->busy && (int32_t)(t - rm->cmdUntil) > 0)
        rmAnswer(nullptr);
    if (rm && rm->rebootArm && (int32_t)(t - rm->rebootArm) > 0) {
        rm->rebootArm = 0;
        if (rm->page == 0)
            rmRender();
    }
}

// ------------------------------------------------------------------ drawing
void App::rmRender(void)
{
    if (!rm)
        return;
    // keep the reader's place (scroll, and the keys' focus) across the rebuild
    int32_t sy = lv_obj_get_scroll_y(rmBody);
    int focusAt = -1;
    {
        std::vector<lv_obj_t *> stops;
        collect(rmOv, stops);
        lv_obj_t *f = lv_group_get_focused(group);
        for (size_t i = 0; i < stops.size(); i++)
            if (stops[i] == f)
                focusAt = (int)i;
    }
    const bool hadRing = focusAt >= 0 && lv_obj_has_state(lv_group_get_focused(group), LV_STATE_FOCUS_KEY);
    if (rm->drawnState != (int)rm->state || rm->drawnPage != rm->page) { // another page: from its top
        focusAt = -1;
        sy = 0;
        rm->drawnState = (int)rm->state;
        rm->drawnPage = rm->page;
    }
    std::string typed = rm->conIn ? lv_textarea_get_text(rm->conIn) : "";
    std::string typedPw = rm->pass ? lv_textarea_get_text(rm->pass) : rm->pw;
    lv_obj_clean(rmBody);
    lv_obj_clean(rmTabs);
    rm->pass = rm->noteLbl = rm->conOut = rm->conIn = rm->readLbl = nullptr;
    for (auto &b : rm->tabBtn)
        b = nullptr;
    rm->ctl.assign(N_RSETS, nullptr);

    lv_obj_t *prefer = nullptr;
    if (rm->state != Remote::IN) {
        lv_obj_add_flag(rmTabs, LV_OBJ_FLAG_HIDDEN);
        prefer = rmLoginPage(typedPw);
    } else {
        lv_obj_remove_flag(rmTabs, LV_OBJ_FLAG_HIDDEN);
        static const char *names[4] = {"Status", "Heard", "Settings", "Console"};
        const bool admin = rm->perms == 3;
        const int32_t tw = (W() - RAIL_W - 2 * PAD - 3 * 4) / 4;
        for (int i = 0; i < 4; i++) {
            if (!admin && i >= 2)
                break; // guests: status only
            lv_obj_t *b = smallButton(rmTabs, names[i], i == rm->page ? C_CYAN : C_MUTED, tw);
            if (i == rm->page) {
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(lv_obj_get_child(b, 0), C_ON_ACCENT, 0);
            }
            lv_obj_add_event_cb(
                b,
                [](lv_event_t *e) { self->rmShowPage((int)(intptr_t)lv_event_get_user_data(e)); },
                LV_EVENT_CLICKED, (void *)(intptr_t)i);
            rm->tabBtn[i] = b;
        }
        if (rm->page == 0)
            rmStatusPage();
        else if (rm->page == 1)
            rmHeardPage();
        else if (rm->page == 2)
            rmSettingsPage();
        else
            prefer = rmConsolePage(typed);
    }

    lv_obj_update_layout(rmOv);
    lv_obj_scroll_to_y(rmBody, sy, LV_ANIM_OFF);
    std::vector<lv_obj_t *> stops;
    collect(rmOv, stops);
    if (prefer)
        refocus(prefer);
    else if (focusAt >= 0 && focusAt < (int)stops.size()) {
        refocus(stops[focusAt]);
        if (!hadRing)
            lv_obj_remove_state(stops[focusAt], LV_STATE_FOCUS_KEY);
    } else
        refocus(rm->tabBtn[rm->page] ? rm->tabBtn[rm->page] : nullptr);
}

// a page of the Manage screen; Heard and Settings ask the node the first time they open
void App::rmShowPage(int p)
{
    if (!rm || rm->state != Remote::IN || rm->page == p)
        return;
    rm->page = p;
    rmRender();
    if (p == 1 && !rm->hasNeigh && rm->asking == Remote::A_NONE)
        rmAsk(Remote::A_NEIGH);
    if (p == 2 && rm->readsLeft == 0 && rm->vals[0].empty())
        rmReadAll();
}

void App::rmFree(void)
{
    delete rm;
    rm = nullptr;
}

lv_obj_t *App::rmLoginPage(const std::string &typed)
{
    char b[160];
    lv_obj_t *c = card(rmBody, rm->type == NodeType::Room ? "LOG IN TO THIS ROOM SERVER" : "LOG IN TO THIS REPEATER");
    wrap(label(c, F12, C_TEXT,
               "Its admin password lets you see its status and change its settings. The guest password shows its status."));
    snprintf(b, sizeof(b), "Route: %s", rm->hops < 0 ? "none yet: the login floods the mesh to find it" : route(rm->hops).c_str());
    wrap(label(c, F12, C_MUTED, b));
    rm->pass = field(c, "Password", 15);
    lv_textarea_set_password_mode(rm->pass, true);
    lv_textarea_set_text(rm->pass, typed.c_str());
    lv_obj_add_flag(rm->pass, F_SUBMIT);
    lv_obj_add_event_cb(rm->pass, [](lv_event_t *) { self->rmLogin(); }, LV_EVENT_READY, nullptr);
    lv_obj_t *sw = nullptr;
    toggle(c, "Keep it on this radio", &sw);
    if (rm->remember)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(
        sw, [](lv_event_t *e) { self->rm->remember = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *r = plain(c);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    row(r, PAD);
    const bool waiting = rm->state == Remote::LOGGING_IN;
    lv_obj_t *go = smallButton(r, waiting ? "Waiting..." : LV_SYMBOL_OK " Log in", C_CYAN, 110);
    lv_obj_add_event_cb(go, [](lv_event_t *) { self->rmLogin(); }, LV_EVENT_CLICKED, nullptr);
    char kept[24];
    if (m.savedPassword(rm->key, kept, sizeof(kept))) {
        lv_obj_t *fg = smallButton(r, "Forget it", C_MUTED, 90);
        lv_obj_add_event_cb(
            fg,
            [](lv_event_t *) {
                self->m.savePassword(self->rm->key, "");
                self->rm->pw.clear();
                self->toast("Password removed from this radio");
                self->rmRender();
            },
            LV_EVENT_CLICKED, nullptr);
    }
    if (!rm->note.empty()) {
        rm->noteLbl = label(c, F12, rm->noteBad ? C_RED : C_AMBER, rm->note.c_str());
        wrap(rm->noteLbl);
    }
    return waiting ? nullptr : rm->pass;
}

void App::rmStatusPage(void)
{
    char b[96], v[48];
    const uint32_t t = lv_tick_get();
    // what it is, and how we're logged in
    lv_obj_t *who = card(rmBody, nullptr);
    static const char *roles[4] = {"guest", "read-only", "read-write", "admin"};
    snprintf(b, sizeof(b), "%s %s, logged in as %s, %s", LV_SYMBOL_OK, nodeTypeName(rm->type), roles[rm->perms & 3],
             route(rm->hops).c_str());
    wrap(label(who, F12, C_GREEN, b));
    stop(who);

    lv_obj_t *s = card(rmBody, "STATUS");
    if (rm->hasStatus) {
        const RemoteStatus &st = rm->st;
        snprintf(b, sizeof(b), "Answered %s ago", durText((t - rm->stAt) / 1000).c_str());
        label(s, F10, C_DIM, b);
        // four tiles
        lv_obj_t *g = plain(s);
        lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(g, 4, 0);
        lv_obj_set_style_pad_row(g, 4, 0);
        const int32_t tw = (W() - RAIL_W - 2 * PAD - 16 - 4) / 2;
        auto tile = [&](const char *k, const char *val, lv_color_t c) {
            lv_obj_t *x = plain(g);
            lv_obj_set_size(x, tw, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(x, C_CARD2, 0);
            lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(x, 8, 0);
            lv_obj_set_style_pad_all(x, 5, 0);
            lv_obj_set_style_border_side(x, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(x, 3, 0);
            lv_obj_set_style_border_color(x, c, 0);
            column(x, 0);
            label(x, F10, C_MUTED, k);
            lv_obj_t *l = label(x, F16, c, val);
            oneLine(l);
        };
        tile("UP", durText(st.uptimeS).c_str(), C_CYAN);
        snprintf(v, sizeof(v), "%.2f V", st.battMv / 1000.0);
        tile("BATTERY", st.battMv ? v : "-", st.battMv && st.battMv < 3500 ? C_RED : C_GREEN);
        snprintf(v, sizeof(v), "%d dBm", st.noiseFloor);
        tile("NOISE FLOOR", st.noiseFloor ? v : "-", st.noiseFloor <= -110 ? C_GREEN : st.noiseFloor <= -100 ? C_AMBER : C_RED);
        snprintf(v, sizeof(v), "%+.1f dB", st.lastSnr4 / 4.0);
        tile("LAST SNR", v, snrColor(st.lastSnr4));
        snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)st.recvFlood, (unsigned long)st.recvDirect);
        kv(s, "Heard (flood / direct)", v, C_TEXT);
        snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)st.sentFlood, (unsigned long)st.sentDirect);
        kv(s, "Sent (flood / direct)", v, C_TEXT);
        snprintf(v, sizeof(v), "%u / %u", (unsigned)st.dupFlood, (unsigned)st.dupDirect);
        kv(s, "Duplicates dropped", v, C_TEXT);
        snprintf(v, sizeof(v), "%d dBm", st.lastRssi);
        kv(s, "Last signal", v, C_TEXT);
        if (st.uptimeS) {
            snprintf(v, sizeof(v), "%.1f%%", 100.0 * st.airtimeS / st.uptimeS);
            kv(s, "Time spent sending", v, st.airtimeS * 10 > st.uptimeS ? C_AMBER : C_TEXT);
            if (st.hasRx) {
                snprintf(v, sizeof(v), "%.1f%%", 100.0 * st.rxAirtimeS / st.uptimeS);
                kv(s, "Time spent receiving", v, C_TEXT);
            }
        }
        snprintf(v, sizeof(v), "%u", (unsigned)st.txQueue);
        kv(s, "Waiting to send", v, st.txQueue > 4 ? C_AMBER : C_TEXT);
        if (st.hasRx) {
            snprintf(v, sizeof(v), "%lu", (unsigned long)st.rxErrors);
            kv(s, "Packets it couldn't read", v, C_TEXT);
        }
        if (st.errFlags) {
            snprintf(v, sizeof(v), "0x%04x", st.errFlags);
            kv(s, "Error flags", v, C_RED);
        }
    } else if (rm->asking == Remote::A_STATUS)
        label(s, F12, C_AMBER, "Asking...");
    else if (rm->statusFailed)
        wrap(label(s, F12, C_RED, "No answer this time."));
    else
        label(s, F12, C_MUTED, "Not asked yet.");
    lv_obj_t *rf = smallButton(s, LV_SYMBOL_REFRESH " Ask again", C_CYAN, 120);
    lv_obj_add_event_cb(rf, [](lv_event_t *) { self->rmAsk(Remote::A_STATUS); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *tc = card(rmBody, "TELEMETRY");
    if (rm->hasTelem) {
        if (rm->telem.empty())
            label(tc, F12, C_MUTED, "It shared nothing.");
        for (auto &l : rm->telem)
            wrap(label(tc, F12, C_TEXT, l.c_str()));
    } else if (rm->asking == Remote::A_TELEM)
        label(tc, F12, C_AMBER, "Asking...");
    else if (rm->telemFailed)
        label(tc, F12, C_RED, "No answer this time.");
    else
        wrap(label(tc, F12, C_MUTED, "Its battery, temperature and any sensors it has."));
    lv_obj_t *tb = smallButton(tc, LV_SYMBOL_DOWNLOAD " Ask for it", C_CYAN, 120);
    lv_obj_add_event_cb(tb, [](lv_event_t *) { self->rmAsk(Remote::A_TELEM); }, LV_EVENT_CLICKED, nullptr);

    if (rm->perms != 3)
        return; // the actions need admin
    lv_obj_t *a = card(rmBody, "ACTIONS");
    lv_obj_t *g = plain(a);
    lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, 4, 0);
    lv_obj_set_style_pad_row(g, 4, 0);
    const int32_t bw = (W() - RAIL_W - 2 * PAD - 16 - 4) / 2;
    struct Act {
        const char *text, *cmd;
        lv_color_t c;
    };
    const Act acts[] = {{LV_SYMBOL_GPS " Flood advert", "advert", C_GREEN},
                        {LV_SYMBOL_WIFI " Local advert", "advert.zerohop", C_GREEN},
                        {LV_SYMBOL_LOOP " Sync its clock", "clock sync", C_CYAN},
                        {LV_SYMBOL_TRASH " Reset counters", "clear stats", C_AMBER}};
    for (auto &x : acts) {
        lv_obj_t *btn = smallButton(g, x.text, x.c, bw);
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t *e) {
                const char *cmd = (const char *)lv_event_get_user_data(e);
                if (!strcmp(cmd, "clock sync") && !self->m.now())
                    return self->toast("This radio's clock isn't set yet (no GPS fix or phone app)");
                self->rmCommand(cmd);
                self->toast("Sent");
            },
            LV_EVENT_CLICKED, (void *)x.cmd);
    }
    const bool armed = rm->rebootArm != 0;
    lv_obj_t *rb = smallButton(g, armed ? "Press again: restart" : LV_SYMBOL_POWER " Restart it", C_RED, bw);
    lv_obj_add_event_cb(
        rb,
        [](lv_event_t *) {
            if (!self->rm->rebootArm) {
                self->rm->rebootArm = lv_tick_get() + 4000;
                return self->rmRender();
            }
            self->rm->rebootArm = 0;
            self->rmCommand("reboot");
            self->rm->state = Remote::LOGIN;
            self->rm->note = "It's restarting. Log in again in a minute.";
            self->rm->noteBad = false;
            self->rmRender();
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lo = smallButton(g, LV_SYMBOL_CLOSE " Log out", C_MUTED, bw);
    lv_obj_add_event_cb(
        lo,
        [](lv_event_t *) {
            self->rm->state = Remote::LOGIN;
            self->rm->note.clear();
            self->rmRender();
        },
        LV_EVENT_CLICKED, nullptr);
}

void App::rmHeardPage(void)
{
    char b[96];
    lv_obj_t *c = card(rmBody, nullptr);
    if (rm->hasNeigh)
        snprintf(b, sizeof(b), "HEARD DIRECTLY: %u, strongest first", (unsigned)rm->nTotal);
    else
        snprintf(b, sizeof(b), "NODES IT HEARS DIRECTLY");
    label(c, F10, C_MUTED, b);
    if (rm->asking == Remote::A_NEIGH)
        label(c, F12, C_AMBER, "Asking...");
    else if (rm->neighFailed)
        wrap(label(c, F12, C_RED, "No answer this time (room servers don't keep this list)."));
    else if (rm->hasNeigh && rm->neigh.empty())
        label(c, F12, C_MUTED, "Nobody yet.");
    for (auto &n : rm->neigh) {
        lv_obj_t *r = plain(c);
        lv_obj_set_size(r, LV_PCT(100), 30);
        row(r, 6);
        Contact ct;
        const bool known = m.findContact(n.key, ct);
        char nm[40];
        if (known)
            snprintf(nm, sizeof(nm), "%s", ct.name);
        else
            snprintf(nm, sizeof(nm), "%02x%02x%02x%02x...", n.key[0], n.key[1], n.key[2], n.key[3]);
        badge(r, n.key, nm, 24);
        lv_obj_t *col = plain(r);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        column(col, 0);
        lv_obj_t *l = label(col, F12, known ? C_TEXT : C_MUTED, nm);
        lv_obj_set_width(l, LV_PCT(100));
        oneLine(l);
        snprintf(b, sizeof(b), "%s ago", durText(n.heardAgoS).c_str());
        label(col, F10, C_DIM, b);
        snprintf(b, sizeof(b), "%+.1f dB", n.snr4 / 4.0);
        pill(r, b, snrColor(n.snr4));
        stop(r);
    }
    if (rm->hasNeigh && rm->nTotal > rm->neigh.size()) {
        snprintf(b, sizeof(b), "+ %u more (one answer holds %u)", (unsigned)(rm->nTotal - rm->neigh.size()), (unsigned)rm->neigh.size());
        label(c, F10, C_DIM, b);
    }
    lv_obj_t *rf = smallButton(c, LV_SYMBOL_REFRESH " Ask again", C_CYAN, 120);
    lv_obj_add_event_cb(rf, [](lv_event_t *) { self->rmAsk(Remote::A_NEIGH); }, LV_EVENT_CLICKED, nullptr);
}

void App::rmReadAll(void)
{
    if (!rm)
        return;
    for (int i = 0; i < N_RSETS; i++) {
        rmCommand(std::string("get ") + RSETS[i].key, i, false);
        rm->readsLeft++;
    }
    rmReadProgress();
}

void App::rmReadProgress(void)
{
    if (!rm || !rm->readLbl)
        return;
    char b[96];
    if (rm->readsLeft > 0)
        snprintf(b, sizeof(b), "Reading %d of %d...", N_RSETS - rm->readsLeft + 1, N_RSETS);
    else
        snprintf(b, sizeof(b), "Change one and press Enter (or pick it): it's sent straight away.");
    lv_label_set_text(rm->readLbl, b);
}

// a settings row's control shows what was read
void App::rmShowValue(int i)
{
    if (!rm || i < 0 || i >= N_RSETS || !rm->ctl[i])
        return;
    const RSet &s = RSETS[i];
    const std::string &v = rm->vals[i];
    lv_obj_t *c = rm->ctl[i];
    loadingSettings = true; // filling a control isn't the user changing it
    if (v == NOT_HERE) {
        lv_obj_add_state(c, LV_STATE_DISABLED);
        if (s.kind == RK_TEXT || s.kind == RK_NUM) {
            lv_textarea_set_text(c, "");
            lv_textarea_set_placeholder_text(c, "not on its firmware");
        }
    } else if (!v.empty()) {
        lv_obj_remove_state(c, LV_STATE_DISABLED);
        if (s.kind == RK_TEXT || s.kind == RK_NUM)
            lv_textarea_set_text(c, v.c_str());
        else if (s.kind == RK_ONOFF) {
            if (v == "on")
                lv_obj_add_state(c, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(c, LV_STATE_CHECKED);
        } else {
            int idx = optIndex(s.vals ? s.vals : s.opts, v.c_str());
            if (idx >= 0)
                lv_dropdown_set_selected(c, idx);
        }
    }
    loadingSettings = false;
}

void App::rmSettingsPage(void)
{
    lv_obj_t *top = card(rmBody, nullptr);
    rm->readLbl = label(top, F12, C_MUTED, "");
    wrap(rm->readLbl);
    rmReadProgress();
    lv_obj_t *rr = smallButton(top, LV_SYMBOL_REFRESH " Read them again", C_CYAN, 150);
    lv_obj_add_event_cb(rr, [](lv_event_t *) { self->rmReadAll(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *c = nullptr;
    for (int i = 0; i < N_RSETS; i++) {
        const RSet &s = RSETS[i];
        if (s.group || !c)
            c = card(rmBody, s.group);
        lv_obj_t *ctl = nullptr;
        if (s.kind == RK_ONOFF) {
            toggle(c, s.label, &ctl);
        } else {
            lv_obj_t *fr = fieldRow(c, s.label);
            if (s.kind == RK_CHOICE)
                ctl = dropdown(fr, s.opts);
            else {
                ctl = field(fr, "-", s.kind == RK_NUM ? 16 : 31);
                lv_obj_add_flag(ctl, F_SUBMIT);
                if (s.kind == RK_NUM)
                    lv_textarea_set_accepted_chars(ctl, "0123456789.-,");
            }
        }
        rm->ctl[i] = ctl;
        lv_obj_add_event_cb(
            ctl,
            [](lv_event_t *e) {
                if (self->loadingSettings || !self->rm)
                    return;
                const int i = (int)(intptr_t)lv_event_get_user_data(e);
                const RSet &s = RSETS[i];
                lv_obj_t *o = lv_event_get_target_obj(e);
                std::string v;
                if (s.kind == RK_ONOFF)
                    v = lv_obj_has_state(o, LV_STATE_CHECKED) ? "on" : "off";
                else if (s.kind == RK_CHOICE)
                    v = optAt(s.vals ? s.vals : s.opts, (int)lv_dropdown_get_selected(o));
                else
                    v = lv_textarea_get_text(o);
                if (v.empty() || v == self->rm->vals[i])
                    return;
                self->rmCommand(std::string("set ") + s.key + " " + v, i, true, v);
            },
            s.kind == RK_TEXT || s.kind == RK_NUM ? LV_EVENT_READY : LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        if (s.help)
            wrap(label(c, F10, C_DIM, s.help));
        rmShowValue(i);
        // extras under some rows
        if (!strcmp(s.key, "lon")) {
            lv_obj_t *b = smallButton(c, LV_SYMBOL_GPS " Use my position", C_CYAN, 150);
            lv_obj_add_event_cb(
                b,
                [](lv_event_t *) {
                    int32_t la, lo;
                    if (!self->m.gpsFix(la, lo))
                        return self->toast("This radio has no position yet");
                    char a[24], o[24];
                    snprintf(a, sizeof(a), "%.6f", la / 1e6);
                    snprintf(o, sizeof(o), "%.6f", lo / 1e6);
                    self->rmCommand(std::string("set lat ") + a, rsetIndex("lat"), true, a);
                    self->rmCommand(std::string("set lon ") + o, rsetIndex("lon"), true, o);
                },
                LV_EVENT_CLICKED, nullptr);
        } else if (!strcmp(s.key, "radio")) {
            lv_obj_t *b = smallButton(c, LV_SYMBOL_LOOP " Try it for 10 minutes", C_AMBER, 180);
            lv_obj_add_event_cb(
                b,
                [](lv_event_t *) {
                    const int ri = rsetIndex("radio");
                    lv_obj_t *t = ri >= 0 ? self->rm->ctl[ri] : nullptr;
                    std::string v = t ? lv_textarea_get_text(t) : "";
                    if (std::count(v.begin(), v.end(), ',') != 3)
                        return self->toast("Type frequency,bandwidth,SF,CR first");
                    self->rmCommand("tempradio " + v + ",10");
                    self->toast("Sent: it uses these for 10 minutes, then goes back by itself");
                },
                LV_EVENT_CLICKED, nullptr);
        }
    }

    // the admin password: no "get"; written with "password <new>"
    lv_obj_t *pc = card(rmBody, "ADMIN PASSWORD");
    lv_obj_t *np = field(pc, "New admin password", 15);
    lv_textarea_set_password_mode(np, true);
    lv_obj_add_flag(np, F_SUBMIT);
    lv_obj_add_event_cb(
        np,
        [](lv_event_t *e) {
            std::string v = lv_textarea_get_text(lv_event_get_target_obj(e));
            if (v.empty() || v.find(' ') != std::string::npos)
                return self->toast("1-15 characters, no spaces");
            self->rmCommand("password " + v);
            lv_textarea_set_text(lv_event_get_target_obj(e), "");
        },
        LV_EVENT_READY, nullptr);
    wrap(label(pc, F10, C_DIM, "Type it and press Enter. If this radio keeps the old one, it keeps the new one instead."));

    // who may log in
    lv_obj_t *ac = card(rmBody, "WHO HAS ACCESS");
    if (rm->asking == Remote::A_ACL)
        label(ac, F12, C_AMBER, "Asking...");
    else if (rm->aclFailed)
        label(ac, F12, C_RED, "No answer this time.");
    else if (rm->hasAcl && rm->acl.empty())
        label(ac, F12, C_MUTED, "Nobody but guests.");
    static const char *roles[4] = {"guest", "read-only", "read-write", "admin"};
    for (size_t i = 0; i < rm->acl.size(); i++) {
        const RemoteClient &cl = rm->acl[i];
        lv_obj_t *r = plain(ac);
        lv_obj_set_size(r, LV_PCT(100), 30);
        row(r, 6);
        Contact ct;
        char nm[40];
        if (m.findContact(cl.key, ct))
            snprintf(nm, sizeof(nm), "%s", ct.name);
        else
            snprintf(nm, sizeof(nm), "%02x%02x%02x%02x%02x%02x", cl.key[0], cl.key[1], cl.key[2], cl.key[3], cl.key[4], cl.key[5]);
        lv_obj_t *l = label(r, F12, C_TEXT, nm);
        lv_obj_set_flex_grow(l, 1);
        oneLine(l);
        pill(r, roles[cl.perms & 3], (cl.perms & 3) == 3 ? C_RED : C_CYAN);
        lv_obj_t *x = smallButton(r, LV_SYMBOL_CLOSE, C_RED, 32);
        lv_obj_add_event_cb(
            x,
            [](lv_event_t *e) {
                size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
                if (!self->rm || i >= self->rm->acl.size())
                    return;
                const uint8_t *k = self->rm->acl[i].key;
                char h[13];
                snprintf(h, sizeof(h), "%02x%02x%02x%02x%02x%02x", k[0], k[1], k[2], k[3], k[4], k[5]);
                self->rmCommand(std::string("setperm ") + h + " 0"); // back to guest: off the list
                self->rm->acl.erase(self->rm->acl.begin() + i);
                self->rmRender();
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *lb = smallButton(ac, LV_SYMBOL_DOWNLOAD " Show the list", C_CYAN, 140);
    lv_obj_add_event_cb(lb, [](lv_event_t *) { self->rmAsk(Remote::A_ACL); }, LV_EVENT_CLICKED, nullptr);
}

lv_obj_t *App::rmConsolePage(const std::string &typed)
{
    lv_obj_t *c = card(rmBody, "ITS COMMAND LINE");
    rm->conOut = label(c, F12, C_TEXT, "");
    wrap(rm->conOut);
    lv_obj_set_style_text_line_space(rm->conOut, 2, 0);
    rm->conIn = field(c, "Command, then Enter", 120);
    lv_textarea_set_text(rm->conIn, typed.c_str());
    lv_obj_add_flag(rm->conIn, F_SUBMIT);
    lv_obj_add_event_cb(
        rm->conIn,
        [](lv_event_t *e) {
            lv_obj_t *t = lv_event_get_target_obj(e);
            std::string v = lv_textarea_get_text(t);
            while (!v.empty() && v.back() == ' ')
                v.pop_back();
            if (v.empty())
                return;
            lv_textarea_set_text(t, "");
            self->rmCommand(v);
        },
        LV_EVENT_READY, nullptr);
    // the ones people use most
    lv_obj_t *q = plain(c);
    lv_obj_set_size(q, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(q, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(q, 4, 0);
    lv_obj_set_style_pad_row(q, 4, 0);
    static const char *quick[] = {"ver", "clock", "neighbors", "get radio", "get tx", "region", "get public.key", "board"};
    for (auto *qc : quick) {
        lv_obj_t *b = smallButton(q, qc, C_MUTED, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_add_event_cb(b, [](lv_event_t *e) { self->rmCommand((const char *)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED,
                            (void *)qc);
    }
    wrap(label(c, F10, C_DIM, "Answers come back one at a time; each command waits for the one before."));
    rmConsoleChanged();
    return rm->conIn;
}

// the console's text, newest last (and scrolled to it)
void App::rmConsoleChanged(void)
{
    if (!rm || !rm->conOut)
        return;
    std::string s;
    const size_t from = rm->log.size() > 14 ? rm->log.size() - 14 : 0;
    for (size_t i = from; i < rm->log.size(); i++) {
        if (!s.empty())
            s += "\n";
        s += rm->log[i];
    }
    if (rm->busy)
        s += "\n...";
    if (s.empty())
        s = "Nothing sent yet. Try ver.";
    lv_label_set_text(rm->conOut, s.c_str());
    lv_obj_update_layout(rmBody);
    if (lv_obj_t *f = lv_group_get_focused(group))
        if (f == rm->conIn)
            lv_obj_scroll_to_view_recursive(rm->conIn, LV_ANIM_OFF);
}

// typing with nothing in focus goes here (the console's command field), or nowhere
lv_obj_t *App::rmTypingTarget(void) const { return rm ? (rm->conIn ? rm->conIn : rm->pass) : nullptr; }
