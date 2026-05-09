#pragma once
#include <lvgl.h>

void chart_screen_init(lv_obj_t* parent);
void chart_screen_set_active(bool active);   // llamar desde evento tileview
void chart_screen_tick();                    // llamar desde loop()
