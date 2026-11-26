#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "control_ble.h"
#include "oled.h"
#include "temas.h"

static const char *TAG = "REPRODUCTOR";
#define VOLUMEN_INICIAL 5
#define VOLUMEN_MAXIMO 10
#define PIN_BUSY GPIO_NUM_27
#define RED_WIFI "Reproductor-MP3"
#define CLAVE_WIFI CONFIG_MP3_WIFI_PASSWORD

typedef enum { LISTO, CARGANDO, REPRODUCIENDO, PAUSADO, FINALIZADO, SIN_PISTA } estado_t;

typedef struct {
    unsigned album;
    unsigned pista;
    unsigned volumen;
    estado_t estado;
    unsigned pagina;
} reproductor_t;

static const char *const estados[] = {
    [LISTO] = "LISTO",
    [CARGANDO] = "CARGANDO",
    [REPRODUCIENDO] = "REPRODUCIENDO",
    [PAUSADO] = "EN PAUSA",
    [FINALIZADO] = "FIN DE LISTA",
    [SIN_PISTA] = "SIN CONFIRMAR",
};

static const char *const estados_ble[] = {
    [LISTO] = "LISTO",
    [CARGANDO] = "CARGA",
    [REPRODUCIENDO] = "SONANDO",
    [PAUSADO] = "PAUSA",
    [FINALIZADO] = "FIN",
    [SIN_PISTA] = "ESTIMA",
};

static bool oled_lista;
static SemaphoreHandle_t bloqueo;
static reproductor_t reproductor = {.album = 1, .pista = 1, .volumen = VOLUMEN_INICIAL, .estado = LISTO};
static tema_t tema_actual;
static int64_t inicio_pista_us;
static int64_t inicio_letra_us;
static int64_t posicion_guardada_us;
static int64_t inicio_busy_us;
static int64_t cambio_pista_us;
static unsigned muestras_inactivas;
static bool busy_estuvo_alto;
static int64_t ultimo_comando_us;

extern const char pagina[] asm("_binary_control_html_start");

static void df_enviar(uint8_t comando, uint16_t dato)
{
    // El DFPlayer necesita una pausa entre órdenes consecutivas.
    int64_t espera_us = ultimo_comando_us + 100000 - esp_timer_get_time();
    if (espera_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((espera_us + 999) / 1000) + 1);
    }
    uint8_t trama[] = {
        0x7E, 0xFF, 0x06, comando, 0x00, dato >> 8, dato & 0xFF, 0, 0, 0xEF
    };
    uint16_t suma = 0;
    for (int i = 1; i <= 6; i++) {
        suma += trama[i];
    }
    uint16_t checksum = (uint16_t)(0u - suma);
    trama[7] = checksum >> 8;
    trama[8] = checksum & 0xFF;
    if (uart_write_bytes(UART_NUM_2, trama, sizeof(trama)) != sizeof(trama)) {
        ESP_LOGE(TAG, "No se pudo enviar el comando 0x%02X", comando);
    }
    ultimo_comando_us = esp_timer_get_time();
}

static bool reproduccion_activa(estado_t estado)
{
    return estado == CARGANDO || estado == REPRODUCIENDO || estado == SIN_PISTA;
}

static int64_t posicion_letra_us(int64_t ahora)
{
    if (!reproduccion_activa(reproductor.estado) || ahora < inicio_letra_us) {
        return posicion_guardada_us;
    }
    return posicion_guardada_us + ahora - inicio_letra_us;
}

