#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>
#include <LittleFS.h>

#include "config.h"
#include "storage.h"
#include "data_store.h"
#include "solarman.h"
#include "dashboard.h"
#include "stats_screen.h"
#include "chart_screen.h"
#include "config_screen.h"
#include "web_server.h"
#include "telegram.h"
#include "psram_alloc.h"
#include "psram_cache.h"
#include "backlight.h"


/* Change to your screen resolution */
static uint32_t screenWidth = 480;
static uint32_t screenHeight = 272;

#include <Arduino_GFX_Library.h>

#define GFX_BL DF_GFX_BL // default backlight pin, you may replace DF_GFX_BL to actual backlight pin
#define TFT2_BL 2

Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* DCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
    0 /*hsync_polarity*/, 1 /* hsync_front_porch*/, 4 /* hsync_pulse_width*/, 43 /* hsync_back_porch*/,
    0 /*vsync_polarity*/, 3 /*vsync_front_porch*/, 4 /*vsync_pulse_width*/, 12 /*vsync_back_porch*/,
    1 /*pclk_active_neg*/, 9000000 /*prefer_speed*/, false /*useBigEndian*/,
    0 /*de_idle_high*/, 0 /*pclk_idle_high*/
);

// Original: 8 /* hsync_front_porch*/ y 8 /*vsync_front_porch*/
Arduino_GFX *gfx = new Arduino_RGB_Display(screenWidth, screenHeight, panel);

/*******************************************************************************
 * End of Arduino_GFX setting
 ******************************************************************************/

#include "touch.h"

static lv_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
static lv_display_t *disp_drv;
static unsigned long last_ms;

