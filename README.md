<p align="center">
  <img src="docs/qa/v2.2.8/home-now-reading.png" width="280" alt="InkPoint X 2.2.8 home screen on XTEINK X4">
</p>

<h1 align="center">InkPoint X</h1>

<p align="center">
  Open-source reader firmware designed for the XTEINK X3 and X4.
  <br>
  A focused library, capable file management, multilingual typography, and an e-ink-native interface.
</p>

<p align="center">
  <img alt="Version 2.2.9" src="https://img.shields.io/badge/version-2.2.9-000000">
  <img alt="Target: XTEINK X3 and X4" src="https://img.shields.io/badge/target-XTEINK%20X3%20%2B%20X4-111111">
  <img alt="Displays: 528 × 792 and 480 × 800 monochrome" src="https://img.shields.io/badge/display-528%C3%97792%20%2F%20480%C3%97800-555555">
  <img alt="Platform: ESP32-C3" src="https://img.shields.io/badge/platform-ESP32--C3-8A8A8A">
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-black"></a>
  <a href="https://ko-fi.com/yokkivans"><img alt="Support on Ko-fi" src="https://img.shields.io/badge/support-Ko--fi-F16061?logo=kofi&logoColor=white"></a>
</p>

> [!IMPORTANT]
> The `main` branch contains the current stable source, while `dev` is used for active development. For a prebuilt,
> user-facing binary, use the [Releases](https://github.com/yokki-vans/InkPointX/releases) page unless you specifically
> want to test development changes. Devices already running InkPoint X can update over the air from
> **Settings → System → Check for updates**.

## Overview

InkPoint X is a complete firmware experience for the XTEINK X3 and X4 rather than a collection of isolated reader patches.
The interface, input model, font system, library, file operations, network transfer, settings, and e-ink refresh
strategy adapt at boot to the X3's 528 × 792 or X4's 480 × 800 monochrome panel and physical controls.

The project is based on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) and keeps its
open architecture while adding an independent InkPoint X product layer, expanded document support, a redesigned
interface, and controller-specific display tuning.

## What's new in 2.2

- **Large-library and dictionary stability in 2.2.9.** The Books view now uses a compact, persistent catalogue instead
  of keeping hundreds of heap-allocated strings in memory, opens the cached catalogue immediately, supports up to
  1,200 books, and provides an explicit **Rescan library** action. StarDict lookup uses a bounded sparse index with a
  validated on-card sidecar cache, reports indexing progress, and rejects corrupt offsets without rebooting. Release
  edges from a parent confirmation screen are quarantined, so Device info and other detail screens no longer close as
  soon as they open.
- **Stable Focus Reading and centred modal notifications in 2.2.8.** Focus Reading no longer requests an increasingly
  large contiguous heap block while splitting long EPUB paragraphs, eliminating the `std::bad_alloc` reboot seen with
  some books on the ESP32-C3. Contextual messages such as indexing, loading, lookup, and error states now share one
  centred 1:1 card with rounded corners, a restrained e-ink shadow, a light background scrim that preserves the page
  beneath it, and a matching rounded progress bar on both X3 and X4.
- **Correct EPUB cover scaling in 2.2.7.** Low-resolution 1-bit thumbnails produced by progressive JPEG covers now
  expand to the full aspect-preserving artwork area on the Now Reading screen instead of remaining in its top-left
  corner. Existing malformed thumbnail caches are supported immediately, and stale prepared Home tiles are invalidated
  automatically. Covers remain uncropped and the fix is limited to the Home artwork path.
- **PDF zoom and safer PDF opening in 2.2.6.** Fixed-layout PDFs such as music scores open through a bounded-memory
  conversion path again. The reader menu now offers 100%, 125%, 150%, and 200% PDF zoom; zoomed pages are explored as
  four overlapping viewports with the normal page buttons, and the selected zoom is remembered per document. The
  update screen also downloads and presents the release's **What's new** notes before installation. On the home screen,
  a book without embedded artwork now receives a full-size typographic cover with its title at the top and author at
  the bottom; explicit metadata visibility settings still take priority.
