#include "data_store.h"
#include <LittleFS.h>
#include <time.h>

// ── Estructura de metadatos en disco ──────────────────────────────────────
struct Meta {
    uint32_t magic;
    uint8_t  version;
    uint32_t head;
    uint32_t count;
    uint32_t capacity;
};

DataStore& DataStore::instance() {
    static DataStore d;
    return d;
}

// ═════════════════════════════════════════════════════════════════════════
// Inicialización
// ═════════════════════════════════════════════════════════════════════════
bool DataStore::begin() {
    _mutex = xSemaphoreCreateMutex();

    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        Serial.println("[Store] Error montando LittleFS");
        return false;
    }

    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    Serial.printf("[Store] LittleFS: %u KB total, %u KB usados\n",
                  total / 1024, used / 1024);

    if (loadMeta()) return true;   // datos previos encontrados

    // Primera vez: calcular capacidad
    uint32_t avail = (total > 8192) ? total - 8192 : total;  // reserva ~8KB
    _capacity = avail / sizeof(Record5Min);
    _head = 0; _count = 0;
    saveMeta();
    Serial.printf("[Store] Nuevo: cap=%lu registros = %lu días\n",
                  (unsigned long)_capacity,
                  (unsigned long)(_capacity / 288));
    return true;
}

bool DataStore::loadMeta() {
    File f = LittleFS.open(META_FILE, "r");
    if (!f) return false;
    Meta m{}; bool ok = (f.read((uint8_t*)&m, sizeof(m)) == sizeof(m));
    f.close();
    if (!ok || m.magic != MAGIC || m.version != VERSION) return false;
    _head = m.head; _count = m.count; _capacity = m.capacity;
    Serial.printf("[Store] Meta cargada: head=%lu count=%lu cap=%lu\n",
                  (unsigned long)_head, (unsigned long)_count,
                  (unsigned long)_capacity);
    return true;
}

bool DataStore::saveMeta() {
    File f = LittleFS.open(META_FILE, "w");
    if (!f) return false;
    Meta m{MAGIC, VERSION, _head, _count, _capacity};
    bool ok = (f.write((uint8_t*)&m, sizeof(m)) == sizeof(m));
    f.close();
    return ok;
}

// ═════════════════════════════════════════════════════════════════════════
// Lectura / escritura de registros individuales
// ═════════════════════════════════════════════════════════════════════════
bool DataStore::writeAt(uint32_t phys, const Record5Min& r) {
    // Abrir en r+ si existe, w si no
    File f = LittleFS.open(DATA_FILE, LittleFS.exists(DATA_FILE) ? "r+" : "w");
    if (!f) return false;
    bool ok = f.seek((size_t)phys * sizeof(Record5Min)) &&
              f.write((uint8_t*)&r, sizeof(r)) == sizeof(r);
    f.close();
    return ok;
}

bool DataStore::readAt(uint32_t phys, Record5Min& r) {
    File f = LittleFS.open(DATA_FILE, "r");
    if (!f) return false;
    bool ok = f.seek((size_t)phys * sizeof(Record5Min)) &&
              f.read((uint8_t*)&r, sizeof(r)) == sizeof(r);
    f.close();
    return ok && (r.flags & 0x01);
}

// ═════════════════════════════════════════════════════════════════════════
// Búsqueda binaria (índices lógicos)
// ═════════════════════════════════════════════════════════════════════════
uint32_t DataStore::lowerBound(uint32_t t) {
    if (_count == 0) return 0;
    uint32_t lo = 0, hi = _count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        Record5Min r;
        if (readAt(physIdx(mid), r) && r.timestamp < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ═════════════════════════════════════════════════════════════════════════
// API pública
// ═════════════════════════════════════════════════════════════════════════
bool DataStore::push(const Record5Min& r) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    uint32_t phys;
    if (_count < _capacity) {
        phys = physIdx(_count);
        _count++;
    } else {
        phys = _head;
        _head = (_head + 1) % _capacity;
    }
    bool ok = writeAt(phys, r);
    if (ok) saveMeta();

    xSemaphoreGive(_mutex);
    Serial.printf("[Store] Registro guardado: ts=%lu pv=%d grid=%d bat=%d "
                  "load=%d soc=%d%% count=%lu\n",
                  (unsigned long)r.timestamp,
                  r.pv_w, r.grid_w, r.batt_w, r.load_w, r.soc,
                  (unsigned long)_count);
    return ok;
}

uint32_t DataStore::readDay(uint32_t day_epoch, Record5Min* out,
                             uint32_t maxCount) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;

    uint32_t next_day = day_epoch + 86400;
    uint32_t start = lowerBound(day_epoch);
    uint32_t found = 0;

    for (uint32_t i = start; i < _count && found < maxCount; i++) {
        Record5Min r;
        if (!readAt(physIdx(i), r)) continue;
        if (r.timestamp >= next_day) break;
        out[found++] = r;
    }

    xSemaphoreGive(_mutex);
    return found;
}

bool DataStore::getLastOfDay(uint32_t day_epoch, Record5Min& out) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

    uint32_t next_day = day_epoch + 86400;
    uint32_t start    = lowerBound(day_epoch);
    bool found = false;

    for (uint32_t i = start; i < _count; i++) {
        Record5Min r;
        if (!readAt(physIdx(i), r)) continue;
        if (r.timestamp >= next_day) break;
        out = r; found = true;
    }

    xSemaphoreGive(_mutex);
    return found;
}

bool DataStore::getLastRecord(Record5Min& out) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool ok = (_count > 0) && readAt(physIdx(_count - 1), out);
    xSemaphoreGive(_mutex);
    return ok;
}

void DataStore::aggregateHourly(const Record5Min* recs, uint32_t count,
                                  HourAgg out[24]) {
    struct Acc { float pv=0,grid=0,batt=0,load=0; int n=0; uint8_t soc=0;
                 uint16_t dpv=0,dexp=0,dimp=0,dlod=0,dbch=0,dbdi=0; };
    Acc acc[24]{};

    for (uint32_t i = 0; i < count; i++) {
        const Record5Min& r = recs[i];
        time_t t = (time_t)r.timestamp;
        struct tm tm; localtime_r(&t, &tm);
        int h = tm.tm_hour;
        acc[h].pv   += r.pv_w;
        acc[h].grid += r.grid_w;
        acc[h].batt += r.batt_w;
        acc[h].load += r.load_w;
        acc[h].soc   = r.soc;
        // Acumulados: guardar el último valor de la hora
        acc[h].dpv  = r.day_pv;
        acc[h].dexp = r.day_export;
        acc[h].dimp = r.day_import;
        acc[h].dlod = r.day_load;
        acc[h].dbch = r.day_bchg;
        acc[h].dbdi = r.day_bdis;
        acc[h].n++;
    }

    for (int h = 0; h < 24; h++) {
        if (acc[h].n > 0) {
            float n = acc[h].n;
            out[h] = {
                acc[h].pv/n, acc[h].grid/n, acc[h].batt/n, acc[h].load/n,
                acc[h].soc,
                acc[h].dpv, acc[h].dexp, acc[h].dimp,
                acc[h].dlod, acc[h].dbch, acc[h].dbdi,
                true
            };
        } else {
            out[h] = {};
        }
    }
}
