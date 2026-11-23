#pragma once
/*
 * Creo Parametric 8.0 C++17 Pro/TOOLKIT plan execution core helpers.
 *
 * Encapsulates low-level ProElement tree construction, rigid section transforms,
 * 2D/3D constrained sketch population (Intent Manager + autodim + regenerate),
 * topological plane/axis/edge resolution, and verification/export utilities.
 */

#include <ProToolkit.h>
#include <Pro2dEntdef.h>
#include <ProAsmcomp.h>
#include <ProAsmcomppath.h>
#include <ProAssembly.h>
#include <ProAxis.h>
#include <ProBodyOpts.h>
#include <ProBooleanBodies.h>
#include <ProChamfer.h>
#include <ProCollect.h>
#include <ProContour.h>
#include <ProCore.h>
#include <ProCrvcollection.h>
#include <ProCurve.h>
#include <ProCurvedata.h>
#include <ProDtmAxis.h>
#include <ProDtmCrv.h>
#include <ProDtmPln.h>
#include <ProDtmPnt.h>
#include <ProEdge.h>
#include <ProElemId.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProExtrude.h>
#include <ProFeatForm.h>
#include <ProFeatType.h>
#include <ProFeature.h>
#include <ProFlatSrf.h>
#include <ProGeomitem.h>
#include <ProGeomitemdata.h>
#include <ProGroup.h>
#include <ProIntf3Dexport.h>
#include <ProLayer.h>
#include <ProMdl.h>
#include <ProMdlUnits.h>
#include <ProMenu.h>
#include <ProMenuBar.h>
#include <ProMerge.h>
#include <ProMirror.h>
#include <ProModelitem.h>
#include <ProObjects.h>
#include <ProPattern.h>
#include <ProPoint.h>
#include <ProQuilt.h>
#include <ProRevolve.h>
#include <ProRound.h>
#include <ProSecConstr.h>
#include <ProSecdim.h>
#include <ProSecdimType.h>
#include <ProSecerror.h>
#include <ProSection.h>
#include <ProSelection.h>
#include <ProSolid.h>
#include <ProSolidBody.h>
#include <ProSolidify.h>
#include <ProStdSection.h>
#include <ProSurface.h>
#include <ProSurfacedata.h>
#include <ProSweep.h>
#include <ProTKRunTime.h>
#include <ProThicken.h>
#include <ProUICmd.h>
#include <ProUtil.h>
#include <ProView.h>
#include <ProWindows.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "third_party/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace creobuild {

using Json = nlohmann::json;

struct PlanContext {
    ProMdl mdl = nullptr;
    ProSolid solid = nullptr;
    FILE *log = nullptr;
    bool use_default_names = false;
    bool is_assembly = false;
    std::map<std::string, ProFeature> features;
    std::map<std::string, std::vector<int>> group_member_ids;
    std::map<std::string, ProAsmcomp> components;
    std::map<std::string, int> quilt_ids;
};

/* ------------------------------------------------------------------ */
/* String encoding and logging                                        */
/* ------------------------------------------------------------------ */

inline void log_printf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    if (!fp) return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
}

inline std::string w2utf8(const wchar_t *ws) {
    if (!ws || !ws[0]) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &out[0], needed, nullptr, nullptr);
    return out;
}

inline std::wstring utf8_to_w(const std::string &s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], needed);
    return out;
}

inline ProError log_st(FILE *log, const char *step, ProError st) {
    if (st != PRO_TK_NO_ERROR) {
        log_printf(log, "STEP_FAIL[%s] st=%d\n", step, static_cast<int>(st));
    }
    return st;
}

inline void log_errorlist(FILE *fp, const char *tag, ProErrorlist *errs) {
    if (!fp || !errs || errs->error_number <= 0 || !errs->error_list) return;
    for (int i = 0; i < errs->error_number; ++i) {
        log_printf(
            fp,
            "FEAT_ERR[%s] idx=%d err_item_id=%d type=%d err=%d\n",
            tag,
            i,
            errs->error_list[i].err_item_id,
            static_cast<int>(errs->error_list[i].err_item_type),
            static_cast<int>(errs->error_list[i].error));
    }
}

inline std::wstring resolve_feat_name(const PlanContext &ctx, const Json &op) {
    if (ctx.use_default_names) return {};
    if (op.contains("name_zh") && op["name_zh"].is_string()) {
        return utf8_to_w(op["name_zh"].get<std::string>());
    }
    if (op.contains("name") && op["name"].is_string()) {
        return utf8_to_w(op["name"].get<std::string>());
    }
    return {};
}

/* ------------------------------------------------------------------ */
/* Element tree helpers                                               */
/* ------------------------------------------------------------------ */

inline ProError elem_add_compound(ProElement parent, ProElemId id, ProElement *out) {
    ProElement e = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    if (parent) {
        st = ProElemtreeElementAdd(parent, nullptr, e);
        if (st != PRO_TK_NO_ERROR) return st;
    }
    if (out) *out = e;
    return PRO_TK_NO_ERROR;
}

inline ProError elem_add_int(ProElement parent, ProElemId id, int val) {
    ProElement e = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProElementIntegerSet(e, val);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElemtreeElementAdd(parent, nullptr, e);
}

inline ProError elem_add_double(ProElement parent, ProElemId id, double val) {
    ProElement e = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    ProElementDecimalsSet(e, 6);
    st = ProElementDoubleSet(e, val);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElemtreeElementAdd(parent, nullptr, e);
}

inline ProError elem_add_wstr(ProElement parent, ProElemId id, const wchar_t *val) {
    if (!val || !val[0]) return PRO_TK_NO_ERROR;
    ProElement e = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProElementWstringSet(e, const_cast<wchar_t *>(val));
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElemtreeElementAdd(parent, nullptr, e);
}

inline ProError elem_add_ref(ProElement parent, ProElemId id, ProSelection sel) {
    ProElement e = nullptr;
    ProReference ref = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProSelectionToReference(sel, &ref);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProElementReferenceSet(e, ref);
    ProReferenceFree(ref);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElemtreeElementAdd(parent, nullptr, e);
}

inline ProError get_elem_by_path(ProElement root, const ProElemId *ids, int n, ProElement *out) {
    ProElempath path = nullptr;
    ProElempathItem items[8];
    if (n <= 0 || n > 8) return PRO_TK_BAD_INPUTS;
    for (int i = 0; i < n; ++i) {
        items[i].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
        items[i].path_item.elem_id = ids[i];
    }
    ProError st = ProElempathAlloc(&path);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProElempathDataSet(path, items, n);
    if (st == PRO_TK_NO_ERROR) {
        st = ProElemtreeElementGet(root, path, out);
    }
    ProElempathFree(&path);
    return st;
}

inline ProError set_int_by_path(ProElement root, const ProElemId *ids, int n, int val) {
    ProElement el = nullptr;
    ProError st = get_elem_by_path(root, ids, n, &el);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElementIntegerSet(el, val);
}

/* ------------------------------------------------------------------ */
/* Geometry reference, plane, axis, body, and edge resolvers          */
/* ------------------------------------------------------------------ */

inline ProError find_datum_plane_sel(ProMdl mdl, const wchar_t *name, ProSelection *out_sel) {
    ProModelitem item;
    ProError st = ProModelitemByNameInit(mdl, PRO_SURFACE, const_cast<wchar_t *>(name), &item);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProSelectionAlloc(nullptr, &item, out_sel);
}

inline ProError find_default_body_sel(ProSolid solid, ProSelection *out_sel) {
    ProSolidBody body;
    ProError st = ProSolidDefaultBodyGet(solid, &body);
    *out_sel = nullptr;
    if (st != PRO_TK_NO_ERROR || body.id == PRO_VALUE_UNUSED) {
        return PRO_TK_E_NOT_FOUND;
    }
    return ProSelectionAlloc(nullptr, reinterpret_cast<ProModelitem *>(&body), out_sel);
}

struct GeomFindCtx {
    ProGeomitem item;
    int found = 0;
};

inline ProError visit_first_geom_action(ProGeomitem *p_item, ProError status, ProAppData data) {
    auto *ctx = static_cast<GeomFindCtx *>(data);
    (void)status;
    ctx->item = *p_item;
    ctx->found = 1;
    return PRO_TK_USER_ABORT;
}

inline ProError find_feat_geom_sel(ProFeature *feat, ProType gtype, ProSelection *out_sel) {
    GeomFindCtx ctx{};
    ProError st = ProFeatureGeomitemVisit(feat, gtype, visit_first_geom_action, nullptr, &ctx);
    if (!ctx.found) {
        return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    }
    return ProSelectionAlloc(nullptr, &ctx.item, out_sel);
}

struct TopPlaneFindCtx {
    double target_z = 0.0;
    ProSurface surf = nullptr;
    int found = 0;
};

inline ProError visit_plane_z_action(ProSurface surf, ProError status, ProAppData data) {
    auto *ctx = static_cast<TopPlaneFindCtx *>(data);
    ProGeomitemdata *gdata = nullptr;
    (void)status;
    if (ProSurfaceDataGet(surf, &gdata) == PRO_TK_NO_ERROR && gdata != nullptr) {
        ProSurfacedata *srf = PRO_SURF_DATA(gdata);
        if (srf && srf->type == PRO_SRF_PLANE) {
            double nz = std::fabs(srf->srf_shape.plane.e3[2]);
            double oz = srf->srf_shape.plane.origin[2];
            if (nz > 0.999 && std::fabs(oz - ctx->target_z) < 1e-3) {
                ctx->surf = surf;
                ctx->found = 1;
                ProGeomitemdataFree(&gdata);
                return PRO_TK_USER_ABORT;
            }
        }
        ProGeomitemdataFree(&gdata);
    }
    return PRO_TK_NO_ERROR;
}

inline ProError find_planar_face_at_z(ProSolid solid, double z, ProSelection *out_sel) {
    ProSolidBody body;
    TopPlaneFindCtx ctx;
    ProModelitem item;
    if (ProSolidDefaultBodyGet(solid, &body) != PRO_TK_NO_ERROR || body.id == PRO_VALUE_UNUSED) {
        return PRO_TK_E_NOT_FOUND;
    }
    ctx.target_z = z;
    ProSolidBodySurfaceVisit(&body, visit_plane_z_action, &ctx);
    if (!ctx.found || !ctx.surf) return PRO_TK_E_NOT_FOUND;
    if (ProSurfaceToGeomitem(solid, ctx.surf, &item) != PRO_TK_NO_ERROR) return PRO_TK_E_NOT_FOUND;
    return ProSelectionAlloc(nullptr, &item, out_sel);
}

inline ProError resolve_plane_selection(PlanContext &ctx, const Json &ref_spec, ProSelection *out_sel) {
    *out_sel = nullptr;
    if (ref_spec.is_string()) {
        std::string key = ref_spec.get<std::string>();
        auto it = ctx.features.find(key);
        if (it != ctx.features.end()) {
            return find_feat_geom_sel(&it->second, PRO_SURFACE, out_sel);
        }
        std::wstring wname = utf8_to_w(key);
        return find_datum_plane_sel(ctx.mdl, wname.c_str(), out_sel);
    }
    if (ref_spec.is_object()) {
        if (ref_spec.contains("plane_at_z")) {
            double z = ref_spec["plane_at_z"].get<double>();
            ProError st = find_planar_face_at_z(ctx.solid, z, out_sel);
            if (st == PRO_TK_NO_ERROR) return st;
            if (ref_spec.contains("fallback")) {
                return resolve_plane_selection(ctx, ref_spec["fallback"], out_sel);
            }
            return st;
        }
        if (ref_spec.contains("datum")) {
            std::wstring wname = utf8_to_w(ref_spec["datum"].get<std::string>());
            return find_datum_plane_sel(ctx.mdl, wname.c_str(), out_sel);
        }
        if (ref_spec.contains("feature")) {
            std::string id = ref_spec["feature"].get<std::string>();
            auto it = ctx.features.find(id);
            if (it == ctx.features.end()) return PRO_TK_E_NOT_FOUND;
            return find_feat_geom_sel(&it->second, PRO_SURFACE, out_sel);
        }
    }
    return PRO_TK_BAD_INPUTS;
}

inline ProError resolve_axis_selection(PlanContext &ctx, const Json &ref_spec, ProSelection *out_sel) {
    *out_sel = nullptr;
    if (ref_spec.is_string()) {
        std::string key = ref_spec.get<std::string>();
        auto it = ctx.features.find(key);
        if (it != ctx.features.end()) {
            return find_feat_geom_sel(&it->second, PRO_AXIS, out_sel);
        }
        ProModelitem item;
        std::wstring wname = utf8_to_w(key);
        ProError st = ProModelitemByNameInit(ctx.mdl, PRO_AXIS, const_cast<wchar_t *>(wname.c_str()), &item);
        if (st == PRO_TK_NO_ERROR) return ProSelectionAlloc(nullptr, &item, out_sel);
        return st;
    }
    if (ref_spec.is_object() && ref_spec.contains("feature")) {
        std::string key = ref_spec["feature"].get<std::string>();
        auto it = ctx.features.find(key);
        if (it == ctx.features.end()) return PRO_TK_E_NOT_FOUND;
        return find_feat_geom_sel(&it->second, PRO_AXIS, out_sel);
    }
    return PRO_TK_BAD_INPUTS;
}

inline ProError resolve_csys_selection(PlanContext &ctx, const Json &ref_spec, ProSelection *out_sel) {
    *out_sel = nullptr;
    std::string key = ref_spec.is_string() ? ref_spec.get<std::string>() : "PRT_CSYS_DEF";
    auto it = ctx.features.find(key);
    if (it != ctx.features.end()) {
        return find_feat_geom_sel(&it->second, PRO_CSYS, out_sel);
    }
    ProModelitem item;
    std::wstring wname = utf8_to_w(key);
    ProError st = ProModelitemByNameInit(ctx.mdl, PRO_CSYS, const_cast<wchar_t *>(wname.c_str()), &item);
    if (st == PRO_TK_NO_ERROR) return ProSelectionAlloc(nullptr, &item, out_sel);
    return st;
}

/* Collect all non-composite curve geomitem IDs owned by a sketch/curve feature */
struct CurveCollectCtx {
    ProSolid solid = nullptr;
    std::vector<int> ids;
};

inline ProError visit_noncomposite_curve_action(ProGeomitem *item, ProError status, ProAppData data) {
    auto *cctx = static_cast<CurveCollectCtx *>(data);
    (void)status;
    if (!item || item->type != PRO_CURVE) return PRO_TK_NO_ERROR;
    ProCurve crv = nullptr;
    ProEnttype kind = PRO_ENT_NONE;
    if (ProCurveInit(cctx->solid, item->id, &crv) == PRO_TK_NO_ERROR &&
        ProCurveTypeGet(crv, &kind) == PRO_TK_NO_ERROR &&
        kind != PRO_ENT_CMP_CRV) {
        cctx->ids.push_back(item->id);
    }
    return PRO_TK_NO_ERROR;
}

inline ProError collect_feat_noncomposite_curves(
    ProSolid solid,
    ProFeature *feat,
    std::vector<int> &out_ids
) {
    CurveCollectCtx cctx{solid, {}};
    ProError st = ProFeatureGeomitemVisit(feat, PRO_CURVE, visit_noncomposite_curve_action, nullptr, &cctx);
    out_ids = std::move(cctx.ids);
    return out_ids.empty() ? (st == PRO_TK_NO_ERROR ? PRO_TK_E_NOT_FOUND : st) : PRO_TK_NO_ERROR;
}

struct GeomIdListCtx {
    std::vector<int> ids;
};

inline ProError visit_geom_id_action(ProGeomitem *item, ProError status, ProAppData data) {
    (void)status;
    if (item) static_cast<GeomIdListCtx *>(data)->ids.push_back(item->id);
    return PRO_TK_NO_ERROR;
}

