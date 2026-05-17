#pragma once
#include <lvgl.h>

typedef void (*CalendarSelectCb)(uint32_t day_epoch);

// selected_epoch : día actualmente seleccionado (para resaltarlo)
// oldest_epoch   : primer día disponible — los anteriores quedan desactivados
//                  (0 = sin límite)
// cb             : llamada con el day_epoch del día pulsado
void calendar_show(uint32_t selected_epoch,
                   uint32_t oldest_epoch,
                   CalendarSelectCb cb);

void calendar_hide();
bool calendar_is_visible();
