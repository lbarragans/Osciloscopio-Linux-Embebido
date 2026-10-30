#include "scope_link.h"

#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adc_capture.h"
#include "board_config.h"
#include "scope_proto.h"

/* ============================================================
 * OSCILOSCOPIO ESP32-C6
 *
 * SPI esclavo + ADC continuo
 *
 * Version de diagnostico:
 * - Registra transacciones SPI
 * - Muestra los comandos recibidos
 * - Verifica el numero de bytes
 * - Registra las respuestas preparadas
 * - Conserva el protocolo original
 *
 * BeaglePlay: maestro SPI
 * ESP32-C6: esclavo SPI
 *
 * SPI modo 0
 * ============================================================ */

static const char *TAG = "scope_link";

#define SPI_HOST SPI2_HOST

/* ============================================================
 * CONFIGURACION DEL OSCILOSCOPIO
 * ============================================================ */

static uint8_t cfg_rate_idx = 1;

static uint16_t cfg_window = SCOPE_DEFAULT_WINDOW;

static bool cfg_test_mode = false;

/* ============================================================
 * ESTADO DEL SISTEMA
 * ============================================================ */

static _Atomic uint8_t state = SCOPE_ST_IDLE;

static volatile bool abort_capture = false;

static bool last_overrun = false;

static uint8_t last_err_flags = 0;

static uint16_t seq = 0;

/* ============================================================
 * BUFFERS
 * ============================================================ */

static uint16_t *cap_a = NULL;
static uint16_t *cap_b = NULL;

static uint8_t *tx_buf = NULL;
static uint8_t *rx_buf = NULL;

static TaskHandle_t capture_task = NULL;

/* ============================================================
 * DIAGNOSTICOS
 * ============================================================ */

static uint32_t spi_transaction_count = 0;

static uint32_t spi_error_count = 0;

/* ============================================================
 * HANDSHAKE
 *
 * GPIO3 = 1: ESP32 preparado para transaccion
 * GPIO3 = 0: ESP32 procesando/rearmando
 * ============================================================ */

static void IRAM_ATTR post_setup_cb(
    spi_slave_transaction_t *trans)
{
    (void)trans;

    gpio_set_level(PIN_HANDSHAKE, 1);
}

static void IRAM_ATTR post_trans_cb(
    spi_slave_transaction_t *trans)
{
    (void)trans;

    gpio_set_level(PIN_HANDSHAKE, 0);
}

/* ============================================================
 * CAMBIAR ESTADO
 * ============================================================ */

static void set_state(scope_state_t s)
{
    atomic_store(&state, (uint8_t)s);
}

/* ============================================================
 * PREPARAR RESPUESTA SPI
 *
 * La respuesta se prepara antes de la transaccion.
 *
 * El primer byte debe ser 0xE5.
 * ============================================================ */

static void prepare_tx(void)
{
    uint8_t st = atomic_load(&state);

    scope_header_t h = {
        .state = st,

        .flags =
            last_err_flags |
            (last_overrun ? SCOPE_FLAG_OVERRUN : 0) |
            (cfg_test_mode ? SCOPE_FLAG_TEST_MODE : 0),

        .rate_idx = cfg_rate_idx,

        .window = cfg_window,

        .seq = seq,

        .fs_hz = scope_rate_get(cfg_rate_idx)->fs_hz,

        .n_channels = SCOPE_NUM_CHANNELS,

        .bits = SCOPE_ADC_BITS,

        .fw_version = SCOPE_PROTO_VERSION,
    };

    /* Limpiar buffer para evitar datos residuales */

    memset(tx_buf, 0, SCOPE_MAX_FRAME);

    /* Preparar cabecera */

    scope_header_pack(tx_buf, &h);

    size_t bytes =
        (size_t)cfg_window * sizeof(uint16_t);

    /* Si existe captura lista, copiar muestras */

    if (st == SCOPE_ST_READY) {

        memcpy(
            tx_buf + SCOPE_HDR_LEN,
            cap_a,
            bytes
        );

        memcpy(
            tx_buf + SCOPE_HDR_LEN + bytes,
            cap_b,
            bytes
        );
    }

    ESP_LOGD(
        TAG,
        "TX preparado: magic=0x%02X estado=%u",
        tx_buf[0],
        (unsigned)st
    );
}

/* ============================================================
 * RECHAZAR COMANDO
 * ============================================================ */

static void reject(void)
{
    last_err_flags |= SCOPE_FLAG_CMD_REJECTED;

    ESP_LOGW(TAG, "Comando SPI rechazado");
}

/* ============================================================
 * PROCESAR COMANDO SPI
 * ============================================================ */

