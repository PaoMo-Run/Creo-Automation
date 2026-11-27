#pragma once
/*
 * Creo Parametric 8.0 GB/T Engineering Drawing Engine (C++17 Pro/TOOLKIT).
 *
 * Generates native associative Creo drawings (.drw) with:
 *   - GB/T 14689-2008 A3 (420x297 mm) binding frame (a=25, c=5), center marks
 *   - Simplified academic/course-design GB/T Chinese title block (ProDwgtable + 1st-angle symbol)
 *   - First-angle orthographic views, planar full cross-sections, vertical broken views,
 *     and circular enlarged detail views (2:1)
 *   - Associative model dimensions (linear, diameter, radius) with ISO/GB fit tolerances
 *   - GB/T 131-2006 surface roughness symbols (Ra) and Chinese technical requirements
 *   - Monochrome high-DPI PDF export (ProPDFExport) & AutoCAD DWG export (Pro2dExport)
 *   - Isolated reopen + sheet regeneration verification
 */

#include "creo_plan_core.hpp"

#include <ProAnnotation.h>
#include <ProDimension.h>
#include <ProDrawing.h>
#include <ProDrawingView.h>
#include <ProDtlattach.h>
#include <ProDtlentity.h>
#include <ProDtlnote.h>
#include <ProDtlsymdef.h>
#include <ProDtlsyminst.h>
#include <ProDwgtable.h>
#include <ProNote.h>
#include <ProPDF.h>
#include <ProXsec.h>

namespace creobuild {

namespace dwg {

struct SheetCoord {
    ProMatrix scn_to_drw{};
    ProMatrix drw_to_scn{};
    double width_mm = 420.0;
    double height_mm = 297.0;
    double scale_mm_to_scn = 1.0;

    static bool invert_4x4(const ProMatrix m, ProMatrix out) {
        double a[4][8]{};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) a[r][c] = m[r][c];
            for (int c = 0; c < 4; ++c) a[r][4 + c] = (r == c) ? 1.0 : 0.0;
        }
        for (int i = 0; i < 4; ++i) {
            int piv = i;
            for (int r = i + 1; r < 4; ++r) {
                if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
            }
            if (std::fabs(a[piv][i]) < 1e-12) return false;
            if (piv != i) {
                for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[piv][c]);
            }
            double diag = a[i][i];
            for (int c = 0; c < 8; ++c) a[i][c] /= diag;
            for (int r = 0; r < 4; ++r) {
                if (r == i) continue;
                double f = a[r][i];
                for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c];
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r][c] = a[r][4 + c];
        return true;
    }

    void init(ProDrawing drw, int sheet, FILE *log) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                scn_to_drw[r][c] = drw_to_scn[r][c] = (r == c) ? 1.0 : 0.0;
        ProPlotPaperSize psize = A3_SIZE_PLOT;
        double sw = 420.0, sh = 297.0;
        ProError sz_st = ProDrawingSheetSizeGet(drw, sheet, &psize, &sw, &sh);
        if (sz_st == PRO_TK_NO_ERROR && sw > 10.0 && sh > 10.0) {
            width_mm = sw;
            height_mm = sh;
        }
        ProName sz_name{};
        ProError trf_st = ProDrawingSheetTrfGet(drw, sheet, sz_name, scn_to_drw);
        bool inv_ok = (trf_st == PRO_TK_NO_ERROR) && invert_4x4(scn_to_drw, drw_to_scn);
        scale_mm_to_scn = std::hypot(drw_to_scn[0][0], drw_to_scn[0][1]);
        if (scale_mm_to_scn < 1e-6) scale_mm_to_scn = 1.0;
        log_printf(
            log,
            "SHEET_COORD sz_st=%d size=%.2fx%.2f trf_st=%d inv=%d s2d_00=%.6f d2s_00=%.6f d2s_30=%.3f d2s_31=%.3f\n",
            static_cast<int>(sz_st), width_mm, height_mm, static_cast<int>(trf_st),
            inv_ok ? 1 : 0, scn_to_drw[0][0], drw_to_scn[0][0], drw_to_scn[3][0], drw_to_scn[3][1]);
    }

    void mm_to_scn(double x_mm, double y_mm, ProPoint3d out) const {
        ProPoint3d p{x_mm, y_mm, 0.0};
        ProPntTrfEval(p, const_cast<double (*)[4]>(drw_to_scn), out);
        out[2] = 0.0;
    }

    void scn_to_mm(const ProPoint3d in, double &x_mm, double &y_mm) const {
        ProPoint3d p{in[0], in[1], 0.0}, out{};
        ProPntTrfEval(p, const_cast<double (*)[4]>(scn_to_drw), out);
        x_mm = out[0];
        y_mm = out[1];
    }
};

inline ProError set_dtl_opt(ProDrawing drw, const wchar_t *opt, const wchar_t *val, FILE *log) {
    ProName wopt{};
    ProLine wval{};
    wcsncpy_s(wopt, PRO_NAME_SIZE, opt, _TRUNCATE);
    wcsncpy_s(wval, PRO_LINE_SIZE, val, _TRUNCATE);
    ProError st = ProDrawingSetupOptionSet(drw, wopt, wval);
    if (st != PRO_TK_NO_ERROR && log) {
        log_printf(log, "  DTL_OPT_WARN[%s=%s] st=%d\n", w2utf8(opt).c_str(), w2utf8(val).c_str(), static_cast<int>(st));
    }
    return st;
}

inline ProError draw_line_scn(
    ProDrawing drw,
    ProView attach_view,
    const ProPoint3d p1,
    const ProPoint3d p2,
    double width_scn,
    const wchar_t *line_font,
    ProColortype color_type = PRO_COLOR_LETTER
) {
    ProDtlentitydata edata = nullptr;
    ProError st = ProDtlentitydataAlloc(drw, &edata);
    if (st != PRO_TK_NO_ERROR) return st;

    ProCurvedata *crv = nullptr;
    ProCurvedataAlloc(&crv);
    ProLinedataInit(const_cast<double *>(p1), const_cast<double *>(p2), crv);
    ProDtlentitydataCurveSet(edata, crv);
    ProCurvedataFree(crv);

    ProView v = attach_view;
    if (!v) ProDrawingBackgroundViewGet(drw, 1, &v);
    if (v) ProDtlentitydataViewSet(edata, v);

    ProColor col{};
    col.method = PRO_COLOR_METHOD_TYPE;
    col.value.type = color_type;
    ProDtlentitydataColorSet(edata, &col);

    if (line_font && line_font[0]) {
        ProName wf{};
        wcsncpy_s(wf, PRO_NAME_SIZE, line_font, _TRUNCATE);
        ProDtlentitydataFontSet(edata, wf);
    }
    if (width_scn > 0.0) {
        ProDtlentitydataWidthSet(edata, width_scn);
    }

    ProDtlentity ent{};
    st = ProDtlentityCreate(drw, nullptr, edata, &ent);
    ProDtlentitydataFree(edata);
    if (st == PRO_TK_NO_ERROR) ProDtlentityDraw(&ent);
    return st;
}

inline ProError draw_line_mm(
    ProDrawing drw,
    const SheetCoord &sc,
    ProView attach_view,
    double x1,
    double y1,
    double x2,
    double y2,
    double width_mm,
    const wchar_t *line_font = L"SOLIDFONT",
    ProColortype color_type = PRO_COLOR_LETTER
) {
    ProPoint3d p1{}, p2{};
    sc.mm_to_scn(x1, y1, p1);
    sc.mm_to_scn(x2, y2, p2);
    return draw_line_scn(drw, attach_view, p1, p2, width_mm * sc.scale_mm_to_scn, line_font, color_type);
}

inline ProError draw_arc_mm(
    ProDrawing drw,
    const SheetCoord &sc,
    ProView attach_view,
    double cx,
    double cy,
    double r_mm,
    double ang0_rad,
    double ang1_rad,
    double width_mm,
    const wchar_t *line_font = L"SOLIDFONT",
    ProColortype color_type = PRO_COLOR_LETTER
) {
    ProDtlentitydata edata = nullptr;
    ProError st = ProDtlentitydataAlloc(drw, &edata);
    if (st != PRO_TK_NO_ERROR) return st;

    ProPoint3d cen{};
    sc.mm_to_scn(cx, cy, cen);
    ProVector v1{1.0, 0.0, 0.0};
    ProVector v2{0.0, 1.0, 0.0};
    ProCurvedata *crv = nullptr;
    ProCurvedataAlloc(&crv);
    ProArcdataInit(v1, v2, cen, ang0_rad, ang1_rad, r_mm * sc.scale_mm_to_scn, crv);
    ProDtlentitydataCurveSet(edata, crv);
    ProCurvedataFree(crv);

    ProView v = attach_view;
    if (!v) ProDrawingBackgroundViewGet(drw, 1, &v);
    if (v) ProDtlentitydataViewSet(edata, v);

    ProColor col{};
    col.method = PRO_COLOR_METHOD_TYPE;
    col.value.type = color_type;
    ProDtlentitydataColorSet(edata, &col);
    if (line_font && line_font[0]) {
        ProName wf{};
        wcsncpy_s(wf, PRO_NAME_SIZE, line_font, _TRUNCATE);
        ProDtlentitydataFontSet(edata, wf);
    }
    if (width_mm > 0.0) {
        ProDtlentitydataWidthSet(edata, width_mm * sc.scale_mm_to_scn);
    }

    ProDtlentity ent{};
    st = ProDtlentityCreate(drw, nullptr, edata, &ent);
    ProDtlentitydataFree(edata);
    if (st == PRO_TK_NO_ERROR) ProDtlentityDraw(&ent);
    return st;
}

inline ProTextHrzJustification to_text_hrz(ProHorizontalJustification hj) {
    if (hj == PRO_HORZJUST_CENTER) return PRO_TEXT_HRZJUST_CENTER;
    if (hj == PRO_HORZJUST_RIGHT) return PRO_TEXT_HRZJUST_RIGHT;
    return PRO_TEXT_HRZJUST_LEFT;
}

inline Json dwg_load_plan_json(const char *path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error(std::string("Cannot open JSON plan: ") + path);
    Json j;
    in >> j;
    return j;
}

struct FeatByNameQuery {
    std::wstring target;
    ProFeature found{};
    bool ok = false;
};

inline ProError dwg_feat_name_visit(ProFeature *feat, ProError st, ProAppData appdata) {
    (void)st;
    auto *q = static_cast<FeatByNameQuery *>(appdata);
    ProName nm{};
    if (ProModelitemNameGet(feat, nm) == PRO_TK_NO_ERROR) {
        if (_wcsicmp(nm, q->target.c_str()) == 0) {
            q->found = *feat;
            q->ok = true;
            return PRO_TK_E_FOUND;
        }
    }
    return PRO_TK_NO_ERROR;
}

inline ProError dwg_find_feat_by_name(ProSolid solid, const wchar_t *name, ProFeature *out) {
    FeatByNameQuery q{};
    q.target = name ? name : L"";
    ProSolidFeatVisit(solid, dwg_feat_name_visit, nullptr, &q);
    if (q.ok) {
        *out = q.found;
        return PRO_TK_NO_ERROR;
    }
    return PRO_TK_E_NOT_FOUND;
}

inline std::wstring creo_encode_symbols(const std::wstring &s) {
    // Convert Unicode 'Ø' (U+00D8) to Creo special.fnt diameter escape (Ctrl-A 'n' Ctrl-B)
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t ch : s) {
        if (ch == L'\u00D8') {
            out.push_back(L'\x01');
            out.push_back(L'n');
            out.push_back(L'\x02');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

inline ProError add_note_mm(
    ProDrawing drw,
    const SheetCoord &sc,
    ProView attach_view,
    double x_mm,
    double y_mm,
    const std::vector<std::string> &lines_utf8,
    double height_mm,
    const std::wstring &font_name,
    ProHorizontalJustification hjust = PRO_HORZJUST_LEFT,
    ProVerticalJustification vjust = PRO_VERTJUST_BOTTOM,
    double angle_deg = 0.0,
    bool has_leader = false,
    double leader_x_mm = 0.0,
    double leader_y_mm = 0.0,
    FILE *log = nullptr
) {
    ProDtlnotedata ndata = nullptr;
    ProError st = ProDtlnotedataAlloc(drw, &ndata);
    if (st != PRO_TK_NO_ERROR) return st;

    ProView v = attach_view;
    if (!v) ProDrawingBackgroundViewGet(drw, 1, &v);

    ProPoint3d pos{};
    sc.mm_to_scn(x_mm, y_mm, pos);
    ProDtlattach attach = nullptr;
    ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, v, pos, nullptr, &attach);
    ProDtlnotedataAttachmentSet(ndata, attach);
    ProDtlattachFree(attach);

    if (has_leader) {
        ProPoint3d lpos{};
        sc.mm_to_scn(leader_x_mm, leader_y_mm, lpos);
        ProDtlattach lattach = nullptr;
        ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, v, lpos, nullptr, &lattach);
        ProDtlattachArrowtypeSet(lattach, PROLEADERTYPE_ARROWHEAD);
        ProDtlnotedataLeaderAdd(ndata, lattach);
        ProDtlattachFree(lattach);
    }

    ProDtlnotedataJustifSet(ndata, hjust, vjust);
    if (std::fabs(angle_deg) > 1e-6) ProDtlnotedataAngleSet(ndata, angle_deg);

    ProColor col{};
    col.method = PRO_COLOR_METHOD_TYPE;
    col.value.type = PRO_COLOR_LETTER;
    ProDtlnotedataColorSet(ndata, &col);
    ProDtlnotedataDisplayedSet(ndata, PRO_B_TRUE);

    ProName wf{};
    if (!font_name.empty()) {
        wcsncpy_s(wf, PRO_NAME_SIZE, font_name.c_str(), _TRUNCATE);
    }

    for (const std::string &ln_str : lines_utf8) {
        ProDtlnoteline nline = nullptr;
        ProDtlnotelineAlloc(&nline);
        ProDtlnotetext ntext = nullptr;
        ProDtlnotetextAlloc(&ntext);
        std::wstring ws = creo_encode_symbols(utf8_to_w(ln_str));
        ProDtlnotetextStringSet(ntext, const_cast<wchar_t *>(ws.c_str()));
        ProDtlnotetextHeightSet(ntext, height_mm * sc.scale_mm_to_scn);
        ProDtlnotetextWidthSet(ntext, 0.85);
        ProDtlnotetextSlantSet(ntext, 0.0);
        ProDtlnotetextThicknessSet(ntext, 0.0);
        if (wf[0]) ProDtlnotetextFontSet(ntext, wf);
        ProDtlnotelineTextAdd(nline, ntext);
        ProDtlnotedataLineAdd(ndata, nline);
    }

    ProDtlnote note{};
    st = ProDtlnoteCreate(drw, nullptr, ndata, &note);
    ProDtlnotedataFree(ndata);
    if (st == PRO_TK_NO_ERROR) {
        ProDtlnoteShow(&note);
    } else if (log) {
        log_printf(log, "  NOTE_CREATE_FAIL st=%d first='%s'\n",
                   static_cast<int>(st), lines_utf8.empty() ? "" : lines_utf8[0].c_str());
    }
    return st;
}

