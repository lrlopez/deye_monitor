#include "psram_cache.h"
#include <time.h>
#include <string.h>

PsramCache& PsramCache::instance() { static PsramCache c; return c; }

// ═══════════════════════════════════════════════════════════════════════════
// begin()
// ═══════════════════════════════════════════════════════════════════════════
bool PsramCache::begin() {
    _mutex = xSemaphoreCreateMutex();

    // Raw buffer
    _raw_buf = (Record5Min*)heap_caps_malloc(CACHE_RAW_SIZE,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _raw_days = (CachedRawDay*)heap_caps_malloc(
                    CACHE_RAW_DAYS * sizeof(CachedRawDay),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Hourly buffer
    _hrly_buf = (HourlyRecord*)heap_caps_malloc(CACHE_HRLY_SIZE,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _hrly_days = (CachedHrlyDay*)heap_caps_malloc(
                    CACHE_HRLY_DAYS * sizeof(CachedHrlyDay),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Daily buffer
    _day_buf = (DailyRecord*)heap_caps_malloc(CACHE_DAY_SIZE,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!_raw_buf || !_raw_days || !_hrly_buf || !_hrly_days || !_day_buf) {
        Serial.println("[Cache] ERROR: PSRAM insuficiente"); return false;
    }

    memset(_raw_buf,   0, CACHE_RAW_SIZE);
    memset(_raw_days,  0, CACHE_RAW_DAYS  * sizeof(CachedRawDay));
    memset(_hrly_buf,  0, CACHE_HRLY_SIZE);
    memset(_hrly_days, 0, CACHE_HRLY_DAYS * sizeof(CachedHrlyDay));
    memset(_day_buf,   0, CACHE_DAY_SIZE);

    Serial.printf("[Cache] PSRAM: raw=%uKB hrly=%uKB day=%uKB total=%uKB\n",
                  (unsigned)(CACHE_RAW_SIZE/1024),
                  (unsigned)(CACHE_HRLY_SIZE/1024),
                  (unsigned)(CACHE_DAY_SIZE/1024),
                  (unsigned)((CACHE_RAW_SIZE+CACHE_HRLY_SIZE+CACHE_DAY_SIZE)/1024));

    // Asignar epochs a los últimos N días
    time_t now; time(&now);
    struct tm tm_now; localtime_r(&now, &tm_now);
    tm_now.tm_hour = 0; tm_now.tm_min = 0; tm_now.tm_sec = 0; tm_now.tm_isdst = -1;
    uint32_t today = (uint32_t)mktime(&tm_now);

    for (int i = 0; i < CACHE_RAW_DAYS; i++) {
        _raw_days[i].day_epoch = today - (uint32_t)(CACHE_RAW_DAYS-1-i)*86400;
        _raw_days[i].loaded    = false;
        _raw_days[i].valid     = false;
    }
    for (int i = 0; i < CACHE_HRLY_DAYS; i++) {
        _hrly_days[i].day_epoch = today - (uint32_t)(CACHE_HRLY_DAYS-1-i)*86400;
        _hrly_days[i].loaded    = false;
        _hrly_days[i].valid     = false;
    }

    // Cargar daily completo (pequeño, carga eager)
    _day_load_all();

    // Cargar últimos 7 días eager, resto en background
    for (int i = CACHE_RAW_DAYS-7; i < CACHE_RAW_DAYS; i++)
        _raw_load(_raw_days[i].day_epoch);
    for (int i = CACHE_HRLY_DAYS-7; i < CACHE_HRLY_DAYS; i++)
        _hrly_load(_hrly_days[i].day_epoch);

    xTaskCreatePinnedToCore(_bg_task, "cache_bg", 4096, this, 0, nullptr, 0);
    return true;
}

void PsramCache::_bg_task(void* pv) {
    PsramCache* self = static_cast<PsramCache*>(pv);
    for (int i = CACHE_HRLY_DAYS-8; i >= 0; i--) {
        if (!self->_hrly_days[i].loaded) {
            self->_hrly_load(self->_hrly_days[i].day_epoch);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    Serial.println("[Cache] Carga background completada");
    vTaskDelete(nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// Daily cache — load all / find / push / get
// ═══════════════════════════════════════════════════════════════════════════
void PsramCache::_day_load_all() {
    _day_count = Store.readAllDaily(_day_buf, CACHE_DAY_MAX);
    Serial.printf("[Cache] Daily cargado: %lu registros\n",
                  (unsigned long)_day_count);
}

int PsramCache::_day_find(uint32_t dep) const {
    // Búsqueda lineal (730 elementos, trivial)
    for (uint32_t i = 0; i < _day_count; i++)
        if (_day_buf[i].day_epoch == dep) return (int)i;
    return -1;
}

bool PsramCache::getDaily(uint32_t dep, DailyRecord& out) {
    int idx = _day_find(dep);
    if (idx < 0 || !(_day_buf[idx].flags & 0x01)) return false;
    out = _day_buf[idx];
    return true;
}

void PsramCache::pushDaily(const DailyRecord& r) {
    Store.pushDaily(r);

    int idx = _day_find(r.day_epoch);
    if (idx >= 0) {
        _day_buf[idx] = r;   // actualizar existente
    } else if (_day_count < CACHE_DAY_MAX) {
        _day_buf[_day_count++] = r;   // añadir nuevo
    } else {
        // Buffer lleno: desplazar (rarísimo, 730 días = 2 años)
        memmove(_day_buf, _day_buf + 1, (_day_count-1) * sizeof(DailyRecord));
        _day_buf[_day_count-1] = r;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Hourly cache
// ═══════════════════════════════════════════════════════════════════════════
int PsramCache::_hrly_slot_for(uint32_t dep) const {
    for (int i = 0; i < CACHE_HRLY_DAYS; i++)
        if (_hrly_days[i].day_epoch == dep) return i;
    return -1;
}

int PsramCache::_hrly_oldest_slot() const {
    int oldest = 0;
    for (int i = 1; i < CACHE_HRLY_DAYS; i++)
        if (_hrly_days[i].day_epoch < _hrly_days[oldest].day_epoch) oldest = i;
    return oldest;
}

void PsramCache::_hrly_load(uint32_t dep) {
    int slot = _hrly_slot_for(dep);
    if (slot < 0) {
        slot = _hrly_oldest_slot();
        _hrly_days[slot].day_epoch   = dep;
        _hrly_days[slot].valid       = false;
        _hrly_days[slot].hours_valid = 0;
    }
    HourlyRecord* dst = _hrly_buf + slot * 24;
    uint8_t n = Store.readHourly(dep, dst);
    _hrly_days[slot].loaded      = true;
    _hrly_days[slot].valid       = (n > 0);
    _hrly_days[slot].hours_valid = n;
}

const HourlyRecord* PsramCache::getHourly(uint32_t dep) {
    int slot = _hrly_slot_for(dep);
    if (slot < 0 || !_hrly_days[slot].loaded) {
        _hrly_load(dep);
        slot = _hrly_slot_for(dep);
    }
    if (slot < 0 || !_hrly_days[slot].valid) return nullptr;
    return _hrly_buf + slot * 24;
}

void PsramCache::pushHourly(const HourlyRecord& r) {
    Store.pushHourly(r);

    time_t t = (time_t)r.hour_epoch;
    struct tm tm_r; localtime_r(&t, &tm_r);
    tm_r.tm_hour = 0; tm_r.tm_min = 0; tm_r.tm_sec = 0; tm_r.tm_isdst = -1;
    uint32_t dep = (uint32_t)mktime(&tm_r);
    int h        = (int)((r.hour_epoch - dep) / 3600);
    if (h < 0 || h >= 24) return;

    int slot = _hrly_slot_for(dep);
    if (slot < 0) {
        slot = _hrly_oldest_slot();
        _hrly_days[slot].day_epoch   = dep;
        _hrly_days[slot].valid       = false;
        _hrly_days[slot].hours_valid = 0;
        _hrly_days[slot].loaded      = true;
        memset(_hrly_buf + slot*24, 0, 24*sizeof(HourlyRecord));
    }
    _hrly_buf[slot*24 + h]  = r;
    _hrly_days[slot].valid  = true;
    _hrly_days[slot].hours_valid++;
}

void PsramCache::invalidateHourly(uint32_t dep) {
    int slot = _hrly_slot_for(dep);
    if (slot >= 0) { _hrly_days[slot].loaded = false; _hrly_days[slot].valid = false; }
}

// ═══════════════════════════════════════════════════════════════════════════
// Raw cache (igual que antes, simplificado)
// ═══════════════════════════════════════════════════════════════════════════
int PsramCache::_raw_slot_for(uint32_t dep) const {
    for (int i = 0; i < CACHE_RAW_DAYS; i++)
        if (_raw_days[i].day_epoch == dep) return i;
    return -1;
}

int PsramCache::_raw_oldest_slot() const {
    int oldest = 0;
    for (int i = 1; i < CACHE_RAW_DAYS; i++)
        if (_raw_days[i].day_epoch < _raw_days[oldest].day_epoch) oldest = i;
    return oldest;
}

void PsramCache::_raw_load(uint32_t dep) {
    int slot = _raw_slot_for(dep);
    if (slot < 0) {
        slot = _raw_oldest_slot();
        _raw_days[slot].day_epoch = dep;
        _raw_days[slot].count     = 0;
        _raw_days[slot].valid     = false;
    }
    Record5Min* dst = _raw_buf + slot * RECS_PER_DAY;
    uint32_t n = Store.readDay(dep, dst, RECS_PER_DAY);
    _raw_days[slot].count  = (uint16_t)n;
    _raw_days[slot].valid  = (n > 0);
    _raw_days[slot].loaded = true;
    if (n > 0) _raw_days[slot].last = dst[n-1];
}

const Record5Min* PsramCache::getRawDay(uint32_t dep, uint32_t& cnt) {
    int slot = _raw_slot_for(dep);
    if (slot < 0 || !_raw_days[slot].loaded) { _raw_load(dep); slot = _raw_slot_for(dep); }
    if (slot < 0 || !_raw_days[slot].valid) { cnt = 0; return nullptr; }
    cnt = _raw_days[slot].count;
    return _raw_buf + slot * RECS_PER_DAY;
}

bool PsramCache::getLastRaw(uint32_t dep, Record5Min& out) {
    int slot = _raw_slot_for(dep);
    if (slot < 0 || !_raw_days[slot].loaded) { _raw_load(dep); slot = _raw_slot_for(dep); }
    if (slot < 0 || !_raw_days[slot].valid) return false;
    out = _raw_days[slot].last; return true;
}

void PsramCache::pushRaw(const Record5Min& r) {
    time_t t = (time_t)r.timestamp;
    struct tm tm_r; localtime_r(&t, &tm_r);
    tm_r.tm_hour = 0; tm_r.tm_min = 0; tm_r.tm_sec = 0; tm_r.tm_isdst = -1;
    uint32_t dep = (uint32_t)mktime(&tm_r);

    int slot = _raw_slot_for(dep);
    if (slot < 0) {
        slot = _raw_oldest_slot();
        _raw_days[slot].day_epoch = dep;
        _raw_days[slot].count     = 0;
        _raw_days[slot].valid     = false;
        _raw_days[slot].loaded    = true;
    }
    uint16_t& cnt = _raw_days[slot].count;
    if (cnt < RECS_PER_DAY) {
        _raw_buf[slot*RECS_PER_DAY + cnt] = r; cnt++;
    } else {
        memmove(_raw_buf + slot*RECS_PER_DAY,
                _raw_buf + slot*RECS_PER_DAY + 1,
                (RECS_PER_DAY-1)*sizeof(Record5Min));
        _raw_buf[slot*RECS_PER_DAY + RECS_PER_DAY-1] = r;
    }
    _raw_days[slot].valid = true;
    _raw_days[slot].last  = r;
}

void PsramCache::printStats() {
    uint32_t total_psram = CACHE_RAW_SIZE + CACHE_HRLY_SIZE + CACHE_DAY_SIZE;
    Serial.printf("[Cache] PSRAM total: %u KB | Daily: %lu/%d registros\n",
                  (unsigned)(total_psram/1024),
                  (unsigned long)_day_count, CACHE_DAY_MAX);
}