inline ProError resolve_quilt_id(PlanContext &ctx, const Json &ref_spec, int *out_qid) {
    *out_qid = -1;
    if (ref_spec.is_number_integer()) {
        *out_qid = ref_spec.get<int>();
        return PRO_TK_NO_ERROR;
    }
    std::string key;
    if (ref_spec.is_string()) {
        key = ref_spec.get<std::string>();
    } else if (ref_spec.is_object() && ref_spec.contains("feature")) {
        key = ref_spec["feature"].get<std::string>();
    } else if (ref_spec.is_object() && ref_spec.contains("quilt_id")) {
        *out_qid = ref_spec["quilt_id"].get<int>();
        return PRO_TK_NO_ERROR;
    } else {
        return PRO_TK_BAD_INPUTS;
    }

    auto qit = ctx.quilt_ids.find(key);
    if (qit != ctx.quilt_ids.end() && qit->second >= 0) {
        *out_qid = qit->second;
        return PRO_TK_NO_ERROR;
    }
    auto fit = ctx.features.find(key);
    if (fit == ctx.features.end()) return PRO_TK_E_NOT_FOUND;

    GeomIdListCtx qlist;
    ProFeatureGeomitemVisit(&fit->second, PRO_QUILT, visit_geom_id_action, nullptr, &qlist);
    if (qlist.ids.size() == 1) {
        *out_qid = qlist.ids[0];
        return PRO_TK_NO_ERROR;
    }
    GeomIdListCtx slist;
    ProFeatureGeomitemVisit(&fit->second, PRO_SURFACE, visit_geom_id_action, nullptr, &slist);
    for (int sid : slist.ids) {
        ProSurface srf = nullptr;
        ProQuilt q = nullptr;
        int qid = -1;
        if (ProSurfaceInit(ctx.mdl, sid, &srf) == PRO_TK_NO_ERROR &&
            ProSurfaceQuiltGet(ctx.solid, srf, &q) == PRO_TK_NO_ERROR &&
            q && ProQuiltIdGet(q, &qid) == PRO_TK_NO_ERROR && qid >= 0) {
            *out_qid = qid;
            return PRO_TK_NO_ERROR;
        }
    }
    return PRO_TK_E_NOT_FOUND;
}

inline ProError resolve_quilt_selection(PlanContext &ctx, const Json &ref_spec, ProSelection *out_sel) {
    *out_sel = nullptr;
    int qid = -1;
    ProError st = resolve_quilt_id(ctx, ref_spec, &qid);
    if (st != PRO_TK_NO_ERROR || qid < 0) return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    ProModelitem item{};
    st = ProModelitemInit(ctx.mdl, qid, PRO_QUILT, &item);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProSelectionAlloc(nullptr, &item, out_sel);
}

inline ProError quilt_contour_area_cb(ProContour contour, ProError status, ProAppData data) {
    auto *acc = static_cast<std::pair<ProSurface, double> *>(data);
    if (status) return PRO_TK_NO_ERROR;
    ProContourTraversal trav = PRO_CONTOUR_TRAV_EXTERNAL;
    double area = 0.0;
    if (ProContourTraversalGet(contour, &trav) == PRO_TK_NO_ERROR &&
        ProContourAreaEval(acc->first, contour, &area) == PRO_TK_NO_ERROR) {
        acc->second += (trav == PRO_CONTOUR_TRAV_EXTERNAL) ? area : -area;
    }
    return PRO_TK_NO_ERROR;
}

inline ProError quilt_surface_area_cb(ProSurface surf, ProError status, ProAppData data) {
    if (status) return PRO_TK_NO_ERROR;
    std::pair<ProSurface, double> s_acc{surf, 0.0};
    ProSurfaceContourVisit(surf, quilt_contour_area_cb, nullptr, &s_acc);
    *static_cast<double *>(data) += s_acc.second;
    return PRO_TK_NO_ERROR;
}

inline ProError eval_quilt_area(ProSolid solid, int quilt_id, double *out_area) {
    *out_area = 0.0;
    ProQuilt q = nullptr;
    ProError st = ProQuiltInit(solid, quilt_id, &q);
    if (st != PRO_TK_NO_ERROR || !q) return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    double total = 0.0;
    st = ProQuiltSurfaceVisit(q, quilt_surface_area_cb, nullptr, &total);
    *out_area = total;
    return st;
}

inline ProError build_curve_collection_from_ids(
    ProMdl mdl,
    const std::vector<int> &curve_ids,
    ProCollection *out_coll,
    ProCrvcollinstrType instr_type = PRO_CURVCOLL_CURVE_INSTR
) {
    *out_coll = nullptr;
    ProCollection coll = nullptr;
    ProError st = ProCrvcollectionAlloc(&coll);
    if (st != PRO_TK_NO_ERROR) return st;
    for (int cid : curve_ids) {
        ProModelitem item{};
        ProSelection sel = nullptr;
        ProReference ref = nullptr;
        ProCrvcollinstr instr = nullptr;
        st = ProModelitemInit(mdl, cid, PRO_CURVE, &item);
        if (st == PRO_TK_NO_ERROR) st = ProSelectionAlloc(nullptr, &item, &sel);
        if (st == PRO_TK_NO_ERROR) st = ProSelectionToReference(sel, &ref);
        if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrAlloc(instr_type, &instr);
        if (st == PRO_TK_NO_ERROR && instr_type == PRO_CURVCOLL_CURVE_INSTR) {
            st = ProCrvcollinstrAttributeSet(instr, PRO_CURVCOLL_NO_ATTR);
        }
        if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrReferenceAdd(instr, ref);
        if (st == PRO_TK_NO_ERROR) st = ProCrvcollectionInstructionAdd(coll, instr);
        if (instr) ProCrvcollinstrFree(instr);
        if (ref) ProReferenceFree(ref);
        if (sel) ProSelectionFree(&sel);
        if (st != PRO_TK_NO_ERROR) {
            ProCollectionFree(&coll);
            return st;
        }
    }
    *out_coll = coll;
    return PRO_TK_NO_ERROR;
}

inline ProError build_point_collection(ProMdl mdl, int point_id, ProCollection *out_coll) {
    *out_coll = nullptr;
    ProCollection coll = nullptr;
    ProCrvcollinstr instr = nullptr;
    ProModelitem item{};
    ProSelection sel = nullptr;
    ProReference ref = nullptr;
    ProError st = ProCrvcollectionAlloc(&coll);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrAlloc(PRO_CURVCOLL_ADD_POINT_INSTR, &instr);
    if (st == PRO_TK_NO_ERROR) st = ProModelitemInit(mdl, point_id, PRO_POINT, &item);
    if (st == PRO_TK_NO_ERROR) st = ProSelectionAlloc(nullptr, &item, &sel);
    if (st == PRO_TK_NO_ERROR) st = ProSelectionToReference(sel, &ref);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollinstrReferenceAdd(instr, ref);
    if (st == PRO_TK_NO_ERROR) st = ProCrvcollectionInstructionAdd(coll, instr);
    if (instr) ProCrvcollinstrFree(instr);
    if (ref) ProReferenceFree(ref);
    if (sel) ProSelectionFree(&sel);
    if (st != PRO_TK_NO_ERROR) {
        if (coll) ProCollectionFree(&coll);
        return st;
    }
    *out_coll = coll;
    return PRO_TK_NO_ERROR;
}

inline ProError add_body_elements(ProElement tree, ProSolid solid, int attach_to_existing) {
    ProElement body_elem = nullptr;
    ProSelection body_sel = nullptr;
    ProError st = elem_add_compound(tree, PRO_E_BODY, &body_elem);
    if (st != PRO_TK_NO_ERROR) return st;
    if (attach_to_existing && find_default_body_sel(solid, &body_sel) == PRO_TK_NO_ERROR && body_sel) {
        st = elem_add_int(body_elem, PRO_E_BODY_USE, PRO_BODY_USE_SELECTED);
        if (st == PRO_TK_NO_ERROR) st = elem_add_ref(body_elem, PRO_E_BODY_SELECT, body_sel);
        ProSelectionFree(&body_sel);
        return st;
    }
    return elem_add_int(body_elem, PRO_E_BODY_USE, PRO_BODY_USE_NEW);
}

inline ProError add_section_setup_plane(
    ProElement sec_elem,
    ProSelection sketch_plane,
    ProSelection orient_ref
) {
    ProElement setup = nullptr;
    ProError st = elem_add_compound(sec_elem, PRO_E_STD_SEC_SETUP_PLANE, &setup);
    if (st != PRO_TK_NO_ERROR) return st;
    st = elem_add_ref(setup, PRO_E_STD_SEC_PLANE, sketch_plane);
    if (st != PRO_TK_NO_ERROR) return st;
    st = elem_add_int(setup, PRO_E_STD_SEC_PLANE_VIEW_DIR, PRO_SEC_VIEW_DIR_SIDE_ONE);
    if (st != PRO_TK_NO_ERROR) return st;
    st = elem_add_int(setup, PRO_E_STD_SEC_PLANE_ORIENT_DIR, PRO_SEC_ORIENT_DIR_UP);
    if (st != PRO_TK_NO_ERROR) return st;
    return elem_add_ref(setup, PRO_E_STD_SEC_PLANE_ORIENT_REF, orient_ref);
}

/* Edge enumeration and geometric matching for Round / Chamfer */
struct EdgeCandidate {
    int id = -1;
    ProEdge handle = nullptr;
    double p0[3]{};
    double pm[3]{};
    double p1[3]{};
    double length = 0.0;
};

struct EdgeCollectState {
    ProSolid solid = nullptr;
    ProSurface current_surf = nullptr;
    std::set<int> seen_ids;
    std::vector<EdgeCandidate> edges;
};

inline void append_edge_candidate(EdgeCollectState *state, ProEdge edge) {
    int id = -1;
    if (!edge || ProEdgeIdGet(edge, &id) != PRO_TK_NO_ERROR) return;
    if (!state->seen_ids.insert(id).second) return;

    EdgeCandidate ec;
    ec.id = id;
    ec.handle = edge;
    ProVector d1{}, d2{}, dir{};
    if (ProEdgeXyzdataEval(edge, 0.0, ec.p0, d1, d2, dir) != PRO_TK_NO_ERROR) return;
    if (ProEdgeXyzdataEval(edge, 0.5, ec.pm, d1, d2, dir) != PRO_TK_NO_ERROR) return;
    if (ProEdgeXyzdataEval(edge, 1.0, ec.p1, d1, d2, dir) != PRO_TK_NO_ERROR) return;
    ProEdgeLengthEval(edge, &ec.length);
    state->edges.push_back(ec);
}

inline ProError visit_edge_action(ProEdge edge, ProError status, ProAppData data) {
    (void)status;
    append_edge_candidate(static_cast<EdgeCollectState *>(data), edge);
    return PRO_TK_NO_ERROR;
}

inline ProError visit_contour_action(ProContour contour, ProError status, ProAppData data) {
    auto *state = static_cast<EdgeCollectState *>(data);
    (void)status;
    ProContourEdgeVisit(state->current_surf, contour, visit_edge_action, nullptr, data);
    return PRO_TK_NO_ERROR;
}

inline ProError visit_surface_edges_action(ProSurface surf, ProError status, ProAppData data) {
    auto *state = static_cast<EdgeCollectState *>(data);
    (void)status;
    state->current_surf = surf;
    ProSurfaceContourVisit(surf, visit_contour_action, nullptr, data);
    return PRO_TK_NO_ERROR;
}

inline ProError visit_feat_edge_geom_action(ProGeomitem *item, ProError status, ProAppData data) {
    auto *state = static_cast<EdgeCollectState *>(data);
    (void)status;
    if (item && item->type == PRO_EDGE) {
        ProEdge edge = nullptr;
        if (ProEdgeInit(state->solid, item->id, &edge) == PRO_TK_NO_ERROR) {
            append_edge_candidate(state, edge);
        }
    }
    return PRO_TK_NO_ERROR;
}

inline double dist3(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline double radial_xy(const double p[3]) {
    return std::sqrt(p[0] * p[0] + p[1] * p[1]);
}

inline bool edge_matches_rule(const EdgeCandidate &e, const Json &rule) {
    double tol = rule.value("tol", 1e-2);

    if (rule.contains("z_range")) {
        double zmin = rule["z_range"].at(0).get<double>() - tol;
        double zmax = rule["z_range"].at(1).get<double>() + tol;
        for (const double *p : {e.p0, e.pm, e.p1}) {
            if (p[2] < zmin || p[2] > zmax) return false;
        }
    }
    if (rule.contains("r_range")) {
        double rmin = rule["r_range"].at(0).get<double>() - tol;
        double rmax = rule["r_range"].at(1).get<double>() + tol;
        for (const double *p : {e.p0, e.pm, e.p1}) {
            double r = radial_xy(p);
            if (r < rmin || r > rmax) return false;
        }
    }
    if (rule.contains("x_range")) {
        double xmin = rule["x_range"].at(0).get<double>() - tol;
        double xmax = rule["x_range"].at(1).get<double>() + tol;
        for (const double *p : {e.p0, e.pm, e.p1}) {
            if (p[0] < xmin || p[0] > xmax) return false;
        }
    }
    if (rule.contains("y_range")) {
        double ymin = rule["y_range"].at(0).get<double>() - tol;
        double ymax = rule["y_range"].at(1).get<double>() + tol;
        for (const double *p : {e.p0, e.pm, e.p1}) {
            if (p[1] < ymin || p[1] > ymax) return false;
        }
    }
    if (rule.contains("min_r")) {
        double mr = rule["min_r"].get<double>() - tol;
        if (radial_xy(e.pm) < mr) return false;
    }
    if (rule.contains("max_r")) {
        double mr = rule["max_r"].get<double>() + tol;
        if (radial_xy(e.pm) > mr) return false;
    }
    if (rule.contains("len_range")) {
        double lmin = rule["len_range"].at(0).get<double>() - tol;
        double lmax = rule["len_range"].at(1).get<double>() + tol;
        if (e.length < lmin || e.length > lmax) return false;
    }
    if (rule.contains("near")) {
        double target[3] = {
            rule["near"].at(0).get<double>(),
            rule["near"].at(1).get<double>(),
            rule["near"].at(2).get<double>()
        };
        if (dist3(e.p0, target) > tol && dist3(e.pm, target) > tol && dist3(e.p1, target) > tol) {
            return false;
        }
    }
    if (rule.contains("mid_near")) {
        double target[3] = {
            rule["mid_near"].at(0).get<double>(),
            rule["mid_near"].at(1).get<double>(),
            rule["mid_near"].at(2).get<double>()
        };
        if (dist3(e.pm, target) > tol) return false;
    }
    if (rule.contains("endpoints_match")) {
        double a[3] = {
            rule["endpoints_match"].at(0).at(0).get<double>(),
            rule["endpoints_match"].at(0).at(1).get<double>(),
            rule["endpoints_match"].at(0).at(2).get<double>()
        };
        double b[3] = {
            rule["endpoints_match"].at(1).at(0).get<double>(),
            rule["endpoints_match"].at(1).at(1).get<double>(),
            rule["endpoints_match"].at(1).at(2).get<double>()
        };
        bool fwd = dist3(e.p0, a) <= tol && dist3(e.p1, b) <= tol;
        bool rev = dist3(e.p0, b) <= tol && dist3(e.p1, a) <= tol;
        if (!fwd && !rev) return false;
    }
    return true;
}

inline ProError collect_matching_edges(
    PlanContext &ctx,
    const Json &rules,
    std::vector<ProSelection> &out_sels,
    const char *tag
) {
    out_sels.clear();
    std::set<int> added_ids;

    for (size_t ri = 0; ri < rules.size(); ++ri) {
        const Json &rule = rules[ri];
        EdgeCollectState state;
        state.solid = ctx.solid;

        if (rule.contains("feature")) {
            std::string fkey = rule["feature"].get<std::string>();
            auto it = ctx.features.find(fkey);
            if (it == ctx.features.end()) {
                log_printf(ctx.log, "EDGE_MATCH[%s] missing feature '%s'\n", tag, fkey.c_str());
                return PRO_TK_E_NOT_FOUND;
            }
            EdgeCollectState body_state;
            body_state.solid = ctx.solid;
            ProSolidBody body{};
            if (ProSolidDefaultBodyGet(ctx.solid, &body) == PRO_TK_NO_ERROR && body.id != PRO_VALUE_UNUSED) {
                ProSolidBodySurfaceVisit(&body, visit_surface_edges_action, &body_state);
            }
            EdgeCollectState feat_state;
            feat_state.solid = ctx.solid;
            ProFeatureGeomitemVisit(&it->second, PRO_EDGE, visit_feat_edge_geom_action, nullptr, &feat_state);
            for (const auto &ec : feat_state.edges) {
                if (body_state.seen_ids.empty() || body_state.seen_ids.count(ec.id)) {
                    state.edges.push_back(ec);
                    state.seen_ids.insert(ec.id);
                }
            }
        } else {
            ProSolidBody body{};
            if (ProSolidDefaultBodyGet(ctx.solid, &body) == PRO_TK_NO_ERROR && body.id != PRO_VALUE_UNUSED) {
                ProSolidBodySurfaceVisit(&body, visit_surface_edges_action, &state);
            }
        }

        int matched_this_rule = 0;
        for (const auto &ec : state.edges) {
            if (!edge_matches_rule(ec, rule)) continue;
            matched_this_rule++;
            if (added_ids.insert(ec.id).second) {
                ProModelitem item{};
                if (ProEdgeToGeomitem(ctx.solid, ec.handle, &item) == PRO_TK_NO_ERROR) {
                    ProSelection sel = nullptr;
                    if (ProSelectionAlloc(nullptr, &item, &sel) == PRO_TK_NO_ERROR && sel) {
                        out_sels.push_back(sel);
                    }
                }
            }
        }
        log_printf(
            ctx.log,
            "EDGE_MATCH[%s] rule=%zu candidates=%zu matched=%d\n",
            tag,
            ri,
            state.edges.size(),
            matched_this_rule);
        if (rule.contains("expect_count")) {
            int exp_c = rule["expect_count"].get<int>();
            if (matched_this_rule != exp_c) {
                log_printf(ctx.log, "EDGE_MATCH_FAIL[%s] expected=%d got=%d\n", tag, exp_c, matched_this_rule);
                for (const auto &ec : state.edges) {
                    if (edge_matches_rule(ec, rule)) {
                        log_printf(
                            ctx.log,
                            "  MATCHED_EDGE id=%d p0=(%.2f,%.2f,%.2f) pm=(%.2f,%.2f,%.2f) p1=(%.2f,%.2f,%.2f) r_mid=%.2f len=%.2f\n",
                            ec.id,
                            ec.p0[0], ec.p0[1], ec.p0[2],
                            ec.pm[0], ec.pm[1], ec.pm[2],
                            ec.p1[0], ec.p1[1], ec.p1[2],
                            radial_xy(ec.pm),
                            ec.length);
                    }
                }
                return PRO_TK_E_NOT_FOUND;
            }
        }
        if (matched_this_rule == 0) {
            return PRO_TK_E_NOT_FOUND;
        }
    }
    return out_sels.empty() ? PRO_TK_E_NOT_FOUND : PRO_TK_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* Section coordinate transform, 2D/3D entities, constraints, solving */
/* ------------------------------------------------------------------ */

inline void rigid_matrix_invert(ProMatrix m, ProMatrix inv) {
    memset(inv, 0, sizeof(ProMatrix));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            inv[r][c] = m[c][r];
        }
    }
    inv[3][0] = -(m[3][0] * inv[0][0] + m[3][1] * inv[1][0] + m[3][2] * inv[2][0]);
    inv[3][1] = -(m[3][0] * inv[0][1] + m[3][1] * inv[1][1] + m[3][2] * inv[2][1]);
    inv[3][2] = -(m[3][0] * inv[0][2] + m[3][1] * inv[1][2] + m[3][2] * inv[2][2]);
    inv[3][3] = 1.0;
}

