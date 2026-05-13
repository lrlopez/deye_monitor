#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>
#include "data_store.h"
#include "psram_alloc.h"

// Cache en PSRAM de los últimos CACHE_DAYS días
// Evita leer LittleFS para gráficas y estadísticas recientes
//
// Capacidad: 7 días × 288 registros × 32 bytes = 64,512 bytes en PSRAM
// Acceso: O(1) para hoy, O(log n) para días recientes

constexpr int    CACHE_DAYS     = 7;
constexpr int    RECS_PER_DAY   = 288;
constexpr size_t CACHE_BUF_SIZE = CACHE_DAYS * RECS_PER_DAY * sizeof(Record5Min);

struct CachedDay {
    uint32_t    day_epoch;
    uint16_t    count;        // registros válidos (0–288)
    bool        valid;
    HourAgg     hours[24];    // agregación horaria pre-calculada
    Record5Min  last;         // último registro del día (para totales)
};

class PsramCache {
public:
    static PsramCache& instance();

    // Llamar en setup() tras DataStore.begin()
    bool begin();

    // Añadir un registro recién grabado (desde solarmanTask)
    void push(const Record5Min& r);

    // Leer registros de 5 min de un día
    // Devuelve puntero directo al buffer PSRAM (sin copia) o nullptr
    // IMPORTANTE: usar bajo mutex (ver lock/unlock)
    const Record5Min* getDayRecs(uint32_t day_epoch, uint32_t& count_out);

    // Acceso a agregación horaria pre-calculada (sin mutex necesario para lectura)
    bool getHourAgg(uint32_t day_epoch, HourAgg out[24]);

    // Último registro de un día (totales diarios)
    bool getLastOfDay(uint32_t day_epoch, Record5Min& out);

    // Mutex para acceso seguro al buffer de registros
    void lock();
    void unlock();

    // Estadísticas
    void printStats();

private:
    PsramCache() = default;

    SemaphoreHandle_t _mutex = nullptr;

    // Buffer principal en PSRAM: CACHE_DAYS slots de RECS_PER_DAY registros
    Record5Min* _buf       = nullptr;   // CACHE_BUF_SIZE bytes en PSRAM
    CachedDay   _days[CACHE_DAYS] = {};  // metadatos en SRAM (pequeño)

    int _slot_for_day(uint32_t day_epoch) const;
    int _oldest_slot()                    const;
    void _recalc_hour_agg(int slot);
    void _load_from_store(uint32_t day_epoch);
};

#define Cache PsramCache::instance()
