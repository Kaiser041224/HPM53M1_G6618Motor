# TNO Authentic v2 Showcase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a second, independently judged showcase that follows the observed TNO Guangdong screenshots: dark teal-black industrial windows, muted text, image-led product/research cards, thin graph lines, and restrained state colors.

**Architecture:** Keep v1 untouched. Create `D:\Download\TNO-assets\09-showcase\v2-tno-authentic` with a source-aware web asset tree, shared CSS/JS, a small DDS decoder for the selected uncompressed BC-free assets, and two pages: an asset page plus a fully standalone CSS/SVG page.

**Tech Stack:** HTML5, CSS, inline SVG, vanilla JavaScript, Node.js 24, PowerShell 5.1, no third-party dependencies.

## Global Constraints

- Do not modify or delete v1 files.
- Do not use the existing workspace GUI design system as the visual source of truth.
- Use low-saturation dark teal-black surfaces; no neon dashboard treatment.
- Use image-led windows, narrow old-game controls, thin graph strokes, and restrained red/yellow status accents.
- The standalone page must have no external URL, external script, external stylesheet, or raster/font dependency.
- No subagents or delegated tasks are used for this implementation.

---

### Task 1: Create v2 directories and selected asset manifest

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\assets\{windows,charts,products,research,map,fonts}`
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\asset-manifest.json`
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\conversion-log.md`

- [ ] Create only the new v2 directories.
- [ ] Record selected DDS source paths and browser destinations for graph background, graph bars, product R&D frame, research progress bars, one product image, map state outline, `AldrichTNOV13.ttf`, and one event PNG.
- [ ] Keep unconverted sources explicitly marked as fallback/reference instead of claiming browser support.

### Task 2: Add a dependency-free DDS decoder for selected web assets

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\tools\dds-to-png.js`
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\tools\test-dds-to-png.js`

**Interface:** `node dds-to-png.js --input <dds> --output <png>` supports uncompressed 32-bit ARGB/RGBA DDS and rejects unsupported compression with a clear error.

- [ ] Write the decoder test against the DDS header, dimensions, output PNG signature, and non-zero output size.
- [ ] Implement DDS header parsing, row orientation handling, BGRA/RGBA channel conversion, and a minimal PNG encoder using Node built-ins.
- [ ] Run the decoder against the selected assets; record unsupported formats in `conversion-log.md` and use CSS/SVG fallback for them.

### Task 3: Build the authentic v2 style system

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\styles\authentic-tno.css`
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\styles\authentic-tno.js`

- [ ] Define `#001010`-family surfaces, low-saturation gray-teal text, muted blue, restrained red/yellow, thin inset borders, tiled texture, title strips, and old-game button states.
- [ ] Implement fixed 1920×1080 composition with responsive scaling, not modern equal-card dashboard layout.
- [ ] Implement window chrome, product card, research frame, economic graph, narrow progress bar, map state outline, event card, decision row, status icon, terminal log, and motor parameter table.
- [ ] Implement only light interactions: tab switch, parameter edit marker, apply/revert feedback, and LIVE/STALE/UNKNOWN state cycling.

### Task 4: Create the asset-enhanced authentic page

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\tno-authentic.html`

- [ ] Build a fixed game-window shell with top status bar, left navigation rail, central active window, right status strip, and bottom message area.
- [ ] Include three switchable windows: Guangdong overview, economy/product/research, and motor control terminal.
- [ ] Use converted graph/product/research/map assets where available, and visibly degrade to CSS texture rather than broken images.
- [ ] Make the motor UI read like an industrial R&D/product decision window: motor profile, current loop development, velocity loop development, power-stage validation, parameter decision table, and protection events.

### Task 5: Create the standalone authentic page

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\tno-authentic-standalone.html`

- [ ] Inline the v2 CSS/JS.
- [ ] Replace every optional raster with CSS texture or inline SVG.
- [ ] Add `STANDALONE / NO EXTRACTED ASSETS` label.
- [ ] Ensure no external URL, external CSS/JS, DDS, PNG, JPG, TTF, WOFF, or `src`/`href` dependency remains.

### Task 6: Validate and document the v2 deliverable

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\v2-tno-authentic\validate-v2.js`
- Modify: `conversion-log.md`

- [ ] Validate required content, relative asset paths, standalone isolation, JavaScript syntax, PNG signatures, and decoder output.
- [ ] Open both pages through a local static server or direct browser file loading and verify all three windows plus motor parameter interactions.
- [ ] Record exact command output and known limitations in `conversion-log.md`.
