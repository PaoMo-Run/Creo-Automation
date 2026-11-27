---
name: creo-engineering-drawing
description: >-
  Creo Parametric (8.0 verified; 9-12 auto-adapted) C++17 Pro/TOOLKIT GB/T 2D part drawing
  automation & verification workflow. Covers GB/T 14689 A3/A4 borders & Chinese title block
  (ProDwgtable), first-angle planar cross-sections with 45-degree hatching, custom sketch
  broken views, 2:1 circular detail views, fit tolerances, GB/T 131-2006 roughness marks,
  chfntf + \x01n\x02 diameter encoding (zero missing-glyph boxes), 300-DPI monochrome PDF &
  AutoCAD DWG (AC1032) export, and isolated reopen sheet verification.
  Use when generating or debugging 2D GB/T Creo engineering drawings (.drw) via build_cpp.py
  and src_c/creo_drawing_engine.hpp.
---

# Creo GB/T 2D Engineering Drawing Automation Guide

This sub-skill pairs with `creo-parametric-automation` for generating associative native `.drw.1`, 300-DPI monochrome `.pdf`, and AutoCAD `.dwg` files via [`src_c/creo_drawing_engine.hpp`](../../../src_c/creo_drawing_engine.hpp) and [`build_cpp.py`](../../../build_cpp.py).

---

## 1. End-to-End Drawing & Verification Pipeline

1. **Source Part & Helper Geometry**:
   - Keep real machining features (undercuts, seal grooves, chamfers) on `.prt.1` without altering assembly-mating datums or center distances.
   - Add hidden `DWG_REF_PNTS` (`PRO_FEAT_DATUM_POINT`) when broken views or enlarged detail views need 3D anchors, and planar cross-sections via `xsections` (`ProXsecPlanarCreate`).
2. **CLI Direct-Open Staging (`build_cpp.py`)**:
   - Copies `<CREO_ROOT>\Common Files\creo_standards\templates_iso_mm\a3_drawing.drw` to `{drawing_name}.drw.1` inside `build_cpp/stage/`, derives `gb_mm.dtl` from `iso_mm.dtl` (`FIRST_ANGLE`, `chfntf`, `symbol_font LEGACY`, `broken_view_offset 5.0`), and writes `drawing_setup_file ./gb_mm.dtl` + `pdf_use_pentable no`.
   - Always launches `parametric.exe parametric.psf {drawing_name}.drw.1` via CLI (`cm_st=0 same_drw=1`).
3. **Clear Template -> Frame & Title Block -> Four-Pass Views -> Annotations -> Hide `DWG_REF_PNTS` -> Export**:
   - Clears pre-existing template views/tables/notes/entities, sets A3 `420x297 mm`, binds the solid part, draws a GB/T 14689-2008 binding frame (25 mm at left, 5 mm at top/right/bottom) and a 180 × 56 mm GB/T 10609.1-2008 title block with first-angle projection symbol. Optional `title_block.designer`, `title_block.date`, and `title_block.organization` strings populate the native Creo title tables. It then runs Pass 1a/1b/1c/2/3 view creation, adds centerlines, dimensions/tolerances, roughness symbols, and technical notes, hides `DWG_REF_PNTS` + saves `.prt.1` + saves `.drw.1`, then exports `ProPDFExport` and `Pro2dExport(PRO_DWG_FILE)`.
4. **Isolated Reopen & Visual Inspection**:
   - Pass `--render-pdf` to `build_cpp.py` (when optional `pymupdf` is installed) to render a 220-DPI PNG preview and verify zero overlaps/missing glyphs.
   - `build_cpp.py` copies `.drw.1` + updated `.prt.1` into `build_cpp/verify/`, reopens via CLI `{drawing_name}.drw.1` in `verify_drawing` mode, and asserts `DRW_REOPEN_VERIFIED=true` with zero trail warnings.

---

## 2. Critical Pro/TOOLKIT Drawing Invariants

