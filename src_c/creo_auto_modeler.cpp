/*
 * Creo Parametric 8.0 JSON-Plan-Driven C++17 Pro/TOOLKIT Modeler & Verifier.
 *
 * Reads modeling plans from `creobuild_plan.json` in the working directory
 * and mode flags (`build:zh`, `build:default`, `verify`) from `creobuild_mode.txt`.
 *
 * Features supported:
 *   - datum_plane (offset / angled through axis)
 *   - datum_axis (through two planes or geometry)
 *   - datum_points (cartesian offset from csys)
 *   - extrude (solid add/cut, blind/through_all/symmetric, 3D/2D constrained sketches)
 *   - revolve (solid add/cut, full/partial angle, lines/arcs/splines/b_splines + constraints)
 *   - round (constant radius on geometrically matched edges)
 *   - chamfer (D x D / D1 x D2 on geometrically matched edges)
 *   - feature_group (ProLocalGroupCreate)
 *   - axis_pattern (single feature or group pattern around axis)
 *   - sketched_curve (PRO_FEAT_CURVE on plane or planar Z face, inc. b_spline)
 *   - sweep (constant section solid add/cut along sketched_curve)
 *   - solid_blend (native ProCmdFtPBlend solid loft over closed sketched_curve sections)
 *   - rotational_blend (native ProCmdFtRBlend multi-angle internal sketched blend add/cut)
 *   - helical_sweep (native ProCmdFtHelicalSweep internal profile+pitch+section add/cut)
 *   - surface_loft / flat_fill / quilt_merge / solidify / mirror (quilt/curve/whole-part)
 */

#include "creo_plan_core.hpp"
#include <ProKinDrag.h>
#include <ProMotorFeat.h>
#include <ProSelbuffer.h>
#include <ProUIDialog.h>
#include <chrono>

using namespace creobuild;

static int g_has_run = 0;
static ProUITimerID g_step_timer = nullptr;
static ProError g_timer_create_st = PRO_TK_GENERAL_ERROR;

namespace {

ProFeature create_with_opts(
    ProSolid solid,
    ProElement tree,
    FILE *log,
    const char *tag
) {
    ProModelitem mdl_item;
    ProSelection mdl_sel = nullptr;
    ProFeatureCreateOptions *opts = nullptr;
    ProErrorlist errs{};
    ProFeature feat{};

    ProMdlToModelitem(static_cast<ProMdl>(solid), &mdl_item);
    ProError st = ProSelectionAlloc(nullptr, &mdl_item, &mdl_sel);
    if (st != PRO_TK_NO_ERROR) {
        log_st(log, tag, st);
        throw st;
    }

    ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, reinterpret_cast<ProArray *>(&opts));
    opts[0] = PRO_FEAT_CR_NO_OPTS;
    st = ProFeatureWithoptionsCreate(mdl_sel, tree, opts, PRO_REGEN_NO_FLAGS, &feat, &errs);
    ProArrayFree(reinterpret_cast<ProArray *>(&opts));
    ProSelectionFree(&mdl_sel);
    if (st != PRO_TK_NO_ERROR) {
        log_errorlist(log, tag, &errs);
        log_st(log, tag, st);
        throw st;
    }
    return feat;
}

ProError read_datum_plane_geom(PlanContext &ctx, ProFeature *feat, double out_origin[3], double out_normal[3]) {
    ProSelection fsel = nullptr;
    ProError st = find_feat_geom_sel(feat, PRO_SURFACE, &fsel);
    if (st != PRO_TK_NO_ERROR || !fsel) return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    ProModelitem item{};
    st = ProSelectionModelitemGet(fsel, &item);
    ProSelectionFree(&fsel);
    if (st != PRO_TK_NO_ERROR) return st;
    ProSurface srf = nullptr;
    st = ProSurfaceInit(ctx.mdl, item.id, &srf);
    if (st != PRO_TK_NO_ERROR || !srf) return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    ProGeomitemdata *gdata = nullptr;
    st = ProSurfaceDataGet(srf, &gdata);
    if (st != PRO_TK_NO_ERROR || !gdata) return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    ProSurfacedata *sd = PRO_SURF_DATA(gdata);
    if (!sd || sd->type != PRO_SRF_PLANE) {
        ProGeomitemdataFree(&gdata);
        return PRO_TK_BAD_CONTEXT;
    }
    for (int i = 0; i < 3; ++i) {
        if (out_origin) out_origin[i] = sd->srf_shape.plane.origin[i];
        if (out_normal) out_normal[i] = sd->srf_shape.plane.e3[i];
    }
    ProGeomitemdataFree(&gdata);
    return PRO_TK_NO_ERROR;
}

ProFeature op_datum_plane(PlanContext &ctx, const Json &op) {
    ProSelection ref_sel = nullptr, axis_sel = nullptr;
    std::wstring wname = resolve_feat_name(ctx, op);
    std::string mode = op.value("mode", "offset");
    std::string id_str = op.value("id", "datum_plane");

    if (resolve_plane_selection(ctx, op.at("reference"), &ref_sel) != PRO_TK_NO_ERROR || !ref_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    auto build_plane_tree = [&](double ang_val, bool flip_dir) -> ProElement {
        ProElement t = nullptr, cs = nullptr, c1 = nullptr;
        elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &t);
        elem_add_int(t, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM);
        if (!wname.empty()) elem_add_wstr(t, PRO_E_STD_FEATURE_NAME, wname.c_str());
        elem_add_compound(t, PRO_E_DTMPLN_CONSTRAINTS, &cs);
        elem_add_compound(cs, PRO_E_DTMPLN_CONSTRAINT, &c1);
        if (mode == "angle") {
            elem_add_int(c1, PRO_E_DTMPLN_CONSTR_TYPE, PRO_DTMPLN_ANG);
            elem_add_ref(c1, PRO_E_DTMPLN_CONSTR_REF, ref_sel);
            elem_add_double(c1, PRO_E_DTMPLN_CONSTR_REF_ANGLE, ang_val);
            ProElement c2 = nullptr;
            elem_add_compound(cs, PRO_E_DTMPLN_CONSTRAINT, &c2);
            elem_add_int(c2, PRO_E_DTMPLN_CONSTR_TYPE, PRO_DTMPLN_THRU);
            elem_add_ref(c2, PRO_E_DTMPLN_CONSTR_REF, axis_sel);
        } else {
            double offset = op.value("offset", 0.0);
            elem_add_int(c1, PRO_E_DTMPLN_CONSTR_TYPE, PRO_DTMPLN_OFFS);
            elem_add_ref(c1, PRO_E_DTMPLN_CONSTR_REF, ref_sel);
            elem_add_double(c1, PRO_E_DTMPLN_CONSTR_REF_OFFSET, offset);
        }
        if (flip_dir) elem_add_int(t, PRO_E_DTMPLN_FLIP_DIR, PRO_DTMPLN_FLIP_DIR_YES);
        if (op.contains("fit_radius") && op["fit_radius"].is_number()) {
            ProElement fit = nullptr;
            elem_add_compound(t, PRO_E_DTMPLN_FIT, &fit);
            elem_add_int(fit, PRO_E_DTMPLN_FIT_TYPE, PRO_DTMPLN_FIT_RADIUS);
            elem_add_double(fit, PRO_E_DTMPLN_FIT_DTM_RAD, op["fit_radius"].get<double>());
        }
        return t;
    };

    double angle = 0.0;
    if (mode == "angle") {
        angle = op.at("angle").get<double>();
        if (resolve_axis_selection(ctx, op.at("axis"), &axis_sel) != PRO_TK_NO_ERROR || !axis_sel) {
            ProSelectionFree(&ref_sel);
            throw PRO_TK_E_NOT_FOUND;
        }
    }

    ProElement root = build_plane_tree(angle, false);
    ProFeature f{};
    try {
        f = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
        ProElementFree(&root);
    } catch (...) {
        ProElementFree(&root);
        ProSelectionFree(&ref_sel);
        if (axis_sel) ProSelectionFree(&axis_sel);
        throw;
    }

    if (mode == "angle" && op.contains("desired_normal")) {
        const Json &dnj = op["desired_normal"];
        double dn[3]{dnj.at(0).get<double>(), dnj.at(1).get<double>(), dnj.at(2).get<double>()};
        double dlen = std::sqrt(dn[0] * dn[0] + dn[1] * dn[1] + dn[2] * dn[2]);
        if (dlen > 1e-9) {
            dn[0] /= dlen; dn[1] /= dlen; dn[2] /= dlen;
            double orig[3]{}, n[3]{};
            if (read_datum_plane_geom(ctx, &f, orig, n) == PRO_TK_NO_ERROR) {
                double dot = n[0] * dn[0] + n[1] * dn[1] + n[2] * dn[2];
                double used_angle = angle;
                auto redef_plane = [&](double ang_val, bool flip_dir) {
                    ProElement t2 = build_plane_tree(ang_val, flip_dir);
                    ProFeatureCreateOptions *ropts = nullptr;
                    ProErrorlist errs{};
                    ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, reinterpret_cast<ProArray *>(&ropts));
                    ropts[0] = PRO_FEAT_CR_NO_OPTS;
                    ProError rst = ProFeatureWithoptionsRedefine(nullptr, &f, t2, ropts, PRO_REGEN_NO_FLAGS, &errs);
                    ProArrayFree(reinterpret_cast<ProArray *>(&ropts));
                    ProElementFree(&t2);
                    if (rst != PRO_TK_NO_ERROR) {
                        log_errorlist(ctx.log, id_str.c_str(), &errs);
                        throw rst;
                    }
                    if (read_datum_plane_geom(ctx, &f, orig, n) == PRO_TK_NO_ERROR) {
                        dot = n[0] * dn[0] + n[1] * dn[1] + n[2] * dn[2];
                    }
                };
                try {
                    if (std::fabs(dot) < 0.995 && std::fabs(angle) > 1e-4) {
                        used_angle = -angle;
                        redef_plane(used_angle, false);
                    }
                    if (dot < -0.995) {
                        redef_plane(used_angle, true);
                    }
                } catch (...) {
                    ProSelectionFree(&ref_sel);
                    if (axis_sel) ProSelectionFree(&axis_sel);
                    throw;
                }
                log_printf(ctx.log, "DTMPLN_ANG[%s] ang=%.3f orig=(%.3f,%.3f,%.3f) n=(%.4f,%.4f,%.4f) dot=%.5f\n",
                           id_str.c_str(), used_angle, orig[0], orig[1], orig[2], n[0], n[1], n[2], dot);
            }
        }
    }

    ProSelectionFree(&ref_sel);
    if (axis_sel) ProSelectionFree(&axis_sel);
    return f;
}

ProFeature op_datum_axis(PlanContext &ctx, const Json &op) {
    ProElement root = nullptr, constrs = nullptr;
    std::wstring wname = resolve_feat_name(ctx, op);

    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_AXIS);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());
    elem_add_compound(root, PRO_E_DTMAXIS_CONSTRAINTS, &constrs);

    for (const Json &r : op.at("references")) {
        ProSelection sel = nullptr;
        if (resolve_plane_selection(ctx, r, &sel) != PRO_TK_NO_ERROR || !sel) {
            ProElementFree(&root);
            throw PRO_TK_E_NOT_FOUND;
        }
        ProElement c = nullptr;
        elem_add_compound(constrs, PRO_E_DTMAXIS_CONSTRAINT, &c);
        elem_add_int(c, PRO_E_DTMAXIS_CONSTR_TYPE, PRO_DTMAXIS_CONSTR_TYPE_THRU);
        elem_add_ref(c, PRO_E_DTMAXIS_CONSTR_REF, sel);
        ProSelectionFree(&sel);
    }

    try {
        ProFeature f = create_with_opts(ctx.solid, root, ctx.log, "datum_axis");
        ProElementFree(&root);
        return f;
    } catch (...) {
        ProElementFree(&root);
        throw;
    }
}

ProFeature op_extrude(PlanContext &ctx, const Json &op) {
    ProSelection plane_sel = nullptr, orient_sel = nullptr;
    if (resolve_plane_selection(ctx, op.at("plane"), &plane_sel) != PRO_TK_NO_ERROR || !plane_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }
    if (resolve_plane_selection(ctx, op.at("orient_ref"), &orient_sel) != PRO_TK_NO_ERROR || !orient_sel) {
        ProSelectionFree(&plane_sel);
        throw PRO_TK_E_NOT_FOUND;
    }

    std::string mode = op.value("mode", "add");
    bool is_cut = (mode == "cut");
    bool attach_body = op.value("attach_body", true);
    std::string depth_type = op.value("depth_type", "blind");
    double depth = op.value("depth", 10.0);
    std::string side_str = op.value("side", "side_two");
    int std_dir = (side_str == "side_one") ? PRO_EXT_CR_IN_SIDE_ONE : PRO_EXT_CR_IN_SIDE_TWO;
    std::wstring wname = resolve_feat_name(ctx, op);

    double vol_before = 0.0;
    Pro3dPnt box_tmp[2]{};
    if (is_cut) {
        measure_solid_volume(ctx.solid, &vol_before, nullptr, box_tmp);
    }

    ProElement tree = nullptr, d_comp = nullptr, d_from = nullptr, d_to = nullptr, sec_e = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &tree);
    elem_add_int(tree, PRO_E_FEATURE_FORM, PRO_EXTRUDE);
    if (!wname.empty()) elem_add_wstr(tree, PRO_E_STD_FEATURE_NAME, wname.c_str());
    elem_add_int(tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_EXT_FEAT_TYPE_SOLID);
    elem_add_int(tree, PRO_E_REMOVE_MATERIAL, is_cut ? PRO_EXT_MATERIAL_REMOVE : PRO_EXT_MATERIAL_ADD);
    add_body_elements(tree, ctx.solid, attach_body ? 1 : 0);
    elem_add_int(tree, PRO_E_FEAT_FORM_IS_THIN, PRO_EXT_FEAT_FORM_NO_THIN);

    elem_add_compound(tree, PRO_E_STD_EXT_DEPTH, &d_comp);
    elem_add_compound(d_comp, PRO_E_EXT_DEPTH_FROM, &d_from);
    if (depth_type == "through_all" && side_str == "symmetric") {
        elem_add_int(d_from, PRO_E_EXT_DEPTH_FROM_TYPE, PRO_EXT_DEPTH_FROM_ALL);
    } else {
        elem_add_int(d_from, PRO_E_EXT_DEPTH_FROM_TYPE, PRO_EXT_DEPTH_FROM_NONE);
    }

    elem_add_compound(d_comp, PRO_E_EXT_DEPTH_TO, &d_to);
    if (depth_type == "through_all") {
        elem_add_int(d_to, PRO_E_EXT_DEPTH_TO_TYPE, PRO_EXT_DEPTH_TO_ALL);
    } else if (depth_type == "symmetric") {
        elem_add_int(d_to, PRO_E_EXT_DEPTH_TO_TYPE, PRO_EXT_DEPTH_SYMMETRIC);
        elem_add_double(d_to, PRO_E_EXT_DEPTH_TO_VALUE, depth);
    } else {
        elem_add_int(d_to, PRO_E_EXT_DEPTH_TO_TYPE, PRO_EXT_DEPTH_TO_BLIND);
        elem_add_double(d_to, PRO_E_EXT_DEPTH_TO_VALUE, depth);
    }

    elem_add_compound(tree, PRO_E_STD_SECTION, &sec_e);
    add_section_setup_plane(sec_e, plane_sel, orient_sel);

    ProElemId sk_path[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProFeature feat{};
    std::string id_str = op.value("id", "extrude");
    const Json &sk_spec = op.contains("sketch") ? op["sketch"] : op;

    ProError st = build_sketched_feature(
        ctx,
        tree,
        sk_path,
        2,
        (depth_type == "symmetric") ? 0 : 1,
        std_dir,
        0,
        sk_spec,
        id_str.c_str(),
        &feat);
    ProElementFree(&tree);
    ProSelectionFree(&plane_sel);
    ProSelectionFree(&orient_sel);
    if (st != PRO_TK_NO_ERROR) throw st;

    if (is_cut && op.value("verify_cut_removed", true)) {
        double vol_after = 0.0;
        if (measure_solid_volume(ctx.solid, &vol_after, nullptr, box_tmp) == PRO_TK_NO_ERROR) {
            double removed = vol_before - vol_after;
            log_printf(ctx.log, "CUT_VOL_REMOVED[%s]=%.6f\n", id_str.c_str(), removed);
            if (removed <= 1e-6) {
                log_printf(ctx.log, "ERROR_CUT_NO_MATERIAL[%s]\n", id_str.c_str());
                throw PRO_TK_GENERAL_ERROR;
            }
        }
    }
    return feat;
}

ProFeature op_revolve(PlanContext &ctx, const Json &op) {
    ProSelection plane_sel = nullptr, orient_sel = nullptr;
    if (resolve_plane_selection(ctx, op.at("plane"), &plane_sel) != PRO_TK_NO_ERROR || !plane_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }
    if (resolve_plane_selection(ctx, op.at("orient_ref"), &orient_sel) != PRO_TK_NO_ERROR || !orient_sel) {
        ProSelectionFree(&plane_sel);
        throw PRO_TK_E_NOT_FOUND;
    }

    std::string mode = op.value("mode", "add");
    bool is_cut = (mode == "cut");
    bool attach_body = op.value("attach_body", true);
    double angle = op.value("angle", 360.0);
    std::wstring wname = resolve_feat_name(ctx, op);

    double vol_before = 0.0;
    Pro3dPnt box_tmp[2]{};
    if (is_cut) {
        measure_solid_volume(ctx.solid, &vol_before, nullptr, box_tmp);
    }

    ProElement tree = nullptr, ang_c = nullptr, a_from = nullptr, a_to = nullptr, sec_e = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &tree);
    elem_add_int(tree, PRO_E_FEATURE_FORM, PRO_REVOLVE);
    if (!wname.empty()) elem_add_wstr(tree, PRO_E_STD_FEATURE_NAME, wname.c_str());
    elem_add_int(tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_REV_FEAT_TYPE_SOLID);
    elem_add_int(tree, PRO_E_REMOVE_MATERIAL, is_cut ? PRO_REV_MATERIAL_REMOVE : PRO_REV_MATERIAL_ADD);
    add_body_elements(tree, ctx.solid, attach_body ? 1 : 0);
    elem_add_int(tree, PRO_E_FEAT_FORM_IS_THIN, PRO_REV_FEAT_FORM_NO_THIN);

    elem_add_compound(tree, PRO_E_REV_ANGLE, &ang_c);
    elem_add_compound(ang_c, PRO_E_REV_ANGLE_FROM, &a_from);
    elem_add_int(a_from, PRO_E_REV_ANGLE_FROM_TYPE, PRO_REV_ANG_FROM_NONE);
    elem_add_compound(ang_c, PRO_E_REV_ANGLE_TO, &a_to);
    elem_add_int(a_to, PRO_E_REV_ANGLE_TO_TYPE, PRO_REV_ANG_TO_ANGLE);
    elem_add_double(a_to, PRO_E_REV_ANGLE_TO_VAL, angle);

    elem_add_compound(tree, PRO_E_STD_SECTION, &sec_e);
    add_section_setup_plane(sec_e, plane_sel, orient_sel);

    ProElemId sk_path[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProFeature feat{};
    std::string id_str = op.value("id", "revolve");
    const Json &sk_spec = op.contains("sketch") ? op["sketch"] : op;

    ProError st = build_sketched_feature(
        ctx, tree, sk_path, 2, 0, 0, 1, sk_spec, id_str.c_str(), &feat);
    ProElementFree(&tree);
    ProSelectionFree(&plane_sel);
    ProSelectionFree(&orient_sel);
    if (st != PRO_TK_NO_ERROR) throw st;

    if (is_cut && op.value("verify_cut_removed", true)) {
        double vol_after = 0.0;
        if (measure_solid_volume(ctx.solid, &vol_after, nullptr, box_tmp) == PRO_TK_NO_ERROR) {
            double removed = vol_before - vol_after;
            log_printf(ctx.log, "CUT_VOL_REMOVED[%s]=%.6f\n", id_str.c_str(), removed);
            if (removed <= 1e-6) {
                log_printf(ctx.log, "ERROR_CUT_NO_MATERIAL[%s]\n", id_str.c_str());
                throw PRO_TK_GENERAL_ERROR;
            }
        }
    }
    return feat;
}

