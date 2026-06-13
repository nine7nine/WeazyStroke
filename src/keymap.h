#pragma once
// Translates keysyms / Unicode characters / "ctrl+shift+t" specs into evdev
// keystrokes (keycode + modifiers) that the uinput injector can emit.
//
// Built from the system default XKB layout via xkbcommon (honoring XKB_DEFAULT_*
// env vars). NOTE: injection happens below the compositor, which re-applies its
// own layout to our keycodes, so for correct text the daemon's layout should
// match the compositor's. Syncing with the compositor's active layout is a
// later refinement; the system default is a sound v0.

#include <cstdint>
#include <string>
#include <unordered_map>

struct xkb_context;
struct xkb_keymap;

namespace es {

class InputInjector;

enum Mod : unsigned {
    MOD_NONE  = 0,
    MOD_SHIFT = 1u << 0,
    MOD_CTRL  = 1u << 1,
    MOD_ALT   = 1u << 2,
    MOD_SUPER = 1u << 3,
    MOD_ALTGR = 1u << 4,
};

struct KeyStroke {
    uint16_t keycode = 0; // evdev keycode; 0 means unresolved
    unsigned mods = 0;    // Mod flags to hold while tapping
    bool valid() const { return keycode != 0; }
};

class Keymap {
public:
    Keymap();
    ~Keymap();
    Keymap(const Keymap &) = delete;
    Keymap &operator=(const Keymap &) = delete;

    // False if no XKB keymap could be compiled (e.g. xkeyboard-config missing).
    bool ok() const { return keymap_ != nullptr; }

    bool from_keysym(uint32_t keysym, KeyStroke &out) const;
    bool from_char(uint32_t codepoint, KeyStroke &out) const;
    bool from_combo(const std::string &spec, KeyStroke &out) const; // "ctrl+shift+t"

private:
    void build_cache();
    unsigned mods_for_level(uint32_t keycode, uint32_t layout, uint32_t level) const;

    xkb_context *ctx_ = nullptr;
    xkb_keymap *keymap_ = nullptr;
    int shift_idx_ = -1;
    int altgr_idx_ = -1;
    std::unordered_map<uint32_t, KeyStroke> cache_; // keysym -> stroke
};

// Bridge Keymap output to the injector primitives.
void send_keystroke(InputInjector &inj, const KeyStroke &ks);
void type_text(const Keymap &km, InputInjector &inj, const std::string &utf8);

} // namespace es
