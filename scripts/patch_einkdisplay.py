"""
PlatformIO pre-build script: bound the e-ink panel BUSY-wait timeout.

freeink-sdk's EpdBus::waitBusy() polls the panel's BUSY line with a 30000 ms
ceiling. Every refresh (BW and grayscale) funnels through it. When a grayscale
refresh stalls the panel (the longer, higher-power waveform can leave BUSY
stuck without tripping the ESP32 brownout-reset), the render task sits in one
or more of these 30 s waits while holding renderingMutex, so the main loop
blocks on RenderLock and input dies for tens of seconds -- the device looks
frozen and needs a manual reset. Shortening the ceiling lets a stuck panel
give up quickly so the reader recovers on its own instead of forcing a reset.

Safe because it only changes how long a GENUINELY STUCK wait persists: healthy
refreshes complete in well under 2 s (grayscale uses FAST_REFRESH), far below
the new ceiling, so they are never clipped.

Applied to the pinned freeink-sdk submodule at build time (the submodule tracks
upstream, so we patch the checked-out source rather than fork it). Idempotent
and re-applied on every build, so a fresh CI `git submodule update` is covered.
"""

Import("env")
import os

MARKER = "AALU: bounded busy-wait"
OLD = "if (millis() - start > 30000)"
NEW = "if (millis() - start > 6000 /* " + MARKER + " (was 30000) */)"


def patch_einkdisplay(env):
    filepath = os.path.join(
        env["PROJECT_DIR"],
        "freeink-sdk",
        "libs",
        "display",
        "FreeInkDisplay",
        "src",
        "bus",
        "EpdBus.cpp",
    )
    if not os.path.isfile(filepath):
        print("WARNING: EpdBus.cpp not found at %s -- skipping busy-wait patch" % filepath)
        return

    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    count = content.count(OLD)
    if count == 0:
        print("WARNING: busy-wait patch target not found in EpdBus.cpp -- SDK may have been updated")
        return

    content = content.replace(OLD, NEW)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched EpdBus busy-wait ceiling 30000ms -> 6000ms (%d site(s))" % count)


patch_einkdisplay(env)