ProFeature op_round_or_chamfer(PlanContext &ctx, const Json &op, bool is_chamfer) {
    std::string id_str = op.value("id", is_chamfer ? "chamfer" : "round");
    std::vector<ProSelection> edge_sels;
    ProError st = collect_matching_edges(ctx, op.at("edge_rules"), edge_sels, id_str.c_str());
    if (st != PRO_TK_NO_ERROR || edge_sels.empty()) {
        for (ProSelection s : edge_sels) ProSelectionFree(&s);
        throw (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    }

    ProCollection crv_coll = nullptr;
    st = ProCrvcollectionAlloc(&crv_coll);
    if (st != PRO_TK_NO_ERROR) {
        for (ProSelection s : edge_sels) ProSelectionFree(&s);
        throw st;
    }

    for (ProSelection s : edge_sels) {
        ProReference ref = nullptr;
        ProCrvcollinstr instr = nullptr;
        if (ProSelectionToReference(s, &ref) == PRO_TK_NO_ERROR &&
            ProCrvcollinstrAlloc(PRO_CURVCOLL_ADD_ONE_INSTR, &instr) == PRO_TK_NO_ERROR) {
            ProCrvcollinstrReferenceAdd(instr, ref);
            ProCrvcollectionInstructionAdd(crv_coll, instr);
            ProCrvcollinstrFree(instr);
        }
        if (ref) ProReferenceFree(ref);
        ProSelectionFree(&s);
    }

    double size_val = op.value("size", op.value("radius", op.value("d", 1.0)));
    std::wstring wname = resolve_feat_name(ctx, op);

    ProElement root = nullptr, sets = nullptr, set_e = nullptr, refs = nullptr, coll_e = nullptr;
    ProElement radii = nullptr, radius_e = nullptr, leg1 = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, is_chamfer ? PRO_FEAT_CHAMFER : PRO_FEAT_ROUND);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());

    elem_add_compound(root, PRO_E_RNDCH_SETS, &sets);
    elem_add_compound(sets, PRO_E_RNDCH_SET, &set_e);
    if (is_chamfer) {
        elem_add_int(set_e, PRO_E_RNDCH_DIMENSIONAL_SCHEMA, PRO_CHM_D_X_D);
        elem_add_int(set_e, PRO_E_RNDCH_CHAMFER_SHAPE, PRO_CHM_TANGENT_LEGS);
    } else {
        ProElement conic = nullptr, spine = nullptr;
        elem_add_int(set_e, PRO_E_RNDCH_SHAPE_OPTIONS, PRO_ROUND_TYPE_CONSTANT);
        elem_add_compound(set_e, PRO_E_RNDCH_COMPOUND_CONIC, &conic);
        elem_add_int(conic, PRO_E_RNDCH_CONIC_TYPE, PRO_ROUND_CONIC_DISABLE);
        elem_add_compound(set_e, PRO_E_RNDCH_COMPOUND_SPINE, &spine);
        elem_add_int(spine, PRO_E_RNDCH_BALL_SPINE, PRO_ROUND_ROLLING_BALL);
    }

    elem_add_compound(set_e, PRO_E_RNDCH_REFERENCES, &refs);
    elem_add_int(refs, PRO_E_RNDCH_REFERENCE_TYPE, PRO_ROUND_REF_EDGE);
    elem_add_compound(refs, PRO_E_STD_CURVE_COLLECTION_APPL, &coll_e);
    ProElementCollectionSet(coll_e, crv_coll);
    ProCollectionFree(&crv_coll);

    elem_add_compound(set_e, PRO_E_RNDCH_RADII, &radii);
    elem_add_compound(radii, PRO_E_RNDCH_RADIUS, &radius_e);
    elem_add_compound(radius_e, PRO_E_RNDCH_LEG1, &leg1);
    elem_add_int(leg1, PRO_E_RNDCH_LEG_TYPE, PRO_ROUND_RADIUS_TYPE_VALUE);
    elem_add_double(leg1, PRO_E_RNDCH_LEG_VALUE, size_val);

    elem_add_int(root, PRO_E_RNDCH_ATTACH_TYPE, PRO_ROUND_ATTACHED);
    ProElement trans = nullptr;
    elem_add_compound(root, PRO_E_RNDCH_TRANSITIONS, &trans);

    try {
        ProFeature f = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
        ProElementFree(&root);
        return f;
    } catch (...) {
        ProElementFree(&root);
        throw;
    }
}

ProFeature op_feature_group(PlanContext &ctx, const Json &op) {
    const Json &members = op.at("members");
    int n = static_cast<int>(members.size());
    if (n <= 0) throw PRO_TK_BAD_INPUTS;

    int *ids = nullptr;
    ProArrayAlloc(n, sizeof(int), 1, reinterpret_cast<ProArray *>(&ids));
    for (int i = 0; i < n; ++i) {
        std::string mkey = members[i].get<std::string>();
        auto it = ctx.features.find(mkey);
        if (it == ctx.features.end()) {
            ProArrayFree(reinterpret_cast<ProArray *>(&ids));
            throw PRO_TK_E_NOT_FOUND;
        }
        ids[i] = it->second.id;
    }

    ProName gp_name{};
    std::wstring wname = resolve_feat_name(ctx, op);
    if (!wname.empty()) {
        wcsncpy_s(gp_name, PRO_NAME_SIZE, wname.c_str(), _TRUNCATE);
    }

    ProGroup grp{};
    ProError st = ProLocalGroupCreate(ctx.solid, ids, n, wname.empty() ? nullptr : gp_name, &grp);
    ProArrayFree(reinterpret_cast<ProArray *>(&ids));
    if (st != PRO_TK_NO_ERROR) {
        log_st(ctx.log, "ProLocalGroupCreate", st);
        throw st;
    }
    ProFeature head{};
    ProFeatureInit(ctx.solid, grp.id, &head);
    return head;
}

ProFeature op_axis_pattern(PlanContext &ctx, const Json &op) {
    std::string leader_key = op.at("leader").get<std::string>();
    auto lit = ctx.features.find(leader_key);
    if (lit == ctx.features.end()) throw PRO_TK_E_NOT_FOUND;
    ProFeature leader = lit->second;

    ProFeattype ltype = PRO_FEAT_FIRST_FEAT;
    ProFeatureTypeGet(&leader, &ltype);
    ProPatternClass pclass = (ltype == PRO_FEAT_GROUP_HEAD) ? PRO_GROUP_PATTERN : PRO_FEAT_PATTERN;

    ProSelection axis_sel = nullptr;
    if (resolve_axis_selection(ctx, op.at("axis"), &axis_sel) != PRO_TK_NO_ERROR || !axis_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    int count = op.at("count").get<int>();
    double inc_deg = op.value("increment_deg", 360.0 / static_cast<double>(count));
    std::wstring wname = resolve_feat_name(ctx, op);

    ProElement root = nullptr, axis_c = nullptr;
    elem_add_compound(nullptr, PRO_E_PATTERN_ROOT, &root);
    elem_add_int(root, PRO_E_GENPAT_TYPE, PRO_GENPAT_AXIS_DRIVEN);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());

    elem_add_compound(root, PRO_E_GENPAT_AXIS, &axis_c);
    elem_add_ref(axis_c, PRO_E_GENPAT_AXIS_REF, axis_sel);
    ProSelectionFree(&axis_sel);
    elem_add_double(axis_c, PRO_E_GENPAT_AXIS1_INC, std::fabs(inc_deg));
    elem_add_int(axis_c, PRO_E_AXIS_PAT_DIR1_FLIP, inc_deg < 0.0 ? 0 : 1);
    elem_add_int(axis_c, PRO_E_GENPAT_DIM_FIRST_DIR_NUM_INST, count);
    elem_add_double(axis_c, PRO_E_GENPAT_AXIS2_INC, 0.0);
    elem_add_int(axis_c, PRO_E_AXIS_PAT_DIR2_FLIP, 0);
    elem_add_int(axis_c, PRO_E_GENPAT_DIM_SECOND_DIR_NUM_INST, 1);
    elem_add_int(root, PRO_E_GENPAT_REGEN_METHOD, PRO_PAT_GENERAL);

    ProError st = ProPatternCreate(&leader, pclass, root);
    ProElementFree(&root);
    if (st != PRO_TK_NO_ERROR) {
        log_st(ctx.log, "ProPatternCreate", st);
        throw st;
    }

    ProPattern pat{};
    ProFeature head = leader;
    if (ProFeaturePatternGet(&leader, pclass, &pat) == PRO_TK_NO_ERROR) {
        ProPatternHeaderGet(&pat, &head);
    }
    return head;
}

ProFeature op_sketched_curve(PlanContext &ctx, const Json &op) {
    ProSelection plane_sel = nullptr, orient_sel = nullptr;
    if (resolve_plane_selection(ctx, op.at("plane"), &plane_sel) != PRO_TK_NO_ERROR || !plane_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }
    if (resolve_plane_selection(ctx, op.at("orient_ref"), &orient_sel) != PRO_TK_NO_ERROR || !orient_sel) {
        ProSelectionFree(&plane_sel);
        throw PRO_TK_E_NOT_FOUND;
    }

    std::wstring wname = resolve_feat_name(ctx, op);
    ProElement tree = nullptr, sec_e = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &tree);
    elem_add_int(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CURVE);
    elem_add_int(tree, PRO_E_CURVE_TYPE, PRO_CURVE_TYPE_SKETCHED);
    if (!wname.empty()) elem_add_wstr(tree, PRO_E_STD_FEATURE_NAME, wname.c_str());

    elem_add_compound(tree, PRO_E_STD_SECTION, &sec_e);
    add_section_setup_plane(sec_e, plane_sel, orient_sel);

    ProElemId sk_path[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProFeature feat{};
    std::string id_str = op.value("id", "sketched_curve");
    const Json &sk_spec = op.contains("sketch") ? op["sketch"] : op;

    ProError st = build_sketched_feature(
        ctx, tree, sk_path, 2, 0, 0, 0, sk_spec, id_str.c_str(), &feat);
    ProElementFree(&tree);
    ProSelectionFree(&plane_sel);
    ProSelectionFree(&orient_sel);
    if (st != PRO_TK_NO_ERROR) throw st;
    return feat;
}

ProFeature op_sweep(PlanContext &ctx, const Json &op) {
    std::string crv_key = op.at("curve").get<std::string>();
    auto cit = ctx.features.find(crv_key);
    if (cit == ctx.features.end()) throw PRO_TK_E_NOT_FOUND;

    ProSelection curve_sel = nullptr;
    if (find_feat_geom_sel(&cit->second, PRO_CURVE, &curve_sel) != PRO_TK_NO_ERROR || !curve_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    ProCollection crv_coll = nullptr;
    ProCrvcollinstr instr = nullptr;
    ProReference crv_ref = nullptr;
    ProError st = ProCrvcollectionAlloc(&crv_coll);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrAlloc(PRO_CURVCOLL_CURVE_INSTR, &instr);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrAttributeSet(instr, PRO_CURVCOLL_NO_ATTR);
    if (st == PRO_TK_NO_ERROR) st = ProSelectionToReference(curve_sel, &crv_ref);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrReferenceAdd(instr, crv_ref);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollectionInstructionAdd(crv_coll, instr);
    if (instr) ProCrvcollinstrFree(instr);
    if (crv_ref) ProReferenceFree(crv_ref);
    ProSelectionFree(&curve_sel);
    if (st != PRO_TK_NO_ERROR) {
        if (crv_coll) ProCollectionFree(&crv_coll);
        throw st;
    }

    std::string mode = op.value("mode", "add");
    bool is_cut = (mode == "cut");
    bool attach_body = op.value("attach_body", true);
    std::wstring wname = resolve_feat_name(ctx, op);

    double vol_before = 0.0;
    Pro3dPnt box_tmp[2]{};
    if (is_cut) {
        measure_solid_volume(ctx.solid, &vol_before, nullptr, box_tmp);
    }

    ProElement tree = nullptr, fcomp = nullptr, ftraj = nullptr, otraj = nullptr, ssel = nullptr;
    ProElement coll_elem = nullptr, fsetup = nullptr, pcomp = nullptr, ssec = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &tree);
    elem_add_int(tree, PRO_E_FEATURE_FORM, PRO_SWEEP);
    elem_add_int(tree, PRO_E_SWEEP_TYPE, PRO_SWEEP_TYPE_MULTI_TRAJ);
    if (!wname.empty()) elem_add_wstr(tree, PRO_E_STD_FEATURE_NAME, wname.c_str());

    elem_add_compound(tree, PRO_E_SWEEP_FRAME_COMP, &fcomp);
    elem_add_compound(fcomp, PRO_E_FRM_OPT_TRAJ, &ftraj);
    elem_add_compound(ftraj, PRO_E_OPT_TRAJ, &otraj);
    elem_add_int(otraj, PRO_E_STD_SEC_METHOD, PRO_SEC_SELECT);
    elem_add_compound(otraj, PRO_E_STD_SEC_SELECT, &ssel);
    elem_add_compound(ssel, PRO_E_STD_CURVE_COLLECTION_APPL, &coll_elem);
    ProElementCollectionSet(coll_elem, crv_coll);
    ProCollectionFree(&crv_coll);

    elem_add_compound(fcomp, PRO_E_FRAME_SETUP, &fsetup);
    elem_add_int(fsetup, PRO_E_FRM_NORMAL, PRO_FRAME_NORM_ORIGIN);
    elem_add_int(fsetup, PRO_E_FRM_ORIENT, PRO_FRAME_MIN);
    elem_add_int(fsetup, PRO_E_FRM_USER_X, PRO_FRAME_DEFAULT_START_X);

    elem_add_compound(tree, PRO_E_SWEEP_PROF_COMP, &pcomp);
    elem_add_int(pcomp, PRO_E_SWP_SEC_TYPE, PRO_SWEEP_CONST_SECTION);
    elem_add_compound(pcomp, PRO_E_SWEEP_SECTION, &ssec);

    elem_add_int(tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_SWEEP_FEAT_TYPE_SOLID);
    elem_add_int(tree, PRO_E_REMOVE_MATERIAL, is_cut ? PRO_SWEEP_MATERIAL_REMOVE : PRO_SWEEP_MATERIAL_ADD);
    elem_add_int(tree, PRO_E_FEAT_FORM_IS_THIN, PRO_SWEEP_FEAT_FORM_NO_THIN);
    add_body_elements(tree, ctx.solid, attach_body ? 1 : 0);

    ProElemId sk_path[3] = {PRO_E_SWEEP_PROF_COMP, PRO_E_SWEEP_SECTION, PRO_E_SKETCHER};
    ProFeature feat{};
    std::string id_str = op.value("id", "sweep");
    const Json &sk_spec = op.contains("sketch") ? op["sketch"] : op;

    st = build_sketched_feature(
        ctx, tree, sk_path, 3, 0, 0, 0, sk_spec, id_str.c_str(), &feat);
    ProElementFree(&tree);
    if (st != PRO_TK_NO_ERROR) throw st;

    if (is_cut && op.value("verify_cut_removed", true)) {
        double vol_after = 0.0;
        if (measure_solid_volume(ctx.solid, &vol_after, nullptr, box_tmp) == PRO_TK_NO_ERROR) {
            double removed = vol_before - vol_after;
            log_printf(ctx.log, "CUT_VOL_REMOVED[%s]=%.6f\n", id_str.c_str(), removed);
            if (removed <= 1e-6) {
                log_printf(ctx.log, "ERROR_CUT_NO_MATERIAL[%s]\n", id_str.c_str());
                throw PRO_TK_GENERAL_ERROR;
            }
        }
    }
    return feat;
}

#ifndef PRO_E_DTM_SRF_TYPE_BND_BLEND
#define PRO_E_DTM_SRF_TYPE_BND_BLEND 11
#endif

int record_feature_quilt_id(PlanContext &ctx, const std::string &id_str, ProFeature *feat) {
    GeomIdListCtx qlist;
    ProFeatureGeomitemVisit(feat, PRO_QUILT, visit_geom_id_action, nullptr, &qlist);
    int qid = (qlist.ids.size() == 1) ? qlist.ids[0] : -1;
    if (qid < 0) {
        GeomIdListCtx slist;
        ProFeatureGeomitemVisit(feat, PRO_SURFACE, visit_geom_id_action, nullptr, &slist);
        for (int sid : slist.ids) {
            ProSurface srf = nullptr;
            ProQuilt q = nullptr;
            if (ProSurfaceInit(ctx.mdl, sid, &srf) == PRO_TK_NO_ERROR &&
                ProSurfaceQuiltGet(ctx.solid, srf, &q) == PRO_TK_NO_ERROR && q) {
                ProQuiltIdGet(q, &qid);
                break;
            }
        }
    }
    if (qid >= 0) ctx.quilt_ids[id_str] = qid;
    return qid;
}

ProFeature op_datum_points(PlanContext &ctx, const Json &op) {
    ProSelection csys_sel = nullptr;
    if (resolve_csys_selection(ctx, op.value("reference", Json("PRT_CSYS_DEF")), &csys_sel) != PRO_TK_NO_ERROR ||
        !csys_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    std::wstring wname = resolve_feat_name(ctx, op);
    ProElement root = nullptr, pts_arr = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_POINT);
    elem_add_int(root, PRO_E_DPOINT_TYPE, PRO_DPOINT_TYPE_OFFSET_CSYS);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());
    elem_add_int(root, PRO_E_DPOINT_OFST_CSYS_TYPE, PRO_DTMPNT_OFFCSYS_CARTESIAN);
    elem_add_ref(root, PRO_E_DPOINT_OFST_CSYS_REF, csys_sel);
    ProSelectionFree(&csys_sel);
    elem_add_int(root, PRO_E_DPOINT_OFST_CSYS_WITH_DIMS, PRO_B_TRUE);
    elem_add_compound(root, PRO_E_DPOINT_OFST_CSYS_PNTS_ARRAY, &pts_arr);

    const Json &points = op.at("points");
    for (size_t i = 0; i < points.size(); ++i) {
        const Json &pt = points[i];
        std::wstring pname = utf8_to_w(pt.value("name", "PNT" + std::to_string(i)));
        const Json &pos = pt.at("position");
        ProElement p_e = nullptr;
        elem_add_compound(pts_arr, PRO_E_DPOINT_OFST_CSYS_PNT, &p_e);
        elem_add_wstr(p_e, PRO_E_DPOINT_OFST_CSYS_PNT_NAME, pname.c_str());
        elem_add_double(p_e, PRO_E_DPOINT_OFST_CSYS_DIR1_VAL, pos.at(0).get<double>());
        elem_add_double(p_e, PRO_E_DPOINT_OFST_CSYS_DIR2_VAL, pos.at(1).get<double>());
        elem_add_double(p_e, PRO_E_DPOINT_OFST_CSYS_DIR3_VAL, pos.at(2).get<double>());
    }

    try {
        ProFeature f = create_with_opts(ctx.solid, root, ctx.log, op.value("id", "datum_points").c_str());
        ProElementFree(&root);
        return f;
    } catch (...) {
        ProElementFree(&root);
        throw;
    }
}

ProError build_loft_chain_collection(PlanContext &ctx, const Json &ref, ProCollection *out_coll) {
    if (ref.is_object() && ref.contains("point")) {
        std::string pkey = ref["point"].get<std::string>();
        auto pit = ctx.features.find(pkey);
        if (pit == ctx.features.end()) return PRO_TK_E_NOT_FOUND;
        GeomIdListCtx plist;
        ProFeatureGeomitemVisit(&pit->second, PRO_POINT, visit_geom_id_action, nullptr, &plist);
        if (plist.ids.empty()) return PRO_TK_E_NOT_FOUND;
        log_printf(ctx.log, "  LOFT_SEC point=%s id=%d\n", pkey.c_str(), plist.ids[0]);
        return build_point_collection(ctx.mdl, plist.ids[0], out_coll);
    }
    if (ref.is_object() && ref.contains("curve_id")) {
        return build_curve_collection_from_ids(ctx.mdl, {ref["curve_id"].get<int>()}, out_coll, PRO_CURVCOLL_ADD_ONE_INSTR);
    }
    if (ref.is_string()) {
        std::string fkey = ref.get<std::string>();
        auto fit = ctx.features.find(fkey);
        if (fit == ctx.features.end()) return PRO_TK_E_NOT_FOUND;
        std::vector<int> cids;
        ProError st = collect_feat_noncomposite_curves(ctx.solid, &fit->second, cids);
        if (st != PRO_TK_NO_ERROR) return st;
        log_printf(ctx.log, "  LOFT_SEC feat=%s curves=%zu first_id=%d\n", fkey.c_str(), cids.size(), cids[0]);
        return build_curve_collection_from_ids(ctx.mdl, cids, out_coll, PRO_CURVCOLL_ADD_ONE_INSTR);
    }
    return PRO_TK_BAD_INPUTS;
}