inline ProError sec_xform_init(
    ProSection sec,
    ProMatrix part_to_sec,
    double out_plane_n[3] = nullptr,
    ProMatrix out_sec_to_part = nullptr
) {
    ProMatrix sec_to_part;
    ProError st = ProSectionLocationGet(sec, sec_to_part);
    if (st != PRO_TK_NO_ERROR) return st;
    if (out_sec_to_part) {
        memcpy(out_sec_to_part, sec_to_part, sizeof(ProMatrix));
    }
    if (out_plane_n) {
        out_plane_n[0] = sec_to_part[2][0];
        out_plane_n[1] = sec_to_part[2][1];
        out_plane_n[2] = sec_to_part[2][2];
    }
    rigid_matrix_invert(sec_to_part, part_to_sec);
    return PRO_TK_NO_ERROR;
}

inline void to_sec2d(ProMatrix part_to_sec, const double p3[3], Pro2dPnt out) {
    ProVector in_p{p3[0], p3[1], p3[2]};
    ProVector out_p{};
    ProPntTrfEval(in_p, part_to_sec, out_p);
    out[0] = out_p[0];
    out[1] = out_p[1];
}

inline double norm_ang_0_2pi(double a) {
    while (a < 0.0) a += 2.0 * M_PI;
    while (a >= 2.0 * M_PI) a -= 2.0 * M_PI;
    return a;
}

struct SketchEntityCache {
    Pro2dEntType type = PRO_2D_ENT_LAST;
    Pro2dPnt p_start{};
    Pro2dPnt p_end{};
    Pro2dPnt p_mid{};
    Pro2dPnt p_center{};
    bool valid = false;
};

struct SketchEntityBook {
    std::map<std::string, int> ids;
    std::map<int, bool> arc_swapped;
    std::map<int, SketchEntityCache> geom;
};

inline ProSectionPointType parse_point_type(const std::string &s) {
    if (s == "start") return PRO_ENT_START;
    if (s == "end") return PRO_ENT_END;
    if (s == "center") return PRO_ENT_CENTER;
    return PRO_ENT_WHOLE;
}

inline ProSectionPointType map_logical_point(
    const SketchEntityBook &book,
    int ent_id,
    ProSectionPointType pt
) {
    if (pt != PRO_ENT_START && pt != PRO_ENT_END) return pt;
    auto it = book.arc_swapped.find(ent_id);
    if (it != book.arc_swapped.end() && it->second) {
        return (pt == PRO_ENT_START) ? PRO_ENT_END : PRO_ENT_START;
    }
    return pt;
}

/* Exact 2D selection coordinate helper avoids arc-center origin collisions & unsafe pre-set entity queries */
inline ProError sec_entity_pick_sel(
    ProSection sec,
    const SketchEntityBook &book,
    int id,
    ProSectionPointType logical_pt,
    ProSelection *out_sel
) {
    ProSectionPointType pt = map_logical_point(book, id, logical_pt);
    auto git = book.geom.find(id);
    if (git != book.geom.end() && git->second.valid) {
        const SketchEntityCache &g = git->second;
        Pro2dPnt p{};
        if (pt == PRO_ENT_START) {
            p[0] = g.p_start[0]; p[1] = g.p_start[1];
        } else if (pt == PRO_ENT_END) {
            p[0] = g.p_end[0]; p[1] = g.p_end[1];
        } else if (pt == PRO_ENT_CENTER) {
            p[0] = g.p_center[0]; p[1] = g.p_center[1];
        } else {
            p[0] = g.p_mid[0]; p[1] = g.p_mid[1];
        }
        return ProSectionEntityGetSelected(sec, id, pt, p, PRO_VALUE_UNUSED, out_sel);
    }

    Pro2dEntdef *def = nullptr;
    ProError st = ProSectionEntityGet(sec, id, &def);
    if (st != PRO_TK_NO_ERROR || !def) {
        return ProSectionEntityGetSelected(sec, id, pt, nullptr, PRO_VALUE_UNUSED, out_sel);
    }

    Pro2dPnt p{};
    bool have_p = false;
    if (def->type == PRO_2D_LINE || def->type == PRO_2D_CENTER_LINE) {
        auto *line = reinterpret_cast<Pro2dLinedef *>(def);
        if (pt == PRO_ENT_START) {
            p[0] = line->end1[0];
            p[1] = line->end1[1];
        } else if (pt == PRO_ENT_END) {
            p[0] = line->end2[0];
            p[1] = line->end2[1];
        } else {
            p[0] = 0.5 * (line->end1[0] + line->end2[0]);
            p[1] = 0.5 * (line->end1[1] + line->end2[1]);
        }
        have_p = true;
    } else if (def->type == PRO_2D_CIRCLE) {
        auto *c = reinterpret_cast<Pro2dCircledef *>(def);
        p[0] = (pt == PRO_ENT_CENTER) ? c->center[0] : (c->center[0] + c->radius);
        p[1] = c->center[1];
        have_p = true;
    } else if (def->type == PRO_2D_ARC) {
        auto *a = reinterpret_cast<Pro2dArcdef *>(def);
        if (pt == PRO_ENT_CENTER) {
            p[0] = a->center[0];
            p[1] = a->center[1];
        } else {
            double ang = (pt == PRO_ENT_START)
                             ? a->start_angle
                             : (pt == PRO_ENT_END ? a->end_angle : 0.5 * (a->start_angle + a->end_angle));
            p[0] = a->center[0] + a->radius * std::cos(ang);
            p[1] = a->center[1] + a->radius * std::sin(ang);
        }
        have_p = true;
    }
    return ProSectionEntityGetSelected(sec, id, pt, have_p ? p : nullptr, PRO_VALUE_UNUSED, out_sel);
}

inline ProError sec_add_arc_3d(
    ProSection sec,
    ProMatrix inv,
    const double c3[3],
    const double s3[3],
    const double e3[3],
    const double m3[3],
    double r,
    int *out_id,
    bool *out_swapped,
    SketchEntityCache *out_cache = nullptr
) {
    Pro2dArcdef def{};
    def.type = PRO_2D_ARC;
    Pro2dPnt c2, s2, e2, m2;
    to_sec2d(inv, c3, c2);
    to_sec2d(inv, s3, s2);
    to_sec2d(inv, e3, e2);
    to_sec2d(inv, m3, m2);

    double a1 = norm_ang_0_2pi(std::atan2(s2[1] - c2[1], s2[0] - c2[0]));
    double a2 = norm_ang_0_2pi(std::atan2(e2[1] - c2[1], e2[0] - c2[0]));
    double am = norm_ang_0_2pi(std::atan2(m2[1] - c2[1], m2[0] - c2[0]));

    if (a2 <= a1) a2 += 2.0 * M_PI;
    double mid_a = 0.5 * (a1 + a2);
    double d1 = std::fabs(norm_ang_0_2pi(mid_a - am));
    if (d1 > M_PI) d1 = 2.0 * M_PI - d1;
    bool swapped = false;
    if (d1 > M_PI * 0.5) {
        double tmp = a1;
        a1 = norm_ang_0_2pi(a2);
        a2 = norm_ang_0_2pi(tmp);
        if (a2 <= a1) a2 += 2.0 * M_PI;
        swapped = true;
    }

    if (out_swapped) *out_swapped = swapped;
    def.center[0] = c2[0];
    def.center[1] = c2[1];
    def.radius = r;
    def.start_angle = a1;
    def.end_angle = a2;
    ProError st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), out_id);
    if (st == PRO_TK_NO_ERROR && out_cache) {
        out_cache->type = PRO_2D_ARC;
        out_cache->p_center[0] = c2[0]; out_cache->p_center[1] = c2[1];
        out_cache->p_start[0] = c2[0] + r * std::cos(a1);
        out_cache->p_start[1] = c2[1] + r * std::sin(a1);
        out_cache->p_end[0] = c2[0] + r * std::cos(a2);
        out_cache->p_end[1] = c2[1] + r * std::sin(a2);
        double mid_phys = 0.5 * (a1 + a2);
        out_cache->p_mid[0] = c2[0] + r * std::cos(mid_phys);
        out_cache->p_mid[1] = c2[1] + r * std::sin(mid_phys);
        out_cache->valid = true;
    }
    return st;
}

/* Extract average center of construction crosshair segments present in interactive Sketcher */
inline bool find_active_sec_crosshair(ProSection sec, double &out_cx, double &out_cy) {
    out_cx = 0.0;
    out_cy = 0.0;
    int *ids = nullptr, n = 0;
    if (ProSectionEntityIdsGet(sec, &ids, &n) != PRO_TK_NO_ERROR || !ids) return false;
    double sx = 0.0, sy = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        Pro2dEntdef *def = nullptr;
        ProBoolean is_c = PRO_B_FALSE, is_p = PRO_B_FALSE;
        ProSectionEntityIsConstruction(sec, ids[i], &is_c);
        ProSectionEntityIsProjection(sec, ids[i], &is_p);
        if (is_c == PRO_B_TRUE && is_p == PRO_B_FALSE &&
            ProSectionEntityGet(sec, ids[i], &def) == PRO_TK_NO_ERROR && def &&
            (def->type == PRO_2D_LINE || def->type == PRO_2D_CENTER_LINE)) {
            auto *ln = reinterpret_cast<Pro2dLinedef *>(def);
            sx += 0.5 * (ln->end1[0] + ln->end2[0]);
            sy += 0.5 * (ln->end1[1] + ln->end2[1]);
            cnt++;
        }
    }
    ProArrayFree(reinterpret_cast<ProArray *>(&ids));
    if (cnt == 0) return false;
    out_cx = sx / static_cast<double>(cnt);
    out_cy = sy / static_cast<double>(cnt);
    return true;
}

/* Extract rotational-blend projected axis offset shift for rotated sections */
inline bool find_rotblend_axis_shift(
    ProSection sec,
    bool axis_horiz,
    double ref_axis_coord0,
    double &out_su,
    double &out_sv
) {
    out_su = 0.0;
    out_sv = 0.0;
    int *ids = nullptr, n = 0;
    if (ProSectionEntityIdsGet(sec, &ids, &n) != PRO_TK_NO_ERROR || !ids) return false;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass) {
        for (int i = 0; i < n; ++i) {
            Pro2dEntdef *def = nullptr;
            ProBoolean is_c = PRO_B_FALSE, is_p = PRO_B_FALSE;
            ProSectionEntityIsConstruction(sec, ids[i], &is_c);
            ProSectionEntityIsProjection(sec, ids[i], &is_p);
            if (ProSectionEntityGet(sec, ids[i], &def) != PRO_TK_NO_ERROR || !def) continue;
            bool match_pass0 = (pass == 0 && is_p == PRO_B_TRUE && def->type == PRO_2D_LINE);
            bool match_pass1 = (pass == 1 && is_c == PRO_B_TRUE && is_p == PRO_B_FALSE && def->type == PRO_2D_CENTER_LINE);
            if (!match_pass0 && !match_pass1) continue;
            auto *ln = reinterpret_cast<Pro2dLinedef *>(def);
            double dx = std::fabs(ln->end1[0] - ln->end2[0]);
            double dy = std::fabs(ln->end1[1] - ln->end2[1]);
            if (axis_horiz && dy < 1e-4 && dx > 1e-2) {
                out_sv = ln->end1[1] - ref_axis_coord0;
                found = true;
                break;
            }
            if (!axis_horiz && dx < 1e-4 && dy > 1e-2) {
                out_su = ln->end1[0] - ref_axis_coord0;
                found = true;
                break;
            }
        }
    }
    ProArrayFree(reinterpret_cast<ProArray *>(&ids));
    return found;
}