static void preparar_oled(reproductor_t actual, oled_vista_t *vista)
{
    const tema_t *tema = &tema_actual;
    memset(vista, 0, sizeof(*vista));
    char pista[20], pie[64];
    int64_t ahora = esp_timer_get_time();
    int64_t transcurrido = ahora - cambio_pista_us;
    int64_t paso = transcurrido > 1600000 ? (transcurrido - 1600000) / 420000 : 0;
    int64_t tiempo_ms = posicion_letra_us(ahora) / 1000;
    snprintf(pista, sizeof(pista), "PISTA %03u", actual.pista);
    tema_texto(tema->titulo, pista, paso, vista->texto[0]);
    tema_texto(tema->artista, "", paso, vista->texto[1]);

    if (tema_letra_lrc(tema->letra, tiempo_ms, vista)) {
        if (!reproduccion_activa(actual.estado)) {
            snprintf(pie, sizeof(pie), "P%02u V%02u %s", actual.pista, actual.volumen, estados[actual.estado]);
        } else {
            unsigned segundos = (unsigned)(tiempo_ms / 1000);
            snprintf(pie, sizeof(pie), "P%02u V%02u %02u:%02u",
                     actual.pista, actual.volumen, segundos / 60, segundos % 60);
        }
    } else {
        unsigned total = tema_letra(tema->letra, actual.pagina, vista);
        if (total) {
            snprintf(pie, sizeof(pie), "P%02u V%02u L%u/%u",
                     actual.pista, actual.volumen, actual.pagina + 1, total);
        } else {
            strcpy(vista->texto[2], "LETRA SIN CARGAR");
            strcpy(vista->texto[3], "ABRE EL CONTROL WEB");
            snprintf(pie, sizeof(pie), "P%02u V%02u %s", actual.pista, actual.volumen, estados[actual.estado]);
        }
    }
    snprintf(vista->texto[OLED_FILAS - 1], sizeof(vista->texto[0]), "%.21s", pie);
}

static void esperar_arranque(int64_t retardo_us)
{
    inicio_pista_us = esp_timer_get_time();
    inicio_letra_us = inicio_pista_us + retardo_us;
    inicio_busy_us = 0;
    busy_estuvo_alto = gpio_get_level(PIN_BUSY) != 0;
    reproductor.estado = CARGANDO;
}

static void seleccionar_pista(unsigned pista)
{
    reproductor.pista = pista;
    reproductor.pagina = 0;
    reproductor.estado = LISTO;
    posicion_guardada_us = 0;
    cambio_pista_us = esp_timer_get_time();
    tema_cargar(reproductor.album, pista, &tema_actual);
}

static void reproducir_pista(unsigned pista)
{
    df_enviar(0x0F, (reproductor.album << 8) | pista);
    seleccionar_pista(pista);
    esperar_arranque(250000);
    ESP_LOGI(TAG, "Abriendo /%02u/%03u.mp3", reproductor.album, pista);
}

static void finalizar_pista(int64_t ahora, const char *motivo)
{
    ESP_LOGI(TAG, "Fin pista %u (%s)", reproductor.pista, motivo);
    if (reproductor.pista < albumes[reproductor.album - 1].pistas) {
        reproducir_pista(reproductor.pista + 1);
    } else {
        posicion_guardada_us = posicion_letra_us(ahora);
        df_enviar(0x16, 0);
        reproductor.estado = FINALIZADO;
    }
}

static void actualizar_reproduccion(void)
{
    if (!reproduccion_activa(reproductor.estado)) {
        return;
    }
    bool ocupado = gpio_get_level(PIN_BUSY) == 0;
    int64_t ahora = esp_timer_get_time();
    if (reproductor.estado == CARGANDO || reproductor.estado == SIN_PISTA) {
        int64_t transcurrido = ahora - inicio_pista_us;
        if (!ocupado) {
            busy_estuvo_alto = true;
            inicio_busy_us = 0;
        } else if (busy_estuvo_alto && !inicio_busy_us) {
            inicio_busy_us = ahora;
        }
        // Tras un salto, BUSY puede seguir indicando la pista anterior un instante.
        if (ocupado && transcurrido >= 300000) {
            if (inicio_busy_us && transcurrido <= 2000000) {
                inicio_letra_us = inicio_busy_us;
            }
            reproductor.estado = REPRODUCIENDO;
            muestras_inactivas = 0;
            ESP_LOGI(TAG, "Reproduciendo pista %u", reproductor.pista);
        } else if (reproductor.estado == CARGANDO && transcurrido >= 1500000) {
            ESP_LOGW(TAG, "BUSY no confirmo la pista %u", reproductor.pista);
            reproductor.estado = SIN_PISTA;
        }
    } else if (reproductor.estado == REPRODUCIENDO) {
        muestras_inactivas = ocupado ? 0 : muestras_inactivas + 1;
        if (muestras_inactivas >= 6) {
            finalizar_pista(ahora, "BUSY");
            return;
        }
    }
    unsigned duracion = tema_duracion_ms(tema_actual.letra);
    if (duracion && posicion_letra_us(ahora) >= (int64_t)duracion * 1000 + 600000) {
        finalizar_pista(ahora, "duracion guardada");
    }
}

