#pragma once

#include "esp_err.h"

#define OLED_FILAS 6
#define OLED_COLUMNAS 21

typedef struct {
    char texto[OLED_FILAS][OLED_COLUMNAS + 1];
} oled_vista_t;

esp_err_t oled_init(void);
esp_err_t oled_show(const oled_vista_t *vista);
