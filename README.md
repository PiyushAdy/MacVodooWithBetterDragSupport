# VoodooInput — `libinput`-Style Tap-to-Click & Tap-and-Drag for macOS

A fork of [Acidanthera's VoodooInput](https://github.com/acidanthera/VoodooInput) that adds a kernel-level, [`libinput`](https://gitlab.freedesktop.org/libinput/libinput)-inspired tap-to-click and 1-finger double-tap-and-drag state machine directly inside `VoodooInputSimulatorDevice`.

## Why This Fork Exists

On many Hackintosh I2C/PS2 trackpads (such as ELAN and Synaptics pads running `VoodooI2C` or `VoodooPS2`), macOS's built-in **Use trackpad for dragging** (*Without Drag Lock* / *With Drag Lock*) either fails to grab windows reliably or forces you to enable **Three-Finger Drag**, which conflicts with native 3-finger Mission Control and Spaces swipes.

This fork brings Windows Precision Touchpad / Linux `libinput` ergonomics to macOS without needing any background userspace daemon:

- **Single-tap to click**, **double-tap to double-click**, and **triple-tap** work natively.
- **1-finger double-tap & slide** grabs windows, drags files, and selects text on the first try (`clickCount == 1`), with **0 ms drop** in the center of the trackpad and **300 ms edge continuation** when you lift near the border.
- **2-finger tap** triggers right-click cleanly.
- **All native macOS multitouch gestures** (2-finger scrolling/pinch-zoom, 3-finger Mission Control/Spaces, 4-finger Launchpad/App Exposé, and physical clickpad presses) remain untouched.

## Gesture Summary

| Gesture | Behavior |
| :--- | :--- |
| **1-finger tap** | Left-click |
| **1-finger double-tap** | Double-click |
| **1-finger triple-tap** | Triple-click |
| **1-finger double-tap & slide** | Click-and-drag (instant grab after ~0.85 mm of motion) |
| **Lift in center while dragging** | Instant release (0 ms delay) |
| **Lift near trackpad edge (~10% border) while dragging** | Holds drag for **300 ms** so you can reposition your finger and continue dragging |
| **2-finger tap** | Right-click (secondary click) |
| **2 / 3 / 4-finger swipes & pinches** | Native macOS gestures (unchanged) |
| **Physical clickpad press** | Native hardware click (cancels any active tap state) |

## Required macOS Settings

Because `VoodooInput.kext` now handles tap-to-click, double-tap, tap-and-drag, and 2-finger tap directly at the HID report level, configure macOS Trackpad settings as follows to avoid duplicate clicks:

1. **System Settings → Trackpad → Point & Click**
   - **Tap to click**: **OFF**
   - **Secondary click**: **Click with two fingers** (**ON**)
2. **System Settings → Accessibility → Pointer Control → Trackpad Options**
   - **Use trackpad for dragging**: **OFF**

> **Why must macOS "Tap to click" be OFF?**
> When macOS's own Tap to click is enabled, Tap #1 fires Click #1 and Tap #2 arrives as `clickCount == 2`, which macOS window title bars ignore for dragging. Letting `VoodooInput` own the unified state machine eliminates that conflict and persists cleanly across reboots.

## Installation (OpenCore)

> **Important:** Most OpenCore configs load `VoodooInput.kext` from **inside** another kext's `PlugIns` folder—typically either:
> - `EFI/OC/Kexts/VoodooPS2Controller.kext/Contents/PlugIns/VoodooInput.kext`, or
> - `EFI/OC/Kexts/VoodooI2C.kext/Contents/PlugIns/VoodooInput.kext`.
>
> Check your `EFI/OC/config.plist` under `Kernel → Add` to see **which single `VoodooInput.kext` entry has `Enabled = true`**, and replace that exact copy. Never enable two copies of `VoodooInput.kext` at the same time.

1. Download the latest **`VoodooInput-TapDrag-Release`** artifact from the **Actions** tab (or **Releases**) and unzip it to get `VoodooInput.kext`.
2. Back up your existing `VoodooInput.kext`.
3. Replace your enabled `VoodooInput.kext` in `EFI/OC/Kexts/...` with the new one.
4. Reboot into macOS and apply the [Required macOS Settings](#required-macos-settings) above.

## Building from Source

### Option 1: GitHub Actions (No Local Xcode Needed)

This repository includes `.github/workflows/main.yml` configured to build `VoodooInput.kext` on `macos-latest`:

1. Fork this repository (or push your changes).
2. Open the **Actions** tab → **Build VoodooInput TapDrag** → **Run workflow** (or push a commit).
3. Download the `VoodooInput-TapDrag-Release` zip from the completed workflow run.

### Option 2: Local Build on macOS

Requires Xcode command-line tools and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK):

```bash
git clone https://github.com/PiyushAdy/MacVodooWithBetterDragSupport.git
cd MacVodooWithBetterDragSupport
git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
xcodebuild -jobs 1 -configuration Release
```

The built kext and release zip will be output to `build/Release/`.

## How It Works & Tuning

All tap/drag logic lives in `VoodooInput/VoodooInputSimulator/VoodooInputSimulatorDevice.cpp` (`constructReportGated` + `onTapTimerTimeout`).

- **State Machine**: Ports `libinput`'s `evdev-mt-touchpad-tap.c` flow (`kTapIdle` → `kTapFirstDown` / `kTapTwoFingerDown` → `kTapTapped` → `kTapSecondPending` → `kTapDragging` / `kTapDraggingWait` / `kTapRightClickPending`) using an `IOTimerEventSource` on the simulator's `IOWorkLoop`.
- **Force Touch `0x281` Compatibility**: On macOS 12+, `VoodooInput` emulates Apple's `0x05AC:0x0281` trackpad, whose driver ignores `Button = 1` if no active finger contact is present. During synthetic tap clicks and edge-drag waits, the kext emits active synthetic finger contact(s) (`kTouchStateActive`, `Pressure = 120`) alongside `Button = 1`, followed by a timestamped release sequence on timeout or double-tap transition.

### Tunable Constants (`VoodooInputSimulatorDevice.cpp`)

| Constant | Default | Purpose |
| :--- | :--- | :--- |
| `kTapMaxDurationMs` | `190` ms | Maximum finger-down duration to count as a tap (`libinput`: 180 ms) |
| `kTappedHoldMs` | `180` ms | Window after Tap #1 lift to begin a double-tap or drag (`libinput`: 180 ms) |
| `kDragHoldMs` | `180` ms | Stationary hold duration on Tap #2 before locking drag (`libinput`: 180 ms) |
| `kEdgeDragWaitMs` | `300` ms | Edge continuation hold time after lifting near the border (`libinput`: 300 ms) |
| `kTapMaxMoveSq` | `95²` (~1.4 mm) | Max finger movement during a 1-finger tap |
| `kTwoFingerMaxMoveSq` | `180²` (~2.6 mm) | Max finger movement during a 2-finger tap |
| `kDragStartMoveSq` | `60²` (~0.85 mm) | Instant drag-grab movement threshold on Tap #2 |
| `kEdgeMarginX` / `kEdgeMarginY` | `800` / `520` | ~10% border zone for edge drag continuation |

## Credits & License

- [Acidanthera / VoodooInput](https://github.com/acidanthera/VoodooInput) ([kprinssu](https://github.com/kprinssu), [usr-sse2](https://github.com/usr-sse2), [vit9696](https://github.com/vit9696), and contributors) for the upstream Magic Trackpad 2 software emulator
- [VoodooI2C Team](https://github.com/VoodooI2C/VoodooI2C) ([alexandred](https://github.com/alexandred), [ben9923](https://github.com/ben9923), [blankmac](https://github.com/blankmac), [coolstar](https://github.com/coolstar), and others) for Magic Trackpad 2 reverse engineering
- [libinput](https://gitlab.freedesktop.org/libinput/libinput) for the tap-and-drag state-machine design and timing reference
- Licensed under the BSD 3-Clause License (see [`LICENSE.txt`](LICENSE.txt))