/* Analytically compute Sketcher 2D origin shift on rotated RBlend Sec k without querying Sec k entities */
inline void compute_rotblend_section_shift(
    const ProMatrix sec0_to_part,
    const double axis_p1[3],
    const double axis_p2[3],
    const double ref_pt3d[3],
    double cum_deg,
    double out_shift[2]
) {
    out_shift[0] = 0.0;
    out_shift[1] = 0.0;
    auto dot3 = [](const double a[3], const double b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    auto cross3 = [](const double a[3], const double b[3], double o[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    };

    double ax_dir[3]{axis_p2[0] - axis_p1[0], axis_p2[1] - axis_p1[1], axis_p2[2] - axis_p1[2]};
    double ax_len = std::sqrt(dot3(ax_dir, ax_dir));
    if (ax_len < 1e-9) return;
    for (int i = 0; i < 3; ++i) ax_dir[i] /= ax_len;

    double eu0[3]{sec0_to_part[0][0], sec0_to_part[0][1], sec0_to_part[0][2]};
    double ev0[3]{sec0_to_part[1][0], sec0_to_part[1][1], sec0_to_part[1][2]};
    double en0[3]{sec0_to_part[2][0], sec0_to_part[2][1], sec0_to_part[2][2]};

    /* Creo anchors Sec 0's 2D origin at the orthogonal projection of (0,0,0) onto plane 0 */
    double p_on[3]{sec0_to_part[3][0], sec0_to_part[3][1], sec0_to_part[3][2]};
    double d_plane = dot3(p_on, en0);
    double orig0[3]{d_plane * en0[0], d_plane * en0[1], d_plane * en0[2]};

    /* Orient axis_dir so +cum_deg sweeps ref_pt3d into +en0 side of sketch plane 0 */
    double diff_ref[3]{ref_pt3d[0] - axis_p1[0], ref_pt3d[1] - axis_p1[1], ref_pt3d[2] - axis_p1[2]};
    double proj_ax = dot3(diff_ref, ax_dir);
    double r_vec[3]{
        diff_ref[0] - proj_ax * ax_dir[0],
        diff_ref[1] - proj_ax * ax_dir[1],
        diff_ref[2] - proj_ax * ax_dir[2]
    };
    if (dot3(r_vec, r_vec) < 1e-9) {
        for (int i = 0; i < 3; ++i) r_vec[i] = orig0[i] - axis_p1[i];
        double p2 = dot3(r_vec, ax_dir);
        for (int i = 0; i < 3; ++i) r_vec[i] -= p2 * ax_dir[i];
    }
    double sweep_dir[3]{};
    cross3(ax_dir, r_vec, sweep_dir);
    if (dot3(sweep_dir, en0) < 0.0) {
        for (int i = 0; i < 3; ++i) ax_dir[i] = -ax_dir[i];
    }

    double rad = cum_deg * M_PI / 180.0;
    double c = std::cos(rad), s = std::sin(rad);
    auto rot_vec = [&](const double v[3], double o[3]) {
        double kv[3]{};
        cross3(ax_dir, v, kv);
        double kd = dot3(ax_dir, v);
        for (int i = 0; i < 3; ++i) {
            o[i] = v[i] * c + kv[i] * s + ax_dir[i] * kd * (1.0 - c);
        }
    };

    double rel0[3]{orig0[0] - axis_p1[0], orig0[1] - axis_p1[1], orig0[2] - axis_p1[2]};
    double relk[3]{}, origk[3]{}, euk[3]{}, evk[3]{};
    rot_vec(rel0, relk);
    for (int i = 0; i < 3; ++i) origk[i] = axis_p1[i] + relk[i];
    rot_vec(eu0, euk);
    rot_vec(ev0, evk);

    /* Creo places Sec k's 2D origin at the orthogonal projection of (0,0,0) onto plane k */
    out_shift[0] = dot3(origk, euk) - dot3(orig0, eu0);
    out_shift[1] = dot3(origk, evk) - dot3(orig0, ev0);
}

/* Calibrated Sketcher 2D<->3D plane frame for interactive dashboard sections */
inline ProError init_interactive_plane_xform(
    PlanContext &ctx,
    const Json &plane_spec,
    ProMatrix part_to_sec,
    double plane_n[3],
    ProMatrix sec_to_part
) {
    double vx[3]{1.0, 0.0, 0.0}, vy[3]{0.0, 1.0, 0.0}, vz[3]{0.0, 0.0, 1.0}, orig[3]{0.0, 0.0, 0.0};
    bool matched = false;
    if (plane_spec.is_string()) {
        std::string nm = plane_spec.get<std::string>();
        if (_stricmp(nm.c_str(), "FRONT") == 0) {
            matched = true;
        } else if (_stricmp(nm.c_str(), "TOP") == 0) {
            vy[0] = 0.0; vy[1] = 0.0; vy[2] = -1.0;
            vz[0] = 0.0; vz[1] = 1.0; vz[2] = 0.0;
            matched = true;
        } else if (_stricmp(nm.c_str(), "RIGHT") == 0) {
            vx[0] = 0.0; vx[1] = -1.0; vx[2] = 0.0;
            vy[0] = 0.0; vy[1] = 0.0; vy[2] = -1.0;
            vz[0] = 1.0; vz[1] = 0.0; vz[2] = 0.0;
            matched = true;
        }
    }
    if (!matched) {
        ProSelection psel = nullptr;
        if (resolve_plane_selection(ctx, plane_spec, &psel) == PRO_TK_NO_ERROR && psel) {
            ProModelitem mi{};
            ProSurface srf = nullptr;
            ProGeomitemdata *gd = nullptr;
            if (ProSelectionModelitemGet(psel, &mi) == PRO_TK_NO_ERROR &&
                ProSurfaceInit(ctx.mdl, mi.id, &srf) == PRO_TK_NO_ERROR && srf &&
                ProSurfaceDataGet(srf, &gd) == PRO_TK_NO_ERROR && gd) {
                ProSurfacedata *sd = PRO_SURF_DATA(gd);
                if (sd && sd->type == PRO_SRF_PLANE) {
                    const double *e3 = sd->srf_shape.plane.e3;
                    const double *po = sd->srf_shape.plane.origin;
                    if (std::fabs(e3[2]) > 0.999) {
                        orig[0] = 0.0; orig[1] = 0.0; orig[2] = po[2];
                    } else if (std::fabs(e3[1]) > 0.999) {
                        vy[0] = 0.0; vy[1] = 0.0; vy[2] = -1.0;
                        vz[0] = 0.0; vz[1] = 1.0; vz[2] = 0.0;
                        orig[0] = 0.0; orig[1] = po[1]; orig[2] = 0.0;
                    } else if (std::fabs(e3[0]) > 0.999) {
                        vx[0] = 0.0; vx[1] = -1.0; vx[2] = 0.0;
                        vy[0] = 0.0; vy[1] = 0.0; vy[2] = -1.0;
                        vz[0] = 1.0; vz[1] = 0.0; vz[2] = 0.0;
                        orig[0] = po[0]; orig[1] = 0.0; orig[2] = 0.0;
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            vx[k] = sd->srf_shape.plane.e1[k];
                            vy[k] = sd->srf_shape.plane.e2[k];
                            vz[k] = e3[k];
                            orig[k] = po[k];
                        }
                    }
                    matched = true;
                }
                ProGeomitemdataFree(&gd);
            }
            ProSelectionFree(&psel);
        }
    }
    if (plane_n) {
        plane_n[0] = vz[0];
        plane_n[1] = vz[1];
        plane_n[2] = vz[2];
    }
    ProError st = ProMatrixInit(vx, vy, vz, orig, sec_to_part);
    if (st == PRO_TK_NO_ERROR) rigid_matrix_invert(sec_to_part, part_to_sec);
    return st;
}

inline ProError populate_section_from_json(
    ProSection sec,
    ProMatrix part_to_sec,
    ProMatrix sec_to_part,
    const double plane_n[3],
    const Json &sk_spec,
    FILE *log,
    const double shift_2d[2] = nullptr
) {
    SketchEntityBook book;
    bool is_3d = sk_spec.contains("entities_3d");
    bool is_rz = !is_3d && sk_spec.contains("entities_rz");
    const Json &ents = is_3d ? sk_spec["entities_3d"]
                             : (is_rz ? sk_spec["entities_rz"] : sk_spec.at("entities_2d"));

    double er[3]{0.0, 1.0, 0.0};
    if (is_rz) {
        er[0] = -plane_n[1];
        er[1] = plane_n[0];
        er[2] = 0.0;
        double len = std::sqrt(er[0] * er[0] + er[1] * er[1]);
        if (len < 1e-6) {
            log_printf(log, "RZ_PLANE_ERROR plane normal is parallel to Z: (%.6f,%.6f,%.6f)\n",
                       plane_n[0], plane_n[1], plane_n[2]);
            return PRO_TK_BAD_INPUTS;
        }
        er[0] /= len;
        er[1] /= len;
        if (er[0] < -1e-6 || (std::fabs(er[0]) <= 1e-6 && er[1] < 0.0)) {
            er[0] = -er[0];
            er[1] = -er[1];
        }
    }

    auto rz_to_3d = [&](const Json &arr, double p3[3]) {
        double r = arr.at(0).get<double>();
        double z = arr.at(1).get<double>();
        p3[0] = r * er[0];
        p3[1] = r * er[1];
        p3[2] = z;
    };

    auto get_pt2 = [&](const Json &arr, Pro2dPnt out) {
        if (is_3d) {
            double p3[3]{arr.at(0).get<double>(), arr.at(1).get<double>(), arr.at(2).get<double>()};
            to_sec2d(part_to_sec, p3, out);
        } else if (is_rz) {
            double p3[3]{};
            rz_to_3d(arr, p3);
            to_sec2d(part_to_sec, p3, out);
        } else {
            out[0] = arr.at(0).get<double>();
            out[1] = arr.at(1).get<double>();
        }
        if (shift_2d) {
            out[0] += shift_2d[0];
            out[1] += shift_2d[1];
        }
    };

    int first_loop_ent_id = -1;
    int loop_seg_count = 0;

    for (size_t i = 0; i < ents.size(); ++i) {
        const Json &e = ents[i];
        std::string etype = e.at("type").get<std::string>();
        std::string ename = e.value("name", "ent_" + std::to_string(i));
        bool construction = e.value("construction", false);
        int id = -1;
        ProError st = PRO_TK_NO_ERROR;

        auto track_loop = [&](int eid, int nsegs = 1) {
            if (!construction && etype != "centerline") {
                if (first_loop_ent_id < 0 && eid >= 0) first_loop_ent_id = eid;
                loop_seg_count += nsegs;
            }
        };

        if (etype == "line" || etype == "centerline") {
            Pro2dLinedef def{};
            def.type = (etype == "centerline") ? PRO_2D_CENTER_LINE : PRO_2D_LINE;
            get_pt2(e.at("start"), def.end1);
            get_pt2(e.at("end"), def.end2);
            st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
            if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_line", st);
            if (etype == "centerline") {
                /* Geometry centerline (construction=FALSE) serves as Revolve axis */
                ProSectionEntityConstructionSet(sec, id, PRO_B_FALSE);
            } else if (construction) {
                ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
            }
            track_loop(id);
            book.ids[ename] = id;
            SketchEntityCache gc{};
            gc.type = def.type;
            gc.p_start[0] = def.end1[0]; gc.p_start[1] = def.end1[1];
            gc.p_end[0] = def.end2[0]; gc.p_end[1] = def.end2[1];
            gc.p_mid[0] = 0.5 * (def.end1[0] + def.end2[0]);
            gc.p_mid[1] = 0.5 * (def.end1[1] + def.end2[1]);
            gc.p_center[0] = gc.p_mid[0]; gc.p_center[1] = gc.p_mid[1];
            gc.valid = true;
            book.geom[id] = gc;
        } else if (etype == "circle") {
            Pro2dCircledef def{};
            def.type = PRO_2D_CIRCLE;
            get_pt2(e.at("center"), def.center);
            def.radius = e.at("radius").get<double>();
            st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
            if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_circle", st);
            if (construction) ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
            book.ids[ename] = id;
            SketchEntityCache gc{};
            gc.type = PRO_2D_CIRCLE;
            gc.p_center[0] = def.center[0]; gc.p_center[1] = def.center[1];
            gc.p_start[0] = def.center[0] + def.radius; gc.p_start[1] = def.center[1];
            gc.p_end[0] = gc.p_start[0]; gc.p_end[1] = gc.p_start[1];
            gc.p_mid[0] = gc.p_start[0]; gc.p_mid[1] = gc.p_start[1];
            gc.valid = true;
            book.geom[id] = gc;
        } else if (etype == "ellipse") {
            Pro2dEllipsedef def{};
            def.type = PRO_2D_ELLIPSE;
            get_pt2(e.at("center"), def.origin);
            if (is_3d && e.contains("radii_xyz")) {
                double rx3 = e["radii_xyz"].at(0).get<double>();
                double ry3 = e["radii_xyz"].at(1).get<double>();
                double rz3 = e["radii_xyz"].at(2).get<double>();
                def.x_radius = std::sqrt(
                    std::pow(rx3 * sec_to_part[0][0], 2) +
                    std::pow(ry3 * sec_to_part[0][1], 2) +
                    std::pow(rz3 * sec_to_part[0][2], 2));
                def.y_radius = std::sqrt(
                    std::pow(rx3 * sec_to_part[1][0], 2) +
                    std::pow(ry3 * sec_to_part[1][1], 2) +
                    std::pow(rz3 * sec_to_part[1][2], 2));
            } else if (is_3d && e.contains("u_dir")) {
                double u0 = e["u_dir"].at(0).get<double>();
                double u1 = e["u_dir"].at(1).get<double>();
                double u2 = e["u_dir"].at(2).get<double>();
                double r1 = e.at("rx").get<double>();
                double r2 = e.at("ry").get<double>();
                double du = std::fabs(u0 * sec_to_part[0][0] + u1 * sec_to_part[0][1] + u2 * sec_to_part[0][2]);
                double dv = std::fabs(u0 * sec_to_part[1][0] + u1 * sec_to_part[1][1] + u2 * sec_to_part[1][2]);
                if (du > 0.999) {
                    def.x_radius = r1;
                    def.y_radius = r2;
                } else if (dv > 0.999) {
                    def.x_radius = r2;
                    def.y_radius = r1;
                } else {
                    return log_st(log, "sec_add_ellipse_align", PRO_TK_BAD_INPUTS);
                }
            } else {
                def.x_radius = e.value("x_radius", e.value("rx", 1.0));
                def.y_radius = e.value("y_radius", e.value("ry", 1.0));
            }
            st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
            if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_ellipse", st);
            if (construction) ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
            track_loop(id);
            book.ids[ename] = id;
            SketchEntityCache gc{};
            gc.type = PRO_2D_ELLIPSE;
            gc.p_center[0] = def.origin[0]; gc.p_center[1] = def.origin[1];
            gc.p_start[0] = def.origin[0] + def.x_radius; gc.p_start[1] = def.origin[1];
            gc.p_end[0] = gc.p_start[0]; gc.p_end[1] = gc.p_start[1];
            gc.p_mid[0] = gc.p_start[0]; gc.p_mid[1] = gc.p_start[1];
            gc.valid = true;
            book.geom[id] = gc;
        } else if (etype == "arc") {
            SketchEntityCache gc{};
            if (is_3d || is_rz) {
                double c3[3]{}, s3[3]{}, e3[3]{}, m3[3]{};
                const Json &jc = e.at("center"), &js = e.at("start"), &je = e.at("end"), &jm = e.at("mid");
                if (is_3d) {
                    c3[0] = jc[0].get<double>(); c3[1] = jc[1].get<double>(); c3[2] = jc[2].get<double>();
                    s3[0] = js[0].get<double>(); s3[1] = js[1].get<double>(); s3[2] = js[2].get<double>();
                    e3[0] = je[0].get<double>(); e3[1] = je[1].get<double>(); e3[2] = je[2].get<double>();
                    m3[0] = jm[0].get<double>(); m3[1] = jm[1].get<double>(); m3[2] = jm[2].get<double>();
                } else {
                    rz_to_3d(jc, c3);
                    rz_to_3d(js, s3);
                    rz_to_3d(je, e3);
                    rz_to_3d(jm, m3);
                }
                bool swapped = false;
                st = sec_add_arc_3d(
                    sec, part_to_sec, c3, s3, e3, m3, e.at("radius").get<double>(), &id, &swapped, &gc);
                if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_arc_3d", st);
                book.arc_swapped[id] = swapped;
            } else {
                Pro2dArcdef def{};
                def.type = PRO_2D_ARC;
                get_pt2(e.at("center"), def.center);
                def.radius = e.at("radius").get<double>();
                double a1 = e.at("start_deg").get<double>() * M_PI / 180.0;
                double a2 = e.at("end_deg").get<double>() * M_PI / 180.0;
                int dir = e.value("dir", 1);
                bool swapped = (dir < 0);
                if (swapped) std::swap(a1, a2);
                a1 = norm_ang_0_2pi(a1);
                a2 = norm_ang_0_2pi(a2);
                if (a2 <= a1) a2 += 2.0 * M_PI;
                def.start_angle = a1;
                def.end_angle = a2;
                st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
                if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_arc_2d", st);
                book.arc_swapped[id] = swapped;
                gc.type = PRO_2D_ARC;
                gc.p_center[0] = def.center[0]; gc.p_center[1] = def.center[1];
                gc.p_start[0] = def.center[0] + def.radius * std::cos(a1);
                gc.p_start[1] = def.center[1] + def.radius * std::sin(a1);
                gc.p_end[0] = def.center[0] + def.radius * std::cos(a2);
                gc.p_end[1] = def.center[1] + def.radius * std::sin(a2);
                double mid_phys = 0.5 * (a1 + a2);
                gc.p_mid[0] = def.center[0] + def.radius * std::cos(mid_phys);
                gc.p_mid[1] = def.center[1] + def.radius * std::sin(mid_phys);
                gc.valid = true;
            }
            if (construction) ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
            track_loop(id);
            book.ids[ename] = id;
            if (gc.valid) book.geom[id] = gc;
        } else if (etype == "polyline") {
            const Json &pts = e.at("points");
            bool closed = e.value("closed", false);
            bool auto_same = e.value("constrain_corners", true);
            size_t count = pts.size();
            size_t segs = closed ? count : (count > 0 ? count - 1 : 0);
            std::vector<int> seg_ids;
            for (size_t k = 0; k < segs; ++k) {
                Pro2dLinedef def{};
                def.type = PRO_2D_LINE;
                get_pt2(pts[k], def.end1);
                get_pt2(pts[(k + 1) % count], def.end2);
                st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
                if (st != PRO_TK_NO_ERROR) {
                    log_printf(log, "  SEC_POLY_FAIL ent=%s seg=%zu p1=(%.3f,%.3f) p2=(%.3f,%.3f) st=%d\n",
                               ename.c_str(), k, def.end1[0], def.end1[1], def.end2[0], def.end2[1], static_cast<int>(st));
                    return log_st(log, "sec_add_polyline_seg", st);
                }
                if (construction) ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
                book.ids[ename + "_" + std::to_string(k)] = id;
                SketchEntityCache gc{};
                gc.type = PRO_2D_LINE;
                gc.p_start[0] = def.end1[0]; gc.p_start[1] = def.end1[1];
                gc.p_end[0] = def.end2[0]; gc.p_end[1] = def.end2[1];
                gc.p_mid[0] = 0.5 * (def.end1[0] + def.end2[0]);
                gc.p_mid[1] = 0.5 * (def.end1[1] + def.end2[1]);
                gc.p_center[0] = gc.p_mid[0]; gc.p_center[1] = gc.p_mid[1];
                gc.valid = true;
                book.geom[id] = gc;
                seg_ids.push_back(id);
            }
            if (auto_same && seg_ids.size() >= 2) {
                size_t joins = closed ? seg_ids.size() : seg_ids.size() - 1;
                for (size_t k = 0; k < joins; ++k) {
                    int id1 = seg_ids[k];
                    int id2 = seg_ids[(k + 1) % seg_ids.size()];
                    ProSelection s1 = nullptr, s2 = nullptr;
                    if (sec_entity_pick_sel(sec, book, id1, PRO_ENT_END, &s1) == PRO_TK_NO_ERROR &&
                        sec_entity_pick_sel(sec, book, id2, PRO_ENT_START, &s2) == PRO_TK_NO_ERROR) {
                        ProSelection arr[2] = {s1, s2};
                        int cid = -1;
                        ProSectionConstraintCreate(sec, arr, 2, PRO_CONSTRAINT_SAME_POINT, &cid);
                    }
                    if (s1) ProSelectionFree(&s1);
                    if (s2) ProSelectionFree(&s2);
                }
            }
            if (!seg_ids.empty()) {
                book.ids[ename] = seg_ids[0];
                track_loop(seg_ids[0], static_cast<int>(seg_ids.size()));
            }
        } else if (etype == "spline") {
            const Json &pts = e.at("points");
            std::vector<Pro2dPnt> xy(pts.size());
            for (size_t k = 0; k < pts.size(); ++k) get_pt2(pts[k], xy[k]);
            Pro2dSplinedef def{};
            def.type = PRO_2D_SPLINE;
            bool is_periodic = e.value("periodic", e.value("closed", false));
            def.tangency_type = is_periodic ? PRO_2D_SPLINE_TAN_PERIODIC : PRO_2D_SPLINE_TAN_NONE;
            def.n_points = static_cast<unsigned int>(xy.size());
            def.point_arr = xy.data();
            st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
            if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_spline", st);
            if (construction) ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
            track_loop(id);
            book.ids[ename] = id;
            if (!xy.empty()) {
                SketchEntityCache gc{};
                gc.type = PRO_2D_SPLINE;
                gc.p_start[0] = xy.front()[0]; gc.p_start[1] = xy.front()[1];
                gc.p_end[0] = xy.back()[0]; gc.p_end[1] = xy.back()[1];
                const auto &mp = xy[xy.size() / 2];
                gc.p_mid[0] = mp[0]; gc.p_mid[1] = mp[1];
                gc.p_center[0] = mp[0]; gc.p_center[1] = mp[1];
                gc.valid = true;
                book.geom[id] = gc;
            }
        } else if (etype == "b_spline") {
            const Json &cpts = e.at("control_points");
            int degree = e.at("degree").get<int>();
            std::vector<Pro2dPnt> ctrl(cpts.size());
            for (size_t k = 0; k < cpts.size(); ++k) get_pt2(cpts[k], ctrl[k]);
            std::vector<double> knots;
            if (e.contains("knots") && !e["knots"].is_null()) {
                knots = e["knots"].get<std::vector<double>>();
            } else {
                int inner = static_cast<int>(ctrl.size()) - degree - 1;
                knots.assign(static_cast<size_t>(degree) + 1, 0.0);
                for (int k = 1; k <= inner; ++k) {
                    knots.push_back(static_cast<double>(k) / static_cast<double>(inner + 1));
                }
                knots.insert(knots.end(), static_cast<size_t>(degree) + 1, 1.0);
            }
            std::vector<double> weights;
            if (e.contains("weights") && !e["weights"].is_null()) {
                weights = e["weights"].get<std::vector<double>>();
            }
            Pro2dBSplinedef def{};
            def.type = PRO_2D_B_SPLINE;
            def.degree = degree;
            def.num_knots = static_cast<int>(knots.size());
            def.params = knots.data();
            def.num_c_points = static_cast<int>(ctrl.size());
            def.c_pnts = ctrl.data();
            def.weights = weights.empty() ? nullptr : weights.data();
            st = ProSectionEntityAdd(sec, reinterpret_cast<Pro2dEntdef *>(&def), &id);
            if (st != PRO_TK_NO_ERROR) return log_st(log, "sec_add_b_spline", st);
            if (construction) ProSectionEntityConstructionSet(sec, id, PRO_B_TRUE);
            track_loop(id);
            book.ids[ename] = id;
            if (!ctrl.empty()) {
                SketchEntityCache gc{};
                gc.type = PRO_2D_B_SPLINE;
                gc.p_start[0] = ctrl.front()[0]; gc.p_start[1] = ctrl.front()[1];
                gc.p_end[0] = ctrl.back()[0]; gc.p_end[1] = ctrl.back()[1];
                const auto &mp = ctrl[ctrl.size() / 2];
                gc.p_mid[0] = mp[0]; gc.p_mid[1] = mp[1];
                gc.p_center[0] = mp[0]; gc.p_center[1] = mp[1];
                gc.valid = true;
                book.geom[id] = gc;
            }
        } else {
            log_printf(log, "UNKNOWN_ENTITY_TYPE=%s\n", etype.c_str());
            return PRO_TK_BAD_INPUTS;
        }
    }

    static const std::map<std::string, ProConstraintType> cmap = {
        {"same_point", PRO_CONSTRAINT_SAME_POINT},
        {"coincident", PRO_CONSTRAINT_SAME_POINT},
        {"tangent", PRO_CONSTRAINT_TANGENT_ENTS},
        {"orthogonal", PRO_CONSTRAINT_ORTHOG_ENTS},
        {"perpendicular", PRO_CONSTRAINT_ORTHOG_ENTS},
        {"parallel", PRO_CONSTRAINT_PARALLEL_ENTS},
        {"horizontal", PRO_CONSTRAINT_HORIZONTAL_ENT},
        {"vertical", PRO_CONSTRAINT_VERTICAL_ENT},
        {"equal_radii", PRO_CONSTRAINT_EQUAL_RADII},
        {"equal_segments", PRO_CONSTRAINT_EQUAL_SEGMENTS},
        {"equal_length", PRO_CONSTRAINT_EQUAL_SEGMENTS},
        {"collinear", PRO_CONSTRAINT_COLLINEAR_LINES},
        {"point_on", PRO_CONSTRAINT_PNT_ON_ENT},
    };

    if (sk_spec.contains("constraints")) {
        for (size_t ci = 0; ci < sk_spec["constraints"].size(); ++ci) {
            const Json &c = sk_spec["constraints"][ci];
            std::string ctype_str = c.at("type").get<std::string>();
            auto cit = cmap.find(ctype_str);
            if (cit == cmap.end()) return PRO_TK_BAD_INPUTS;

            const Json &refs = c.at("refs");
            std::vector<ProSelection> sels;
            for (const Json &r : refs) {
                int ent_id = book.ids.at(r.at("entity").get<std::string>());
                ProSectionPointType pt = parse_point_type(r.value("pt", "whole"));
                ProSelection s = nullptr;
                ProError st = sec_entity_pick_sel(sec, book, ent_id, pt, &s);
                if (st != PRO_TK_NO_ERROR) {
                    for (ProSelection free_s : sels) ProSelectionFree(&free_s);
                    return log_st(log, "sec_constr_sel", st);
                }
                sels.push_back(s);
            }
            int cid = -1;
            ProError cst = ProSectionConstraintCreate(
                sec, sels.data(), static_cast<int>(sels.size()), cit->second, &cid);
            for (ProSelection s : sels) ProSelectionFree(&s);
            if (cst != PRO_TK_NO_ERROR && cst != PRO_TK_E_FOUND) {
                log_printf(log, "CONSTR_WARN idx=%zu type=%s st=%d\n", ci, ctype_str.c_str(), static_cast<int>(cst));
            }
        }
    }

    static const std::map<std::string, ProSecdimType> dmap = {
        {"line", PRO_TK_DIM_LINE},
        {"length", PRO_TK_DIM_LINE},
        {"radius", PRO_TK_DIM_RAD},
        {"diameter", PRO_TK_DIM_DIA},
        {"pnt_pnt", PRO_TK_DIM_PNT_PNT},
        {"distance", PRO_TK_DIM_PNT_PNT},
        {"horiz", PRO_TK_DIM_PNT_PNT_HORIZ},
        {"vert", PRO_TK_DIM_PNT_PNT_VERT},
        {"line_line", PRO_TK_DIM_LINE_LINE},
        {"angle", PRO_TK_DIM_LINES_ANGLE},
    };

    if (sk_spec.contains("dimensions")) {
        for (size_t di = 0; di < sk_spec["dimensions"].size(); ++di) {
            const Json &d = sk_spec["dimensions"][di];
            std::string dtype_str = d.at("type").get<std::string>();
            auto dit = dmap.find(dtype_str);
            if (dit == dmap.end()) return PRO_TK_BAD_INPUTS;

            std::vector<int> eids;
            std::vector<ProSectionPointType> pts;
            for (const Json &r : d.at("refs")) {
                int ent_id = book.ids.at(r.at("entity").get<std::string>());
                eids.push_back(ent_id);
                pts.push_back(map_logical_point(book, ent_id, parse_point_type(r.value("pt", "whole"))));
            }
            Pro2dPnt place{0.0, 0.0};
            if (d.contains("place_3d")) {
                double p3[3]{
                    d["place_3d"].at(0).get<double>(),
                    d["place_3d"].at(1).get<double>(),
                    d["place_3d"].at(2).get<double>()
                };
                to_sec2d(part_to_sec, p3, place);
            } else if (d.contains("place_rz")) {
                double p3[3]{};
                rz_to_3d(d["place_rz"], p3);
                to_sec2d(part_to_sec, p3, place);
            } else if (d.contains("place_2d")) {
                place[0] = d["place_2d"].at(0).get<double>();
                place[1] = d["place_2d"].at(1).get<double>();
            }
            if (shift_2d) {
                place[0] += shift_2d[0];
                place[1] += shift_2d[1];
            }
            int dim_id = -1;
            ProError dst = ProSecdimCreate(
                sec,
                eids.data(),
                pts.data(),
                static_cast<int>(eids.size()),
                dit->second,
                place,
                &dim_id);
            if (dst != PRO_TK_NO_ERROR) {
                log_printf(log, "DIM_WARN idx=%zu type=%s st=%d\n", di, dtype_str.c_str(), static_cast<int>(dst));
            } else if (d.contains("value") && d["value"].is_number()) {
                ProSecdimValueSet(sec, dim_id, d["value"].get<double>());
            }
        }
    }

    if (sk_spec.contains("start_vertex") && sk_spec["start_vertex"].is_object()) {
        const Json &sv = sk_spec["start_vertex"];
        int ent_id = book.ids.at(sv.at("entity").get<std::string>());
        ProSectionPointType pt = map_logical_point(book, ent_id, parse_point_type(sv.value("pt", "start")));
        ProSectionSubsectionStartSet(sec, ent_id, pt);
    } else if (sk_spec.value("auto_start_vertex", false) && first_loop_ent_id >= 0 && loop_seg_count >= 2) {
        ProSectionSubsectionStartSet(sec, first_loop_ent_id, PRO_ENT_START);
    }

    if (sk_spec.contains("blend_vertices") && sk_spec["blend_vertices"].is_array()) {
        for (const Json &bv : sk_spec["blend_vertices"]) {
            int ent_id = book.ids.at(bv.at("entity").get<std::string>());
            ProSectionPointType pt = map_logical_point(book, ent_id, parse_point_type(bv.value("pt", "end")));
            int vid = -1;
            ProSectionSubsectionBlendVertexSet(sec, ent_id, pt, &vid);
        }
    }

    return PRO_TK_NO_ERROR;
}

inline ProError sec_autodim_and_regen(
    ProSection sec,
    const Json &sk_spec,
    FILE *log,
    const char *tag
) {
    ProWSecerror sec_err = nullptr;
    ProError st = PRO_TK_NO_ERROR;

    if (sk_spec.contains("epsilon") && sk_spec["epsilon"].is_number()) {
        ProSectionEpsilonSet(sec, sk_spec["epsilon"].get<double>());
    }

    auto log_sec_errs = [&](const char *phase) {
        int n_err = 0;
        ProSecerrorCount(&sec_err, &n_err);
        log_printf(log, "SEC_FAIL[%s/%s] st=%d n_err=%d\n", tag, phase, static_cast<int>(st), n_err);
        for (int i = 0; i < n_err; ++i) {
            ProMsg wmsg;
            if (ProSecerrorMsgGet(sec_err, i, wmsg) == PRO_TK_NO_ERROR) {
                log_printf(log, "SEC_ERR[%s/%s] %s\n", tag, phase, w2utf8(wmsg).c_str());
            }
        }
    };

    if (!sk_spec.value("skip_autodim", false)) {
        st = ProSecerrorAlloc(&sec_err);
        if (st != PRO_TK_NO_ERROR) return st;
        st = ProSectionAutodim(sec, &sec_err);
        if (st != PRO_TK_NO_ERROR) {
            log_sec_errs("autodim");
            ProSecerrorFree(&sec_err);
            return st;
        }
        ProSecerrorFree(&sec_err);
    }

    st = ProSecerrorAlloc(&sec_err);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProSectionRegenerate(sec, &sec_err);
    if (st != PRO_TK_NO_ERROR) {
        log_sec_errs("regen");
    }
    ProSecerrorFree(&sec_err);
    return st;
}

/* Shared two-phase sketched feature creation */
inline ProError build_sketched_feature(
    PlanContext &ctx,
    ProElement tree,
    const ProElemId *sketch_path,
    int sketch_path_len,
    int set_ext_dir,
    int std_dir,
    int is_revolve,
    const Json &sk_spec,
    const char *tag,
    ProFeature *out_feat
) {
    ProSolid solid = ctx.solid;
    FILE *log = ctx.log;
    ProModelitem mdl_item;
    ProSelection mdl_sel = nullptr;
    ProFeatureCreateOptions *inc_opts = nullptr;
    ProFeatureCreateOptions *redef_opts = nullptr;
    ProErrorlist errs{};
    ProElement extracted = nullptr, sk_elem = nullptr;
    ProSection sec = nullptr;
    ProMatrix part_to_sec;

    ProMdlToModelitem(static_cast<ProMdl>(solid), &mdl_item);
    ProError st = ProSelectionAlloc(nullptr, &mdl_item, &mdl_sel);
    if (st != PRO_TK_NO_ERROR) return st;

    ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, reinterpret_cast<ProArray *>(&inc_opts));
    inc_opts[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;

    st = ProFeatureWithoptionsCreate(mdl_sel, tree, inc_opts, PRO_REGEN_NO_FLAGS, out_feat, &errs);
    ProArrayFree(reinterpret_cast<ProArray *>(&inc_opts));
    ProSelectionFree(&mdl_sel);
    if (st != PRO_TK_NO_ERROR) {
        log_errorlist(log, tag, &errs);
        return log_st(log, "FeatureCreate_inc", st);
    }

    st = ProFeatureElemtreeExtract(out_feat, nullptr, PRO_FEAT_EXTRACT_NO_OPTS, &extracted);
    if (st != PRO_TK_NO_ERROR) return log_st(log, "ElemtreeExtract", st);

    st = get_elem_by_path(extracted, sketch_path, sketch_path_len, &sk_elem);
    if (st != PRO_TK_NO_ERROR) {
        ProFeatureElemtreeFree(out_feat, extracted);
        return log_st(log, "get_sk_elem", st);
    }

    st = ProElementSpecialvalueGet(sk_elem, nullptr, reinterpret_cast<ProAppData *>(&sec));
    if (st != PRO_TK_NO_ERROR || !sec) {
        ProFeatureElemtreeFree(out_feat, extracted);
        return log_st(log, "SpecialvalueGet", (st == PRO_TK_NO_ERROR) ? PRO_TK_GENERAL_ERROR : st);
    }

    double plane_n[3]{0.0, 0.0, 1.0};
    ProMatrix sec_to_part{};
    st = sec_xform_init(sec, part_to_sec, plane_n, sec_to_part);
    if (st != PRO_TK_NO_ERROR) {
        ProFeatureElemtreeFree(out_feat, extracted);
        return log_st(log, "sec_xform_init", st);
    }

    /* Optional explicit section projections (required for tilted/rotated planes) */
    if (sk_spec.contains("project_planes")) {
        for (const Json &pref : sk_spec["project_planes"]) {
            ProSelection psel = nullptr;
            if (resolve_plane_selection(ctx, pref, &psel) == PRO_TK_NO_ERROR && psel) {
                int pid = -1;
                ProError pst = ProSectionEntityFromProjection(sec, psel, &pid);
                log_printf(log, "SEC_PROJ_PLANE[%s] st=%d id=%d\n", tag, static_cast<int>(pst), pid);
                ProSelectionFree(&psel);
            }
        }
    }
    if (sk_spec.contains("project_axes")) {
        for (const Json &aref : sk_spec["project_axes"]) {
            ProSelection asel = nullptr;
            if (resolve_axis_selection(ctx, aref, &asel) == PRO_TK_NO_ERROR && asel) {
                int pid = -1;
                ProError pst = ProSectionEntityFromProjection(sec, asel, &pid);
                log_printf(log, "SEC_PROJ_AXIS[%s] st=%d id=%d\n", tag, static_cast<int>(pst), pid);
                ProSelectionFree(&asel);
            }
        }
    }

    /* Strict Intent Manager ordering required by Creo 8+ solver; honor custom epsilon BEFORE entity creation */
    if (sk_spec.contains("epsilon") && sk_spec["epsilon"].is_number()) {
        ProSectionEpsilonSet(sec, sk_spec["epsilon"].get<double>());
    }
    ProSectionIntentManagerModeSet(sec, PRO_B_TRUE);
    st = populate_section_from_json(sec, part_to_sec, sec_to_part, plane_n, sk_spec, log);
    ProSectionIntentManagerModeSet(sec, PRO_B_FALSE);
    if (st != PRO_TK_NO_ERROR) {
        ProFeatureElemtreeFree(out_feat, extracted);
        return log_st(log, "populator", st);
    }

    st = sec_autodim_and_regen(sec, sk_spec, log, tag);
    if (st != PRO_TK_NO_ERROR) {
        ProFeatureElemtreeFree(out_feat, extracted);
        return log_st(log, "sec_autodim_and_regen", st);
    }

    if (set_ext_dir) {
        int chosen_dir = std_dir;
        if (sk_spec.contains("toward_point")) {
            const Json &tp = sk_spec["toward_point"];
            double dx = tp.at(0).get<double>() - sec_to_part[3][0];
            double dy = tp.at(1).get<double>() - sec_to_part[3][1];
            double dz = tp.at(2).get<double>() - sec_to_part[3][2];
            double dot = dx * plane_n[0] + dy * plane_n[1] + dz * plane_n[2];
            chosen_dir = (dot >= 0.0) ? PRO_EXT_CR_IN_SIDE_TWO : PRO_EXT_CR_IN_SIDE_ONE;
            log_printf(log, "EXT_TOWARD[%s] orig=(%.3f,%.3f,%.3f) n=(%.4f,%.4f,%.4f) dot=%.4f dir=%d\n",
                       tag, sec_to_part[3][0], sec_to_part[3][1], sec_to_part[3][2],
                       plane_n[0], plane_n[1], plane_n[2], dot, chosen_dir);
        }
        ProElemId p_dir[] = {PRO_E_STD_DIRECTION};
        set_int_by_path(extracted, p_dir, 1, chosen_dir);
    }
    if (is_revolve) {
        ProElemId p_axis_opt[] = {PRO_E_REVOLVE_AXIS_OPT};
        set_int_by_path(extracted, p_axis_opt, 1, PRO_REV_AXIS_INT_REF);
    }

    ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, reinterpret_cast<ProArray *>(&redef_opts));
    redef_opts[0] = PRO_FEAT_CR_NO_OPTS;
    st = ProFeatureWithoptionsRedefine(nullptr, out_feat, extracted, redef_opts, PRO_REGEN_NO_FLAGS, &errs);
    ProArrayFree(reinterpret_cast<ProArray *>(&redef_opts));
    if (st != PRO_TK_NO_ERROR) {
        log_errorlist(log, tag, &errs);
        log_st(log, "Redefine", st);
    }
    ProFeatureElemtreeFree(out_feat, extracted);
    return st;
}

