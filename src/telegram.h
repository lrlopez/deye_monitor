#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Tipos de alerta
enum class AlertType : uint8_t {
    BATT_LOW = 0,
    BATT_RECOVERED,
    SOLAR_START,
    SOLAR_STOP,
    LOGGER_FAIL,
    GRID_OUTAGE,
    GRID_RESTORED,
    TEST              // botón de prueba desde config
};

struct AlertMsg {
    AlertType type;
    int32_t   value;   // dato contextual (SOC, W, etc.)
};

class TelegramNotifier {
public:
    static TelegramNotifier& instance() {
        static TelegramNotifier t;
        return t;
    }

    // Llamar desde setup() después de WiFi conectado
    void begin(const char* token, const char* chat_id);

    // Actualizar credenciales sin reiniciar (tras guardar config)
    void setCredentials(const char* token, const char* chat_id);

    // Encolar una alerta (seguro llamar desde cualquier tarea)
    bool enqueue(AlertType type, int32_t value = 0);

    // Comprobar si las notificaciones están configuradas
    bool isConfigured() const;

private:
    TelegramNotifier() = default;

    char          _token[128]   = {};
    char          _chat_id[32]  = {};
    QueueHandle_t _queue        = nullptr;

    static void task(void* pv);
    bool        sendMessage(const String& text);
    String      formatMessage(const AlertMsg& msg);
};

#define Telegram TelegramNotifier::instance()
