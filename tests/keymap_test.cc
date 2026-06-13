// Verifies keysym/character/combo resolution. Assertions are layout-robust:
// letters use structural invariants (same key for a/A, shift differs), while
// function keys (Enter/Tab/Esc/Space) use their fixed Linux evdev codes.

#include "keymap.h"

#include <cassert>
#include <cstdio>
#include <linux/input-event-codes.h>

using namespace es;

int main() {
    Keymap km;
    if (!km.ok()) {
        // No xkeyboard-config available (e.g. minimal build host). Skip rather
        // than fail; on a real desktop the keymap is always present.
        std::printf("keymap_test: SKIP (no XKB keymap available)\n");
        return 0;
    }

    KeyStroke a, A;
    assert(km.from_char('a', a));
    assert(km.from_char('A', A));
    assert(a.valid() && A.valid());
    assert(a.keycode == A.keycode);          // same physical key
    assert((a.mods & MOD_SHIFT) == 0);
    assert((A.mods & MOD_SHIFT) != 0);

    // Function keys map to fixed evdev codes regardless of layout.
    KeyStroke k;
    assert(km.from_combo("Return", k));
    assert(k.keycode == KEY_ENTER); // 28
    assert(km.from_combo("space", k));
    assert(k.keycode == KEY_SPACE); // 57
    assert(km.from_combo("Escape", k));
    assert(k.keycode == KEY_ESC); // 1

    // Modifier parsing.
    assert(km.from_combo("ctrl+a", k));
    assert(k.keycode == a.keycode);
    assert(k.mods & MOD_CTRL);
    assert((k.mods & MOD_SHIFT) == 0);

    assert(km.from_combo("Ctrl+Shift+Tab", k));
    assert(k.keycode == KEY_TAB); // 15
    assert(k.mods & MOD_CTRL);
    assert(k.mods & MOD_SHIFT);

    assert(km.from_combo("super+Left", k));
    assert(k.keycode == KEY_LEFT);
    assert(k.mods & MOD_SUPER);

    // Garbage combos fail cleanly.
    assert(!km.from_combo("ctrl+", k));
    assert(!km.from_combo("notakey", k));

    std::printf("keymap_test: PASS\n");
    return 0;
}