/* Display flushing */
void my_disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*) px_map, w, h);

    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    if (touch_has_signal())
    {
        if (touch_touched())
        {
            data->state = LV_INDEV_STATE_PR;

            data->point.x = touch_last_x;
            data->point.y = touch_last_y;

            //Mostrar coordenadas por consola
            //Serial0.printf("%d, %d\n", data->point.x, data->point.y);
        }
        else if (touch_released())
        {
            data->state = LV_INDEV_STATE_REL;
        }
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

uint32_t esp_tick_get_cb() {
    return esp_timer_get_time() / 1000;
}

static EnergyData        g_energy;
static DailyStats        g_daily;
static SemaphoreHandle_t g_mutex;
static volatile bool     g_energy_ready = false;
static volatile bool     g_daily_ready  = false;
static lv_obj_t*         g_tile_chart = nullptr;
static lv_obj_t*         g_tile_stats = nullptr;
static lv_obj_t*         g_tile_summary = nullptr;

// Config en RAM cargada desde NVS al arrancar
static AppConfig g_cfg;

// Función delta con protección de rollover de medianoche
static int16_t delta_wh(float cur, float prev) {
    float d = cur - prev;
    if (d < 0.0f) d = cur;           // reset de medianoche
    int v = (int)(d * 1000.0f + 0.5f);
    return (int16_t)(v > 32000 ? 32000 : v);
}

// ── Tarea de red (Core 0) ─────────────────────────────────────────────────
static void solarmanTask(void* /*pv*/) {
    // Usamos g_cfg que ya fue rellenado en setup() desde NVS
    SolarmanClient client(g_cfg.logger_ip, LOGGER_PORT,
                          g_cfg.logger_serial, MODBUS_UNIT_ID);
    EnergyData  local_e;
    DailyStats  local_d;
    uint32_t    last_daily = millis() - POLL_DAILY_MS + POLL_INTERVAL_MS;

    // ── Estado de agregación ──────────────────────────────────────────────────
    static int32_t   s_cur_5min_slot = -1;   // slot actual (epoch/300)
    static int32_t   s_cur_hour      = -1;   // hora actual
    static int32_t   s_cur_day       = -1;   // día del mes actual
    static bool      s_startup_done  = false;

    // Acumuladores para la hora en curso
    static float     s_acc_pv   = 0, s_acc_grid = 0;
    static float     s_acc_batt = 0, s_acc_load = 0;
    static int       s_acc_n    = 0;
    static uint8_t   s_acc_soc  = 0;

    // Snapshot de inicio de hora (para calcular energía del periodo)
    static uint16_t  s_snap_day_pv = 0, s_snap_day_exp = 0;
    static uint16_t  s_snap_day_imp = 0, s_snap_day_load = 0;
    static uint16_t  s_snap_day_bchg = 0, s_snap_day_bdis = 0;
    static bool      s_snap_valid = false;

    // SOC al inicio del día
    static uint8_t   s_soc_start_of_day = 0;
    static bool      s_soc_start_set    = false;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial0.println("[WiFi] Reconectando...");
            WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
            uint32_t t = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t < 15000)
                vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (WiFi.status() == WL_CONNECTED) {
            if (client.fetchEnergyData(local_e)) {
                Serial0.printf("[Live] PV:%dW Grid:%dW Bat:%dW(%d%%) Load:%dW\n",
                    (int)local_e.pv_power, (int)local_e.grid_power,
                    (int)local_e.batt_power, (int)local_e.batt_soc,
                    (int)local_e.load_power);
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_energy       = local_e;
                    g_energy_ready = true;
                    xSemaphoreGive(g_mutex);
                }
            }
        

            // ── Alineación a intervalos de 5 minutos ─────────────────────────────────
            uint32_t now_ep    = (uint32_t)time(nullptr);
            uint32_t slot_5min = now_ep / 300;           // slot actual (cambia cada 5 min)

            // Esperar NTP válido
            if (now_ep < 1700000000UL) goto end_record;

            // ── Primer arranque: alinear al próximo slot ──────────────────────────────
            if (!s_startup_done && local_e.valid && local_d.valid) {
                s_startup_done = true;
                s_cur_5min_slot = (int32_t)slot_5min;   // empezar desde el slot actual

                struct tm now_tm; getLocalTime(&now_tm, 500);
                s_cur_hour = now_tm.tm_hour;
                s_cur_day  = now_tm.tm_mday;

                // Inicializar snapshot de acumulados
                s_snap_day_pv   = (uint16_t)(local_d.pv_kwh          * 10.0f + 0.5f);
                s_snap_day_exp  = (uint16_t)(local_d.export_kwh       * 10.0f + 0.5f);
                s_snap_day_imp  = (uint16_t)(local_d.import_kwh       * 10.0f + 0.5f);
                s_snap_day_load = (uint16_t)(local_d.load_kwh         * 10.0f + 0.5f);
                s_snap_day_bchg = (uint16_t)(local_d.batt_charge_kwh  * 10.0f + 0.5f);
                s_snap_day_bdis = (uint16_t)(local_d.batt_discharge_kwh*10.0f + 0.5f);
                s_snap_valid    = true;

                s_soc_start_of_day = (uint8_t)local_e.batt_soc;
                s_soc_start_set    = true;

                Serial.printf("[Record] Startup: slot=%lu hora=%d dia=%d\n",
                            (unsigned long)slot_5min, s_cur_hour, s_cur_day);
                goto end_record;
            }

            if (!s_startup_done) goto end_record;
            if (!local_e.valid || !local_d.valid) goto end_record;

            {
                struct tm now_tm; getLocalTime(&now_tm, 500);
                int new_hour = now_tm.tm_hour;
                int new_day  = now_tm.tm_mday;

                // Acumular para la media de la hora en curso
                s_acc_pv   += local_e.pv_power;
                s_acc_grid += local_e.grid_power;
                s_acc_batt += local_e.batt_power;
                s_acc_load += local_e.load_power;
                s_acc_soc   = (uint8_t)local_e.batt_soc;
                s_acc_n++;

                // ── Nuevo slot de 5 minutos (grabar Record5Min en el límite exacto) ───
                if ((int32_t)slot_5min != s_cur_5min_slot) {
                    s_cur_5min_slot = (int32_t)slot_5min;

                    // El timestamp del registro es el inicio del slot completado
                    uint32_t record_ts = slot_5min * 300;   // inicio del slot actual

                    Record5Min r{};
                    r.timestamp  = record_ts;
                    r.pv_w       = (int16_t)constrain(local_e.pv_power,   -32767, 32767);
                    r.grid_w     = (int16_t)constrain(local_e.grid_power,  -32767, 32767);
                    r.batt_w     = (int16_t)constrain(local_e.batt_power,  -32767, 32767);
                    r.load_w     = (int16_t)constrain(local_e.load_power,  -32767, 32767);
                    r.day_pv     = (uint16_t)(local_d.pv_kwh          * 10.0f + 0.5f);
                    r.day_export = (uint16_t)(local_d.export_kwh       * 10.0f + 0.5f);
                    r.day_import = (uint16_t)(local_d.import_kwh       * 10.0f + 0.5f);
                    r.day_load   = (uint16_t)(local_d.load_kwh         * 10.0f + 0.5f);
                    r.day_bchg   = (uint16_t)(local_d.batt_charge_kwh  * 10.0f + 0.5f);
                    r.day_bdis   = (uint16_t)(local_d.batt_discharge_kwh*10.0f + 0.5f);
                    r.soc        = (uint8_t)local_e.batt_soc;
                    r.flags      = 0x01;
                    r.extra[0]   = r.extra[1] = 0;

                    Store.push(r);
                    Cache.pushRaw(r);

                    // Guardar sesión para detección de gaps
                    SessionState ss{record_ts, true};
                    Storage.saveSessionState(ss);

                    // Actualizar snapshot de acumulados si ha pasado la hora
                    // (el snapshot se actualiza al cerrar la hora, no aquí)
                }

                // ── Cambio de hora: finalizar HourlyRecord ────────────────────────────
                if (new_hour != s_cur_hour) {
                    if (s_acc_n > 0 && s_snap_valid) {
                        // Medianoche del día actual
                        struct tm mid = now_tm;
                        // Si new_hour == 0, la hora que cierra es la 23 de ayer
                        if (new_hour == 0) mid.tm_mday--;   // mktime normaliza
                        mid.tm_hour = s_cur_hour;
                        mid.tm_min = 0; mid.tm_sec = 0; mid.tm_isdst = -1;
                        uint32_t hour_ep = (uint32_t)mktime(&mid);

                        // Acumulados al final de la hora (último valor leído)
                        uint16_t cur_pv   = (uint16_t)(local_d.pv_kwh          * 10.0f + 0.5f);
                        uint16_t cur_exp  = (uint16_t)(local_d.export_kwh       * 10.0f + 0.5f);
                        uint16_t cur_imp  = (uint16_t)(local_d.import_kwh       * 10.0f + 0.5f);
                        uint16_t cur_load = (uint16_t)(local_d.load_kwh         * 10.0f + 0.5f);
                        uint16_t cur_bchg = (uint16_t)(local_d.batt_charge_kwh  * 10.0f + 0.5f);
                        uint16_t cur_bdis = (uint16_t)(local_d.batt_discharge_kwh*10.0f + 0.5f);

                        HourlyRecord hr{};
                        hr.hour_epoch   = hour_ep;
                        hr.avg_pv_w     = (int16_t)(s_acc_pv   / s_acc_n);
                        hr.avg_grid_w   = (int16_t)(s_acc_grid  / s_acc_n);
                        hr.avg_batt_w   = (int16_t)(s_acc_batt  / s_acc_n);
                        hr.avg_load_w   = (int16_t)(s_acc_load  / s_acc_n);
                        hr.soc_end       = s_acc_soc;
                        hr.sample_count  = (uint8_t)min(s_acc_n, 255);
                        hr.flags         = 0x01;
                        // Acumulados diarios al final de la hora
                        hr.day_pv     = cur_pv;
                        hr.day_export = cur_exp;
                        hr.day_import = cur_imp;
                        hr.day_load   = cur_load;
                        hr.day_bchg   = cur_bchg;
                        hr.day_bdis   = cur_bdis;

                        Cache.pushHourly(hr);   // guarda en flash + PSRAM

                        // Nuevo snapshot para la siguiente hora
                        s_snap_day_pv   = cur_pv;
                        s_snap_day_exp  = cur_exp;
                        s_snap_day_imp  = cur_imp;
                        s_snap_day_load = cur_load;
                        s_snap_day_bchg = cur_bchg;
                        s_snap_day_bdis = cur_bdis;
                    }

                    // Resetear acumuladores
                    s_acc_pv = s_acc_grid = s_acc_batt = s_acc_load = 0;
                    s_acc_n  = 0;
                    s_cur_hour = new_hour;
                }

                // ── Cambio de día: finalizar DailyRecord ──────────────────────────────
                if (new_day != s_cur_day) {
                    // Medianoche del día que acaba de terminar
                    struct tm yesterday = now_tm;
                    yesterday.tm_mday--;
                    yesterday.tm_hour = 0; yesterday.tm_min = 0;
                    yesterday.tm_sec  = 0; yesterday.tm_isdst = -1;
                    uint32_t dep_yesterday = (uint32_t)mktime(&yesterday);

                    // Los registros del inversor se resetean a medianoche
                    // El último valor antes del reset es el total del día
                    HourlyRecord last_hr{};
                    if (Store.getLastHourly(dep_yesterday, last_hr)) {
                        DailyRecord dr{};
                        dr.day_epoch   = dep_yesterday;
                        dr.pv_10wh     = last_hr.day_pv;
                        dr.export_10wh = last_hr.day_export;
                        dr.import_10wh = last_hr.day_import;
                        dr.load_10wh   = last_hr.day_load;
                        dr.bchg_10wh   = last_hr.day_bchg;
                        dr.bdis_10wh   = last_hr.day_bdis;
                        dr.soc_start   = s_soc_start_of_day;
                        dr.soc_end     = last_hr.soc_end;
                        dr.flags       = 0x03;   // válido + completo
                        Cache.pushDaily(dr);
                    }

                    // Nuevo día
                    s_soc_start_of_day = (uint8_t)local_e.batt_soc;
                    s_soc_start_set    = true;
                    s_cur_day          = new_day;

                    // Actualizar DailyRecord del día actual también (parcial)
                    {
                        struct tm today_tm = now_tm;
                        today_tm.tm_hour = 0; today_tm.tm_min = 0;
                        today_tm.tm_sec  = 0; today_tm.tm_isdst = -1;
                        uint32_t dep_today = (uint32_t)mktime(&today_tm);

                        DailyRecord dr_today{};
                        dr_today.day_epoch   = dep_today;
                        dr_today.pv_10wh     = (uint16_t)(local_d.pv_kwh          * 10.0f + 0.5f);
                        dr_today.export_10wh = (uint16_t)(local_d.export_kwh       * 10.0f + 0.5f);
                        dr_today.import_10wh = (uint16_t)(local_d.import_kwh       * 10.0f + 0.5f);
                        dr_today.load_10wh   = (uint16_t)(local_d.load_kwh         * 10.0f + 0.5f);
                        dr_today.bchg_10wh   = (uint16_t)(local_d.batt_charge_kwh  * 10.0f + 0.5f);
                        dr_today.bdis_10wh   = (uint16_t)(local_d.batt_discharge_kwh*10.0f + 0.5f);
                        dr_today.soc_start   = s_soc_start_of_day;
                        dr_today.soc_end     = (uint8_t)local_e.batt_soc;
                        dr_today.flags       = 0x01;   // válido, no completo
                        Cache.pushDaily(dr_today);
                    }
                } else {
                    // Mismo día: actualizar DailyRecord parcial cada hora
                    if (new_hour != s_cur_hour) {
                        struct tm today_tm = now_tm;
                        today_tm.tm_hour = 0; today_tm.tm_min = 0;
                        today_tm.tm_sec  = 0; today_tm.tm_isdst = -1;
                        uint32_t dep_today = (uint32_t)mktime(&today_tm);

                        DailyRecord dr_today{};
                        dr_today.day_epoch   = dep_today;
                        dr_today.pv_10wh     = (uint16_t)(local_d.pv_kwh          * 10.0f + 0.5f);
                        dr_today.export_10wh = (uint16_t)(local_d.export_kwh       * 10.0f + 0.5f);
                        dr_today.import_10wh = (uint16_t)(local_d.import_kwh       * 10.0f + 0.5f);
                        dr_today.load_10wh   = (uint16_t)(local_d.load_kwh         * 10.0f + 0.5f);
                        dr_today.bchg_10wh   = (uint16_t)(local_d.batt_charge_kwh  * 10.0f + 0.5f);
                        dr_today.bdis_10wh   = (uint16_t)(local_d.batt_discharge_kwh*10.0f + 0.5f);
                        dr_today.soc_start   = s_soc_start_of_day;
                        dr_today.soc_end     = (uint8_t)local_e.batt_soc;
                        dr_today.flags       = 0x01;
                        Cache.pushDaily(dr_today);
                    }
                }
            }

            end_record:;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial0.begin(115200);
    
    // ── Tu código de display/touch aquí (ya configurado) ─────────────────
    touch_init();

    // Init Display
    gfx->begin();

    Backlight.begin(TFT2_BL);

    lv_init();

    screenWidth = gfx->width();
    screenHeight = gfx->height();
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(2 * screenWidth * screenHeight / 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disp_draw_buf)
    {
        Serial0.println("LVGL disp_draw_buf allocate failed!");
        while (true)
            delay(1);
    }

    /* Initialize the display */
    disp_drv = lv_display_create(screenWidth, screenHeight);
    lv_display_set_flush_cb(disp_drv, my_disp_flush);
    /* Change the following line to your display resolution */
    lv_display_set_buffers(disp_drv, disp_draw_buf, NULL, 2 * screenWidth * screenHeight / 4, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Initialize the (dummy) input device driver */
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);
    lv_tick_set_cb(esp_tick_get_cb);

    // ─────────────────────────────────────────────────────────────────────

    // NVS → config en RAM
    Storage.loadConfig(g_cfg);

    // LittleFS + DataStore + Caché
    Store.begin();
    
    // PSRAM disponible
    Serial.printf("[Setup] PSRAM total: %lu KB, libre: %lu KB\n",
                  (unsigned long)ESP.getPsramSize()  / 1024,
                  (unsigned long)ESP.getFreePsram()  / 1024);
    print_mem_stats("antes de cache");

    // Cache PSRAM (carga los últimos 7 días desde LittleFS)
    Cache.begin();

    print_mem_stats("después de cache");

    // WiFi con valores de NVS
    WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
    Serial0.print("[WiFi] Conectando");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(500); Serial0.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial0.printf("\n[WiFi] IP: ");
        Serial0.println(WiFi.localIP().toString());
    } else {
        Serial0.println("\n[WiFi] Sin conexion – se reintentará en la tarea");
    }

    // NTP
    configTime(0, 0, "es.pool.ntp.org", "time.cloudflare.com");
    setenv("TZ", TIMEZONE, 1);
    tzset();
    Serial0.print("[NTP] Sincronizando");
    struct tm ntp_t;
    for (int i = 0; i < 20 && !getLocalTime(&ntp_t, 500); i++) Serial0.print('.');
    Serial0.println(getLocalTime(&ntp_t, 0) ? " OK" : " TIMEOUT");

    // Telegram
    TelegramConfig tgcfg = Storage.loadTelegramConfig();
    Telegram.begin(tgcfg.token, tgcfg.chat_id);

    // ── Tileview: 4 pantallas horizontales ───────────────────────────────
    lv_obj_t* tv = lv_tileview_create(lv_scr_act());
    lv_obj_set_size(tv, 480, 272);
    lv_obj_set_pos(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x0D1117), 0);

    lv_obj_t* tile_dash    = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    g_tile_stats           = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
    g_tile_chart           = lv_tileview_add_tile(tv, 2, 0, LV_DIR_HOR);
    lv_obj_t* tile_config  = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT);

    dashboard_init(tile_dash);
    stats_screen_init(g_tile_stats);
    chart_screen_init(g_tile_chart);
    config_screen_init(tile_config);

    // Evento de cambio de tile → activa/desactiva refresco del chart
    lv_obj_add_event_cb(tv, [](lv_event_t* e) {
        lv_obj_t* tile = lv_tileview_get_tile_active(lv_event_get_target_obj(e));
        chart_screen_set_active(tile == g_tile_chart);
        stats_screen_set_active(tile == g_tile_stats);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Mutex + tarea de red en Core 0
    g_mutex = xSemaphoreCreateMutex();

    webserver_set_data(g_mutex, &g_energy, &g_daily);
    webserver_begin();   // crea su propia tarea en Core 0

    xTaskCreatePinnedToCore(solarmanTask, "solarman",
                             8192, nullptr, 1, nullptr, 0);
}

// ── Loop (Core 1) ─────────────────────────────────────────────────────────
void loop() {
    lv_timer_handler();

    // ── Detectar toque para resetear inactividad ──────────────────────────
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev)  == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            Backlight.onTouch();
            break;
        }
        indev = lv_indev_get_next(indev);
    }
    Backlight.tick();

    // Resto del loop igual
    if (xSemaphoreTake(g_mutex, 0) == pdTRUE) {
        if (g_energy_ready) { dashboard_update(g_energy); g_energy_ready = false; }
        if (g_daily_ready)  {
            stats_screen_update(g_daily);
            g_daily_ready = false;
        }
        xSemaphoreGive(g_mutex);
    }

    dashboard_tick();
    chart_screen_tick();
    config_screen_tick();

    delay(5);
}