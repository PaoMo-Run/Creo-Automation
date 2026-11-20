# Creo-Automation JSON Plan Schema Reference

Every modeling job is described by a single UTF-8 JSON file under `plans/*.json` (or any custom path passed to `python build_cpp.py --plan <path>`).

The C++17 Pro/TOOLKIT engine (`src_c/creo_auto_modeler.cpp` & `src_c/creo_plan_core.hpp`) uses **millimeter-Newton-second (`mmNs`)** coordinates via `mmns_part_solid_abs` / `mmns_asm_design_abs` templates unless overridden.

### Authoring Principles (Human-Like Model Tree)

- Keep the datum skeleton minimal (`RIGHT`, `TOP`, `FRONT` + key offset/angled planes and axes).
- Prefer single native features (`revolve`, `extrude`, `rotational_blend`, `helical_sweep`, `axis_pattern`) over dozens of discrete loft planes so engineers can inspect and redefine dimensions directly in Creo UI.
- Order operations naturally: base solid (`attach_body: true`) -> cuts/grooves -> `round` / `chamfer` -> `feature_group` / `axis_pattern` -> feature/surface-scoped `set_appearance`.
- Keep `rename_component_feature` as `false` (default) in assemblies to prevent `Note_` clutter in the model tree.

---

## 1. Top-Level Plan Fields

| Field | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `part_name` | `string` | *(required)* | Model stem name (`.prt` or `.asm`). |
| `doc_type` | `string` | `"part"` | `"part"` or `"assembly"`. |
| `template` | `string` | `"mmns_part_solid_abs"` / `"mmns_asm_design_abs"` | Creo starter template name resolved from `<CREO_ROOT>/Common Files/templates`. |
| `hide_datums_in_views` | `boolean` | `false` | Temporarily hide datum planes, axes, points, and CSYS when exporting `.jpg` screenshots. |
| `export_right_view` | `boolean` | `false` | Export `<part_name>_right.jpg` in addition to `default`, `front`, and `top`. |
| `output_subdir` | `string` | `part_name` | Optional relative folder under `output/`. |
| `components_plans` | `string[]` | `[]` | For `assembly` plans: list of child part plan paths built before assembling. |
| `components_dir_name` | `string` | `"components"` | Subfolder inside `output/<part_name>/` for built child component deliverables. |
| `textures` | `string[]` | `[]` | Extra texture bitmap files to stage into Creo working directory before `set_texture`. |
| `motors` | `object[]` | `[]` | Assembly-level Mechanism servo motors (`PRO_FEAT_PM_MOTOR`). |
| `mechanism_analysis` | `boolean` | `false` | Run and persist `AnalysisDefinition1` in Creo Mechanism mode after motors are created. |
| `operations` | `object[]` | *(required)* | Ordered list of modeling operations. |

---

## 2. Common Operation Properties

- `id` (`string`): Unique identifier referenced by downstream operations (`datum_axis`, `round`, `axis_pattern`, `sweep`, `set_appearance`, etc.).
- `op` (`string`): Operation type (listed in Section 3).
- `name_zh` / `name` (`string`): Optional custom feature name (`PRO_E_STD_FEATURE_NAME`). Ignored when `--default-names` is passed to `build_cpp.py`.
- `metric_tag` (`string`): Optional checkpoint tag to log solid volume, surface area, and bounding box to `build_report.txt`.

---

## 3. Supported Operations (`op`)

### 3.1 Datums

- **`datum_plane`**
  - `mode`: `"offset"` (default) or `"angle"`
  - `reference`: Plane name (`"FRONT"`, `"TOP"`, `"RIGHT"`), feature `id`, or `{"plane_at_z": z, "fallback": ...}`
  - `offset`: Offset distance in `mm` (when `mode == "offset"`)
  - `axis`, `angle`, `desired_normal` (`[nx, ny, nz]`): Used when `mode == "angle"`
  - `fit_radius`: Optional display radius
- **`datum_axis`**
  - `references`: Array of 2 plane references (e.g., `["TOP", "RIGHT"]`)
- **`datum_points`**
  - `reference`: CSYS name (default `"PRT_CSYS_DEF"`)
  - `points`: `[{"name": "PNT0", "position": [x, y, z]}, ...]`

### 3.2 Solid Primitives & Sketched Features

