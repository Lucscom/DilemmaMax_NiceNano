# Dilemma Max — ZMK firmware

Split 5×6 keyboard on two nice!nano v2 controllers, built with ZMK v0.3.
Left half is the central and carries a nice!view display, right half is the
peripheral and carries a Procyon trackpad. Both halves have a 51 LED underglow
strip and an EC11 encoder footprint.

Firmware is built by GitHub Actions on every push; download the artifacts from
the run and flash the `.uf2` files over USB.

---

## Hardware

| Part | Detail |
|---|---|
| Controllers | nice!nano v2, one per half |
| Matrix | 5 rows × 6 columns per half, row2col diodes |
| Display | nice!view on the left half, SPI (SCK P0.17, MOSI P0.20, CS P0.06) |
| Underglow | 51 SK6805/WS2812 via SPIM3, MOSI P1.04, GRB order |
| Encoder | EC11 on A P1.00 / B P0.11, disabled by default |
| Trackpad | Procyon 42×50 on the right half, see below |

### Trackpad

The pad is George Norton's open hardware [Procyon 42×50](https://github.com/george-norton/procyon),
not a Cirque product — the connector on the PCB is just labelled "Cirque". It
carries a Microchip mXT336UD touchscreen controller.

| Property | Value |
|---|---|
| Bus | I²C on TWIM1, SDA P0.22, SCL P0.24, 100 kHz |
| Address | `0x4a` |
| CHG (data ready) | P0.06, active low, drives the driver by interrupt |
| Power | switched VCC pin of the nice!nano |
| Sensor matrix | 10 X lines × 12 Y lines, 3.8 mm pitch (chip maximum is 14×24) |
| Pull-ups | 3.3 kΩ on the pad plus 4.7 kΩ on the keyboard PCB |

**The cover plate matters more than any firmware setting.** A thin PLA plate
(0.5–1.6 mm) glued down flat with double sided tape is required. With an air
gap the finger *increases* the coupling between the electrodes instead of
draining it, the controller reports it as anti-touch, recalibrates with the
finger present and then tracks a ghost image. Days of this project were spent
chasing that as if it were a software bug.

---

## Repository layout

```
boards/shields/dilemma_max/    shield definition, keymap, overlays
config/                        west manifest and per-side Kconfig
src/                           RGB split helpers, the nice!view status screen, the
                               charge reporting and the gesture key processor,
                               compiled into the ZMK app
drivers/input/                 the maXTouch trackpad driver, including all gestures
dts/bindings/                  devicetree bindings for the driver and the processor
include/dt-bindings/           gesture codes shared by driver, processor and devicetree
Kconfig, CMakeLists.txt        make this repo a Zephyr module for those sources
build.yaml                     GitHub Actions build matrix
cirque_test/                   Arduino sketches used to bring the pad up
```