/* Draw standard GB/T 4458.1 first-angle projection symbol centered at (cx, cy) */
inline void draw_gb_first_angle_symbol(ProDrawing drw, const SheetCoord &sc, double cx, double cy) {
    // Left: truncated cone (small end on left, large end on right)
    double cone_x0 = cx - 7.5;
    double cone_x1 = cx - 1.5;
    double r_big = 2.6;
    double r_sml = 1.4;
    draw_line_mm(drw, sc, nullptr, cone_x0, cy - r_sml, cone_x0, cy + r_sml, 0.5);
    draw_line_mm(drw, sc, nullptr, cone_x1, cy - r_big, cone_x1, cy + r_big, 0.5);
    draw_line_mm(drw, sc, nullptr, cone_x0, cy + r_sml, cone_x1, cy + r_big, 0.5);
    draw_line_mm(drw, sc, nullptr, cone_x0, cy - r_sml, cone_x1, cy - r_big, 0.5);

    // Right: concentric circles
    double circ_x = cx + 4.2;
    draw_arc_mm(drw, sc, nullptr, circ_x, cy, r_big, 0.0, 2.0 * M_PI, 0.5);
    draw_arc_mm(drw, sc, nullptr, circ_x, cy, r_sml, 0.0, 2.0 * M_PI, 0.5);

    // Centerlines
    draw_line_mm(drw, sc, nullptr, cone_x0 - 1.5, cy, circ_x + r_big + 1.5, cy, 0.18, L"CTRLFONT");
    draw_line_mm(drw, sc, nullptr, circ_x, cy - r_big - 0.9, circ_x, cy + r_big + 0.9, 0.18, L"CTRLFONT");
}

/* Draw GB/T 131-2006 surface roughness symbol at (x_mm, y_mm) pointing down onto surface */
inline void draw_gb_ra_mark(
    ProDrawing drw,
    const SheetCoord &sc,
    ProView view,
    double x_mm,
    double y_mm,
    const std::string &ra_text,
    const std::string &orientation, // "up" (tip at bottom pointing down), "down" (tip at top), "left", "right"
    bool machined,
    const std::wstring &font_name,
    double shelf_len_mm = 0.0,
    bool leader_on_left = false
) {
    // Base GB/T 131-2006 geometry (H = 3.2 mm):
    // Tip at (0,0), short leg 60 deg up-left to (-h1*tan30, h1), long leg 60 deg up-right to (h2*tan30, h2)
    const double h1 = 3.8;
    const double h2 = 8.4;
    const double t30 = 0.577350269; // tan(30 deg)
    const double dx1 = -h1 * t30;   // -2.19
    const double dx2 = h2 * t30;    // +4.85
    const double dy1 = h1;
    const double dy2 = h2;

    double rot_rad = 0.0;
    if (orientation == "down") rot_rad = M_PI;
    else if (orientation == "left") rot_rad = M_PI * 0.5;
    else if (orientation == "right") rot_rad = -M_PI * 0.5;

    auto xf = [&](double lx, double ly, double &ox, double &oy) {
        double c = std::cos(rot_rad), s = std::sin(rot_rad);
        ox = x_mm + lx * c - ly * s;
        oy = y_mm + lx * s + ly * c;
    };

    double p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
    xf(0.0, 0.0, p0x, p0y);
    xf(dx1, dy1, p1x, p1y);
    xf(dx2, dy2, p2x, p2y);
    xf(h1 * t30, h1, p3x, p3y); // closing bar for material removal

    draw_line_mm(drw, sc, view, p0x, p0y, p1x, p1y, 0.25);
    draw_line_mm(drw, sc, view, p0x, p0y, p2x, p2y, 0.25);
    if (machined) {
        draw_line_mm(drw, sc, view, p1x, p1y, p3x, p3y, 0.25);
    }

    if (shelf_len_mm > 0.5) {
        double sx = p2x + (leader_on_left ? -shelf_len_mm : shelf_len_mm);
        double sy = p2y;
        draw_line_mm(drw, sc, view, p2x, p2y, sx, sy, 0.25);
        double tx = std::min(p2x, sx) + 1.0;
        double ty = sy + 2.2;
        add_note_mm(drw, sc, view, tx, ty, {ra_text}, 2.5, font_name, PRO_HORZJUST_LEFT, PRO_VERTJUST_BOTTOM, 0.0);
    } else if (orientation == "up") {
        // Place upright Ra text cleanly above the closing bar, shifted slightly left of the long leg
        add_note_mm(drw, sc, view, x_mm - 0.8, y_mm + h1 + 2.2, {ra_text}, 2.4, font_name, PRO_HORZJUST_CENTER, PRO_VERTJUST_BOTTOM, 0.0);
    } else if (orientation == "down") {
        // Horizontal shelf extending right from bottom of long leg (completely clear of the inverted V)
        double sx = p2x + 13.5;
        draw_line_mm(drw, sc, view, p2x, p2y, sx, p2y, 0.25);
        add_note_mm(drw, sc, view, p2x + 7.2, p2y + 2.0, {ra_text}, 2.4, font_name, PRO_HORZJUST_LEFT, PRO_VERTJUST_BOTTOM, 0.0);
    } else if (orientation == "left") {
        // Text rotated 90 deg CCW (reading from right per GB/T)
        add_note_mm(drw, sc, view, x_mm - h1 - 2.0, y_mm + 0.3, {ra_text}, 2.4, font_name, PRO_HORZJUST_CENTER, PRO_VERTJUST_BOTTOM, 90.0);
    } else if (orientation == "right") {
        // Horizontal shelf extending right from top of tick for clean upright reading
        double sx = p2x + 8.5;
        draw_line_mm(drw, sc, view, p2x, p2y, sx, p2y, 0.25);
        add_note_mm(drw, sc, view, p2x + 1.0, p2y + 2.2, {ra_text}, 2.4, font_name, PRO_HORZJUST_LEFT, PRO_VERTJUST_BOTTOM, 0.0);
    }
}

/* Style a ProDwgtable cell note */
inline void style_table_cell(
    ProDwgtable *tbl,
    int col,
    int row,
    const std::string &text_utf8,
    double height_mm,
    const SheetCoord &sc,
    const std::wstring &font_name,
    ProHorizontalJustification hjust = PRO_HORZJUST_CENTER,
    ProVerticalJustification vjust = PRO_VERTJUST_MIDDLE
) {
    if (!text_utf8.empty()) {
        std::wstring ws = utf8_to_w(text_utf8);
        ProWstring *lines = nullptr;
        ProArrayAlloc(1, sizeof(ProWstring), 1, reinterpret_cast<ProArray *>(&lines));
        lines[0] = const_cast<wchar_t *>(ws.c_str());
        ProDwgtableTextEnter(tbl, col, row, lines);
        ProArrayFree(reinterpret_cast<ProArray *>(&lines));
    }
    ProDtlnote cell_note{};
    if (ProDwgtableCellNoteGet(tbl, col, row, &cell_note) == PRO_TK_NO_ERROR) {
        ProDtlnotedata ndata = nullptr;
        if (ProDtlnoteDataGet(&cell_note, nullptr, PRODISPMODE_SYMBOLIC, &ndata) == PRO_TK_NO_ERROR && ndata) {
            ProTextStyle style = nullptr;
            ProTextStyleAlloc(&style);
            ProTextStyleHeightSet(style, height_mm * sc.scale_mm_to_scn);
            ProTextStyleWidthSet(style, 0.85);
            ProTextStyleJustificationSet(style, to_text_hrz(hjust));
            ProTextStyleVertJustificationSet(style, vjust);
            if (!font_name.empty()) {
                ProName wf{};
                wcsncpy_s(wf, PRO_NAME_SIZE, font_name.c_str(), _TRUNCATE);
                ProTextStyleFontSet(style, wf);
            }
            ProColor colr{};
            colr.method = PRO_COLOR_METHOD_TYPE;
            colr.value.type = PRO_COLOR_LETTER;
            ProTextStyleColorSetWithDef(style, &colr);
            ProDtlnotedataTextStyleSet(ndata, style);

            ProDtlnoteline *nlines = nullptr;
            if (ProDtlnotedataLinesCollect(ndata, &nlines) == PRO_TK_NO_ERROR && nlines) {
                int nl = 0;
                ProArraySizeGet(nlines, &nl);
                for (int i = 0; i < nl; ++i) {
                    ProDtlnotetext *ntexts = nullptr;
                    if (ProDtlnotelineTextsCollect(nlines[i], &ntexts) == PRO_TK_NO_ERROR && ntexts) {
                        int nt = 0;
                        ProArraySizeGet(ntexts, &nt);
                        for (int j = 0; j < nt; ++j) {
                            ProDtlnotetextStyleSet(ntexts[j], style);
                        }
                        ProArrayFree(reinterpret_cast<ProArray *>(&ntexts));
                    }
                }
                ProArrayFree(reinterpret_cast<ProArray *>(&nlines));
            }
            ProDtlnoteModify(&cell_note, nullptr, ndata);
            ProTextStyleFree(&style);
            ProDtlnotedataFree(ndata);
        }
    }
}

// A3 binding frame (GB/T 14689-2008) and 180 x 56 title block (GB/T 10609.1-2008).
inline void draw_gb_a3_frame_and_titleblock(
    ProDrawing drw, const SheetCoord &sc, const Json &plan, double mass_kg,
    const std::wstring &font_name, FILE *log
) {
    auto line = [&](double x1, double y1, double x2, double y2) {
        draw_line_mm(drw, sc, nullptr, x1, y1, x2, y2, 0.7);
    };
    constexpr double left = 235, bottom = 5, top = 61, right = 415;
    line(25, bottom, right, bottom);
    line(right, bottom, right, 292);
    line(right, 292, 25, 292);
    line(25, 292, 25, bottom);
    line(210, 297, 210, 287);
    line(210, 0, 210, 10);
    line(0, 148.5, 30, 148.5);
    line(420, 148.5, 410, 148.5);

    // Table dimensions use sheet millimeters. Only the origin uses screen coordinates.
    auto create = [&](const char *section, double x, double y,
                      std::vector<double> widths, std::vector<double> heights) {
        std::vector<ProHorzJust> align(widths.size(), PROHORZJUST_CENTER);
        ProDwgtabledata data = nullptr;
        ProDwgtable table{};
        ProDwgtabledataAlloc(&data);
        ProPoint3d origin{};
        sc.mm_to_scn(x, y, origin);
        ProDwgtabledataOriginSet(data, origin);
        ProDwgtabledataSizetypeSet(data, PRODWGTABLESIZE_SCREEN);
        ProDwgtabledataColumnsSet(data, static_cast<int>(widths.size()), widths.data(), align.data());
        ProDwgtabledataRowsSet(data, static_cast<int>(heights.size()), heights.data());
        const ProError status = ProDrawingTableCreate(drw, data, PRO_B_TRUE, &table);
        log_printf(log, "TITLEBLOCK_TABLE[%s] st=%d\n", section, static_cast<int>(status));
        if (status != PRO_TK_NO_ERROR) throw std::runtime_error("Title block table creation failed");
        return table;
    };
    const Json tb = plan.value("title_block", Json::object());
    const double sheet_scale = plan.value("sheet_scale", 1.0);
    char default_scale[32]{};
    if (sheet_scale >= 1.0) {
        std::snprintf(default_scale, sizeof(default_scale), "%.6g:1", sheet_scale);
    } else if (sheet_scale > 0.0) {
        std::snprintf(default_scale, sizeof(default_scale), "1:%.6g", 1.0 / sheet_scale);
    }
    char mass_text[32]{};
    std::snprintf(mass_text, sizeof(mass_text), "%.2f", mass_kg);
    auto write = [&](ProDwgtable &table, int col, int row, const std::string &value, double height = 3.5) {
        style_table_cell(&table, col, row, value, height, sc, font_name);
    };
    auto merge = [](ProDwgtable &table, int c1, int r1, int c2, int r2) {
        ProDwgtableCellsMerge(&table, c1, r1, c2, r2, PRO_B_FALSE);
    };

    auto revisions = create("revisions", left, top, {10, 10, 16, 16, 12, 16}, {7, 7, 7, 7});
    write(revisions, 1, 4, "标记", 2.5);
    write(revisions, 2, 4, "处数", 2.5);
    write(revisions, 3, 4, "分区", 2.5);
    write(revisions, 4, 4, "更改文件号", 2.5);
    write(revisions, 5, 4, "签名", 2.5);
    write(revisions, 6, 4, "年 月 日", 2.5);

    auto signatures = create("signatures", left, 33, {12, 12, 21, 13, 9, 13}, {7, 7, 7, 7});
    write(signatures, 1, 1, "设计", 3.0);
    write(signatures, 2, 1, tb.value("designer", std::string()), 3.2);
    write(signatures, 3, 1, tb.value("date", std::string()), 2.4);
    write(signatures, 4, 1, "标准化", 2.8);
    write(signatures, 1, 3, "审核", 3.0);
    write(signatures, 1, 4, "工艺", 3.0);
    write(signatures, 4, 4, "批准", 3.0);

    auto metadata = create("metadata", 315, top, {6.5, 6.5, 6.5, 6.5, 12, 12}, {28, 10, 9, 9});
    merge(metadata, 1, 1, 6, 1);
    merge(metadata, 1, 2, 4, 2);
    merge(metadata, 1, 4, 6, 4);
    write(metadata, 1, 1, tb.value("material", std::string()));
    write(metadata, 1, 2, "阶段标记", 3.0);
    write(metadata, 5, 2, "重量", 3.0);
    write(metadata, 6, 2, "比例", 3.0);
    write(metadata, 5, 3, mass_text, 2.5);
    write(metadata, 6, 3, tb.value("scale", std::string(default_scale)), 3.0);
    write(metadata, 1, 4, "共1张  第1张", 3.0);

    const std::string quantity = tb.value("quantity", std::string());
    if (!quantity.empty()) {
        add_note_mm(drw, sc, nullptr, 318, 40, {"件数 " + quantity}, 3.0,
                    font_name, PRO_HORZJUST_LEFT, PRO_VERTJUST_BOTTOM);
    }

    auto identity = create("identity", 365, top, {50}, {17, 18, 12, 9});
    write(identity, 1, 1, tb.value("organization", std::string()));
    write(identity, 1, 2, tb.value("part_title", std::string("零件图")), 5.0);
    write(identity, 1, 3, tb.value("drawing_no", std::string("DWG-001")), 4.0);
    draw_gb_first_angle_symbol(drw, sc, 390, 9.5);

    line(left, bottom, left, top);
    line(left, top, right, top);
    line(315, bottom, 315, top);
    line(365, bottom, 365, top);
}