/* ------------------------------------------------------------------ */
/* Metrics, feature-tree logging, and view/model exports              */
/* ------------------------------------------------------------------ */

inline ProError measure_solid_volume(ProSolid solid, double *out_vol, double *out_area, Pro3dPnt box[2]) {
    ProMassProperty mp{};
    ProError st = ProSolidOutlineCompute(solid, nullptr, nullptr, 0, box);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProSolidMassPropertyWithDensityGet(solid, nullptr, PRO_MP_DENS_USE_IF_MISSING, 1.0, &mp);
    if (st != PRO_TK_NO_ERROR) return st;
    if (out_vol) *out_vol = mp.volume;
    if (out_area) *out_area = mp.surface_area;
    return PRO_TK_NO_ERROR;
}

inline ProError log_solid_metrics(FILE *log, ProSolid solid, const char *stage) {
    Pro3dPnt box[2]{};
    double vol = 0.0, area = 0.0;
    int *failed_ids = nullptr, *co_ids = nullptr, *cox_ids = nullptr;
    ProMdlType mtype = PRO_MDL_PART;
    ProMdlTypeGet(reinterpret_cast<ProMdl>(solid), &mtype);
    int n_failed = 0, n_bodies = 0;
    int def_body_id = -1;
    std::ostringstream binfo;

    if (mtype == PRO_MDL_PART) {
        ProSolidBody *bodies = nullptr;
        ProSolidBody def_body{};
        if (ProSolidDefaultBodyGet(solid, &def_body) == PRO_TK_NO_ERROR) def_body_id = def_body.id;
        if (ProSolidBodiesCollect(solid, &bodies) == PRO_TK_NO_ERROR && bodies) {
            ProArraySizeGet(reinterpret_cast<ProArray>(bodies), &n_bodies);
            for (int bi = 0; bi < n_bodies; ++bi) {
                ProOutline bo{};
                ProSolidBodyState bstate = PRO_BODY_STATE_NONE;
                ProSolidBodyStateGet(&bodies[bi], &bstate);
                ProError ost = ProSolidBodyOutlineGet(&bodies[bi], bo);
                binfo << " body[" << bi << "]{id=" << bodies[bi].id
                      << (bodies[bi].id == def_body_id ? "(DEF)" : "")
                      << ",st=" << static_cast<int>(bstate);
                if (ost == PRO_TK_NO_ERROR) {
                    binfo << ",x=" << bo[0][0] << ".." << bo[1][0]
                          << ",y=" << bo[0][1] << ".." << bo[1][1]
                          << ",z=" << bo[0][2] << ".." << bo[1][2];
                }
                binfo << "}";
            }
            ProArrayFree(reinterpret_cast<ProArray *>(&bodies));
        }
    }
    ProError st = measure_solid_volume(solid, &vol, &area, box);
    if (st != PRO_TK_NO_ERROR) return st;

    st = ProSolidFailedfeaturesList(solid, &failed_ids, &co_ids, &cox_ids);
    if (st == PRO_TK_NO_ERROR && failed_ids) {
        ProArraySizeGet(reinterpret_cast<ProArray>(failed_ids), &n_failed);
    }
    if (failed_ids) ProArrayFree(reinterpret_cast<ProArray *>(&failed_ids));
    if (co_ids) ProArrayFree(reinterpret_cast<ProArray *>(&co_ids));
    if (cox_ids) ProArrayFree(reinterpret_cast<ProArray *>(&cox_ids));

    log_printf(
        log,
        "METRIC[%s] mdl_type=%d bodies=%d def=%d bbox=[%.3f..%.3f, %.3f..%.3f, %.3f..%.3f] vol=%.4f area=%.4f failed=%d%s\n",
        stage,
        static_cast<int>(mtype),
        n_bodies,
        def_body_id,
        box[0][0],
        box[1][0],
        box[0][1],
        box[1][1],
        box[0][2],
        box[1][2],
        vol,
        area,
        n_failed,
        binfo.str().c_str());
    return (n_failed == 0) ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
}