- **`extrude`**
  - `plane`, `orient_ref`: Sketch plane and horizontal/vertical orientation reference
  - `mode`: `"add"` (default) or `"cut"`
  - `attach_body`: `true` (default — attaches to Creo's `DefaultBody` to avoid orphan-body cut errors) or `false`
  - `depth_type`: `"blind"` (default), `"through_all"`, or `"symmetric"`
  - `side`: `"side_two"` (default, `+Z` on `FRONT`) or `"side_one"`
  - `depth`: Extrusion distance in `mm`
  - `verify_cut_removed`: `true` (default; fails build if a cut removes `<= 1e-6 mm³`)
  - Sketch spec inline (`entities_3d` / `entities_rz` / `entities_2d`, `constraints`, `dimensions`) or inside `"sketch": {...}`
- **`revolve`**
  - `plane`, `orient_ref`, `mode` (`"add"`/`"cut"`), `attach_body`, `angle` (default `360.0`), `verify_cut_removed`
  - Sketch must contain a `{"type": "centerline", ...}` entity serving as the axis of revolution.
- **`sketched_curve`**
  - `plane`, `orient_ref`, and sketch spec (`entities_3d` / `entities_rz` / `entities_2d`). Produces a `PRO_FEAT_CURVE` datum curve.
- **`sweep`**
  - `curve`: Feature `id` of a `sketched_curve` trajectory
  - `mode` (`"add"`/`"cut"`), `attach_body`, `verify_cut_removed`
  - `sketch`: Cross-section sketch (2D coordinates relative to trajectory start frame)

### 3.3 Edge Treatments & Patterns

- **`round`** / **`chamfer`**
  - `size` (aliases: `radius`, `d`): Constant radius (or `D x D` chamfer distance) in `mm`
  - `edge_rules`: Array of matching rules filtering active solid B-rep edges:
    - `feature`: Optional feature `id` (automatically intersected with live solid body edges)
    - `tol`: Tolerance in `mm` (default `0.01`)
    - `z_range`, `x_range`, `y_range`, `r_range`: `[min, max]` bounds checked at edge start, mid, and end points
    - `min_r`, `max_r`, `len_range`, `near`: `[x, y, z]`, `mid_near`: `[x, y, z]`, `endpoints_match`: `[[x1,y1,z1], [x2,y2,z2]]`
- **`feature_group`**
  - `members`: Ordered list of existing feature `id`s to group via `ProLocalGroupCreate`
- **`axis_pattern`**
  - `leader`: Single feature `id` or `feature_group` `id`
  - `axis`: Axis reference (`"A_1"` or `datum_axis` `id`)
  - `count`: Total instance count (including leader)
  - `increment_deg`: Angular step in degrees (defaults to `360.0 / count`)

### 3.4 Advanced Native Interactive Features (Dashboard Macro-Driven)

> Plans using `solid_blend`, `rotational_blend`, or `helical_sweep` automatically trigger `build_cpp.py` to launch Creo with an active template window.

- **`solid_blend`**
  - `sections`: Array of `>= 2` closed `sketched_curve` feature `id`s
  - `interpolation`: `"smooth"` (default) or `"straight"`
- **`rotational_blend`**
  - `plane`, `axis`, `mode` (`"add"`/`"cut"`), `interpolation` (`"smooth"`/`"straight"`), `closed` (`boolean`)
  - `sections`: Array of `>= 2` section objects:
    - Section 0: `angle_deg` (usually `0.0`), `entities_3d` or `entities_2d`, `constraints`, `dimensions`, `epsilon` (recommended `0.01` for far-axis small arcs)
    - Section `k >= 1`: `offset_deg` (incremental rotation from previous section) or cumulative `angle_deg`, plus `entities_3d`/`entities_2d`, `auto_start_vertex`, `start_vertex`, `blend_vertices`, `raw_section_coords`
- **`helical_sweep`**
  - `plane`, `pitch`, `mode` (`"add"`/`"cut"`), `handedness` (`"right"`/`"left"`), `orientation` (`"through_axis"`/`"normal"`/`"normal_to_trajectory"`)
  - `profile`: Sketch containing trajectory line/arc/spline + centerline
  - `section` (or `cross_section`): 2D profile centered near `[0, 0]` (automatically shifted to trajectory crosshair unless `raw_section_coords: true`)

### 3.5 Surfaces, Quilts & Mirroring

- **`surface_loft`**: `sections` (array of curve feature `id`s, `{"point": "<datum_points_id>"}`, or `{"curve_id": int}`), optional `guides`, `closed` (`boolean`)
- **`flat_fill`**: `curve` (`sketched_curve` `id`)
- **`quilt_merge`**: `quilts` (`string[]`) or `target` + `tool`
- **`solidify`**: `quilt`, `mode` (`"add"`/`"cut"`), `attach_body`, `side` (`"auto"`, `"side_one"`, `"side_two"`)
- **`mirror`**: `plane`, `references` (empty for whole-part mirror; or `quilt` / `curve_feat` references — never raw solid features), `keep_original` (`true` default)

### 3.6 Assemblies, Mechanism Joints & Servo Motors

- **`assemble_component`** (`doc_type == "assembly"` only)
  - `model`: Component stem name; `model_type`: `"part"` (default) or `"assembly"`
  - `placement`: `"fixed"`, `"default"`, `"csys"`, `"constraints"`, or Mechanism joint (`"pin"`, `"slider"`, `"cylindrical"`, `"bearing"`, `"ball"`, `"planar"`)
  - For Mechanism joints:
    - `set_name`, `set_flip` (`boolean`), `constraints` (`[{"type": "align"|"mate"|..., "asm_component": ..., "asm_ref": ..., "comp_ref": ..., "offset": ...}]`)
    - `jas`: `{"comp_zero_ref": ..., "asm_zero_ref": ..., "asm_zero_component": ..., "zero_offset": 0.0, "regen_value": 0.0, "min_limit": ..., "max_limit": ...}`
    - `additional_sets`: Array of extra joint sets on the same component (for closed kinematic loops)
    - `rename_component_feature`: Keep `false` (default) to avoid creating `Note_` annotation items in the Creo model tree.
- **Top-level `motors` & `mechanism_analysis`**:
  - Each motor: `{"name": "M1", "axis_name": "set_name.first_rot_axis", "motion_type": "rotational"|"translational", "flip_dir": false, "interp": "monotonic"|"linear"|"spline", "table": [[0.0, 0.0], [2.0, 45.0]]}`

### 3.7 Materials, Colors & Embedded Textures

- **`set_appearance`** / **`set_texture`**
  - `scope`: `"part"` (or `"model"`), `"features"`, `"surfaces"`, or `"component"`
  - `preset`: `"matte_paint"`, `"gunmetal"`, `"brushed_steel"`, `"heat_metal"`, `"rubber"`, `"white_tex_base"`
  - `rgb` / `highlight_rgb`: `[r, g, b]` in `0..1` or `0..255`, plus optional `ambient`, `diffuse`, `highlite`, `shininess`, `reflection`, `transparency`
  - `features`: Array of feature `id`s (expands `PRO_FEAT_PATTERN_HEAD` members and intersects with live solid surfaces)
  - `filter`: Surface filters (`surface_types`, `cyl_radius_range`, `cyl_axis_dir`, `plane_normal`, `signed_normal`, `x_range`, `y_range`, `z_range`, `center_r_range`, `bbox_inside`, `tol`)
  - For `set_texture`: `texture_file`, `projection` (`"cylindrical"` | `"planar"` | `"spherical"` | `"box"`), `csys_x`, `csys_y`, `csys_z`, `csys_origin`, `u_scale`, `v_scale`, `u_offset`, `v_offset`, `rotate_deg`, `world_tile_size`

---

## 4. Sketch Entities, Constraints & Dimensions

- **Coordinate Modes**:
  - `entities_3d`: Part 3D `[X, Y, Z]` automatically projected onto sketch 2D `(u, v)`.
  - `entities_rz`: Cylindrical `[R, Z]` automatically projected onto meridian sketch plane.
  - `entities_2d`: Direct sketch-plane `[u, v]`.
- **Entity Types**:
  - `line`, `centerline`: `start`, `end`, optional `construction`
  - `circle`: `center`, `radius`
  - `ellipse`: `center`, plus `radii_xyz` / `u_dir`+`rx`+`ry` / `x_radius`+`y_radius`
  - `arc`: 3D/RZ uses `center`, `start`, `end`, `mid`, `radius`; 2D uses `center`, `radius`, `start_deg`, `end_deg`, `dir` (`1` CCW / `-1` CW)
  - `polyline`: `points`, `closed`, `constrain_corners`
  - `spline`: `points`, `periodic` / `closed`
  - `b_spline`: `control_points`, `degree`, optional `knots`, optional `weights`
- **Constraints (`constraints`)**: `same_point` (`coincident`), `tangent`, `orthogonal` (`perpendicular`), `parallel`, `horizontal`, `vertical`, `equal_radii`, `equal_segments` (`equal_length`), `collinear`, `point_on`.
- **Dimensions (`dimensions`)**: `line` (`length`), `radius`, `diameter`, `pnt_pnt` (`distance`), `horiz`, `vert`, `line_line`, `angle`, placed at `place_3d` / `place_rz` / `place_2d` with optional driving `value`.