The trackpad driver started out in a separate repository
(philippkober/maxtouch-zephyr-module, forked from george-norton's module) and
now lives in `drivers/input/`. Changes to gestures or driver behaviour go
there, changes to tuning values go into the right side overlay.

---

## Branches and the layout workflow

Two people build from this repository with different keymaps. To keep a merge
from ever overwriting someone's layout, the layout lives only in the layout
branches and `main` stays the shared base.

| Branch | Contains |
|---|---|
| `main` | Shared work: trackpad, RGB, configuration. Keymap is LM's layout. |
| `LM_Layout` | LM's layout, identical to `main` |
| `PK_Layout` | PK's German QWERTZ layout with umlauts |

Rules that keep this working:

1. **Never commit a keymap change to `main`.** Anything else — driver tuning,
   configuration, hardware fixes — belongs there.
2. **Build and flash from your own layout branch**, not from `main`.
3. **To pick up shared work**, merge `main` into your layout branch:
   `git checkout PK_Layout && git merge main`. Git reports a conflict in the
   keymap only if rule 1 was broken; keep your own side in that case.
4. **To share work you did on a layout branch**, cherry-pick or merge only the
   non-keymap commits into `main`.

Watch out for one trap: if a layout branch is an ancestor of the branch being
merged, git fast-forwards and silently replaces the keymap. Check with
`git diff main..HEAD -- '*keymap*'` after merging — the German layout shows up
as a non-empty diff on `PK_Layout`.

---

## Building and flashing

Push to any branch, then open the run under
[Actions](https://github.com/Lucscom/DilemmaMax_NiceNano/actions) and download
the firmware artifact. Put a half into bootloader mode by double tapping reset
and copy the matching `.uf2` onto the USB drive that appears.

Which halves need flashing depends on what changed:

| Changed file | Flash |
|---|---|
| `dilemma_max_right.overlay`, `drivers/input/` | right only |
| `src/gesture_keys.c` | left only |
| `dilemma_max.dtsi` (including the gesture shortcuts), `Kconfig.shield`, `dilemma_max.keymap` | both |
| `include/dt-bindings/dilemma_max/gestures.h` | both |
| `config/dilemma_max_left.conf` and split parameters | both |

### USB logging

The right half is built with the `zmk-usb-logging` snippet, so it prints to a
USB serial console. Log output is held back for 15 seconds after boot so the
init messages survive until the console is attached:

```
cd ~/Developer/DilemmaMax_NiceNano && screen -L /dev/tty.usbmodem* 115200
```

Start it within those 15 seconds. `-L` writes `screenlog.0` in the working
directory. Quit with `Ctrl+A`, `K`, `y`.

To see raw sensor data, uncomment `diag-dump` in the trackpad node. The driver
then logs the T37 reference values once after calibration and the per-node
deltas every three seconds — that is how the signal level, the noise floor and
the merged-finger area thresholds were measured.

---

## Trackpad integration in ZMK

A pointing device on a split **peripheral** needs three pieces, and missing the
first one was the original reason nothing worked at all:

1. `dilemma_max.dtsi` declares a `zmk,input-split` node plus a disabled
   `zmk,input-listener` that points at it.
2. `dilemma_max_right.overlay` attaches the physical `trackpad` node to that
   input split, so the peripheral forwards events over the split link.
3. `dilemma_max_left.overlay` enables the listener, so the central turns those
   events into HID reports.

Cursor speed is the scaler on the listener in `dilemma_max.dtsi`
(`&zip_xy_scaler 2 5` — a larger second number is slower). The logical
resolution in `Kconfig.shield` is deliberately fine (2048 × 2438 counts, about
48.8 counts/mm on both axes) so that the controller's own movement hysteresis
and the scroll step do not become coarse.

### Gestures

Implemented in `drivers/input/input_maxtouch.c`, not in ZMK. macOS accepts no
precision touchpad from a third-party device, so everything is recognised in
the firmware and sent as mouse events or key shortcuts:

| Gesture | Action |
|---|---|
| One finger | Move the cursor |
| Tap | Left click |
| Double tap | Double click |
| Tap, then touch and move | Drag |
| Two fingers moving in parallel | Scroll, one step per ~1.6 mm, with a short momentum after lift |
| Two fingers moving apart / together | Zoom in / out, one step per ~4.5 mm (`Cmd+=` / `Cmd+-`) |
| Two finger tap | Right click |
| Two finger flick sideways | Mouse buttons 4/5 — page back/forward in Safari |
| Three finger swipe left / right | Next / previous space (`Ctrl+→` / `Ctrl+←`) |
| Three finger swipe up / down | Mission Control / App Exposé (`Ctrl+↑` / `Ctrl+↓`) |
| Three finger tap | Middle click |

Two fingers settle on scrolling or zooming once per touch, after about 0.8 mm
of movement, so a gesture cannot flip between the two. A three finger swipe
needs about 6 mm in one clear direction within 700 ms and fires once per
touch. Rotation is not implemented.

**Do not route trackpad events into key behaviours.** Mapping button codes to
`&kp` shortcuts through `zip_button_behaviors` crashed *both* halves as soon
as the gesture fired. The likely reason: input processors run in the Zephyr
input thread, whose stack stays at the Zephyr default of 512 bytes on the
central (ZMK only raises it on peripherals), and a behaviour runs the whole
keymap and HID chain synchronously in there.

Shortcuts therefore take a separate path. The driver reports a recognised
gesture as a key press and release with its own code from
`include/dt-bindings/dilemma_max/gestures.h` (`DM_GESTURE_*`, above
`INPUT_BTN_4`, so the listener never mistakes them for mouse buttons). The
split link forwards type, code and value unchanged. On the left half the input
processor `dilemma,input-processor-gesture-keys` (`src/gesture_keys.c`, first
in the listener's `input-processors`) catches these codes, only queues the
keycode in the input thread and raises press and release as
`keycode_state_changed` events on the system work queue, 30 ms apart. No
behaviour, key position or keymap is involved.

The shortcut for each gesture is set on the `gesture_keys` node at the end of
`dilemma_max.dtsi`:

| Property | Meaning |
|---|---|
| `codes` | Gesture codes to catch (`DM_GESTURE_*`) |
| `keycodes` | Shortcut per code, same order, ZMK keycodes such as `LC(RIGHT)` |

The zoom shortcuts assume a US layout on the Mac. With a German host layout
`+` sits on `RBKT` and `-` on `FSLH`; override the node from the keymap of a
layout branch:

```
&gesture_keys {
    keycodes = <LC(RIGHT) LC(LEFT) LC(UP) LC(DOWN) LG(RBKT) LG(FSLH)>;
};
```

`keys.h` has to be included after the matrix transform in `dilemma_max.dtsi`:
its `RC(keycode)` would otherwise replace the transform's `RC(row, col)`.

### Driver behaviour worth knowing

- **Interrupt driven.** The driver waits for CHG and only reads when the
  controller signals data, with a 500 ms safety poll. An earlier version polled
  every 8 ms around the clock, which cost battery for nothing.
- **Deferred probe.** The chip is powered from the switched VCC rail and needs
  a moment after power-on, so the driver retries every 500 ms until the object
  table reads back.
- **Jump filter.** Steps above 150 counts in one report are dropped, as is the
  first report after any change in finger count. When a second finger
  approaches, the controller shifts the centroid of the single reported touch
  by up to 340 counts before reporting the finger separately — that was the
  cursor flying away at the start of a scroll.
- **Lift-off suppression.** Movement is held back for up to five reports when
  the amplitude drops by 20 % or the area shrinks by 25 %, and discarded if the
  finger lifts. Disabled during fast movement, where it caused jumps itself.
- **Merged fingers.** Two fingers close together are reported as one touch.
  A contact counts as merged when its area reaches 24, or reaches 16 after
  growing by half since touch down, and then drives scrolling rather than the
  cursor. The growth check only applies to a finger that is already down; at
  touch down only the absolute threshold counts, since the reference area
  otherwise still belonged to the previous touch.
- **Two-finger scrolling.** Both fingers collect their movement and are
  evaluated once per message batch: the scroll step is the mean of both paths,
  and only while they point the same way (positive dot product, lengths within
  3:1). Pinching or rotating does not scroll.
- **Stale fingers.** If the T100 status message reports zero touches while
  fingers are still active, an UP got lost. The check waits for the next
  status message or 50 ms, because the UPs of the same cycle arrive after the
  status, and then resets all finger, gesture and cursor state and releases a
  held button without any click.

### Tuning values

The values in the trackpad node come from george-norton's QMK driver
(`drivers/sensors/procyon.h` and `drivers/sensors/maxtouch.c` on the
`multitouch_experiment` branch), which configures this exact pad for the
Dilemma 4×6. They belong together: threshold 20 only works alongside
`charge_time = 1`, `gain = 10` and both measurement types enabled. Raising the
touch threshold without those turns touch reporting off completely — a mistake
made twice in this project.

---

## Display

The nice!view on the left half runs a custom status screen
(`src/status_screen.c`) instead of the widget that ships with the `nice_view`
shield. Read upright, top to bottom:

| Area | Shows |
|---|---|
| Top | Battery of both halves (`L`, `R`) as a small bar with percentage. A bolt after the left bar means USB is powering the left half. A small cross replaces the right bar while the right half is not connected (ZMK reports 0 % on disconnect and before the first report). Below: USB or a Wi-Fi style symbol for Bluetooth with the profile number, and the state `WIRED`, `ONLINE`, `WAITING` (paired host not connected) or `PAIRING` (profile is empty). |
| Middle | Held modifiers `SHFT`, `CTRL`, `OPT`, `CMD` (left and right combined), each inverted while held. |
| Bottom | Active layer as an inverted band, taken from `display-name` in the keymap. |

ZMK v0.3 has no channel for the charging state of the peripheral: the right
half only reports its battery level, as a plain BAS value the central
subscribes to. `src/peripheral_charge_report.c` therefore encodes charging in
the parity of that value — odd means charging — which costs one percent of
accuracy and skews the level by at most 1. The right half reads USB power
straight from the nRF52840 VBUS detection, because `CONFIG_ZMK_USB` is not
allowed on a peripheral. Both halves have to be flashed together: with an older
firmware on the right, the central reads every odd level as charging. The
decoding side is `CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_DECODE`, which can be
turned off in the meantime.

Anything diagonal — the cross, the charging bolt — is set pixel by pixel with
1×1 rectangles. `lv_canvas_draw_line` anti-aliases, and at one bit per pixel
every partly covered pixel falls back to the background, which makes a 1 px
diagonal invisible on this panel. ZMK also never raises `zmk_modifiers_state_changed`,
so the modifier area listens to every keycode event and redraws only when the
modifiers actually changed.

It is switched on in `config/dilemma_max_left.conf` with
`CONFIG_NICE_VIEW_WIDGET_STATUS=n`, which lets `CONFIG_DILEMMA_MAX_STATUS_SCREEN`
default to on. That option also turns on
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING` so the central collects the
right half's level. Like the stock widget, each block is drawn upright on a
68×68 canvas and rotated 90°, because the 160×68 panel stands on its side; only
the top 24 px of the bottom block fit on the panel.

---

## Power

Battery life is dominated by the 51 LEDs. Everything else is a rounding error
by comparison, so the first lever is the underglow, not the firmware.

The right half no longer switches its own underglow off on idle. It follows the
central instead (`src/rgb_split_sync.c`, `src/rgb_split_follow.c`), because
ZMK's activity state is local: a peripheral only sees its own key presses and
went dark while typing on the left half. `src/rgb_sleep_blank.c` clears the
strip synchronously before deep sleep, since WS2812 hold their last state once
the controller stops sending.

Split connection latency is left at the ZMK default. Lowering it makes
underglow commands arrive faster but forces the peripheral to listen three
times as often, all day.

---

## Bring-up history worth remembering

Problems that cost the most time, so they are not repeated:

- **Missing `zmk,input-split`.** The listener ran on the peripheral, which never
  sends HID reports. Roughly twenty "trigger rebuild" commits went into driver
  internals before this was noticed.
- **USB logging needs the snippet.** `CONFIG_ZMK_USB_LOGGING=y` alone does not
  build in ZMK v0.3; the `zmk-usb-logging` snippet supplies the CDC ACM node.
- **TWIM concatenation buffer.** Configuration writes are longer than 16 bytes,
  so `zephyr,concat-buf-size = <128>` is required on the I²C node.
- **A logic analyser on the bus.** An unpowered XIAO attached to SDA, SCL and
  GND clamped the bus and produced I²C timeouts that looked like a dead chip.
- **The air gap.** See the trackpad section — by far the largest time sink.
