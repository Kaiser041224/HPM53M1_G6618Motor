# TNO Motor UI Showcase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a detailed local HTML showcase under `D:\Download\TNO-assets\09-showcase` that demonstrates the TNO Guangdong / Silicon Dreams visual language, a reusable component catalogue, and a motor-tuning upper-computer UI in the same design system.

**Architecture:** Keep the original extracted assets untouched. Add a small web-ready asset catalogue and conversion manifest under `D:\Download\TNO-assets\00-catalog` and `D:\Download\TNO-assets\08-web-ready`. Produce two self-contained page entry points: an asset-enhanced page that references selected converted assets and a standalone page that uses only CSS/SVG/inline data. Both pages share the same component markup and CSS variables, with lightweight JavaScript for tabs, buttons, sliders, parameter editing, and status toggles.

**Tech Stack:** HTML5, CSS3, inline SVG, vanilla JavaScript, PowerShell 5.1 for filesystem/catalogue work, Node.js for offline validation, ImageMagick or available local image tooling only when already installed.

## Global Constraints

- Do not modify or delete the original directories under `D:\Download\TNO-assets\01-design-sources` through `07-misc`.
- Keep the final deliverables under `D:\Download\TNO-assets\09-showcase`.
- Keep converted files separated by purpose: fonts, chrome, icons, event-art, map-art, and achievements.
- Provide both `tno-ui-showcase.html` and `tno-ui-showcase-standalone.html`.
- The standalone page must load with no external network request and no dependency on extracted image/font files.
- Use TNO Guangdong facts as visual references, while clearly labelling the motor-control section as an original design exercise.
- Encode status with color, icon, and text; do not rely on color alone.
- Include `LIVE`, `STALE`, and `UNKNOWN` telemetry states.
- Do not overwrite the existing `Gui/ref` sample or token files in the HPM project.

---

### Task 1: Create the asset catalogue and web-ready directory layout

**Files:**
- Create: `D:\Download\TNO-assets\00-catalog\asset-index.json`
- Create: `D:\Download\TNO-assets\00-catalog\guangdong-ui-map.md`
- Create: `D:\Download\TNO-assets\00-catalog\conversion-log.md`
- Create directories: `D:\Download\TNO-assets\08-web-ready\fonts`, `backgrounds`, `ui-chrome`, `icons`, `event-art`, `map-art`, `achievements`
- Create directories: `D:\Download\TNO-assets\09-showcase\assets`, `styles`

**Interfaces:**
- Produces stable relative paths consumed by the asset-enhanced HTML.
- `asset-index.json` records source path, destination path, source extension, output extension, dimensions when known, and selection reason.

- [ ] **Step 1: Verify destination parents and create only the new directories.**

Run in PowerShell:

```powershell
Test-Path -LiteralPath 'D:\Download\TNO-assets'
New-Item -ItemType Directory -Force -Path 'D:\Download\TNO-assets\00-catalog','D:\Download\TNO-assets\08-web-ready\fonts','D:\Download\TNO-assets\08-web-ready\backgrounds','D:\Download\TNO-assets\08-web-ready\ui-chrome','D:\Download\TNO-assets\08-web-ready\icons','D:\Download\TNO-assets\08-web-ready\event-art','D:\Download\TNO-assets\08-web-ready\map-art','D:\Download\TNO-assets\08-web-ready\achievements','D:\Download\TNO-assets\09-showcase\assets','D:\Download\TNO-assets\09-showcase\styles'
```

- [ ] **Step 2: Write the catalogue entries for the selected source families.**

The catalogue must include at least these source families:

```json
{
  "focus_silicon_dreams": "03-content-art/goals/GNG/focus_GNG_silicon_dreams_at_pearl_river.dds",
  "silicon_years_idea": "03-content-art/ideas/national_spirits/Asia/GNG/idea_GNG_silicon_years.dds",
  "guangdong_event_art": "03-content-art/event_pictures/Asia/Guangdong/report_event_GNG_guangzhou.png",
  "guangdong_loading": "02-ui-textures/loadingscreens/load_guangdong_causeway_bay.dds",
  "silicon_achievement": "03-content-art/achievements/GNG_silicon_dreamers_achievement.dds",
  "guangdong_gui_defs": "01-design-sources/interface-defs/Guangdong/",
  "tno_fonts": "01-design-sources/fonts/"
}
```

Use paths that match the actual `TNO-assets` layout; if a family is stored elsewhere, record the actual path rather than inventing a normalized path.

- [ ] **Step 3: Document the visual mapping and conversion decisions.**

`guangdong-ui-map.md` must map source UI systems to showcase components: economy graph → telemetry chart, map GUI → motor topology panel, LegCo/progress radial → controller status ring, product/research cards → parameter cards, and Guangdong teal/orange semantics → connected/warning states.

- [ ] **Step 4: Write the conversion log with explicit source and destination paths.**

Record which files were copied without conversion, which were converted, which were intentionally represented with SVG/CSS, and the reason for each choice. A missing converter must be recorded as a limitation, not silently ignored.

