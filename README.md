# Proxy Audio Device (Fixed Fork)

A macOS virtual audio driver that adds software volume control to USB audio interfaces like the Focusrite Scarlett 2i2 that don't natively support macOS volume keys.

This is a fork of [briankendall/proxy-audio-device](https://github.com/briankendall/proxy-audio-device) with a fix for the long-standing audio cut-out bug ([#19](https://github.com/briankendall/proxy-audio-device/issues/19), [#14](https://github.com/briankendall/proxy-audio-device/issues/14), [#43](https://github.com/briankendall/proxy-audio-device/issues/43)). Likely related to [#62](https://github.com/briankendall/proxy-audio-device/issues/62) (macOS 26) but untested on that version.

## What was fixed

The original driver randomly stops producing audio after minutes to hours of use. The only recovery is to restart CoreAudio or toggle the settings. This bug has been reported since 2021 and was never resolved.

**Root cause**: The driver maps between two independent audio clocks (the virtual proxy device and the physical USB device) using a value called `inputOutputSampleDelta`, computed once when audio starts. When macOS restarts the proxy device's IO — due to idle timeout, power management, or app lifecycle — the clock mapping becomes stale. The read position falls millions of frames behind the write position in a 16,384-frame ring buffer, and the output silently reads zeros.

**Fix**: Before each audio buffer fetch, check whether the read position has drifted beyond the ring buffer bounds. If it has, recalculate the clock mapping on the spot. The check is a single integer comparison per audio cycle — effectively computationally free.

## Download

**[Download the latest signed release](../../releases/latest)** — notarized, no build tools required.

The installer includes the audio driver and the companion settings app.

### Install

1. Download and open `ProxyAudioDevice.pkg`
2. Follow the installer — it will place the driver and settings app automatically
3. CoreAudio restarts automatically after installation
4. The settings app will open — select your USB audio interface as the output device
5. Set the proxy device as your system audio output in System Settings > Sound

### Uninstall

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/ProxyAudioDevice.driver
rm -rf /Applications/Proxy\ Audio\ Device\ Settings.app
sudo killall coreaudiod
```

## Build from source

Requires Xcode.

```bash
git clone https://github.com/scottblackmgfx/proxy-audio-device.git
cd proxy-audio-device

# Build the driver
xcodebuild -project proxyAudioDevice.xcodeproj -target ProxyAudioDevice -configuration Release SYMROOT="$(pwd)/build" CODE_SIGN_IDENTITY="-" CODE_SIGNING_ALLOWED=YES

# Install
sudo rm -rf /Library/Audio/Plug-Ins/HAL/ProxyAudioDevice.driver
sudo cp -R build/Release/ProxyAudioDevice.driver /Library/Audio/Plug-Ins/HAL/ProxyAudioDevice.driver
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/ProxyAudioDevice.driver
sudo killall coreaudiod
```

## Run tests

```bash
cd tests

# Automated tests (no audio hardware needed)
make test

# Audible test (plays a tone, injects desync, verifies recovery)
make test-audible
```

## How the fix works

The fix is in [`proxyAudioDevice/DesyncRecovery.h`](proxyAudioDevice/DesyncRecovery.h) — a self-contained pure function (~80 lines, fully documented).

In `outputDeviceIOProc`, after computing the read position from `inputOutputSampleDelta`, we check the ring buffer fill level:

```
fill = mEndFrame - (startFrame + bufferSize)
```

If `fill > ringCapacity`, the clock mapping is stale. We recalculate `inputOutputSampleDelta` from the current input position and recompute `startFrame` before the fetch. One integer comparison per cycle on the hot path. The recalculation only runs when something has gone wrong.

The driver also includes optional event-driven diagnostics ([`DesyncDiagnostics.h`](proxyAudioDevice/DesyncDiagnostics.h)) that log state transitions to `/tmp/ProxyAudioDiagnostics.log`. These can be disabled at compile time by setting `DESYNC_DIAGNOSTICS_ENABLED` to `0`.

## Credits

- Original project by [Brian Kendall](https://github.com/briankendall)
- Audio cut-out fix by [Scott Black](https://github.com/scottblackmgfx)
