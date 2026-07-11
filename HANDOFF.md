# AALU ← CrossPoint parity backlog (working notes)

Goal (user, 2026-07-11): incorporate **all** CrossPoint `develop` features into AALU, adapted to AALU's look & feel. Reference repo: `/Users/disrael/Projects/personal/xteink/crosspoint-reader` (branch `develop`).

## Shipped this run (each its own minor version on `master`, full CI mirror green; UNPUSHED)
- **2.22.0** `#2452` render-first-page-while-chapter-builds (Section incremental build engine + reader restructure). dec78ed0. **Needs on-device pagination test.**
- **2.23.0** crash report screen on panic reboot (`CrashActivity`). 5c21455c.
- **2.24.0** `#2452` giant-single-spine indexing popup (`BUILD_POPUP_BYTE_THRESHOLD`). 3d173078.
- **2.25.0** crash-safe atomic progress writes (`ProgressFile::writeAtomic`, EPUB/XTC/TXT). 086fe2fb.
- **2.26.0** bookmark added/removed confirmation (timed overlay, survives grayscale AA re-display). 907896e8.
- **2.27.0** finished-book automation toggles (removeReadBooksFromRecents + moveFinishedToReadFolder; `/read/` move re-keys cache via `FsHelpers::cachePathHash`). 15ac29ac.
- **2.28.0** "Never" sleep-timeout enum value (`SLEEP_NEVER`, getSleepTimeoutMs=~0UL; adapted to AALU's enum picker, not the raw-minutes widget). 6ff0eea8.
- **3.0.0** multi-server OPDS (replaced single Calibre). `OpdsServerStore` (mirrors WifiCredentialStore → `/.crosspoint/opds.json`) + `JsonSettingsIO::saveOpds/loadOpds` + `OpdsServerListActivity` (list/add/edit/delete + pickerMode→browse) + `OpdsServerEditActivity` (name/url/user/pass + delete). Browser takes an `OpdsServer`; `goToBrowser`→picker; Settings "OPDS Browser"→list. One-time migration seeds from the old `SETTINGS.opds*`. Retired `CalibreSettingsActivity`. **MAJOR bump** for accumulated breaking changes (section.bin v20→v21 + OPDS config). 7ea8b5e4.
- **Focus Reading = already done** (== `bionicReading`, render-time, better than CrossPoint's cached version). No port.
- **Audit complete** (Explore agent): full gap map below.

## Version note
Now on **3.0.0** (major bump done — breaking changes shipped). Continue remaining features as **3.x minor** bumps (3.1.0, 3.2.0, …); another major bump only if a further breaking change (e.g. the v25 format) warrants it.

## Remaining (scoped this run) — recommended order: web/OPDS-search → KOReader → RTL → v25
- All small wins + multi-server OPDS are SHIPPED. Remaining below (each medium-to-large, none a quick win).

## Rules for every item
- Re-implement adapting to AALU's diverged code, NOT diff-apply. English-only (`lib/I18n/translations/english.yaml` only). No source comments unless a load-bearing WHY (ask first). Enum/GraphQL docs exempt (N/A here). 380 KB RAM discipline. New screens use `GUI.drawButtonHintsGlyphs`, not the legacy bar. Own minor version + full CI mirror (clang-format-21, `pio check` med/high, `pio run`, `cd test/build && cmake --build . && ctest`) + sim build before each commit. Leave `git push` to the user.

## Remaining backlog (recommended order: small verifiable → large named → format)

### Small / self-contained (verifiable by build)
1. **End-of-book automation toggles** (medium). Two additive `uint8_t` settings (default 0, no migration): `removeReadBooksFromRecents`, `moveFinishedToReadFolder`. CrossPoint `CrossPointSettings.h:263,265`, `SettingsList.h:192-195`, logic `EpubReaderActivity.cpp:223,298-318` (reader state `recentsEntryRemoved`, `pendingReadFolderMove`; move happens in `onExit`). AALU end-of-book hook: `currentSpineIndex >= epub->getSpineItemsCount()`. Verify AALU `RecentBooksStore` has `removeByPath`; add `isInReadFolder` + `/Read/` move helper. New strings `STR_REMOVE_READ_FROM_RECENTS`, `STR_MOVE_FINISHED_TO_READ`. Touches the big `EpubReaderActivity.cpp`.
2. **Bookmark added/removed confirmation** (small-medium — NOT tiny, integration surface found). AALU strings `STR_BOOKMARK_ADDED`/`STR_BOOKMARK_REMOVED` ALREADY EXIST. `toggleBookmark()` (`EpubReaderActivity.cpp` ~640-713) detects add vs remove at ~696 (`bookmarks.size() == countBefore` → added). Caller `ADD_BOOKMARK` does `toggleBookmark(); requestUpdate();` (line 408-411) so an inline popup gets erased by the re-render — need CrossPoint's TIMED OVERLAY: add reader state `showBookmarkMessage`/`bookmarkRemoved`/`bookmarkMessageTime`, set in `toggleBookmark`, draw the popup INSIDE `renderContents` before its `displayBuffer`/`displayWithRefreshCycle` calls (it displays across 4 branches ~1090-1097, gate each on `showBookmarkMessage`), and clear after ~1.5 s in `loop()` (+ `requestUpdate`). Also wire the reader Aa/menu/button that maps to ADD_BOOKMARK.
3. **Crash-safe atomic progress writes** (small, reliability). CrossPoint `ProgressFile::writeAtomic()` (write `.tmp` → remove → rename, avoids FAT corruption, ref bug #2275). AALU saves progress via plain `Storage.openFileForWrite` in `EpubReaderActivity.cpp` (~1024-1026) + `XtcReaderActivity.cpp` + `TxtReaderActivity.cpp`. Add a shared atomic-write helper, use in all 3.
4. **Custom / "Never" sleep timeout** (small-medium, has a migration). CrossPoint stores raw `sleepTimeoutMinutes` (1-30, 31=Never) via reusable `IntervalSelectionActivity` (`activities/util/IntervalSelectionActivity.{cpp,h}`, `SettingsActivity.cpp:198,336-338`). AALU uses fixed enum `SLEEP_TIMEOUT` (`CrossPointSettings.h:118-125`). Port the widget; migrate the enum→minutes (needs a settings-version bump / mapping). Menu-option gap.
5. **#2452 refinements** (small, I know this code). CrossPoint extras not in AALU: `BUILD_POPUP_BYTE_THRESHOLD` (popup for giant-HTML single-spine chapters, `EpubReaderActivity.h:95`). NOTE: AALU deliberately full-builds the percent/anchor/reflow cases, so CrossPoint's `applyDeferredReposition` + `BUILD_WINDOW_AHEAD`/`suspendBuild` are NOT needed unless we later add bounded look-ahead + partial-file persistence (giant single-spine reopen). See [[aalu-port-backlog-progress]].

### Web / OPDS (medium)
6. **Web `/api/wifi`** management (small). CrossPoint `CrossPointWebServer.cpp:177-179,1413-1526` GET/POST/delete for saved WiFi. AALU shares `WifiCredentialStore` (on-device mgmt exists) but its `CrossPointWebServer.cpp` has no `/api/wifi` route. Add the route + reuse the store.
7. **OPDS catalog in-book search** (small-medium). CrossPoint `OpdsBookBrowserActivity` has `SEARCH_INPUT` state + `launchSearch()`/`performSearch(query)`. AALU's `OpdsBookBrowserActivity` has no search state. Add a search input (reuse `KeyboardEntryActivity`) issuing the catalog's OpenSearch query. Now builds cleanly on the shipped multi-server browser (each browser instance already has its `OpdsServer`).
8. ✅ **Multi-server OPDS management — SHIPPED (3.0.0, 7ea8b5e4).** See shipped list above. Also still-open follow-on: web `/api/opds` (browser-side add/edit/delete of servers) — pairs with item 6's web WiFi work.

### Large / user-named (need external verification)
9. **KOReader exact-path sync** (medium-large, remote-verifiable only). CrossPoint `lib/KOReaderSync/ChapterXPathResolver.{cpp,h}` (600 lines: re-parse chapter, resolve a real DOM XPath → exact page) + heavy `ProgressMapper.cpp` (903 lines). AALU `ProgressMapper.cpp` is 138 lines, heuristic ("~6 DOM nodes/page", `generateXPath`/`toCrossPoint`). **AALU's SEND side is already exact** (uses `page->syncXPath` from the parser's `childTracker`/`pathElements`/`currentXPath`); the gap is the RECEIVE side (map an incoming XPath → exact page). Port `ChapterXPathResolver` (adapt to AALU's parser) + upgrade `ProgressMapper::toCrossPoint`. Does NOT touch layout/section format. Verify against a real KOReader server.
10. **RTL / Bidi text** (large, RTL-book-verifiable only). CrossPoint `lib/MiniBidi/` (~1090 lines: `minibidi.c`, `BidiUtils`, bidi class tables — self-contained, copyable). Integration: `BlockStyle.isRtl`; CSS `direction:rtl` (`CssParser.cpp:397`) + `dir="rtl"` (`ChapterHtmlSlimParser.cpp:399`); per-paragraph/per-word RTL detection + `computeVisualWordOrder` (visual reordering) + RTL alignment in `ParsedText.cpp`; `detectParagraphLevel` + **`isRtl` serialized in `section.bin`** (`TextBlock.cpp:271,349` → SECTION_FILE_VERSION bump); plain-text RTL in `TxtReaderActivity.cpp:363` (`startsWithRtl`). English-only UI stays; this is book-content only. Watch flash (currently 4.92 MB / 6.55 MB).

### Last
11. **Converge `section.bin` to CrossPoint v25** (`[[aalu-section-format-v25-target]]`). AALU is v21. v25 adds paragraph/list-item LUTs (KOReader page refinement) + per-word focus-reading split + `isRtl`. Do AFTER #9 (sync) + #10 (RTL) since those bring the fields. Then rewrite the `docs/file-formats.md` `section.bin` block (currently CrossPoint-stale: says v25+focus-reading while AALU is really v21).

### Explicitly NOT porting (AALU intentional divergences)
- **Language selector** — AALU removed it in 1.2.0 (English-only, `SettingsActivity.cpp:67-69`). Do NOT restore.