static unsigned numero_parametro(const char *texto, unsigned maximo)
{
    unsigned valor = 0;
    while (*texto >= '0' && *texto <= '9') {
        valor = valor * 10 + (*texto++ - '0');
        if (valor > maximo) return 0;
    }
    return *texto ? 0 : valor;
}

static bool aplicar_control(const char *accion)
{
    unsigned total = albumes[reproductor.album - 1].pistas;
    if (strcmp(accion, "reproducir") == 0) {
        if (reproductor.estado == PAUSADO) {
            df_enviar(0x0D, 0);
            esperar_arranque(100000);
        } else if (reproductor.estado == LISTO || reproductor.estado == FINALIZADO) {
            reproducir_pista(reproductor.estado == FINALIZADO ? 1 : reproductor.pista);
        }
    } else if (strcmp(accion, "pausar") == 0) {
        if (reproductor.estado == REPRODUCIENDO || reproductor.estado == SIN_PISTA) {
            df_enviar(0x0E, 0);
            posicion_guardada_us = posicion_letra_us(esp_timer_get_time());
            reproductor.estado = PAUSADO;
        }
    } else if (strncmp(accion, "album_", 6) == 0) {
        unsigned album = numero_parametro(accion + 6, MAX_ALBUMES);
        if (!album || !albumes[album - 1].pistas) {
            return false;
        }
        if (album != reproductor.album) {
            if (reproduccion_activa(reproductor.estado) || reproductor.estado == PAUSADO) {
                df_enviar(0x16, 0);
            }
            reproductor.album = album;
            seleccionar_pista(1);
        }
    } else if (strcmp(accion, "siguiente") == 0 || strcmp(accion, "anterior") == 0 ||
               strncmp(accion, "pista_", 6) == 0) {
        unsigned pista;
        if (accion[0] == 's') {
            pista = reproductor.pista % total + 1;
        } else if (accion[0] == 'a') {
            pista = reproductor.pista == 1 ? total : reproductor.pista - 1;
        } else {
            pista = numero_parametro(accion + 6, total);
            if (!pista) {
                return false;
            }
            if (pista == reproductor.pista && reproductor.estado != FINALIZADO) {
                return true;
            }
        }
        if (reproduccion_activa(reproductor.estado)) {
            reproducir_pista(pista);
        } else {
            seleccionar_pista(pista);
        }
    } else if (strcmp(accion, "subir") == 0) {
        if (reproductor.volumen < VOLUMEN_MAXIMO) {
            df_enviar(0x06, ++reproductor.volumen);
        }
    } else if (strcmp(accion, "bajar") == 0) {
        if (reproductor.volumen > 0) {
            df_enviar(0x06, --reproductor.volumen);
        }
    } else if (strcmp(accion, "letra_ant") == 0) {
        if (reproductor.pagina > 0) {
            reproductor.pagina--;
        }
    } else if (strcmp(accion, "letra_sig") == 0) {
        const char *letra = tema_actual.letra;
        if (!tema_letra_lrc(letra, 0, NULL)) {
            unsigned total = tema_letra(letra, 0, NULL);
            if (reproductor.pagina + 1 < total) {
                reproductor.pagina++;
            }
        }
    } else {
        return false;
    }
    return true;
}

static void escapar_json(const char *texto, char *salida)
{
    while (*texto) {
        unsigned char c = *texto++;
        if (c == '"' || c == '\\') {
            *salida++ = '\\';
        }
        *salida++ = c < 32 ? ' ' : c;
    }
    *salida = 0;
}

