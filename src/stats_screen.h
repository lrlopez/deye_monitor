#pragma once
#include <lvgl.h>
#include "solarman.h"

void stats_screen_init(lv_obj_t* parent);
void stats_screen_update(const DailyStats& data);
