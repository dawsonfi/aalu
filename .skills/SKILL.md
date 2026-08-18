# AALU Development Guide

Project: Open-source e-reader firmware for the **Xteink X4** (ESP32-C3), forked from [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader). EPUB-focused, RAM-constrained.
Repo: `/Users/disrael/Projects/personal/xteink/aalu`. Version: see `[aalu] version` in `platformio.ini`.

> Legacy identifiers (`CrossPointSettings`, `CrossPointState`, `.crosspoint/`) are retained for cache backward compatibility. Do not rename without a binary migration path.

---

## AI Agent Rules
- Role: Senior embedded engineer (ESP-IDF / Arduino-ESP32, ESP32-C3 RISC-V).
- **380 KB RAM is a hard ceiling. Stability is non-negotiable.**
- Cite file:line for any change you propose. Do not invent APIs — verify against `open-x4-sdk/` or official docs.
- Justify any new heap allocation. Explain memory/perf claims by mechanism (DRAM vs IRAM, etc.).
- After any change: instruct the user how to verify (heap, cache, all 4 orientations).
- **Always build + run host tests after changes.** See [Testing Workflow](#testing-workflow). No exceptions, even one-line edits.
- **Keep `README.md` in sync with features.** Whenever you add, remove, or materially change a user-facing feature, update `README.md` in the same change — its "What's new in AALU" list, the "Using it" reference, and the "Roadmap" (move shipped items into the ✅ landed list). If the change is visual, refresh the affected screenshot in `docs/images/screenshots/`: capture it in the emulator (Power+Down — `P`+`↓` — writes a BMP to `sdcard/screenshots/`), convert with `sips -s format jpeg <in>.bmp --out docs/images/screenshots/<name>.jpeg`, or ask the user to regenerate it.

---

## Hardware Constraints
- ESP32-C3 single-core RISC-V @ 160 MHz, ~380 KB RAM (NO PSRAM), 16 MB flash.
- Display: 800×480 e-ink, single 48 KB framebuffer (SINGLE_BUFFER mode), slow refresh (1–2 s full).
- Storage: SD card (books + cache in `.crosspoint/`).

### Resource Protocol
1. **Stack**: locals < 256 B. Use `std::unique_ptr` or static pools for larger.
2. **Heap**: no repeated `new`/`delete` in loops. Allocate in `onEnter()`, reuse.
3. **Flash placement**: large constants → `static const` / `static constexpr` (lives in flash, not DRAM).
4. **Strings**: NO `std::string`/Arduino `String` in hot paths. Use `std::string_view` (read-only) or `snprintf` into fixed `char[]`.
5. **UI strings**: use `tr(STR_*)` always — never hardcode. Log strings may be hardcoded.
6. **`constexpr` first** — for compile-time tables and constants.
7. **`std::vector`**: always `.reserve(N)` before `push_back` loops.
8. **SD/SPIFFS writes**: never on every interaction. Guard with change-check; debounce progress saves. Flash has finite erase cycles.

### Flash budget — 6,553,600-byte HARD LIMIT
Each OTA slot in `partitions.csv` is `0x640000`. Exceeding it → bootloader rejects → infinite reset loop, looks like a freeze.

Rules:
1. **Never push the `default` env image over 6,553,600 bytes.** It's close to the limit because `LOG_LEVEL=2` keeps LOG_DBG strings in flash.
2. After each change, check `Total image size: N bytes` in `pio run` output (NOT the misleading "Flash X% used"). Ground truth: `wc -c .pio/build/default/firmware.bin`.
3. Soft ceiling: **6,553,344 bytes** (256-byte safety margin).
4. If a feature won't fit: shrink it, gate LOG_DBG behind `#if LOG_LEVEL >= 3`, or target `gh_release` (~33 KB headroom). **Do not bump OTA partition without explicit user approval** — breaks OTA upgrades from older firmware.

### Debugging "frozen device" → capture serial first
```bash
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

| Boot-log signature | Meaning | Fix |
|---|---|---|
| `Image length N doesn't fit in partition length 6553600` | `.bin` too big | Shrink image / use slimmer env |
| `No bootable app partitions in the partition table` | Both OTA slots invalid | Reflash known-good `.bin` via USB |
| `Guru Meditation: LoadProhibited/StoreProhibited` | Null / use-after-free | Decode backtrace; audit `onExit()` cleanup |
| `Stack canary watchpoint triggered (TaskName)` | Stack overflow | Bump task stack; move locals to heap |
| `Task watchdog got triggered. CPU 0: TaskName` | Task ran >5 s without yielding | Add `vTaskDelay(1)` in tight loops |
| `Brownout detector was triggered` | Voltage sag, often during refresh | Check battery; not code |
| `rst:0x3 (RTC_SW_SYS_RST)` looping | Watchdog / assertion early in boot | Audit `HalSystem::checkPanic`, `setupDisplayAndFonts` |
| `0x4080xxxx` in backtrace | Code address | Feed to `debugging_monitor.py` to symbolicate |

Workflow: (1) flash-size error → don't touch logic, shrink. (2) Guru Meditation → capture full backtrace, find function. (3) Repeated `rst:0x3` without panic → suspect watchdog/brownout/assertion before flush. **Never guess without serial output.**

---

## ESP32-C3 Platform Pitfalls

### `std::string_view` is NOT null-terminated
Passing `.data()` to C APIs (`drawText`, `snprintf`, `strcmp`, SdFat paths) is UB.
```cpp
// WRONG: renderer.drawText(font, x, y, myView.data(), true);
// CORRECT:
renderer.drawText(font, x, y, std::string(myView).c_str(), true);
// CORRECT zero-alloc:
char buf[64]; snprintf(buf, sizeof(buf), "%.*s", (int)myView.size(), myView.data());
```

### `IRAM_ATTR` and flash cache safety
During SPI flash ops (OTA, SPIFFS, NVS) the instruction cache is suspended. Any code that can run then (ISRs especially) must be in IRAM.
- ISR handlers → `IRAM_ATTR`.
- Data read by `IRAM_ATTR` code → `DRAM_ATTR` (flash-resident `static const` will fault).
- Normal task code does NOT need `IRAM_ATTR`.

### ISR vs task shared state
`xSemaphoreTake()` cannot be called from ISR — will crash.

| Direction | Primitive |
|---|---|
| ISR → task (data) | `xQueueSendFromISR()` + `portYIELD_FROM_ISR()` |
| ISR → task (signal) | `xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()` |
| Task → task | `xSemaphoreTake()` / mutex |
| Single-writer ISR flag | `volatile bool` + `portENTER_CRITICAL_ISR()` |

### RISC-V alignment
ESP32-C3 faults on unaligned multi-byte loads. Never cast `uint8_t*` → wider pointer and dereference. Use `memcpy`:
```cpp
uint32_t val; memcpy(&val, buf, sizeof(val));
```
Applies to cache deserialisation and `__attribute__((packed))` member access.

### Template / `std::function` bloat
`std::function<void()>` adds 2–4 KB per signature and heap-allocates its closure. Avoid in library code and render paths. Prefer plain function pointers or `struct { void* ctx; void (*fn)(void*); }`.

---

## Build System (PlatformIO)

| Env | Purpose | Log Level |
|---|---|---|
| `default` | Local dev | 2 (serial debug on) |
| `gh_release` | Production | 0 |
| `gh_release_rc` | RC | 1 |
| `slim` | Minimal, no serial | — |

Files: `platformio.ini` (committed), `platformio.local.ini` (gitignored personal overrides, included via `extra_configs`), `partitions.csv` (flash layout).

### Critical build flags (do not change lightly)
```cpp
-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1   // 48 KB single FB
-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1
-DXML_CONTEXT_BYTES=1024 -DXML_GE=0   // EPUB XML cap + entity disable
-DUSE_UTF8_LONG_NAMES=1
-DPNG_MAX_BUFFERED_PIXELS=16416       // 2048px-wide PNG scanlines
-DAALU_VERSION=\"...\"                // from [aalu] version
-fno-exceptions -std=gnu++2a
-Wl,--wrap=panic_print_backtrace,--wrap=panic_abort  // custom panic
```

### Pre-build scripts (`extra_scripts`)
- `scripts/build_html.py` → `src/network/html/*.generated.h` from `data/html/`.
- `scripts/gen_i18n.py` → I18n tables from `lib/I18n/translations/*.yaml`.
- `scripts/patch_jpegdec.py`, `scripts/git_branch.py`.

### SINGLE_BUFFER implications
Only one framebuffer. Grayscale rendering needs `renderer.storeBwBuffer()` / `restoreBwBuffer()` (see `lib/GfxRenderer/GfxRenderer.cpp:439-440`).

### Commands
```bash
pio run                       # default build
pio run -e gh_release         # production
pio run -t upload             # build + flash
pio run -t upload && pio device monitor
pio device monitor            # basic
python3 scripts/debugging_monitor.py [/dev/cu.usbmodemXXXX]  # enhanced, decodes backtraces
pio check                     # cppcheck
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git submodule update --init --recursive  # required for open-x4-sdk
```

Port detection: macOS `/dev/cu.usbmodem*`, Linux `/dev/ttyUSB*`/`ttyACM*`, Windows `COMx`.

---

## Architecture

```
src/
├── main.cpp                    # entry + global font init
├── CrossPointSettings.h        # user settings (legacy name)
├── MappedInputManager.cpp/.h   # logical button → hardware
├── fontIds.h
├── activities/                 # UI screens (Activity lifecycle)
│   ├── home/                   # HomeActivity, FileBrowser, RecentBooks
│   ├── reader/                 # EpubReader, TxtReader, XtcReader,
│   │                           # Dictionary*, KOReaderSync, etc.
│   ├── settings/  stats/  network/  browser/  boot_sleep/  util/
└── network/html/               # HTML sources + generated headers
lib/
├── hal/                        # HalDisplay, HalGPIO, HalStorage
├── Epub/                       # parsing + section caching
├── GfxRenderer/                # e-ink primitives
├── EpdFont/                    # font conversion + builtins
├── UITheme/                    # themed UI (GUI macro)
└── I18n/                       # translations YAML + generated
open-x4-sdk/                    # submodule
docs/  scripts/  English-Dictionary/  partitions.csv  platformio.ini
```

### HAL — always use, never the SDK classes directly
| HAL | Wraps | Singleton |
|---|---|---|
| `HalDisplay` | `EInkDisplay` | — |
| `HalGPIO` | `InputManager` | — |
| `HalStorage` | `SDCardManager` | `Storage` |

```cpp
#include <HalStorage.h>
FsFile file;  // SdFat FsFile, NOT Arduino File
if (Storage.openFileForRead("MODULE", "/path/file.bin", file)) {
  // read
  file.close();   // explicit close required
}
```

### Singletons
```cpp
#define SETTINGS  CrossPointSettings::getInstance()  // user settings
#define APP_STATE CrossPointState::getInstance()     // runtime state
#define GUI       UITheme::getInstance()             // theme
#define Storage   HalStorage::getInstance()
#define I18N      I18n::getInstance()
```

### Activity lifecycle
Activities are heap-allocated, deleted on exit (see `src/main.cpp:132-143`).
```cpp
void onEnter() { Activity::onEnter(); /* alloc, tasks */ render(); }
void loop()    { mappedInput.update(); /* input */ }
void onExit()  { /* vTaskDelete BEFORE destruction; free; close files */ Activity::onExit(); }
```
Anything allocated in `onEnter()` MUST be freed in `onExit()`. Tasks MUST be `vTaskDelete()`d before activity destruction. File handles MUST be closed.

### FreeRTOS tasks
Pattern: `xTaskCreate(&taskTrampoline, "Name", stackBytes, this, 1, &handle)`.
Stack sizing (BYTES): 2048 simple render, 4096 network/EPUB. Monitor `uxTaskGetStackHighWaterMark()`. < 512 B headroom → bump stack. Use mutex for shared state.

### Fonts
~80+ global `EpdFont`/`EpdFontFamily` objects in `src/main.cpp:40-115`. Stored in flash; rendering data caches in DRAM on first use. Font IDs in `src/fontIds.h`. `OMIT_FONTS` reduces binary for minimal builds.

---

## AALU-Specific Features (on top of CrossPoint)
- **UI themes** (`src/activities/home/`, `lib/UITheme/`): Classic, Lyra, Recent6 (3×2 grid, memory-safe). Asymmetric bottom menu, cascading cover-resolution fallbacks (anti-ghosting).
- **Apps submenu** (`AppsActivity.*`): File Transfer, Stats, OPDS hub.
- **KOReader Sync** (`KOReaderSyncActivity.*`): heuristic paragraph-level sync, prevents XML parser crashes on remote.
- **Reading Stats v2.5** (`src/activities/stats/`): global dashboard, Reading/Finished filter (Right button), per-book analytics, 3-line title wrapping, **binary migration v4/v5 → v6 for `stats.bin`** — bump version + add migration code if layout changes.
- **Stability**: deep-sleep forced session save; 3-minute session threshold prevents short-cycle corruption.
- **Offline English Dictionary** (`Dictionary*Activity.*`, `English-Dictionary/`): pixel-perfect selection, StarDict (`dictionary.dict`+`.idx`), Levenshtein "did you mean?", lookup history.
- **In-Reader Quick Settings (Aa) overlay**: zero-heap formatting, deferred SD writes (flash-life), custom buffering to prevent ghosting.

---

## Coding Standards
- Naming: PascalCase classes, camelCase methods/vars, UPPER_SNAKE constants, `memberVariable` (no prefix), `#pragma once`.
- Memory: prefer `std::unique_ptr`; avoid `std::shared_ptr` (single-core RISC-V wastes atomics). Explicit `file.close()` / `vTaskDelete()` for deterministic release.
- **Private methods below all public ones.**

### Error handling
- NO C++ exceptions. NO `abort()`. ALWAYS log before error return.
1. **`LOG_ERR + return false`** (90%): `LOG_ERR("MOD", "Failed: %s", reason); return false;`
2. **`LOG_ERR + fallback`**: log and `useDefault()`.
3. **`assert(false)`**: only for fatal impossible states (e.g. missing framebuffer).
4. **`ESP.restart()`**: only for recovery (e.g. OTA complete).

### Acceptable malloc patterns
For >256 B buffers, one-time activity init, and variable-size bitmap buffers:
```cpp
auto* buffer = static_cast<uint8_t*>(malloc(bufferSize));
if (!buffer) { LOG_ERR("MODULE", "malloc failed: %d", bufferSize); return false; }
processData(buffer, bufferSize);
free(buffer); buffer = nullptr;
```
Examples in repo: `HomeActivity.cpp:166`, `TxtReaderActivity.cpp:259`, `GfxRenderer.cpp:439-440`, `OtaUpdater.cpp:40`.

---

## UI & Orientation
- No hardcoded 800/480 — use `renderer.getScreenWidth()/getScreenHeight()`.
- Use `renderer.getOrientedViewableTRBL()` to respect bezel margins.
- **Test all 4 orientations** (Portrait, Inverted, Landscape CW/CCW). Many bugs appear in only one.

### Logical button mapping (`src/MappedInputManager.cpp:20-55`)
Physical fixed: `Button::Up` → `HalGPIO::BTN_UP`, `Button::Down` → `HalGPIO::BTN_DOWN`.
User-remappable (front): `Button::Back/Confirm/Left/Right` ↔ `SETTINGS.frontButton*`.
Reader (swappable): `Button::PageBack`/`PageForward` via `SETTINGS.sideButtonLayout`.
**Use `MappedInputManager::Button::*` enums; never raw `HalGPIO::BTN_*` (except in `ButtonRemapActivity`).**

### UITheme
All UI rendering through `GUI` macro. Don't hardcode fonts/colours/positioning — that's what keeps Classic/Lyra/Recent6 consistent across orientations.

### Bottom button hints — use the glyph band, not the rectangle bar
New screens MUST render the bottom button hints via `GUI.drawButtonHintsGlyphs(renderer [, variant])`, not the legacy `GUI.drawButtonHints(...)` rectangle bar. The glyph band matches the home-screen style (procedural ◀ ● ▲ ▼ glyphs, no labels, no border) and stays visually consistent with `HomeRenderer::drawBottomButtonHints`.

Variants (`BaseTheme::ButtonHintGlyphSet`):
- `Navigation` (default): ◀ ● ▲ ▼ — back, confirm, up, down. Use for any list/menu where Up/Down navigate. Settings, Network mode selection, Reader quick-settings, Detailed stats, ConfirmationActivity all use this.
- `StatsActions`: ◀ ● ⋯ ⇄ — back, confirm, "more", Reading/Finished toggle. Use ONLY where the Up button means "More" and Down means "swap views" (Statistics list, Transfer screen).

Do not pass labels — the glyphs are unlabeled by design. Do not reintroduce the rectangle bar in new screens; the only remaining `drawButtonHints` callers are legacy reader sub-screens that still need text labels (chapter/page selection, dictionary, KOReader sync) and have not yet migrated.

When adding a screen whose buttons can't be expressed by an existing variant, add a new `ButtonHintGlyphSet` value and draw the appropriate procedural glyphs in `BaseTheme::drawButtonHintsGlyphs` (`src/components/themes/BaseTheme.cpp`). Do not rebuild the rectangle bar.

---

## Testing Workflow (MANDATORY after every code change)

Run all four in order before declaring complete, proposing a commit, or handing off. Mirrors CI on `master` — green locally = green CI = release fires.

```bash
# 1. Format (matches clang-format CI job). Requires clang-format-21 on PATH.
#    macOS: brew install llvm@21 && PATH="/opt/homebrew/opt/llvm@21/bin:$PATH"
./bin/clang-format-fix
git diff --exit-code   # MUST be clean; if not, stage formatter changes

# 2. Static analysis (matches cppcheck CI)
pio check --fail-on-defect medium --fail-on-defect high

# 3. Build default env (matches build CI)
pio run

# 4. Host tests (compile + run on macOS/Linux, no device)
cmake -S test -B build/test
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
```

Rules: all four green or it's not done. Fix warnings, don't paper over. `pio check` `[low:*]` hints are advisory; `medium`/`high` fail. If `clang-format-fix` rewrites files, include them in the commit. Most UI work isn't covered by host tests — say so explicitly when reporting status.

### AI-agent scope (you verify)
1. Build (`pio run -t clean && pio run`, 0 errors, 0 warnings).
2. Host tests (both scripts).
3. `pio check` + `clang-format`.
4. Commit format (`feat:`/`fix:`), no gitignored files staged.
5. Fix CI failures before review.
6. Inspect orientation switch/case coverage.

### Human-tester scope (flag for user)
- Real hardware test, all 4 orientations.
- `ESP.getFreeHeap()` > 50 KB; no leaks across activity transitions.
- If EPUB code changed: delete `.crosspoint/`, verify clean re-parse.
- AALU flows: Quick Settings overlay, Dictionary lookup, KOReader sync, Stats v2.5.

### Live debugging snippets
```cpp
LOG_DBG("MEM", "Free: %d", ESP.getFreeHeap());
LOG_DBG("TASK", "Stack hi-water: %d", uxTaskGetStackHighWaterMark(nullptr));
logSerial.flush();   // before suspected crash
```

### Common crash causes
1. **OOM**: log free heap; verify buffers freed in `onExit()`; look for >10 KB allocs near crash.
2. **Stack overflow**: high-water log; bump 2048→4096; move locals to heap.
3. **Use-after-free**: `vTaskDelete()` BEFORE destruction; `nullptr` after `free()`.
4. **Corrupt cache**: delete `.crosspoint/`; check format versions.
5. **Watchdog**: tight loops need `vTaskDelay(1)`; no blocking I/O.

### CI/CD workflows
| Workflow | File | Purpose |
|---|---|---|
| Build Check | `ci.yml` | clang-format + cppcheck + build |
| Format Check | `pr-formatting-check.yml` | PR title check |
| Release | `release.yml` | Production |
| RC | `release_candidate.yml` | RC |

### Always confirm CI is green — non-negotiable

CI runs three required jobs on every push to `master` and every PR (`ci.yml`): `clang-format`, `cppcheck`, `build`. A failing run on `master` blocks the next release (`Release` only fires after `CI (build)` succeeds). Treat a red `master` as stop-the-line.

Rules:
1. **Run the full local CI mirror before every commit — hard pre-commit gate.** Before any `git commit` that touches code (`*.c`/`*.cpp`/`*.h`, `platformio.ini`, `scripts/`, `lib/I18n/translations/*.yaml`) — and before declaring any task done — run the full [Testing Workflow](#testing-workflow-mandatory-after-every-code-change) on the exact tree you are about to commit and confirm all four steps are green. The three CI jobs (`clang-format`, `cppcheck`, `build`) are a strict subset of that workflow, so all-green locally is what guarantees CI won't fail. Never `git commit` on an unrun or red check — not for a "tiny" change, not to "save progress," not for a version bump. If you skipped a step, you are not ready to commit.
2. **After pushing**, check the CI run on the pushed branch and wait for it to finish:
   ```bash
   GH_HOST=github.com gh run list --repo <owner>/<repo> --branch "$(git branch --show-current)" --limit 3
   GH_HOST=github.com gh run watch <run-id> --repo <owner>/<repo>
   ```
   (Use `GH_HOST=github.com` + explicit `--repo` because this repo's remotes can confuse `gh`.) The branch is not "done" until CI is green.
3. **If CI is red**, treat it as the active task. Don't move on, don't start something else, don't ask the user to fix it — diagnose the failure (`gh run view <id>`), reproduce it locally, fix it, push, and re-watch.
4. **If CI failed before you were involved** (e.g. red `master` when you arrive), surface it to the user immediately and offer to fix it before doing anything else. Don't pile new work on top of a red main.
5. **Don't bypass.** Never `--no-verify`, never skip the format/check steps locally to "just get the commit out." If a check is wrong, fix the check; don't route around it.

---

## Generated files — NEVER hand-edit

1. **HTML headers**: `src/network/html/*.generated.h` ← `data/html/*.html` via `scripts/build_html.py`. Edit source HTML, `pio run` regenerates. Commit only source.
2. **I18n**: `lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` ← `lib/I18n/translations/*.yaml` via `scripts/gen_i18n.py`. Required keys: `_language_name`, `_language_code`, `_order`, then `STR_*`. English is reference; missing keys fall back. Run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` or just `pio run`. **Commit YAML only — generated files gitignored.**
3. **Build artefacts**: `.pio/`, `build/`, `*.generated.h`, `compile_commands.json` (all gitignored).
4. **Fonts**: source → `lib/EpdFont/fontsrc/` (gitignored). Conversion script (see `lib/EpdFont/README`). Add global object in `src/main.cpp`, ID in `src/fontIds.h`.

```cpp
#include <I18n.h>
renderer.drawText(FONT_UI, x, y, tr(STR_LOADING), true);
```

---

## Cache (`.crosspoint/` on SD root — legacy name retained)

```
.crosspoint/
├── epub_<hash>/
│   ├── progress.bin      # reading position
│   ├── cover.bmp         # multi-res
│   ├── book.bin          # metadata (title, author, spine, ToC)
│   └── sections/*.bin
├── stats.bin             # global + per-book stats (v6)
└── system/BasicCover.bmp # fallback
```
Hash: `std::hash<std::string>{}(filepath)` — move/rename → new hash → lost progress.

### Invalidation triggers
1. File format version bump (`book.bin`, `section.bin`, `stats.bin`).
2. Render settings: font/size/spacing/margin.
3. Viewport: orientation or resolution.
4. Book file modified/moved/renamed (new hash).

### Manual clear
```bash
rm -rf /path/to/sd/.crosspoint/                # full
rm -rf /path/to/sd/.crosspoint/epub_<hash>/    # one book
rm -rf /path/to/sd/.crosspoint/epub_<hash>/sections/  # keep progress, drop sections
```

### Versions (verify in source before changing)
- `book.bin`: v5 (`lib/Epub/Epub/BookMetadataCache.cpp`)
- `section.bin`: v12 (`lib/Epub/Epub/Section.cpp`)
- `stats.bin`: v6 with v4/v5 → v6 migration engine

**Rules**: bump version BEFORE layout change; mismatch → auto-invalidate or migrate; document in `docs/file-formats.md`.

---

## Git Workflow

**ALWAYS work and commit directly on `master` for this repo — never create feature branches.** Commit straight to `master` and push to `origin master`. This is a hard rule and overrides the global "branch first when on the default branch" default: do not create branches and do not open PRs unless explicitly asked.

`origin` = your fork (`dawsonfi/aalu`), `upstream` = CrossPoint. Verify before committing:
```bash
git branch --show-current   # expect: master
git remote -v
git status --short
```

Rules:
1. Commit and push on `master`: `git push origin master`.
2. Push to `origin` (your fork); open a PR to `upstream` only when explicitly asked.
3. Sync before work: `git fetch upstream && git merge upstream/main`.

Commit format: `<type>: <50-char summary>` + optional body. Types: `feat fix refactor docs test chore perf`.

**Do commit** when user asks AND: feature tested on hardware, fix verified, refactor preserves behaviour, `pio run` clean.
**Do NOT commit** when untested, build fails/warns, mid-experiment, user hasn't asked, or `.gitignore`d files would be staged (`*.generated.h`, `.pio/`, `compile_commands.json`, `platformio.local.ini`). **Always `git status` first.** If uncertain, ASK.

---

## Local Dev Config (`platformio.local.ini`, gitignored)

```ini
[env:default]
upload_port = /dev/cu.usbmodem2101    # macOS example
monitor_port = /dev/cu.usbmodem2101
build_flags =
  ${base.build_flags}
  -DMY_DEBUG_FLAG=1
```
NEVER commit. NEVER put serial ports or secrets in `platformio.ini`. Use `${base.build_flags}` to extend, not replace.

---

## Desktop Simulator (`[env:simulator]`)

SDL2-backed native build of the real `src/` + `lib/` tree, powered by the [`uxjulia/crosspoint-simulator`](https://github.com/uxjulia/crosspoint-simulator) PlatformIO library. Same AALU code that runs on the device, compiled for the host, rendered into an SDL window.

```bash
brew install sdl2                       # prereq, one time

make emulator                           # build, mount ./sdcard/, launch
make sim-build                          # build only → .pio/build/simulator/program
make sim-clean                          # wipe sim build cache + ./sdcard/.crosspoint/
pio run -e simulator -t run_simulator   # equivalent direct command
```

### Filesystem layout
User-facing SD-card root is `./sdcard/` at the repo root (gitignored, auto-created by `make emulator`). Internally the sim hardcodes its sandbox to `./fs_/` (see sim's `HalStorage.cpp`), so the Makefile keeps `fs_` as a symlink to `sdcard/`. Device SD root → `./sdcard/`. Device `/books/file.epub` → `./sdcard/books/file.epub`. Cache at `./sdcard/.crosspoint/`.

**Point the sim at a real SD card's contents** — three options:

```bash
# A. Drop loose EPUBs (simplest)
cp ~/Downloads/MyBook.epub sdcard/

# B. Snapshot copy of a real device's SD card
cp -R /path/to/sdcard/. sdcard/

# C. Replace the whole sdcard/ with a symlink to a mounted SD card
rm -rf sdcard fs_
ln -s /Volumes/your-sd-card sdcard
```

After any change to storage/cache binary layout, `rm -rf sdcard/.crosspoint/` (or `make sim-clean`) before re-running — stale caches mask the fix. Cache from a real device works *only* if the sim runs at the same panel resolution (800×480 by default) and identical render settings (font / size / margin / orientation).

### Input mapping (authoritative: sim's `HalGPIO.cpp`)

| SDL key | Physical button (`HalGPIO::`) | Default logical mapping in AALU |
|---|---|---|
| Escape | `BTN_BACK` | `Button::Back` (front, remappable) |
| Return | `BTN_CONFIRM` | `Button::Confirm` (front, remappable) |
| ← | `BTN_LEFT` | `Button::Left` (front, remappable) |
| → | `BTN_RIGHT` | `Button::Right` (front, remappable) |
| ↑ | `BTN_UP` | `Button::Up` / `Button::PageBack` in reader |
| ↓ | `BTN_DOWN` | `Button::Down` / `Button::PageForward` in reader |
| P | `BTN_POWER` | Power |
| S | (simulator sleep request) | `HalGPIO::consumeSimulatorSleepRequest()` |

Front-button mapping is user-controlled via Settings → Button Remap (`SETTINGS.frontButton*`). Reader-context page direction is `SETTINGS.sideButtonLayout`. The sim feeds the *physical* button index; AALU's `MappedInputManager` (`src/MappedInputManager.cpp:20-55`) does the physical → logical translation exactly as on the device.

No hotkey rotates the display. Orientation is changed through the app's own Settings → Orientation, which calls `GfxRenderer::setOrientation` — the sim's `HalDisplay::setSimulatorOrientation` (auto-patched into AALU's `GfxRenderer.h` by the sim lib) rotates the SDL window to match.

### Sim-specific source files (do not delete)
- `src/simulator/sim_compat.h` — `-include`-injected shim for `TickType_t`, `xTaskGetTickCount`, and the FreeRTOS tick macro. Sim's `<freertos/FreeRTOS.h>` doesn't provide all three.
- `src/simulator/sim_stubs/network/{FirmwareFlasher,OtaBootSwitch}.h` — declarations the sim's `simulator_firmware.cpp` / `simulator_ota.cpp` shims expect to find. AALU has no firmware-flashing UI; these stubs exist only so the sim links. Device builds never see them (`-Isrc/simulator/sim_stubs` only on `[env:simulator]`).
- `src/network/OtaUpdater.h` — declares a `#ifdef SIMULATOR`-only `installUpdate(ProgressCallback, void*, std::atomic<bool>*)` overload + `CANCELLED_ERROR` enum value, both implemented by the sim's `simulator_ota.cpp`. Device build doesn't see either.

### Fork-drift cost — read this
The sim was written against upstream CrossPoint. Each time AALU adds a new method to a `Hal*` class (or to a class the sim already stubs, like `OtaUpdater`) and that code path is exercised, the sim build can fail until a matching stub is added. Usually one-line no-ops. This is the single most common reason `pio run -e simulator` breaks after pulling firmware updates.

### What the sim does NOT reproduce

| Device | Sim |
|---|---|
| E-ink ghosting / 1–2 s full refresh | Instant SDL redraw |
| 380 KB RAM hard ceiling | Host has GB; leaks pass silently |
| 160 MHz RISC-V | GHz x86/ARM — timings are wrong |
| FreeRTOS scheduling | std::thread + condvars (different semantics) |
| Wi-Fi / OTA / Bluetooth / battery / deep sleep | Stubbed or no-op |
| Flash cache suspend behaviour (`IRAM_ATTR`) | N/A |

Useful for: UI iteration, EPUB parsing, dictionary, KOReader sync, stats math, all four orientations. **Not a substitute** for the hardware checklist in [Testing Workflow](#testing-workflow-mandatory-after-every-code-change) — visual or input changes still need 4-orientation device testing before declaring done.

---

## Reverting to stock firmware
<https://xteink.dve.al/>

---

Philosophy: **A dedicated e-reader, not a Swiss Army knife.** Features that add RAM pressure without significantly improving reading are Out of Scope. AALU's additions (Dictionary, KOReader sync, Stats v2.5, Quick Settings) earn their RAM cost — new features clear the same bar.