- **A faster, adaptive Now Reading screen in 2.2.5.** Book title and author each support **Default**, **Show**, and
  **Hide** under Interface settings. The default keeps cover-backed books visually clean, while books without artwork
  retain their title; explicit user choices always win. The cover expands into every released row without cropping,
  and its progress bar follows the exact rendered cover width. Cover, percentage, page estimate, and reading time now
  arrive in one complete frame from lightweight geometry-aware caches instead of appearing after the screen is shown.
  Physical-button legends no longer animate a pressed state, eliminating the second e-ink refresh formerly needed on
  release and making horizontal navigation materially faster.
- **Korean system language in 2.2.4.** All 537 interface strings are available in Korean, and every UI size embeds
  an exact, 1-bit **Noto Sans KR** subset for crisp Hangul on both X3 and X4. Korean automatically uses this complete
  built-in family instead of a potentially incompatible custom interface or accent font; the custom selection is
  preserved and returns when another language is selected.
- **Reliable X3 navigation and Wi-Fi in 2.2.3.** X3 focus surfaces now use a panel-appropriate, higher-contrast
  treatment that stays visible during fast side-button navigation. Wi-Fi association scans all channels, chooses
  the strongest matching access point, keeps the station radio active between scan and connect, ignores stale
  immediate failure states, and waits for DHCP before reporting success.
- **Live OTA progress in 2.2.2.** The updater now releases the activity render lock while downloading, validating,
  and flashing, so the progress bar, percentage, phase, and byte counters update on screen instead of remaining at
  zero. Progress values are synchronized safely between the installer and e-ink render tasks.
- **Safer, cleaner web pairing in 2.2.1.** Long pairing URLs no longer run outside the X3/X4 display: the full
  credential stays in the QR payload while the address and bounded pairing code use separate lines. Web-interface
  authorization now has an enabled-by-default switch under **Settings → Network & Sync** and consistently controls
  HTTP, WebSocket uploads, and WebDAV.
- **Reading statistics.** Library now includes an e-ink-native statistics hub with an overview, the last 7 days,
  the last 8 weeks, per-book details, reading-time habits, sessions, streaks, completed books, and up to 730 days
  of compact history. EPUB, FB2, PDF, XTC, TXT, and Markdown reading all contribute to the same totals.
- **Clearer library and reader navigation.** Book rows keep secondary information quiet: the author uses a smaller
  face, non-zero progress follows it after a middle dot, and an unopened book gets a compact `NEW` mark inside the
  existing book icon. The in-reader menu is one vertical list with section dividers and an icon for every action;
  chapter and menu position share one compact footer line.
- **StarDict dictionaries in EPUB.** Select a word on the page and look it up without leaving the reader. Uncompressed
  `.ifo`, `.idx`, and `.dict` sets are discovered from `/dictionaries/<name>/`, indexed with bounded RAM, and selected
  under **Settings → Reading → Dictionary**.
- **On-device font catalog.** Reader and interface font families can be downloaded, updated, selected, and removed
  directly from Settings. Downloads are transactional, CRC-verified, show real e-ink-friendly progress, and reuse the
  GitHub TLS session; a measured four-file family now installs in about 60 seconds instead of 100 on X4.
- **Broader fixed-layout PDF support.** Large vector scores and diagram-heavy pages use bounded rasterization and
  geometry-aware caches on both X3 and X4 instead of aborting under ESP32-C3 memory pressure.
- **Faster interaction.** Navigation edges are queued and coalesced while the panel is busy, automatic menu CLEAN
  flashes and release-time button-animation redraws are removed, and the normal UI stays on the controller's fast
  differential path.
- **Production hardening.** Network downloads and OTA writes are staged atomically, retries always restart from a
  clean file, caches carry source fingerprints and integrity records, and destructive dialogs require a distinct
  confirmation click.
- **Lower idle and reading cost.** Indexing, metadata caching, page layout, SD access, glyph reuse, battery polling,
  and power-state transitions were audited together so background work no longer competes with input or reading.

## What's new in 2.1

- one runtime-detecting ESP32-C3 image supports both XTEINK X3 and X4;
- X3 support covers both UC8253 and newer UC8279 panel-controller production runs;
- X3 battery percentage, DS3231 clock, QMI8658 tilt gestures, microSD power rail, wake and deep sleep use the active hardware profile;
- PDF music, diagrams and other fixed-layout graphics rasterize at the active device width (528 px on X3, 480 px on X4), with geometry-aware caches;
- X3 Quick Resume uses its differential display path to avoid unnecessary full-screen flashes.