- [ ] **Step 5: Verify the catalogue is parseable and all recorded source paths exist.**

Run:

```powershell
node -e "const fs=require('fs');const p='D:/Download/TNO-assets/00-catalog/asset-index.json';const x=JSON.parse(fs.readFileSync(p,'utf8'));for(const [k,v] of Object.entries(x.sources||{})){if(v.source && !fs.existsSync('D:/Download/TNO-assets/'+v.source)) throw new Error(k+': '+v.source)}console.log('catalog-ok',Object.keys(x.sources||{}).length)"
```

Expected: `catalog-ok <number>` with no exception.

---

### Task 2: Select and prepare web-ready assets without touching originals

**Files:**
- Create: selected files under `D:\Download\TNO-assets\08-web-ready\...`
- Modify: `D:\Download\TNO-assets\00-catalog\asset-index.json`
- Modify: `D:\Download\TNO-assets\00-catalog\conversion-log.md`

**Interfaces:**
- Produces only browser-readable assets: PNG, JPEG, SVG, WOFF/WOFF2/TTF where legally and technically available.
- The HTML must fall back to CSS/SVG if any optional asset is absent.

- [ ] **Step 1: Copy browser-readable PNG/JPEG assets into purpose directories.**

Prioritize one Guangzhou event image, one Guangdong loading image if already PNG/JPEG, the Silicon Dreams achievement, and the Silicon Dreams focus image if a readable raster version exists. Use explicit `Copy-Item -LiteralPath` commands and preserve stable semantic destination names such as `silicon-dreams-focus.png` and `guangzhou-event.png`.

- [ ] **Step 2: Convert only the selected `.dds` assets if a local converter is available.**

Check availability before use:

```powershell
Get-Command magick,convert,texconv,ffmpeg -ErrorAction SilentlyContinue | Select-Object Name,Source
```

Use the available converter with a single-file smoke test, then convert only the selected files. If no converter is installed, keep the source `.dds` out of the web-ready directory and record the fallback in `conversion-log.md`; the showcase must still function through SVG/CSS.

- [ ] **Step 3: Copy or convert the permitted font assets into `08-web-ready/fonts`.**

Prefer the existing open font files already present in `TNO-assets` or the project reference assets. Record exact font file names and use CSS `@font-face` only in the asset-enhanced page. The standalone page must use system fallbacks and not fail if fonts are missing.

- [ ] **Step 4: Add a tiny SVG asset layer for items that should remain resolution-independent.**

Create `08-web-ready/ui-chrome/tno-ui-symbols.svg` containing reusable symbols for power, warning, fault, encoder, current, voltage, and motor rotor. This is original vector UI chrome, not a copied binary asset.

- [ ] **Step 5: Verify each destination asset opens or has the expected file signature.**

Run a Node script that checks files exist and reports byte size; for PNG/JPEG verify the first bytes. Do not claim visual validity for a conversion that was not opened or decoded.

---

### Task 3: Implement the shared CSS and component visual language

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\styles\tno-ui.css`
- Create: `D:\Download\TNO-assets\09-showcase\styles\tno-ui.js`

**Interfaces:**
- Exposes CSS variables for the TNO palette, panel primitives, typography, CRT effects, badges, charts, maps, cards, controls, and motor UI.
- JavaScript binds `[data-tab]`, `[data-action]`, `[data-slider]`, `[data-live-state]`, and `[data-param]` without external libraries.

- [ ] **Step 1: Define the palette and layout tokens.**

Include the Guangdong source colors (`#006C72`, `#D27727`) and the existing project-compatible CRT colors (`#59C7C2`, `#D3C900`, `#FF8072`, `#3CB43C`, `#7447D1`) as semantic variables. Add deep blue-black backgrounds, 1px/2px frame tokens, spacing, and type scale.

- [ ] **Step 2: Implement the CRT surface treatment.**

Create reusable `.crt-shell`, `.crt-scanlines`, `.crt-noise`, and `.crt-vignette` layers using CSS gradients and pseudo-elements. Keep effects subtle enough that labels and values remain readable.

- [ ] **Step 3: Implement reusable TNO components.**

Implement `.tno-panel`, `.panel-title`, `.tno-button` states, `.status-chip`, `.meter`, `.segmented-bar`, `.event-card`, `.decision-card`, `.focus-node`, `.faction-badge`, `.graph`, `.map-board`, `.legend`, `.data-row`, `.terminal-log`, and `.tooltip-note`.

- [ ] **Step 4: Implement the motor-control components.**

Implement `.motor-shell`, `.motor-topbar`, `.motor-sidebar`, `.motor-workspace`, `.motor-param-grid`, `.parameter-row`, `.telemetry-card`, `.fault-log`, `.loop-status`, `.rotor-diagram`, and `.command-deck`. Use `V/A/W/Hz/RPM/%/°C` unit-driven colors and include text labels/icons for every warning/fault state.

- [ ] **Step 5: Implement small JavaScript interactions.**

The script must support:

```js
toggleTab(name)
setTelemetryState(element, state)
updateParameter(input)
resetParameter(button)
runCommand(command)
```

