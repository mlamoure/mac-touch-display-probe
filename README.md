# TouchProbe: a Mac touch display and a QEMU-style view

A small test app for the MacBook Pro with a touch display (macOS 27). It shows
what AppKit delivers to a view built like QEMU's `QemuCocoaView`, and it runs
the touch rules proposed for [OmacVM](https://github.com/gillesgoetsch/OmacVM)
so they can be tried on the hardware. Issue:
[gillesgoetsch/OmacVM#843](https://github.com/gillesgoetsch/OmacVM/issues/843).

The data in `results/` comes from one M5 MacBook Pro with the touch display,
macOS 27.2.1 (26B8132), built-in display 2064 × 1347 pt at 2×.

## Build and run

```sh
./build.sh            # builds build/TouchProbe.app (Xcode or its Command Line Tools)
open build/TouchProbe.app
python3 analyze.py ~/Library/Logs/touch-probe-<date>-<time>.tsv
```

The app is signed ad hoc and installs nothing. It only observes: it posts no
events and changes no settings. Each run writes a new log to `~/Library/Logs/`.
Follow the steps on screen with **Next**. **Skip to the fix** (or the F key)
goes straight to step F, and so does `open build/TouchProbe.app --args --fix`.

The log's `tap` rows come from a listen-only session event tap. They show
whether an event passed through the session's event stream. If macOS refuses
the tap, the log says `tap unavailable` and every other row is still written.

## The steps

| Step | What it shows |
| --- | --- |
| A | A plain view that overrides the mouse methods, as `QemuCocoaView` does: what AppKit's mouse emulation makes of a tap, a scroll, a hold-drag, a long press, a pinch and two fingers. |
| B | The raw direct touches, from a gesture recognizer that only logs. |
| C | AppKit's own recognizers (click, pan, press, magnify): whether they take a touch away from the emulation. |
| D | The cursor detached, the way OmacVM's full-screen corner guard detaches it. |
| E | Full screen: taps and swipes at the screen's edges. |
| F | The fix. `omacvm-touch.h` is compiled in and driven the same way OmacVM's `ui/cocoa.m` patch drives it. The view draws what the guest would get: green dots for clicks, scrolling stripes, an orange line for a drag, purple rings for right clicks. |

`omacvm-touch.h` is the header that OmacVM's `omacvm-cocoa-touch-logic.patch`
adds to QEMU, copied unchanged. Its 49-check unit test lives with the patch in
OmacVM (`app/runtime/Tests/touch/`).

## What the hardware showed (steps A–E, `results/1-…`)

- **A tap:** the view gets an emulated left click (A1: 5 taps, 5 down/up pairs).
  The down and up arrive together at the lift, about 0.2 ms apart.
- **A touch that moves:** a left-button drag (A2: 3 drags, 160 `mouseDragged`
  events). In a VM, that selects text or tears off a browser tab instead of
  scrolling. **No scroll events ever come.**
- **A hold:** after about 730 ms the emulation sends a right button, and moving
  after that is a right-button drag (A3, A4).
- **A pinch:** magnify and rotate events (A5). Two fingers dragging: magnify,
  rotate and one left drag (A6).
- **Event origin:** the emulated events are made inside the app (the app's own
  pid, source state 0). A session event tap never sees them, and their deltas
  are measured from the real cursor, which never moves.
- **Recognizers:** a direct-touch recognizer that begins at touch-down suppresses
  the emulation (C1, C2: no emulated events). A press recognizer loses the race
  to the right-button emulation (C3, C4), so the fix uses one recognizer of its
  own that begins at touch-down.
- **Full screen:** an emulated drag still arrives at the edges (E1–E3).

## Step F runs (`results/2-…` to `4-…`)

Each run's `.summary.txt` is `analyze.py`'s output for its `.tsv`. In all three
runs, **no emulated button event reached the view during step F** (the
summaries' "emulated button events in a fix step: 0").

| Run | What happened | What changed after it |
| --- | --- | --- |
| 1 | Taps, scrolling and flicks worked. Holds never registered (F3: 8 hold-drags all became scrolls; F4: 2 long presses became clicks). A double tap's second tap landed several points from the first. | The hold timers were in the default run-loop mode, which AppKit leaves while it tracks touches; they moved to the common modes. A second tap now snaps to the first tap's place. |
| 2 | Hold-drags worked, but the fixed-time right click was too quick: 2 of the 4 hold-drags in F3 also got a right click. A two-finger scroll jumped when one finger lifted. | Right click moved to the release (as on Windows), and moving after the hold is always a drag. Two-finger scrolling now uses each finger's own movement and ends when the first finger lifts. |
| 3 | Everything felt right: F3's hold-drags were clean (fingers started moving 630–790 ms in), F4 gave 2 right clicks on release, and F6's 6 taps made 3 double clicks, each second tap snapped up to 26 pt onto the first. | The rules were written as `omacvm-touch.h` for the OmacVM patch. |

Runs 1–3 used the app's earlier versions, which had the rules in Swift. That is
why their `fix` rows name actions such as `fling` and `longPressEnd`. This
version runs the final C header. Its own hardware run is still to come and
will be added here.

## OmacVM tests on the Mac (`results/mac-tests-2026-11-21.txt`)

Run on the same MacBook Pro, on the two OmacVM fix branches:

- **Gestures helper** (`fix/gestures-skip-touch-display`): every test passes,
  including the new case. The touch display (HID usage page 0x0D, usage 0x04) is
  not taken as a trackpad, and the VM keeps the built-in trackpad's scaling.
- **Touch rules and wiring** (`feature/touch-display`):
  - The rules' unit test (49 checks) passes.
  - Every wiring check fails on `ui/cocoa.m` without the patch, as it should.
  - QEMU built from source with all patches applied. The touch tests pass inside
    that build, and no compiler warning comes from the touch code.

## License

MIT (see `LICENSE`). `omacvm-touch.h` is part of a contribution to OmacVM,
which is also MIT-licensed.
