#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define NOMBRE_CONTROL_BLE "MP3-Control"
#define COMANDO_BLE_TAM 16

esp_err_t control_ble_iniciar(void);
bool control_ble_recibir(char comando[COMANDO_BLE_TAM]);
void control_ble_publicar(unsigned album, unsigned pista, unsigned volumen, const char *estado);
