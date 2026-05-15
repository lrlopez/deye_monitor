#include "telegram.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Certificado raíz de api.telegram.org (DigiCert Global Root CA)
// Válido hasta 2031 — actualizar si caduca
/*static const char* TELEGRAM_CERT = R"=EOF=(
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)=EOF=";*/

static const char* API_HOST = "api.telegram.org";
static SemaphoreHandle_t s_net_sem = nullptr;

// ── Mensajes localizados ───────────────────────────────────────────────────
static String fmt_msg(const AlertMsg& msg) {
    switch (msg.type) {
    case AlertType::BATT_LOW:
        return String("⚠️ *Batería baja*\n")
             + "SOC actual: *" + msg.value + "%*\n"
             + "La batería ha bajado del umbral configurado.";

    case AlertType::BATT_RECOVERED:
        return String("✅ *Batería recuperada*\n")
             + "SOC actual: *" + msg.value + "%*\n"
             + "La batería ha vuelto a nivel normal.";

    case AlertType::SOLAR_START:
        return String("☀️ *Producción solar iniciada*\n")
             + "Potencia actual: *" + msg.value + " W*";

    case AlertType::SOLAR_STOP:
        return String("🌙 *Producción solar detenida*\n")
             + "El inversor ha dejado de generar energía.";

    case AlertType::LOGGER_FAIL:
        return String("❌ *Sin comunicación con el inversor*\n")
             + "No se puede conectar al datalogger.\n"
             + "Verifica la red WiFi y la IP del logger.";

    case AlertType::GRID_OUTAGE:
        return String("🔴 *Posible corte de red eléctrica*\n")
             + "Importación detectada: *" + msg.value + " W*\n"
             + "El sistema está operando con batería/solar.";

    case AlertType::GRID_RESTORED:
        return String("🟢 *Red eléctrica restaurada*\n")
             + "El sistema vuelve a operar con red.";

    case AlertType::TEST:
        return String("🔔 *Notificaciones Telegram activas*\n")
             + "El monitor Deye está funcionando correctamente.";

    default:
        return "📡 Evento desconocido";
    }
}

// ── Tarea FreeRTOS ─────────────────────────────────────────────────────────
void TelegramNotifier::task(void* pv) {
    TelegramNotifier* self = static_cast<TelegramNotifier*>(pv);
    AlertMsg msg;

    for (;;) {
        // Espera bloqueante — no consume CPU mientras no hay alertas
        if (xQueueReceive(self->_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (!self->isConfigured()) continue;
            if (WiFi.status() != WL_CONNECTED) continue;

            String text = fmt_msg(msg);
            bool ok = self->sendMessage(text);
            Serial0.printf("[Telegram] %s → %s\n",
                          text.substring(0, 30).c_str(), ok ? "OK" : "FALLO");

            // Si falla, reintenta una vez tras 5 s
            if (!ok) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                self->sendMessage(text);
            }
        }
    }
}

// ── HTTP POST a la API de Telegram ────────────────────────────────────────
bool TelegramNotifier::sendMessage(const String& text) {
    WiFiClientSecure client;

    time_t now; time(&now);
    if (now < 1700000000UL) {
        Serial0.println("[Telegram] NTP no sincronizado");
        return false;
    }

    if (s_net_sem) {
        if (xSemaphoreTake(s_net_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            return false;
        }
    }

    client.setInsecure();
    //client.setCACert(TELEGRAM_CERT);
    client.setTimeout(10);

    HTTPClient http;
    String url = String("https://") + API_HOST
               + "/bot" + _token + "/sendMessage";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    // Escapado básico de comillas en el texto
    String escaped = text;
    escaped.replace("\"", "\\\"");
    escaped.replace("\n", "\\n");  // IMPORTANTE: Escapa saltos de línea    
    String body = String("{\"chat_id\":\"") + _chat_id
                + "\",\"text\":\"" + escaped
                + "\",\"parse_mode\":\"Markdown\"}";

    int code = http.POST(body);
    bool ok  = (code == 200);
    
    Serial0.println("URL: " + url);
    Serial0.println("Body: " + body);
    Serial0.printf("Response: %d\n", code);
    
    http.end();

    if (s_net_sem) xSemaphoreGive(s_net_sem);

    return ok;
}

// ── API pública ───────────────────────────────────────────────────────────
void TelegramNotifier::begin(const char* token, const char* chat_id) {
    Serial0.println("[Telegram] Iniciando...");
    setCredentials(token, chat_id);

    _queue = xQueueCreate(8, sizeof(AlertMsg));
    xTaskCreatePinnedToCore(task, "telegram",
                             8 * 1024, this, 1, nullptr, 0);
    Serial0.printf("[Telegram] Iniciado. Configurado: %s\n",
                  isConfigured() ? "SI" : "NO");
}

void TelegramNotifier::setCredentials(const char* token, const char* chat_id) {
    strncpy(_token,   token,   sizeof(_token)   - 1);
    strncpy(_chat_id, chat_id, sizeof(_chat_id) - 1);
}

bool TelegramNotifier::enqueue(AlertType type, int32_t value) {
    if (!_queue) return false;
    AlertMsg msg{type, value};
    return xQueueSend(_queue, &msg, 0) == pdTRUE;
}

bool TelegramNotifier::isConfigured() const {
    return _token[0] != '\0' && _chat_id[0] != '\0';
}

void TelegramNotifier::setNetworkSemaphore(SemaphoreHandle_t sem) {
    s_net_sem = sem;
}