/* ------------------------------------------------------------------ */
/* Solid geometry finders for drawing references & dimensions         */
/* ------------------------------------------------------------------ */

struct SurfCand {
    int id = -1;
    ProSrftype type = PRO_SRF_NONE;
    double origin[3]{};
    double axis_or_normal[3]{};
    double radius = 0.0;
    double uv_min[2]{}, uv_max[2]{};
    double bbox[2][3]{};
    double mid[3]{};
};

inline ProError collect_body_surfs_cb(ProSurface srf, ProError filt, ProAppData appdata) {
    (void)filt;
    auto *vec = static_cast<std::vector<SurfCand> *>(appdata);
    ProGeomitemdata *gd = nullptr;
    if (ProSurfaceDataGet(srf, &gd) != PRO_TK_NO_ERROR || !gd) return PRO_TK_NO_ERROR;
    ProSurfacedata *sd = PRO_SURF_DATA(gd);
    if (!sd) {
        ProGeomitemdataFree(&gd);
        return PRO_TK_NO_ERROR;
    }
    SurfCand c{};
    ProSurfaceIdGet(srf, &c.id);
    c.type = sd->type;
    Pro3dPnt ext[2]{};
    if (ProSurfaceXyzdataEval(srf, sd->uv_min, ext[0], nullptr, nullptr, nullptr) != PRO_TK_NO_ERROR) {
        ext[0][0] = ext[0][1] = ext[0][2] = 0.0;
    }
    // Sample 3x3 UV grid for robust bbox & midpoint
    bool first = true;
    for (int iu = 0; iu <= 2; ++iu) {
        for (int iv = 0; iv <= 2; ++iv) {
            ProUvParam uv{
                sd->uv_min[0] + (sd->uv_max[0] - sd->uv_min[0]) * (iu * 0.5),
                sd->uv_min[1] + (sd->uv_max[1] - sd->uv_min[1]) * (iv * 0.5)
            };
            Pro3dPnt pt{};
            if (ProSurfaceXyzdataEval(srf, uv, pt, nullptr, nullptr, nullptr) == PRO_TK_NO_ERROR) {
                for (int k = 0; k < 3; ++k) {
                    if (first) {
                        c.bbox[0][k] = c.bbox[1][k] = pt[k];
                    } else {
                        c.bbox[0][k] = std::min(c.bbox[0][k], pt[k]);
                        c.bbox[1][k] = std::max(c.bbox[1][k], pt[k]);
                    }
                }
                first = false;
            }
        }
    }
    for (int k = 0; k < 3; ++k) c.mid[k] = 0.5 * (c.bbox[0][k] + c.bbox[1][k]);

    if (sd->type == PRO_SRF_PLANE) {
        for (int k = 0; k < 3; ++k) {
            c.origin[k] = sd->srf_shape.plane.origin[k];
            c.axis_or_normal[k] = sd->srf_shape.plane.e3[k];
        }
    } else if (sd->type == PRO_SRF_CYL) {
        for (int k = 0; k < 3; ++k) {
            c.origin[k] = sd->srf_shape.cylinder.origin[k];
            c.axis_or_normal[k] = sd->srf_shape.cylinder.e3[k];
        }
        c.radius = sd->srf_shape.cylinder.radius;
    }
    ProGeomitemdataFree(&gd);
    vec->push_back(c);
    return PRO_TK_NO_ERROR;
}

inline std::vector<SurfCand> collect_all_solid_surfaces(ProSolid solid) {
    std::vector<SurfCand> out;
    ProSolidBody body{};
    if (ProSolidDefaultBodyGet(solid, &body) == PRO_TK_NO_ERROR && body.id != PRO_VALUE_UNUSED) {
        ProSolidBodySurfaceVisit(&body, collect_body_surfs_cb, &out);
    } else {
        ProSolidSurfaceVisit(solid, collect_body_surfs_cb, nullptr, &out);
    }
    return out;
}

inline bool find_surface_by_rule(
    ProSolid solid,
    const std::vector<SurfCand> &surfs,
    const Json &rule,
    ProModelitem &out_item,
    Pro3dPnt out_pt,
    FILE *log,
    const char *tag
) {
    std::string skind = rule.value("surf_kind", std::string("any"));
    double tol = rule.value("tol", 0.35);
    double best_score = 1e18;
    int best_idx = -1;

    for (size_t i = 0; i < surfs.size(); ++i) {
        const SurfCand &s = surfs[i];
        if (skind == "plane" && s.type != PRO_SRF_PLANE) continue;
        if (skind == "cylinder" && s.type != PRO_SRF_CYL) continue;

        if (rule.contains("normal")) {
            double nx = rule["normal"].at(0).get<double>();
            double ny = rule["normal"].at(1).get<double>();
            double nz = rule["normal"].at(2).get<double>();
            double dot = std::fabs(s.axis_or_normal[0] * nx + s.axis_or_normal[1] * ny + s.axis_or_normal[2] * nz);
            if (dot < 0.98) continue;
        }
        if (rule.contains("axis")) {
            double ax = rule["axis"].at(0).get<double>();
            double ay = rule["axis"].at(1).get<double>();
            double az = rule["axis"].at(2).get<double>();
            double dot = std::fabs(s.axis_or_normal[0] * ax + s.axis_or_normal[1] * ay + s.axis_or_normal[2] * az);
            if (dot < 0.98) continue;
        }
        if (rule.contains("radius")) {
            if (std::fabs(s.radius - rule["radius"].get<double>()) > tol) continue;
        }
        if (rule.contains("x_range")) {
            double lo = rule["x_range"].at(0).get<double>() - tol;
            double hi = rule["x_range"].at(1).get<double>() + tol;
            if (s.mid[0] < lo || s.mid[0] > hi) continue;
        }
        if (rule.contains("y_range")) {
            double lo = rule["y_range"].at(0).get<double>() - tol;
            double hi = rule["y_range"].at(1).get<double>() + tol;
            if (s.mid[1] < lo || s.mid[1] > hi) continue;
        }
        if (rule.contains("z_range")) {
            double lo = rule["z_range"].at(0).get<double>() - tol;
            double hi = rule["z_range"].at(1).get<double>() + tol;
            if (s.mid[2] < lo || s.mid[2] > hi) continue;
        }
        if (rule.contains("plane_coord")) {
            // Check signed projection along normal (abs value match if sign_free)
            double nx = rule["normal"].at(0).get<double>();
            double ny = rule["normal"].at(1).get<double>();
            double nz = rule["normal"].at(2).get<double>();
            double d = s.origin[0] * nx + s.origin[1] * ny + s.origin[2] * nz;
            if (std::fabs(d - rule["plane_coord"].get<double>()) > tol) continue;
        }
        double score = 0.0;
        if (rule.contains("near")) {
            double tx = rule["near"].at(0).get<double>();
            double ty = rule["near"].at(1).get<double>();
            double tz = rule["near"].at(2).get<double>();
            score = std::hypot(std::hypot(s.mid[0] - tx, s.mid[1] - ty), s.mid[2] - tz);
        }
        if (score < best_score) {
            best_score = score;
            best_idx = static_cast<int>(i);
        }
    }

    if (best_idx < 0) {
        log_printf(log, "  DWG_SURF_MISS[%s]\n", tag);
        return false;
    }
    const SurfCand &hit = surfs[static_cast<size_t>(best_idx)];
    if (ProModelitemInit(solid, hit.id, PRO_SURFACE, &out_item) != PRO_TK_NO_ERROR) return false;
    if (rule.contains("pick_pt")) {
        out_pt[0] = rule["pick_pt"].at(0).get<double>();
        out_pt[1] = rule["pick_pt"].at(1).get<double>();
        out_pt[2] = rule["pick_pt"].at(2).get<double>();
    } else {
        out_pt[0] = hit.mid[0];
        out_pt[1] = hit.mid[1];
        out_pt[2] = hit.mid[2];
    }
    return true;
}

inline bool find_datum_point_item_by_name(ProSolid solid, const std::string &name_utf8, ProModelitem &out_item, Pro3dPnt out_pt) {
    struct PntQuery {
        std::wstring target;
        ProModelitem item{};
        Pro3dPnt pt{};
        bool found = false;
    };
    PntQuery q{};
    q.target = utf8_to_w(name_utf8);
    auto geom_cb = [](ProGeomitem *gi, ProError st, ProAppData ad) -> ProError {
        (void)st;
        auto *pq = static_cast<PntQuery *>(ad);
        ProName nm{};
        if (ProModelitemNameGet(gi, nm) == PRO_TK_NO_ERROR && _wcsicmp(nm, pq->target.c_str()) == 0) {
            pq->item = *gi;
            ProPoint p = nullptr;
            if (ProPointInit(static_cast<ProSolid>(gi->owner), gi->id, &p) == PRO_TK_NO_ERROR && p) {
                ProPointCoordGet(p, pq->pt);
            }
            pq->found = true;
            return PRO_TK_E_FOUND;
        }
        return PRO_TK_NO_ERROR;
    };
    auto feat_cb = [](ProFeature *f, ProError st, ProAppData ad) -> ProError {
        (void)st;
        ProFeatStatus fst = PRO_FEAT_INVALID;
        if (ProFeatureStatusGet(f, &fst) != PRO_TK_NO_ERROR || fst != PRO_FEAT_ACTIVE) return PRO_TK_NO_ERROR;
        ProFeattype ft = PRO_FEAT_FIRST_FEAT;
        if (ProFeatureTypeGet(f, &ft) == PRO_TK_NO_ERROR && ft == PRO_FEAT_DATUM_POINT) {
            ProFeatureGeomitemVisit(f, PRO_POINT,
                [](ProGeomitem *gi, ProError gst, ProAppData gad) -> ProError {
                    (void)gst;
                    auto *pq = static_cast<PntQuery *>(gad);
                    ProName nm{};
                    if (ProModelitemNameGet(gi, nm) == PRO_TK_NO_ERROR && _wcsicmp(nm, pq->target.c_str()) == 0) {
                        pq->item = *gi;
                        ProPoint p = nullptr;
                        if (ProPointInit(static_cast<ProSolid>(gi->owner), gi->id, &p) == PRO_TK_NO_ERROR && p) {
                            ProPointCoordGet(p, pq->pt);
                        }
                        pq->found = true;
                        return PRO_TK_E_FOUND;
                    }
                    return PRO_TK_NO_ERROR;
                }, nullptr, ad);
            if (static_cast<PntQuery *>(ad)->found) return PRO_TK_E_FOUND;
        }
        return PRO_TK_NO_ERROR;
    };
    (void)geom_cb;
    ProSolidFeatVisit(solid, feat_cb, nullptr, &q);
    if (!q.found) return false;
    out_item = q.item;
    out_pt[0] = q.pt[0];
    out_pt[1] = q.pt[1];
    out_pt[2] = q.pt[2];
    return true;
}

