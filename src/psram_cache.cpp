#include "psram_cache.h"
#include <time.h>

PsramCache& PsramCache::instance() {
    static PsramCache c;
    return c;
}

// ── Inicialización ────────────────────────────────────────────────────────
bool PsramCache::begin() {
    _mutex = xSemaphoreCreateMutex();

    // Alojar buffer principal en PSRAM
    _buf = (Record5Min*)heap_caps_malloc(CACHE_BUF_SIZE,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_buf) {
        Serial.println("[Cache] ERROR: no hay PSRAM disponible");
        return false;
    }
    memset(_buf, 0, CACHE_BUF_SIZE);
    memset(_days, 0, sizeof(_days));

    Serial.printf("[Cache] Buffer PSRAM: %u KB en 0x%08X\n",
                  (unsigned)(CACHE_BUF_SIZE / 1024), (uintptr_t)_buf);

    // Cargar los últimos CACHE_DAYS días desde DataStore
    time_t now; time(&now);
    struct tm tm_now; localtime_r(&now, &tm_now);
    tm_now.tm_hour = 0; tm_now.tm_min = 0;
    tm_now.tm_sec  = 0; tm_now.tm_isdst = -1;
    uint32_t today = (uint32_t)mktime(&tm_now);

    for (int i = 0; i < CACHE_DAYS; i++) {
        uint32_t dep = today - (uint32_t)(CACHE_DAYS - 1 - i) * 86400;
        _days[i].day_epoch = dep;
        _days[i].valid     = false;
        _days[i].count     = 0;
        _load_from_store(dep);
    }

    printStats();
    return true;
}

// ── Slot para un día ───────────────────────────────────────────────────────
int PsramCache::_slot_for_day(uint32_t dep) const {
    for (int i = 0; i < CACHE_DAYS; i++)
        if (_days[i].day_epoch == dep) return i;
    return -1;
}

int PsramCache::_oldest_slot() const {
    int oldest = 0;
    for (int i = 1; i < CACHE_DAYS; i++)
        if (_days[i].day_epoch < _days[oldest].day_epoch) oldest = i;
    return oldest;
}

// ── Carga desde DataStore ─────────────────────────────────────────────────
void PsramCache::_load_from_store(uint32_t dep) {
    int slot = _slot_for_day(dep);
    if (slot < 0) {
        // Reemplazar slot más antiguo
        slot = _oldest_slot();
        _days[slot].day_epoch = dep;
        _days[slot].count     = 0;
        _days[slot].valid     = false;
    }

    Record5Min* slot_buf = _buf + slot * RECS_PER_DAY;
    uint32_t n = Store.readDay(dep, slot_buf, RECS_PER_DAY);
    _days[slot].count = (uint16_t)n;
    _days[slot].valid = (n > 0);

    if (n > 0) {
        _days[slot].last = slot_buf[n - 1];
        _recalc_hour_agg(slot);
        Serial.printf("[Cache] Slot %d cargado: día %lu → %u registros\n",
                      slot, (unsigned long)dep, n);
    }
}

// ── Re-calcular agregación horaria ────────────────────────────────────────
void PsramCache::_recalc_hour_agg(int slot) {
    Record5Min* slot_buf = _buf + slot * RECS_PER_DAY;
    Store.aggregateHourly(slot_buf, _days[slot].count, _days[slot].hours);
}

// ── push: añadir registro recién grabado ──────────────────────────────────
void PsramCache::push(const Record5Min& r) {
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // Calcular medianoche del timestamp del registro
    time_t t = (time_t)r.timestamp;
    struct tm tm_r; localtime_r(&t, &tm_r);
    tm_r.tm_hour = 0; tm_r.tm_min = 0; tm_r.tm_sec = 0; tm_r.tm_isdst = -1;
    uint32_t dep = (uint32_t)mktime(&tm_r);

    int slot = _slot_for_day(dep);
    if (slot < 0) {
        // Nuevo día: reusar slot más antiguo
        slot = _oldest_slot();
        _days[slot].day_epoch = dep;
        _days[slot].count     = 0;
        _days[slot].valid     = false;
        memset(_buf + slot * RECS_PER_DAY, 0,
               RECS_PER_DAY * sizeof(Record5Min));
        Serial.printf("[Cache] Nuevo día en slot %d: %lu\n",
                      slot, (unsigned long)dep);
    }

    uint16_t& cnt = _days[slot].count;
    if (cnt < RECS_PER_DAY) {
        _buf[slot * RECS_PER_DAY + cnt] = r;
        cnt++;
    } else {
        // Slot lleno (no debería pasar en condiciones normales)
        // Desplazar y añadir al final
        memmove(_buf + slot * RECS_PER_DAY,
                _buf + slot * RECS_PER_DAY + 1,
                (RECS_PER_DAY - 1) * sizeof(Record5Min));
        _buf[slot * RECS_PER_DAY + RECS_PER_DAY - 1] = r;
    }
    _days[slot].valid = true;
    _days[slot].last  = r;
    _recalc_hour_agg(slot);

    xSemaphoreGive(_mutex);
}

// ── API de lectura ────────────────────────────────────────────────────────
const Record5Min* PsramCache::getDayRecs(uint32_t dep, uint32_t& count_out) {
    int slot = _slot_for_day(dep);
    if (slot < 0 || !_days[slot].valid) {
        // Intentar cargar desde DataStore
        _load_from_store(dep);
        slot = _slot_for_day(dep);
    }
    if (slot < 0 || !_days[slot].valid) { count_out = 0; return nullptr; }
    count_out = _days[slot].count;
    return _buf + slot * RECS_PER_DAY;
}

bool PsramCache::getHourAgg(uint32_t dep, HourAgg out[24]) {
    int slot = _slot_for_day(dep);
    if (slot < 0 || !_days[slot].valid) {
        _load_from_store(dep);
        slot = _slot_for_day(dep);
    }
    if (slot < 0 || !_days[slot].valid) return false;
    memcpy(out, _days[slot].hours, sizeof(HourAgg) * 24);
    return true;
}

bool PsramCache::getLastOfDay(uint32_t dep, Record5Min& out) {
    int slot = _slot_for_day(dep);
    if (slot < 0 || !_days[slot].valid) {
        _load_from_store(dep);
        slot = _slot_for_day(dep);
    }
    if (slot < 0 || !_days[slot].valid) return false;
    out = _days[slot].last;
    return true;
}

void PsramCache::lock()   { xSemaphoreTake(_mutex, portMAX_DELAY); }
void PsramCache::unlock() { xSemaphoreGive(_mutex); }

void PsramCache::printStats() {
    Serial.printf("[Cache] %d slots | PSRAM usada: %u KB\n",
                  CACHE_DAYS, (unsigned)(CACHE_BUF_SIZE / 1024));
    for (int i = 0; i < CACHE_DAYS; i++) {
        if (_days[i].valid)
            Serial.printf("  Slot %d: día %lu → %d registros\n",
                          i, (unsigned long)_days[i].day_epoch,
                          _days[i].count);
    }
}