struct FeatLogCtx {
    FILE *log = nullptr;
    int index = 0;
    int require_active = 1;
    int bad_count = 0;
};

inline ProError visit_feat_log_action(ProFeature *p_feat, ProError status, ProAppData data) {
    auto *ctx = static_cast<FeatLogCtx *>(data);
    ProBoolean is_vis = PRO_B_FALSE;
    ProName wname{}, wtype{};
    ProFeatStatus fstat = PRO_FEAT_INVALID;
    (void)status;

    if (ProFeatureVisibilityGet(p_feat, &is_vis) != PRO_TK_NO_ERROR || is_vis != PRO_B_TRUE) {
        return PRO_TK_NO_ERROR;
    }
    ProModelitemNameGet(reinterpret_cast<ProModelitem *>(p_feat), wname);
    ProFeatureTypenameGet(p_feat, wtype);
    ProFeatureStatusGet(p_feat, &fstat);

    log_printf(
        ctx->log,
        "FEAT[%d] id=%d name=%s type=%s status=%d\n",
        ctx->index++,
        p_feat->id,
        w2utf8(wname).c_str(),
        w2utf8(wtype).c_str(),
        static_cast<int>(fstat));
    if (ctx->require_active && fstat != PRO_FEAT_ACTIVE) {
        ctx->bad_count++;
    }
    return PRO_TK_NO_ERROR;
}

inline ProError log_feature_tree(FILE *log, ProSolid solid, int require_active) {
    FeatLogCtx ctx;
    ctx.log = log;
    ctx.require_active = require_active;
    ProSolidFeatVisit(solid, visit_feat_log_action, nullptr, &ctx);
    log_printf(log, "FEATURE_COUNT=%d\n", ctx.index);
    return (ctx.bad_count == 0) ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
}

struct DatumDisplayGuard {
    struct Entry {
        const wchar_t *key;
        ProPath prev{};
        bool saved = false;
    };
    std::vector<Entry> entries;

    void hide() {
        const wchar_t *keys[] = {
            L"DATUM_DISPLAY",
            L"AXIS_DISPLAY",
            L"DATUM_POINT_DISPLAY",
            L"DISPLAY_COORD_SYS",
        };
        for (const wchar_t *k : keys) {
            Entry e{k};
            ProName kbuf{};
            wcscpy_s(kbuf, PRO_NAME_SIZE, k);
            if (ProConfigoptionGet(kbuf, e.prev) != PRO_TK_NO_ERROR) {
                wcscpy_s(e.prev, PRO_PATH_SIZE, L"yes");
            }
            e.saved = true;
            ProPath no_val{};
            wcscpy_s(no_val, PRO_PATH_SIZE, L"no");
            ProConfigoptSet(kbuf, no_val);
            entries.push_back(e);
        }
    }

    ~DatumDisplayGuard() {
        for (auto &e : entries) {
            if (e.saved) {
                ProName kbuf{};
                wcscpy_s(kbuf, PRO_NAME_SIZE, e.key);
                ProConfigoptSet(kbuf, e.prev);
            }
        }
    }
};

inline ProError export_view_jpg(ProMdl mdl, int win_id, const wchar_t *view_name, const wchar_t *out_file) {
    ProView view = nullptr;
    if (ProViewRetrieve(mdl, const_cast<wchar_t *>(view_name), &view) != PRO_TK_NO_ERROR) {
        ProViewReset(mdl, nullptr);
    }
    ProWindowRefit(win_id);
    ProWindowRepaint(win_id);
    return ProRasterFileWrite(
        win_id, PRORASTERDEPTH_24, 10.0, 7.5, PRORASTERDPI_200, PRORASTERTYPE_JPEG, const_cast<wchar_t *>(out_file));
}

struct AuxHideCtx {
    ProMdl mdl = nullptr;
    FILE *log = nullptr;
    std::set<int> keep_fids;
    std::set<std::string> keep_names;
    bool check_only = false;
    int hidden_cnt = 0;
    int kept_cnt = 0;
    int err_cnt = 0;
};

inline bool is_aux_ref_feattype(ProFeattype ftype) {
    return ftype == PRO_FEAT_DATUM ||
           ftype == PRO_FEAT_DATUM_AXIS ||
           ftype == PRO_FEAT_DATUM_POINT ||
           ftype == PRO_FEAT_CURVE ||
           ftype == PRO_FEAT_CSYS;
}

inline ProError visit_aux_hide_action(ProFeature *p_feat, ProError status, ProAppData data) {
    auto *hctx = static_cast<AuxHideCtx *>(data);
    ProBoolean is_vis = PRO_B_FALSE;
    ProFeattype ftype = PRO_FEAT_FIRST_FEAT;
    (void)status;

    if (ProFeatureVisibilityGet(p_feat, &is_vis) != PRO_TK_NO_ERROR || is_vis != PRO_B_TRUE) {
        return PRO_TK_NO_ERROR;
    }
    if (ProFeatureTypeGet(p_feat, &ftype) != PRO_TK_NO_ERROR || !is_aux_ref_feattype(ftype)) {
        return PRO_TK_NO_ERROR;
    }

    ProName wname{};
    ProModelitemNameGet(reinterpret_cast<ProModelitem *>(p_feat), wname);
    std::string fname = w2utf8(wname);

    if (hctx->keep_fids.count(p_feat->id) || (!fname.empty() && hctx->keep_names.count(fname))) {
        hctx->kept_cnt++;
        return PRO_TK_NO_ERROR;
    }

    ProModelitem fmi{};
    if (ProModelitemInit(hctx->mdl, p_feat->id, PRO_FEATURE, &fmi) != PRO_TK_NO_ERROR) {
        hctx->err_cnt++;
        return PRO_TK_NO_ERROR;
    }

    if (hctx->check_only) {
        ProBoolean is_hid = PRO_B_FALSE;
        if (ProModelitemIsHidden(&fmi, &is_hid) == PRO_TK_NO_ERROR && is_hid == PRO_B_TRUE) {
            hctx->hidden_cnt++;
        } else {
            hctx->kept_cnt++;
        }
        return PRO_TK_NO_ERROR;
    }

    ProError hst = ProModelitemHide(&fmi);
    if (hst == PRO_TK_NO_ERROR || hst == PRO_TK_NO_CHANGE) {
        hctx->hidden_cnt++;
    } else {
        hctx->err_cnt++;
        if (hctx->log) {
            log_printf(hctx->log, "AUX_HIDE_WARN fid=%d name=%s type=%d st=%d\n",
                       p_feat->id, fname.c_str(), static_cast<int>(ftype), static_cast<int>(hst));
        }
    }
    return PRO_TK_NO_ERROR;
}

