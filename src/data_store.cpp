#include "data_store.h"
#include <LittleFS.h>
#include <time.h>

const char* DataStore::META_FILE = "/meta2.bin";

DataStore& DataStore::instance() {
    static DataStore d; return d;
}

// ── Meta ──────────────────────────────────────────────────────────────────
struct Meta2 {
    uint32_t magic, version;
    uint32_t raw_head,  raw_count,  raw_cap;
    uint32_t hrly_head, hrly_count, hrly_cap;
    uint32_t day_head,  day_count,  day_cap;
};

bool DataStore::loadMeta() {
    File f = LittleFS.open(META_FILE, "r");
    if (!f) return false;
    Meta2 m{};
    bool ok = f.read((uint8_t*)&m, sizeof(m)) == sizeof(m);
    f.close();
    if (!ok || m.magic != MAGIC || m.version != VERSION) return false;
    _raw.head  = m.raw_head;  _raw.count  = m.raw_count;  _raw.capacity  = m.raw_cap;
    _hrly.head = m.hrly_head; _hrly.count = m.hrly_count; _hrly.capacity = m.hrly_cap;
    _day.head  = m.day_head;  _day.count  = m.day_count;  _day.capacity  = m.day_cap;
    return true;
}

bool DataStore::saveMeta() {
    File f = LittleFS.open(META_FILE, "w");
    if (!f) return false;
    Meta2 m{MAGIC, VERSION,
            _raw.head,  _raw.count,  _raw.capacity,
            _hrly.head, _hrly.count, _hrly.capacity,
            _day.head,  _day.count,  _day.capacity};
    bool ok = f.write((uint8_t*)&m, sizeof(m)) == sizeof(m);
    f.close();
    return ok;
}

// ── Lectura/escritura genérica ────────────────────────────────────────────
uint32_t DataStore::physIdx(const CircBuf& cb, uint32_t logical) const {
    return (cb.head + logical) % cb.capacity;
}

bool DataStore::writeAt(const CircBuf& cb, uint32_t phys,
                         const void* data, size_t sz) {
    File f = LittleFS.open(cb.path, LittleFS.exists(cb.path) ? "r+" : "w");
    if (!f) return false;
    bool ok = f.seek((uint32_t)phys * sz) &&
              f.write((const uint8_t*)data, sz) == sz;
    f.close();
    return ok;
}

bool DataStore::readAt(const CircBuf& cb, uint32_t phys,
                        void* data, size_t sz) {
    File f = LittleFS.open(cb.path, "r");
    if (!f) return false;
    bool ok = f.seek((uint32_t)phys * sz) &&
              f.read((uint8_t*)data, sz) == sz;
    f.close();
    return ok;
}

// ── Inicialización ────────────────────────────────────────────────────────
bool DataStore::begin() {
    _mutex = xSemaphoreCreateMutex();
    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        Serial0.println("[Store] Error montando LittleFS"); return false;
    }
    size_t total = LittleFS.totalBytes();
    Serial0.printf("[Store] LittleFS: %u KB total, %u KB usados\n",
                  total/1024, (unsigned)LittleFS.usedBytes()/1024);

    if (loadMeta()) {
        Serial0.printf("[Store] Meta v2 cargada: raw=%lu hrly=%lu day=%lu\n",
                      (unsigned long)_raw.count,
                      (unsigned long)_hrly.count,
                      (unsigned long)_day.count);
        return true;
    }

    // Primera vez: calcular capacidades y guardar
    uint32_t avail  = total > 65536 ? total - 65536 : total;
    // Asignación proporcional:
    // raw=92%, hrly=7.6%, day=0.4%
    _raw.capacity  = 201600;   // fijo: 700 días × 288
    _hrly.capacity =  17520;   // fijo: 730 días × 24
    _day.capacity  =    730;   // fijo: 730 días
    _raw.head  = _raw.count  = 0;
    _hrly.head = _hrly.count = 0;
    _day.head  = _day.count  = 0;
    saveMeta();

    Serial0.printf("[Store] Nuevo: raw=%lu hrly=%lu day=%lu registros\n",
                  (unsigned long)_raw.capacity,
                  (unsigned long)_hrly.capacity,
                  (unsigned long)_day.capacity);
    return true;
}

// ── Raw push ──────────────────────────────────────────────────────────────
bool DataStore::push(const Record5Min& r) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    uint32_t phys;
    if (_raw.count < _raw.capacity) {
        phys = physIdx(_raw, _raw.count++);
    } else {
        phys = _raw.head;
        _raw.head = (_raw.head + 1) % _raw.capacity;
    }
    bool ok = writeAt(_raw, phys, &r, sizeof(r));
    if (ok) saveMeta();
    xSemaphoreGive(_mutex);
    return ok;
}

// ── Hourly push ───────────────────────────────────────────────────────────
bool DataStore::pushHourly(const HourlyRecord& r) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    uint32_t phys;
    if (_hrly.count < _hrly.capacity) {
        phys = physIdx(_hrly, _hrly.count++);
    } else {
        phys = _hrly.head;
        _hrly.head = (_hrly.head + 1) % _hrly.capacity;
    }
    bool ok = writeAt(_hrly, phys, &r, sizeof(r));
    if (ok) saveMeta();
    xSemaphoreGive(_mutex);

    Serial0.printf("[Hourly] %08lu guardado: pv=%dW grid=%dW bat=%dW load=%dW soc=%d%% n=%d\n",
                  (unsigned long)r.hour_epoch,
                  r.avg_pv_w, r.avg_grid_w, r.avg_batt_w, r.avg_load_w,
                  r.soc_end, r.sample_count);
    return ok;
}