See [the X3 implementation notes](docs/X3_SUPPORT_RU.md) for hardware details and the validation matrix.

### Design principles

- clear hierarchy and restrained, high-contrast layouts;
- consistent focus, headers, rows, dialogs, icons, and hardware-button legends;
- no touch assumptions, color-only states, shadows, or animation-heavy interactions;
- fast differential updates for navigation, with deliberate clean refreshes when the panel needs them;
- readable 1-bit typography and predictable behavior in every supported language.

## What's new in 2.0

Version 2.0 is a full design and engineering pass over the entire firmware — every screen was audited element by
element, and the result was verified by reading the device's own framebuffer rather than by eye.

### Interface

- **A calmer home screen.** The reading card is one clear hierarchy instead of five near-equal centred rows: a
  larger cover, the title, the author, then a single progress band that carries percent, pages and invested time.
- **A handwritten accent voice.** Caveat sets the moments that are about a person rather than a function — home
  greetings and hub headers, the quieter author line, empty states, successful completion, and end-of-book screens.
  Functional text, list rows, settings, progress, and every error state stay in the structural face on purpose.
- **A type scale one step smaller** across the whole interface, so long book titles fit where they used to
  ellipsize, without making any list denser.
- **A minimal reader status bar.** The reading page's bottom lane uses the smallest legible size, and the pixels it
  gives back go to the book text.
- **Shared layout primitives.** Empty states, full-screen reader messages, list content bounds and position
  counters come from one place, so equivalent screens no longer disagree by a few pixels or a font size.
- **A one-size button legend** whose seam shifts to fit a wide label, instead of the whole bar changing size as the
  selection moved.
- **Larger, unmistakable toggles** — on and off differ by fill, not only by knob position.

### Reliability

- **Atomic settings writes.** Every JSON store is written to a temporary file and swapped in with a rename. The
  previous delete-then-write could destroy `settings.json`, `state.json` or `wifi.json` outright if power was lost
  mid-save.
- **A working task watchdog.** It was armed with no subscribers, so all 25 feed calls in the network code were
  no-ops and an application-level hang required force-powering the device.
- **Real OTA rollback.** A pending image is confirmed only after the device boots and stays healthy, so an update
  that panics on start rolls back instead of bricking the slot.
- **Out-of-memory guards** across the pagination and parsing paths, where an unguarded allocation used to reboot
  the device instead of failing back to the library.
- **Post-mortem breadcrumbs.** On battery the device has no console, so each boot reports how the previous session
  ended — the screen that was up, how long it had run, and whether the firmware asked for the power-down — to the
  serial log and to `/.crosspoint/diag.log`.

Three power optimizations from the first 2.0 build were withdrawn in 2.0.2 after they proved unreliable in the
field: idle light sleep, unconditional panel-rail power-down, and the critical-battery force-sleep. All three ran
only on battery, which is the one configuration a USB-tethered bench cannot observe. See
[design-qa.md](design-qa.md) for the post-mortem.

### Battery and performance

- **Glyph caches survive page turns**, so body text is no longer re-inflated on every page.
- **Progress screens repaint per percent**, not per 2 KB chunk, which removed roughly 120 full-panel refreshes per
  minute of downloading.
- **Reading position is coalesced** instead of costing four filesystem operations per page turn.

The full element-by-element findings and decisions are recorded in [design-qa.md](design-qa.md).

## Interface

The home screen is organized into three horizontal pages:

1. **Now Reading** — the largest safe uncropped cover and a compact progress band; title and author visibility adapt
   to the artwork by default and remain independently configurable.
2. **Library** — Books, Files, Gallery, Favorites, Reading Stats, plus a dedicated Transfer subsection.
3. **Settings** — focused submenus for interface, power, reading, controls, files, network, and system options.

Selection uses a subtle rounded gray surface instead of a heavy inverted bar. Compact legends at the bottom mirror
the two physical two-section controls and always show their current assignments. The legends can be disabled in
Settings and are intentionally omitted from the reading page.

