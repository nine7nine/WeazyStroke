#include "keymap.h"
#include "input_injector.h"

#include <xkbcommon/xkbcommon.h>

#include <linux/input-event-codes.h>

#include <cctype>

namespace es {

namespace {

// X11/XKB keycodes are Linux evdev keycodes plus 8 (the evdev ruleset).
constexpr uint32_t kXkbEvdevOffset = 8;

int decode_utf8(const std::string &s, size_t i, uint32_t &cp) {
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = byte(i);
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) {
        cp = ((c & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu);
        return 2;
    }
    if ((c >> 4) == 0xE && i + 2 < s.size()) {
        cp = ((c & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) | (byte(i + 2) & 0x3Fu);
        return 3;
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) {
        cp = ((c & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) | ((byte(i + 2) & 0x3Fu) << 6) |
             (byte(i + 3) & 0x3Fu);
        return 4;
    }
    return 0;
}

} // namespace

Keymap::Keymap() {
    ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx_)
        return;
    // names == nullptr -> default rules + XKB_DEFAULT_* environment.
    keymap_ = xkb_keymap_new_from_names(ctx_, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap_)
        return;

    xkb_mod_index_t shift = xkb_keymap_mod_get_index(keymap_, XKB_MOD_NAME_SHIFT);
    xkb_mod_index_t altgr = xkb_keymap_mod_get_index(keymap_, "Mod5"); // AltGr / Level3
    shift_idx_ = shift == XKB_MOD_INVALID ? -1 : static_cast<int>(shift);
    altgr_idx_ = altgr == XKB_MOD_INVALID ? -1 : static_cast<int>(altgr);

    build_cache();
}

Keymap::~Keymap() {
    if (keymap_)
        xkb_keymap_unref(keymap_);
    if (ctx_)
        xkb_context_unref(ctx_);
}

unsigned Keymap::mods_for_level(uint32_t keycode, uint32_t layout, uint32_t level) const {
    xkb_mod_mask_t masks[8];
    size_t n = xkb_keymap_key_get_mods_for_level(keymap_, keycode, layout, level, masks, 8);
    unsigned mods = 0;
    if (n > 0) {
        xkb_mod_mask_t mask = masks[0]; // simplest combination that reaches the level
        if (shift_idx_ >= 0 && (mask & (1u << shift_idx_)))
            mods |= MOD_SHIFT;
        if (altgr_idx_ >= 0 && (mask & (1u << altgr_idx_)))
            mods |= MOD_ALTGR;
    }
    return mods;
}

void Keymap::build_cache() {
    xkb_keycode_t min = xkb_keymap_min_keycode(keymap_);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap_);
    for (xkb_keycode_t kc = min; kc <= max; ++kc) {
        if (kc < kXkbEvdevOffset)
            continue;
        if (xkb_keymap_num_layouts_for_key(keymap_, kc) == 0)
            continue;
        const xkb_layout_index_t layout = 0;
        xkb_level_index_t levels = xkb_keymap_num_levels_for_key(keymap_, kc, layout);
        for (xkb_level_index_t lvl = 0; lvl < levels; ++lvl) {
            const xkb_keysym_t *syms = nullptr;
            int n = xkb_keymap_key_get_syms_by_level(keymap_, kc, layout, lvl, &syms);
            if (n <= 0)
                continue;
            unsigned mods = mods_for_level(kc, layout, lvl);
            for (int s = 0; s < n; ++s) {
                // First insertion wins, so lower levels (fewer mods) are preferred.
                KeyStroke ks;
                ks.keycode = static_cast<uint16_t>(kc - kXkbEvdevOffset);
                ks.mods = mods;
                cache_.emplace(static_cast<uint32_t>(syms[s]), ks);
            }
        }
    }
}

bool Keymap::from_keysym(uint32_t keysym, KeyStroke &out) const {
    auto it = cache_.find(keysym);
    if (it == cache_.end())
        return false;
    out = it->second;
    return true;
}

bool Keymap::from_char(uint32_t codepoint, KeyStroke &out) const {
    if (!keymap_)
        return false;
    xkb_keysym_t sym = xkb_utf32_to_keysym(codepoint);
    if (sym == XKB_KEY_NoSymbol)
        return false;
    return from_keysym(sym, out);
}

bool Keymap::from_combo(const std::string &spec, KeyStroke &out) const {
    if (!keymap_)
        return false;

    unsigned mods = 0;
    std::string base;

    size_t start = 0;
    for (size_t i = 0; i <= spec.size(); ++i) {
        if (i != spec.size() && spec[i] != '+')
            continue;
        std::string tok = spec.substr(start, i - start);
        start = i + 1;
        // trim surrounding spaces
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front())))
            tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())))
            tok.pop_back();
        if (tok.empty())
            continue;

        std::string low;
        for (char c : tok)
            low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (low == "ctrl" || low == "control")
            mods |= MOD_CTRL;
        else if (low == "shift")
            mods |= MOD_SHIFT;
        else if (low == "alt")
            mods |= MOD_ALT;
        else if (low == "super" || low == "meta" || low == "win" || low == "cmd" || low == "logo")
            mods |= MOD_SUPER;
        else if (low == "altgr" || low == "iso_level3_shift")
            mods |= MOD_ALTGR;
        else
            base = tok; // the last non-modifier token is the key
    }

    if (base.empty())
        return false;
    xkb_keysym_t sym = xkb_keysym_from_name(base.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol)
        return false;
    KeyStroke base_ks;
    if (!from_keysym(static_cast<uint32_t>(sym), base_ks))
        return false;
    out.keycode = base_ks.keycode;
    out.mods = mods; // combo semantics: explicit modifiers only
    return out.valid();
}

void send_keystroke(InputInjector &inj, const KeyStroke &ks) {
    if (!ks.valid())
        return;
    struct ModKey {
        unsigned flag;
        uint16_t code;
    };
    // Press low-level modifiers first, release in reverse order.
    static const ModKey order[] = {
        {MOD_CTRL, KEY_LEFTCTRL},   {MOD_ALT, KEY_LEFTALT},   {MOD_SUPER, KEY_LEFTMETA},
        {MOD_ALTGR, KEY_RIGHTALT},  {MOD_SHIFT, KEY_LEFTSHIFT},
    };
    constexpr int count = sizeof(order) / sizeof(order[0]);
    for (int i = 0; i < count; ++i)
        if (ks.mods & order[i].flag)
            inj.key(order[i].code, true);
    inj.key(ks.keycode, true);
    inj.key(ks.keycode, false);
    for (int i = count - 1; i >= 0; --i)
        if (ks.mods & order[i].flag)
            inj.key(order[i].code, false);
    inj.flush();
}

void type_text(const Keymap &km, InputInjector &inj, const std::string &utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        int len = decode_utf8(utf8, i, cp);
        if (len <= 0) {
            ++i;
            continue;
        }
        i += static_cast<size_t>(len);
        KeyStroke ks;
        if (km.from_char(cp, ks))
            send_keystroke(inj, ks);
    }
}

} // namespace es
