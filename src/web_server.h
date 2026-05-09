#pragma once
#include "solarman.h"
#include "storage.h"

// Arranca el servidor en el puerto 80.
// Debe llamarse desde setup(), después de WiFi.begin().
void webserver_begin();

// Tarea FreeRTOS interna – no llamar directamente.
void webserver_task(void* pv);

// Inyecta el puntero al mutex y a los datos compartidos.
// Llamar antes de webserver_begin().
void webserver_set_data(SemaphoreHandle_t mutex,
                        const EnergyData* energy,
                        const DailyStats* daily);
                        