<table>
  <tr>
    <td align="center"><img src="docs/qa/v2.2.8/home-now-reading.png" width="220" alt="Now Reading"><br><sub>Now Reading</sub></td>
    <td align="center"><img src="docs/qa/v2.2.8/home-library.png" width="220" alt="Library hub"><br><sub>Library</sub></td>
    <td align="center"><img src="docs/qa/v2.2.8/library-books.png" width="220" alt="Books"><br><sub>Books</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/qa/v2.2.8/home-settings.png" width="220" alt="Settings hub"><br><sub>Settings</sub></td>
    <td align="center"><img src="docs/qa/v2.2.8/settings.png" width="220" alt="Interface settings"><br><sub>Interface settings</sub></td>
    <td align="center"><img src="docs/qa/v2.2.8/reader-page.png" width="220" alt="Reading page"><br><sub>Reading page</sub></td>
  </tr>
</table>

All screenshots show InkPoint X 2.2.8 and were captured from an XTEINK X4's own framebuffer over serial, not rendered
on a host.

The detailed interface specification is available in [docs/inkpoint-x-ui.md](docs/inkpoint-x-ui.md).

## Features

### Library and reading

- recursive book discovery on microSD;
- a Books view restricted to **EPUB**, **FB2**, and **PDF**;
- sorting by title, author, format, and recent activity;
- Favorites stored separately from the book files;
- compact book rows with a smaller author line, non-zero reading progress, and an in-icon `NEW` marker for unopened books;
- reading progress, bookmarks, table of contents, book information, and per-book statistics;
- global reading statistics with overview metrics, 7-day and 8-week charts, book breakdowns, habits, sessions,
  streaks, completed-book counts, and a rolling 730-day history;
- configurable font, size, line spacing, margins, alignment, hyphenation, orientation, and inversion;
- automatic page turning, screenshots, QR display, OPDS, and KOReader Sync;
- low-memory **StarDict** lookup for EPUB, with dictionary selection in Reading settings;
- a vertically grouped in-reader menu with section labels, action icons, and a single compact chapter/position footer;
- an on-device gesture reference, so the reader's hold gestures are discoverable;
- support for **XTC**, **TXT**, and **Markdown** when opened from Files, with CP1251/KOI8-R/CP1252/ISO-8859-5/CP866
  detection for legacy single-byte text.

### Gallery

- discovers **BMP**, **JPEG**, and **PNG** images across the card;
- includes images and screenshots created while reading;
- opens images in a viewer adapted to the active X3/X4 display;
- uses corrected 1-bit scaling and dithering to avoid block and moiré artifacts.

### File manager

Files is a full on-device file manager, not a second book list:

- browse folders and inspect file properties;
- create folders;
- copy and move files or folders;
- rename items;
- delete files and directory trees with confirmation;
- open supported books and images directly.

### Transfer and network

- join an existing Wi-Fi network;
- receive books wirelessly from Calibre;
- create a local access point;
- upload, download, rename, move, and delete files through the web interface;
- pairing-protected web and WebDAV access, with an enabled-by-default authorization switch under Network & Sync;
- OPDS catalog browsing, with cancellable downloads;
- over-the-air updates with rollback protection.

### Device settings

Settings are grouped by purpose rather than split across tab and “advanced” screens:

- interface language and interface font;
- button legends and battery indicator visibility;
- sleep, lock, power, and refresh behavior;
- reading defaults and reader status bar;
- button remapping;
- library and file behavior;
- Wi-Fi, OPDS, Calibre, and synchronization;
- cache maintenance, firmware update, device information, and safe settings reset.

Resetting settings preserves books, reading progress, bookmarks, statistics, recent books, and favorites.

## Typography and languages

The system interface uses **Inter Medium** for normal text and **Inter SemiBold** for headings, selection, and
emphasis, instanced from Inter's variable `wght` and `opsz` axes so each size gets its own optical treatment.
**Caveat** supplies the handwritten accent voice. Inter carries no Hebrew, Arabic, or Korean, so **Noto Sans Hebrew**,
**Noto Naskh Arabic**, and **Noto Sans KR** supply exactly the code points it is missing. Full font files are not embedded. During the
build, `scripts/build_ui_fonts.py` scans every string in `lib/I18n/translations/*.yaml` and generates compact native
subsets containing only the glyphs the firmware needs.

