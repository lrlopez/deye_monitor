#include "chart_screen.h"
#include "storage.h"
#include <time.h>

// ── Paleta ────────────────────────────────────────────────────────────────
#define C_BG    lv_color_hex(0x0D1117)
#define C_CARD  lv_color_hex(0x161B22)
#define C_MUTED lv_color_hex(0x6E7681)
#define C_WHITE lv_color_hex(0xEAEAEA)
#define C_PV    lv_color_hex(0xF5C518)
#define C_GRID  lv_color_hex(0x4A9EFF)
#define C_BATT  lv_color_hex(0x2ECC71)
#define C_LOAD  lv_color_hex(0xBB6BD9)
#define C_SOC   lv_color_hex(0x1A56DB)
#define C_GBTN  lv_color_hex(0x21262D)
#define C_GRID_LINE lv_color_hex(0x21262D)

// ── Layout 480×272 ────────────────────────────────────────────────────────
#define NAV_H  28
#define PWR_Y  NAV_H
#define PWR_H  138
#define LEG_Y  (PWR_Y + PWR_H)      // 166
#define LEG_H  16
#define SOC_Y  (LEG_Y + LEG_H)      // 182
#define SOC_H  54
#define XLAB_Y (SOC_Y + SOC_H + 2)  // 238

// Padding horizontal idéntico en ambos charts → x-labels alineadas
#define CH_PAD_L  38
#define CH_PAD_R   4
#define CH_PAD_TV  4

// ── Estado ────────────────────────────────────────────────────────────────
static DayData  s_day;
static int      s_offset      = 0;    // 0=hoy, -1=ayer, …, -6
static bool     s_active      = false;
static uint32_t s_last_tick   = 0;

// ── Widgets ───────────────────────────────────────────────────────────────
static lv_obj_t           *s_lbl_date;
static lv_obj_t           *s_btn_prev, *s_btn_next;
static lv_obj_t           *s_chart_pwr, *s_chart_soc;
static lv_chart_series_t  *s_spv, *s_sgrid, *s_sbatt, *s_sload, *s_ssoc;
static lv_obj_t           *s_popup, *s_popup_lbl;
static lv_obj_t           *s_no_data;

// ── Helpers de tiempo ─────────────────────────────────────────────────────
static uint32_t day_epoch_from_offset(int offset) {
    time_t now; time(&now);
    now += (time_t)offset * 86400;
    struct tm t; localtime_r(&now, &t);
    t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
    return (uint32_t)mktime(&t);
}

static const char* MESES[] = {
    "Enero","Febrero","Marzo","Abril","Mayo","Junio",
    "Julio","Agosto","Septiembre","Octubre","Noviembre","Diciembre"
};
static const char* DIASEM[] = {
    "Domingo","Lunes","Martes","Miercoles","Jueves","Viernes","Sabado"
};

static void update_date_label() {
    time_t t = (time_t)day_epoch_from_offset(s_offset);
    struct tm ti; localtime_r(&t, &ti);

    char buf[48];
    if      (s_offset ==  0) snprintf(buf,sizeof(buf),"Hoy, %d de %s",   ti.tm_mday, MESES[ti.tm_mon]);
    else if (s_offset == -1) snprintf(buf,sizeof(buf),"Ayer, %d de %s",  ti.tm_mday, MESES[ti.tm_mon]);
    else                     snprintf(buf,sizeof(buf),"%s, %d de %s",
                                 DIASEM[ti.tm_wday], ti.tm_mday, MESES[ti.tm_mon]);
    lv_label_set_text(s_lbl_date, buf);

    if (s_offset >= 0)  lv_obj_add_state(s_btn_next, LV_STATE_DISABLED);
    else                lv_obj_remove_state(s_btn_next, LV_STATE_DISABLED);
    if (s_offset <= -6) lv_obj_add_state(s_btn_prev, LV_STATE_DISABLED);
    else                lv_obj_remove_state(s_btn_prev, LV_STATE_DISABLED);
}