ProFeature op_surface_loft(PlanContext &ctx, const Json &op) {
    std::string id_str = op.value("id", "surface_loft");
    std::wstring wname = resolve_feat_name(ctx, op);

    ProElement root = nullptr, crvs = nullptr, dir1 = nullptr, holders1 = nullptr;
    ProElement dir2 = nullptr, holders2 = nullptr, apx_dir = nullptr, apx_holders = nullptr;
    ProElement tan_arr = nullptr, ctrl_pts = nullptr, cp1 = nullptr, cp1_sets = nullptr;
    ProElement cp2 = nullptr, cp2_sets = nullptr, adv = nullptr, apx_p = nullptr;

    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_SURF);
    elem_add_int(root, PRO_E_FEATURE_FORM, PRO_USE_SURFS);
    elem_add_int(root, PRO_E_DTM_SRF_TYPE, PRO_E_DTM_SRF_TYPE_BND_BLEND);

    elem_add_compound(root, PRO_E_BNBLN_CURVES, &crvs);
    elem_add_compound(crvs, PRO_E_BNBLN_1DIR, &dir1);
    elem_add_compound(dir1, PRO_E_BNBLN_DIR_CURVES, &holders1);

    auto add_dir_items = [&](ProElement dir_holders, const Json &items) {
        for (const Json &item : items) {
            ProCollection coll = nullptr;
            ProError cst = build_loft_chain_collection(ctx, item, &coll);
            if (cst != PRO_TK_NO_ERROR || !coll) {
                if (coll) ProCollectionFree(&coll);
                throw (cst == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : cst;
            }
            ProElement h = nullptr, c_e = nullptr;
            elem_add_compound(dir_holders, PRO_E_STD_CHAIN_HOLDER, &h);
            elem_add_compound(h, PRO_E_STD_CURVE_COLLECTION_APPL, &c_e);
            ProElementCollectionSet(c_e, coll);
            ProCollectionFree(&coll);
        }
    };

    try {
        add_dir_items(holders1, op.at("sections"));
        elem_add_int(dir1, PRO_E_BBL_ADD_CLOSED, op.value("closed", false) ? PRO_B_TRUE : PRO_B_FALSE);

        bool has_guides = op.contains("guides") && op["guides"].is_array() && !op["guides"].empty();
        elem_add_compound(crvs, PRO_E_BNBLN_2DIR, &dir2);
        elem_add_compound(dir2, PRO_E_BNBLN_DIR_CURVES, &holders2);
        if (has_guides) {
            add_dir_items(holders2, op["guides"]);
        }
        elem_add_int(dir2, PRO_E_BBL_ADD_CLOSED, PRO_B_FALSE);

        elem_add_compound(crvs, PRO_E_APPROX_DIR, &apx_dir);
        elem_add_compound(apx_dir, PRO_E_BNBLN_DIR_CURVES, &apx_holders);
        elem_add_int(apx_dir, PRO_E_BBL_ADD_CLOSED, PRO_B_FALSE);

        elem_add_compound(root, PRO_E_BNBLN_TAN_COND, &tan_arr);
        for (int b = 0; b < 4; ++b) {
            ProElement bndry = nullptr, conn = nullptr, cmpnd = nullptr, rsurfs = nullptr, rsurf = nullptr;
            elem_add_compound(tan_arr, PRO_E_BNBLN_TNC_BNDRY, &bndry);
            int ttype = (b < 2 || has_guides) ? 1 : 0;
            elem_add_int(bndry, PRO_E_BNBLN_TAN_COND_TYPE, ttype);
            elem_add_compound(bndry, PRO_E_BNBLN_CONNECT_SURF_CHAIN, &conn);
            elem_add_compound(conn, PRO_E_SURF_CHAIN_CMPND, &cmpnd);
            elem_add_int(cmpnd, PRO_E_SURF_CHAIN_METHOD, 1);
            elem_add_compound(cmpnd, PRO_E_SURF_CHAIN_REF_SURFS, &rsurfs);
            elem_add_compound(rsurfs, PRO_E_SURF_CHAIN_SURF, &rsurf);
            elem_add_int(bndry, PRO_E_BNBLN_ADV_VIA_CRV, 0);
            elem_add_int(bndry, PRO_E_BNBLN_TAN_FLD_TYPE, -1);
            elem_add_double(bndry, PRO_E_BNBLN_STRETCH, 0.0);
        }

        elem_add_compound(root, PRO_E_CTRL_PTS, &ctrl_pts);
        elem_add_compound(ctrl_pts, PRO_E_CTRL_PTS_1DIR, &cp1);
        elem_add_compound(cp1, PRO_E_CTRL_PTS_SETS, &cp1_sets);
        elem_add_compound(ctrl_pts, PRO_E_CTRL_PTS_2DIR, &cp2);
        elem_add_compound(cp2, PRO_E_CTRL_PTS_SETS, &cp2_sets);
        elem_add_int(ctrl_pts, PRO_E_BLEND_TYPE, 0);

        elem_add_compound(root, PRO_E_BNBLN_ADVANCED, &adv);
        elem_add_int(adv, PRO_E_BNBLN_ADV_INN_EDGS1, 0);
        elem_add_int(adv, PRO_E_BNBLN_ADV_INN_EDGS2, 0);
        elem_add_int(adv, PRO_E_BNBLN_ADV_GHST_2DIR, 0);
        elem_add_int(adv, PRO_E_BBL_VARI_RECALC, 0);

        elem_add_compound(root, PRO_E_APPROX_PARAMETERS, &apx_p);
        elem_add_double(apx_p, PRO_E_APPROX_SMOOTHNESS, 0.0);
        elem_add_int(apx_p, PRO_E_APPROX_NUM_U_PATCH, 0);
        elem_add_int(apx_p, PRO_E_APPROX_NUM_V_PATCH, 0);

        if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());
        elem_add_int(root, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_FEAT_DATUM_SURF);
        elem_add_int(root, PRO_E_BBL_HDS_CONTROL, 1);
        ProElement body_e = nullptr;
        elem_add_compound(root, PRO_E_BODY, &body_e);
        elem_add_int(body_e, PRO_E_BODY_USE, PRO_BODY_USE_NEW);
    } catch (...) {
        ProElementFree(&root);
        throw;
    }

    ProFeature feat{};
    try {
        feat = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
        ProElementFree(&root);
    } catch (ProError first_err) {
        ProElementFree(&root);
        log_printf(ctx.log, "LOFT_FULL_TREE_FAIL[%s] st=%d, trying XML route\n", id_str.c_str(), static_cast<int>(first_err));
        // Diagnostic: test minimal XML load via ProElemtreeFromXMLCreate
        ProPath mname{};
        ProMdlNameGet(ctx.mdl, mname);
        std::string mdl_str = w2utf8(mname) + ".prt";
        std::ostringstream xml;
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<PRO_E_FEATURE_TREE type=\"compound\">\n"
            << "  <PRO_E_FEATURE_TYPE type=\"int\">PRO_FEAT_DATUM_SURF</PRO_E_FEATURE_TYPE>\n"
            << "  <PRO_E_FEATURE_FORM type=\"int\">PRO_USE_SURFS</PRO_E_FEATURE_FORM>\n"
            << "  <PRO_E_DTM_SRF_TYPE type=\"int\">11</PRO_E_DTM_SRF_TYPE>\n"
            << "  <PRO_E_BNBLN_CURVES type=\"compound\">\n"
            << "    <PRO_E_BNBLN_1DIR type=\"compound\">\n"
            << "      <PRO_E_BNBLN_DIR_CURVES type=\"array\">\n";
        for (const Json &item : op.at("sections")) {
            std::vector<int> cids;
            if (item.is_string()) {
                auto fit = ctx.features.find(item.get<std::string>());
                if (fit != ctx.features.end()) collect_feat_noncomposite_curves(ctx.solid, &fit->second, cids);
            }
            xml << "        <PRO_E_STD_CHAIN_HOLDER type=\"compound\">\n"
                << "          <PRO_E_STD_CURVE_COLLECTION_APPL type=\"collection\">\n"
                << "            <PRO_XML_COLLECTION type=\"curve\">\n"
                << "              <PRO_XML_COLLECTION_INSTRUCTIONS type=\"array\">\n";
            for (int cid : cids) {
                xml << "                <PRO_XML_COLLECTION_INSTRUCTION type=\"compound\">\n"
                    << "                  <PRO_XML_COLLECTION_INSTRUCTION_TYPE type=\"int\">101</PRO_XML_COLLECTION_INSTRUCTION_TYPE>\n"
                    << "                  <PRO_XML_CRVCOLL_REFS type=\"array\">\n"
                    << "                    <PRO_XML_CRVCOLL_REF type=\"selection\">\n"
                    << "                      <PRO_XML_REFERENCE type=\"reference\">\n"
                    << "                        <PRO_XML_REFERENCE_OWNER type=\"owner\">" << mdl_str << "</PRO_XML_REFERENCE_OWNER>\n"
                    << "                        <PRO_XML_REFERENCE_ID type=\"id\">" << cid << "</PRO_XML_REFERENCE_ID>\n"
                    << "                        <PRO_XML_REFERENCE_TYPE type=\"protype\">PRO_CURVE</PRO_XML_REFERENCE_TYPE>\n"
                    << "                      </PRO_XML_REFERENCE>\n"
                    << "                    </PRO_XML_CRVCOLL_REF>\n"
                    << "                  </PRO_XML_CRVCOLL_REFS>\n"
                    << "                </PRO_XML_COLLECTION_INSTRUCTION>\n";
            }
            xml << "              </PRO_XML_COLLECTION_INSTRUCTIONS>\n"
                << "            </PRO_XML_COLLECTION>\n"
                << "          </PRO_E_STD_CURVE_COLLECTION_APPL>\n"
                << "        </PRO_E_STD_CHAIN_HOLDER>\n";
        }
        xml << "      </PRO_E_BNBLN_DIR_CURVES>\n"
            << "      <PRO_E_BBL_ADD_CLOSED type=\"int\">0</PRO_E_BBL_ADD_CLOSED>\n"
            << "    </PRO_E_BNBLN_1DIR>\n"
            << "    <PRO_E_BNBLN_2DIR type=\"compound\"><PRO_E_BNBLN_DIR_CURVES type=\"array\"></PRO_E_BNBLN_DIR_CURVES><PRO_E_BBL_ADD_CLOSED type=\"int\">0</PRO_E_BBL_ADD_CLOSED></PRO_E_BNBLN_2DIR>\n"
            << "    <PRO_E_APPROX_DIR type=\"compound\"><PRO_E_BNBLN_DIR_CURVES type=\"array\"></PRO_E_BNBLN_DIR_CURVES><PRO_E_BBL_ADD_CLOSED type=\"int\">0</PRO_E_BBL_ADD_CLOSED></PRO_E_APPROX_DIR>\n"
            << "    <PRO_E_BBL_APPENDIX type=\"selection\"></PRO_E_BBL_APPENDIX>\n"
            << "  </PRO_E_BNBLN_CURVES>\n"
            << "  <PRO_E_EXT_SURF_CUT_SOLID_TYPE type=\"int\">942</PRO_E_EXT_SURF_CUT_SOLID_TYPE>\n"
            << "</PRO_E_FEATURE_TREE>\n";
        std::string xml_file = "loft_" + id_str + ".xml";
        {
            std::ofstream xout(xml_file, std::ios::binary);
            xout << xml.str();
        }
        ProPath xw{};
        wcsncpy_s(xw, PRO_PATH_SIZE, utf8_to_w(xml_file).c_str(), _TRUNCATE);
        ProElement xroot = nullptr;
        ProXMLErrorlist *xerrs = nullptr;
        ProError xst = ProElemtreeFromXMLCreate(xw, &xroot, &xerrs);
        if (xerrs) {
            int xn = 0;
            ProArraySizeGet(reinterpret_cast<ProArray>(xerrs), &xn);
            for (int xi = 0; xi < xn; ++xi) {
                log_printf(ctx.log, "XML_ERR[%d] type=%d line=%d col=%d msg=%s\n",
                           xi, static_cast<int>(xerrs[xi].error_type),
                           xerrs[xi].line_number, xerrs[xi].column_number,
                           w2utf8(xerrs[xi].message).c_str());
            }
            ProXmlerrorlistProarrayFree(&xerrs);
        }
        if (xst != PRO_TK_NO_ERROR || !xroot) {
            log_st(ctx.log, "ProElemtreeFromXMLCreate", xst);
            throw (xst == PRO_TK_NO_ERROR) ? PRO_TK_GENERAL_ERROR : xst;
        }
        try {
            feat = create_with_opts(ctx.solid, xroot, ctx.log, (id_str + "_xml").c_str());
            ProElementFree(&xroot);
        } catch (...) {
            ProElementFree(&xroot);
            throw;
        }
    }

    ProFeatStatus fstat = PRO_FEAT_INVALID;
    ProFeatureStatusGet(&feat, &fstat);
    log_printf(ctx.log, "LOFT_STATUS[%s]=%d\n", id_str.c_str(), static_cast<int>(fstat));
    if (fstat != PRO_FEAT_ACTIVE) {
        throw PRO_TK_GENERAL_ERROR;
    }

    int qid = record_feature_quilt_id(ctx, id_str, &feat);
    double area = 0.0, qvol = 0.0;
    bool is_closed = false;
    if (qid >= 0) {
        eval_quilt_area(ctx.solid, qid, &area);
        ProQuilt q = nullptr;
        if (ProQuiltInit(ctx.solid, qid, &q) == PRO_TK_NO_ERROR && q) {
            is_closed = (ProQuiltVolumeEval(q, &qvol) == PRO_TK_NO_ERROR);
        }
    }
    log_printf(ctx.log, "LOFT_QUILT[%s] qid=%d area=%.4f closed=%s\n",
               id_str.c_str(), qid, area, is_closed ? "true" : "false");
    return feat;
}

ProFeature op_flat_fill(PlanContext &ctx, const Json &op) {
    std::string id_str = op.value("id", "flat_fill");
    std::string sk_key = op.at("curve").get<std::string>();
    auto sit = ctx.features.find(sk_key);
    if (sit == ctx.features.end()) throw PRO_TK_E_NOT_FOUND;

    ProSelection sk_sel = nullptr;
    if (ProSelectionAlloc(nullptr, reinterpret_cast<ProModelitem *>(&sit->second), &sk_sel) != PRO_TK_NO_ERROR ||
        !sk_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    std::wstring wname = resolve_feat_name(ctx, op);
    ProElement root = nullptr, sec_e = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_SURF);
    elem_add_int(root, PRO_E_FEATURE_FORM, PRO_FLAT);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());
    elem_add_compound(root, PRO_E_STD_SECTION, &sec_e);
    elem_add_ref(sec_e, PRO_E_SEC_USE_SKETCH, sk_sel);
    ProSelectionFree(&sk_sel);

    try {
        ProFeature f = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
        ProElementFree(&root);
        int qid = record_feature_quilt_id(ctx, id_str, &f);
        log_printf(ctx.log, "FLAT_QUILT[%s] qid=%d\n", id_str.c_str(), qid);
        return f;
    } catch (...) {
        ProElementFree(&root);
        throw;
    }
}

ProFeature op_quilt_merge_pair(
    PlanContext &ctx,
    const std::string &id_str,
    const std::wstring &wname,
    int qid1,
    int qid2,
    int *out_qid
) {
    ProModelitem it1{}, it2{};
    ProSelection s1 = nullptr, s2 = nullptr;
    if (ProModelitemInit(ctx.mdl, qid1, PRO_QUILT, &it1) != PRO_TK_NO_ERROR ||
        ProSelectionAlloc(nullptr, &it1, &s1) != PRO_TK_NO_ERROR) {
        throw PRO_TK_E_NOT_FOUND;
    }
    if (ProModelitemInit(ctx.mdl, qid2, PRO_QUILT, &it2) != PRO_TK_NO_ERROR ||
        ProSelectionAlloc(nullptr, &it2, &s2) != PRO_TK_NO_ERROR) {
        ProSelectionFree(&s1);
        throw PRO_TK_E_NOT_FOUND;
    }

    double area1_before = 0.0;
    eval_quilt_area(ctx.solid, qid1, &area1_before);

    ProElement root = nullptr, arr = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_QUILT);
    elem_add_int(root, PRO_E_SRF_MRG_TYPE, PRO_SRF_MRG_JOIN);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());
    elem_add_compound(root, PRO_E_SRF_MRG_QUILT_ARR, &arr);

    ProElement *members = nullptr;
    ProArrayAlloc(2, sizeof(ProElement), 1, reinterpret_cast<ProArray *>(&members));
    for (int i = 0; i < 2; ++i) {
        elem_add_compound(nullptr, PRO_E_SRF_MRG_QUILT_CMPD, &members[i]);
        elem_add_ref(members[i], PRO_E_SRF_MRG_QUILT_REF, (i == 0) ? s1 : s2);
        elem_add_int(members[i], PRO_E_SRF_MRG_QUILT_SIDE, PRO_SRF_MRG_QLT_SIDE_1);
    }
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);

    ProElempath apath = nullptr;
    ProElempathItem pitem{PRO_ELEM_PATH_ITEM_TYPE_ID, {PRO_E_SRF_MRG_QUILT_ARR}};
    ProElempathAlloc(&apath);
    ProElempathDataSet(apath, &pitem, 1);
    ProElementArraySet(root, apath, members);
    ProElempathFree(&apath);
    ProArrayFree(reinterpret_cast<ProArray *>(&members));

    ProFeature f{};
    try {
        f = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
        ProElementFree(&root);
    } catch (...) {
        ProElementFree(&root);
        throw;
    }

    int res_qid = record_feature_quilt_id(ctx, id_str, &f);
    if (res_qid < 0) {
        double area1_after = 0.0;
        eval_quilt_area(ctx.solid, qid1, &area1_after);
        if (area1_after > area1_before + 1e-6) {
            res_qid = qid1;
            ctx.quilt_ids[id_str] = res_qid;
        }
    }
    if (res_qid < 0) {
        log_printf(ctx.log, "ERROR_QUILT_MERGE_NO_RESULT[%s]\n", id_str.c_str());
        throw PRO_TK_GENERAL_ERROR;
    }
    if (out_qid) *out_qid = res_qid;
    return f;
}

ProFeature op_quilt_merge(PlanContext &ctx, const Json &op) {
    std::string id_str = op.value("id", "quilt_merge");
    std::wstring wname = resolve_feat_name(ctx, op);

    std::vector<int> qids;
    if (op.contains("quilts") && op["quilts"].is_array()) {
        for (const Json &qr : op["quilts"]) {
            int qid = -1;
            if (resolve_quilt_id(ctx, qr, &qid) != PRO_TK_NO_ERROR || qid < 0) throw PRO_TK_E_NOT_FOUND;
            qids.push_back(qid);
        }
    } else {
        int q1 = -1, q2 = -1;
        if (resolve_quilt_id(ctx, op.at("target"), &q1) != PRO_TK_NO_ERROR || q1 < 0) throw PRO_TK_E_NOT_FOUND;
        if (resolve_quilt_id(ctx, op.at("tool"), &q2) != PRO_TK_NO_ERROR || q2 < 0) throw PRO_TK_E_NOT_FOUND;
        qids = {q1, q2};
    }
    if (qids.size() < 2) throw PRO_TK_BAD_INPUTS;

    int cur_qid = qids[0];
    ProFeature last_feat{};
    for (size_t i = 1; i < qids.size(); ++i) {
        std::string step_id = (qids.size() == 2) ? id_str : (id_str + "_" + std::to_string(i));
        std::wstring step_name = wname.empty() ? std::wstring() : (qids.size() == 2 ? wname : (wname + L"_" + std::to_wstring(i)));
        last_feat = op_quilt_merge_pair(ctx, step_id, step_name, cur_qid, qids[i], &cur_qid);
    }
    ctx.quilt_ids[id_str] = cur_qid;
    double area = 0.0, qvol = 0.0;
    eval_quilt_area(ctx.solid, cur_qid, &area);
    ProQuilt q = nullptr;
    bool is_closed = (ProQuiltInit(ctx.solid, cur_qid, &q) == PRO_TK_NO_ERROR && q &&
                      ProQuiltVolumeEval(q, &qvol) == PRO_TK_NO_ERROR);
    log_printf(ctx.log, "MERGE_QUILT[%s] qid=%d area=%.4f closed=%s qvol=%.4f\n",
               id_str.c_str(), cur_qid, area, is_closed ? "true" : "false", qvol);
    return last_feat;
}