The firmware currently provides complete UI resources for 28 languages:

<details>
<summary>Show language list</summary>

Arabic, Belarusian, Catalan, Czech, Danish, Dutch, English, Finnish, French, German, Hebrew, Hungarian, Italian,
Kazakh, Korean, Lithuanian, Polish, Portuguese (Brazil), Romanian, Russian, Slovak, Slovenian, Spanish, Swedish, Turkish,
Ukrainian, Valencian, and Vietnamese.

</details>

Every user-facing string is translated — including month abbreviations, sync and font-manager errors, and the SD
card failure screen, all of which were English-only before 2.0. Menu labels and row titles are measured against the
real glyph advance tables during development, so they fit their lanes instead of ellipsizing.

The text pipeline supports:

- extended Cyrillic used by Belarusian, Kazakh, Russian, and Ukrainian;
- Vietnamese diacritics and NFC composition;
- bidirectional Hebrew and Arabic text;
- contextual Arabic shaping for both translated UI strings and dynamic book, author, and file names;
- complete Hangul coverage for the Korean system interface through Noto Sans KR subsets;
- mirrored accessories and layout behavior for RTL content.

The interface font and the handwritten accent face can also come from the card:
**Settings › Interface › Interface font** and **Accent font** list the built-in face first, then every family
installed in `/.fonts/`. A slot takes a card size only when the family ships one within 2 pt of it, so the layout
keeps its proportions; slots the family cannot cover stay on the built-in face. Reader packs typically start at
12 pt, which covers everything except the smallest captions.

Reader fonts are independent from the UI font. Optimized `.cpfont` families can be installed in `/.fonts/` or
`/fonts/` on microSD, downloaded through **Settings → System → Manage Fonts**, or uploaded from the web font manager.
The device catalog supports install, update, progress, checksum validation, and confirmed removal. The reader font
pipeline includes a Noto Serif family for Latin/Cyrillic/Vietnamese with Noto Naskh Arabic fallback. See
[docs/sd-card-fonts.md](docs/sd-card-fonts.md).

## E-ink behavior

InkPoint X contains a panel-aware refresh policy built around the active controller's actual behavior:

- controller RAM is synchronized after updates so differential refreshes compare against a valid previous frame;
- interactive navigation avoids full-screen black flashes;
- explicit clean and full refresh paths are retained for recovery from accumulated ghosting;
- the first update after controller initialization uses a stronger waveform;
- grayscale and 1-bit image paths use panel-aware conversion;
- home covers use geometry-keyed prepared tiles, so repeated carousel visits restore the exact final region instead
  of decoding and rescaling the source bitmap again;
- button debounce is tuned for the shared X3/X4 ADC ladder so one physical press produces one action.

E-ink cannot behave exactly like an emissive phone display, but normal navigation is designed to feel immediate
without trading away panel cleanliness.

## Installation

### Over the air

On a device already running InkPoint X, open **Settings → System → Check for updates**. The updater verifies the
release over HTTPS, stages the image, and switches boot slots only after the whole binary has landed. If the new
firmware fails to start, the bootloader rolls back to the previous slot automatically.

### Prebuilt firmware

