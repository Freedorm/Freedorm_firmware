/*
 * SPDX-FileCopyrightText: 2017-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOSConfig.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble_prox_prph.h"

#if CONFIG_EXAMPLE_EXTENDED_ADV
static uint8_t ext_adv_pattern_1[] = {
    0x02,
    0x01,
    0x06,
    0x03,
    0x03,
    0xab,
    0xcd,
    0x03,
    0x03,
    0x18,
    0x03,
    0x13,
    0X09,
    'n',
    'i',
    'm',
    'b',
    'l',
    'e',
    '-',
    'p',
    'r',
    'o',
    'x',
    '-',
    'p',
    'r',
    'p',
    'h',
    '-',
    'e',
};
#endif

static const char *tag = "NimBLE_PROX_PRPH";
static const char *device_name = "Freedorm Lite";

static int ble_prox_prph_gap_event(struct ble_gap_event *event, void *arg);

static uint8_t ble_prox_prph_addr_type;

static uint16_t ble_prox_prph_conn_handle = -1;

static void rssi_monitor_task(void *param);

static void kill_the_connection_after_10s();

static TaskHandle_t rssi_task_handle = NULL; // 全局变量存储任务句柄

/**
 * Utility function to log an array of bytes.
 */
void print_bytes(const uint8_t *bytes, int len)
{
    int i;
    for (i = 0; i < len; i++)
    {
        MODLOG_DFLT(INFO, "%s0x%02x", i != 0 ? ":" : "", bytes[i]);
    }
}

void print_addr(const void *addr)
{
    const uint8_t *u8p;

    u8p = addr;
    MODLOG_DFLT(INFO, "%02x:%02x:%02x:%02x:%02x:%02x",
                u8p[5], u8p[4], u8p[3], u8p[2], u8p[1], u8p[0]);
}

#if CONFIG_EXAMPLE_EXTENDED_ADV
/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void
ext_ble_prox_prph_advertise(void)
{
    struct ble_gap_ext_adv_params params;
    struct os_mbuf *data;
    uint8_t instance = 0;
    int rc;

    /* First check if any instance is already active */
    if (ble_gap_ext_adv_active(instance))
    {
        return;
    }

    /* use defaults for non-set params */
    memset(&params, 0, sizeof(params));

    /* enable connectable advertising */
    params.connectable = 1;

    /* advertise using random addr */
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;

    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_2M;
    params.sid = 1;

    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MIN;

    /* configure instance 0 */
    rc = ble_gap_ext_adv_configure(instance, &params, NULL,
                                   ble_prox_prph_gap_event, NULL);
    assert(rc == 0);

    /* in this case only scan response is allowed */

    /* get mbuf for scan rsp data */
    data = os_msys_get_pkthdr(sizeof(ext_adv_pattern_1), 0);
    assert(data);

    /* fill mbuf with scan rsp data */
    rc = os_mbuf_append(data, ext_adv_pattern_1, sizeof(ext_adv_pattern_1));
    assert(rc == 0);

    rc = ble_gap_ext_adv_set_data(instance, data);
    assert(rc == 0);

    /* start advertising */
    rc = ble_gap_ext_adv_start(instance, 0, 0);
    assert(rc == 0);
}
#else

static void
ble_prox_prph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    /*
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info)
     *     o Advertising tx power
     *     o Device name
     */
    memset(&fields, 0, sizeof(fields));

    /*
     * Advertise two flags:
     *      o Discoverability in forthcoming advertisement (general)
     *      o BLE-only (BR/EDR unsupported)
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /*
     * Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]){
        BLE_UUID16_INIT(BLE_SVC_LINK_LOSS_UUID16)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return;
    }

    /* Begin advertising */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(ble_prox_prph_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_prox_prph_gap_event, NULL);
    if (rc != 0)
    {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
        return;
    }
}
#endif

static int
ble_prox_prph_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed */
        MODLOG_DFLT(INFO, "connection %s; status=%d\n",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        ble_prox_prph_conn_handle = event->connect.conn_handle;
        xTaskCreate(rssi_monitor_task, "rssi_monitor_task", 2048, &ble_prox_prph_conn_handle, 5, &rssi_task_handle);
        // xTaskCreate(kill_the_connection_after_10s, "kill_the_connection_after_10s", 2048, NULL, 5, NULL);
        /* resume advertising */
#if CONFIG_EXAMPLE_EXTENDED_ADV
        ext_ble_prox_prph_advertise();
#else
        ble_prox_prph_advertise();
#endif
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%#04x\n", event->disconnect.reason);
        vTaskDelete(rssi_task_handle);

        /* Connection terminated; resume advertising */
#if CONFIG_EXAMPLE_EXTENDED_ADV
        ext_ble_prox_prph_advertise();
#else
        ble_prox_prph_advertise();
#endif
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "adv complete\n");
#if CONFIG_EXAMPLE_EXTENDED_ADV
        ext_ble_prox_prph_advertise();
#else
        ble_prox_prph_advertise();
#endif
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        MODLOG_DFLT(INFO, "subscribe event; cur_notify=%d\n value handle; "
                          "val_handle=%d\n",
                    event->subscribe.cur_notify, event->subscribe.attr_handle);
        break;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.value);
        break;
    }

    return 0;
}

static void
ble_prox_prph_on_sync(void)
{
    int rc;

    rc = ble_hs_id_infer_auto(0, &ble_prox_prph_addr_type);
    assert(rc == 0);

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(ble_prox_prph_addr_type, addr_val, NULL);

    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");

    /* Begin advertising */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    ext_ble_prox_prph_advertise();
#else
    ble_prox_prph_advertise();
#endif
}

static void
ble_prox_prph_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

void ble_prox_prph_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

static void rssi_monitor_task(void *param)
{
    uint16_t conn_handle = *(uint16_t *)param; // 传递连接句柄
    int8_t measured_rssi = 0;

    while (1)
    {
        // 查询并打印 RSSI
        ble_gap_conn_rssi(conn_handle, &measured_rssi);
        MODLOG_DFLT(INFO, "For connection %d, RSSI: %d dBm\n", conn_handle, measured_rssi);

        // 每隔1秒更新一次
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void
kill_the_connection_after_10s()
{
    vTaskDelay(pdMS_TO_TICKS(10000));
    int rc = ble_gap_terminate(ble_prox_prph_conn_handle, BLE_ERR_CONN_TERM_LOCAL);
    if (rc == 0)
    {
        MODLOG_DFLT(INFO, "Successfully initiated disconnection\n");
    }
    else
    {
        MODLOG_DFLT(ERROR, "Failed to disconnect, error code: %d\n", rc);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    int rc;

    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK)
    {
        MODLOG_DFLT(ERROR, "Failed to init nimble %d \n", ret);
        return;
    }

    /* Initialize a task to keep checking path loss of the link */
    ble_svc_prox_init();

    /* Initialize the NimBLE host configuration */
    ble_hs_cfg.sync_cb = ble_prox_prph_on_sync;
    ble_hs_cfg.reset_cb = ble_prox_prph_on_reset;

    /* Enable bonding */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    // ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    // ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;

    /* Set the default device name */
    rc = ble_svc_gap_device_name_set(device_name);
    assert(rc == 0);

    /* Start the task */
    nimble_port_freertos_init(ble_prox_prph_host_task);
}