ProFeature op_solidify(PlanContext &ctx, const Json &op) {
    std::string id_str = op.value("id", "solidify");
    ProSelection qsel = nullptr;
    if (resolve_quilt_selection(ctx, op.at("quilt"), &qsel) != PRO_TK_NO_ERROR || !qsel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    std::string mode = op.value("mode", "add");
    bool is_cut = (mode == "cut");
    bool attach_body = op.value("attach_body", true);
    std::string side = op.value("side", "auto");
    std::wstring wname = resolve_feat_name(ctx, op);

    auto build_and_create = [&](int side_opt) -> ProFeature {
        ProElement root = nullptr;
        elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
        elem_add_int(root, PRO_E_FEATURE_TYPE, is_cut ? PRO_FEAT_CUT : PRO_FEAT_PROTRUSION);
        elem_add_int(root, PRO_E_FEATURE_FORM, PRO_USE_SURFS);
        if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());
        elem_add_ref(root, PRO_E_PATCH_QUILT, qsel);
        elem_add_int(root, PRO_E_PATCH_MATERIAL_SIDE, side_opt);
        add_body_elements(root, ctx.solid, attach_body ? 1 : 0);
        try {
            ProFeature f = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
            ProElementFree(&root);
            return f;
        } catch (...) {
            ProElementFree(&root);
            throw;
        }
    };

    ProFeature feat{};
    try {
        if (side == "positive" || side == "side_one") {
            feat = build_and_create(PRO_SOLIDIFY_SIDE_ONE);
        } else if (side == "negative" || side == "side_two") {
            feat = build_and_create(PRO_SOLIDIFY_SIDE_TWO);
        } else {
            try {
                feat = build_and_create(PRO_SOLIDIFY_SIDE_ONE);
            } catch (...) {
                log_printf(ctx.log, "SOLIDIFY_RETRY_SIDE_TWO[%s]\n", id_str.c_str());
                feat = build_and_create(PRO_SOLIDIFY_SIDE_TWO);
            }
        }
    } catch (...) {
        ProSelectionFree(&qsel);
        throw;
    }
    ProSelectionFree(&qsel);
    return feat;
}

ProFeature op_mirror(PlanContext &ctx, const Json &op) {
    std::string id_str = op.value("id", "mirror");
    std::wstring wname = resolve_feat_name(ctx, op);
    ProSelection plane_sel = nullptr;
    if (resolve_plane_selection(ctx, op.at("plane"), &plane_sel) != PRO_TK_NO_ERROR || !plane_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    const Json &refs_spec = op.value("references", Json::array());
    bool whole_part = refs_spec.empty();
    std::vector<ProReference> ref_vec;
    int mirrored_quilt_src = -1;

    if (!whole_part) {
        for (const Json &r : refs_spec) {
            ProSelection s = nullptr;
            if (r.is_object() && r.contains("quilt")) {
                if (resolve_quilt_selection(ctx, r["quilt"], &s) != PRO_TK_NO_ERROR || !s) {
                    ProSelectionFree(&plane_sel);
                    throw PRO_TK_E_NOT_FOUND;
                }
                resolve_quilt_id(ctx, r["quilt"], &mirrored_quilt_src);
            } else if (r.is_object() && r.contains("feature")) {
                log_printf(ctx.log, "ERROR_MIRROR_FEATURE_UNSUPPORTED[%s] use geometry (quilt/curve) or explicit symmetric feature\n", id_str.c_str());
                ProSelectionFree(&plane_sel);
                throw PRO_TK_BAD_INPUTS;
            } else if (r.is_object() && r.contains("curve_feat")) {
                auto fit = ctx.features.find(r["curve_feat"].get<std::string>());
                if (fit == ctx.features.end() || find_feat_geom_sel(&fit->second, PRO_CURVE, &s) != PRO_TK_NO_ERROR) {
                    ProSelectionFree(&plane_sel);
                    throw PRO_TK_E_NOT_FOUND;
                }
            } else if (r.is_string()) {
                if (resolve_quilt_selection(ctx, r, &s) == PRO_TK_NO_ERROR && s) {
                    resolve_quilt_id(ctx, r, &mirrored_quilt_src);
                } else {
                    auto fit = ctx.features.find(r.get<std::string>());
                    if (fit != ctx.features.end() &&
                        find_feat_geom_sel(&fit->second, PRO_CURVE, &s) == PRO_TK_NO_ERROR && s) {
                        /* Mirrored curve geometry safely */
                    } else {
                        log_printf(ctx.log, "ERROR_MIRROR_FEATURE_UNSUPPORTED[%s] ref='%s' (not quilt/curve)\n",
                                   id_str.c_str(), r.get<std::string>().c_str());
                        ProSelectionFree(&plane_sel);
                        throw (fit == ctx.features.end()) ? PRO_TK_E_NOT_FOUND : PRO_TK_BAD_INPUTS;
                    }
                }
            } else {
                ProSelectionFree(&plane_sel);
                throw PRO_TK_BAD_INPUTS;
            }
            ProReference pref = nullptr;
            ProSelectionToReference(s, &pref);
            ProSelectionFree(&s);
            if (pref) ref_vec.push_back(pref);
        }
    } else {
        ProModelitem mitem{};
        ProSelection msel = nullptr;
        ProReference pref = nullptr;
        ProMdlToModelitem(ctx.mdl, &mitem);
        ProSelectionAlloc(nullptr, &mitem, &msel);
        ProSelectionToReference(msel, &pref);
        ProSelectionFree(&msel);
        if (pref) ref_vec.push_back(pref);
    }

    ProElement root = nullptr, items_e = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, whole_part ? PRO_FEAT_GEN_MERGE : PRO_FEAT_SRF_MDL);
    elem_add_int(root, PRO_E_SRF_TRANS_TYPE,
                 whole_part ? PRO_SURF_TRANS_TYPE_PART_MIRROR : PRO_SURF_TRANS_TYPE_MIRROR);
    if (!wname.empty()) elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, wname.c_str());

    elem_add_compound(root, PRO_E_MIRROR_REF_ITEMS, &items_e);
    ProReference *ref_arr = nullptr;
    ProArrayAlloc(static_cast<int>(ref_vec.size()), sizeof(ProReference), 1, reinterpret_cast<ProArray *>(&ref_arr));
    for (size_t i = 0; i < ref_vec.size(); ++i) ref_arr[i] = ref_vec[i];
    ProElementReferencesSet(items_e, ref_arr);
    ProArrayFree(reinterpret_cast<ProArray *>(&ref_arr));

    elem_add_ref(root, PRO_E_MIRROR_REF_PLANE, plane_sel);
    ProSelectionFree(&plane_sel);
    elem_add_int(root, PRO_E_COPY_NO_COPY,
                 op.value("keep_original", true) ? PRO_MIRROR_KEEP_ORIGINAL : PRO_MIRROR_HIDE_ORIGINAL);

    try {
        ProFeature f = create_with_opts(ctx.solid, root, ctx.log, id_str.c_str());
        ProElementFree(&root);
        if (mirrored_quilt_src >= 0) {
            int mqid = record_feature_quilt_id(ctx, id_str, &f);
            log_printf(ctx.log, "MIRROR_QUILT[%s] src=%d dst=%d\n", id_str.c_str(), mirrored_quilt_src, mqid);
        }
        return f;
    } catch (...) {
        ProElementFree(&root);
        throw;
    }
}

static ProDatumside parse_datum_side(const Json &spec, const char *key, ProDatumside def_side) {
    if (!spec.contains(key) || !spec[key].is_string()) return def_side;
    std::string v = spec[key].get<std::string>();
    if (v == "yellow" || v == "pos" || v == "+") return PRO_DATUM_SIDE_YELLOW;
    if (v == "red" || v == "neg" || v == "-") return PRO_DATUM_SIDE_RED;
    if (v == "none") return PRO_DATUM_SIDE_NONE;
    return def_side;
}

static int parse_conn_flip_state(const Json &spec, const char *key, int def_state) {
    if (!spec.contains(key) || !spec[key].is_string()) return def_state;
    std::string v = spec[key].get<std::string>();
    if (v == "flipped" || v == "flip" || v == "red") return PRO_ASM_FLIPPED;
    if (v == "not_flipped" || v == "normal" || v == "yellow") return PRO_ASM_NOT_FLIPPED;
    if (v == "undefined" || v == "none") return PRO_ASM_FLIP_UNDEFINED;
    return def_state;
}

static ProAsmcompConstrType parse_asm_constr_type(const std::string &t) {
    static const std::map<std::string, ProAsmcompConstrType> m = {
        {"mate", PRO_ASM_MATE},
        {"mate_offset", PRO_ASM_MATE_OFF},
        {"align", PRO_ASM_ALIGN},
        {"align_offset", PRO_ASM_ALIGN_OFF},
        {"insert", PRO_ASM_INSERT},
        {"orient", PRO_ASM_ORIENT},
        {"csys", PRO_ASM_CSYS},
        {"tangent", PRO_ASM_TANGENT},
        {"align_angle", PRO_ASM_ALIGN_ANG_OFF},
        {"mate_angle", PRO_ASM_MATE_ANG_OFF},
        {"fix", PRO_ASM_FIX},
        {"default", PRO_ASM_DEF_PLACEMENT},
    };
    auto it = m.find(t);
    if (it == m.end()) throw PRO_TK_BAD_INPUTS;
    return it->second;
}