Download `firmware.bin` from [Releases](https://github.com/yokki-vans/InkPointX/releases). It is a universal X3/X4 image;
the device-labelled X3 and X4 files in the same release are byte-identical aliases for convenience.

#### Recovery update from microSD

1. Format a microSD card as FAT32.
2. Copy `firmware.bin` to the root of the card.
3. Safely eject the card and insert it into the reader.
4. Power the device off.
5. Hold the **left side / Up** button while powering on.
6. Choose the firmware file in Recovery Mode and confirm the update.
7. Keep the device powered until it restarts.

#### Flash over USB

Install [esptool](https://github.com/espressif/esptool), connect the reader over USB-C, and run:

```bash
esptool --chip esp32c3 \
  --port /dev/ttyACM0 \
  --baud 921600 \
  write-flash 0x10000 firmware.bin
```

Replace `/dev/ttyACM0` with the actual serial port. On macOS it is usually named `/dev/cu.usbmodem*`.

> [!CAUTION]
> Flash only InkPoint X binaries built for the XTEINK X3/X4 ESP32-C3 family, do not disconnect power while writing, and keep a recovery-capable
> microSD card available when testing development builds.

## Build from source

### Requirements

- Git with submodule support;
- Python 3;
- [PlatformIO Core](https://platformio.org/install/cli);
- internet access on the first build for declared toolchains, libraries, and font sources.

### Clone the development branch

```bash
git clone --branch dev --recurse-submodules https://github.com/yokki-vans/InkPointX.git
cd InkPointX
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

### Build

Development build:

```bash
pio run -e default
```

Release-style binary:

```bash
pio run -e gh_release
```

The resulting binary is:

```text
.pio/build/gh_release/firmware.bin
```

Upload directly through PlatformIO:

```bash
pio run -e gh_release --target upload
```

## Validation

Run localization and font coverage checks:

```bash
python3 scripts/validate_i18n.py
```

Run host tests:

```bash
cmake -S test -B test/build -DCMAKE_BUILD_TYPE=Release
cmake --build test/build
ctest --test-dir test/build --output-on-failure
```

Run static analysis:

```bash
pio check -e default --fail-on-defect=medium
```

Release validation includes the host suite, localization coverage across 28 languages, development and release
compilation, static analysis, PDF conversion checks, and a hard flash budget so the image cannot silently grow into
the OTA slot's limit.

## Releases and OTA

Pushing a tag builds the universal `gh_release` image and publishes `firmware.bin`, X3/X4-labelled aliases, and
SHA-256 checksums. The on-device updater reads `releases/latest` and looks for exactly that asset. It downloads to a
temporary SD-card file, requires an exact size and GitHub release SHA-256 match, validates the complete ESP image,
and only then writes the inactive OTA slot. Network operations retry three times; a failed download or validation
never selects the candidate image. Version comparison is semantic — major, then minor, then patch — with release
candidates treated as older than the final tag.

## Repository layout

```text
src/                         Firmware activities, settings, stores, and UI
lib/                         Readers, rendering, fonts, bidi, i18n, and HAL
freeink-sdk/                 X3/X4 display, input, storage, and hardware libraries
scripts/                     Code generation, font subsetting, and validation
test/                        Host-side unit and policy tests
docs/                        User, developer, attribution, and visual QA docs
design-qa.md                 Design audit findings and deliberate decisions
platformio.ini               ESP32-C3 build environments
partitions.csv               16 MB flash partition layout
```

The `freeink-sdk` submodule points to
[`yokki-vans/community-sdk`](https://github.com/yokki-vans/community-sdk/tree/inkpointx-v2.2), the InkPoint X hardware
branch based on FreeInk SDK. It provides the runtime X3/X4 board profiles, panel drivers, input, sensors, storage,
TLS transport, and power management used by this firmware.

## Data and storage

Books stay on the microSD card. InkPoint X stores its generated caches and application data under `/.crosspoint/`.
Settings and application state are written atomically, so an interrupted save leaves the previous file intact —
but a separate backup of irreplaceable files remains the safest protection before testing development builds.

The web interface and device file manager can modify or delete files. Destructive operations require confirmation.

> [!NOTE]
> Updating to 2.0 changes the reader viewport slightly, because the reading page's status bar is smaller. Each book
> re-indexes once the first time it is opened afterwards; reading positions are preserved.

## Project origin and attribution

InkPoint X is derived from CrossPoint Reader and includes work from its contributors and the wider open-source
e-reader community. Third-party components, fonts, and icons retain their original licenses.

- Project license: [MIT](LICENSE)
- Third-party notices: [docs/third-party-notices.md](docs/third-party-notices.md)
- Lucide icon notice: [docs/licenses/lucide-ISC.txt](docs/licenses/lucide-ISC.txt)

## Contributing

Bug reports, hardware observations, translations, documentation improvements, and focused pull requests are
welcome. When changing the interface, validate it against both 528 × 792 and 480 × 800 framebuffers and, whenever
possible, physical X3 and X4 panels.

Please run the relevant validation commands above before opening a pull request.

## Support

If InkPoint X is useful to you, you can support ongoing development on
[Ko-fi](https://ko-fi.com/yokkivans).