// ── Rango del chart de potencias ──────────────────────────────────────────
static void apply_range() {
    ChartConfig cfg = Storage.loadChartConfig();

    if (cfg.autoscale) {
        int32_t mn = 0, mx = 500;
        for (int h = 0; h < 24; h++) {
            if (!s_day.hours[h].valid) continue;
            const HourlyRecord& r = s_day.hours[h];
            int32_t vals[] = {
                (int32_t)r.pv_wh,
                (int32_t)r.import_wh,
                -(int32_t)r.export_wh,
                (int32_t)r.batt_charge_wh - r.batt_discharge_wh,
                (int32_t)r.load_wh,
            };
            for (auto v : vals) { if (v > mx) mx = v; if (v < mn) mn = v; }
        }
        int32_t margin = (mx - mn) / 8;
        if (margin < 200) margin = 200;
        lv_chart_set_range(s_chart_pwr, LV_CHART_AXIS_PRIMARY_Y, mn - margin, mx + margin);
    } else {
        int32_t m = (int32_t)cfg.max_kw * 1000;
        lv_chart_set_range(s_chart_pwr, LV_CHART_AXIS_PRIMARY_Y, -m, m);
    }
}

// ── Rellena los charts con s_day ──────────────────────────────────────────
static void update_charts() {
    // Si mostramos hoy, no dibujar horas futuras
    int cur_hour = 24;
    if (s_offset == 0) {
        time_t now; time(&now);
        struct tm t; localtime_r(&now, &t);
        cur_hour = t.tm_hour;
    }

    bool has_any = false;
    for (int h = 0; h < 24; h++) {
        const HourlyRecord& r = s_day.hours[h];
        bool valid = r.valid && (s_offset < 0 || h < cur_hour);

        if (!valid) {
            lv_chart_set_value_by_id(s_chart_pwr, s_spv,   h, LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart_pwr, s_sgrid, h, LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart_pwr, s_sbatt, h, LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart_pwr, s_sload, h, LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart_soc, s_ssoc,  h, LV_CHART_POINT_NONE);
        } else {
            has_any = true;
            lv_chart_set_value_by_id(s_chart_pwr, s_spv,   h, (int32_t)r.pv_wh);
            lv_chart_set_value_by_id(s_chart_pwr, s_sgrid, h, (int32_t)r.import_wh - r.export_wh);
            lv_chart_set_value_by_id(s_chart_pwr, s_sbatt, h, (int32_t)r.batt_charge_wh - r.batt_discharge_wh);
            lv_chart_set_value_by_id(s_chart_pwr, s_sload, h, (int32_t)r.load_wh);
            lv_chart_set_value_by_id(s_chart_soc, s_ssoc,  h, (int32_t)r.soc);
        }
    }

    if (has_any) {
        lv_obj_add_flag(s_no_data, LV_OBJ_FLAG_HIDDEN);
        apply_range();
    } else {
        lv_obj_remove_flag(s_no_data, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

static void load_day() {
    uint32_t dep = day_epoch_from_offset(s_offset);
    if (!Storage.getDayData(dep, s_day)) {
        s_day = DayData{};
        s_day.day_epoch = dep;
    }
    update_date_label();
    update_charts();
}

// ── Popup con valores del instante clicado ────────────────────────────────
static void show_popup(uint32_t h) {
    if (h >= 24 || !s_day.hours[h].valid) {
        lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const HourlyRecord& r = s_day.hours[h];
    int net_grid = (int)r.import_wh  - r.export_wh;
    int net_batt = (int)r.batt_charge_wh - r.batt_discharge_wh;

    char buf[160];
    snprintf(buf, sizeof(buf),
        "%02u:00 – %02u:00\n"
        "PV:    %.2f kWh\n"
        "Red:   %+.2f kWh\n"
        "Bat:   %+.2f kWh\n"
        "Carga: %.2f kWh\n"
        "SOC:   %u%%",
        (unsigned)h, (unsigned)(h + 1) % 24,
        r.pv_wh / 1000.0f,
        net_grid / 1000.0f,
        net_batt / 1000.0f,
        r.load_wh / 1000.0f,
        (unsigned)r.soc);

    lv_label_set_text(s_popup_lbl, buf);
    lv_obj_remove_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

// ── Callbacks ─────────────────────────────────────────────────────────────
static void prev_cb(lv_event_t*) { if (s_offset > -6) { s_offset--; load_day(); } }
static void next_cb(lv_event_t*) { if (s_offset <  0) { s_offset++; load_day(); } }

static void chart_click_cb(lv_event_t*) {
    uint32_t idx = lv_chart_get_pressed_point(s_chart_pwr);
    if (idx == LV_CHART_POINT_NONE)
        lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    else
        show_popup(idx);
}
static void popup_click_cb(lv_event_t*) { lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN); }

// ── Helpers de UI ─────────────────────────────────────────────────────────
static lv_obj_t* nav_btn(lv_obj_t* p, int x, int y, int w, int h, const char* txt,
                          lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(p);
    lv_obj_set_pos(btn, x, y); lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, C_GBTN, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x4A9EFF), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

static lv_obj_t* legend_dot(lv_obj_t* p, int x, int y, lv_color_t c, const char* txt) {
    lv_obj_t* dot = lv_obj_create(p);
    lv_obj_set_pos(dot, x, y + 4); lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_bg_color(dot, c, 0); lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, 4, 0);   lv_obj_set_style_border_width(dot, 0, 0);

    lv_obj_t* lbl = lv_label_create(p);
    lv_obj_set_pos(lbl, x + 11, y + 1);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, C_MUTED, 0);
    lv_label_set_text(lbl, txt);
    return lbl;
}

static lv_obj_t* make_chart(lv_obj_t* parent, int y, int h, int y_lo, int y_hi, int hdiv) {
    lv_obj_t* c = lv_chart_create(parent);
    lv_obj_set_pos(c, 0, y); lv_obj_set_size(c, 480, h);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, 24);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, y_lo, y_hi);
    lv_chart_set_div_line_count(c, hdiv, 0);

    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_line_color(c, C_GRID_LINE, LV_PART_MAIN);
    lv_obj_set_style_line_opa(c, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_left(c,   CH_PAD_L, 0);
    lv_obj_set_style_pad_right(c,  CH_PAD_R, 0);
    lv_obj_set_style_pad_top(c,    CH_PAD_TV, 0);
    lv_obj_set_style_pad_bottom(c, CH_PAD_TV, 0);

    // Series: líneas de 2px, sin puntos
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(c,  0, LV_PART_INDICATOR);
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// ── Inicialización ────────────────────────────────────────────────────────
void chart_screen_init(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // ── Barra de navegación ───────────────────────────────────────────────
    s_btn_prev = nav_btn(parent, 2,   2, 44, NAV_H - 4, LV_SYMBOL_LEFT,  prev_cb);
    s_btn_next = nav_btn(parent, 434, 2, 44, NAV_H - 4, LV_SYMBOL_RIGHT, next_cb);

    s_lbl_date = lv_label_create(parent);
    lv_obj_set_pos(s_lbl_date, 48, 7); lv_obj_set_width(s_lbl_date, 384);
    lv_obj_set_style_text_align(s_lbl_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_date,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_date, C_WHITE, 0);
    lv_label_set_text(s_lbl_date, "---");

    // ── Chart de potencias ────────────────────────────────────────────────
    s_chart_pwr = make_chart(parent, PWR_Y, PWR_H, -6000, 6000, 5);

    // Series (la última en añadirse dibuja encima)
    s_sload = lv_chart_add_series(s_chart_pwr, C_LOAD, LV_CHART_AXIS_PRIMARY_Y);
    s_sbatt = lv_chart_add_series(s_chart_pwr, C_BATT, LV_CHART_AXIS_PRIMARY_Y);
    s_sgrid = lv_chart_add_series(s_chart_pwr, C_GRID, LV_CHART_AXIS_PRIMARY_Y);
    s_spv   = lv_chart_add_series(s_chart_pwr, C_PV,   LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_add_flag(s_chart_pwr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chart_pwr, chart_click_cb, LV_EVENT_CLICKED, nullptr);

    // ── Leyenda ───────────────────────────────────────────────────────────
    legend_dot(parent,   5, LEG_Y, C_PV,   "PV");
    legend_dot(parent,  65, LEG_Y, C_GRID, "Red");
    legend_dot(parent, 120, LEG_Y, C_BATT, "Bat");
    legend_dot(parent, 170, LEG_Y, C_LOAD, "Carga");
    legend_dot(parent, 240, LEG_Y, C_SOC,  "SOC");

    lv_obj_t* u = lv_label_create(parent);
    lv_obj_set_pos(u, 390, LEG_Y + 1); lv_obj_set_style_text_color(u, C_MUTED, 0);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_12, 0);
    lv_label_set_text(u, "kWh/h | %");

    // ── Chart de SOC ──────────────────────────────────────────────────────
    s_chart_soc = make_chart(parent, SOC_Y, SOC_H, 0, 100, 2);
    s_ssoc = lv_chart_add_series(s_chart_soc, C_SOC, LV_CHART_AXIS_PRIMARY_Y);

    // ── Etiquetas eje X ───────────────────────────────────────────────────
    // content_w = 480 - CH_PAD_L - CH_PAD_R = 438
    // px(h) = CH_PAD_L + round(h * 438 / 23)
    const int cw = 480 - CH_PAD_L - CH_PAD_R;
    const struct { int h; const char* s; } xlabs[] = {
        {0,"00"}, {6,"06"}, {12,"12"}, {18,"18"}, {21,"21"}
    };
    for (auto& xl : xlabs) {
        int px = CH_PAD_L + (int)(xl.h * cw / 23.0f + 0.5f) - 8;
        lv_obj_t* l = lv_label_create(parent);
        lv_obj_set_pos(l, px, XLAB_Y);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, C_MUTED, 0);
        lv_label_set_text(l, xl.s);
    }

    // ── "Sin datos" ───────────────────────────────────────────────────────
    s_no_data = lv_label_create(parent);
    lv_obj_set_pos(s_no_data, 0, PWR_Y + PWR_H / 2 - 8);
    lv_obj_set_width(s_no_data, 480);
    lv_obj_set_style_text_align(s_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_no_data, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_no_data, C_MUTED, 0);
    lv_label_set_text(s_no_data, "Sin datos para este dia");

    // ── Popup de detalle ──────────────────────────────────────────────────
    s_popup = lv_obj_create(parent);
    lv_obj_set_pos(s_popup, CH_PAD_L + 2, PWR_Y + 4);
    lv_obj_set_size(s_popup, 158, 116);
    lv_obj_set_style_bg_color(s_popup, lv_color_hex(0x1C2128), 0);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_popup, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_popup, 1, 0);
    lv_obj_set_style_radius(s_popup, 6, 0);
    lv_obj_set_style_pad_all(s_popup, 7, 0);
    lv_obj_set_scrollbar_mode(s_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_popup, popup_click_cb, LV_EVENT_CLICKED, nullptr);

    s_popup_lbl = lv_label_create(s_popup);
    lv_obj_set_pos(s_popup_lbl, 0, 0);
    lv_obj_set_style_text_font(s_popup_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_popup_lbl, C_WHITE, 0);
    lv_label_set_text(s_popup_lbl, "");
}

// ── API pública ───────────────────────────────────────────────────────────
void chart_screen_set_active(bool active) {
    s_active = active;
    if (active) { load_day(); s_last_tick = millis(); }
}

void chart_screen_tick() {
    if (!s_active || s_offset != 0) return;
    if (millis() - s_last_tick >= 300000UL) {   // refrescar hoy cada 5 min
        load_day();
        s_last_tick = millis();
    }
}