inline ProError create_hidden_datum_points(ProSolid solid, const Json &pts_arr_json, FILE *log) {
    // Check if points already exist (reuse across re-runs)
    bool all_exist = true;
    for (const Json &pt : pts_arr_json) {
        ProModelitem dummy{};
        Pro3dPnt dpt{};
        if (!find_datum_point_item_by_name(solid, pt.at("name").get<std::string>(), dummy, dpt)) {
            all_exist = false;
            break;
        }
    }
    if (all_exist) {
        log_printf(log, "DWG_DATUM_POINTS already exist, reusing\n");
        return PRO_TK_NO_ERROR;
    }

    ProFeature csys_feat{};
    if (dwg_find_feat_by_name(solid, L"PRT_CSYS_DEF", &csys_feat) != PRO_TK_NO_ERROR) {
        log_printf(log, "DWG_DATUM_POINTS missing PRT_CSYS_DEF\n");
        return PRO_TK_E_NOT_FOUND;
    }
    ProSelection csys_sel = nullptr;
    if (find_feat_geom_sel(&csys_feat, PRO_CSYS, &csys_sel) != PRO_TK_NO_ERROR || !csys_sel) {
        return PRO_TK_E_NOT_FOUND;
    }

    ProElement root = nullptr, pts_arr = nullptr;
    elem_add_compound(nullptr, PRO_E_FEATURE_TREE, &root);
    elem_add_int(root, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_POINT);
    elem_add_int(root, PRO_E_DPOINT_TYPE, PRO_DPOINT_TYPE_OFFSET_CSYS);
    elem_add_wstr(root, PRO_E_STD_FEATURE_NAME, L"DWG_REF_PNTS");
    elem_add_int(root, PRO_E_DPOINT_OFST_CSYS_TYPE, PRO_DTMPNT_OFFCSYS_CARTESIAN);
    elem_add_ref(root, PRO_E_DPOINT_OFST_CSYS_REF, csys_sel);
    ProSelectionFree(&csys_sel);
    elem_add_int(root, PRO_E_DPOINT_OFST_CSYS_WITH_DIMS, PRO_B_TRUE);
    elem_add_compound(root, PRO_E_DPOINT_OFST_CSYS_PNTS_ARRAY, &pts_arr);

    for (const Json &pt : pts_arr_json) {
        std::wstring pname = utf8_to_w(pt.at("name").get<std::string>());
        const Json &pos = pt.at("position");
        ProElement p_e = nullptr;
        elem_add_compound(pts_arr, PRO_E_DPOINT_OFST_CSYS_PNT, &p_e);
        elem_add_wstr(p_e, PRO_E_DPOINT_OFST_CSYS_PNT_NAME, pname.c_str());
        elem_add_double(p_e, PRO_E_DPOINT_OFST_CSYS_DIR1_VAL, pos.at(0).get<double>());
        elem_add_double(p_e, PRO_E_DPOINT_OFST_CSYS_DIR2_VAL, pos.at(1).get<double>());
        elem_add_double(p_e, PRO_E_DPOINT_OFST_CSYS_DIR3_VAL, pos.at(2).get<double>());
    }

    ProModelitem mdl_item{};
    ProSelection mdl_sel = nullptr;
    ProMdlToModelitem(static_cast<ProMdl>(solid), &mdl_item);
    ProSelectionAlloc(nullptr, &mdl_item, &mdl_sel);
    ProFeatureCreateOptions *opts = nullptr;
    ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, reinterpret_cast<ProArray *>(&opts));
    opts[0] = PRO_FEAT_CR_NO_OPTS;
    ProFeature feat{};
    ProErrorlist errs{};
    ProError st = ProFeatureWithoptionsCreate(mdl_sel, root, opts, PRO_REGEN_NO_FLAGS, &feat, &errs);
    ProArrayFree(reinterpret_cast<ProArray *>(&opts));
    ProSelectionFree(&mdl_sel);
    ProElementFree(&root);
    log_printf(log, "DWG_DATUM_POINTS create st=%d feat_id=%d\n", static_cast<int>(st), feat.id);
    if (st == PRO_TK_NO_ERROR) {
        // Keep datum points visible during drawing creation; hide after views/breaks finish if needed
        ProSolidRegenerate(solid, PRO_REGEN_NO_FLAGS);
    }
    return st;
}

inline bool find_datum_plane_item_by_name(ProSolid solid, const std::string &name_utf8, ProModelitem &out_item) {
    ProFeature feat{};
    std::wstring wn = utf8_to_w(name_utf8);
    if (dwg_find_feat_by_name(solid, wn.c_str(), &feat) != PRO_TK_NO_ERROR) return false;
    ProSelection sel = nullptr;
    if (find_feat_geom_sel(&feat, PRO_SURFACE, &sel) != PRO_TK_NO_ERROR || !sel) return false;
    ProError st = ProSelectionModelitemGet(sel, &out_item);
    ProSelectionFree(&sel);
    return st == PRO_TK_NO_ERROR;
}

inline bool find_datum_axis_item_by_name(ProSolid solid, const std::string &name_utf8, ProModelitem &out_item) {
    ProFeature feat{};
    std::wstring wn = utf8_to_w(name_utf8);
    if (dwg_find_feat_by_name(solid, wn.c_str(), &feat) != PRO_TK_NO_ERROR) return false;
    ProSelection sel = nullptr;
    if (find_feat_geom_sel(&feat, PRO_AXIS, &sel) != PRO_TK_NO_ERROR || !sel) return false;
    ProError st = ProSelectionModelitemGet(sel, &out_item);
    ProSelectionFree(&sel);
    return st == PRO_TK_NO_ERROR;
}

inline bool find_edge_by_rule_dwg(
    ProSolid solid,
    const Json &rule,
    ProModelitem &out_item,
    Pro3dPnt out_pt,
    FILE *log,
    const char *tag
) {
    EdgeCollectState state{};
    state.solid = solid;
    ProSolidBody body{};
    if (ProSolidDefaultBodyGet(solid, &body) == PRO_TK_NO_ERROR && body.id != PRO_VALUE_UNUSED) {
        ProSolidBodySurfaceVisit(&body, visit_surface_edges_action, &state);
    }
    double best_score = 1e18;
    int best_idx = -1;
    double tol = rule.value("tol", 0.35);
    for (size_t i = 0; i < state.edges.size(); ++i) {
        const EdgeCandidate &e = state.edges[i];
        if (!edge_matches_rule(e, rule)) continue;
        if (rule.contains("edge_type") || rule.contains("arc_radius")) {
            ProGeomitemdata *gd = nullptr;
            bool ok = false;
            if (ProEdgeDataGet(e.handle, &gd) == PRO_TK_NO_ERROR && gd) {
                ProCurvedata *cd = PRO_CURVE_DATA(gd);
                if (cd) {
                    bool type_ok = true;
                    if (rule.contains("edge_type")) {
                        std::string et = rule["edge_type"].get<std::string>();
                        if (et == "arc") type_ok = (cd->line.type == PRO_ENT_ARC || cd->line.type == PRO_ENT_CIRCLE);
                        else if (et == "line") type_ok = (cd->line.type == PRO_ENT_LINE);
                    }
                    bool rad_ok = true;
                    if (rule.contains("arc_radius")) {
                        if (cd->line.type == PRO_ENT_ARC || cd->line.type == PRO_ENT_CIRCLE) {
                            double r = (cd->line.type == PRO_ENT_ARC) ? cd->arc.radius : cd->circle.radius;
                            rad_ok = (std::fabs(r - rule["arc_radius"].get<double>()) <= tol);
                        } else {
                            rad_ok = false;
                        }
                    }
                    ok = type_ok && rad_ok;
                }
                ProGeomitemdataFree(&gd);
            }
            if (!ok) continue;
        }
        double score = 0.0;
        if (rule.contains("prefer_near")) {
            double tx = rule["prefer_near"].at(0).get<double>();
            double ty = rule["prefer_near"].at(1).get<double>();
            double tz = rule["prefer_near"].at(2).get<double>();
            score = std::hypot(std::hypot(e.pm[0] - tx, e.pm[1] - ty), e.pm[2] - tz);
        }
        if (score < best_score) {
            best_score = score;
            best_idx = static_cast<int>(i);
        }
    }
    if (best_idx < 0) {
        log_printf(log, "  DWG_EDGE_MISS[%s]\n", tag);
        return false;
    }
    const EdgeCandidate &hit = state.edges[static_cast<size_t>(best_idx)];
    if (ProEdgeToGeomitem(solid, hit.handle, &out_item) != PRO_TK_NO_ERROR) return false;
    if (rule.contains("pick_pt")) {
        out_pt[0] = rule["pick_pt"].at(0).get<double>();
        out_pt[1] = rule["pick_pt"].at(1).get<double>();
        out_pt[2] = rule["pick_pt"].at(2).get<double>();
    } else {
        out_pt[0] = hit.pm[0];
        out_pt[1] = hit.pm[1];
        out_pt[2] = hit.pm[2];
    }
    return true;
}

inline bool resolve_draw_selection(
    ProDrawing drw,
    ProSolid solid,
    const std::vector<SurfCand> &surfs,
    ProView view,
    const Json &ref,
    ProSelection &out_sel,
    Pro3dPnt out_pt3d,
    FILE *log,
    const char *tag
) {
    out_sel = nullptr;
    ProModelitem item{};
    std::string kind = ref.value("kind", std::string("surface"));
    bool ok = false;
    if (kind == "surface") {
        ok = find_surface_by_rule(solid, surfs, ref, item, out_pt3d, log, tag);
    } else if (kind == "edge") {
        ok = find_edge_by_rule_dwg(solid, ref, item, out_pt3d, log, tag);
    } else if (kind == "datum_plane") {
        ok = find_datum_plane_item_by_name(solid, ref.at("name").get<std::string>(), item);
        out_pt3d[0] = out_pt3d[1] = out_pt3d[2] = 0.0;
        if (ref.contains("pick_pt")) {
            out_pt3d[0] = ref["pick_pt"].at(0).get<double>();
            out_pt3d[1] = ref["pick_pt"].at(1).get<double>();
            out_pt3d[2] = ref["pick_pt"].at(2).get<double>();
        }
    } else if (kind == "datum_point") {
        ok = find_datum_point_item_by_name(solid, ref.at("name").get<std::string>(), item, out_pt3d);
        if (!ok) log_printf(log, "  DWG_PNT_MISS[%s name=%s]\n", tag, ref.at("name").get<std::string>().c_str());
    } else if (kind == "axis") {
        ok = find_datum_axis_item_by_name(solid, ref.at("name").get<std::string>(), item);
        out_pt3d[0] = out_pt3d[1] = out_pt3d[2] = 0.0;
        if (ref.contains("pick_pt")) {
            out_pt3d[0] = ref["pick_pt"].at(0).get<double>();
            out_pt3d[1] = ref["pick_pt"].at(1).get<double>();
            out_pt3d[2] = ref["pick_pt"].at(2).get<double>();
        }
    }
    if (!ok) return false;

    ProAsmcomppath cp{};
    ProAsmcomppathInit(solid, nullptr, 0, &cp);
    ProError st = ProSelectionAlloc(&cp, &item, &out_sel);
    if (st != PRO_TK_NO_ERROR || !out_sel) return false;
    ProSelectionViewSet(view, &out_sel);
    if (kind != "datum_point") {
        ProSelectionPoint3dSet(out_pt3d, &out_sel);
    }
    if (kind == "surface") {
        ProSurface srf = nullptr;
        if (ProSurfaceInit(solid, item.id, &srf) == PRO_TK_NO_ERROR && srf) {
            ProUvParam uv{};
            if (ProSurfaceParamEval(solid, srf, out_pt3d, uv) == PRO_TK_NO_ERROR) {
                ProSelectionUvParamSet(out_sel, uv);
            }
        }
    }
    (void)drw;
    return true;
}

/* Build a closed circular ProCurvedata spline in drawing screen coords for ProDrawingViewDetailCreate */
inline ProCurvedata *make_detail_circle_spline(double cx_scn, double cy_scn, double r_scn) {
    const int N = 13; // 12 segments + closure
    double *pars = nullptr;
    ProPoint3d *pnts = nullptr;
    ProPoint3d *tans = nullptr;
    ProArrayAlloc(N, sizeof(double), 1, reinterpret_cast<ProArray *>(&pars));
    ProArrayAlloc(N, sizeof(ProPoint3d), 1, reinterpret_cast<ProArray *>(&pnts));
    ProArrayAlloc(N, sizeof(ProPoint3d), 1, reinterpret_cast<ProArray *>(&tans));
    for (int i = 0; i < N; ++i) {
        double a = (2.0 * M_PI * i) / (N - 1);
        pars[i] = a;
        pnts[i][0] = cx_scn + r_scn * std::cos(a);
        pnts[i][1] = cy_scn + r_scn * std::sin(a);
        pnts[i][2] = 0.0;
        tans[i][0] = -r_scn * std::sin(a);
        tans[i][1] = r_scn * std::cos(a);
        tans[i][2] = 0.0;
    }
    ProCurvedata *crv = nullptr;
    ProCurvedataAlloc(&crv);
    ProSplinedataInit(pars, pnts, tans, N, crv);
    return crv;
}

inline void log_view_outline(ProDrawing drw, const SheetCoord &sc, ProView v, const std::string &vid, FILE *log) {
    ProPoint3d ol[2]{};
    if (ProDrawingViewOutlineGet(drw, v, ol) == PRO_TK_NO_ERROR) {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        sc.scn_to_mm(ol[0], x0, y0);
        sc.scn_to_mm(ol[1], x1, y1);
        double scale = 1.0;
        ProDrawingViewScaleGet(drw, v, &scale);
        log_printf(log, "  VIEW_BBOX[%s] mm=[%.2f..%.2f, %.2f..%.2f] w=%.2f h=%.2f scale=%.3f\n",
                   vid.c_str(), x0, x1, y0, y1, x1 - x0, y1 - y0, scale);
    }
}

/* ------------------------------------------------------------------ */
/* Main Drawing Creation Entrypoint                                   */
/* ------------------------------------------------------------------ */