Buttons should update visible UI state without navigating away; parameter inputs should update the displayed value and mark the row as `EDITED`; live-state buttons should cycle `LIVE → STALE → UNKNOWN` for demonstration.

- [ ] **Step 6: Verify the stylesheet and script are syntactically loadable.**

Run:

```powershell
node --check 'D:\Download\TNO-assets\09-showcase\styles\tno-ui.js'
```

Expected: no output and exit code 0.

---

### Task 4: Build the asset-enhanced showcase HTML

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\tno-ui-showcase.html`

**Interfaces:**
- References `styles/tno-ui.css`, `styles/tno-ui.js`, and optional files under `../08-web-ready/` using relative URLs.
- Contains three major sections: `TNO / SILICON DREAMS`, `COMPONENT ATLAS`, and `G6618 MOTOR CONTROL TERMINAL`.

- [ ] **Step 1: Add the page shell, tab navigation, and live status header.**

Use accessible `<button>` elements with `data-tab` and `aria-selected`; default to the TNO overview tab. Include date, faction, connected state, and a small asset-source note.

- [ ] **Step 2: Create the TNO Guangdong overview.**

Build a three-column layout containing:

```text
left: regional controller, faction support, control nodes
center: stylized Pearl River map, Silicon Dreams focus chain, economy graph
right: GDP/debt meters, decisions, event preview, achievement panel
bottom: event ticker, decision deck, system log
```

Use an optional event image or focus image with a CSS/SVG fallback panel.

- [ ] **Step 3: Create the component atlas.**

Show palette swatches with hex values, typography roles, all button states, badges, cards, graph variants, progress bars, map nodes, status states, and a tooltip. Each component needs a short label describing the TNO design principle it demonstrates.

- [ ] **Step 4: Create the motor-control upper-computer screen at the end of the document.**

The motor screen must include:

```text
topbar: G6618 MOTOR CONTROL TERMINAL / USB-CAN / firmware / sampling
left rail: RUN, STOP, FAULT RESET, FOC state, rotor and bus summary
center: speed/current/torque plots, dq vector diagram, loop timing
right rail: current/velocity/power PID parameters, limits, apply/revert
bottom: ADC/PWM/encoder diagnostics, fault log, LIVE/STALE/UNKNOWN controls
```

Use plausible example values, but label the screen as a design prototype rather than live hardware data.

- [ ] **Step 5: Add responsive fallback styles and reduced-motion behavior.**

At widths below 1050px, collapse columns into a single flow. Respect `prefers-reduced-motion: reduce` by disabling scanline animation and pulsing glows.

---

### Task 5: Build the standalone no-extracted-assets page

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\tno-ui-showcase-standalone.html`

**Interfaces:**
- Must be a single HTML file with inline `<style>` and `<script>`.
- Must contain no `http://`, `https://`, `file://`, `.dds`, `.png`, `.jpg`, `.ttf`, `.woff`, or external stylesheet/script references.

- [ ] **Step 1: Inline the shared CSS and JavaScript.**

Copy the verified content from `styles/tno-ui.css` and `styles/tno-ui.js` into the standalone document, preserving the same class names and interactions.

- [ ] **Step 2: Replace optional raster assets with inline SVG/CSS equivalents.**

Use an SVG event-poster abstraction, a vector focus icon, a CSS map silhouette, and inline symbols. Keep the content structure identical to the asset-enhanced page so comparison is direct.

- [ ] **Step 3: Add a visible dependency label.**

Display `STANDALONE / NO EXTRACTED ASSETS` in the header so the user can distinguish the fallback page.

---

### Task 6: Validate both deliverables offline and review the visual result

**Files:**
- Create: `D:\Download\TNO-assets\09-showcase\validate-showcase.js`
- Modify if needed: both HTML files and shared styles/scripts

- [ ] **Step 1: Validate references and required sections with Node.**

The validator must load both HTML files as text and assert the presence of `Silicon Dreams`, `COMPONENT ATLAS`, `G6618 MOTOR CONTROL TERMINAL`, `LIVE`, `STALE`, `UNKNOWN`, `data-tab`, and the motor parameter labels. It must also validate that every relative `src`/`href` in the asset page points to an existing file.

- [ ] **Step 2: Validate standalone dependency isolation.**

Reject the standalone file if it contains external URLs, raster/font extensions, or external `<link>`/`<script src>` tags.

- [ ] **Step 3: Open both pages in a local browser or static server.**

Use a local static server if direct `file://` execution blocks local font/image loading. Inspect the page at desktop and narrow widths. Verify tab switching, button state changes, slider edits, parameter reset, command deck feedback, and telemetry state cycling.

- [ ] **Step 4: Record verification output.**

Append commands, exit codes, and any known limitations to `D:\Download\TNO-assets\00-catalog\conversion-log.md`. Do not claim a converted image is valid without a browser or decoder check.

- [ ] **Step 5: Run final directory listing and report exact deliverables.**

Report the two HTML paths, shared asset paths, catalogue paths, and any optional asset that fell back to SVG/CSS.