// ── Daily push ────────────────────────────────────────────────────────────
bool DataStore::pushDaily(const DailyRecord& r) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    // Buscar si ya existe un registro para ese día (actualizar en lugar de duplicar)
    for (uint32_t i = 0; i < _day.count; i++) {
        uint32_t pi = physIdx(_day, i);
        DailyRecord existing{};
        if (readAt(_day, pi, &existing, sizeof(existing)) &&
            existing.day_epoch == r.day_epoch) {
            writeAt(_day, pi, &r, sizeof(r));
            saveMeta();
            xSemaphoreGive(_mutex);
            return true;
        }
    }

    uint32_t phys;
    if (_day.count < _day.capacity) {
        phys = physIdx(_day, _day.count++);
    } else {
        phys = _day.head;
        _day.head = (_day.head + 1) % _day.capacity;
    }
    bool ok = writeAt(_day, phys, &r, sizeof(r));
    if (ok) saveMeta();
    xSemaphoreGive(_mutex);

    Serial0.printf("[Daily] %08lu guardado: pv=%.1f exp=%.1f imp=%.1f load=%.1f kWh\n",
                  (unsigned long)r.day_epoch,
                  r.pv_10wh/10.0f, r.export_10wh/10.0f,
                  r.import_10wh/10.0f, r.load_10wh/10.0f);
    return ok;
}

// ── Raw read ──────────────────────────────────────────────────────────────
uint32_t DataStore::lowerBoundRaw(uint32_t ts) {
    if (_raw.count == 0) return 0;
    uint32_t lo = 0, hi = _raw.count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        Record5Min r{};
        if (readAt(_raw, physIdx(_raw, mid), &r, sizeof(r)) && r.timestamp < ts)
            lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

uint32_t DataStore::readDay(uint32_t dep, Record5Min* out, uint32_t max) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;
    uint32_t next = dep + 86400, found = 0;
    uint32_t start = lowerBoundRaw(dep);
    for (uint32_t i = start; i < _raw.count && found < max; i++) {
        Record5Min r{};
        if (!readAt(_raw, physIdx(_raw, i), &r, sizeof(r))) continue;
        if (r.timestamp >= next) break;
        out[found++] = r;
    }
    xSemaphoreGive(_mutex);
    return found;
}

bool DataStore::getLastRecord(Record5Min& out) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool ok = (_raw.count > 0) &&
              readAt(_raw, physIdx(_raw, _raw.count - 1), &out, sizeof(out));
    xSemaphoreGive(_mutex);
    return ok;
}

// ── Hourly read ───────────────────────────────────────────────────────────
uint32_t DataStore::lowerBoundHrly(uint32_t ts) {
    if (_hrly.count == 0) return 0;
    uint32_t lo = 0, hi = _hrly.count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        HourlyRecord r{};
        if (readAt(_hrly, physIdx(_hrly, mid), &r, sizeof(r)) && r.hour_epoch < ts)
            lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

uint8_t DataStore::readHourly(uint32_t dep, HourlyRecord out[24]) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;

    // Inicializar todas las horas como inválidas
    for (int h = 0; h < 24; h++) {
        out[h] = {};
        out[h].hour_epoch = dep + (uint32_t)h * 3600;
    }

    uint32_t next  = dep + 86400;
    uint32_t start = lowerBoundHrly(dep);
    uint8_t  found = 0;

    for (uint32_t i = start; i < _hrly.count; i++) {
        HourlyRecord r{};
        if (!readAt(_hrly, physIdx(_hrly, i), &r, sizeof(r))) continue;
        if (r.hour_epoch >= next) break;
        if (!(r.flags & 0x01)) continue;
        // Calcular índice de hora
        uint32_t h = (r.hour_epoch - dep) / 3600;
        if (h < 24) { out[h] = r; found++; }
    }

    xSemaphoreGive(_mutex);
    return found;
}

bool DataStore::getLastHourly(uint32_t dep, HourlyRecord& out) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    uint32_t next = dep + 86400;
    bool found = false;
    uint32_t start = lowerBoundHrly(dep);
    for (uint32_t i = start; i < _hrly.count; i++) {
        HourlyRecord r{};
        if (!readAt(_hrly, physIdx(_hrly, i), &r, sizeof(r))) continue;
        if (r.hour_epoch >= next) break;
        if (r.flags & 0x01) { out = r; found = true; }
    }
    xSemaphoreGive(_mutex);
    return found;
}

// ── Daily read ────────────────────────────────────────────────────────────
bool DataStore::readDaily(uint32_t dep, DailyRecord& out) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool found = false;
    for (uint32_t i = 0; i < _day.count; i++) {
        DailyRecord r{};
        if (readAt(_day, physIdx(_day, i), &r, sizeof(r)) &&
            r.day_epoch == dep && (r.flags & 0x01)) {
            out = r; found = true; break;
        }
    }
    xSemaphoreGive(_mutex);
    return found;
}

uint32_t DataStore::readAllDaily(DailyRecord* out, uint32_t max) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;
    uint32_t n = (_day.count < max) ? _day.count : max;
    for (uint32_t i = 0; i < n; i++)
        readAt(_day, physIdx(_day, i), &out[i], sizeof(out[i]));
    xSemaphoreGive(_mutex);
    return n;
}
