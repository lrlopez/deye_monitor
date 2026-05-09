#include "stats_screen.h"
#include <math.h>

// ── Paleta ────────────────────────────────────────────────────────────────
#define C_BG        lv_color_hex(0x0D1117)
#define C_CARD      lv_color_hex(0x161B22)
#define C_MUTED     lv_color_hex(0x6E7681)
#define C_WHITE     lv_color_hex(0xEAEAEA)
#define C_BAR_BG    lv_color_hex(0x21262D)

// Segmentos consumo
#define C_CON_PV    lv_color_hex(0x2ECC71)   // solar directo – verde
#define C_CON_DIS   lv_color_hex(0x4A9EFF)   // descarga bat. – azul
#define C_CON_IMP   lv_color_hex(0xBB6BD9)   // importación  – violeta

// Segmentos producción
#define C_PRO_LOAD  lv_color_hex(0xF5C518)   // autoconsumo – amarillo
#define C_PRO_CHG   lv_color_hex(0x4A9EFF)   // carga bat.  – azul
#define C_PRO_EXP   lv_color_hex(0xE88080)   // exportación – salmón

// ── Geometría ─────────────────────────────────────────────────────────────
// Pantalla 480×272
// Título:   y=0..27  (28px)
// Donuts:   y=28..177 (150px) → centro Y = 103, radio 55, grosor 18
// Leyenda:  y=180..272 (92px) → 3 filas de 30px
#define DONUT_R      55
#define ARC_W        18
#define COL_W       240
#define DONUT_CY    103
#define LEGEND_Y    180
#define LEG_ROW_H    30
#define DOT_SZ        8

// Punteros actualizables
static lv_obj_t* con_arcs[3];
static lv_obj_t* con_total_lbl;
static lv_obj_t* con_leg_val[3];
static lv_obj_t* con_leg_pct[3];

static lv_obj_t* pro_arcs[3];
static lv_obj_t* pro_total_lbl;
static lv_obj_t* pro_leg_val[3];
static lv_obj_t* pro_leg_pct[3];

// ── Anillo de fondo gris ──────────────────────────────────────────────────
static void make_bg_ring(lv_obj_t* parent, int cx, int cy) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, DONUT_R * 2, DONUT_R * 2);
    lv_obj_set_pos(arc,  cx - DONUT_R, cy - DONUT_R);
    lv_arc_set_bg_angles(arc, 0, 359);
    lv_obj_set_style_bg_opa(arc,  LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ARC_W,    LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc,  LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc,   LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
}

// ── Arco de segmento (inicialmente vacío) ─────────────────────────────────
static lv_obj_t* make_arc_segment(lv_obj_t* parent, int cx, int cy,
                                   lv_color_t color) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);

    lv_obj_set_size(arc, DONUT_R * 2, DONUT_R * 2);
    lv_obj_set_pos(arc,  cx - DONUT_R, cy - DONUT_R);

    // Ocultar pista y fondo
    lv_obj_set_style_bg_opa(arc,   LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc,  LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);

    // Ocultar knob
    lv_obj_set_style_bg_opa(arc,   LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc,  0, LV_PART_KNOB);

    // Indicador coloreado
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, ARC_W, LV_PART_INDICATOR);

    // Inicialmente invisible (start == end → longitud 0)
    lv_arc_set_angles(arc, 270, 270);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

// ── Actualiza ángulos de un conjunto de arcos ─────────────────────────────
static void update_donut(lv_obj_t* arcs[], const float vals[], int n,
                          float total) {
    float cum = 0.0f;
    for (int i = 0; i < n; i++) {
        float frac = (total > 0.01f) ? (vals[i] / total) : 0.0f;
        frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);

        if (frac < 0.005f) {
            lv_arc_set_angles(arcs[i], 270, 270);   // invisible
        } else {
            uint16_t a0 = (uint16_t)fmodf(cum * 360.0f + 270.0f, 360.0f);
            uint16_t a1 = (uint16_t)fmodf((cum + frac) * 360.0f + 270.0f, 360.0f);
            lv_arc_set_angles(arcs[i], a0, a1);
        }
        cum += frac;
    }
}