static esp_err_t enviar_estado(httpd_req_t *req)
{
    xSemaphoreTake(bloqueo, portMAX_DELAY);
    reproductor_t estado = reproductor;
    int64_t tiempo_us = posicion_letra_us(esp_timer_get_time());
    const tema_t *tema = &tema_actual;
    char titulo[2 * TEMA_TEXTO + 1], artista[2 * TEMA_TEXTO + 1];
    char json[sizeof(titulo) + sizeof(artista) + 290];
    escapar_json(tema->titulo, titulo);
    escapar_json(tema->artista, artista);
    const char *letra = tema->letra;
    bool sincronizada = tema_letra_lrc(letra, 0, NULL);
    unsigned paginas = sincronizada ? 0 : tema_letra(letra, 0, NULL);
    unsigned duracion = tema_duracion_ms(letra);
    uint64_t posicion = tiempo_us / 1000;
    if (duracion && posicion > duracion) {
        posicion = duracion;
    }
    snprintf(json, sizeof(json),
             "{\"album\":%u,\"pista\":%u,\"total\":%u,\"volumen\":%u,\"maximo\":%d,\"estado\":\"%s\","
             "\"sincronizada\":%s,\"pagina\":%u,\"paginas\":%u,\"titulo\":\"%s\",\"artista\":\"%s\","
             "\"duracionMs\":%u,\"posicionMs\":%llu}",
             estado.album, estado.pista, (unsigned)albumes[estado.album - 1].pistas,
             estado.volumen, VOLUMEN_MAXIMO, estados[estado.estado],
             sincronizada ? "true" : "false", paginas ? estado.pagina + 1 : 0, paginas,
             titulo, artista, duracion, (unsigned long long)posicion);
    xSemaphoreGive(bloqueo);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t pagina_http(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, pagina);
}

static esp_err_t estado_http(httpd_req_t *req)
{
    return enviar_estado(req);
}

static esp_err_t control_http(httpd_req_t *req)
{
    char consulta[64], accion[16];
    if (req->content_len != 0 ||
        httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) != ESP_OK ||
        httpd_query_key_value(consulta, "accion", accion, sizeof(accion)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Accion invalida");
    }
    xSemaphoreTake(bloqueo, portMAX_DELAY);
    bool valida = aplicar_control(accion);
    xSemaphoreGive(bloqueo);
    if (!valida) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Accion invalida");
    }
    ESP_LOGI(TAG, "Control web: %s", accion);
    return enviar_estado(req);
}

static unsigned leer_numero_http(const char *consulta, const char *nombre, unsigned maximo)
{
    char valor[8];
    return httpd_query_key_value(consulta, nombre, valor, sizeof(valor)) == ESP_OK ?
           numero_parametro(valor, maximo) : 0;
}

static bool pista_http(httpd_req_t *req, unsigned *album, unsigned *pista)
{
    char consulta[48];
    if (httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) != ESP_OK) return false;
    *album = leer_numero_http(consulta, "album", MAX_ALBUMES);
    if (!*album || !albumes[*album - 1].pistas) return false;
    *pista = leer_numero_http(consulta, "pista", albumes[*album - 1].pistas);
    return *pista != 0;
}

static esp_err_t datos_pista_http(httpd_req_t *req)
{
    unsigned album, pista;
    if (!pista_http(req, &album, &pista)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Pista invalida");
    }
    tema_t *tema = malloc(sizeof(*tema));
    if (!tema) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
    }
    tema_cargar(album, pista, tema);
    char cabecera[2 * TEMA_TEXTO + 3];
    snprintf(cabecera, sizeof(cabecera), "%s\n%s\n", tema->titulo, tema->artista);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t resultado = httpd_resp_sendstr_chunk(req, cabecera);
    if (resultado == ESP_OK && *tema->letra) {
        resultado = httpd_resp_sendstr_chunk(req, tema->letra);
    }
    free(tema);
    return resultado == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : resultado;
}

static esp_err_t guardar_pista_http(httpd_req_t *req)
{
    unsigned album, pista;
    size_t tam = req->content_len;
    if (!pista_http(req, &album, &pista) || tam < 2 || tam > 2 * TEMA_TEXTO + TEMA_LETRA + 2) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Datos fuera de rango");
    }
    char *entrada = malloc(tam + 1);
    if (!entrada) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
    }
    size_t leido = 0;
    while (leido < tam) {
        int recibido = httpd_req_recv(req, entrada + leido, tam - leido);
        if (recibido <= 0) {
            free(entrada);
            return ESP_FAIL;
        }
        leido += recibido;
    }
    entrada[tam] = 0;
    char *artista = strchr(entrada, '\n');
    char *letra = artista ? strchr(artista + 1, '\n') : NULL;
    if (!letra || memchr(entrada, 0, tam) || artista - entrada > TEMA_TEXTO ||
        letra - artista - 1 > TEMA_TEXTO || strlen(letra + 1) > TEMA_LETRA) {
        free(entrada);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Datos invalidos o demasiado largos");
    }
    *artista++ = 0;
    *letra++ = 0;
    tema_t *tema = calloc(1, sizeof(*tema));
    if (!tema) {
        free(entrada);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
    }
    strcpy(tema->titulo, entrada);
    strcpy(tema->artista, artista);
    strcpy(tema->letra, letra);
    free(entrada);

    xSemaphoreTake(bloqueo, portMAX_DELAY);
    esp_err_t resultado = tema_guardar(album, pista, tema);
    if (resultado == ESP_OK && reproductor.album == album && reproductor.pista == pista) {
        tema_actual = *tema;
        reproductor.pagina = 0;
    }
    xSemaphoreGive(bloqueo);
    free(tema);
    if (resultado != ESP_OK) {
        ESP_LOGW(TAG, "No se guardo la pista %u: %s", pista, esp_err_to_name(resultado));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudieron guardar los datos");
    }
    return httpd_resp_sendstr(req, "Guardado");
}

