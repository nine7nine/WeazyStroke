# Dev installation checks

Manual steps for testing a fresh RPM install on Fedora.

## 1. Uninstall

Stop running processes and remove the user-level leftovers:

```bash
pkill -x eswl-config; pkill -x eswl-tray; pkill -x eswl-overlay; pkill -x eswl-daemon
rm -f ~/.local/bin/eswl-{config,daemon,overlay,tray}
rm -f ~/.local/share/applications/weazystroke.desktop
rm -f ~/.local/share/icons/hicolor/scalable/apps/weazystroke.svg
```

Remove an install made by an older `./reinstall.sh` (it landed in `/usr/local`; it now installs the RPM):

```bash
sudo xargs rm -v < build-release/install_manifest.txt
```

Remove a previous RPM install:

```bash
sudo dnf remove weazystroke
```

The user config in `~/.config` is not touched. Delete `~/.config/systemd/user/weazystroke.service` too if a clean unit is wanted.

## 2. Build and install the RPM

```bash
cmake -S . -B build-rpm -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
cmake --build build-rpm
(cd build-rpm && cpack -G RPM)

sudo dnf install ./build-rpm/weazystroke-0.1.0-1.x86_64.rpm
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Requires `rpm-build`. The user must be in the `input` group (`sudo usermod -aG input "$USER"`, then log in again).

## 3. Post-installation checks

Installed files:

```bash
hash -r
which eswl-config        # /usr/bin/eswl-config
rpm -q weazystroke
rpm -ql weazystroke
```

GUI (`eswl-config`, Preferences, page 3):

- Both **Start WeazyStroke on login** and **Show stroke trail overlay** are listed under System.
- On GNOME (`XDG_CURRENT_DESKTOP=GNOME eswl-config`) the overlay checkbox is greyed out and unchecked.
- Toggling **Start on login** creates `~/.config/systemd/user/weazystroke.service` with `ExecStart=/usr/bin/eswl-daemon [--overlay] --tray`.

Service:

```bash
systemctl --user status weazystroke.service       # active (running), enabled
systemctl --user cat weazystroke.service          # check ExecStart
pgrep -a eswl                                     # daemon, tray, overlay (if on)
journalctl --user -u weazystroke.service -b       # add -f to follow
```

After editing the unit by hand:

```bash
systemctl --user daemon-reload && systemctl --user restart weazystroke.service
```

Gestures:

- Draw a gesture with the configured trigger button; its action should run.
- With the overlay on (KDE), the trail should be visible while drawing.
- Troubleshooting: `Permission denied` on `/dev/input/*` or `/dev/uinput` means the `input` group or the udev rule is not applied.