ProFeature op_assemble_component(PlanContext &ctx, const Json &op) {
    if (!ctx.is_assembly) {
        log_printf(ctx.log, "ERROR_ASSEMBLE_IN_PART_PLAN\n");
        throw PRO_TK_BAD_CONTEXT;
    }

    std::string id_str = op.value("id", "component");
    std::string mdl_name = op.at("model").get<std::string>();
    std::wstring w_mdl = utf8_to_w(mdl_name);
    ProMdlfileType ctype = (op.value("model_type", "part") == "assembly") ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART;

    ProMdl child_mdl = nullptr;
    ProError st = ProMdlnameRetrieve(const_cast<wchar_t *>(w_mdl.c_str()), ctype, &child_mdl);
    if (st != PRO_TK_NO_ERROR || !child_mdl) {
        std::string fext = (ctype == PRO_MDLFILE_ASSEMBLY) ? ".asm" : ".prt";
        std::wstring w_file = utf8_to_w(mdl_name + fext);
        ProPath pbuf{};
        wcsncpy_s(pbuf, PRO_PATH_SIZE, w_file.c_str(), _TRUNCATE);
        st = ProMdlFiletypeLoad(pbuf, PRO_MDLFILE_UNUSED, PRO_B_FALSE, &child_mdl);
    }
    if (st != PRO_TK_NO_ERROR || !child_mdl) {
        log_st(ctx.log, ("retrieve_comp:" + mdl_name).c_str(), (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st);
        throw (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    }

    ProMatrix init_pos{};
    st = build_rigid_trf_from_op(op, init_pos);
    if (st != PRO_TK_NO_ERROR) throw st;

    ProAsmcomp comp{};
    std::string placement = op.value("placement", "fixed");
    if (placement == "fixed" || placement == "default" || placement == "csys" || placement == "constraints") {
        st = ProAsmcompAssemble(ctx.solid, reinterpret_cast<ProSolid>(child_mdl), init_pos, &comp);
        if (st != PRO_TK_NO_ERROR) {
            log_st(ctx.log, ("ProAsmcompAssemble:" + id_str).c_str(), st);
            throw st;
        }
    }

    if (placement == "fixed" || placement == "default") {
        ProAsmcompconstraint *clist = nullptr;
        ProArrayAlloc(1, sizeof(ProAsmcompconstraint), 1, reinterpret_cast<ProArray *>(&clist));
        ProAsmcompconstraintAlloc(&clist[0]);
        ProAsmcompconstraintTypeSet(clist[0], (placement == "fixed") ? PRO_ASM_FIX : PRO_ASM_DEF_PLACEMENT);
        st = ProAsmcompConstraintsSet(nullptr, &comp, clist);
        ProAsmcompconstraintArrayFree(clist);
        if (st != PRO_TK_NO_ERROR) {
            log_st(ctx.log, ("asm_" + placement + ":" + id_str).c_str(), st);
            throw st;
        }
    } else if (placement == "csys") {
        ProSelection asm_sel = nullptr, comp_sel = nullptr;
        Json cspec = {
            {"asm_ref", op.value("asm_csys", Json("ASM_DEF_CSYS"))},
            {"asm_component", op.value("asm_component", Json())}
        };
        if (resolve_asm_side_sel(ctx, cspec, "asm_component", "asm_ref", &asm_sel) != PRO_TK_NO_ERROR || !asm_sel) {
            throw PRO_TK_E_NOT_FOUND;
        }
        if (resolve_model_item_sel(child_mdl, nullptr, op.value("comp_csys", Json("PRT_CSYS_DEF")), &comp_sel, ctx.log) !=
                PRO_TK_NO_ERROR ||
            !comp_sel) {
            ProSelectionFree(&asm_sel);
            throw PRO_TK_E_NOT_FOUND;
        }
        ProAsmcompconstraint *clist = nullptr;
        ProArrayAlloc(1, sizeof(ProAsmcompconstraint), 1, reinterpret_cast<ProArray *>(&clist));
        ProAsmcompconstraintAlloc(&clist[0]);
        ProAsmcompconstraintTypeSet(clist[0], PRO_ASM_CSYS);
        ProAsmcompconstraintAsmreferenceSet(clist[0], asm_sel, PRO_DATUM_SIDE_YELLOW);
        ProAsmcompconstraintCompreferenceSet(clist[0], comp_sel, PRO_DATUM_SIDE_YELLOW);
        ProSelectionFree(&asm_sel);
        ProSelectionFree(&comp_sel);
        st = ProAsmcompConstraintsSet(nullptr, &comp, clist);
        ProAsmcompconstraintArrayFree(clist);
        if (st != PRO_TK_NO_ERROR) {
            log_st(ctx.log, ("asm_csys:" + id_str).c_str(), st);
            throw st;
        }
    } else if (placement == "constraints") {
        const Json &cs = op.at("constraints");
        int nc = static_cast<int>(cs.size());
        ProAsmcompconstraint *clist = nullptr;
        ProArrayAlloc(nc, sizeof(ProAsmcompconstraint), 1, reinterpret_cast<ProArray *>(&clist));
        for (int i = 0; i < nc; ++i) {
            const Json &c = cs[i];
            ProAsmcompConstrType ctype = parse_asm_constr_type(c.at("type").get<std::string>());
            ProAsmcompconstraintAlloc(&clist[i]);
            ProAsmcompconstraintTypeSet(clist[i], ctype);

            if (ctype != PRO_ASM_FIX && ctype != PRO_ASM_DEF_PLACEMENT) {
                ProSelection asm_sel = nullptr, comp_sel = nullptr;
                if (resolve_asm_side_sel(ctx, c, "asm_component", "asm_ref", &asm_sel) != PRO_TK_NO_ERROR || !asm_sel) {
                    ProAsmcompconstraintArrayFree(clist);
                    throw PRO_TK_E_NOT_FOUND;
                }
                if (resolve_model_item_sel(child_mdl, nullptr, c.at("comp_ref"), &comp_sel, ctx.log) != PRO_TK_NO_ERROR ||
                    !comp_sel) {
                    ProSelectionFree(&asm_sel);
                    ProAsmcompconstraintArrayFree(clist);
                    throw PRO_TK_E_NOT_FOUND;
                }
                ProAsmcompconstraintAsmreferenceSet(clist[i], asm_sel, parse_datum_side(c, "asm_side", PRO_DATUM_SIDE_YELLOW));
                ProAsmcompconstraintCompreferenceSet(clist[i], comp_sel, parse_datum_side(c, "comp_side", PRO_DATUM_SIDE_YELLOW));
                ProSelectionFree(&asm_sel);
                ProSelectionFree(&comp_sel);
                if (c.contains("offset")) {
                    ProAsmcompconstraintOffsetSet(clist[i], c["offset"].get<double>());
                }
            }
        }
        st = ProAsmcompConstraintsSet(nullptr, &comp, clist);
        ProAsmcompconstraintArrayFree(clist);
        if (st != PRO_TK_NO_ERROR) {
            log_st(ctx.log, ("asm_constraints:" + id_str).c_str(), st);
            throw st;
        }
    } else if (placement == "pin" || placement == "slider" || placement == "cylindrical" ||
               placement == "bearing" || placement == "ball" || placement == "planar") {
        auto parse_set_type = [](const std::string &p) -> ProAsmcompSetType {
            if (p == "slider") return PRO_ASM_SET_TYPE_SLIDER;
            if (p == "cylindrical") return PRO_ASM_SET_TYPE_CYLINDRICAL;
            if (p == "bearing") return PRO_ASM_SET_TYPE_BEARING;
            if (p == "ball") return PRO_ASM_SET_TYPE_BALL;
            if (p == "planar") return PRO_ASM_SET_TYPE_PLANAR;
            return PRO_ASM_SET_TYPE_PIN;
        };

        ProElement root = nullptr, mdl_el = nullptr;
        elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
        elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_COMPONENT);
        if (ProElementAlloc(PRO_E_COMPONENT_MODEL, &mdl_el) != PRO_TK_NO_ERROR ||
            ProElementSpecialvalueSet(mdl_el, reinterpret_cast<ProAppData>(child_mdl)) != PRO_TK_NO_ERROR ||
            ProElemtreeElementAdd(root, nullptr, mdl_el) != PRO_TK_NO_ERROR) {
            ProElementFree(&root);
            throw PRO_TK_GENERAL_ERROR;
        }
        elem_add_trf(root, PRO_E_COMPONENT_INIT_POS, init_pos);

        ProElement sets_arr = nullptr, carr = nullptr;
        elem_add_compound(root, PRO_E_COMPONENT_SETS, &sets_arr);
        elem_add_compound(root, PRO_E_COMPONENT_CONSTRAINTS, &carr);

        auto add_set_block = [&](int set_id, const Json &set_op, const std::string &p_str) {
            ProAsmcompSetType set_type = parse_set_type(p_str);
            ProElement set_e = nullptr;
            elem_add_compound(sets_arr, PRO_E_COMPONENT_SET, &set_e);
            elem_add_int(set_e, PRO_E_COMPONENT_SET_ID, set_id);
            elem_add_int(set_e, PRO_E_COMPONENT_SET_TYPE, static_cast<int>(set_type));
            std::wstring sname = utf8_to_w(set_op.value("set_name", p_str + "_" + std::to_string(set_id + 1)));
            elem_add_wstr(set_e, PRO_E_COMPONENT_SET_NAME, sname.c_str());
            elem_add_int(set_e, PRO_E_COMPONENT_SET_MISC_ATTR, PRO_ASM_ENABLE_COMP_SET);
            elem_add_bool(set_e, PRO_E_COMPONENT_SET_FLIP, set_op.value("set_flip", false) ? PRO_B_TRUE : PRO_B_FALSE);

            if (set_op.contains("jas") && set_op["jas"].is_object()) {
                const Json &jas = set_op["jas"];
                ProElement jas_sets = nullptr, jas_set = nullptr;
                elem_add_compound(set_e, PRO_E_COMPONENT_JAS_SETS, &jas_sets);
                elem_add_compound(jas_sets, PRO_E_COMPONENT_JAS_SET, &jas_set);

                ProAxiszerotype ztype = (p_str == "slider") ? PRO_AXIS_ZERO_TRANSLATE1 : PRO_AXIS_ZERO_ROTATION1;
                elem_add_int(jas_set, PRO_E_COMPONENT_JAS_ZERO_TYPE, static_cast<int>(ztype));

                if (jas.contains("comp_zero_ref") && jas.contains("asm_zero_ref")) {
                    ProSelection orange_sel = nullptr, green_sel = nullptr;
                    if (resolve_model_item_sel(child_mdl, nullptr, jas["comp_zero_ref"], &orange_sel, ctx.log) != PRO_TK_NO_ERROR ||
                        !orange_sel) {
                        ProElementFree(&root);
                        throw PRO_TK_E_NOT_FOUND;
                    }
                    if (resolve_asm_side_sel(ctx, jas, "asm_zero_component", "asm_zero_ref", &green_sel) != PRO_TK_NO_ERROR ||
                        !green_sel) {
                        ProSelectionFree(&orange_sel);
                        ProElementFree(&root);
                        throw PRO_TK_E_NOT_FOUND;
                    }
                    ProElement jrefs = nullptr;
                    elem_add_compound(jas_set, PRO_E_COMPONENT_JAS_REFS, &jrefs);
                    elem_add_ref(jrefs, PRO_E_COMPONENT_JAS_ORANGE_REF, orange_sel);
                    elem_add_ref(jrefs, PRO_E_COMPONENT_JAS_GREEN_REF, green_sel);
                    ProSelectionFree(&orange_sel);
                    ProSelectionFree(&green_sel);
                    if (jas.contains("zero_offset")) {
                        elem_add_double(jrefs, PRO_E_COMPONENT_JAS_0_OFFSET_VAL, jas["zero_offset"].get<double>());
                    }
                }

                if (jas.contains("regen_value")) {
                    ProElement rgrp = nullptr;
                    elem_add_compound(jas_set, PRO_E_COMPONENT_JAS_REGEN_VALUE_GROUP, &rgrp);
                    elem_add_bool(rgrp, PRO_E_COMPONENT_JAS_REGEN_VALUE_FLAG, PRO_B_TRUE);
                    elem_add_double(rgrp, PRO_E_COMPONENT_JAS_REGEN_VALUE, jas["regen_value"].get<double>());
                }
                if (jas.contains("min_limit")) {
                    ProElement mgrp = nullptr;
                    elem_add_compound(jas_set, PRO_E_COMPONENT_JAS_MIN_LIMIT, &mgrp);
                    elem_add_bool(mgrp, PRO_E_COMPONENT_JAS_MIN_LIMIT_FLAG, PRO_B_TRUE);
                    elem_add_double(mgrp, PRO_E_COMPONENT_JAS_MIN_LIMIT_VAL, jas["min_limit"].get<double>());
                }
                if (jas.contains("max_limit")) {
                    ProElement xgrp = nullptr;
                    elem_add_compound(jas_set, PRO_E_COMPONENT_JAS_MAX_LIMIT, &xgrp);
                    elem_add_bool(xgrp, PRO_E_COMPONENT_JAS_MAX_LIMIT_FLAG, PRO_B_TRUE);
                    elem_add_double(xgrp, PRO_E_COMPONENT_JAS_MAX_LIMIT_VAL, jas["max_limit"].get<double>());
                }
            }

            const Json &cs = set_op.at("constraints");
            for (size_t i = 0; i < cs.size(); ++i) {
                const Json &c = cs[i];
                ProAsmcompConstrType ctype = parse_asm_constr_type(c.at("type").get<std::string>());
                ProSelection asm_sel = nullptr, comp_sel = nullptr;
                if (resolve_asm_side_sel(ctx, c, "asm_component", "asm_ref", &asm_sel) != PRO_TK_NO_ERROR || !asm_sel) {
                    ProElementFree(&root);
                    throw PRO_TK_E_NOT_FOUND;
                }
                if (resolve_model_item_sel(child_mdl, nullptr, c.at("comp_ref"), &comp_sel, ctx.log) != PRO_TK_NO_ERROR ||
                    !comp_sel) {
                    ProSelectionFree(&asm_sel);
                    ProElementFree(&root);
                    throw PRO_TK_E_NOT_FOUND;
                }
                ProElement ce = nullptr;
                elem_add_compound(carr, PRO_E_COMPONENT_CONSTRAINT, &ce);
                elem_add_int(ce, PRO_E_COMPONENT_CONSTR_TYPE, static_cast<int>(ctype));
                elem_add_ref(ce, PRO_E_COMPONENT_COMP_CONSTR_REF, comp_sel);
                elem_add_ref(ce, PRO_E_COMPONENT_ASSEM_CONSTR_REF, asm_sel);
                ProSelectionFree(&asm_sel);
                ProSelectionFree(&comp_sel);
                elem_add_double(ce, PRO_E_COMPONENT_CONSTR_REF_OFFSET, c.value("offset", 0.0));
                elem_add_int(ce, PRO_E_COMPONENT_CONSTR_ATTR, PRO_ASM_CONSTR_ATTR_NONE);
                elem_add_int(ce, PRO_E_COMPONENT_ASSM_ORIENT, parse_conn_flip_state(c, "asm_orient", PRO_ASM_NOT_FLIPPED));
                elem_add_int(ce, PRO_E_COMPONENT_COMP_ORIENT, parse_conn_flip_state(c, "comp_orient", PRO_ASM_NOT_FLIPPED));
                elem_add_int(ce, PRO_E_COMPONENT_CONSTR_SET_ID, set_id);
            }
        };

        add_set_block(0, op, placement);
        if (op.contains("additional_sets") && op["additional_sets"].is_array()) {
            const Json &asets = op["additional_sets"];
            for (size_t si = 0; si < asets.size(); ++si) {
                const Json &aspec = asets[si];
                add_set_block(static_cast<int>(si + 1), aspec, aspec.value("placement", "pin"));
            }
        }

        try {
            comp = create_with_opts(ctx.solid, root, ctx.log, ("asm_" + placement + ":" + id_str).c_str());
        } catch (...) {
            std::wstring xdiag = utf8_to_w("comp_err_" + id_str + ".xml");
            ProPath xp{};
            wcsncpy_s(xp, PRO_PATH_SIZE, xdiag.c_str(), _TRUNCATE);
            ProElemtreeWrite(root, PRO_ELEMTREE_XML, xp);
            ProElementFree(&root);
            throw;
        }
        ProElementFree(&root);
    } else {
        log_printf(ctx.log, "UNKNOWN_PLACEMENT=%s\n", placement.c_str());
        throw PRO_TK_BAD_INPUTS;
    }

    if (op.value("rename_component_feature", false)) {
        std::wstring wname = resolve_feat_name(ctx, op);
        if (!wname.empty()) {
            ProName cname{};
            wcsncpy_s(cname, PRO_NAME_SIZE, wname.c_str(), _TRUNCATE);
            ProModelitemNameSet(reinterpret_cast<ProModelitem *>(&comp), cname);
        }
    }
    ProSolidRegenerate(ctx.solid, PRO_REGEN_NO_FLAGS);
    ctx.components[id_str] = comp;
    log_component_placement(ctx.log, ctx.solid, id_str.c_str(), &comp);
    return comp;
}

inline bool eval_surface_size_metrics(ProMdl mdl, int sid, double &out_area, double &out_max_span, double &out_eff_width) {
    out_area = 0.0;
    out_max_span = 0.0;
    out_eff_width = 0.0;
    ProSurface surf = nullptr;
    if (ProSurfaceInit(mdl, sid, &surf) != PRO_TK_NO_ERROR || !surf) return false;
    if (ProSurfaceAreaEval(surf, &out_area) != PRO_TK_NO_ERROR || out_area <= 1e-3) {
        out_area = 0.0;
        return true;
    }
    SrfBboxAccum acc;
    acc.surf = surf;
    ProSurfaceContourVisit(surf, visit_srf_contour_bbox_cb, nullptr, &acc);
    if (acc.sample_count <= 0) {
        out_max_span = std::sqrt(out_area);
        out_eff_width = out_max_span;
        return true;
    }
    double dx = std::max(0.0, acc.bmax[0] - acc.bmin[0]);
    double dy = std::max(0.0, acc.bmax[1] - acc.bmin[1]);
    double dz = std::max(0.0, acc.bmax[2] - acc.bmin[2]);
    out_max_span = std::max({dx, dy, dz});
    out_eff_width = (out_max_span > 1e-3) ? (out_area / out_max_span) : 0.0;
    return true;
}

void op_set_appearance_or_texture(PlanContext &ctx, const Json &op, bool is_texture_op) {
    std::string id_str = op.value("id", is_texture_op ? "texture" : "appearance");
    std::string scope = op.value("scope", ctx.is_assembly ? "component" : "part");

    ProSurfaceAppearanceProps props{};
    fill_appearance_props_from_json(op, props, id_str);

    bool has_fallback_app = op.contains("fallback_rgb") || op.contains("fallback_preset");
    ProSurfaceAppearanceProps fb_props{};
    if (has_fallback_app) {
        Json fb_op = Json::object();
        fb_op["preset"] = op.value("fallback_preset", "military_paint");
        if (op.contains("fallback_rgb")) fb_op["rgb"] = op["fallback_rgb"];
        fb_op["appearance_name"] = op.value("fallback_appearance_name", id_str + "_solid");
        fill_appearance_props_from_json(fb_op, fb_props, id_str + "_solid");
    }

    const double min_srf_area = op.value("min_surface_area", 0.0);
    const double min_srf_span = op.value("min_surface_span", 0.0);
    const double min_max_span = op.value("min_max_span", 0.0);
    const double world_tile_size = op.value("world_tile_size", 0.0);
    const bool use_size_filter = (min_srf_area > 0.0 || min_srf_span > 0.0 || min_max_span > 0.0);

    bool apply_texture = is_texture_op || (op.contains("texture_file") && op["texture_file"].is_string());
    ProSurfaceTextureProps tex_props{};
    ProSurfaceTexturePlacementProps *place_arr = nullptr;
    double base_u_scale = 1.0, base_v_scale = 1.0;
    if (apply_texture) {
        std::string tex_file = op.at("texture_file").get<std::string>();
        strncpy_s(tex_props.texture_map, sizeof(tex_props.texture_map), tex_file.c_str(), _TRUNCATE);
        ProArrayAlloc(1, sizeof(ProSurfaceTexturePlacementProps), 1, reinterpret_cast<ProArray *>(&place_arr));
        fill_texture_placement_from_json(op, place_arr[0]);
        base_u_scale = place_arr[0].horizontal_scale;
        base_v_scale = place_arr[0].vertical_scale;
    }

    auto apply_texture_props = [&](ProModelitem &item, bool &logged_once) -> ProError {
        ProAsmcomppath cpath{};
        ProIdTable dummy_ids{};
        ProAsmcomppathInit(ctx.solid, dummy_ids, 0, &cpath);
        ProAsmitem asm_item{};
        asm_item.item = item;
        asm_item.name[0] = 0;
        asm_item.path = cpath;
        ProError tst = ProMdlVisibleSideTexturepropsSet(&asm_item, 0, &tex_props);
        if (tst == PRO_TK_NO_ERROR) {
            tst = ProMdlVisibleSideTextureplacementpropsSet(&asm_item, 0, place_arr);
        }
        if (tst == PRO_TK_NO_ERROR && !logged_once) {
            logged_once = true;
            ProSurfaceTexturePlacementProps *rb_arr = nullptr;
            if (ProMdlVisibleSideTextureplacementpropsGet(&asm_item, 0, &rb_arr) == PRO_TK_NO_ERROR && rb_arr) {
                int rn = 0;
                ProArraySizeGet(reinterpret_cast<ProArray>(rb_arr), &rn);
                if (rn > 0) {
                    log_printf(ctx.log, "TEX_PLACED[%s] proj=%d ttype=%d h_sc=%.4f v_sc=%.4f orig=(%.1f,%.1f,%.1f)\n",
                               id_str.c_str(), static_cast<int>(rb_arr[0].projection),
                               static_cast<int>(rb_arr[0].texture_type),
                               rb_arr[0].horizontal_scale, rb_arr[0].vertical_scale,
                               rb_arr[0].local_csys[3][0], rb_arr[0].local_csys[3][1], rb_arr[0].local_csys[3][2]);
                }
                ProArrayFree(reinterpret_cast<ProArray *>(&rb_arr));
            }
        }
        return tst;
    };

    auto apply_to_surface_ids = [&](const std::vector<int> &sids) {
        int ok_cnt = 0;
        int tex_cnt = 0;
        int skipped_small = 0;
        bool logged_once = false;
        ProError first_err = PRO_TK_NO_ERROR;
        for (int sid : sids) {
            ProModelitem sitem{};
            if (ProModelitemInit(ctx.mdl, sid, PRO_SURFACE, &sitem) != PRO_TK_NO_ERROR) continue;

            double srf_area = 0.0, max_span = 0.0, eff_width = 0.0;
            bool have_metrics = false;
            if (apply_texture && (use_size_filter || world_tile_size > 0.0)) {
                have_metrics = eval_surface_size_metrics(ctx.mdl, sid, srf_area, max_span, eff_width);
            }

            if (apply_texture && use_size_filter && have_metrics) {
                bool too_small = (min_srf_area > 0.0 && srf_area < min_srf_area) ||
                                 (min_srf_span > 0.0 && eff_width < min_srf_span) ||
                                 (min_max_span > 0.0 && max_span < min_max_span);
                if (too_small) {
                    skipped_small++;
                    if (has_fallback_app) {
                        ProError fst = ProSurfaceSideAppearancepropsSet(&sitem, 0, &fb_props);
                        if (fst == PRO_TK_NO_ERROR) ok_cnt++;
                        else if (first_err == PRO_TK_NO_ERROR) first_err = fst;
                    } else {
                        ok_cnt++;
                    }
                    continue;
                }
            }

            if (apply_texture && world_tile_size > 0.0 && have_metrics && max_span > 1e-3) {
                double ratio = std::max(0.05, max_span / world_tile_size);
                place_arr[0].horizontal_scale = ratio * base_u_scale;
                place_arr[0].vertical_scale = ratio * base_v_scale;
            } else if (apply_texture) {
                place_arr[0].horizontal_scale = base_u_scale;
                place_arr[0].vertical_scale = base_v_scale;
            }

            ProError ast = PRO_TK_NO_ERROR;
            if (!is_texture_op || op.contains("rgb") || op.contains("preset")) {
                ast = ProSurfaceSideAppearancepropsSet(&sitem, 0, &props);
            }
            if (ast == PRO_TK_NO_ERROR && apply_texture) {
                ast = apply_texture_props(sitem, logged_once);
                if (ast == PRO_TK_NO_ERROR) tex_cnt++;
            }
            if (ast == PRO_TK_NO_ERROR) ok_cnt++;
            else if (first_err == PRO_TK_NO_ERROR) first_err = ast;
        }
        log_printf(ctx.log, "%s[%s] scope=%s matched=%zu applied=%d tex=%d skipped_small=%d st=%d\n",
                   is_texture_op ? "TEXTURE" : "APPEAR",
                   id_str.c_str(), scope.c_str(), sids.size(), ok_cnt, tex_cnt, skipped_small, static_cast<int>(first_err));
        if (sids.empty() || ok_cnt == 0) {
            throw (first_err == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : first_err;
        }
    };

    try {
        if (scope == "part" || scope == "model") {
            if (op.value("apply_to_faces", false)) {
                std::vector<int> all_ids;
                collect_solid_surface_ids_by_filter(ctx.solid, nullptr, all_ids);
                apply_to_surface_ids(all_ids);
            } else {
                ProModelitem mi{};
                ProMdlToModelitem(ctx.mdl, &mi);
                ProError st = ProSurfaceSideAppearancepropsSet(&mi, 0, &props);
                if (st == PRO_TK_NO_ERROR && apply_texture) {
                    bool logged_once = false;
                    st = apply_texture_props(mi, logged_once);
                }
                log_printf(ctx.log, "%s[%s] scope=part st=%d\n",
                           is_texture_op ? "TEXTURE" : "APPEAR", id_str.c_str(), static_cast<int>(st));
                if (st != PRO_TK_NO_ERROR) throw st;
            }
        } else if (scope == "features") {
            std::set<int> live_ids;
            std::vector<int> dummy;
            collect_solid_surface_ids_by_filter(ctx.solid, nullptr, dummy, &live_ids);
            std::set<int> target_set;
            std::vector<int> target_ids;
            for (const Json &fn : op.at("features")) {
                std::vector<int> fids;
                ProError fst = collect_feature_live_surface_ids(ctx, fn.get<std::string>(), live_ids, fids);
                if (fst != PRO_TK_NO_ERROR) throw fst;
                for (int sid : fids) {
                    if (target_set.insert(sid).second) target_ids.push_back(sid);
                }
            }
            if (op.contains("filter") && op["filter"].is_object()) {
                std::vector<int> filtered;
                collect_solid_surface_ids_by_filter(ctx.solid, &op["filter"], filtered);
                std::set<int> fset(filtered.begin(), filtered.end());
                std::vector<int> both;
                for (int sid : target_ids) {
                    if (fset.count(sid)) both.push_back(sid);
                }
                target_ids = std::move(both);
            }
            apply_to_surface_ids(target_ids);
        } else if (scope == "surfaces") {
            std::vector<int> matched;
            const Json *f_ptr = op.contains("filter") ? &op["filter"] : nullptr;
            ProError sst = collect_solid_surface_ids_by_filter(ctx.solid, f_ptr, matched);
            if (sst != PRO_TK_NO_ERROR) throw sst;
            apply_to_surface_ids(matched);
        } else if (scope == "component") {
            std::string ckey = op.at("component").get<std::string>();
            auto cit = ctx.components.find(ckey);
            if (cit == ctx.components.end()) throw PRO_TK_E_NOT_FOUND;
            ProMdl child = nullptr;
            if (ProAsmcompMdlGet(&cit->second, &child) != PRO_TK_NO_ERROR || !child) throw PRO_TK_E_NOT_FOUND;
            ProMdlType ctype = PRO_MDL_PART;
            ProMdlTypeGet(child, &ctype);
            ProType itype = (ctype == PRO_MDL_ASSEMBLY) ? PRO_ASSEMBLY : PRO_PART;
            ProIdTable ids{cit->second.id};
            ProAsmcomppath cpath{};
            ProAsmitem asmitem{};
            ProError st = ProAsmcomppathInit(ctx.solid, ids, 1, &cpath);
            if (st == PRO_TK_NO_ERROR) {
                ProName ename = L"";
                st = ProAsmcompAsmitemInit(ctx.mdl, cit->second.id, itype, ename, &cpath, &asmitem);
            }
            if (st == PRO_TK_NO_ERROR) {
                st = ProMdlVisibleSideAppearancepropsSet(&asmitem, 0, &props);
            }
            if (st == PRO_TK_NO_ERROR && apply_texture) {
                st = ProMdlVisibleSideTexturepropsSet(&asmitem, 0, &tex_props);
                if (st == PRO_TK_NO_ERROR) st = ProMdlVisibleSideTextureplacementpropsSet(&asmitem, 0, place_arr);
            }
            log_printf(ctx.log, "%s[%s] scope=component comp=%s st=%d\n",
                       is_texture_op ? "TEXTURE" : "APPEAR", id_str.c_str(), ckey.c_str(), static_cast<int>(st));
            if (st != PRO_TK_NO_ERROR) throw st;
        } else {
            log_printf(ctx.log, "UNKNOWN_APPEAR_SCOPE=%s\n", scope.c_str());
            throw PRO_TK_BAD_INPUTS;
        }
    } catch (...) {
        if (place_arr) ProArrayFree(reinterpret_cast<ProArray *>(&place_arr));
        throw;
    }
    if (place_arr) ProArrayFree(reinterpret_cast<ProArray *>(&place_arr));
}

Json load_plan_file() {
    std::ifstream in("creobuild_plan.json", std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open creobuild_plan.json");
    }
    Json plan;
    in >> plan;
    return plan;
}

void read_mode(bool &is_verify, bool &is_anim, bool &use_default_names) {
    is_verify = false;
    is_anim = false;
    use_default_names = false;
    std::ifstream in("creobuild_mode.txt");
    if (!in) return;
    std::string line;
    if (std::getline(in, line)) {
        if (line.find("verify") != std::string::npos) is_verify = true;
        if (line.find("anim") != std::string::npos) is_anim = true;
        if (line.find("default") != std::string::npos) use_default_names = true;
    }
}

struct PendingSketchJob {
    Json spec;
    ProMatrix part_to_sec{};
    ProMatrix sec_to_part{};
    double plane_n[3]{0.0, 0.0, 1.0};
    bool offset_to_crosshair = false;
    bool has_preset_shift = false;
    double preset_shift[2]{0.0, 0.0};
};

struct ActiveBuildState {
    FILE *log = nullptr;
    bool use_default_names = false;
    Json plan;
    size_t next_idx = 0;
    PlanContext ctx;
    std::wstring w_part;
    int win_id = PRO_VALUE_UNUSED;
    bool finished = false;
    bool mech_done = false;

    std::string pending_op;
    std::string pending_id;
    std::wstring pending_name;
    std::string pending_metric_tag;
    bool pending_is_cut = false;
    bool pending_verify_cut = true;
    double pending_vol_before = 0.0;
    std::vector<int> blend_curve_ids;
    size_t blend_pick_idx = 0;
    std::vector<ProSelection> macro_sel_queue;
    size_t macro_sel_idx = 0;
    std::vector<PendingSketchJob> macro_sketch_queue;
    size_t macro_sketch_idx = 0;
    std::set<int> pre_blend_feat_ids;
};

static ActiveBuildState g_build;

void clear_pending_macro_state() {
    for (ProSelection s : g_build.macro_sel_queue) {
        if (s) ProSelectionFree(&s);
    }
    g_build.macro_sel_queue.clear();
    g_build.macro_sel_idx = 0;
    g_build.macro_sketch_queue.clear();
    g_build.macro_sketch_idx = 0;
    g_build.blend_curve_ids.clear();
    g_build.blend_pick_idx = 0;
    g_build.pre_blend_feat_ids.clear();
}

ProError collect_all_feat_ids_cb(ProFeature *p_feat, ProError status, ProAppData data) {
    (void)status;
    auto *out = static_cast<std::vector<int> *>(data);
    if (p_feat) out->push_back(p_feat->id);
    return PRO_TK_NO_ERROR;
}

void finish_build_and_close(ProError st) {
    if (g_build.finished) return;
    g_build.finished = true;
    clear_pending_macro_state();
    FILE *log = g_build.log;

    if (st == PRO_TK_NO_ERROR && g_build.ctx.solid && g_build.ctx.is_assembly &&
        g_build.plan.value("probe_conn_axes", false)) {
        for (const auto &kv : g_build.ctx.components) {
            ProFeature cf = kv.second;
            for (ProType pt : {PRO_MDO_CONN, PRO_MDO_CONN_AXIS_ROT_1, PRO_MDO_CONN_AXIS_TR_1, PRO_MDO_BODY}) {
                GeomIdListCtx gl;
                ProError gst = ProFeatureGeomitemVisit(&cf, pt, visit_geom_id_action, nullptr, &gl);
                if (gst == PRO_TK_NO_ERROR && !gl.ids.empty()) {
                    log_printf(log, "PROBE_COMP_GEOM[%s] type=%d count=%zu first_id=%d\n",
                               kv.first.c_str(), static_cast<int>(pt), gl.ids.size(), gl.ids[0]);
                }
            }
        }
        ProError kst = ProKinDragStart();
        log_printf(log, "PROBE_KIN_DRAG_START st=%d\n", static_cast<int>(kst));
        if (kst == PRO_TK_NO_ERROR || kst == PRO_TK_CHECKOUT_CONFLICT) {
            ProName sname = L"PROBE_SNAP";
            ProError scst = ProSnapshotCreate(sname);
            log_printf(log, "PROBE_SNAP_CREATE st=%d\n", static_cast<int>(scst));
            if (scst == PRO_TK_NO_ERROR) {
                ProSnapshotConstraint *snaps = nullptr;
                ProError cgst = ProSnapshotConstraintsGet(sname, nullptr, &snaps);
                int nsn = 0;
                if (cgst == PRO_TK_NO_ERROR && snaps) ProArraySizeGet(reinterpret_cast<ProArray>(snaps), &nsn);
                log_printf(log, "PROBE_SNAP_CONSTRS st=%d count=%d\n", static_cast<int>(cgst), nsn);
                for (int i = 0; i < nsn; ++i) {
                    int nsel = 0;
                    if (snaps[i].sel_array) ProArraySizeGet(reinterpret_cast<ProArray>(snaps[i].sel_array), &nsel);
                    ProModelitem mi{};
                    ProName miname{};
                    if (nsel > 0) {
                        ProSelectionModelitemGet(snaps[i].sel_array[0], &mi);
                        ProModelitemNameGet(&mi, miname);
                    }
                    log_printf(log, "  SNAP_C[%d] type=%d val=%.3f nsel=%d item_type=%d item_id=%d name=%s\n",
                               i, static_cast<int>(snaps[i].type), snaps[i].value, nsel,
                               static_cast<int>(mi.type), mi.id, w2utf8(miname).c_str());
                }
                if (snaps) ProArrayFree(reinterpret_cast<ProArray *>(&snaps));
                ProSnapshotDelete(sname, nullptr);
            }
            ProKinDragStop();
        }
        // Also probe ProSelect / ProMdl for PRO_MDO_CONN_AXIS_* via ProFeatureVisit or ProSolidFeatVisit
    }

    if (st == PRO_TK_NO_ERROR && g_build.ctx.solid && g_build.ctx.is_assembly) {
        ProSolidRegenerate(g_build.ctx.solid, PRO_REGEN_FORCE_REGEN);
    }

    if (st == PRO_TK_NO_ERROR && g_build.ctx.solid) {
        ProError mst = log_solid_metrics(log, g_build.ctx.solid, "FINAL_MODEL");
        if (mst != PRO_TK_NO_ERROR) st = mst;
        if (st == PRO_TK_NO_ERROR) {
            mst = log_feature_tree(log, g_build.ctx.solid, 1);
            if (mst != PRO_TK_NO_ERROR) st = mst;
        }
        if (st == PRO_TK_NO_ERROR) {
            st = ProMdlSave(g_build.ctx.mdl);
            if (st != PRO_TK_NO_ERROR) log_st(log, "save", st);
        }
        if (st == PRO_TK_NO_ERROR) {
            st = ProIntf3DFileWriteWithDefaultProfile(
                g_build.ctx.solid, PRO_INTF_EXPORT_STEP, const_cast<wchar_t *>(g_build.w_part.c_str()));
            if (st != PRO_TK_NO_ERROR) log_st(log, "export_step", st);
        }
        if (st == PRO_TK_NO_ERROR) {
            std::wstring inf_name = g_build.w_part + L".inf";
            st = ProOutputFileMdlnameWrite(
                g_build.ctx.mdl, const_cast<wchar_t *>(inf_name.c_str()), PRO_MODEL_INFO, nullptr, nullptr, nullptr, nullptr);
            if (st != PRO_TK_NO_ERROR) log_st(log, "export_inf", st);
        }
        if (st == PRO_TK_NO_ERROR && g_build.win_id != PRO_VALUE_UNUSED) {
            ProSelbufferClear();
            DatumDisplayGuard guard;
            if (g_build.plan.value("hide_datums_in_views", false)) {
                guard.hide();
            }
            std::wstring j_def = g_build.w_part + L"_default.jpg";
            std::wstring j_front = g_build.w_part + L"_front.jpg";
            std::wstring j_top = g_build.w_part + L"_top.jpg";
            export_view_jpg(g_build.ctx.mdl, g_build.win_id, L"DEFAULT", j_def.c_str());
            export_view_jpg(g_build.ctx.mdl, g_build.win_id, L"FRONT", j_front.c_str());
            export_view_jpg(g_build.ctx.mdl, g_build.win_id, L"TOP", j_top.c_str());
            if (g_build.plan.value("export_right_view", false)) {
                std::wstring j_right = g_build.w_part + L"_right.jpg";
                export_view_jpg(g_build.ctx.mdl, g_build.win_id, L"RIGHT", j_right.c_str());
            }
            log_printf(log, "EXPORTS_COMPLETE=true\n");
        }
    }
    if (st == PRO_TK_NO_ERROR) {
        log_printf(log, "STATUS=OK\n");
    } else {
        log_printf(log, "STATUS=ERROR\nERROR_CODE=%d\n", static_cast<int>(st));
    }
    if (log) {
        fclose(log);
        g_build.log = nullptr;
    }
}

void run_mech_setup_impl(FILE *log) {
    if (!log || !g_build.ctx.mdl) return;

    std::map<std::string, ProModelitem> axis_by_typed_name;
    std::map<std::string, ProModelitem> axis_by_name;
    for (ProType pt : {PRO_MDO_CONN_AXIS_TR_1, PRO_MDO_CONN_AXIS_ROT_1}) {
        const char *pfx = (pt == PRO_MDO_CONN_AXIS_TR_1) ? "T:" : "R:";
        int found_cnt = 0;
        for (int id = 0; id <= 2000; ++id) {
            ProModelitem mi{};
            if (ProModelitemInit(g_build.ctx.mdl, id, pt, &mi) == PRO_TK_NO_ERROR) {
                ProName nm{};
                ProModelitemNameGet(&mi, nm);
                std::string aname = w2utf8(nm);
                if (!aname.empty()) {
                    axis_by_typed_name[std::string(pfx) + aname] = mi;
                    axis_by_name[aname] = mi;
                }
                found_cnt++;
            }
        }
        log_printf(log, "MDO_AXIS_SCAN type=%d count=%d\n", static_cast<int>(pt), found_cnt);
    }

    if (g_build.plan.contains("motors") && g_build.plan["motors"].is_array()) {
        for (const Json &ms : g_build.plan["motors"]) {
            std::string mname = ms.at("name").get<std::string>();
            std::string axis_name = ms.at("axis_name").get<std::string>();
            std::string mtype_s = ms.value("motion_type", "translational");
            bool is_rot = (mtype_s == "rotational");
            bool flip_dir = ms.value("flip_dir", false);
            std::string interp_s = ms.value("interp", "monotonic");
            const Json &tbl = ms.at("table");
            if (!tbl.is_array() || tbl.empty()) throw PRO_TK_BAD_INPUTS;

            std::string tkey = std::string(is_rot ? "R:" : "T:") + axis_name;
            auto ait = axis_by_typed_name.find(tkey);
            if (ait == axis_by_typed_name.end()) ait = axis_by_name.find(axis_name);
            if (ait == axis_by_typed_name.end() && ait == axis_by_name.end()) {
                log_printf(log, "MOTOR_AXIS_NOT_FOUND[%s] axis=%s\n", mname.c_str(), axis_name.c_str());
                throw PRO_TK_E_NOT_FOUND;
            }

            ProModelitem axis_mi = ait->second;
            ProSelection motor_sel = nullptr;
            if (ProSelectionAlloc(nullptr, &axis_mi, &motor_sel) != PRO_TK_NO_ERROR || !motor_sel) {
                throw PRO_TK_E_NOT_FOUND;
            }

            ProMotorTableInterpType interp_t = PRO_MOTOR_TBL_MONOTONIC;
            if (interp_s == "linear") interp_t = PRO_MOTOR_TBL_LINEAR;
            else if (interp_s == "spline") interp_t = PRO_MOTOR_TBL_SPLINE;

            ProElement tree = nullptr, prof = nullptr, tdata = nullptr, rows = nullptr;
            elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &tree);
            elem_add_int(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_PM_MOTOR);
            std::wstring w_mname = utf8_to_w(mname);
            elem_add_wstr(tree, PRO_E_STD_FEATURE_NAME, w_mname.c_str());
            elem_add_int(tree, PRO_E_MOTOR_MOTION_TYPE, is_rot ? PRO_MOTOR_ROTATIONAL : PRO_MOTOR_TRANSLATIONAL);
            elem_add_ref(tree, PRO_E_MOTOR_DRIVEN_ENT_REF, motor_sel);
            elem_add_int(tree, PRO_E_MOTOR_FLIP_DIR, flip_dir ? 1 : 0);
            elem_add_compound(tree, PRO_E_MOTOR_PROFILE, &prof);
            elem_add_int(prof, PRO_E_MOTOR_DRIVEN_QUANTITY, PRO_MOTOR_POSITION);
            elem_add_int(prof, PRO_E_MOTOR_FUNC_TYPE, PRO_MOTOR_TABLE);
            elem_add_compound(prof, PRO_E_MOTOR_TABLE_DATA, &tdata);
            elem_add_int(tdata, PRO_E_MOTOR_TBL_INTERPOL_TYPE, interp_t);
            elem_add_compound(tdata, PRO_E_MOTOR_TBL_ROWS, &rows);
            for (const Json &pt_row : tbl) {
                ProElement row = nullptr;
                elem_add_compound(rows, PRO_E_MOTOR_TBL_ROW, &row);
                elem_add_double(row, PRO_E_MOTOR_TBL_VAR_VAL, pt_row.at(0).get<double>());
                elem_add_double(row, PRO_E_MOTOR_TBL_FUNC_VAL, pt_row.at(1).get<double>());
                if (interp_t == PRO_MOTOR_TBL_MONOTONIC) {
                    elem_add_int(row, PRO_E_MOTOR_TBL_DERIV_GIVEN, 0);
                }
            }
            elem_add_int(tdata, PRO_E_MOTOR_TBL_DEPEND_ON_FILE, 0);

            try {
                ProFeature mfeat = create_with_opts(g_build.ctx.solid, tree, log, ("pm_motor:" + mname).c_str());
                log_printf(log, "MOTOR_CREATED[%s] feat_id=%d axis_id=%d flip=%d\n",
                           mname.c_str(), mfeat.id, axis_mi.id, flip_dir ? 1 : 0);
            } catch (...) {
                std::wstring xp_name = utf8_to_w("motor_err_" + mname + ".xml");
                ProPath xp{};
                wcsncpy_s(xp, PRO_PATH_SIZE, xp_name.c_str(), _TRUNCATE);
                ProElemtreeWrite(tree, PRO_ELEMTREE_XML, xp);
                ProElementFree(&tree);
                ProSelectionFree(&motor_sel);
                throw;
            }
            ProElementFree(&tree);
            ProSelectionFree(&motor_sel);
        }
    }
    fflush(log);
}

void run_mech_log_pose_impl(FILE *log) {
    if (!log || !g_build.ctx.solid) return;
    if (g_build.plan.contains("operations") && g_build.plan["operations"].is_array()) {
        for (const Json &op : g_build.plan["operations"]) {
            if (op.value("op", "") != "assemble_component") continue;
            std::string id = op.value("id", "");
            auto it = g_build.ctx.components.find(id);
            if (it != g_build.ctx.components.end()) {
                log_component_placement(log, g_build.ctx.solid, ("POST_ANALYSIS:" + id).c_str(), &it->second);
            }
        }
    }
    fflush(log);
}

void prepare_macro_op(const Json &op, const std::string &id_str, const char *op_kind) {
    clear_pending_macro_state();
    ProSelbufferClear();

    g_build.pending_op = op_kind;
    g_build.pending_id = id_str;
    g_build.pending_name = resolve_feat_name(g_build.ctx, op);
    g_build.pending_metric_tag = op.contains("metric_tag") && op["metric_tag"].is_string()
                                     ? op["metric_tag"].get<std::string>()
                                     : std::string();
    std::string mode = op.value("mode", "add");
    g_build.pending_is_cut = (mode == "cut");
    g_build.pending_verify_cut = op.value("verify_cut_removed", true);
    g_build.pending_vol_before = 0.0;
    if (g_build.pending_is_cut) {
        Pro3dPnt box_tmp[2]{};
        measure_solid_volume(g_build.ctx.solid, &g_build.pending_vol_before, nullptr, box_tmp);
    }

    std::vector<int> before_ids;
    ProSolidFeatVisit(g_build.ctx.solid, collect_all_feat_ids_cb, nullptr, &before_ids);
    g_build.pre_blend_feat_ids.insert(before_ids.begin(), before_ids.end());
}

void launch_dashboard_macro(const std::wstring &macro, const char *tag) {
    ProError mst = ProMacroLoad(const_cast<wchar_t *>(macro.c_str()));
    int cur_w = -1;
    ProWindowCurrentGet(&cur_w);
    log_printf(g_build.log, "  %s_MACRO_LOAD id=%s len=%zu st=%d win_id=%d cur_win=%d\n",
               tag, g_build.pending_id.c_str(), macro.size(), static_cast<int>(mst), g_build.win_id, cur_w);
    if (mst != PRO_TK_NO_ERROR) throw mst;
}

std::wstring fmt_macro_double(double v) {
    std::ostringstream oss;
    oss << v;
    return utf8_to_w(oss.str());
}

void schedule_solid_blend(const Json &op, const std::string &id_str) {
    const Json &sections = op.at("sections");
    if (sections.size() < 2) throw PRO_TK_BAD_INPUTS;

    prepare_macro_op(op, id_str, "solid_blend");
    for (const Json &sec_ref : sections) {
        std::string fkey = sec_ref.get<std::string>();
        auto fit = g_build.ctx.features.find(fkey);
        if (fit == g_build.ctx.features.end()) throw PRO_TK_E_NOT_FOUND;
        std::vector<int> cids;
        ProError cst = collect_feat_noncomposite_curves(g_build.ctx.solid, &fit->second, cids);
        if (cst != PRO_TK_NO_ERROR || cids.empty()) throw PRO_TK_E_NOT_FOUND;
        g_build.blend_curve_ids.push_back(cids[0]);
    }

    std::string interp = op.value("interpolation", "smooth");
    std::wstring w_interp = (interp == "straight") ? L"Straight" : L"Smooth";

    std::wstring macro =
        L"~ Command `ProCmdFtPBlend`;"
        L" ~ Activate `main_dlg_cur` `chkbn.blend_sectionspage.0` 1;"
        L" ~ Select `blend_sectionspage.0.0` `PH.sketchselopt` 1 `Selected`;"
        L" ~ Activate `UI Message Dialog` `confirm`;";
    for (size_t i = 0; i < g_build.blend_curve_ids.size(); ++i) {
        if (i > 0) {
            macro += L" ~ Activate `blend_sectionspage.0.0` `PH.insertbtn`;";
        }
        macro += L" ~ Focus `blend_sectionspage.0.0` `PH.curve_coll_repr`;"
                 L" ~ Select `blend_sectionspage.0.0` `PH.curve_coll_repr` 0;"
                 L" ~ Activate `blend_sectionspage.0.0` `PH.chain_btn`;"
                 L" ~ Select `chain_dlg` `ChainMethod` 1 `others`;"
                 L" ~ Select `chain_dlg` `RuleOptions` 1 `all_curves_in_feature`;"
                 L" ~ Command `CreoBlendPickSec`;"
                 L" ~ Activate `chain_dlg` `OKButton`;";
    }
    macro += L" ~ Activate `main_dlg_cur` `chkbn.blend_optionspage.0` 1;"
             L" ~ Select `blend_optionspage.0.0` `PH.blendoptions` 1 `" + w_interp + L"`;"
             L" ~ Activate `main_dlg_cur` `dashInst0.Done`;"
             L" ~ Activate `main_dlg_cur` `dashInst0.Quit`;"
             L" ~ Activate `UI Message Dialog` `yes`;"
             L" ~ Command `CreoBuildResume`;";
    launch_dashboard_macro(macro, "BLEND");
}

void schedule_rotational_blend(const Json &op, const std::string &id_str) {
    const Json &sections = op.at("sections");
    if (sections.size() < 2) throw PRO_TK_BAD_INPUTS;

    ProSelection plane_sel = nullptr, axis_sel = nullptr;
    if (resolve_plane_selection(g_build.ctx, op.at("plane"), &plane_sel) != PRO_TK_NO_ERROR || !plane_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }
    if (resolve_axis_selection(g_build.ctx, op.at("axis"), &axis_sel) != PRO_TK_NO_ERROR || !axis_sel) {
        ProSelectionFree(&plane_sel);
        throw PRO_TK_E_NOT_FOUND;
    }

    prepare_macro_op(op, id_str, "rotational_blend");
    g_build.macro_sel_queue.push_back(plane_sel);
    g_build.macro_sel_queue.push_back(axis_sel);

    PendingSketchJob sec0_job;
    Json spec0 = sections[0];
    if (!spec0.contains("auto_start_vertex")) spec0["auto_start_vertex"] = true;
    sec0_job.spec = spec0;
    init_interactive_plane_xform(g_build.ctx, op.at("plane"), sec0_job.part_to_sec, sec0_job.plane_n, sec0_job.sec_to_part);

    ProMatrix sec0_s2p{};
    memcpy(sec0_s2p, sec0_job.sec_to_part, sizeof(ProMatrix));

    double ref_pt3d[3]{sec0_s2p[3][0], sec0_s2p[3][1], sec0_s2p[3][2]};
    {
        double su = 0.0, sv = 0.0, sx3 = 0.0, sy3 = 0.0, sz3 = 0.0;
        int cnt2 = 0, cnt3 = 0;
        if (spec0.contains("entities_3d")) {
            for (const Json &e : spec0["entities_3d"]) {
                for (const char *k : {"start", "end", "center"}) {
                    if (e.contains(k) && e[k].is_array() && e[k].size() >= 3) {
                        sx3 += e[k][0].get<double>(); sy3 += e[k][1].get<double>(); sz3 += e[k][2].get<double>();
                        cnt3++;
                    }
                }
            }
        } else if (spec0.contains("entities_2d")) {
            for (const Json &e : spec0["entities_2d"]) {
                for (const char *k : {"start", "end", "center"}) {
                    if (e.contains(k) && e[k].is_array() && e[k].size() >= 2) {
                        su += e[k][0].get<double>(); sv += e[k][1].get<double>();
                        cnt2++;
                    }
                }
            }
        }
        if (cnt3 > 0) {
            ref_pt3d[0] = sx3 / cnt3; ref_pt3d[1] = sy3 / cnt3; ref_pt3d[2] = sz3 / cnt3;
        } else if (cnt2 > 0) {
            double u = su / cnt2, v = sv / cnt2;
            ref_pt3d[0] = sec0_s2p[3][0] + u * sec0_s2p[0][0] + v * sec0_s2p[1][0];
            ref_pt3d[1] = sec0_s2p[3][1] + u * sec0_s2p[0][1] + v * sec0_s2p[1][1];
            ref_pt3d[2] = sec0_s2p[3][2] + u * sec0_s2p[0][2] + v * sec0_s2p[1][2];
        }
    }

    bool axis_geom_ok = false;
    Pro3dPnt axis_p1{}, axis_p2{};
    {
        ProModelitem ami{};
        ProAxis ax = nullptr;
        ProGeomitemdata *agd = nullptr;
        if (ProSelectionModelitemGet(axis_sel, &ami) == PRO_TK_NO_ERROR &&
            ProAxisInit(g_build.ctx.solid, ami.id, &ax) == PRO_TK_NO_ERROR && ax &&
            ProAxisDataGet(ax, &agd) == PRO_TK_NO_ERROR && agd &&
            ProLinedataGet(PRO_CURVE_DATA(agd), axis_p1, axis_p2) == PRO_TK_NO_ERROR) {
            axis_geom_ok = true;
            log_printf(g_build.log, "  RBLEND_AXIS[%s] p1=(%.2f,%.2f,%.2f) p2=(%.2f,%.2f,%.2f) ref=(%.2f,%.2f,%.2f)\n",
                       id_str.c_str(), axis_p1[0], axis_p1[1], axis_p1[2],
                       axis_p2[0], axis_p2[1], axis_p2[2],
                       ref_pt3d[0], ref_pt3d[1], ref_pt3d[2]);
        }
        if (agd) ProGeomitemdataFree(&agd);
    }

    g_build.macro_sketch_queue.push_back(std::move(sec0_job));

    double base_deg = sections[0].value("angle_deg", 0.0);
    double prev_cum_deg = base_deg;
    std::vector<double> step_degs;
    step_degs.reserve(sections.size() - 1);
    for (size_t k = 1; k < sections.size(); ++k) {
        const Json &sk = sections[k];
        double delta = 0.0;
        if (sk.contains("offset_deg")) {
            delta = sk.at("offset_deg").get<double>();
            prev_cum_deg += delta;
        } else {
            double cum = sk.at("angle_deg").get<double>();
            delta = cum - prev_cum_deg;
            prev_cum_deg = cum;
        }
        if (std::fabs(delta) < 1e-4) {
            clear_pending_macro_state();
            throw PRO_TK_BAD_INPUTS;
        }
        step_degs.push_back(delta);

        PendingSketchJob job;
        Json sk_copy = sk;
        if (!sk_copy.contains("auto_start_vertex")) sk_copy["auto_start_vertex"] = true;
        job.spec = std::move(sk_copy);
        if (axis_geom_ok && !sk.value("raw_section_coords", false)) {
            compute_rotblend_section_shift(
                sec0_s2p, axis_p1, axis_p2, ref_pt3d, prev_cum_deg - base_deg, job.preset_shift);
            job.has_preset_shift = true;
            log_printf(g_build.log, "  RBLEND_SEC_SHIFT[%s] k=%zu cum=%.3f shift=(%.4f,%.4f)\n",
                       id_str.c_str(), k, prev_cum_deg - base_deg, job.preset_shift[0], job.preset_shift[1]);
        }
        ProVector ex{1.0, 0.0, 0.0}, ey{0.0, 1.0, 0.0}, ez{0.0, 0.0, 1.0}, orig{0.0, 0.0, 0.0};
        ProMatrixInit(ex, ey, ez, orig, job.sec_to_part);
        ProMatrixInit(ex, ey, ez, orig, job.part_to_sec);
        g_build.macro_sketch_queue.push_back(std::move(job));
    }

    std::string interp = op.value("interpolation", "smooth");
    std::wstring w_interp = (interp == "straight") ? L"Straight" : L"Smooth";
    bool is_closed = op.value("closed", false);

    std::wstring macro = L"~ Command `ProCmdFtRBlend`;";
    if (g_build.pending_is_cut) {
        macro += L" ~ Activate `main_dlg_cur` `maindashInst0.Trim` 1;";
    }
    macro +=
        L" ~ Activate `main_dlg_cur` `chkbn.blend_sectionspage.0` 1;"
        L" ~ Activate `blend_sectionspage.0.0` `PH.sketchbtn`;"
        L" ~ Command `CreoMacroPickRef`;"
        L" ~ Activate `Odui_Dlg_01` `stdbtn_1`;"
        L" ~ Command `CreoMacroFillSketch`;"
        L" ~ Command `ProCmdSketDone`;"
        L" ~ Command `CreoMacroPickRef`;";
    for (size_t k = 0; k < step_degs.size(); ++k) {
        std::wstring wdeg = fmt_macro_double(step_degs[k]);
        macro +=
            L" ~ Activate `main_dlg_cur` `chkbn.blend_sectionspage.0` 1;"
            L" ~ Activate `blend_sectionspage.0.0` `PH.insertbtn`;"
            L" ~ Input `blend_sectionspage.0.0` `PH.offset_value` `" + wdeg + L"`;"
            L" ~ Activate `blend_sectionspage.0.0` `PH.offset_value`;"
            L" ~ Activate `blend_sectionspage.0.0` `PH.offset_sketch`;"
            L" ~ Command `CreoMacroFillSketch`;"
            L" ~ Command `ProCmdSketDone`;";
    }
    macro += L" ~ Activate `main_dlg_cur` `chkbn.blend_optionspage.0` 1;"
             L" ~ Select `blend_optionspage.0.0` `PH.blendoptions` 1 `" + w_interp + L"`;";
    if (is_closed) {
        macro += L" ~ Activate `blend_optionspage.0.0` `PH.openclosends` 1;";
    }
    macro += L" ~ Activate `main_dlg_cur` `dashInst0.Done`;"
             L" ~ Activate `main_dlg_cur` `dashInst0.Quit`;"
             L" ~ Activate `UI Message Dialog` `yes`;"
             L" ~ Command `CreoBuildResume`;";
    launch_dashboard_macro(macro, "RBLEND");
}

void schedule_helical_sweep(const Json &op, const std::string &id_str) {
    ProSelection plane_sel = nullptr;
    if (resolve_plane_selection(g_build.ctx, op.at("plane"), &plane_sel) != PRO_TK_NO_ERROR || !plane_sel) {
        throw PRO_TK_E_NOT_FOUND;
    }

    double pitch = op.at("pitch").get<double>();
    if (pitch <= 1e-6) {
        ProSelectionFree(&plane_sel);
        throw PRO_TK_BAD_INPUTS;
    }

    prepare_macro_op(op, id_str, "helical_sweep");
    g_build.macro_sel_queue.push_back(plane_sel);

    PendingSketchJob prof_job;
    prof_job.spec = op.at("profile");
    init_interactive_plane_xform(g_build.ctx, op.at("plane"), prof_job.part_to_sec, prof_job.plane_n, prof_job.sec_to_part);
    g_build.macro_sketch_queue.push_back(std::move(prof_job));

    PendingSketchJob sec_job;
    sec_job.spec = op.contains("section") ? op.at("section") : op.at("cross_section");
    sec_job.offset_to_crosshair = !op.value("raw_section_coords", false);
    ProVector ex{1.0, 0.0, 0.0}, ey{0.0, 1.0, 0.0}, ez{0.0, 0.0, 1.0}, orig{0.0, 0.0, 0.0};
    ProMatrixInit(ex, ey, ez, orig, sec_job.sec_to_part);
    ProMatrixInit(ex, ey, ez, orig, sec_job.part_to_sec);
    g_build.macro_sketch_queue.push_back(std::move(sec_job));

    std::string hand = op.value("handedness", "right");
    std::wstring w_hand = (hand == "left") ? L"Left" : L"Right";
    std::string orient = op.value("orientation", "through_axis");
    bool normal_orient = (orient == "normal" || orient == "normal_to_trajectory");

    std::wstring macro = L"~ Command `ProCmdFtHelicalSweep`;";
    if (g_build.pending_is_cut) {
        macro += L" ~ Activate `main_dlg_cur` `maindashInst0.Trim` 1;";
    }
    macro += L" ~ Activate `main_dlg_cur` `chkbn.hlswp_refs.0` 1;";
    macro +=
        L" ~ Activate `hlswp_refs.0.0` `PH.profile_repr_btn`;"
        L" ~ Command `CreoMacroPickRef`;"
        L" ~ Activate `Odui_Dlg_01` `stdbtn_1`;"
        L" ~ Command `CreoMacroFillSketch`;"
        L" ~ Command `ProCmdSketDone`;";
    if (normal_orient) {
        macro +=
            L" ~ Activate `main_dlg_cur` `chkbn.hlswp_refs.0` 1;"
            L" ~ Select `hlswp_refs.0.0` `PH.sket_opt` 1 `Option 2`;";
    }
    macro +=
        L" ~ Input `main_dlg_cur` `maindashInst0.const_pitch_input` `" + fmt_macro_double(pitch) + L"`;"
        L" ~ Activate `main_dlg_cur` `maindashInst0.const_pitch_input`;"
        L" ~ Select `main_dlg_cur` `maindashInst0.Hand_opt_RG` 1 `" + w_hand + L"`;"
        L" ~ Activate `main_dlg_cur` `maindashInst0.int_sketch`;"
        L" ~ Command `CreoMacroFillSketch`;"
        L" ~ Command `ProCmdSketDone`;"
        L" ~ Activate `main_dlg_cur` `dashInst0.Done`;"
        L" ~ Activate `main_dlg_cur` `dashInst0.Quit`;"
        L" ~ Activate `UI Message Dialog` `yes`;"
        L" ~ Command `CreoBuildResume`;";
    launch_dashboard_macro(macro, "HLSWP");
}

void run_build_steps() {
    PlanContext &ctx = g_build.ctx;
    FILE *log = g_build.log;
    const Json &ops = g_build.plan.at("operations");
    try {
        while (g_build.next_idx < ops.size()) {
            size_t idx = g_build.next_idx;
            const Json &op = ops[idx];
            std::string kind = op.at("op").get<std::string>();
            std::string id = op.value("id", kind + "_" + std::to_string(idx));
            log_printf(log, "OP_START idx=%zu op=%s id=%s\n", idx, kind.c_str(), id.c_str());

            if (kind == "solid_blend") {
                schedule_solid_blend(op, id);
                return;
            }
            if (kind == "rotational_blend") {
                schedule_rotational_blend(op, id);
                return;
            }
            if (kind == "helical_sweep") {
                schedule_helical_sweep(op, id);
                return;
            }
            if (kind == "set_appearance" || kind == "set_texture") {
                op_set_appearance_or_texture(ctx, op, kind == "set_texture");
                g_build.next_idx++;
                continue;
            }

            ProFeature created{};
            if (kind == "datum_plane") {
                created = op_datum_plane(ctx, op);
            } else if (kind == "datum_axis") {
                created = op_datum_axis(ctx, op);
            } else if (kind == "extrude") {
                created = op_extrude(ctx, op);
            } else if (kind == "revolve") {
                created = op_revolve(ctx, op);
            } else if (kind == "round") {
                created = op_round_or_chamfer(ctx, op, false);
            } else if (kind == "chamfer") {
                created = op_round_or_chamfer(ctx, op, true);
            } else if (kind == "feature_group") {
                created = op_feature_group(ctx, op);
            } else if (kind == "axis_pattern") {
                created = op_axis_pattern(ctx, op);
            } else if (kind == "sketched_curve") {
                created = op_sketched_curve(ctx, op);
            } else if (kind == "sweep") {
                created = op_sweep(ctx, op);
            } else if (kind == "datum_points") {
                created = op_datum_points(ctx, op);
            } else if (kind == "surface_loft") {
                created = op_surface_loft(ctx, op);
            } else if (kind == "flat_fill") {
                created = op_flat_fill(ctx, op);
            } else if (kind == "quilt_merge") {
                created = op_quilt_merge(ctx, op);
            } else if (kind == "solidify") {
                created = op_solidify(ctx, op);
            } else if (kind == "mirror") {
                created = op_mirror(ctx, op);
            } else if (kind == "assemble_component") {
                created = op_assemble_component(ctx, op);
            } else {
                log_printf(log, "UNKNOWN_OP=%s\n", kind.c_str());
                finish_build_and_close(PRO_TK_BAD_INPUTS);
                return;
            }

            ctx.features[id] = created;
            {
                ProFeatStatus fstat = PRO_FEAT_INVALID;
                if (ProFeatureStatusGet(&created, &fstat) == PRO_TK_NO_ERROR && fstat != PRO_FEAT_ACTIVE) {
                    log_printf(log, "FEAT_INACTIVE[%s] id=%d status=%d\n", id.c_str(), created.id, static_cast<int>(fstat));
                    finish_build_and_close(PRO_TK_GENERAL_ERROR);
                    return;
                }
            }
            if (op.contains("metric_tag") && op["metric_tag"].is_string()) {
                std::string mtag = op["metric_tag"].get<std::string>();
                ProError mst = log_solid_metrics(log, ctx.solid, mtag.c_str());
                if (mst != PRO_TK_NO_ERROR) {
                    log_st(log, mtag.c_str(), mst);
                    finish_build_and_close(mst);
                    return;
                }
            }
            g_build.next_idx++;
        }
        bool has_motors =
            g_build.plan.contains("motors") && g_build.plan["motors"].is_array() && !g_build.plan["motors"].empty();
        bool want_mech_analysis = g_build.plan.value("mechanism_analysis", false);
        if (g_build.ctx.is_assembly &&
            (has_motors || want_mech_analysis || g_build.plan.value("probe_mech", false)) &&
            !g_build.mech_done) {
            g_build.mech_done = true;
            ProError rgst = ProSolidRegenerate(g_build.ctx.solid, PRO_REGEN_FORCE_REGEN);
            if (g_build.win_id != PRO_VALUE_UNUSED) {
                ProWindowRepaint(g_build.win_id);
            }
            ProMdl cm_check = nullptr;
            ProError cm_st = ProMdlCurrentGet(&cm_check);
            log_printf(log, "PRE_MECH_REGEN st=%d win_id=%d cm_st=%d same_mdl=%d\n",
                       static_cast<int>(rgst), g_build.win_id, static_cast<int>(cm_st),
                       (cm_check == g_build.ctx.mdl) ? 1 : 0);
            fflush(log);
            std::wstring mm =
                L"~ Activate `main_dlg_cur` `page_Applications_control_btn` 1;"
                L" ~ Command `ProCmdUtilAppMech` 1;"
                L" ~ Command `CreoMechSetup`;";
            if (want_mech_analysis) {
                mm += L" ~ Command `PMechCmdAnalyses`;"
                      L" ~ Activate `pm_analysis_ui` `runBtn`;"
                      L" ~ Activate `pm_analysis_ui` `OKButton`;";
            }
            mm += L" ~ Command `CreoMechLogPose`;"
                  L" ~ Activate `main_dlg_cur` `page_Applications_control_btn` 1;"
                  L" ~ Command `ProCmdUtilAppMech` 0;"
                  L" ~ Command `CreoBuildFinalize`;";
            ProError mst = ProMacroLoad(const_cast<wchar_t *>(mm.c_str()));
            log_printf(log, "MECH_MACRO_LOAD st=%d\n", static_cast<int>(mst));
            fflush(log);
            return;
        }
        finish_build_and_close(PRO_TK_NO_ERROR);
    } catch (ProError err) {
        log_st(log, "op_exec", err);
        finish_build_and_close(err);
    } catch (const std::exception &ex) {
        log_printf(log, "EXCEPTION=%s\n", ex.what());
        finish_build_and_close(PRO_TK_GENERAL_ERROR);
    } catch (...) {
        log_printf(log, "EXCEPTION=unknown\n");
        finish_build_and_close(PRO_TK_GENERAL_ERROR);
    }
}

struct AsmCompLogCtx {
    FILE *log = nullptr;
    ProSolid asm_solid = nullptr;
};

static ProError visit_asm_comp_log_cb(ProFeature *p_feat, ProError status, ProAppData data) {
    auto *actx = static_cast<AsmCompLogCtx *>(data);
    ProFeattype ftype = PRO_FEAT_FIRST_FEAT;
    (void)status;
    if (ProFeatureTypeGet(p_feat, &ftype) == PRO_TK_NO_ERROR && ftype == PRO_FEAT_COMPONENT) {
        ProName wname{};
        ProModelitemNameGet(reinterpret_cast<ProModelitem *>(p_feat), wname);
        log_component_placement(actx->log, actx->asm_solid, w2utf8(wname).c_str(), p_feat);
    }
    return PRO_TK_NO_ERROR;
}

void start_build_async(FILE *log, bool use_default_names) {
    g_build = ActiveBuildState{};
    g_build.log = log;
    g_build.use_default_names = use_default_names;
    g_build.plan = load_plan_file();

    std::string doc_type = g_build.plan.value("doc_type", "part");
    bool is_asm = (doc_type == "assembly");
    ProMdlfileType mdl_ftype = is_asm ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART;
    ProType win_type = is_asm ? PRO_ASSEMBLY : PRO_PART;

    std::string part_name = g_build.plan.at("part_name").get<std::string>();
    std::string template_name = g_build.plan.value("template", is_asm ? "mmns_asm_design_abs" : "mmns_part_solid_abs");
    g_build.w_part = utf8_to_w(part_name);
    std::wstring w_tmpl = utf8_to_w(template_name);

    log_printf(log, "IMPL=c_protoolkit_msvc142_plan_driven\n");
    log_printf(log, "DOC_TYPE=%s\n", doc_type.c_str());
    log_printf(log, "PART_NAME=%s\n", part_name.c_str());
    log_printf(log, "NAME_STYLE=%s\n", use_default_names ? "creo_default" : "chinese_custom");

    ProMdl tmpl = nullptr, part_mdl = nullptr;
    ProError st = PRO_TK_NO_ERROR;
    ProMdl cur_init = nullptr;
    ProError cm_init = ProMdlCurrentGet(&cur_init);
    bool from_active_window = false;
    if (cm_init == PRO_TK_NO_ERROR && cur_init) {
        ProPath cur_name{};
        ProMdlNameGet(cur_init, cur_name);
        log_printf(log, "INIT_ACTIVE_MDL=%s\n", w2utf8(cur_name).c_str());
        if (_wcsicmp(cur_name, g_build.w_part.c_str()) != 0) {
            st = ProMdlnameRename(cur_init, const_cast<wchar_t *>(g_build.w_part.c_str()));
            if (st != PRO_TK_NO_ERROR) {
                log_st(log, "rename_active_part", st);
                finish_build_and_close(st);
                return;
            }
        }
        part_mdl = cur_init;
        from_active_window = true;
    } else {
        st = ProMdlnameRetrieve(const_cast<wchar_t *>(w_tmpl.c_str()), mdl_ftype, &tmpl);
        if (st != PRO_TK_NO_ERROR) {
            log_st(log, "retrieve_template", st);
            finish_build_and_close(st);
            return;
        }
        st = ProMdlnameCopy(tmpl, const_cast<wchar_t *>(g_build.w_part.c_str()), &part_mdl);
        ProMdlErase(tmpl);
        if (st != PRO_TK_NO_ERROR) {
            log_st(log, "copy_part", st);
            finish_build_and_close(st);
            return;
        }
    }

    ProSolid solid = reinterpret_cast<ProSolid>(part_mdl);
    int win_id = PRO_VALUE_UNUSED;
    ProError disp_st = from_active_window ? PRO_TK_NO_ERROR : ProMdlDisplay(part_mdl);
    ProError wg_st = ProMdlWindowGet(part_mdl, &win_id);
    if (!from_active_window && (wg_st != PRO_TK_NO_ERROR || win_id == PRO_VALUE_UNUSED)) {
        ProObjectwindowMdlnameCreate(const_cast<wchar_t *>(g_build.w_part.c_str()), win_type, &win_id);
        disp_st = ProMdlDisplay(part_mdl);
        wg_st = ProMdlWindowGet(part_mdl, &win_id);
    }
    ProError wcs_st = PRO_TK_NO_ERROR, wact_st = PRO_TK_NO_ERROR;
    if (win_id != PRO_VALUE_UNUSED) {
        wcs_st = ProWindowCurrentSet(win_id);
        if (!from_active_window) wact_st = ProWindowActivate(win_id);
        ProWindowRepaint(win_id);
    }
    ProMdl cur_mdl = nullptr;
    ProError cm_st = ProMdlCurrentGet(&cur_mdl);
    log_printf(log, "WIN_INIT win_id=%d disp_st=%d wg_st=%d wcs_st=%d wact_st=%d cm_st=%d same_mdl=%d tcreate_st=%d\n",
               win_id, static_cast<int>(disp_st), static_cast<int>(wg_st),
               static_cast<int>(wcs_st), static_cast<int>(wact_st),
               static_cast<int>(cm_st), (cur_mdl == part_mdl) ? 1 : 0,
               static_cast<int>(g_timer_create_st));
    g_build.win_id = win_id;

    ProUnitsystem usys{};
    if (ProMdlPrincipalunitsystemGet(part_mdl, &usys) == PRO_TK_NO_ERROR) {
        log_printf(log, "UNITS=%s\n", w2utf8(usys.name).c_str());
    }

    g_build.ctx.mdl = part_mdl;
    g_build.ctx.solid = solid;
    g_build.ctx.log = log;
    g_build.ctx.use_default_names = use_default_names;
    g_build.ctx.is_assembly = is_asm;
    g_build.next_idx = 0;

    if (g_step_timer) {
        char dlg_name[] = "main_dlg_cur";
        ProError tst = ProUIDialogTimerStart(dlg_name, g_step_timer, 400, PRO_B_FALSE);
        log_printf(log, "STEP_TIMER_START st=%d\n", static_cast<int>(tst));
        if (tst == PRO_TK_NO_ERROR) return;
    }
    ProMacro step_macro;
    ProStringToWstring(step_macro, const_cast<char *>("~ Command `CreoBuildStep` ;"));
    ProMacroLoad(step_macro);
}

ProError run_verify(FILE *log) {
    Json plan = load_plan_file();
    std::string doc_type = plan.value("doc_type", "part");
    bool is_asm = (doc_type == "assembly");
    ProMdlfileType mdl_ftype = is_asm ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART;
    ProType win_type = is_asm ? PRO_ASSEMBLY : PRO_PART;

    std::string part_name = plan.at("part_name").get<std::string>();
    std::wstring w_part = utf8_to_w(part_name);

    ProMdl part_mdl = nullptr;
    ProError st = PRO_TK_NO_ERROR;
    bool from_active_win = false;
    ProMdl cur_mdl = nullptr;
    if (ProMdlCurrentGet(&cur_mdl) == PRO_TK_NO_ERROR && cur_mdl) {
        ProPath cur_name{};
        ProMdlNameGet(cur_mdl, cur_name);
        if (_wcsicmp(cur_name, w_part.c_str()) == 0) {
            part_mdl = cur_mdl;
            from_active_win = true;
        }
    }
    if (!part_mdl) {
        st = ProMdlnameRetrieve(const_cast<wchar_t *>(w_part.c_str()), mdl_ftype, &part_mdl);
        if (st != PRO_TK_NO_ERROR) return log_st(log, "verify_retrieve", st);
    }

    ProSolid solid = reinterpret_cast<ProSolid>(part_mdl);
    int win_id = PRO_VALUE_UNUSED;
    if (from_active_win) {
        ProMdlWindowGet(part_mdl, &win_id);
    } else {
        ProObjectwindowMdlnameCreate(const_cast<wchar_t *>(w_part.c_str()), win_type, &win_id);
        ProMdlDisplay(part_mdl);
    }
    if (win_id != PRO_VALUE_UNUSED) {
        ProWindowCurrentSet(win_id);
        if (!from_active_win) ProWindowActivate(win_id);
        ProWindowRepaint(win_id);
    }

    st = ProSolidRegenerate(solid, PRO_REGEN_FORCE_REGEN);
    if (st != PRO_TK_NO_ERROR) return log_st(log, "force_regen", st);

    ProUnitsystem usys{};
    if (ProMdlPrincipalunitsystemGet(part_mdl, &usys) == PRO_TK_NO_ERROR) {
        log_printf(log, "UNITS=%s\n", w2utf8(usys.name).c_str());
    }
    if ((st = log_solid_metrics(log, solid, "REOPEN_REGEN")) != PRO_TK_NO_ERROR) return st;
    if ((st = log_feature_tree(log, solid, 1)) != PRO_TK_NO_ERROR) return st;
    if (is_asm) {
        AsmCompLogCtx actx{log, solid};
        ProSolidFeatVisit(solid, visit_asm_comp_log_cb, nullptr, &actx);
    }
    fflush(log);
    if ((st = ProOutputFileMdlnameWrite(
             part_mdl, const_cast<wchar_t *>(L"verify_reopen.inf"), PRO_MODEL_INFO, nullptr, nullptr, nullptr, nullptr)) !=
        PRO_TK_NO_ERROR) {
        return log_st(log, "verify_inf", st);
    }

    if (plan.contains("operations") && plan["operations"].is_array()) {
        std::set<std::string> checked_tex;
        for (const Json &op : plan["operations"]) {
            if (op.value("op", "") != "set_texture" || !op.contains("texture_file")) continue;
            std::string tf = op["texture_file"].get<std::string>();
            if (!checked_tex.insert(tf).second) continue;
            std::wstring w_tf = utf8_to_w(tf);
            ProName tname{};
            wcsncpy_s(tname, PRO_NAME_SIZE, w_tf.c_str(), _TRUNCATE);
            ProBoolean was_emb = PRO_B_FALSE;
            ProError tst = ProTexturePathGet(tname, PRO_B_FALSE, &was_emb, nullptr);
            log_printf(log, "TEX_EMBED[%s] st=%d embedded=%d\n", tf.c_str(), static_cast<int>(tst), static_cast<int>(was_emb));
            if (tst != PRO_TK_NO_ERROR || was_emb != PRO_B_TRUE) {
                return (tst != PRO_TK_NO_ERROR) ? tst : PRO_TK_E_NOT_FOUND;
            }
        }
    }

    if (win_id != PRO_VALUE_UNUSED) {
        ProWindowCurrentSet(win_id);
        ProWindowActivate(win_id);
        ProSelbufferClear();
        DatumDisplayGuard guard;
        if (plan.value("hide_datums_in_views", false)) guard.hide();
        export_view_jpg(part_mdl, win_id, L"DEFAULT", L"verify_default.jpg");
    }

    log_printf(log, "REOPEN_VERIFIED=true\n");
    return PRO_TK_NO_ERROR;
}

int creo_mech_setup_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id; (void)p_val; (void)appdata;
    if (!g_build.finished && g_build.log) {
        try {
            run_mech_setup_impl(g_build.log);
        } catch (ProError err) {
            log_st(g_build.log, "mech_setup", err);
            finish_build_and_close(err);
        } catch (const std::exception &ex) {
            log_printf(g_build.log, "MECH_SETUP_EXCEPTION=%s\n", ex.what());
            finish_build_and_close(PRO_TK_GENERAL_ERROR);
        } catch (...) {
            log_printf(g_build.log, "MECH_SETUP_EXCEPTION=unknown\n");
            finish_build_and_close(PRO_TK_GENERAL_ERROR);
        }
    }
    return 0;
}