static esp_err_t albumes_http(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t resultado = httpd_resp_sendstr_chunk(req, "[");
    bool primero = true;
    for (unsigned i = 0; i < MAX_ALBUMES && resultado == ESP_OK; i++) {
        const album_t *album = &albumes[i];
        if (!album->pistas) continue;
        char nombre[2 * ALBUM_NOMBRE + 1], json[2 * ALBUM_NOMBRE + 64];
        escapar_json(album->nombre, nombre);
        snprintf(json, sizeof(json), "%s{\"id\":%u,\"nombre\":\"%s\",\"pistas\":%u}",
                 primero ? "" : ",", i + 1, nombre, (unsigned)album->pistas);
        resultado = httpd_resp_sendstr_chunk(req, json);
        primero = false;
    }
    if (resultado == ESP_OK) resultado = httpd_resp_sendstr_chunk(req, "]");
    return resultado == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : resultado;
}

static esp_err_t guardar_album_http(httpd_req_t *req)
{
    char consulta[48];
    album_t datos = {0};
    size_t tam = req->content_len;
    if (!tam || tam > ALBUM_NOMBRE ||
        httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Datos de album invalidos");
    }
    unsigned album = leer_numero_http(consulta, "album", MAX_ALBUMES);
    unsigned pistas = leer_numero_http(consulta, "pistas", MAX_PISTAS);
    if (!album || !pistas) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Carpeta o cantidad invalida");
    }
    for (size_t leido = 0; leido < tam;) {
        int n = httpd_req_recv(req, datos.nombre + leido, tam - leido);
        if (n <= 0) return ESP_FAIL;
        leido += n;
    }
    for (size_t i = 0; i < tam; i++) {
        if ((unsigned char)datos.nombre[i] < 32) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nombre de album invalido");
        }
    }
    datos.pistas = pistas;
    xSemaphoreTake(bloqueo, portMAX_DELAY);
    esp_err_t resultado = album_guardar(album, &datos);
    if (resultado == ESP_OK && reproductor.album == album && reproductor.pista > pistas) {
        if (reproduccion_activa(reproductor.estado) || reproductor.estado == PAUSADO) {
            df_enviar(0x16, 0);
        }
        seleccionar_pista(1);
    }
    xSemaphoreGive(bloqueo);
    if (resultado != ESP_OK) {
        ESP_LOGW(TAG, "No se guardo el album %u: %s", album, esp_err_to_name(resultado));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo guardar el album");
    }
    return httpd_resp_sendstr(req, "Guardado");
}

