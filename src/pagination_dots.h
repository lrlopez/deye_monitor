#pragma once
#include <lvgl.h>

// N: número de tiles
void pagination_dots_init(lv_obj_t* parent, int n);
void pagination_dots_set(int active_idx);