int creo_mech_log_pose_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id; (void)p_val; (void)appdata;
    if (!g_build.finished && g_build.log) {
        run_mech_log_pose_impl(g_build.log);
    }
    return 0;
}

int creo_build_finalize_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id; (void)p_val; (void)appdata;
    finish_build_and_close(PRO_TK_NO_ERROR);
    return 0;
}

uiCmdAccessState access_always(uiCmdAccessMode access_mode) {
    (void)access_mode;
    return ACCESS_AVAILABLE;
}

int creo_blend_pick_sec_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id;
    (void)p_val;
    (void)appdata;
    if (g_build.blend_pick_idx >= g_build.blend_curve_ids.size()) return 0;
    int cid = g_build.blend_curve_ids[g_build.blend_pick_idx++];
    ProModelitem item{};
    ProSelection sel = nullptr;
    ProError st = ProModelitemInit(g_build.ctx.mdl, cid, PRO_CURVE, &item);
    if (st == PRO_TK_NO_ERROR) st = ProSelectionAlloc(nullptr, &item, &sel);
    if (st == PRO_TK_NO_ERROR && sel) {
        st = ProSelectExternalselectionRecord(sel, PRO_SELECT_OVERRIDE);
    }
    log_printf(g_build.log, "  BLEND_PICK idx=%zu cid=%d st=%d\n",
               g_build.blend_pick_idx - 1, cid, static_cast<int>(st));
    if (sel) ProSelectionFree(&sel);
    return 0;
}