struct PendingDrawingJob {
    ProDrawing drw = nullptr;
    ProSolid solid = nullptr;
    ProMdl part_mdl = nullptr;
    int win_id = PRO_VALUE_UNUSED;
    double mass_kg = 0.0;
    Json plan;
    std::wstring font_name;
    FILE *log = nullptr;
    bool active = false;
};

inline PendingDrawingJob g_dwg_job;
inline ProError run_drawing_build_stage2();

inline ProError run_drawing_build(FILE *log) {
    Json plan = dwg_load_plan_json("creobuild_plan.json");
    std::string model_name = plan.at("model_name").get<std::string>();
    std::string drw_name = plan.at("drawing_name").get<std::string>();
    std::string tmpl_name = plan.value("template", std::string("a3_drawing"));
    std::wstring font_name = utf8_to_w(plan.value("font", std::string("chfntf")));

    log_printf(log, "IMPL=c_protoolkit_gb_drawing_engine\n");
    log_printf(log, "MODEL_NAME=%s\nDRAWING_NAME=%s\n", model_name.c_str(), drw_name.c_str());

    // 1. Retrieve source part model
    ProMdl part_mdl = nullptr;
    ProMdlName w_model{};
    wcsncpy_s(w_model, PRO_MDLNAME_SIZE, utf8_to_w(model_name).c_str(), _TRUNCATE);
    ProError st = ProMdlnameRetrieve(w_model, PRO_MDLFILE_PART, &part_mdl);
    if (st != PRO_TK_NO_ERROR || !part_mdl) {
        log_st(log, "retrieve_part", st);
        return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    }
    ProSolid solid = static_cast<ProSolid>(part_mdl);

    // Measure steel mass (density = 7.85 g/cm^3 = 7.85e-6 kg/mm^3)
    double vol_mm3 = 0.0, area_mm2 = 0.0;
    Pro3dPnt pbox[2]{};
    measure_solid_volume(solid, &vol_mm3, &area_mm2, pbox);
    double density = plan.value("density_g_cm3", 7.85);
    double mass_kg = vol_mm3 * density * 1e-6;
    log_printf(log, "PART_METRICS vol=%.3f area=%.3f mass_kg=%.3f\n", vol_mm3, area_mm2, mass_kg);

    // 2. Create optional hidden datum points & planar cross-sections on solid
    bool part_modified = false;
    if (plan.contains("datum_points") && !plan["datum_points"].empty()) {
        ProError pst = create_hidden_datum_points(solid, plan["datum_points"], log);
        if (pst != PRO_TK_NO_ERROR) return pst;
        part_modified = true;
    }
    if (plan.contains("xsections")) {
        for (const Json &xs : plan["xsections"]) {
            std::string xname = xs.at("name").get<std::string>();
            std::string pname = xs.at("plane").get<std::string>();
            ProModelitem pitem{};
            if (!find_datum_plane_item_by_name(solid, pname, pitem)) {
                log_printf(log, "XSEC_PLANE_NOT_FOUND name=%s plane=%s\n", xname.c_str(), pname.c_str());
                return PRO_TK_E_NOT_FOUND;
            }
            ProName wx{};
            wcsncpy_s(wx, PRO_NAME_SIZE, utf8_to_w(xname).c_str(), _TRUNCATE);
            struct XsecQuery {
                const wchar_t *target;
                bool found = false;
            } xq{wx, false};
            ProSolidXsecVisit(
                solid,
                [](ProXsec *xs_ptr, ProAppData ad) -> ProError {
                    auto *q = static_cast<XsecQuery *>(ad);
                    if (xs_ptr && _wcsicmp(xs_ptr->xsec_name, q->target) == 0) {
                        q->found = true;
                        return PRO_TK_E_FOUND;
                    }
                    return PRO_TK_NO_ERROR;
                },
                &xq);
            if (xq.found) {
                log_printf(log, "XSEC_REUSE[%s on %s]\n", xname.c_str(), pname.c_str());
            } else {
                ProXsec xsec{};
                ProError xst = ProXsecPlanarCreate(solid, wx, pitem.id, PRO_XSECTYPE_MODEL, -1, &xsec);
                log_printf(log, "XSEC_CREATE[%s on %s id=%d] st=%d\n", xname.c_str(), pname.c_str(), pitem.id, static_cast<int>(xst));
                if (xst != PRO_TK_NO_ERROR && xst != PRO_TK_E_FOUND) {
                    return xst;
                }
                part_modified = true;
            }
        }
    }
    if (part_modified) {
        ProMdlSave(part_mdl);
    }

    // 3. Use CLI-opened active drawing if already loaded, else create from template
    ProMdlName w_drw{}, w_tmpl{};
    wcsncpy_s(w_drw, PRO_MDLNAME_SIZE, utf8_to_w(drw_name).c_str(), _TRUNCATE);
    wcsncpy_s(w_tmpl, PRO_MDLNAME_SIZE, utf8_to_w(tmpl_name).c_str(), _TRUNCATE);

    ProDrawing drw = nullptr;
    ProMdl cur_mdl = nullptr;
    ProMdlType cur_type = PRO_MDL_UNUSED;
    if (ProMdlCurrentGet(&cur_mdl) == PRO_TK_NO_ERROR && cur_mdl &&
        ProMdlTypeGet(cur_mdl, &cur_type) == PRO_TK_NO_ERROR && cur_type == PRO_MDL_DRAWING) {
        drw = static_cast<ProDrawing>(cur_mdl);
        log_printf(log, "DWG_ACTIVE_CLI_MDL drw=%p\n", static_cast<void *>(drw));
    } else {
        ProMdlnameShortdata shortdata{};
        wcsncpy_s(shortdata.name, PRO_MDLNAME_SIZE, w_model, _TRUNCATE);
        wcsncpy_s(shortdata.type, PRO_MDLEXTENSION_SIZE, L"prt", _TRUNCATE);
        ProDwgcreateErrs errs = nullptr;
        ProDwgcreateOptions opts = PRODWGCREATE_DISPLAY_DRAWING;
        st = ProDrawingFromTemplateCreate(w_drw, w_tmpl, &shortdata, opts, &drw, &errs);
        log_printf(log, "DWG_CREATE_FROM_TMPL tmpl=%s st=%d drw=%p\n", tmpl_name.c_str(), static_cast<int>(st), static_cast<void *>(drw));
        if (errs) ProDwgcreateErrsFree(&errs);
        if ((st != PRO_TK_NO_ERROR && st != PRO_TK_DWGCREATE_ERRORS) || !drw) {
            log_st(log, "drawing_create", st);
            return (st == PRO_TK_NO_ERROR) ? PRO_TK_GENERAL_ERROR : st;
        }
    }

    int win_id = PRO_VALUE_UNUSED;
    ProMdlWindowGet(drw, &win_id);
    if (win_id == PRO_VALUE_UNUSED) {
        ProObjectwindowMdlnameCreate(w_drw, PRO_DRAWING, &win_id);
    }
    ProMdlDisplay(drw);
    if (win_id != PRO_VALUE_UNUSED) {
        ProWindowCurrentSet(win_id);
        ProWindowActivate(win_id);
        ProWindowRepaint(win_id);
    }
    ProDrawingCurrentSheetSet(drw, 1);

    g_dwg_job.drw = drw;
    g_dwg_job.solid = solid;
    g_dwg_job.part_mdl = part_mdl;
    g_dwg_job.win_id = win_id;
    g_dwg_job.mass_kg = mass_kg;
    g_dwg_job.plan = plan;
    g_dwg_job.font_name = font_name;
    g_dwg_job.log = log;
    g_dwg_job.active = true;
    if (cur_mdl == drw) {
        // Already active in UI; execute stage 2 immediately
        ProError s2 = run_drawing_build_stage2();
        g_dwg_job.active = false;
        return s2;
    }
    log_printf(log, "DWG_STAGE1_DONE deferring to CreoDwgStep\n");
    ProMacro wmac{};
    ProStringToWstring(wmac, const_cast<char *>("~ Command `CreoDwgStep` ;"));
    ProMacroLoad(wmac);
    return PRO_TK_NO_ERROR;
}