static void handle_command(
    const uint8_t *rx,
    size_t len)
{
    scope_cmd_frame_t f;

    if (len < SCOPE_CMD_LEN) {

        ESP_LOGW(
            TAG,
            "Comando incompleto: %u bytes",
            (unsigned)len
        );

        return;
    }

    if (!scope_cmd_decode(rx, len, &f)) {

        last_err_flags = SCOPE_FLAG_BAD_CMD_CHK;

        ESP_LOGW(
            TAG,
            "Checksum de comando incorrecto"
        );

        return;
    }

    /* NOP: consultar estado */

    if (f.cmd == SCOPE_CMD_NOP) {

        ESP_LOGI(TAG, "Comando NOP recibido");

        return;
    }

    uint8_t st = atomic_load(&state);

    bool busy = (st == SCOPE_ST_ARMED);

    last_err_flags = 0;

    switch (f.cmd) {

    case SCOPE_CMD_SET_RATE:

        ESP_LOGI(
            TAG,
            "SET_RATE: %u",
            (unsigned)f.arg
        );

        if (
            busy ||
            scope_rate_get((uint8_t)f.arg) == NULL
        ) {
            reject();
        } else {

            cfg_rate_idx = (uint8_t)f.arg;

            set_state(SCOPE_ST_IDLE);
        }

        break;

    case SCOPE_CMD_SET_WINDOW:

        ESP_LOGI(
            TAG,
            "SET_WINDOW: %u",
            (unsigned)f.arg
        );

        if (
            busy ||
            !scope_window_valid(f.arg)
        ) {
            reject();
        } else {

            cfg_window = f.arg;

            set_state(SCOPE_ST_IDLE);
        }

        break;

    case SCOPE_CMD_SET_TESTMODE:

        ESP_LOGI(
            TAG,
            "SET_TESTMODE: %u",
            (unsigned)f.arg
        );

        if (busy || f.arg > 1) {

            reject();

        } else {

            cfg_test_mode = (f.arg == 1);

            set_state(SCOPE_ST_IDLE);
        }

        break;

    case SCOPE_CMD_ARM:

        ESP_LOGI(TAG, "ARM: iniciar captura");

        if (busy) {

            reject();

        } else {

            abort_capture = false;

            last_overrun = false;

            set_state(SCOPE_ST_ARMED);

            xTaskNotifyGive(capture_task);
        }

        break;

    case SCOPE_CMD_RESET:

        ESP_LOGI(TAG, "RESET recibido");

        if (busy) {

            abort_capture = true;

        } else {

            set_state(SCOPE_ST_IDLE);
        }

        break;

    default:

        ESP_LOGW(
            TAG,
            "Comando desconocido: 0x%02X",
            f.cmd
        );

        reject();

        break;
    }
}

/* ============================================================
 * TAREA SPI
 *
 * Esta funcion permite identificar si la BeaglePlay
 * realmente esta comunicandose con el ESP32-C6.
 * ============================================================ */

static void spi_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "================================");

    ESP_LOGI(TAG, "TAREA SPI INICIADA");

    ESP_LOGI(TAG, "ESP32-C6 EN MODO ESCLAVO");

    ESP_LOGI(TAG, "================================");

    for (;;) {

        /* Preparar respuesta */

        prepare_tx();

        /* Limpiar buffer de recepcion */

        memset(rx_buf, 0, SCOPE_MAX_FRAME);

        /* Configurar transaccion */

        spi_slave_transaction_t t = {
            .length = SCOPE_MAX_FRAME * 8,
            .tx_buffer = tx_buf,
            .rx_buffer = rx_buf,
        };

        ESP_LOGI(
            TAG,
            "Esperando maestro SPI. TX magic=0x%02X",
            tx_buf[0]
        );

        /* Esperar transaccion del maestro */

        esp_err_t err = spi_slave_transmit(
            SPI_HOST,
            &t,
            portMAX_DELAY
        );

        if (err == ESP_OK) {

            spi_transaction_count++;

            size_t nbytes = t.trans_len / 8;

            ESP_LOGI(
                TAG,
                "TRANSACCION SPI #%lu",
                (unsigned long)spi_transaction_count
            );

            ESP_LOGI(
                TAG,
                "Bytes recibidos: %u",
                (unsigned)nbytes
            );

            /* Mostrar los primeros bytes */

            if (nbytes >= 4) {

                ESP_LOGI(
                    TAG,
                    "RX: %02X %02X %02X %02X",
                    rx_buf[0],
                    rx_buf[1],
                    rx_buf[2],
                    rx_buf[3]
                );

                ESP_LOGI(
                    TAG,
                    "TX: %02X %02X %02X %02X",
                    tx_buf[0],
                    tx_buf[1],
                    tx_buf[2],
                    tx_buf[3]
                );
            }

            /* Ejecutar comando recibido */

            handle_command(rx_buf, nbytes);

        } else {

            spi_error_count++;

            ESP_LOGE(
                TAG,
                "ERROR SPI #%lu: %s",
                (unsigned long)spi_error_count,
                esp_err_to_name(err)
            );
        }
    }
}

/* ============================================================
 * TAREA DE ADQUISICION ADC
 * ============================================================ */

