# Deferred research — real accelerometer on webOS

**Status:** ☐ Research / deferred (Stage 3 is fully verifiable with a correctly-*framed* synthetic
vector; only live-tilt gameplay needs this).
**Consumed by:** Stage 3 (Display/Sensor/Orientation). Feeds `SensorManager`/`SensorEvent`
`TYPE_ACCELEROMETER`.

## Goal

Source real device acceleration on the HP TouchPad and present it to the engine in the **Android 2.3
natural (landscape) sensor frame** (see Stage 3 §1): m/s², +X along the 1024 edge, +Y along the 768
edge, +Z out of the screen, value = acceleration − gravity (flat face-up → `(0,0,+9.81)`). The engine
remaps to its app frame via `getRotation()`.

## Candidate sources (cheapest first)

1. **SDL joystick axes (try first — nearly free).** `platform/webos.c` already inits
   `SDL_INIT_JOYSTICK`. webOS/PDK has historically surfaced the accelerometer as an SDL joystick on
   some devices; probe `SDL_NumJoysticks()` / `SDL_JoystickOpen(0)` / `SDL_JoystickGetAxis()` on the
   TouchPad and see whether 3 axes track tilt. If so, map axes → m/s² in the natural frame. Lowest
   effort, no new dependency.
2. **Luna service bus (user's lead — most likely the real source).** webOS exposes sensors over the
   Luna (LS2) service bus. From a PDK native app, reach it via PDL's service-call surface or a
   luna-service2 client. To determine on-device:
   - Enumerate services: `ls-monitor` / `luna-send -l` to find the sensor/orientation service
     (candidates: a `com.palm.*` sensors or orientation service; the system uses an orientation/accel
     feed for auto-rotate).
   - Subscribe and read the payload; map its axes/units into the Android natural frame.
   - Cross-check against `webos://knowledge/services` and `webos://knowledge/system-internals` via the
     webOS-MCP for the exact service URI and method (not yet confirmed — **to verify on-device**).
3. **Kernel input / sysfs node (fallback).** Linux 2.6.35; the accelerometer likely presents as a
   `/dev/input/event*` device or a sysfs path. Identify with `cat /proc/bus/input/devices`; read raw
   counts and scale to m/s². Most direct, least portable.

## Acceptance for this item

- [ ] On-device tilt produces a 3-axis vector that tracks gravity, mapped to the **natural landscape**
      frame with correct signs (flat face-up ≈ `(0,0,+9.81)`).
- [ ] Plugged into the Stage-3 `SensorManager`/`getRotation()` remap, **live tilt drives in-game
      gravity** (e.g. WMW fluid responds to physical tilt), with no change to the Stage-3 render path.

## Notes

- Resolve the 90↔270 / axis-sign convention empirically (Stage 3 §1): correct posture → fluid pools at
  portrait-bottom; tilt portrait-left → fluid goes left.
- Until this lands, Stage 3 uses a synthetic vector in the correct frame, so orientation correctness is
  not blocked on it.
