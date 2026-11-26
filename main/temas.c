#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "temas.h"

#define PARTICION "mp3_catalogo"
#define ESPACIO "biblioteca"

album_t albumes[MAX_ALBUMES] = {{.nombre = "Álbum 01", .pistas = 3}};

esp_err_t temas_cargar(void)
{
    esp_err_t resultado = nvs_flash_init_partition(PARTICION);
    if (resultado != ESP_OK) {
        return resultado;
    }
    nvs_handle_t nvs;
    resultado = nvs_open_from_partition(PARTICION, ESPACIO, NVS_READONLY, &nvs);
    if (resultado != ESP_OK) {
        return resultado == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : resultado;
    }
    for (unsigned album = 1; album <= MAX_ALBUMES; album++) {
        album_t datos;
        char clave[16];
        snprintf(clave, sizeof(clave), "a%02u", album);
        size_t tam = sizeof(datos);
        if (nvs_get_blob(nvs, clave, &datos, &tam) == ESP_OK && tam == sizeof(datos) &&
            datos.pistas && datos.nombre[0]) {
            datos.nombre[ALBUM_NOMBRE] = 0;
            albumes[album - 1] = datos;
        }
    }
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t guardar(const char *clave, const void *datos, size_t tam)
{
    nvs_handle_t nvs;
    esp_err_t resultado = nvs_open_from_partition(PARTICION, ESPACIO, NVS_READWRITE, &nvs);
    if (resultado != ESP_OK) {
        return resultado;
    }
    resultado = nvs_set_blob(nvs, clave, datos, tam);
    if (resultado == ESP_OK) {
        resultado = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return resultado;
}

esp_err_t album_guardar(unsigned album, const album_t *datos)
{
    char clave[16];
    snprintf(clave, sizeof(clave), "a%02u", album);
    esp_err_t resultado = guardar(clave, datos, sizeof(*datos));
    if (resultado == ESP_OK) {
        albumes[album - 1] = *datos;
    }
    return resultado;
}

void tema_cargar(unsigned album, unsigned pista, tema_t *tema)
{
    memset(tema, 0, sizeof(*tema));
    char clave[24];
    snprintf(clave, sizeof(clave), "a%02up%03u", album, pista);
    size_t tam = sizeof(*tema);
    nvs_handle_t nvs;
    esp_err_t resultado = nvs_open_from_partition(PARTICION, ESPACIO, NVS_READONLY, &nvs);
    if (resultado == ESP_OK) {
        resultado = nvs_get_blob(nvs, clave, tema, &tam);
        nvs_close(nvs);
    }
    // La carpeta 01 puede conservar las fichas grabadas por la versión anterior.
    if (resultado != ESP_OK && album == 1 && pista <= 3 &&
        nvs_open("mp3_temas", NVS_READONLY, &nvs) == ESP_OK) {
        snprintf(clave, sizeof(clave), "pista%u", pista);
        tam = sizeof(*tema);
        resultado = nvs_get_blob(nvs, clave, tema, &tam);
        nvs_close(nvs);
    }
    if (resultado != ESP_OK || tam <= offsetof(tema_t, letra)) {
        memset(tema, 0, sizeof(*tema));
    } else {
        tema->titulo[TEMA_TEXTO] = 0;
        tema->artista[TEMA_TEXTO] = 0;
        tema->letra[tam - offsetof(tema_t, letra) - 1] = 0;
    }
}

esp_err_t tema_guardar(unsigned album, unsigned pista, const tema_t *tema)
{
    char clave[24];
    snprintf(clave, sizeof(clave), "a%02up%03u", album, pista);
    return guardar(clave, tema, offsetof(tema_t, letra) + strlen(tema->letra) + 1);
}

static char leer_caracter(const char **texto)
{
    const unsigned char *s = (const unsigned char *)*texto;
    unsigned char primero = *s++;
    unsigned char segundo = *s;
    while ((*s & 0xC0) == 0x80) {
        s++;
    }
    *texto = (const char *)s;
    if (primero == '\r' || primero == '\t') {
        return ' ';
    }
    if (primero < 0x80) {
        return primero >= 32 && primero <= 90 ? primero :
               (primero >= 'a' && primero <= 'z' ? primero : '?');
    }
    if (primero == 0xC3) {
        switch (segundo) {
        case 0x81: case 0xA1: return 'A';
        case 0x89: case 0xA9: return 'E';
        case 0x8D: case 0xAD: return 'I';
        case 0x91: case 0xB1: return 'N';
        case 0x93: case 0xB3: return 'O';
        case 0x9A: case 0xBA: case 0x9C: case 0xBC: return 'U';
        }
    }
    if (primero == 0xE2 && segundo == 0x80) {
        return '-';
    }
    if (primero == 0xC2 && segundo == 0xA1) {
        return '!';
    }
    return '?';
}

static const char *leer_fila(const char *texto, char salida[OLED_COLUMNAS + 1])
{
    while (*texto == ' ' || *texto == '\t' || *texto == '\r') {
        texto++;
    }
    unsigned longitud = 0, corte = 0;
    const char *espacio = NULL;
    while (*texto && *texto != '\n' && longitud < OLED_COLUMNAS) {
        char c = leer_caracter(&texto);
        if (c == ' ') {
            espacio = texto;
            corte = longitud;
        }
        salida[longitud++] = c;
    }
    if (longitud == OLED_COLUMNAS && *texto && *texto != '\n' && *texto != ' ' && corte > 0) {
        longitud = corte;
        texto = espacio;
    }
    while (longitud && salida[longitud - 1] == ' ') {
        longitud--;
    }
    salida[longitud] = 0;
    return *texto == '\n' ? texto + 1 : texto;
}

unsigned tema_letra(const char *letra, unsigned pagina, oled_vista_t *vista)
{
    unsigned filas = 0;
    char fila[OLED_COLUMNAS + 1];
    while (*letra) {
        if (strncmp(letra, "[length:", 8) == 0) {
            const char *fin = strchr(letra, '\n');
            if (!fin) break;
            letra = fin + 1;
            continue;
        }
        letra = leer_fila(letra, fila);
        if (vista && filas / LETRA_FILAS == pagina) {
            strcpy(vista->texto[LETRA_PRIMERA_FILA + filas % LETRA_FILAS], fila);
        }
        filas++;
    }
    return (filas + LETRA_FILAS - 1) / LETRA_FILAS;
}

static bool es_digito(char c)
{
    return c >= '0' && c <= '9';
}

static const char *leer_tiempo(const char *marca, unsigned *tiempo_ms)
{
    unsigned minutos = 0, digitos = 0;
    while (es_digito(*marca) && digitos < 3) {
        minutos = minutos * 10 + (*marca++ - '0');
        digitos++;
    }
    if (!digitos || *marca++ != ':' || *marca < '0' || *marca > '5') {
        return NULL;
    }
    unsigned segundos = (*marca++ - '0') * 10;
    if (!es_digito(*marca)) {
        return NULL;
    }
    segundos += *marca++ - '0';
    unsigned fraccion = 0;
    if (*marca == '.' || *marca == ':') {
        marca++;
        if (!es_digito(*marca)) {
            return NULL;
        }
        for (unsigned peso = 100; peso && es_digito(*marca); peso /= 10) {
            fraccion += (*marca++ - '0') * peso;
        }
    }
    if (*marca != ']') {
        return NULL;
    }
    *tiempo_ms = (minutos * 60 + segundos) * 1000 + fraccion;
    return marca + 1;
}

static const char *leer_marca(const char *marca, unsigned *tiempo_ms)
{
    return *marca == '[' ? leer_tiempo(marca + 1, tiempo_ms) : NULL;
}

unsigned tema_duracion_ms(const char *letra)
{
    if (strncmp(letra, "\xEF\xBB\xBF", 3) == 0) {
        letra += 3;
    }
    const char *tag = letra;
    while ((tag = strstr(tag, "[length:")) != NULL) {
        unsigned ms;
        if ((tag == letra || tag[-1] == '\n') && leer_tiempo(tag + 8, &ms) && ms >= 1000) {
            return ms;
        }
        tag += 8;
    }
    return 0;
}

bool tema_letra_lrc(const char *letra, int64_t tiempo_ms, oled_vista_t *vista)
{
    if (strncmp(letra, "\xEF\xBB\xBF", 3) == 0) {
        letra += 3;
    }
    const char *offset = strstr(letra, "[offset:");
    if (offset && (offset == letra || offset[-1] == '\n')) {
        char *fin;
        long ajuste = strtol(offset + 8, &fin, 10);
        if (fin != offset + 8 && *fin == ']' && ajuste >= -300000 && ajuste <= 300000) {
            tiempo_ms += ajuste;
        }
    }

    const char *verso = NULL;
    unsigned mejor = 0, proxima = UINT_MAX;
    bool sincronizada = false;
    for (const char *fila = letra; *fila;) {
        const char *p = fila;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        unsigned marca, candidata = 0;
        bool tiene_candidata = false;
        const char *siguiente;
        while ((siguiente = leer_marca(p, &marca)) != NULL) {
            if (!vista) {
                return true;
            }
            sincronizada = true;
            if ((int64_t)marca <= tiempo_ms && (!tiene_candidata || marca >= candidata)) {
                tiene_candidata = true;
                candidata = marca;
            } else if ((int64_t)marca > tiempo_ms && marca < proxima) {
                proxima = marca;
            }
            p = siguiente;
        }
        if (tiene_candidata && (!verso || candidata >= mejor)) {
            mejor = candidata;
            verso = p;
        }
        const char *fin = strchr(p, '\n');
        if (!fin) {
            break;
        }
        fila = fin + 1;
    }

    if (verso) {
        const char *fin = strchr(verso, '\n');
        char texto[OLED_COLUMNAS + 1];
        unsigned filas = 0;
        for (const char *p = verso; *p && (!fin || p < fin); filas++) {
            p = leer_fila(p, texto);
        }
        unsigned paginas = (filas + LETRA_FILAS - 1) / LETRA_FILAS;
        int64_t duracion = proxima == UINT_MAX ? 3500LL * paginas : proxima - mejor;
        unsigned pagina = duracion > 0 ? (unsigned)((tiempo_ms - mejor) * paginas / duracion) : 0;
        if (paginas && pagina >= paginas) {
            pagina = paginas - 1;
        }
        for (unsigned i = 0; i < filas && i < (pagina + 1) * LETRA_FILAS; i++) {
            verso = leer_fila(verso, texto);
            if (i / LETRA_FILAS == pagina) {
                strcpy(vista->texto[LETRA_PRIMERA_FILA + i % LETRA_FILAS], texto);
            }
        }
    }
    return sincronizada;
}

void tema_texto(const char *texto, const char *alternativo, int64_t pasos,
                char salida[OLED_COLUMNAS + 1])
{
    if (!*texto) {
        texto = alternativo;
    }
    char linea[TEMA_TEXTO + 1];
    unsigned largo = 0;
    while (*texto && largo < TEMA_TEXTO) {
        linea[largo++] = leer_caracter(&texto);
    }
    if (largo <= OLED_COLUMNAS) {
        memcpy(salida, linea, largo);
        salida[largo] = 0;
        return;
    }
    unsigned offset = pasos % (largo + 4);
    for (unsigned i = 0; i < OLED_COLUMNAS; i++) {
        unsigned indice = (offset + i) % (largo + 4);
        salida[i] = indice < largo ? linea[indice] : ' ';
    }
    salida[OLED_COLUMNAS] = 0;
}
