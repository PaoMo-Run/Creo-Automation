# Creo Parametric Simulated Feature Mirroring & Model Tree Slimming Guide

This reference supplements [SKILL.md](../SKILL.md) with the complete dual-channel `op: "mirror"` workflow (`ProCmdFtMirror`), IntentManager multi-contour stability bounds, and `.inf` verification normalization.

---

## 1. Dual-Channel Mirror Dispatch (`is_feature_mirror_op`)

In `src_c/creo_auto_modeler.cpp`, `"op": "mirror"` routes automatically based on its fields:

- **Simulated Manual Feature / Group Mirror (`schedule_feature_mirror`)**: Triggered when the operation specifies `"features"` (array), `"feature"` (string), or `"references"` containing `{"feature": ...}`. Mirrors native Creo UI workflow (`pre-select features/groups -> Mirror tool -> pick datum plane -> confirm`).
- **Geometry Quilt / Curve / Whole-Part Mirror (`op_mirror`)**: Triggered when `"references"` contains only `quilt` / `curve` refs or is empty (whole-part mirror) via `ProElementAlloc` + `PRO_FEAT_GEN_MERGE`.

### Recommended JSON Syntax

```json
{
  "id": "mirror_side_bezels_r",
  "op": "mirror",
  "name_zh": "镜像右侧窗框护框与灯框",
  "plane": "RIGHT",
  "expand_groups": true,
  "features": [
    "swin_bezel_f_l",
    "swin_bezel_r_l",
    "side_louver_bezel_l",
    "side_marker_cages_l"
  ]
}
```

- `expand_groups` defaults to `true`: if `features` references a `feature_group` or an earlier `mirror` block ID, `ctx.group_member_ids[key]` is automatically expanded so all member features are pre-selected together.
- The resulting mirror ID (e.g. `"mirror_side_bezels_r"`) can be passed directly into downstream `set_appearance` / `set_texture` (`"scope": "features"`).

---

## 2. C++ Pro/TOOLKIT Implementation Details

### 2.1 Pre-Selection & Macro Sequence (`schedule_feature_mirror`)

1. **Resolve and pre-select features into `ProSelbuffer`** (**must happen before** loading `ProCmdFtMirror`):
   - Expand `ctx.group_member_ids` (if `expand_groups == true`) or look up `ctx.features[key].id`.
   - For each `fid`:
     `ProModelitemInit(mdl, fid, PRO_FEATURE, &mi)` -> `ProSelectionAlloc(nullptr, &mi, &fsel)` -> `ProSelbufferSelectionAdd(fsel)` -> free temporary `fsel`.
2. **Queue mirror datum plane**:
   - Call `resolve_plane_selection(ctx, op.at("plane"), &plane_sel)` and push to `macro_sel_queue` for `CreoMacroPickRef`.
3. **Snapshot pre-mirror feature IDs and launch dashboard macro**:
   - Call `prepare_macro_op(op, id_str, "mirror_feats")`, set `g_build.used_mirror_feats = true`, and load:
     ```text
     ~ Command `ProCmdFtMirror`;
      ~ Command `CreoMacroPickRef`;
      ~ Activate `main_dlg_cur` `maindashInst0.MainDashList` 0;
      ~ Activate `main_dlg_cur` `dashInst0.stdbtn_1`;
      ~ Command `CreoBuildResume`;
     ```

### 2.2 Callback Registration & Downstream Coloring

- Mirroring multiple source features produces a `PRO_FEAT_GROUP_HEAD` plus subordinate copy features.
- In `creo_build_resume_cb` (`pending_op == "mirror_feats"`):
  1. Diff active features against `pre_blend_feat_ids` into `mirror_created_ids`.
  2. Record `PRO_FEAT_GROUP_HEAD` (or first created feature) as `ctx.features[pending_id]`.
  3. Store all `mirror_created_ids` into `ctx.group_member_ids[pending_id]`.
- Downstream `collect_feature_live_surface_ids` scans both `ctx.features[key]` and `ctx.group_member_ids[key]` (further expanding `PRO_FEAT_PATTERN_HEAD` if present) and intersects with live solid body surfaces.

### 2.3 Final Force-Regeneration

- When `used_mirror_feats == true`, `finish_build_and_close` runs `ProSolidRegenerate(solid, PRO_REGEN_FORCE_REGEN)` before saving and exporting `.inf`.

---

## 3. Model Tree Slimming & Sketch Stability Bounds

1. **Single-Side Modeling + Color-Grouped Batch Mirroring**:
   - Build symmetric details on one side first, group by target color/finish, mirror each batch across `RIGHT`, then immediately apply `set_appearance` to the mirrored group ID.
   - Symmetric graphics (emblems, borders) can be mirrored; directional text/numbers must be sketched independently on both sides so characters do not invert.
2. **Multi-Contour `IntentManager` `<= 3` Limit**:
   - Under `ProSectionIntentManagerModeSet(sec, PRO_B_TRUE)`, putting **more than 3** collinear narrow rectangles along the same axis in a single sketch can trigger implicit collinear/equal-length over-constraints (`ProSectionAutodim st=-1 n_err=0`). Split 4–5 collinear rectangles into `2+2` or `3+2` features. Circular arrays and short centerline-symmetric `revolve` profiles are unaffected.
3. **Sloped Blend End-Cap Overlap**:
   - End-caps meeting sloped `solid_blend` walls must tuck slightly inward and extend into the solid wall by `>= 0.5–2 mm` so mirror symmetry does not create zero-thickness boundary failures (`FEAT_INACTIVE status=6`).

---

## 4. Verification `.inf` Normalization (`normalize_mirror_inf`)

When Creo 8.0 exports `PRO_MODEL_INFO` (`.inf`) during build, mirrored feature tables include `2 镜像项 已定义`; after saving `.prt.1` and reopening in an isolated verification session, Creo 8.0 omits that line and shifts rows `3/4/5 -> 2/3/4`:

```python
pat = re.compile(
    r"^[ \t]*2[ \t]+镜像项[ \t]+已定义[ \t]*\r?\n"
    r"([ \t]*)3([ \t]+镜像平面[^\r\n]*\r?\n)"
    r"([ \t]*)4([ \t]+不复制任何副本[^\r\n]*\r?\n)"
    r"([ \t]*)5([ \t]+完全从属副本[^\r\n]*\r?\n)",
    re.MULTILINE,
)
txt2, n = pat.subn(r"\g<1>2\g<2>\g<3>3\g<4>\g<5>4\g<6>", txt)
```

`build_cpp.py` applies `normalize_mirror_inf` only on hash mismatch so all non-mirror geometry/tree diffs remain strictly enforced.
