#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "control_ble.h"

static const char *TAG = "CONTROL_BLE";

// UUID de Nordic UART para que las apps BLE UART reconozcan el servicio.
static const ble_uuid128_t servicio_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t comando_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t estado_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static QueueHandle_t comandos;
static uint16_t estado_handle;
static atomic_bool servicio_listo;
static uint8_t tipo_direccion;
static portMUX_TYPE estado_mutex = portMUX_INITIALIZER_UNLOCKED;
static char estado_actual[21] = "A01 P001 V05 LISTO";

void ble_store_config_init(void);

static int acceder_comando(uint16_t conexion, uint16_t atributo,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char comando[COMANDO_BLE_TAM] = {0};
    uint16_t longitud = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, comando, sizeof(comando) - 1, &longitud) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    while (longitud && (comando[longitud - 1] == '\r' || comando[longitud - 1] == '\n')) {
        comando[--longitud] = 0;
    }
    if (!longitud) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    for (unsigned i = 0; i < longitud; i++) {
        char c = comando[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
    }
    return xQueueSend(comandos, comando, 0) == pdTRUE ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int acceder_estado(uint16_t conexion, uint16_t atributo,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char estado[sizeof(estado_actual)];
    portENTER_CRITICAL(&estado_mutex);
    memcpy(estado, estado_actual, sizeof(estado));
    portEXIT_CRITICAL(&estado_mutex);
    return os_mbuf_append(ctxt->om, estado, strlen(estado)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def caracteristicas[] = {
    {
        .uuid = &comando_uuid.u,
        .access_cb = acceder_comando,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = &estado_uuid.u,
        .access_cb = acceder_estado,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &estado_handle,
    },
    {0}
};

static const struct ble_gatt_svc_def servicios[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &servicio_uuid.u, .characteristics = caracteristicas},
    {0}
};

static void anunciar(void);

static int evento_gap(struct ble_gap_event *evento, void *arg)
{
    switch (evento->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (evento->connect.status) {
            anunciar();
        } else {
            ESP_LOGI(TAG, "Control Bluetooth conectado");
            ble_gap_security_initiate(evento->connect.conn_handle);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        xQueueReset(comandos);
        ESP_LOGI(TAG, "Control Bluetooth desconectado");
        anunciar();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (evento->enc_change.status == 0) {
            ESP_LOGI(TAG, "Enlace Bluetooth cifrado");
        } else {
            ESP_LOGW(TAG, "Emparejamiento incompleto: %d", evento->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        anunciar();
        break;
    default:
        break;
    }
    return 0;
}

static void anunciar(void)
{
    struct ble_hs_adv_fields primario = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &servicio_uuid,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields respuesta = {
        .name = (const uint8_t *)NOMBRE_CONTROL_BLE,
        .name_len = sizeof(NOMBRE_CONTROL_BLE) - 1,
        .name_is_complete = 1,
    };
    const struct ble_gap_adv_params parametros = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int rc = ble_gap_adv_set_fields(&primario);
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&respuesta);
    if (!rc) rc = ble_gap_adv_start(tipo_direccion, NULL, BLE_HS_FOREVER, &parametros, evento_gap, NULL);
    if (rc) {
        ESP_LOGW(TAG, "No se pudo anunciar Bluetooth: %d", rc);
    } else {
        ESP_LOGI(TAG, "Visible como %s", NOMBRE_CONTROL_BLE);
    }
}

static void sincronizar(void)
{
    int rc = ble_hs_id_infer_auto(0, &tipo_direccion);
    if (rc == 0) {
        atomic_store(&servicio_listo, true);
        anunciar();
    } else {
        ESP_LOGW(TAG, "No se pudo obtener la dirección Bluetooth: %d", rc);
    }
}

static void reinicio_ble(int motivo)
{
    atomic_store(&servicio_listo, false);
    ESP_LOGW(TAG, "Reinicio de Bluetooth: %d", motivo);
}

static void tarea_ble(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t control_ble_iniciar(void)
{
    comandos = xQueueCreate(6, COMANDO_BLE_TAM);
    if (!comandos) return ESP_ERR_NO_MEM;

    esp_err_t resultado = nimble_port_init();
    if (resultado == ESP_OK) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
        int rc = ble_svc_gap_device_name_set(NOMBRE_CONTROL_BLE);
        if (!rc) rc = ble_gatts_count_cfg(servicios);
        if (!rc) rc = ble_gatts_add_svcs(servicios);
        if (rc) {
            ESP_LOGW(TAG, "No se pudo configurar el servicio: %d", rc);
            nimble_port_deinit();
            resultado = ESP_FAIL;
        }
    }
    if (resultado != ESP_OK) {
        vQueueDelete(comandos);
        comandos = NULL;
        return resultado;
    }
    ble_hs_cfg.sync_cb = sincronizar;
    ble_hs_cfg.reset_cb = reinicio_ble;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    nimble_port_freertos_init(tarea_ble);
    return ESP_OK;
}

bool control_ble_recibir(char comando[COMANDO_BLE_TAM])
{
    return comandos && xQueueReceive(comandos, comando, 0) == pdTRUE;
}

void control_ble_publicar(unsigned album, unsigned pista, unsigned volumen, const char *estado)
{
    if (!comandos) return;

    char nuevo[sizeof(estado_actual)];
    snprintf(nuevo, sizeof(nuevo), "A%02u P%03u V%02u %.7s",
             album % 100, pista % 1000, volumen % 100, estado);
    portENTER_CRITICAL(&estado_mutex);
    bool cambio = strcmp(nuevo, estado_actual) != 0;
    if (cambio) strcpy(estado_actual, nuevo);
    portEXIT_CRITICAL(&estado_mutex);
    if (cambio && atomic_load(&servicio_listo)) ble_gatts_chr_updated(estado_handle);
}