inline ProError run_drawing_build_stage2() {
    if (!g_dwg_job.active || !g_dwg_job.drw || !g_dwg_job.log) return PRO_TK_BAD_CONTEXT;
    FILE *log = g_dwg_job.log;
    ProDrawing drw = g_dwg_job.drw;
    ProSolid solid = g_dwg_job.solid;
    int win_id = g_dwg_job.win_id;
    double mass_kg = g_dwg_job.mass_kg;
    const Json &plan = g_dwg_job.plan;
    const std::wstring &font_name = g_dwg_job.font_name;
    std::string drw_name = plan.at("drawing_name").get<std::string>();

    ProMdl cm = nullptr;
    ProError cm_st = ProMdlCurrentGet(&cm);
    int cur_w = -1;
    ProWindowCurrentGet(&cur_w);
    log_printf(log, "DWG_STAGE2 cur_win=%d cm_st=%d same_drw=%d\n", cur_w, static_cast<int>(cm_st), (cm == drw) ? 1 : 0);

    // 4. Strip any pre-existing template views, tables, notes, entities & set blank A3 (420x297 mm)
    ProView *init_views = nullptr;
    if (ProDrawingViewsCollect(drw, &init_views) == PRO_TK_NO_ERROR && init_views) {
        int nv = 0;
        ProArraySizeGet(init_views, &nv);
        for (int i = 0; i < nv; ++i) {
            ProDrawingViewDelete(drw, init_views[i], PRO_B_TRUE);
        }
        ProArrayFree(reinterpret_cast<ProArray *>(&init_views));
        log_printf(log, "STRIPPED_TEMPLATE_VIEWS count=%d\n", nv);
    }
    ProDwgtable *init_tbls = nullptr;
    if (ProDrawingTablesCollect(drw, &init_tbls) == PRO_TK_NO_ERROR && init_tbls) {
        int nt = 0;
        ProArraySizeGet(init_tbls, &nt);
        for (int i = 0; i < nt; ++i) ProDwgtableDelete(&init_tbls[i], PRO_B_FALSE);
        ProArrayFree(reinterpret_cast<ProArray *>(&init_tbls));
    }
    ProDtlnote *init_notes = nullptr;
    if (ProDrawingDtlnotesCollect(drw, nullptr, 1, &init_notes) == PRO_TK_NO_ERROR && init_notes) {
        int nn = 0;
        ProArraySizeGet(init_notes, &nn);
        for (int i = 0; i < nn; ++i) ProDtlnoteDelete(&init_notes[i], nullptr);
        ProArrayFree(reinterpret_cast<ProArray *>(&init_notes));
    }
    ProDtlentity *init_ents = nullptr;
    if (ProDrawingDtlentitiesCollect(drw, nullptr, 1, &init_ents) == PRO_TK_NO_ERROR && init_ents) {
        int ne = 0;
        ProArraySizeGet(init_ents, &ne);
        for (int i = 0; i < ne; ++i) ProDtlentityDelete(&init_ents[i], nullptr);
        ProArrayFree(reinterpret_cast<ProArray *>(&init_ents));
    }
    ProDtlsyminst *init_syms = nullptr;
    if (ProDrawingDtlsyminstsCollect(drw, 1, &init_syms) == PRO_TK_NO_ERROR && init_syms) {
        int ns = 0;
        ProArraySizeGet(init_syms, &ns);
        for (int i = 0; i < ns; ++i) ProDtlsyminstDelete(&init_syms[i]);
        ProArrayFree(reinterpret_cast<ProArray *>(&init_syms));
    }

    ProError fmt_st = ProDrawingFormatSizeSet(drw, 1, A3_SIZE_PLOT, 420.0, 297.0);
    log_printf(log, "FORMAT_SIZE_A3 st=%d\n", static_cast<int>(fmt_st));

    // Ensure part is added to drawing
    ProDrawingSolidAdd(drw, solid);
    ProDrawingScaleSet(drw, solid, 1, plan.value("sheet_scale", 1.0));

    // 5. Apply strict GB/T first-angle detail options
    set_dtl_opt(drw, L"projection_type", L"FIRST_ANGLE", log);
    set_dtl_opt(drw, L"drawing_units", L"MM", log);
    set_dtl_opt(drw, L"decimal_marker", L"PERIOD", log);
    set_dtl_opt(drw, L"default_font", font_name.c_str(), log);
    set_dtl_opt(drw, L"default_annotation_font", font_name.c_str(), log);
    set_dtl_opt(drw, L"symbol_font", L"LEGACY", log);
    set_dtl_opt(drw, L"drawing_text_height", L"3.5", log);
    set_dtl_opt(drw, L"def_view_text_height", L"4.0", log);
    set_dtl_opt(drw, L"text_orientation", L"PARALLEL_DIAM_HORIZ", log);
    set_dtl_opt(drw, L"parallel_dim_placement", L"ABOVE", log);
    set_dtl_opt(drw, L"draw_arrow_style", L"FILLED", log);
    set_dtl_opt(drw, L"draw_arrow_length", L"3.5", log);
    set_dtl_opt(drw, L"draw_arrow_width", L"1.2", log);
    set_dtl_opt(drw, L"dim_text_gap", L"1.0", log);
    set_dtl_opt(drw, L"dim_leader_length", L"5.0", log);
    set_dtl_opt(drw, L"tol_display", L"YES", log);
    set_dtl_opt(drw, L"tol_text_height_factor", L"0.65", log);
    set_dtl_opt(drw, L"tol_text_width_factor", L"0.75", log);
    set_dtl_opt(drw, L"symmetric_tol_display_standard", L"STD_ISO", log);
    set_dtl_opt(drw, L"lead_trail_zeros", L"STD_METRIC", log);
    set_dtl_opt(drw, L"broken_view_offset", L"5.0", log);
    set_dtl_opt(drw, L"view_scale_denominator", L"10", log);
    set_dtl_opt(drw, L"view_scale_format", L"RATIO_COLON", log);
    set_dtl_opt(drw, L"detail_view_boundary_type", L"CIRCLE", log);
    set_dtl_opt(drw, L"detail_view_circle", L"ON", log);
    set_dtl_opt(drw, L"detail_circle_line_style", L"SOLIDFONT", log);
    set_dtl_opt(drw, L"crossec_arrow_style", L"HEAD_ONLINE", log);
    set_dtl_opt(drw, L"crossec_arrow_length", L"6.0", log);
    set_dtl_opt(drw, L"crossec_arrow_width", L"2.0", log);
    set_dtl_opt(drw, L"default_show_2d_section_xhatch", L"ASSEMBLY_AND_PART", log);
    set_dtl_opt(drw, L"def_xhatch_break_around_text", L"YES", log);
    set_dtl_opt(drw, L"view_note", L"STD_ISO", log);

    ProDwgSheetRegenerate(drw, 1);
    SheetCoord sc;
    sc.init(drw, 1, log);

    // 6. Draw the GB/T A3 binding frame, center marks and title block
    draw_gb_a3_frame_and_titleblock(drw, sc, plan, mass_kg, font_name, log);

    // Collect solid surfaces once for geometric queries
    std::vector<SurfCand> surfs = collect_all_solid_surfaces(solid);
    log_printf(log, "SOLID_SURFACES_COLLECTED count=%zu\n", surfs.size());

    // 7. Create Views in 4 clean passes:
    //    Pass 1: general + projected views + display + 2D section
    //    Pass 2: broken visible area (must run AFTER projected views are created!)
    //    Pass 3: move_center_mm + post section arrows
    //    Pass 4: detail views (referencing final broken & positioned parent view)
    std::map<std::string, ProView> views;
    if (plan.contains("views")) {
        auto apply_view_display = [&](ProView v, const Json &vs) {
            ProDrawingViewDisplay vdisp{};
            ProDrawingViewDisplayGet(drw, v, &vdisp);
            std::string dstyle = vs.value("display", std::string("no_hidden"));
            vdisp.style = (dstyle == "hidden_line") ? PRO_DISPSTYLE_HIDDEN_LINE : PRO_DISPSTYLE_NO_HIDDEN;
            std::string tdisp = vs.value("tangent_edges", std::string("none"));
            if (tdisp == "none") vdisp.tangent_edge_display = PRO_TANEDGE_NONE;
            else if (tdisp == "dimmed") vdisp.tangent_edge_display = PRO_TANEDGE_DIMMED;
            else if (tdisp == "solid") vdisp.tangent_edge_display = PRO_TANEDGE_SOLID;
            ProDrawingViewDisplaySet(drw, v, &vdisp);
        };

        // Pass 1: General & Projected views
        for (const Json &vs : plan["views"]) {
            std::string vid = vs.at("id").get<std::string>();
            std::string vtype = vs.at("type").get<std::string>();
            if (vtype == "detail") continue;
            double px = vs.at("pos_mm").at(0).get<double>();
            double py = vs.at("pos_mm").at(1).get<double>();
            ProPoint3d pos_scn{};
            sc.mm_to_scn(px, py, pos_scn);

            ProView v = nullptr;
            ProError vst = PRO_TK_GENERAL_ERROR;
            if (vtype == "general") {
                double vscale = vs.value("scale", 0.0);
                ProMatrix orient{};
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        orient[r][c] = (r == c) ? 1.0 : 0.0;
                if (vs.contains("matrix3")) {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            orient[r][c] = vs["matrix3"].at(r).at(c).get<double>();
                }
                vst = ProDrawingGeneralviewCreate(drw, solid, 1, PRO_B_FALSE, pos_scn, vscale, orient, &v);
                if (vst == PRO_TK_NO_ERROR && vscale <= 0.0) {
                    ProDrawingScaleSet(drw, solid, 1, plan.value("sheet_scale", 1.0));
                }
            } else if (vtype == "projected") {
                std::string pid = vs.at("parent").get<std::string>();
                auto pit = views.find(pid);
                if (pit == views.end()) {
                    log_printf(log, "VIEW_MISSING_PARENT[%s -> %s]\n", vid.c_str(), pid.c_str());
                    return PRO_TK_E_NOT_FOUND;
                }
                // Position projected view relative to parent's CURRENT bbox so alignment never collides
                ProPoint3d pol[2]{};
                if (ProDrawingViewOutlineGet(drw, pit->second, pol) == PRO_TK_NO_ERROR && vs.contains("proj_dir")) {
                    double px0, py0, px1, py1;
                    sc.scn_to_mm(pol[0], px0, py0);
                    sc.scn_to_mm(pol[1], px1, py1);
                    double pcx = 0.5 * (px0 + px1), pcy = 0.5 * (py0 + py1);
                    std::string pdir = vs["proj_dir"].get<std::string>();
                    double gap = vs.value("proj_gap_mm", 45.0);
                    if (pdir == "right") sc.mm_to_scn(px1 + gap, pcy, pos_scn);
                    else if (pdir == "left") sc.mm_to_scn(px0 - gap, pcy, pos_scn);
                    else if (pdir == "below") sc.mm_to_scn(pcx, py0 - gap, pos_scn);
                    else if (pdir == "above") sc.mm_to_scn(pcx, py1 + gap, pos_scn);
                }
                vst = ProDrawingProjectedviewCreate(drw, pit->second, PRO_B_FALSE, pos_scn, &v);
            }
            log_printf(log, "VIEW_CREATE[%s type=%s] st=%d view=%p\n", vid.c_str(), vtype.c_str(), static_cast<int>(vst), static_cast<void *>(v));
            if (vst != PRO_TK_NO_ERROR || !v) return (vst == PRO_TK_NO_ERROR) ? PRO_TK_GENERAL_ERROR : vst;
            views[vid] = v;

            apply_view_display(v, vs);
            ProDwgSheetRegenerate(drw, 1);
            if (win_id != PRO_VALUE_UNUSED) ProWindowRepaint(win_id);
            log_view_outline(drw, sc, v, vid + "_p1", log);
        }

        // Pass 1b: 2D Sections + arrows + 45-deg cross-hatching (all projected views exist, before broken!)
        for (const Json &vs : plan["views"]) {
            std::string vid = vs.at("id").get<std::string>();
            if (!views.count(vid) || !vs.contains("section")) continue;
            ProView v = views[vid];
            const Json &sec = vs["section"];
            ProName wx{};
            wcsncpy_s(wx, PRO_NAME_SIZE, utf8_to_w(sec.at("name").get<std::string>()).c_str(), _TRUNCATE);
            ProView arrow_v = nullptr;
            std::string av_id;
            if (sec.contains("arrow_view")) av_id = sec["arrow_view"].get<std::string>();
            else if (sec.contains("arrow_view_post")) av_id = sec["arrow_view_post"].get<std::string>();
            if (!av_id.empty() && views.count(av_id)) {
                arrow_v = views[av_id];
            }
            ProError sst = ProDrawingView2DSectionSet(drw, v, wx, PRO_VIEW_SECTION_AREA_FULL, nullptr, nullptr, arrow_v);
            ProError tst2 = ProDrawingView2DSectionTotalSet(drw, v);
            ProDrawingViewRegenerate(drw, v);
            // Apply crisp GB/T 45-deg metal cross-hatching (2.5 mm spacing)
            ProXsec xs_handle{};
            wcsncpy_s(xs_handle.xsec_name, PRO_NAME_SIZE, wx, _TRUNCATE);
            xs_handle.solid_owner = solid;
            ProXsecXhatch xh{};
            xh.angle = 45;
            xh.spacing = 2.5;
            xh.offset = 0.0;
            ProError xhst = ProXsecCompXhatchReplace(&xs_handle, 0, v, &xh);
            if (xhst != PRO_TK_NO_ERROR) {
                xhst = ProXsecCompXhatchReplace(&xs_handle, -1, v, &xh);
            }
            ProDrawingViewRegenerate(drw, v);
            if (arrow_v) ProDrawingViewRegenerate(drw, arrow_v);
            log_printf(log, "  VIEW_XSEC[%s sec=%s arrow=%s] set_st=%d total_st=%d xh_st=%d\n",
                       vid.c_str(), sec.at("name").get<std::string>().c_str(), av_id.c_str(),
                       static_cast<int>(sst), static_cast<int>(tst2), static_cast<int>(xhst));
        }

        // Pass 1c: Detail views (created BEFORE broken so right-segment refs match parent transform!)
        for (const Json &vs : plan["views"]) {
            std::string vid = vs.at("id").get<std::string>();
            std::string vtype = vs.at("type").get<std::string>();
            if (vtype != "detail") continue;
            std::string pid = vs.at("parent").get<std::string>();
            auto pit = views.find(pid);
            if (pit == views.end()) {
                log_printf(log, "VIEW_MISSING_PARENT[%s -> %s]\n", vid.c_str(), pid.c_str());
                return PRO_TK_E_NOT_FOUND;
            }
            ProView pv = pit->second;
            ProSelection ref_sel = nullptr;
            Pro3dPnt ref_pt3d{};
            if (!resolve_draw_selection(drw, solid, surfs, pv, vs.at("ref"), ref_sel, ref_pt3d, log, vid.c_str())) {
                log_printf(log, "DETAIL_REF_MISS[%s]\n", vid.c_str());
                return PRO_TK_E_NOT_FOUND;
            }
            ProMatrix vtrf{};
            ProDrawingViewTransformGet(drw, pv, PRO_B_TRUE, vtrf);
            ProPoint3d center_scn{};
            ProPntTrfEval(ref_pt3d, vtrf, center_scn);
            double rad_mm = vs.value("circle_radius_mm", 18.0);
            ProCurvedata *spl = make_detail_circle_spline(center_scn[0], center_scn[1], rad_mm * sc.scale_mm_to_scn);
            double px = vs.at("pos_mm").at(0).get<double>();
            double py = vs.at("pos_mm").at(1).get<double>();
            ProPoint3d pos_scn{};
            sc.mm_to_scn(px, py, pos_scn);
            ProView v = nullptr;
            ProError vst = ProDrawingViewDetailCreate(drw, pv, ref_sel, spl, pos_scn, &v);
            ProCurvedataFree(spl);
            ProSelectionFree(&ref_sel);
            log_printf(log, "VIEW_CREATE[%s type=detail] st=%d view=%p\n", vid.c_str(), static_cast<int>(vst), static_cast<void *>(v));
            if (vst != PRO_TK_NO_ERROR || !v) return (vst == PRO_TK_NO_ERROR) ? PRO_TK_GENERAL_ERROR : vst;
            views[vid] = v;
            ProDrawingViewDetailBoundarySet(drw, v, PRO_DETAIL_BOUNDARY_CIRCLE, PRO_B_TRUE);
            if (vs.contains("scale")) ProDrawingViewScaleSet(drw, v, vs["scale"].get<double>());
            if (vs.contains("label")) {
                ProName wlbl{};
                wcsncpy_s(wlbl, PRO_NAME_SIZE, utf8_to_w(vs["label"].get<std::string>()).c_str(), _TRUNCATE);
                ProDrawingViewNameSet(drw, v, wlbl);
            }
            apply_view_display(v, vs);
            ProDrawingViewRegenerate(drw, v);
            log_view_outline(drw, sc, v, vid + "_p1c", log);
        }

        // Pass 2: Broken visible areas (applied after projected, section & detail views exist)
        for (const Json &vs : plan["views"]) {
            std::string vid = vs.at("id").get<std::string>();
            if (!views.count(vid) || !vs.contains("broken")) continue;
            ProView v = views[vid];
            const Json &brk = vs["broken"];
            ProSelection s1 = nullptr, s2 = nullptr;
            Pro3dPnt p1{}, p2{};
            bool ok1 = resolve_draw_selection(drw, solid, surfs, v, brk.at("first"), s1, p1, log, (vid + "_brk1").c_str());
            bool ok2 = resolve_draw_selection(drw, solid, surfs, v, brk.at("second"), s2, p2, log, (vid + "_brk2").c_str());
            log_printf(log, "  VIEW_BROKEN_PRE[%s] ok1=%d p1=(%.1f,%.1f,%.1f) ok2=%d p2=(%.1f,%.1f,%.1f)\n",
                       vid.c_str(), ok1 ? 1 : 0, p1[0], p1[1], p1[2], ok2 ? 1 : 0, p2[0], p2[1], p2[2]);
            if (ok1 && ok2) {
                ProViewBrokenDir bdir = (brk.value("dir", std::string("vertical")) == "horizontal")
                                            ? PRO_VIEW_BROKEN_DIR_HORIZONTAL
                                            : PRO_VIEW_BROKEN_DIR_VERTICAL;
                ProViewBrokenLineStyle bstyle = PRO_VIEW_BROKEN_LINE_S_CURVE_GEOMETRY;
                std::string bs = brk.value("style", std::string("s_curve_geom"));
                ProCurvedata *brk_crv = nullptr;
                if (bs == "straight") bstyle = PRO_VIEW_BROKEN_LINE_STRAIGHT;
                else if (bs == "s_curve") bstyle = PRO_VIEW_BROKEN_LINE_S_CURVE_OUTLINE;
                else if (bs == "s_curve_geom") bstyle = PRO_VIEW_BROKEN_LINE_S_CURVE_GEOMETRY;
                else if (bs == "heartbeat") bstyle = PRO_VIEW_BROKEN_LINE_HEART_BEAT_OUTLINE;
                else if (bs == "heartbeat_geom") bstyle = PRO_VIEW_BROKEN_LINE_HEART_BEAT_GEOMETRY;
                else if (bs == "sketch") {
                    bstyle = PRO_VIEW_BROKEN_LINE_SKETCH;
                    // Build crisp GB/T vertical wavy break spline (+/- 1.6 mm amplitude) along view height
                    ProMatrix vtrf{};
                    ProDrawingViewTransformGet(drw, v, PRO_B_TRUE, vtrf);
                    ProPoint3d sp1{};
                    ProPntTrfEval(p1, vtrf, sp1);
                    ProPoint3d vol[2]{};
                    ProDrawingViewOutlineGet(drw, v, vol);
                    double y_lo = vol[0][1] - 3.0 * sc.scale_mm_to_scn;
                    double y_hi = vol[1][1] + 3.0 * sc.scale_mm_to_scn;
                    double amp = brk.value("wave_amp_mm", 1.8) * sc.scale_mm_to_scn;
                    const int N = 9;
                    double *pars = nullptr;
                    ProPoint3d *pnts = nullptr;
                    ProPoint3d *tans = nullptr;
                    ProArrayAlloc(N, sizeof(double), 1, reinterpret_cast<ProArray *>(&pars));
                    ProArrayAlloc(N, sizeof(ProPoint3d), 1, reinterpret_cast<ProArray *>(&pnts));
                    ProArrayAlloc(N, sizeof(ProPoint3d), 1, reinterpret_cast<ProArray *>(&tans));
                    for (int k = 0; k < N; ++k) {
                        double t = static_cast<double>(k) / (N - 1);
                        pars[k] = t;
                        double ang = t * 2.0 * M_PI;
                        pnts[k][0] = sp1[0] + amp * std::sin(ang);
                        pnts[k][1] = y_lo + t * (y_hi - y_lo);
                        pnts[k][2] = 0.0;
                        tans[k][0] = amp * 2.0 * M_PI * std::cos(ang);
                        tans[k][1] = (y_hi - y_lo);
                        tans[k][2] = 0.0;
                    }
                    ProCurvedataAlloc(&brk_crv);
                    ProSplinedataInit(pars, pnts, tans, N, brk_crv);
                }
                ProError bst = ProDrawingViewBrokenVisibleAreaSet(drw, v, bdir, s1, s2, bstyle, brk_crv);
                if (bst != PRO_TK_NO_ERROR && bstyle != PRO_VIEW_BROKEN_LINE_STRAIGHT) {
                    log_printf(log, "  VIEW_BROKEN_RETRY_STRAIGHT[%s prev_st=%d]\n", vid.c_str(), static_cast<int>(bst));
                    bst = ProDrawingViewBrokenVisibleAreaSet(drw, v, bdir, s1, s2, PRO_VIEW_BROKEN_LINE_STRAIGHT, nullptr);
                }
                if (brk_crv) ProCurvedataFree(brk_crv);
                ProDrawingViewRegenerate(drw, v);
                log_printf(log, "  VIEW_BROKEN[%s style=%s] st=%d\n", vid.c_str(), bs.c_str(), static_cast<int>(bst));
            }
            if (s1) ProSelectionFree(&s1);
            if (s2) ProSelectionFree(&s2);
            log_view_outline(drw, sc, v, vid + "_brk", log);
        }

        // Pass 3: Exact view center placement (all views including details)
        for (const Json &vs : plan["views"]) {
            std::string vid = vs.at("id").get<std::string>();
            if (!views.count(vid)) continue;
            ProView v = views[vid];
            if (vs.contains("move_center_mm")) {
                ProPoint3d ol[2]{};
                if (ProDrawingViewOutlineGet(drw, v, ol) == PRO_TK_NO_ERROR) {
                    double cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
                    sc.scn_to_mm(ol[0], cx0, cy0);
                    sc.scn_to_mm(ol[1], cx1, cy1);
                    double cur_cx = 0.5 * (cx0 + cx1);
                    double cur_cy = 0.5 * (cy0 + cy1);
                    double tgt_cx = vs["move_center_mm"].at(0).get<double>();
                    double tgt_cy = vs["move_center_mm"].at(1).get<double>();
                    ProVector mv{(tgt_cx - cur_cx) * sc.scale_mm_to_scn, (tgt_cy - cur_cy) * sc.scale_mm_to_scn, 0.0};
                    ProDrawingViewMove(drw, v, mv);
                    ProDrawingViewRegenerate(drw, v);
                    log_view_outline(drw, sc, v, vid + "_moved", log);
                }
            }
        }
        ProDwgSheetRegenerate(drw, 1);
    }

    ProDwgSheetRegenerate(drw, 1);

    // 8. Centerlines (either native model axis show or crisp view-attached CTRLFONT lines)
    if (plan.contains("centerlines")) {
        for (const Json &cl : plan["centerlines"]) {
            std::string vid = cl.at("view").get<std::string>();
            if (!views.count(vid)) continue;
            ProView v = views[vid];
            std::string mode = cl.value("mode", std::string("3d_line"));
            if (mode == "model_axis") {
                ProModelitem aitem{};
                if (find_datum_axis_item_by_name(solid, cl.at("axis").get<std::string>(), aitem)) {
                    ProError ast = ProAnnotationShow(reinterpret_cast<ProAnnotation *>(&aitem), nullptr, v);
                    log_printf(log, "  AXIS_SHOW[%s/%s] st=%d\n", vid.c_str(), cl.at("axis").get<std::string>().c_str(), static_cast<int>(ast));
                }
            } else if (mode == "3d_line") {
                ProMatrix vtrf{};
                ProDrawingViewTransformGet(drw, v, PRO_B_TRUE, vtrf);
                ProPoint3d p1_3d{cl["start"].at(0).get<double>(), cl["start"].at(1).get<double>(), cl["start"].at(2).get<double>()};
                ProPoint3d p2_3d{cl["end"].at(0).get<double>(), cl["end"].at(1).get<double>(), cl["end"].at(2).get<double>()};
                ProPoint3d s1{}, s2{};
                ProPntTrfEval(p1_3d, vtrf, s1);
                ProPntTrfEval(p2_3d, vtrf, s2);
                if (cl.contains("break_shift_mm")) {
                    double bsh = cl["break_shift_mm"].get<double>() * sc.scale_mm_to_scn;
                    s1[0] += bsh;
                    s2[0] += bsh;
                }
                s1[2] = s2[2] = 0.0;
                draw_line_scn(drw, v, s1, s2, 0.18 * sc.scale_mm_to_scn, L"CTRLFONT");
            } else if (mode == "view_mid_h" || mode == "view_mid_v") {
                ProPoint3d ol[2]{};
                if (ProDrawingViewOutlineGet(drw, v, ol) == PRO_TK_NO_ERROR) {
                    double x0, y0, x1, y1;
                    sc.scn_to_mm(ol[0], x0, y0);
                    sc.scn_to_mm(ol[1], x1, y1);
                    double ext = cl.value("extend_mm", 4.0);
                    double ext_r = cl.value("extend_right_mm", ext);
                    double off = cl.value("offset_mm", 0.0);
                    if (mode == "view_mid_h") {
                        double cy = 0.5 * (y0 + y1) + off;
                        draw_line_mm(drw, sc, v, x0 - ext, cy, x1 + ext_r, cy, 0.18, L"CTRLFONT");
                    } else {
                        double cx = 0.5 * (x0 + x1) + off;
                        draw_line_mm(drw, sc, v, cx, y0 - ext, cx, y1 + ext_r, 0.18, L"CTRLFONT");
                    }
                }
            } else if (mode == "mm_line") {
                draw_line_mm(drw, sc, v,
                             cl["p1"].at(0).get<double>(), cl["p1"].at(1).get<double>(),
                             cl["p2"].at(0).get<double>(), cl["p2"].at(1).get<double>(),
                             0.18, L"CTRLFONT");
            }
        }
    }

    // 9. Create Associative Dimensions
    int dims_ok = 0, dims_fail = 0;
    if (plan.contains("dimensions")) {
        for (const Json &ds : plan["dimensions"]) {
            std::string did = ds.value("id", std::string("dim"));
            std::string vid = ds.at("view").get<std::string>();
            if (!views.count(vid)) {
                log_printf(log, "  DIM_SKIP[%s] missing view %s\n", did.c_str(), vid.c_str());
                dims_fail++;
                continue;
            }
            ProView v = views[vid];
            const Json &refs = ds.at("refs");
            int nref = static_cast<int>(refs.size());
            if (nref < 1 || nref > 2) {
                dims_fail++;
                continue;
            }

            ProDimAttachment *att_arr = nullptr;
            ProDimSense *sense_arr = nullptr;
            ProArrayAlloc(nref, sizeof(ProDimAttachment), 1, reinterpret_cast<ProArray *>(&att_arr));
            ProArrayAlloc(nref, sizeof(ProDimSense), 1, reinterpret_cast<ProArray *>(&sense_arr));
            bool ref_ok = true;
            for (int r = 0; r < nref; ++r) {
                att_arr[r][0] = nullptr;
                att_arr[r][1] = nullptr;
                Pro3dPnt pt3d{};
                if (!resolve_draw_selection(drw, solid, surfs, v, refs[r], att_arr[r][0], pt3d, log, (did + "_r" + std::to_string(r)).c_str())) {
                    ref_ok = false;
                }
                std::memset(&sense_arr[r], 0, sizeof(ProDimSense));
                std::string stype = refs[r].value("sense", std::string("none"));
                if (stype == "center") {
                    sense_arr[r].type = PRO_DIM_SNS_TYP_PNT;
                    sense_arr[r].sense = PRO_POINT_TYP_CENTER;
                } else if (stype == "start") {
                    sense_arr[r].type = PRO_DIM_SNS_TYP_PNT;
                    sense_arr[r].sense = PRO_POINT_TYP_END1;
                } else if (stype == "end") {
                    sense_arr[r].type = PRO_DIM_SNS_TYP_PNT;
                    sense_arr[r].sense = PRO_POINT_TYP_END2;
                } else if (stype == "mid") {
                    sense_arr[r].type = PRO_DIM_SNS_TYP_PNT;
                    sense_arr[r].sense = PRO_POINT_TYP_MIDPT;
                } else {
                    sense_arr[r].type = PRO_DIM_SNS_TYP_NONE;
                }
            }

            if (!ref_ok) {
                for (int r = 0; r < nref; ++r)
                    if (att_arr[r][0]) ProSelectionFree(&att_arr[r][0]);
                ProArrayFree(reinterpret_cast<ProArray *>(&att_arr));
                ProArrayFree(reinterpret_cast<ProArray *>(&sense_arr));
                dims_fail++;
                continue;
            }

            ProDimOrient orient = PRO_DIM_ORNT_NONE;
            std::string ostr = ds.value("orient", std::string("none"));
            if (ostr == "horiz") orient = PRO_DIM_ORNT_HORIZ;
            else if (ostr == "vert") orient = PRO_DIM_ORNT_VERT;
            else if (ostr == "slanted") orient = PRO_DIM_ORNT_SLANTED;

            ProPoint3d loc_scn{};
            if (ds.contains("pos_mm")) {
                sc.mm_to_scn(ds["pos_mm"].at(0).get<double>(), ds["pos_mm"].at(1).get<double>(), loc_scn);
            } else if (ds.contains("rel_view_mm")) {
                ProPoint3d ol[2]{};
                ProDrawingViewOutlineGet(drw, v, ol);
                double vx0, vy0, vx1, vy1;
                sc.scn_to_mm(ol[0], vx0, vy0);
                sc.scn_to_mm(ol[1], vx1, vy1);
                std::string anchor = ds.value("anchor", std::string("center"));
                double ax = 0.5 * (vx0 + vx1), ay = 0.5 * (vy0 + vy1);
                if (anchor == "top") ay = vy1;
                else if (anchor == "bottom") ay = vy0;
                else if (anchor == "left") ax = vx0;
                else if (anchor == "right") ax = vx1;
                else if (anchor == "top_left") { ax = vx0; ay = vy1; }
                else if (anchor == "top_right") { ax = vx1; ay = vy1; }
                else if (anchor == "bottom_left") { ax = vx0; ay = vy0; }
                else if (anchor == "bottom_right") { ax = vx1; ay = vy0; }
                sc.mm_to_scn(ax + ds["rel_view_mm"].at(0).get<double>(),
                             ay + ds["rel_view_mm"].at(1).get<double>(), loc_scn);
            }

            ProDimension dim{};
            ProError dst = ProDrawingDimensionCreate(drw, att_arr, sense_arr, orient, loc_scn, PRO_B_FALSE, &dim);
            if (dst == PRO_TK_NO_ERROR) {
                double val = 0.0;
                ProDimensionValueGet(&dim, &val);
                if (ds.contains("prefix")) {
                    std::wstring wp = creo_encode_symbols(utf8_to_w(ds["prefix"].get<std::string>()));
                    ProDimensionPrefixSet(&dim, const_cast<wchar_t *>(wp.c_str()));
                }
                if (ds.contains("suffix")) {
                    std::wstring ws = creo_encode_symbols(utf8_to_w(ds["suffix"].get<std::string>()));
                    ProDimensionSuffixSet(&dim, const_cast<wchar_t *>(ws.c_str()));
                }
                if (ds.contains("text_lines")) {
                    const Json &tl = ds["text_lines"];
                    ProWstring *wlines = nullptr;
                    ProArrayAlloc(static_cast<int>(tl.size()), sizeof(ProWstring), 1, reinterpret_cast<ProArray *>(&wlines));
                    std::vector<std::wstring> keep;
                    keep.reserve(tl.size());
                    for (size_t li = 0; li < tl.size(); ++li) {
                        keep.push_back(creo_encode_symbols(utf8_to_w(tl[li].get<std::string>())));
                        wlines[li] = const_cast<wchar_t *>(keep.back().c_str());
                    }
                    ProDimensionTextWstringsSet(&dim, wlines);
                    ProArrayFree(reinterpret_cast<ProArray *>(&wlines));
                }
                if (ds.contains("tol_upper") && ds.contains("tol_lower")) {
                    double up = ds["tol_upper"].get<double>();
                    double lo = ds["tol_lower"].get<double>();
                    // Creo ProDimensionToleranceSet takes lower as positive magnitude below nominal!
                    if (ds.value("tol_sym", false)) {
                        ProDimensionToltypeSet(&dim, PRO_TOL_PLUS_MINUS_SYM);
                        ProDimensionToleranceSet(&dim, std::fabs(up), std::fabs(up));
                    } else {
                        ProDimensionToltypeSet(&dim, PRO_TOL_PLUS_MINUS);
                        ProDimensionToleranceSet(&dim, up, -lo);
                    }
                    int tdec = ds.value("tol_decimals", 3);
                    ProDimensionTolerancedecimalsSet(&dim, tdec);
                }
                if (ds.contains("decimals")) {
                    ProDimensionDecimalsSet(&dim, ds["decimals"].get<int>());
                }
                ProAnnotationShow(reinterpret_cast<ProAnnotation *>(&dim), nullptr, v);
                ProDrawingDimensionMove(drw, &dim, loc_scn);
                dims_ok++;
                double lx_mm = 0, ly_mm = 0;
                sc.scn_to_mm(loc_scn, lx_mm, ly_mm);
                log_printf(log, "  DIM_OK[%s view=%s] id=%d val=%.3f pos=(%.1f,%.1f)\n",
                           did.c_str(), vid.c_str(), dim.id, val, lx_mm, ly_mm);
            } else {
                dims_fail++;
                log_printf(log, "  DIM_FAIL[%s view=%s] st=%d\n", did.c_str(), vid.c_str(), static_cast<int>(dst));
            }

            for (int r = 0; r < nref; ++r)
                if (att_arr[r][0]) ProSelectionFree(&att_arr[r][0]);
            ProArrayFree(reinterpret_cast<ProArray *>(&att_arr));
            ProArrayFree(reinterpret_cast<ProArray *>(&sense_arr));
        }
    }
    log_printf(log, "DIMS_SUMMARY ok=%d fail=%d\n", dims_ok, dims_fail);

    // 10. GB/T Surface Roughness Marks
    if (plan.contains("roughness_rest")) {
        std::string rrest = plan["roughness_rest"].get<std::string>();
        double rx = plan.value("roughness_rest_x", 374.0);
        double ry = plan.value("roughness_rest_y", 272.0);
        add_note_mm(drw, sc, nullptr, rx - 11.5, ry + 1.0, {"其余"}, 3.5, font_name, PRO_HORZJUST_LEFT, PRO_VERTJUST_BOTTOM);
        draw_gb_ra_mark(drw, sc, nullptr, rx + 2.0, ry, rrest, "up", false, font_name, 8.0, false);
    }
    if (plan.contains("roughness_marks")) {
        for (const Json &rm : plan["roughness_marks"]) {
            ProView rv = nullptr;
            if (rm.contains("view") && views.count(rm["view"].get<std::string>())) {
                rv = views[rm["view"].get<std::string>()];
            }
            double rx = 0.0, ry = 0.0;
            if (rm.contains("pos_mm")) {
                rx = rm["pos_mm"].at(0).get<double>();
                ry = rm["pos_mm"].at(1).get<double>();
            } else if (rm.contains("pt3d") && rv) {
                ProMatrix vtrf{};
                ProDrawingViewTransformGet(drw, rv, PRO_B_TRUE, vtrf);
                ProPoint3d p3{rm["pt3d"].at(0).get<double>(), rm["pt3d"].at(1).get<double>(), rm["pt3d"].at(2).get<double>()};
                ProPoint3d ps{};
                ProPntTrfEval(p3, vtrf, ps);
                sc.scn_to_mm(ps, rx, ry);
                rx += rm.value("break_shift_mm", 0.0);
                rx += rm.value("dx_mm", 0.0);
                ry += rm.value("dy_mm", 0.0);
            }
            if (rm.contains("ext_line_mm")) {
                const Json &el = rm["ext_line_mm"];
                draw_line_mm(drw, sc, rv, el.at(0).get<double>(), el.at(1).get<double>(),
                             el.at(2).get<double>(), el.at(3).get<double>(), 0.20);
            }
            draw_gb_ra_mark(
                drw, sc, rv, rx, ry,
                rm.at("ra").get<std::string>(),
                rm.value("orient", std::string("up")),
                rm.value("machined", true),
                font_name,
                rm.value("shelf_len_mm", 0.0),
                rm.value("leader_on_left", false));
        }
    }

    // 11. Free Chinese Notes (View titles, Technical requirements, etc.)
    if (plan.contains("notes")) {
        for (const Json &ns : plan["notes"]) {
            ProView nv = nullptr;
            if (ns.contains("view") && views.count(ns["view"].get<std::string>())) {
                nv = views[ns["view"].get<std::string>()];
            }
            double nx = ns.at("pos_mm").at(0).get<double>();
            double ny = ns.at("pos_mm").at(1).get<double>();
            std::vector<std::string> lns;
            for (const Json &l : ns.at("lines")) lns.push_back(l.get<std::string>());
            double h = ns.value("height_mm", 3.5);
            std::string hj = ns.value("hjust", std::string("left"));
            ProHorizontalJustification hje = (hj == "center") ? PRO_HORZJUST_CENTER
                                           : (hj == "right") ? PRO_HORZJUST_RIGHT : PRO_HORZJUST_LEFT;
            std::string vj = ns.value("vjust", std::string("top"));
            ProVerticalJustification vje = (vj == "bottom") ? PRO_VERTJUST_BOTTOM
                                         : (vj == "middle") ? PRO_VERTJUST_MIDDLE : PRO_VERTJUST_TOP;
            bool has_ld = ns.contains("leader_mm");
            double lx = has_ld ? ns["leader_mm"].at(0).get<double>() : 0.0;
            double ly = has_ld ? ns["leader_mm"].at(1).get<double>() : 0.0;
            ProError nst = add_note_mm(drw, sc, nv, nx, ny, lns, h, font_name, hje, vje, 0.0, has_ld, lx, ly, log);
            if (nst != PRO_TK_NO_ERROR) {
                log_printf(log, "  NOTE_FAIL st=%d\n", static_cast<int>(nst));
            }
        }
    }

    // 11b. Hide helper datum point feature DWG_REF_PNTS in part and persist layer status
    {
        ProFeature pfeat{};
        if (dwg_find_feat_by_name(solid, L"DWG_REF_PNTS", &pfeat) == PRO_TK_NO_ERROR) {
            ProModelitemHide(&pfeat);
            ProLayerDisplaystatusSave(g_dwg_job.part_mdl);
            ProMdlSave(g_dwg_job.part_mdl);
        }
    }

    // 12. Regenerate, repaint & save .drw.1
    ProError regen_st = ProDwgSheetRegenerate(drw, 1);
    if (win_id != PRO_VALUE_UNUSED) {
        ProWindowRepaint(win_id);
    }
    ProError save_st = ProMdlSave(drw);
    log_printf(log, "DWG_SAVE regen_st=%d save_st=%d\n", static_cast<int>(regen_st), static_cast<int>(save_st));
    if (save_st != PRO_TK_NO_ERROR) return save_st;

    // 13. Export Monochrome 300-DPI A3 PDF via ProPDFExport
    {
        ProPDFOptions pdf_opts = nullptr;
        ProPDFoptionsAlloc(&pdf_opts);
        ProPDFoptionsIntpropertySet(pdf_opts, PRO_PDFOPT_EXPORT_MODE, PRO_PDF_2D_DRAWING);
        ProPDFoptionsIntpropertySet(pdf_opts, PRO_PDFOPT_SIZE, A3_SIZE_PLOT);
        ProPDFoptionsIntpropertySet(pdf_opts, PRO_PDFOPT_COLOR_DEPTH, PRO_PDF_CD_MONO);
        ProPDFoptionsIntpropertySet(pdf_opts, PRO_PDFOPT_HIDDENLINE_MODE, PRO_PDF_HLM_DASHED);
        ProPDFoptionsIntpropertySet(pdf_opts, PRO_PDFOPT_FONT_STROKE, PRO_PDF_STROKE_ALL_FONTS);
        ProPDFoptionsIntpropertySet(pdf_opts, PRO_PDFOPT_RASTER_DPI, 300);
        ProPDFoptionsBoolpropertySet(pdf_opts, PRO_PDFOPT_LAUNCH_VIEWER, PRO_B_FALSE);
        ProPDFoptionsBoolpropertySet(pdf_opts, PRO_PDFOPT_SEARCHABLE_TEXT, PRO_B_FALSE);

        ProPath pdf_path{};
        wcsncpy_s(pdf_path, PRO_PATH_SIZE, utf8_to_w(drw_name + ".pdf").c_str(), _TRUNCATE);
        ProError pst = ProPDFExport(drw, pdf_path, pdf_opts);
        ProPDFoptionsFree(pdf_opts);
        log_printf(log, "EXPORT_PDF[%s.pdf] st=%d\n", drw_name.c_str(), static_cast<int>(pst));
        if (pst != PRO_TK_NO_ERROR) return pst;
    }

    // 14. Export AutoCAD DWG via Pro2dExport
    {
        Pro2dExportdata dwg_data = nullptr;
        Pro2dExportdataAlloc(&dwg_data);
        Pro2dExportdataSheetoptionSet(dwg_data, PRO2DEXPORT_CURRENT_TO_MODEL_SPACE);
        ProPath dwg_path{};
        wcsncpy_s(dwg_path, PRO_PATH_SIZE, utf8_to_w(drw_name + ".dwg").c_str(), _TRUNCATE);
        ProError est = Pro2dExport(PRO_DWG_FILE, dwg_path, drw, dwg_data);
        Pro2dExportdataFree(dwg_data);
        log_printf(log, "EXPORT_DWG[%s.dwg] st=%d\n", drw_name.c_str(), static_cast<int>(est));
        if (est != PRO_TK_NO_ERROR) return est;
    }

    return (dims_fail == 0) ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
}

