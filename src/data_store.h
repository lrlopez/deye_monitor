#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>
#include "solarman.h"   // DailyStats

// ── Registro de 5 minutos ─────────────────────────────────────────────────
// 32 bytes exactos — no cambiar orden sin incrementar versión de formato
#pragma pack(push, 1)
struct Record5Min {
    uint32_t timestamp;     // epoch UTC

    // Potencias instantáneas (W) — valores RAW del inversor
    int16_t  pv_w;          // ≥ 0
    int16_t  grid_w;        // + importando / - exportando
    int16_t  batt_w;        // + descargando / - cargando  (RAW)
    int16_t  load_w;        // ≥ 0

    // Totales acumulados del día (unidad: 0.1 kWh)
    // Son los registros directos del inversor; se resetean a medianoche
    uint16_t day_pv;        // producción FV
    uint16_t day_export;    // exportación a red
    uint16_t day_import;    // importación de red
    uint16_t day_load;      // consumo de carga
    uint16_t day_bchg;      // carga de batería
    uint16_t day_bdis;      // descarga de batería

    uint8_t  soc;           // %
    uint8_t  flags;         // bit0=válido  bit1=parcial (arranque tras corte)

    int16_t  extra[3];      // reserva para 3 medidas futuras
};  // = 32 bytes
#pragma pack(pop)

static_assert(sizeof(Record5Min) == 32, "Record5Min must be 32 bytes");

// ── Agregación horaria ─────────────────────────────────────────────────────
struct HourAgg {
    float   pv_w;       // media W del periodo
    float   grid_w;
    float   batt_w;
    float   load_w;
    uint8_t soc;        // último valor de la hora
    // Totales acumulados al final de la hora (0.1 kWh)
    uint16_t day_pv, day_export, day_import;
    uint16_t day_load, day_bchg, day_bdis;
    bool    valid;
};

// ── Conversión a DailyStats ───────────────────────────────────────────────
inline DailyStats record_to_stats(const Record5Min& r) {
    DailyStats d{};
    d.pv_kwh             = r.day_pv     / 10.0f;
    d.export_kwh         = r.day_export / 10.0f;
    d.import_kwh         = r.day_import / 10.0f;
    d.load_kwh           = r.day_load   / 10.0f;
    d.batt_charge_kwh    = r.day_bchg   / 10.0f;
    d.batt_discharge_kwh = r.day_bdis   / 10.0f;
    d.valid = true;
    return d;
}

// ── Buffer circular en LittleFS ───────────────────────────────────────────
class DataStore {
public:
    static DataStore& instance();

    // Llamar en setup() tras LittleFS.begin()
    bool begin();

    // Escribe un registro (thread-safe)
    bool push(const Record5Min& r);

    // Lee todos los registros de un día (medianoche local epoch)
    // Devuelve número de registros encontrados
    uint32_t readDay(uint32_t day_epoch, Record5Min* out, uint32_t maxCount);

    // Último registro de un día (para totales diarios)
    bool getLastOfDay(uint32_t day_epoch, Record5Min& out);

    // Último registro almacenado (para detección de gaps al arrancar)
    bool getLastRecord(Record5Min& out);

    // Agrega en 24 cubos horarios
    void aggregateHourly(const Record5Min* recs, uint32_t count,
                          HourAgg out[24]);

    uint32_t getCapacity()   const { return _capacity; }
    uint32_t getCount()      const { return _count;    }
    uint32_t getDaysStored() const { return _count / 288; }

private:
    DataStore() = default;

    SemaphoreHandle_t _mutex    = nullptr;
    uint32_t          _head     = 0;        // índice lógico del más antiguo
    uint32_t          _count    = 0;        // registros válidos
    uint32_t          _capacity = 0;        // máximo

    static constexpr const char* DATA_FILE = "/data.bin";
    static constexpr const char* META_FILE = "/meta.bin";
    static constexpr uint32_t    MAGIC     = 0x5A5ADEAD;
    static constexpr uint8_t     VERSION   = 1;

    bool saveMeta();
    bool loadMeta();
    bool writeAt(uint32_t phys_idx, const Record5Min& r);
    bool readAt(uint32_t phys_idx, Record5Min& r);

    // Índice físico a partir de índice lógico (0 = más antiguo)
    uint32_t physIdx(uint32_t logical) const {
        return (_head + logical) % _capacity;
    }

    // Búsqueda binaria del primer registro con timestamp >= t
    uint32_t lowerBound(uint32_t t);
};

#define Store DataStore::instance()