int creo_macro_pick_ref_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id;
    (void)p_val;
    (void)appdata;
    if (g_build.finished || g_build.macro_sel_idx >= g_build.macro_sel_queue.size()) return 0;
    ProSelection sel = g_build.macro_sel_queue[g_build.macro_sel_idx++];
    ProError st = sel ? ProSelectExternalselectionRecord(sel, PRO_SELECT_OVERRIDE) : PRO_TK_BAD_INPUTS;
    log_printf(g_build.log, "  MACRO_PICK_REF[%s] idx=%zu st=%d\n",
               g_build.pending_id.c_str(), g_build.macro_sel_idx - 1, static_cast<int>(st));
    return 0;
}

int creo_macro_fill_sketch_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id;
    (void)p_val;
    (void)appdata;
    if (g_build.finished || g_build.macro_sketch_idx >= g_build.macro_sketch_queue.size()) return 0;
    const PendingSketchJob &job = g_build.macro_sketch_queue[g_build.macro_sketch_idx++];
    ProSection sec = nullptr;
    ProError gst = ProSectionActiveGet(&sec);
    if (gst != PRO_TK_NO_ERROR || !sec) {
        log_printf(g_build.log, "  MACRO_SKET_GET_FAIL[%s] idx=%zu st=%d\n",
                   g_build.pending_id.c_str(), g_build.macro_sketch_idx - 1, static_cast<int>(gst));
        return 0;
    }
    double shift[2]{0.0, 0.0};
    bool use_shift = false;
    if (job.offset_to_crosshair) {
        use_shift = find_active_sec_crosshair(sec, shift[0], shift[1]);
    } else if (job.has_preset_shift) {
        shift[0] = job.preset_shift[0];
        shift[1] = job.preset_shift[1];
        use_shift = true;
    }
    double eps = job.spec.value("epsilon", 0.01);
    ProSectionEpsilonSet(sec, eps);
    ProSectionIntentManagerModeSet(sec, PRO_B_TRUE);
    ProError pst = populate_section_from_json(
        sec,
        const_cast<ProMatrix &>(job.part_to_sec),
        const_cast<ProMatrix &>(job.sec_to_part),
        job.plane_n,
        job.spec,
        g_build.log,
        use_shift ? shift : nullptr);
    ProError sst = (pst == PRO_TK_NO_ERROR) ? ProSectionActiveSet(sec) : pst;
    log_printf(g_build.log, "  MACRO_SKET[%s] idx=%zu pop_st=%d set_st=%d shift=(%.3f,%.3f)\n",
               g_build.pending_id.c_str(), g_build.macro_sketch_idx - 1,
               static_cast<int>(pst), static_cast<int>(sst), shift[0], shift[1]);
    ProSectionFree(&sec);
    return 0;
}

