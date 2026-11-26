#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "oled.h"

#define MAX_ALBUMES 99
#define MAX_PISTAS 255
#define ALBUM_NOMBRE 64
#define TEMA_TEXTO 120
#define TEMA_LETRA 4096
#define LETRA_FILAS 3
#define LETRA_PRIMERA_FILA 2

typedef struct {
    char titulo[TEMA_TEXTO + 1];
    char artista[TEMA_TEXTO + 1];
    char letra[TEMA_LETRA + 1];
} tema_t;

typedef struct {
    char nombre[ALBUM_NOMBRE + 1];
    uint8_t pistas;
} album_t;

extern album_t albumes[MAX_ALBUMES];

esp_err_t temas_cargar(void);
esp_err_t album_guardar(unsigned album, const album_t *datos);
void tema_cargar(unsigned album, unsigned pista, tema_t *tema);
esp_err_t tema_guardar(unsigned album, unsigned pista, const tema_t *tema);
unsigned tema_letra(const char *letra, unsigned pagina, oled_vista_t *vista);
bool tema_letra_lrc(const char *letra, int64_t tiempo_ms, oled_vista_t *vista);
unsigned tema_duracion_ms(const char *letra);
void tema_texto(const char *texto, const char *alternativo, int64_t pasos,
                char salida[OLED_COLUMNAS + 1]);