/* ------------------------------------------------------------------ */
/* Isolated Drawing Reopen Verification                               */
/* ------------------------------------------------------------------ */

inline ProError run_drawing_verify(FILE *log) {
    Json plan = dwg_load_plan_json("creobuild_plan.json");
    std::string drw_name = plan.at("drawing_name").get<std::string>();

    ProMdlName w_drw{};
    wcsncpy_s(w_drw, PRO_MDLNAME_SIZE, utf8_to_w(drw_name).c_str(), _TRUNCATE);
    ProMdl mdl = nullptr;
    ProError st = ProMdlnameRetrieve(w_drw, PRO_MDLFILE_DRAWING, &mdl);
    if (st != PRO_TK_NO_ERROR || !mdl) {
        log_st(log, "verify_drw_retrieve", st);
        return (st == PRO_TK_NO_ERROR) ? PRO_TK_E_NOT_FOUND : st;
    }
    ProDrawing drw = static_cast<ProDrawing>(mdl);
    int win_id = PRO_VALUE_UNUSED;
    ProMdlWindowGet(drw, &win_id);
    if (win_id == PRO_VALUE_UNUSED) {
        ProObjectwindowMdlnameCreate(w_drw, PRO_DRAWING, &win_id);
    }
    ProMdlDisplay(drw);
    if (win_id != PRO_VALUE_UNUSED) {
        ProWindowCurrentSet(win_id);
        ProWindowActivate(win_id);
        ProWindowRepaint(win_id);
    }

    ProError rst = ProDwgSheetRegenerate(drw, 1);
    int n_views = 0, n_tables = 0, n_notes = 0, n_solids = 0;
    ProView *vs = nullptr;
    if (ProDrawingViewsCollect(drw, &vs) == PRO_TK_NO_ERROR && vs) {
        ProArraySizeGet(vs, &n_views);
        ProArrayFree(reinterpret_cast<ProArray *>(&vs));
    }
    ProDwgtable *ts = nullptr;
    if (ProDrawingTablesCollect(drw, &ts) == PRO_TK_NO_ERROR && ts) {
        ProArraySizeGet(ts, &n_tables);
        ProArrayFree(reinterpret_cast<ProArray *>(&ts));
    }
    ProDtlnote *ns = nullptr;
    if (ProDrawingDtlnotesCollect(drw, nullptr, 1, &ns) == PRO_TK_NO_ERROR && ns) {
        ProArraySizeGet(ns, &n_notes);
        ProArrayFree(reinterpret_cast<ProArray *>(&ns));
    }
    ProSolid *ss = nullptr;
    if (ProDrawingSolidsCollect(drw, &ss) == PRO_TK_NO_ERROR && ss) {
        ProArraySizeGet(ss, &n_solids);
        ProArrayFree(reinterpret_cast<ProArray *>(&ss));
    }
    log_printf(log, "DRW_VERIFY name=%s regen_st=%d solids=%d views=%d tables=%d notes=%d\n",
               drw_name.c_str(), static_cast<int>(rst), n_solids, n_views, n_tables, n_notes);
    if (rst != PRO_TK_NO_ERROR || n_solids < 1 || n_views < 2) {
        return (rst != PRO_TK_NO_ERROR) ? rst : PRO_TK_GENERAL_ERROR;
    }
    log_printf(log, "DRW_REOPEN_VERIFIED=true\n");
    return PRO_TK_NO_ERROR;
}

} // namespace dwg

} // namespace creobuild
