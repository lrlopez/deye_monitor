#pragma once
#include <lvgl.h>
#include "ui_constants.h"

// Alias locales para compatibilidad con el código existente
#define PWR_Y    CHART_PWR_Y
#define PWR_H    CHART_PWR_H
#define LEG_Y    CHART_LEG_Y
#define LEG_H    CHART_LEG_H
#define SOC_Y    CHART_SOC_Y
#define SOC_H    CHART_SOC_H
#define XLAB_Y   CHART_XLAB_Y

void chart_screen_init(lv_obj_t* parent);
void chart_screen_set_active(bool active);   // llamar desde evento tileview
void chart_screen_tick();                    // llamar desde loop()