// ── Fila de leyenda ───────────────────────────────────────────────────────
//  [●] Nombre         XX%
//      X.XX kWh
static void make_legend_row(lv_obj_t* parent, int x, int y,
                             lv_color_t color, const char* name,
                             lv_obj_t** val_out, lv_obj_t** pct_out) {
    // Punto de color
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_set_size(dot, DOT_SZ, DOT_SZ);
    lv_obj_set_pos(dot, x, y + 4);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, 2, 0);

    // Nombre
    lv_obj_t* lbl_name = lv_label_create(parent);
    lv_obj_set_pos(lbl_name, x + DOT_SZ + 5, y + 1);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_name, C_MUTED, 0);
    lv_label_set_text(lbl_name, name);

    // Porcentaje (esquina derecha de la columna)
    *pct_out = lv_label_create(parent);
    lv_obj_set_pos(*pct_out, x + 150, y + 1);
    lv_obj_set_style_text_font(*pct_out, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(*pct_out, color, 0);
    lv_label_set_text(*pct_out, "--%");

    // Valor kWh
    *val_out = lv_label_create(parent);
    lv_obj_set_pos(*val_out, x + DOT_SZ + 5, y + 16);
    lv_obj_set_style_text_font(*val_out, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(*val_out, C_WHITE, 0);
    lv_label_set_text(*val_out, "-.-- kWh");
}

// ── Inicialización ────────────────────────────────────────────────────────
void stats_screen_init(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // Título
    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_width(title, 480);
    lv_obj_set_pos(title, 0, 7);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, C_MUTED, 0);
    lv_label_set_text(title, LV_SYMBOL_CHARGE "  Estadisticas del dia");

    // Separador horizontal
    lv_obj_t* sep_h = lv_obj_create(parent);
    lv_obj_set_size(sep_h, 480, 1);
    lv_obj_set_pos(sep_h, 0, 27);
    lv_obj_set_style_bg_color(sep_h, C_CARD, 0);
    lv_obj_set_style_bg_opa(sep_h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep_h, 0, 0);

    // Separador vertical
    lv_obj_t* sep_v = lv_obj_create(parent);
    lv_obj_set_size(sep_v, 1, 245);
    lv_obj_set_pos(sep_v, COL_W, 27);
    lv_obj_set_style_bg_color(sep_v, C_CARD, 0);
    lv_obj_set_style_bg_opa(sep_v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep_v, 0, 0);

    // Subtítulos de columna
    const char* sub_texts[2] = {"CONSUMO", "PRODUCCION"};
    for (int c = 0; c < 2; c++) {
        lv_obj_t* sub = lv_label_create(parent);
        lv_obj_set_pos(sub, c * COL_W, 30);
        lv_obj_set_width(sub, COL_W);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sub, C_MUTED, 0);
        lv_label_set_text(sub, sub_texts[c]);
    }

    int cx[2] = {120, 360};
    lv_color_t con_colors[3] = {C_CON_PV, C_CON_DIS, C_CON_IMP};
    lv_color_t pro_colors[3] = {C_PRO_LOAD, C_PRO_CHG, C_PRO_EXP};

    // ── Donut CONSUMO ─────────────────────────────────────────────────────
    make_bg_ring(parent, cx[0], DONUT_CY);
    for (int i = 0; i < 3; i++)
        con_arcs[i] = make_arc_segment(parent, cx[0], DONUT_CY, con_colors[i]);

    con_total_lbl = lv_label_create(parent);
    lv_obj_set_pos(con_total_lbl, cx[0] - 32, DONUT_CY - 18);
    lv_obj_set_size(con_total_lbl, 64, 36);
    lv_obj_set_style_text_align(con_total_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(con_total_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(con_total_lbl, C_WHITE, 0);
    lv_label_set_text(con_total_lbl, "--\nkWh");

    // ── Donut PRODUCCIÓN ──────────────────────────────────────────────────
    make_bg_ring(parent, cx[1], DONUT_CY);
    for (int i = 0; i < 3; i++)
        pro_arcs[i] = make_arc_segment(parent, cx[1], DONUT_CY, pro_colors[i]);

    pro_total_lbl = lv_label_create(parent);
    lv_obj_set_pos(pro_total_lbl, cx[1] - 32, DONUT_CY - 18);
    lv_obj_set_size(pro_total_lbl, 64, 36);
    lv_obj_set_style_text_align(pro_total_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(pro_total_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pro_total_lbl, C_WHITE, 0);
    lv_label_set_text(pro_total_lbl, "--\nkWh");

    // ── Leyendas ──────────────────────────────────────────────────────────
    const char* con_names[3] = {"Solar directo", "Descarga bat.", "Importacion"};
    const char* pro_names[3] = {"Autoconsumo",   "Carga bat.",   "Exportacion"};

    for (int i = 0; i < 3; i++) {
        make_legend_row(parent, 4, LEGEND_Y + i * LEG_ROW_H,
                        con_colors[i], con_names[i],
                        &con_leg_val[i], &con_leg_pct[i]);
        make_legend_row(parent, COL_W + 4, LEGEND_Y + i * LEG_ROW_H,
                        pro_colors[i], pro_names[i],
                        &pro_leg_val[i], &pro_leg_pct[i]);
    }
}

// ── Actualización de datos ────────────────────────────────────────────────
void stats_screen_update(const DailyStats& d) {
    if (!d.valid) return;

    // Consumo: solar_directo + descarga + importación = load total
    float pv_direct = d.load_kwh - d.batt_discharge_kwh - d.import_kwh;
    if (pv_direct < 0.0f) pv_direct = 0.0f;
    float con_vals[3] = {pv_direct, d.batt_discharge_kwh, d.import_kwh};
    float con_total   = d.load_kwh;

    // Producción: autoconsumo + carga_bat + exportación = pv total
    float pv_to_load = d.pv_kwh - d.export_kwh - d.batt_charge_kwh;
    if (pv_to_load < 0.0f) pv_to_load = 0.0f;
    float pro_vals[3] = {pv_to_load, d.batt_charge_kwh, d.export_kwh};
    float pro_total   = d.pv_kwh;

    update_donut(con_arcs, con_vals, 3, con_total);
    update_donut(pro_arcs, pro_vals, 3, pro_total);

    lv_label_set_text_fmt(con_total_lbl, "%.1f\nkWh", con_total);
    lv_label_set_text_fmt(pro_total_lbl, "%.1f\nkWh", pro_total);

    for (int i = 0; i < 3; i++) {
        float cp = (con_total > 0.01f) ? (con_vals[i] / con_total * 100.0f) : 0.0f;
        float pp = (pro_total > 0.01f) ? (pro_vals[i] / pro_total * 100.0f) : 0.0f;
        lv_label_set_text_fmt(con_leg_val[i], "%.2f kWh", con_vals[i]);
        lv_label_set_text_fmt(con_leg_pct[i], "%.1f%%",   cp);
        lv_label_set_text_fmt(pro_leg_val[i], "%.2f kWh", pro_vals[i]);
        lv_label_set_text_fmt(pro_leg_pct[i], "%.1f%%",   pp);
    }
}