inline ProError hide_auxiliary_ref_features(
    FILE *log,
    ProMdl mdl,
    ProSolid solid,
    const Json &plan,
    const PlanContext *ctx,
    bool check_only = false
) {
    if (!mdl || !solid || !plan.value("hide_auxiliary_layers", false)) return PRO_TK_NO_ERROR;

    AuxHideCtx hctx;
    hctx.mdl = mdl;
    hctx.log = log;
    hctx.check_only = check_only;
    if (plan.value("doc_type", "part") == "assembly") {
        hctx.keep_names.insert("ASM_RIGHT");
        hctx.keep_names.insert("ASM_TOP");
        hctx.keep_names.insert("ASM_FRONT");
        hctx.keep_names.insert("ASM_DEF_CSYS");
    }
    if (plan.contains("keep_visible_features") && plan["keep_visible_features"].is_array()) {
        for (const Json &kv : plan["keep_visible_features"]) {
            if (!kv.is_string()) continue;
            std::string key = kv.get<std::string>();
            hctx.keep_names.insert(key);
            if (ctx) {
                auto fit = ctx->features.find(key);
                if (fit != ctx->features.end()) {
                    hctx.keep_fids.insert(fit->second.id);
                }
            }
        }
    }

    ProSolidFeatVisit(solid, visit_aux_hide_action, nullptr, &hctx);
    ProError save_st = PRO_TK_NO_ERROR;
    if (!check_only) {
        save_st = ProLayerDisplaystatusSave(mdl);
    }
    if (log) {
        log_printf(log, "%s hidden=%d kept=%d err=%d layer_save_st=%d\n",
                   check_only ? "AUX_VERIFY_HIDDEN" : "AUX_HIDE",
                   hctx.hidden_cnt, hctx.kept_cnt, hctx.err_cnt, static_cast<int>(save_st));
    }
    return PRO_TK_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* Assembly transforms, semantic face/datum lookup, and diagnostics   */
/* ------------------------------------------------------------------ */

inline ProError elem_add_bool(ProElement parent, ProElemId id, ProBoolean val) {
    ProElement e = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProElementBooleanSet(e, val);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElemtreeElementAdd(parent, nullptr, e);
}

inline ProError elem_add_trf(ProElement parent, ProElemId id, ProMatrix mat) {
    ProElement e = nullptr;
    ProError st = ProElementAlloc(id, &e);
    if (st != PRO_TK_NO_ERROR) return st;
    st = ProElementTransformSet(e, mat);
    if (st != PRO_TK_NO_ERROR) return st;
    return ProElemtreeElementAdd(parent, nullptr, e);
}

inline ProError build_rigid_trf_from_op(const Json &op, ProMatrix out) {
    if (op.contains("matrix") && op["matrix"].is_array() && op["matrix"].size() == 4) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                out[r][c] = op["matrix"].at(r).at(c).get<double>();
            }
        }
        return PRO_TK_NO_ERROR;
    }
    const double deg2rad = M_PI / 180.0;
    double rx = 0.0, ry = 0.0, rz = 0.0;
    if (op.contains("rotation") && op["rotation"].is_array() && op["rotation"].size() == 3) {
        rx = op["rotation"].at(0).get<double>() * deg2rad;
        ry = op["rotation"].at(1).get<double>() * deg2rad;
        rz = op["rotation"].at(2).get<double>() * deg2rad;
    }
    ProPoint3d origin{0.0, 0.0, 0.0};
    if (op.contains("translation") && op["translation"].is_array() && op["translation"].size() == 3) {
        origin[0] = op["translation"].at(0).get<double>();
        origin[1] = op["translation"].at(1).get<double>();
        origin[2] = op["translation"].at(2).get<double>();
    }
    double cx = std::cos(rx), sx = std::sin(rx);
    double cy = std::cos(ry), sy = std::sin(ry);
    double cz = std::cos(rz), sz = std::sin(rz);
    ProVector vx{cz * cy, sz * cy, -sy};
    ProVector vy{cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx};
    ProVector vz{cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx};
    return ProMatrixInit(vx, vy, vz, origin, out);
}

struct NamedFeatSearchCtx {
    const wchar_t *target_name = nullptr;
    ProFeature found_feat{};
    int found = 0;
};

inline ProError visit_feat_by_name_action(ProFeature *p_feat, ProError status, ProAppData data) {
    auto *ctx = static_cast<NamedFeatSearchCtx *>(data);
    ProName wname{};
    (void)status;
    if (ProModelitemNameGet(reinterpret_cast<ProModelitem *>(p_feat), wname) == PRO_TK_NO_ERROR) {
        if (_wcsicmp(wname, ctx->target_name) == 0) {
            ctx->found_feat = *p_feat;
            ctx->found = 1;
            return PRO_TK_USER_ABORT;
        }
    }
    return PRO_TK_NO_ERROR;
}

struct AsmSrfSearchCtx {
    const Json *rule = nullptr;
    ProSrftype target_type = PRO_SRF_PLANE;
    ProSurface matched = nullptr;
    int found = 0;
};

inline ProError visit_asm_srf_match_action(ProSurface surf, ProError status, ProAppData data) {
    auto *ctx = static_cast<AsmSrfSearchCtx *>(data);
    ProGeomitemdata *gdata = nullptr;
    (void)status;
    if (ProSurfaceDataGet(surf, &gdata) != PRO_TK_NO_ERROR || !gdata) return PRO_TK_NO_ERROR;
    ProSurfacedata *srf = PRO_SURF_DATA(gdata);
    bool ok = (srf && srf->type == ctx->target_type);
    const Json &r = *(ctx->rule);
    double tol = r.value("tol", 1.5);

    if (ok && ctx->target_type == PRO_SRF_CYL) {
        if (r.contains("radius")) {
            if (std::fabs(srf->srf_shape.cylinder.radius - r["radius"].get<double>()) > tol) ok = false;
        }
        if (ok && r.contains("axis_dir")) {
            double dx = r["axis_dir"].at(0).get<double>();
            double dy = r["axis_dir"].at(1).get<double>();
            double dz = r["axis_dir"].at(2).get<double>();
            double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            double dot = (len > 1e-9)
                             ? (srf->srf_shape.cylinder.e3[0] * dx + srf->srf_shape.cylinder.e3[1] * dy +
                                srf->srf_shape.cylinder.e3[2] * dz) /
                                   len
                             : 0.0;
            if (std::fabs(dot) < 0.995) ok = false;
        }
        if (ok && r.contains("axis_point")) {
            double px = r["axis_point"].at(0).get<double>() - srf->srf_shape.cylinder.origin[0];
            double py = r["axis_point"].at(1).get<double>() - srf->srf_shape.cylinder.origin[1];
            double pz = r["axis_point"].at(2).get<double>() - srf->srf_shape.cylinder.origin[2];
            double ex = srf->srf_shape.cylinder.e3[0], ey = srf->srf_shape.cylinder.e3[1],
                   ez = srf->srf_shape.cylinder.e3[2];
            double proj = px * ex + py * ey + pz * ez;
            double rx = px - proj * ex, ry = py - proj * ey, rz = pz - proj * ez;
            if (std::sqrt(rx * rx + ry * ry + rz * rz) > tol) ok = false;
        }
    } else if (ok && ctx->target_type == PRO_SRF_PLANE) {
        if (r.contains("normal")) {
            double nx = r["normal"].at(0).get<double>();
            double ny = r["normal"].at(1).get<double>();
            double nz = r["normal"].at(2).get<double>();
            double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            double dot = (len > 1e-9)
                             ? (srf->srf_shape.plane.e3[0] * nx + srf->srf_shape.plane.e3[1] * ny +
                                srf->srf_shape.plane.e3[2] * nz) /
                                   len
                             : 0.0;
            if (r.value("signed_normal", false) ? (dot < 0.995) : (std::fabs(dot) < 0.995)) ok = false;
        }
        if (ok && r.contains("plane_offset")) {
            double nx = srf->srf_shape.plane.e3[0], ny = srf->srf_shape.plane.e3[1],
                   nz = srf->srf_shape.plane.e3[2];
            const double *o = srf->srf_shape.plane.origin;
            double d = o[0] * nx + o[1] * ny + o[2] * nz;
            if (r.contains("normal")) {
                double snx = r["normal"].at(0).get<double>();
                double sny = r["normal"].at(1).get<double>();
                double snz = r["normal"].at(2).get<double>();
                double slen = std::sqrt(snx * snx + sny * sny + snz * snz);
                if (slen > 1e-9) {
                    d = o[0] * (snx / slen) + o[1] * (sny / slen) + o[2] * (snz / slen);
                }
            }
            if (std::fabs(d - r["plane_offset"].get<double>()) > tol) ok = false;
        }
        if (ok && r.contains("origin_near")) {
            double t[3]{
                r["origin_near"].at(0).get<double>(),
                r["origin_near"].at(1).get<double>(),
                r["origin_near"].at(2).get<double>()
            };
            double dx = srf->srf_shape.plane.origin[0] - t[0];
            double dy = srf->srf_shape.plane.origin[1] - t[1];
            double dz = srf->srf_shape.plane.origin[2] - t[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > tol) ok = false;
        }
    }

    ProGeomitemdataFree(&gdata);
    if (ok) {
        ctx->matched = surf;
        ctx->found = 1;
        return PRO_TK_USER_ABORT;
    }
    return PRO_TK_NO_ERROR;
}

inline ProError resolve_model_item_sel(
    ProMdl mdl,
    ProAsmcomppath *path_or_null,
    const Json &ref_spec,
    ProSelection *out_sel,
    FILE *log
) {
    *out_sel = nullptr;
    std::string kind = "auto";
    std::string name;

    if (ref_spec.is_string()) {
        name = ref_spec.get<std::string>();
    } else if (ref_spec.is_object()) {
        if (ref_spec.contains("cylindrical_face")) {
            ProSolid solid = reinterpret_cast<ProSolid>(mdl);
            ProSolidBody body{};
            if (ProSolidDefaultBodyGet(solid, &body) != PRO_TK_NO_ERROR || body.id == PRO_VALUE_UNUSED) {
                return PRO_TK_E_NOT_FOUND;
            }
            AsmSrfSearchCtx sctx{&ref_spec["cylindrical_face"], PRO_SRF_CYL, nullptr, 0};
            ProSolidBodySurfaceVisit(&body, visit_asm_srf_match_action, &sctx);
            if (!sctx.found || !sctx.matched) return PRO_TK_E_NOT_FOUND;
            ProModelitem item{};
            ProError st = ProSurfaceToGeomitem(solid, sctx.matched, &item);
            if (st != PRO_TK_NO_ERROR) return st;
            return ProSelectionAlloc(path_or_null, &item, out_sel);
        }
        if (ref_spec.contains("planar_face")) {
            ProSolid solid = reinterpret_cast<ProSolid>(mdl);
            ProSolidBody body{};
            if (ProSolidDefaultBodyGet(solid, &body) != PRO_TK_NO_ERROR || body.id == PRO_VALUE_UNUSED) {
                return PRO_TK_E_NOT_FOUND;
            }
            AsmSrfSearchCtx sctx{&ref_spec["planar_face"], PRO_SRF_PLANE, nullptr, 0};
            ProSolidBodySurfaceVisit(&body, visit_asm_srf_match_action, &sctx);
            if (!sctx.found || !sctx.matched) return PRO_TK_E_NOT_FOUND;
            ProModelitem item{};
            ProError st = ProSurfaceToGeomitem(solid, sctx.matched, &item);
            if (st != PRO_TK_NO_ERROR) return st;
            return ProSelectionAlloc(path_or_null, &item, out_sel);
        }
        kind = ref_spec.value("kind", "auto");
        name = ref_spec.value("name", "");
    } else {
        return PRO_TK_BAD_INPUTS;
    }

    if (name.empty()) return PRO_TK_BAD_INPUTS;
    std::wstring wname = utf8_to_w(name);
    ProType try_types[4]{PRO_SURFACE, PRO_AXIS, PRO_CSYS, PRO_POINT};
    int n_try = 4;
    if (kind == "plane" || kind == "surface") {
        try_types[0] = PRO_SURFACE;
        n_try = 1;
    } else if (kind == "axis") {
        try_types[0] = PRO_AXIS;
        n_try = 1;
    } else if (kind == "csys") {
        try_types[0] = PRO_CSYS;
        n_try = 1;
    } else if (kind == "point") {
        try_types[0] = PRO_POINT;
        n_try = 1;
    }

    for (int i = 0; i < n_try; ++i) {
        ProModelitem item{};
        if (ProModelitemByNameInit(mdl, try_types[i], const_cast<wchar_t *>(wname.c_str()), &item) == PRO_TK_NO_ERROR) {
            return ProSelectionAlloc(path_or_null, &item, out_sel);
        }
    }

    /* Fallback: match datum feature name, then extract its child geomitem */
    NamedFeatSearchCtx fctx{wname.c_str(), {}, 0};
    ProSolidFeatVisit(reinterpret_cast<ProSolid>(mdl), visit_feat_by_name_action, nullptr, &fctx);
    if (fctx.found) {
        for (int i = 0; i < n_try; ++i) {
            GeomFindCtx gctx{};
            ProFeatureGeomitemVisit(&fctx.found_feat, try_types[i], visit_first_geom_action, nullptr, &gctx);
            if (gctx.found) {
                return ProSelectionAlloc(path_or_null, &gctx.item, out_sel);
            }
        }
    }
    log_printf(log, "ASM_REF_NOT_FOUND kind=%s name=%s\n", kind.c_str(), name.c_str());
    return PRO_TK_E_NOT_FOUND;
}

inline ProError resolve_asm_side_sel(
    PlanContext &ctx,
    const Json &spec,
    const char *comp_key,
    const char *ref_key,
    ProSelection *out_sel
) {
    *out_sel = nullptr;
    ProMdl target_mdl = ctx.mdl;
    ProIdTable ids{};
    int levels = 0;

    if (spec.contains(comp_key) && spec[comp_key].is_string() && !spec[comp_key].get<std::string>().empty()) {
        std::string ckey = spec[comp_key].get<std::string>();
        auto cit = ctx.components.find(ckey);
        if (cit == ctx.components.end()) {
            log_printf(ctx.log, "ASM_COMP_NOT_FOUND key=%s\n", ckey.c_str());
            return PRO_TK_E_NOT_FOUND;
        }
        ProError st = ProAsmcompMdlGet(&cit->second, &target_mdl);
        if (st != PRO_TK_NO_ERROR || !target_mdl) return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
        ids[0] = cit->second.id;
        levels = 1;
    }

    ProAsmcomppath cpath{};
    ProError st = ProAsmcomppathInit(ctx.solid, ids, levels, &cpath);
    if (st != PRO_TK_NO_ERROR) return st;
    return resolve_model_item_sel(target_mdl, &cpath, spec.at(ref_key), out_sel, ctx.log);
}

inline void log_component_placement(FILE *log, ProSolid asm_solid, const char *tag, ProAsmcomp *comp) {
    ProBoolean is_placed = PRO_B_FALSE;
    ProBoolean under = PRO_B_FALSE;
    ProFeatStatus fstat = PRO_FEAT_INVALID;
    ProFeatureStatusGet(comp, &fstat);
    ProAsmcompIsPlaced(comp, &is_placed);
    ProAsmcompIsUnderconstrained(comp, &under);
    ProIdTable ids{comp->id};
    ProAsmcomppath path{};
    ProMatrix trf{};
    ProError pst = ProAsmcomppathInit(asm_solid, ids, 1, &path);
    if (pst == PRO_TK_NO_ERROR) {
        pst = ProAsmcomppathTrfGet(&path, PRO_B_TRUE, trf);
    }
    log_printf(
        log,
        "COMP_POSE[%s] id=%d status=%d placed=%d underconstr=%d trf_st=%d origin=(%.3f,%.3f,%.3f) "
        "vx=(%.4f,%.4f,%.4f) vy=(%.4f,%.4f,%.4f) vz=(%.4f,%.4f,%.4f)\n",
        tag,
        comp->id,
        static_cast<int>(fstat),
        static_cast<int>(is_placed),
        static_cast<int>(under),
        static_cast<int>(pst),
        trf[3][0], trf[3][1], trf[3][2],
        trf[0][0], trf[0][1], trf[0][2],
        trf[1][0], trf[1][1], trf[1][2],
        trf[2][0], trf[2][1], trf[2][2]);
}