static void capture_task_fn(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Tarea ADC iniciada");

    for (;;) {

        ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        const scope_rate_t *rate =
            scope_rate_get(cfg_rate_idx);

        esp_err_t err;

        bool overrun = false;

        ESP_LOGI(
            TAG,
            "Iniciando captura: %u muestras/canal",
            (unsigned)cfg_window
        );

        if (cfg_test_mode) {

            ESP_LOGI(
                TAG,
                "Modo prueba: generando datos sinteticos"
            );

            capture_fill_test_pattern(
                cfg_window,
                cap_a,
                cap_b
            );

            err = ESP_OK;

        } else {

            err = capture_run(
                rate,
                cfg_window,
                cap_a,
                cap_b,
                &overrun,
                &abort_capture
            );
        }

        last_overrun = overrun;

        if (err == ESP_OK) {

            seq++;

            set_state(SCOPE_ST_READY);

            ESP_LOGI(
                TAG,
                "Captura lista. Secuencia=%u",
                (unsigned)seq
            );

        } else if (err == ESP_ERR_INVALID_STATE) {

            set_state(SCOPE_ST_IDLE);

            ESP_LOGW(
                TAG,
                "Captura cancelada"
            );

        } else {

            set_state(SCOPE_ST_ERROR);

            ESP_LOGE(
                TAG,
                "Error ADC: %s",
                esp_err_to_name(err)
            );
        }
    }
}

/* ============================================================
 * INICIALIZACION
 * ============================================================ */

esp_err_t scope_link_start(void)
{
    ESP_LOGI(TAG, "Inicializando osciloscopio");

    /* Reservar buffers de captura */

    cap_a = heap_caps_calloc(
        SCOPE_MAX_WINDOW,
        sizeof(uint16_t),
        MALLOC_CAP_INTERNAL
    );

    cap_b = heap_caps_calloc(
        SCOPE_MAX_WINDOW,
        sizeof(uint16_t),
        MALLOC_CAP_INTERNAL
    );

    /* Buffers SPI con capacidad DMA */

    tx_buf = heap_caps_calloc(
        1,
        SCOPE_MAX_FRAME,
        MALLOC_CAP_DMA
    );

    rx_buf = heap_caps_calloc(
        1,
        SCOPE_MAX_FRAME,
        MALLOC_CAP_DMA
    );

    ESP_RETURN_ON_FALSE(
        cap_a && cap_b && tx_buf && rx_buf,
        ESP_ERR_NO_MEM,
        TAG,
        "Error reservando memoria"
    );

    /* Configurar GPIO handshake */

    gpio_config_t hs = {
        .pin_bit_mask = 1ULL << PIN_HANDSHAKE,
        .mode = GPIO_MODE_OUTPUT,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&hs),
        TAG,
        "Error configurando handshake"
    );

    gpio_set_level(PIN_HANDSHAKE, 0);

    /* Configurar bus SPI */

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCOPE_MAX_FRAME,
    };

    /* Configurar ESP32-C6 como esclavo */

    spi_slave_interface_config_t slv = {
        .mode = 0,
        .spics_io_num = PIN_SPI_CS,
        .queue_size = 1,
        .flags = 0,
        .post_setup_cb = post_setup_cb,
        .post_trans_cb = post_trans_cb,
    };

    ESP_LOGI(TAG, "Inicializando SPI2");

    ESP_RETURN_ON_ERROR(
        spi_slave_initialize(
            SPI_HOST,
            &bus,
            &slv,
            SPI_DMA_CH_AUTO
        ),
        TAG,
        "Error inicializando SPI"
    );

    /* Crear tarea de captura primero */

    BaseType_t ok = xTaskCreate(
        capture_task_fn,
        "capture",
        4096,
        NULL,
        6,
        &capture_task
    );

    ESP_RETURN_ON_FALSE(
        ok == pdPASS,
        ESP_ERR_NO_MEM,
        TAG,
        "No se pudo crear tarea ADC"
    );

    /* Crear tarea de comunicacion SPI */

    ok = xTaskCreate(
        spi_task,
        "spi_link",
        4096,
        NULL,
        7,
        NULL
    );

    ESP_RETURN_ON_FALSE(
        ok == pdPASS,
        ESP_ERR_NO_MEM,
        TAG,
        "No se pudo crear tarea SPI"
    );

    ESP_LOGI(TAG, "================================");

    ESP_LOGI(TAG, "OSCILOSCOPIO ESP32-C6 LISTO");

    ESP_LOGI(
        TAG,
        "SPI: SCLK=%d MOSI=%d MISO=%d CS=%d",
        PIN_SPI_SCLK,
        PIN_SPI_MOSI,
        PIN_SPI_MISO,
        PIN_SPI_CS
    );

    ESP_LOGI(
        TAG,
        "HANDSHAKE GPIO=%d",
        PIN_HANDSHAKE
    );

    ESP_LOGI(TAG, "================================");

    return ESP_OK;
}
