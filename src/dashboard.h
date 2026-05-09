#pragma once
#include <lvgl.h>
#include "solarman.h"

void dashboard_init(lv_obj_t* parent);   // antes: void dashboard_init()
void dashboard_update(const EnergyData& data);