### 2.1 Active CLI Drawing Context (`ProDrawingViewBrokenVisibleAreaSet` Crash Guard)
- Creating a new drawing in `CreoBuildExec` without CLI-opening `{drawing_name}.drw.1` leaves `ProMdlCurrentGet` returning `-8`, causing `ProDrawingViewBrokenVisibleAreaSet` to crash in `coreutils_sh`.
- Always pass `cli_open_name = f"{drw_name}.drw.1"` in both `drawing` build and `verify_drawing` reopen stages.

### 2.2 Sheet Coordinates & `ProDwgtable` Mixed Units
- Invert `ProDrawingSheetTrfGet` to compute `scale_mm_to_scn` (~`2.380952` on A3 landscape).
- Multiply views, `ProDtlentity`, `ProDtlnote`, text heights, and `ProDwgtabledataOriginSet` by `scale_mm_to_scn`.
- **Exception**: Pass raw millimeter arrays directly to `ProDwgtabledataColumnsSet` and `ProDwgtabledataRowsSet` under `PRODWGTABLESIZE_SCREEN`.

### 2.3 Four-Pass View Ordering (Do Not Reorder)
1. **Pass 1a (General & Projected views)**: Create `general` view with `scale = 0.0` then lock `1:1` via `ProDrawingScaleSet(..., 1.0)` (avoids `0.200` auto-shrink and redundant `"比例 1:1"` text); create all `projected` views before any broken view (`ProDrawingProjectedviewCreate` returns `-52` on broken parents).
2. **Pass 1b (2D Section + Arrows + Hatching)**: After arrow target views exist, call `ProDrawingView2DSectionSet(..., arrow_view)` once (calling after break returns `-2`), `ProDrawingViewRegenerate`, and 4-arg `ProXsecCompXhatchReplace` (`0`, fallback `-1`) for 45-degree 2.5 mm hatching.
3. **Pass 1c (Detail views BEFORE break)**: Create `ProDrawingViewDetailCreate` before Pass 2, and orient the parent view horizontally so the magnified end stays on the **left unshifted anchor segment**.
4. **Pass 2 (Parent broken view ONLY)**: Call `ProDrawingViewBrokenVisibleAreaSet` only on the parent view (never repeat on projected children, which inherit breaks automatically and would shift twice). Use `PRO_VIEW_BROKEN_LINE_SKETCH` (`±1.6 mm` wave spline, straight fallback) for slender shafts, do not overwrite `ProSelectionPoint3dSet` on `DWG_REF_PNTS`, and apply explicit `break_shift_mm = -(removed_len - offset)` for right-segment centerlines/roughness marks.
5. **Pass 3**: Final `ProDrawingViewMove` (`move_center_mm`).

### 2.4 Zero `□` Glyphs, Robust Diameters & Tolerances
- Use Creo's built-in GB/T stroke font **`chfntf`** (`default_font chfntf`, `default_annotation_font chfntf`, `symbol_font LEGACY`) and encode `Ø` as `\x01n\x02` cleanly across PDF & DWG.
- Avoid `[arc, arc]` auto-diameter boxes: use symmetric half-cylinder surfaces for sectioned diameters, and `[start, end]` arc endpoints (or `orient: "none"`) for unsectioned circular edges.
- `ProDimensionToleranceSet(&dim, up, -lo)` takes lower tolerance as a positive magnitude below nominal (`0 / -0.13` -> `0.0, +0.13`).
- Always save `.prt.1` after hiding `DWG_REF_PNTS` (`ProLayerDisplaystatusSave`) alongside `.drw.1`.

---

## 3. Acceptance Gates

1. `DIMS_SUMMARY fail=0`, `EXPORT_PDF st=0`, `EXPORT_DWG st=0` (`AC1032` DWG header).
2. `DRW_REOPEN_VERIFIED=true` (`regen_st=0`) in isolated `verify_drawing` reopen.
3. Zero `!%CE`, `!%CW`, `失序`, or `out of sequence` in build or verify `trail.txt.*`.