/* ------------------------------------------------------------------ */
/* Appearance, material finish, surface filtering, and texture maps   */
/* ------------------------------------------------------------------ */

struct SrfBboxAccum {
    ProSurface surf = nullptr;
    double bmin[3]{1e30, 1e30, 1e30};
    double bmax[3]{-1e30, -1e30, -1e30};
    int sample_count = 0;
};

inline ProError visit_srf_edge_pts_cb(ProEdge edge, ProError status, ProAppData data) {
    auto *acc = static_cast<SrfBboxAccum *>(data);
    ProGeomitemdata *gdata = nullptr;
    (void)status;
    if (ProEdgeDataGet(edge, &gdata) == PRO_TK_NO_ERROR && gdata) {
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            ProVector xyz{};
            if (ProEdgeXyzdataEval(edge, t, xyz, nullptr, nullptr, nullptr) == PRO_TK_NO_ERROR) {
                for (int k = 0; k < 3; ++k) {
                    acc->bmin[k] = std::min(acc->bmin[k], xyz[k]);
                    acc->bmax[k] = std::max(acc->bmax[k], xyz[k]);
                }
                acc->sample_count++;
            }
        }
        ProGeomitemdataFree(&gdata);
    }
    return PRO_TK_NO_ERROR;
}

inline ProError visit_srf_contour_bbox_cb(ProContour contour, ProError status, ProAppData data) {
    auto *acc = static_cast<SrfBboxAccum *>(data);
    (void)status;
    return ProContourEdgeVisit(acc->surf, contour, visit_srf_edge_pts_cb, nullptr, acc);
}

struct SolidSrfFilterCtx {
    ProSolid solid = nullptr;
    const Json *filter = nullptr;
    std::vector<int> matched_ids;
    std::set<int> all_body_ids;
};

inline std::string srftype_to_str(ProSrftype t) {
    switch (t) {
    case PRO_SRF_PLANE: return "plane";
    case PRO_SRF_CYL: return "cylinder";
    case PRO_SRF_CONE: return "cone";
    case PRO_SRF_TORUS: return "torus";
    case PRO_SRF_REV: return "revolve";
    default: return "other";
    }
}

inline ProError visit_solid_srf_filter_cb(ProSurface surf, ProError status, ProAppData data) {
    auto *ctx = static_cast<SolidSrfFilterCtx *>(data);
    int sid = -1;
    (void)status;
    if (ProSurfaceIdGet(surf, &sid) != PRO_TK_NO_ERROR || sid < 0) return PRO_TK_NO_ERROR;
    ctx->all_body_ids.insert(sid);
    if (!ctx->filter) {
        ctx->matched_ids.push_back(sid);
        return PRO_TK_NO_ERROR;
    }

    const Json &f = *(ctx->filter);
    ProGeomitemdata *gdata = nullptr;
    if (ProSurfaceDataGet(surf, &gdata) != PRO_TK_NO_ERROR || !gdata) return PRO_TK_NO_ERROR;
    ProSurfacedata *srf = PRO_SURF_DATA(gdata);
    if (!srf) {
        ProGeomitemdataFree(&gdata);
        return PRO_TK_NO_ERROR;
    }

    bool ok = true;
    std::string stype = srftype_to_str(srf->type);
    if (f.contains("surface_types") && f["surface_types"].is_array()) {
        bool type_hit = false;
        for (const Json &st : f["surface_types"]) {
            if (st.get<std::string>() == stype) {
                type_hit = true;
                break;
            }
        }
        if (!type_hit) ok = false;
    }

    if (ok && f.contains("cyl_radius_range")) {
        if (srf->type != PRO_SRF_CYL) {
            ok = false;
        } else {
            double r = srf->srf_shape.cylinder.radius;
            double lo = f["cyl_radius_range"].at(0).get<double>();
            double hi = f["cyl_radius_range"].at(1).get<double>();
            if (r < lo || r > hi) ok = false;
        }
    }
    if (ok && f.contains("cyl_axis_dir")) {
        if (srf->type != PRO_SRF_CYL) {
            ok = false;
        } else {
            double dx = f["cyl_axis_dir"].at(0).get<double>();
            double dy = f["cyl_axis_dir"].at(1).get<double>();
            double dz = f["cyl_axis_dir"].at(2).get<double>();
            double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            double dot = (len > 1e-9)
                             ? (srf->srf_shape.cylinder.e3[0] * dx + srf->srf_shape.cylinder.e3[1] * dy +
                                srf->srf_shape.cylinder.e3[2] * dz) /
                                   len
                             : 0.0;
            if (std::fabs(dot) < 0.99) ok = false;
        }
    }
    if (ok && f.contains("plane_normal")) {
        if (srf->type != PRO_SRF_PLANE) {
            ok = false;
        } else {
            double nx = f["plane_normal"].at(0).get<double>();
            double ny = f["plane_normal"].at(1).get<double>();
            double nz = f["plane_normal"].at(2).get<double>();
            double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            double dot = (len > 1e-9)
                             ? (srf->srf_shape.plane.e3[0] * nx + srf->srf_shape.plane.e3[1] * ny +
                                srf->srf_shape.plane.e3[2] * nz) /
                                   len
                             : 0.0;
            if (f.value("signed_normal", false) ? (dot < 0.99) : (std::fabs(dot) < 0.99)) ok = false;
        }
    }
    ProGeomitemdataFree(&gdata);
    if (!ok) return PRO_TK_NO_ERROR;

    bool need_bbox = f.contains("x_range") || f.contains("y_range") || f.contains("z_range") ||
                     f.contains("center_r_range") || f.contains("bbox_inside");
    if (need_bbox) {
        SrfBboxAccum acc;
        acc.surf = surf;
        ProSurfaceContourVisit(surf, visit_srf_contour_bbox_cb, nullptr, &acc);
        if (acc.sample_count <= 0) return PRO_TK_NO_ERROR;
        double cx = 0.5 * (acc.bmin[0] + acc.bmax[0]);
        double cy = 0.5 * (acc.bmin[1] + acc.bmax[1]);
        double cz = 0.5 * (acc.bmin[2] + acc.bmax[2]);
        double tol = f.value("tol", 0.5);
        bool strict_bounds = f.value("bbox_inside", false);

        auto check_rng = [&](const char *key, int axis, double cval) {
            if (!f.contains(key)) return true;
            double lo = f[key].at(0).get<double>() - tol;
            double hi = f[key].at(1).get<double>() + tol;
            if (strict_bounds) {
                return acc.bmin[axis] >= lo && acc.bmax[axis] <= hi;
            }
            return cval >= lo && cval <= hi;
        };
        if (!check_rng("x_range", 0, cx) || !check_rng("y_range", 1, cy) || !check_rng("z_range", 2, cz)) {
            ok = false;
        }
        if (ok && f.contains("center_r_range")) {
            double ox = f.value("axis_origin_x", 0.0);
            double oz = f.value("axis_origin_z", 0.0);
            double r = std::sqrt((cx - ox) * (cx - ox) + (cz - oz) * (cz - oz));
            double lo = f["center_r_range"].at(0).get<double>();
            double hi = f["center_r_range"].at(1).get<double>();
            if (r < lo || r > hi) ok = false;
        }
    }

    if (ok) ctx->matched_ids.push_back(sid);
    return PRO_TK_NO_ERROR;
}

inline ProError collect_solid_surface_ids_by_filter(
    ProSolid solid,
    const Json *filter_or_null,
    std::vector<int> &out_ids,
    std::set<int> *out_all_body_ids = nullptr
) {
    out_ids.clear();
    ProSolidBody body{};
    if (ProSolidDefaultBodyGet(solid, &body) != PRO_TK_NO_ERROR || body.id == PRO_VALUE_UNUSED) {
        return PRO_TK_E_NOT_FOUND;
    }
    SolidSrfFilterCtx ctx{solid, filter_or_null, {}, {}};
    ProSolidBodySurfaceVisit(&body, visit_solid_srf_filter_cb, &ctx);
    out_ids = std::move(ctx.matched_ids);
    if (out_all_body_ids) *out_all_body_ids = std::move(ctx.all_body_ids);
    return PRO_TK_NO_ERROR;
}

inline ProError collect_feature_live_surface_ids(
    PlanContext &ctx,
    const std::string &fkey,
    const std::set<int> &live_body_srf_ids,
    std::vector<int> &out_ids
) {
    auto fit = ctx.features.find(fkey);
    if (fit == ctx.features.end()) return PRO_TK_E_NOT_FOUND;
    std::vector<ProFeature> feats_to_scan;
    auto append_feat_or_pattern = [&](const ProFeature &src_feat) {
        feats_to_scan.push_back(src_feat);
        ProFeature mutable_f = src_feat;
        ProFeattype ftype = PRO_FEAT_FIRST_FEAT;
        if (ProFeatureTypeGet(&mutable_f, &ftype) == PRO_TK_NO_ERROR && ftype == PRO_FEAT_PATTERN_HEAD) {
            ProPattern pat{};
            for (ProPatternClass pc : {PRO_FEAT_PATTERN, PRO_GROUP_PATTERN}) {
                if (ProFeaturePatternGet(&mutable_f, pc, &pat) == PRO_TK_NO_ERROR) {
                    ProFeature *members = nullptr;
                    if (ProPatternMembersGet(&pat, &members) == PRO_TK_NO_ERROR && members) {
                        int n = 0;
                        ProArraySizeGet(reinterpret_cast<ProArray>(members), &n);
                        for (int i = 0; i < n; ++i) feats_to_scan.push_back(members[i]);
                        ProArrayFree(reinterpret_cast<ProArray *>(&members));
                    }
                    break;
                }
            }
        }
    };
    append_feat_or_pattern(fit->second);
    auto git = ctx.group_member_ids.find(fkey);
    if (git != ctx.group_member_ids.end()) {
        for (int mfid : git->second) {
            ProFeature mf{};
            if (ProFeatureInit(ctx.solid, mfid, &mf) == PRO_TK_NO_ERROR) {
                append_feat_or_pattern(mf);
            }
        }
    }

    for (ProFeature &f : feats_to_scan) {
        GeomIdListCtx slist;
        ProFeatureGeomitemVisit(&f, PRO_SURFACE, visit_geom_id_action, nullptr, &slist);
        for (int sid : slist.ids) {
            if (live_body_srf_ids.empty() || live_body_srf_ids.count(sid)) {
                out_ids.push_back(sid);
            }
        }
    }
    return PRO_TK_NO_ERROR;
}

inline void fill_appearance_props_from_json(const Json &op, ProSurfaceAppearanceProps &props, const std::string &id_tag) {
    std::memset(&props, 0, sizeof(props));
    std::string preset = op.value("preset", "matte_paint");

    double amb = 0.45, diff = 0.78, hi = 0.35, shine = 0.30, refl = 0.08, transp = 0.0;
    bool metal_hi = false;
    if (preset == "gunmetal") {
        amb = 0.52; diff = 0.78; hi = 0.75; shine = 0.85; refl = 0.45; metal_hi = true;
    } else if (preset == "brushed_steel") {
        amb = 0.55; diff = 0.80; hi = 0.80; shine = 0.90; refl = 0.55; metal_hi = true;
    } else if (preset == "heat_metal") {
        amb = 0.50; diff = 0.75; hi = 0.78; shine = 0.78; refl = 0.38; metal_hi = true;
    } else if (preset == "rubber" || preset == "matte_paint" || preset == "military_paint") {
        amb = 0.48; diff = 0.80; hi = 0.30; shine = 0.25; refl = 0.05;
    } else if (preset == "white_tex_base") {
        amb = 0.55; diff = 0.88; hi = 0.25; shine = 0.20; refl = 0.04;
    }

    amb = op.value("ambient", amb);
    diff = op.value("diffuse", diff);
    hi = op.value("highlite", hi);
    shine = op.value("shininess", shine);
    refl = op.value("reflection", refl);
    transp = op.value("transparency", transp);

    double rgb[3]{1.0, 1.0, 1.0};
    if (op.contains("rgb") && op["rgb"].is_array() && op["rgb"].size() == 3) {
        bool byte_scale = false;
        for (int i = 0; i < 3; ++i) {
            rgb[i] = op["rgb"].at(i).get<double>();
            if (rgb[i] > 1.0) byte_scale = true;
        }
        if (byte_scale) {
            for (int i = 0; i < 3; ++i) rgb[i] /= 255.0;
        }
    }
    double hrgb[3]{metal_hi ? 1.0 : rgb[0], metal_hi ? 1.0 : rgb[1], metal_hi ? 1.0 : rgb[2]};
    if (op.contains("highlight_rgb") && op["highlight_rgb"].is_array() && op["highlight_rgb"].size() == 3) {
        bool byte_scale = false;
        for (int i = 0; i < 3; ++i) {
            hrgb[i] = op["highlight_rgb"].at(i).get<double>();
            if (hrgb[i] > 1.0) byte_scale = true;
        }
        if (byte_scale) {
            for (int i = 0; i < 3; ++i) hrgb[i] /= 255.0;
        }
    }

    props.ambient = amb;
    props.diffuse = diff;
    props.highlite = hi;
    props.shininess = shine;
    props.reflection = refl;
    props.transparency = transp;
    for (int i = 0; i < 3; ++i) {
        props.color_rgb[i] = rgb[i];
        props.highlight_color[i] = hrgb[i];
    }

    std::string app_name = op.value("appearance_name", "cb_" + id_tag);
    std::string app_label = op.value("appearance_label", app_name);
    std::string app_desc = op.value("description", "CreoBuild procedural material appearance");
    wcsncpy_s(props.name, PRO_PATH_SIZE, utf8_to_w(app_name).c_str(), _TRUNCATE);
    wcsncpy_s(props.label, PRO_PATH_SIZE, utf8_to_w(app_label).c_str(), _TRUNCATE);
    wcsncpy_s(props.description, PRO_PATH_SIZE, utf8_to_w(app_desc).c_str(), _TRUNCATE);
    wcsncpy_s(props.keywords, PRO_PATH_SIZE, L"creobuild material", _TRUNCATE);
}

inline void fill_texture_placement_from_json(const Json &op, ProSurfaceTexturePlacementProps &tp) {
    std::memset(&tp, 0, sizeof(tp));
    std::string proj = op.value("projection", "cylindrical");
    if (proj == "planar") tp.projection = PRO_TEXTURE_PROJECTION_PLANER;
    else if (proj == "spherical") tp.projection = PRO_TEXTURE_PROJECTION_SPHERICAL;
    else if (proj == "box") tp.projection = PRO_TEXTURE_PROJECTION_BOX;
    else tp.projection = PRO_TEXTURE_PROJECTION_CYLINDRICAL;

    tp.texture_type = PRO_COLOR_TEXTURE;
    double vx[3]{1.0, 0.0, 0.0};
    double vy[3]{0.0, 0.0, -1.0};
    double vz[3]{0.0, 1.0, 0.0};
    double orig[3]{0.0, 0.0, 0.0};
    auto read_v3 = [&](const char *k, double out[3]) {
        if (op.contains(k) && op[k].is_array() && op[k].size() == 3) {
            out[0] = op[k].at(0).get<double>();
            out[1] = op[k].at(1).get<double>();
            out[2] = op[k].at(2).get<double>();
        }
    };
    read_v3("csys_x", vx);
    read_v3("csys_y", vy);
    read_v3("csys_z", vz);
    read_v3("csys_origin", orig);
    for (int c = 0; c < 3; ++c) {
        tp.local_csys[0][c] = vx[c];
        tp.local_csys[1][c] = vy[c];
        tp.local_csys[2][c] = vz[c];
        tp.local_csys[3][c] = orig[c];
    }
    tp.horizontal_offset = op.value("u_offset", 0.0);
    tp.vertical_offset = op.value("v_offset", 0.0);
    tp.rotate = op.value("rotate_deg", 0.0);
    tp.horizontal_scale = op.value("u_scale", 1.0);
    tp.vertical_scale = op.value("v_scale", 1.0);
    tp.bump_height = 0.0;
    tp.decal_intensity = 1.0;
    tp.flip_horizontal = PRO_B_FALSE;
    tp.flip_vertical = PRO_B_FALSE;
}

} // namespace creobuild