ProError log_win_visit_cb(int window_id, ProAppData data) {
    FILE *log = static_cast<FILE *>(data);
    ProMdl wm = nullptr;
    ProError st = ProWindowMdlGet(window_id, &wm);
    ProPath mname{};
    if (st == PRO_TK_NO_ERROR && wm) ProMdlNameGet(wm, mname);
    log_printf(log, "  OPEN_WIN id=%d mdl_st=%d name=%s\n",
               window_id, static_cast<int>(st), wm ? w2utf8(mname).c_str() : "(none)");
    return PRO_TK_NO_ERROR;
}

void step_timer_cb(char *dialog, ProUITimerID timer_id, ProAppData appdata) {
    (void)dialog;
    (void)timer_id;
    (void)appdata;
    if (g_build.finished || !g_build.log) return;
    log_printf(g_build.log, "STEP_TIMER_FIRED\n");
    ProMacro step_macro;
    ProStringToWstring(step_macro, const_cast<char *>("~ Command `CreoBuildStep` ;"));
    ProMacroLoad(step_macro);
}

int creo_build_step_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id;
    (void)p_val;
    (void)appdata;
    if (g_build.finished || !g_build.log) return 0;
    int cur_w = -1;
    ProWindowCurrentGet(&cur_w);
    ProMdl cur_m = nullptr;
    ProError cm_st = ProMdlCurrentGet(&cur_m);
    uiCmdCmdId blend_cmd = nullptr, ext_cmd = nullptr;
    ProError bf_st = ProCmdCmdIdFind(const_cast<char *>("ProCmdFtPBlend"), &blend_cmd);
    ProError ef_st = ProCmdCmdIdFind(const_cast<char *>("ProCmdFtExtrude"), &ext_cmd);
    log_printf(g_build.log, "STEP_CB cur_win=%d cm_st=%d same_mdl=%d blend_find=%d(%p) ext_find=%d(%p)\n",
               cur_w, static_cast<int>(cm_st), (cur_m == g_build.ctx.mdl) ? 1 : 0,
               static_cast<int>(bf_st), blend_cmd, static_cast<int>(ef_st), ext_cmd);
    ProWindowsVisit(log_win_visit_cb, g_build.log);
    run_build_steps();
    return 0;
}

int creo_build_resume_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id;
    (void)p_val;
    (void)appdata;
    if (g_build.finished || !g_build.log) return 0;

    std::vector<int> after_ids;
    ProSolidFeatVisit(g_build.ctx.solid, collect_all_feat_ids_cb, nullptr, &after_ids);
    ProFeattype want_type = g_build.pending_is_cut ? PRO_FEAT_CUT : PRO_FEAT_PROTRUSION;
    ProFeature created{};
    bool found = false;
    for (int fid : after_ids) {
        if (g_build.pre_blend_feat_ids.count(fid)) continue;
        ProFeature f{};
        ProFeattype ftype = PRO_FEAT_FIRST_FEAT;
        if (ProFeatureInit(g_build.ctx.solid, fid, &f) == PRO_TK_NO_ERROR &&
            ProFeatureTypeGet(&f, &ftype) == PRO_TK_NO_ERROR &&
            (ftype == want_type || ftype == PRO_FEAT_PROTRUSION || ftype == PRO_FEAT_CUT)) {
            created = f;
            found = true;
            break;
        }
    }
    if (!found) {
        log_printf(g_build.log, "MACRO_OP_NOT_CREATED[%s/%s]\n", g_build.pending_op.c_str(), g_build.pending_id.c_str());
        finish_build_and_close(PRO_TK_GENERAL_ERROR);
        return 0;
    }

    ProFeatStatus fstat = PRO_FEAT_INVALID;
    ProFeatureStatusGet(&created, &fstat);
    if (fstat != PRO_FEAT_ACTIVE) {
        log_printf(g_build.log, "MACRO_OP_INACTIVE[%s/%s] status=%d\n",
                   g_build.pending_op.c_str(), g_build.pending_id.c_str(), static_cast<int>(fstat));
        finish_build_and_close(PRO_TK_GENERAL_ERROR);
        return 0;
    }

    if (g_build.pending_is_cut && g_build.pending_verify_cut) {
        double vol_after = 0.0;
        Pro3dPnt box_tmp[2]{};
        if (measure_solid_volume(g_build.ctx.solid, &vol_after, nullptr, box_tmp) == PRO_TK_NO_ERROR) {
            double removed = g_build.pending_vol_before - vol_after;
            log_printf(g_build.log, "CUT_VOL_REMOVED[%s]=%.6f\n", g_build.pending_id.c_str(), removed);
            if (removed <= 1e-6) {
                log_printf(g_build.log, "ERROR_CUT_NO_MATERIAL[%s]\n", g_build.pending_id.c_str());
                finish_build_and_close(PRO_TK_GENERAL_ERROR);
                return 0;
            }
        }
    }

    if (!g_build.pending_name.empty()) {
        ProName fname{};
        wcsncpy_s(fname, PRO_NAME_SIZE, g_build.pending_name.c_str(), _TRUNCATE);
        ProModelitemNameSet(reinterpret_cast<ProModelitem *>(&created), fname);
    }

    g_build.ctx.features[g_build.pending_id] = created;
    log_printf(g_build.log, "MACRO_OP_OK[%s/%s] feat_id=%d\n",
               g_build.pending_op.c_str(), g_build.pending_id.c_str(), created.id);
    if (!g_build.pending_metric_tag.empty()) {
        ProError mst = log_solid_metrics(g_build.log, g_build.ctx.solid, g_build.pending_metric_tag.c_str());
        if (mst != PRO_TK_NO_ERROR) {
            finish_build_and_close(mst);
            return 0;
        }
    }
    clear_pending_macro_state();
    g_build.next_idx++;
    run_build_steps();
    return 0;
}

int creo_build_exec_cb(uiCmdCmdId cmd_id, uiCmdValue *p_val, void *appdata) {
    (void)cmd_id;
    (void)p_val;
    (void)appdata;

    if (g_has_run) return 0;
    g_has_run = 1;

    bool is_verify = false, is_anim = false, use_default_names = false;
    read_mode(is_verify, is_anim, use_default_names);
    (void)is_anim;
    const char *report_name = is_verify ? "verify_result.txt" : "build_report.txt";
    FILE *log = fopen(report_name, "w");
    if (!log) return -1;

    if (is_verify) {
        ProError st = PRO_TK_GENERAL_ERROR;
        try {
            st = run_verify(log);
        } catch (ProError err) {
            st = err;
        } catch (const std::exception &ex) {
            log_printf(log, "EXCEPTION=%s\n", ex.what());
            st = PRO_TK_GENERAL_ERROR;
        } catch (...) {
            log_printf(log, "EXCEPTION=unknown\n");
            st = PRO_TK_GENERAL_ERROR;
        }
        if (st == PRO_TK_NO_ERROR) {
            log_printf(log, "STATUS=OK\n");
        } else {
            log_printf(log, "STATUS=ERROR\nERROR_CODE=%d\n", static_cast<int>(st));
        }
        fclose(log);
        return 0;
    }

    try {
        start_build_async(log, use_default_names);
    } catch (ProError err) {
        log_st(log, "start_build", err);
        finish_build_and_close(err);
    } catch (const std::exception &ex) {
        log_printf(log, "EXCEPTION=%s\n", ex.what());
        finish_build_and_close(PRO_TK_GENERAL_ERROR);
    } catch (...) {
        log_printf(log, "EXCEPTION=unknown\n");
        finish_build_and_close(PRO_TK_GENERAL_ERROR);
    }
    return 0;
}

} // namespace

extern "C" int user_initialize(
    int argc,
    char *argv[],
    char *version,
    char *build,
    wchar_t errbuf[PRO_PATH_SIZE]
) {
    (void)argc;
    (void)argv;
    (void)version;
    (void)build;
    (void)errbuf;

    uiCmdCmdId cmd_id = 0, pick_id = 0, mpick_id = 0, msket_id = 0, res_id = 0, step_id = 0, mechp_id = 0, mechl_id = 0, fin_id = 0;
    ProMacro wmacro;
    ProCmdActionAdd(
        const_cast<char *>("CreoBlendPickSec"),
        reinterpret_cast<uiCmdCmdActFn>(creo_blend_pick_sec_cb),
        uiProeAsynch,
        access_always,
        PRO_B_FALSE,
        PRO_B_FALSE,
        &pick_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoMacroPickRef"),
        reinterpret_cast<uiCmdCmdActFn>(creo_macro_pick_ref_cb),
        uiProeAsynch,
        access_always,
        PRO_B_FALSE,
        PRO_B_FALSE,
        &mpick_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoMacroFillSketch"),
        reinterpret_cast<uiCmdCmdActFn>(creo_macro_fill_sketch_cb),
        uiProeAsynch,
        access_always,
        PRO_B_FALSE,
        PRO_B_FALSE,
        &msket_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoBuildStep"),
        reinterpret_cast<uiCmdCmdActFn>(creo_build_step_cb),
        uiProe2ndImmediate,
        access_always,
        PRO_B_TRUE,
        PRO_B_TRUE,
        &step_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoBuildResume"),
        reinterpret_cast<uiCmdCmdActFn>(creo_build_resume_cb),
        uiProe2ndImmediate,
        access_always,
        PRO_B_TRUE,
        PRO_B_TRUE,
        &res_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoMechSetup"),
        reinterpret_cast<uiCmdCmdActFn>(creo_mech_setup_cb),
        uiProe2ndImmediate,
        access_always,
        PRO_B_TRUE,
        PRO_B_TRUE,
        &mechp_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoMechLogPose"),
        reinterpret_cast<uiCmdCmdActFn>(creo_mech_log_pose_cb),
        uiProe2ndImmediate,
        access_always,
        PRO_B_TRUE,
        PRO_B_TRUE,
        &mechl_id);
    ProCmdActionAdd(
        const_cast<char *>("CreoBuildFinalize"),
        reinterpret_cast<uiCmdCmdActFn>(creo_build_finalize_cb),
        uiProe2ndImmediate,
        access_always,
        PRO_B_TRUE,
        PRO_B_TRUE,
        &fin_id);
    ProError st = ProCmdActionAdd(
        const_cast<char *>("CreoBuildExec"),
        reinterpret_cast<uiCmdCmdActFn>(creo_build_exec_cb),
        uiProe2ndImmediate,
        access_always,
        PRO_B_TRUE,
        PRO_B_TRUE,
        &cmd_id);
    ProName tname = L"CreoBuildStepTimer";
    g_timer_create_st = ProUITimerCreate(step_timer_cb, nullptr, tname, &g_step_timer);
    if (st == PRO_TK_NO_ERROR) {
        ProCmdDesignate(
            cmd_id,
            const_cast<char *>("CreoBuild AutoModeler (C++ Pro/TOOLKIT)"),
            const_cast<char *>("CreoBuild AutoModeler Help"),
            const_cast<char *>("CreoBuild AutoModeler Help"),
            const_cast<wchar_t *>(L"msg_creobuild.txt"));
        ProStringToWstring(wmacro, const_cast<char *>("~ Command `CreoBuildExec` ;"));
        ProMacroLoad(wmacro);
    }
    return 0;
}

extern "C" void user_terminate(void) {
}