static esp_err_t iniciar_control_web(void)
{
    esp_err_t resultado;
    if (sizeof(CLAVE_WIFI) < 9 || sizeof(CLAVE_WIFI) > 64) {
        ESP_LOGW(TAG, "Define una clave WPA2 de 8 a 63 caracteres en menuconfig");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Red local");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Eventos");
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), TAG, "Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Ajustes Wi-Fi");
    wifi_config_t config = {
        .ap = {
            .ssid = RED_WIFI,
            .password = CLAVE_WIFI,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "Modo punto de acceso");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "Configurar Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Encender Wi-Fi");

    httpd_handle_t servidor = NULL;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&servidor, &http), TAG, "Iniciar HTTP");
    const httpd_uri_t rutas[] = {
        {.uri = "/", .method = HTTP_GET, .handler = pagina_http},
        {.uri = "/estado", .method = HTTP_GET, .handler = estado_http},
        {.uri = "/control", .method = HTTP_POST, .handler = control_http},
        {.uri = "/tema", .method = HTTP_GET, .handler = datos_pista_http},
        {.uri = "/tema", .method = HTTP_POST, .handler = guardar_pista_http},
        {.uri = "/albumes", .method = HTTP_GET, .handler = albumes_http},
        {.uri = "/albumes", .method = HTTP_POST, .handler = guardar_album_http},
    };
    for (size_t i = 0; i < sizeof(rutas) / sizeof(rutas[0]); i++) {
        resultado = httpd_register_uri_handler(servidor, &rutas[i]);
        if (resultado != ESP_OK) {
            httpd_stop(servidor);
            return resultado;
        }
    }
    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap, &ip), TAG, "Obtener IP del reproductor");
    ESP_LOGI(TAG, "Control Wi-Fi: %s | http://" IPSTR, RED_WIFI, IP2STR(&ip.ip));
    return ESP_OK;
}

void app_main(void)
{
    const uart_config_t serial = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &serial));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, GPIO_NUM_17, UART_PIN_NO_CHANGE,
                                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 256, 0, 0, NULL, 0));
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&busy));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t pantalla = oled_init();
    oled_lista = pantalla == ESP_OK;
    if (oled_lista) {
        const oled_vista_t arranque = {.texto = {"PANTALLA OK", "INICIANDO"}};
        ESP_ERROR_CHECK_WITHOUT_ABORT(oled_show(&arranque));
    } else {
        ESP_LOGW(TAG, "OLED sin iniciar: %s", esp_err_to_name(pantalla));
    }

    vTaskDelay(pdMS_TO_TICKS(1200));
    df_enviar(0x0C, 0);  // Reinicio del DFPlayer.
    vTaskDelay(pdMS_TO_TICKS(2000));
    df_enviar(0x16, 0);
    df_enviar(0x06, VOLUMEN_INICIAL);
    vTaskDelay(pdMS_TO_TICKS(250));
    bloqueo = xSemaphoreCreateMutex();
    configASSERT(bloqueo != NULL);
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_OK) {
        esp_err_t catalogo = temas_cargar();
        if (catalogo == ESP_OK) {
            ESP_LOGI(TAG, "Catalogo de albumes disponible");
        } else {
            ESP_LOGW(TAG, "Catalogo no disponible: %s", esp_err_to_name(catalogo));
        }
    } else {
        ESP_LOGW(TAG, "No se pudo iniciar el almacenamiento: %s", esp_err_to_name(nvs));
    }
    seleccionar_pista(1);
    esp_err_t web = nvs == ESP_OK ? iniciar_control_web() : nvs;
    if (web != ESP_OK) {
        ESP_LOGW(TAG, "Control web no disponible: %s", esp_err_to_name(web));
    }
    esp_err_t ble = nvs == ESP_OK ? control_ble_iniciar() : nvs;
    if (ble != ESP_OK) {
        ESP_LOGW(TAG, "Control Bluetooth no disponible: %s", esp_err_to_name(ble));
    }
    ESP_LOGI(TAG, "OLED: titulo, artista y letra LRC");
    ESP_LOGI(TAG, "Listo: esperando Reproducir");
    oled_vista_t anterior = {0};
    for (;;) {
        char comando[COMANDO_BLE_TAM];
        xSemaphoreTake(bloqueo, portMAX_DELAY);
        if (control_ble_recibir(comando)) {
            if (aplicar_control(comando)) {
                ESP_LOGI(TAG, "Control Bluetooth: %s", comando);
            } else {
                ESP_LOGW(TAG, "Comando Bluetooth no válido: %s", comando);
            }
        }
        actualizar_reproduccion();
        reproductor_t actual = reproductor;
        oled_vista_t vista;
        if (oled_lista) {
            preparar_oled(actual, &vista);
        }
        xSemaphoreGive(bloqueo);
        control_ble_publicar(actual.album, actual.pista, actual.volumen, estados_ble[actual.estado]);

        if (oled_lista && memcmp(&vista, &anterior, sizeof(vista)) != 0) {
            esp_err_t resultado = oled_show(&vista);
            if (resultado == ESP_OK) {
                anterior = vista;
            } else {
                ESP_LOGW(TAG, "OLED sin respuesta: %s", esp_err_to_name(resultado));
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
