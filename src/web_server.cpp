#include "web_server.h"
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>

static WebServer          server(80);
static SemaphoreHandle_t  s_mutex   = nullptr;
static const EnergyData*  s_energy  = nullptr;
static const DailyStats*  s_daily   = nullptr;

void webserver_set_data(SemaphoreHandle_t mutex,
                        const EnergyData* energy,
                        const DailyStats* daily) {
    s_mutex  = mutex;
    s_energy = energy;
    s_daily  = daily;
}

// ═════════════════════════════════════════════════════════════════════════
// Ruta GET /   →  Dashboard HTML con auto-refresco
// ═════════════════════════════════════════════════════════════════════════
static void handle_root() {
    // Copia segura de los datos
    EnergyData e{};
    DailyStats d{};
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_energy) e = *s_energy;
        if (s_daily)  d = *s_daily;
        xSemaphoreGive(s_mutex);
    }

    // ── Helpers de formato ────────────────────────────────────────────────
    auto signedW = [](int32_t v) -> String {
        return (v >= 0 ? "+" : "") + String(v) + " W";
    };

    // ── HTML minimalista, oscuro, responsive ──────────────────────────────
    String html;
    html.reserve(4096);
    html += R"(<!DOCTYPE html><html lang="es"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="10">
<title>Deye Monitor</title>
<style>
  :root{--bg:#0d1117;--card:#161b22;--accent:#4a9eff;--ok:#2ecc71;
        --warn:#f5c518;--err:#e74c3c;--muted:#6e7681;--white:#eaeaea}
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--white);font-family:system-ui,sans-serif;padding:16px}
  h1{font-size:1.1rem;color:var(--muted);margin-bottom:16px;text-align:center}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
  .card{background:var(--card);border:1px solid #21262d;border-radius:10px;padding:14px}
  .card h2{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;
           color:var(--muted);margin-bottom:8px}
  .val{font-size:1.6rem;font-weight:700;line-height:1.1}
  .sub{font-size:.75rem;color:var(--muted);margin-top:4px}
  .solar{border-color:#f5c518}.grid-card{border-color:#4a9eff}
  .batt{border-color:#2ecc71}.load{border-color:#bb6bd9}
  .bar-wrap{background:#21262d;border-radius:6px;height:10px;margin:8px 0}
  .bar{height:10px;border-radius:6px;transition:width .5s}
  hr{border:none;border-top:1px solid #21262d;margin:20px 0}
  .daily-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
  .daily-card{background:var(--card);border:1px solid #21262d;
              border-radius:8px;padding:10px;text-align:center}
  .daily-card h3{font-size:.65rem;color:var(--muted);margin-bottom:4px;text-transform:uppercase}
  .daily-card .kwh{font-size:1.2rem;font-weight:700}
  footer{text-align:center;color:var(--muted);font-size:.7rem;margin-top:20px}
  a{color:var(--accent);text-decoration:none}
</style></head><body>
<h1>&#9889; Deye Monitor – )";
    html += WiFi.localIP().toString();
    html += R"(</h1>
<div class="grid">)";

    // Tarjeta Solar
    html += "<div class='card solar'><h2>&#9728; Solar</h2>";
    html += "<div class='val' style='color:#f5c518'>" + String(e.pv_power) + " W</div>";
    html += "<div class='sub'>PV1: " + String(e.pv1_power) + " W &nbsp; PV2: " + String(e.pv2_power) + " W</div></div>";

    // Tarjeta Red
    String grid_color = (e.grid_power > 0) ? "#4a9eff" : "#2ecc71";
    String grid_label = (e.grid_power > 0) ? "Importando" : (e.grid_power < 0) ? "Exportando" : "Sin intercambio";
    html += "<div class='card grid-card'><h2>&#8644; Red</h2>";
    html += "<div class='val' style='color:" + grid_color + "'>" + signedW(e.grid_power) + "</div>";
    html += "<div class='sub'>" + grid_label + "</div></div>";

    // Tarjeta Batería
    String batt_color = (e.batt_power >= 0) ? "#2ecc71" : "#e74c3c";
    String batt_label = (e.batt_power > 0) ? "Cargando" : (e.batt_power < 0) ? "Descargando" : "En reposo";
    html += "<div class='card batt'><h2>&#128267; Bateria</h2>";
    html += "<div class='val' style='color:" + batt_color + "'>" + String(e.batt_soc) + "%</div>";
    html += "<div class='bar-wrap'><div class='bar' style='width:" + String(e.batt_soc) + "%;background:" + batt_color + "'></div></div>";
    html += "<div class='sub'>" + signedW(e.batt_power) + " – " + batt_label + "</div></div>";

    // Tarjeta Carga
    html += "<div class='card load'><h2>&#127968; Carga</h2>";
    html += "<div class='val' style='color:#bb6bd9'>" + String(e.load_power) + " W</div>";
    html += "<div class='sub'>Consumo actual</div></div>";

    html += "</div>";  // .grid

    // ── Estadísticas diarias ──────────────────────────────────────────────
    if (d.valid) {
        html += "<hr><h1 style='margin-bottom:12px'>Hoy</h1><div class='daily-grid'>";

        auto kwh_card = [&](const char* icon, const char* label,
                            float val, const char* color) {
            html += "<div class='daily-card'><h3>";
            html += icon; html += " "; html += label;
            html += "</h3><div class='kwh' style='color:";
            html += color; html += "'>";
            char buf[12]; snprintf(buf, sizeof(buf), "%.2f", val);
            html += buf; html += " kWh</div></div>";
        };

        kwh_card("&#9728;",  "Produccion",  d.pv_kwh,             "#f5c518");
        kwh_card("&#127968;","Consumo",     d.load_kwh,            "#bb6bd9");
        kwh_card("&#8593;",  "Exportado",   d.export_kwh,          "#2ecc71");
        kwh_card("&#8595;",  "Importado",   d.import_kwh,          "#4a9eff");
        kwh_card("&#128267;","Carga bat.",  d.batt_charge_kwh,     "#f5c518");
        kwh_card("&#128267;","Descarga bat.",d.batt_discharge_kwh, "#e74c3c");

        html += "</div>";
    }

    html += R"(<footer>Actualización cada 10 s &nbsp;|&nbsp;
<a href="/update">&#8593; Actualizar firmware</a></footer>
</body></html>)";

    server.send(200, "text/html; charset=utf-8", html);
}

// ═════════════════════════════════════════════════════════════════════════
// Ruta GET /update  →  Formulario de subida OTA
// ═════════════════════════════════════════════════════════════════════════
static void handle_update_get() {
    String html = R"(<!DOCTYPE html><html lang="es"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Update</title>
<style>
  body{background:#0d1117;color:#eaeaea;font-family:system-ui,sans-serif;
       display:flex;flex-direction:column;align-items:center;
       justify-content:center;min-height:100vh;gap:16px}
  h1{color:#4a9eff;font-size:1.2rem}
  form{background:#161b22;border:1px solid #21262d;border-radius:10px;
       padding:28px;display:flex;flex-direction:column;gap:14px;min-width:300px}
  input[type=file]{color:#eaeaea}
  button{background:#1f6feb;color:#fff;border:none;border-radius:6px;
         padding:10px;font-size:1rem;cursor:pointer}
  button:hover{background:#388bfd}
  #progress{width:100%;background:#21262d;border-radius:6px;height:12px;display:none}
  #bar{height:12px;border-radius:6px;background:#2ecc71;width:0%;transition:width .3s}
  #msg{color:#6e7681;font-size:.85rem;text-align:center}
  a{color:#4a9eff;font-size:.8rem}
</style></head><body>
<h1>&#8593; Actualización de Firmware OTA</h1>
<form id="frm" enctype="multipart/form-data">
  <label>Selecciona el fichero <code>.bin</code></label>
  <input type="file" id="fw" accept=".bin" required>
  <div id="progress"><div id="bar"></div></div>
  <div id="msg">Elige un fichero y pulsa Actualizar</div>
  <button type="button" onclick="upload();">&#8593; Actualizar</button>
</form>
<a href="/">&#8592; Volver al dashboard</a>
<script>
function upload(){
  const f=document.getElementById('fw').files[0];
  if(!f){alert('Elige un fichero .bin');return;}
  const fd=new FormData();fd.append('firmware',f);
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/update');
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){
      const p=Math.round(e.loaded/e.total*100);
      document.getElementById('progress').style.display='block';
      document.getElementById('bar').style.width=p+'%';
      document.getElementById('msg').textContent='Subiendo… '+p+'%';
    }
  };
  xhr.onload=()=>{
    if(xhr.status===200){
      document.getElementById('msg').textContent=
        'OK – el dispositivo se reiniciará ahora.';
      document.getElementById('bar').style.background='#2ecc71';
    } else {
      document.getElementById('msg').textContent='Error: '+xhr.responseText;
      document.getElementById('bar').style.background='#e74c3c';
    }
  };
  xhr.onerror=()=>{
    document.getElementById('msg').textContent='Error de red.';
  };
  xhr.send(fd);
}
</script></body></html>)";

    server.send(200, "text/html; charset=utf-8", html);
}

// ═════════════════════════════════════════════════════════════════════════
// Ruta POST /update  →  Recepción del .bin y flasheo
// ═════════════════════════════════════════════════════════════════════════
static void handle_update_post() {
    // La respuesta se envía desde el handler de upload (abajo)
    // Aquí solo reiniciamos si todo fue bien
    if (Update.hasError()) return;
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OK");
    delay(500);
    ESP.restart();
}

static void handle_upload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial0.printf("[OTA] Inicio: %s (%u bytes)\n",
                      upload.filename.c_str(), upload.totalSize);
        // UPDATE_SIZE_UNKNOWN → Update calcula el espacio él solo
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial0.print("[OTA] Error al iniciar: ");
            Update.printError(Serial);
        }

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Serial0.print("[OTA] Error escribiendo: ");
            Update.printError(Serial);
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial0.printf("[OTA] Completado: %u bytes. Reiniciando.\n",
                          upload.totalSize);
        } else {
            Serial0.print("[OTA] Error al finalizar: ");
            Update.printError(Serial);
            // Informar al cliente del error
            server.send(500, "text/plain",
                        String("Error OTA: ") + Update.errorString());
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Ruta GET /api/data  →  JSON para integraciones externas
// ═════════════════════════════════════════════════════════════════════════
static void handle_api_data() {
    EnergyData e{};
    DailyStats d{};
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_energy) e = *s_energy;
        if (s_daily)  d = *s_daily;
        xSemaphoreGive(s_mutex);
    }

    char json[512];
    snprintf(json, sizeof(json),
        R"({"live":{"pv_w":%d,"pv1_w":%d,"pv2_w":%d,)"
        R"("grid_w":%d,"batt_w":%d,"batt_soc":%d,"load_w":%d},)"
        R"("daily":{"pv_kwh":%.2f,"export_kwh":%.2f,"import_kwh":%.2f,)"
        R"("load_kwh":%.2f,"batt_charge_kwh":%.2f,"batt_discharge_kwh":%.2f}})",
        (int)e.pv_power, (int)e.pv1_power, (int)e.pv2_power,
        (int)e.grid_power, (int)e.batt_power, (int)e.batt_soc, (int)e.load_power,
        d.pv_kwh, d.export_kwh, d.import_kwh,
        d.load_kwh, d.batt_charge_kwh, d.batt_discharge_kwh);

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

// ═════════════════════════════════════════════════════════════════════════
// Tarea FreeRTOS
// ═════════════════════════════════════════════════════════════════════════
void webserver_task(void* /*pv*/) {
    for (;;) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));   // cede CPU; 2 ms de latencia máxima
    }
}

void webserver_begin() {
    server.on("/",         HTTP_GET,  handle_root);
    server.on("/update",   HTTP_GET,  handle_update_get);
    server.on("/api/data", HTTP_GET,  handle_api_data);
    server.on("/update",   HTTP_POST, handle_update_post, handle_upload);
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
    server.begin();
    Serial0.println("[Web] Servidor en http://" + WiFi.localIP().toString());

    xTaskCreatePinnedToCore(webserver_task, "webserver",
                             8192, nullptr, 1, nullptr, 0);
}