#include "dashboard.h"
#include "ui_constants.h"
#include "config.h"

// ── Paleta ────────────────────────────────────────────────────────────────
#define C_BG        lv_color_hex(0x0D1117)
#define C_CARD      lv_color_hex(0x161B22)
#define C_SOLAR     lv_color_hex(0xF5C518)
#define C_GRID      lv_color_hex(0x4A9EFF)
#define C_BATT_OK   lv_color_hex(0x2ECC71)
#define C_BATT_DIS  lv_color_hex(0xE74C3C)
#define C_LOAD      lv_color_hex(0xBB6BD9)
#define C_WHITE     lv_color_hex(0xEAEAEA)
#define C_MUTED     lv_color_hex(0x6E7681)
#define C_BAR_BG    lv_color_hex(0x21262D)

// ── Punteros a widgets que se actualizan ──────────────────────────────────
static lv_obj_t *lbl_solar_val, *lbl_solar_sub;
static lv_obj_t *lbl_grid_val,  *lbl_grid_sub;
static lv_obj_t *lbl_batt_soc,  *lbl_batt_pwr, *lbl_batt_sub, *bar_batt;
static lv_obj_t *lbl_load_val;
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_selfcon;
static lv_obj_t *bar_selfcon;

// ── Helper: crea una tarjeta con título ───────────────────────────────────
static lv_obj_t* make_card(lv_obj_t* parent, int x, int y,
                            const char* title, lv_color_t accent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, DASH_CARD_W, DASH_CARD_H);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, accent, 0);
    lv_obj_set_style_border_width(card, UI_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_radius(card, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(card, UI_PAD, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(card);
    lv_obj_set_pos(lbl, 0, 0);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, &FONT_NORMAL, 0);
    lv_label_set_text(lbl, title);
    return card;
}

// ── Helper: crea un label estándar en coordenadas de content area ─────────
static lv_obj_t* make_label(lv_obj_t* parent, int x, int y,
                             const lv_font_t* font, lv_color_t color,
                             const char* text)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

// ── Inicialización del dashboard ──────────────────────────────────────────
void dashboard_init(lv_obj_t* parent) {
    lv_obj_t* scr = parent;
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ── Barra superior ────────────────────────────────────────────────────
    lv_obj_t* topbar = lv_obj_create(scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, SCREEN_WIDTH, TOP_BAR_H);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(0x0A0F14), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_hor(topbar, UI_MARGIN, 0);
    lv_obj_set_style_pad_ver(topbar, SY(3), 0);
    lv_obj_remove_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_clock = lv_label_create(topbar);
    lv_obj_set_pos(lbl_clock, 0, 0);
    lv_obj_set_style_text_font(lbl_clock, &FONT_NORMAL, 0);
    lv_obj_set_style_text_color(lbl_clock, C_MUTED, 0);
    lv_label_set_text(lbl_clock, "--:--:--");

    lbl_selfcon = lv_label_create(topbar);
    lv_obj_set_style_text_font(lbl_selfcon, &FONT_NORMAL, 0);
    lv_obj_set_style_text_color(lbl_selfcon, C_WHITE, 0);
    lv_label_set_text(lbl_selfcon, "Autoconsumo --%");
    lv_obj_align(lbl_selfcon, LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Posiciones de tarjetas ────────────────────────────────────────────
    int x0 = UI_MARGIN;
    int x1 = UI_MARGIN + DASH_CARD_W + UI_GAP;
    int y0 = TOP_BAR_H + UI_MARGIN;
    int y1 = TOP_BAR_H + UI_MARGIN + DASH_CARD_H + UI_GAP;

    // Posiciones internas relativas al tamaño de tarjeta
    int val_y  = SY(20);
    int sub_y  = DASH_CARD_H - UI_PAD*2 - FONT_SMALL_SIZE - SY(4);
    int soc_y  = SY(16);
    int bar_y  = soc_y + FONT_LARGE_SIZE + SY(6);
    int pwr_y  = bar_y + SS(12) + SY(4);
    int bsub_y = pwr_y + FONT_NORMAL_SIZE + SY(2);

    // ── Tarjeta SOLAR ─────────────────────────────────────────────────────
    lv_obj_t* c_solar = make_card(scr, x0, y0, LV_SYMBOL_CHARGE " SOLAR", C_SOLAR);
    lbl_solar_val = make_label(c_solar, 0, val_y, &FONT_LARGE, C_WHITE, "--- W");
    lbl_solar_sub = make_label(c_solar, 0, sub_y, &FONT_SMALL, C_MUTED,
                               "PV1: --- W   PV2: --- W");

    // ── Tarjeta RED ───────────────────────────────────────────────────────
    lv_obj_t* c_grid = make_card(scr, x1, y0, LV_SYMBOL_WIFI " RED", C_GRID);
    lbl_grid_val = make_label(c_grid, 0, val_y, &FONT_LARGE, C_WHITE, "--- W");
    lbl_grid_sub = make_label(c_grid, 0, sub_y, &FONT_SMALL, C_MUTED, "--");

    // ── Tarjeta BATERÍA ───────────────────────────────────────────────────
    lv_obj_t* c_batt = make_card(scr, x0, y1,
                                  LV_SYMBOL_BATTERY_FULL " BATERIA", C_BATT_OK);
    lbl_batt_soc = make_label(c_batt, 0, soc_y, &FONT_LARGE, C_WHITE, "--%");

    bar_batt = lv_bar_create(c_batt);
    lv_obj_set_pos(bar_batt, 0, bar_y);
    lv_obj_set_size(bar_batt, DASH_CARD_W - UI_PAD*2, SS(12));
    lv_bar_set_range(bar_batt, 0, 100);
    lv_bar_set_value(bar_batt, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_batt, C_BAR_BG,  LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_batt, C_BATT_OK, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_batt, SS(6), LV_PART_MAIN);
    lv_obj_set_style_radius(bar_batt, SS(6), LV_PART_INDICATOR);

    lbl_batt_pwr = make_label(c_batt, 0, pwr_y,  &FONT_NORMAL, C_WHITE, "--- W");
    lbl_batt_sub = make_label(c_batt, 0, bsub_y, &FONT_SMALL,  C_MUTED, "--");

    // ── Tarjeta CARGA ─────────────────────────────────────────────────────
    lv_obj_t* c_load = make_card(scr, x1, y1, LV_SYMBOL_HOME " CARGA", C_LOAD);
    make_label(c_load, 0, soc_y, &FONT_SMALL, C_MUTED, "Consumo actual");
    lbl_load_val = make_label(c_load, 0, soc_y + FONT_SMALL_SIZE + SY(6),
                               &FONT_LARGE, C_WHITE, "--- W");
}

// ── Actualización de datos ────────────────────────────────────────────────
void dashboard_update(const EnergyData& d) {
    if (!d.valid) return;

    // Solar
    lv_label_set_text_fmt(lbl_solar_val, "%d W", (int)d.pv_power);
    lv_label_set_text_fmt(lbl_solar_sub, "PV1: %d W   PV2: %d W",
                          (int)d.pv1_power, (int)d.pv2_power);

    // Red
    if (d.grid_power > 0) {
        lv_label_set_text_fmt(lbl_grid_val, "+%d W", (int)d.grid_power);
        lv_label_set_text(lbl_grid_sub, "Importando de la red");
    } else if (d.grid_power < 0) {
        lv_label_set_text_fmt(lbl_grid_val, "%d W", (int)d.grid_power);
        lv_label_set_text(lbl_grid_sub, "Exportando a la red");
    } else {
        lv_label_set_text(lbl_grid_val, "0 W");
        lv_label_set_text(lbl_grid_sub, "Sin intercambio");
    }

    // Batería
    lv_color_t batt_color = (d.batt_power <= 0) ? C_BATT_OK : C_BATT_DIS;
    lv_obj_set_style_bg_color(bar_batt, batt_color, LV_PART_INDICATOR);
    lv_bar_set_value(bar_batt, (int32_t)d.batt_soc, LV_ANIM_ON);
    lv_label_set_text_fmt(lbl_batt_soc, "%d%%", (int)d.batt_soc);

    if (d.batt_power < 0) {                                    // negativo = cargando
        lv_label_set_text_fmt(lbl_batt_pwr, "%d W", (int)d.batt_power);
        lv_label_set_text(lbl_batt_sub, "Cargando");
    } else if (d.batt_power > 0) {                             // positivo = descargando
        lv_label_set_text_fmt(lbl_batt_pwr, "+%d W", (int)d.batt_power);
        lv_label_set_text(lbl_batt_sub, "Descargando");
    } else {
        lv_label_set_text(lbl_batt_pwr, "0 W");
        lv_label_set_text(lbl_batt_sub, "En reposo");
    }


    // Carga
    lv_label_set_text_fmt(lbl_load_val, "%d W", (int)d.load_power);

    // ── Autoconsumo instantáneo ───────────────────────────────────────────
    // Energía que no sale a la red = PV + min(0, grid) = PV - exportado
    // Autoconsumo% = (PV - max(0, -grid)) / PV × 100
    if (d.pv_power > 10) {   // umbral mínimo para evitar divisiones con ruido
        int32_t export_w  = (d.grid_power < 0) ? -d.grid_power : 0;
        int32_t local_use = d.pv_power - export_w;
        if (local_use < 0) local_use = 0;
        int pct = (int)(local_use * 100 / d.pv_power);
        if (pct > 100) pct = 100;

        lv_color_t col = (pct >= 80) ? C_BATT_OK :
                         (pct >= 50) ? lv_color_hex(0xF5C518) : C_BATT_DIS;
        lv_label_set_text_fmt(lbl_selfcon, "Autoconsumo %d%%", pct);
        lv_obj_set_style_text_color(lbl_selfcon, col, 0);
    } else {
        // Sin producción solar → no tiene sentido mostrar el ratio
        lv_label_set_text(lbl_selfcon, "Sin produccion");
        lv_obj_set_style_text_color(lbl_selfcon, C_MUTED, 0);
    }
}

void dashboard_tick() {
    static uint32_t s_last = 0;
    if (millis() - s_last < 1000) return;
    s_last = millis();

    struct tm ti;
    if (!getLocalTime(&ti, 0)) return;   // sin bloqueo

    static const char* MESES[] = {
        "Ene","Feb","Mar","Abr","May","Jun",
        "Jul","Ago","Sep","Oct","Nov","Dic"
    };
    char buf[32];
    snprintf(buf, sizeof(buf), "%d %s  %02d:%02d:%02d",
             ti.tm_mday, MESES[ti.tm_mon],
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    lv_label_set_text(lbl_clock, buf);
}
