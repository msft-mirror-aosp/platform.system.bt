/******************************************************************************
 *
 *  Copyright (C) 2009-2014 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*******************************************************************************
 *
 *  Filename:      btif_gatt_client.c
 *
 *  Description:   GATT client implementation
 *
 *******************************************************************************/

#define LOG_TAG "bt_btif_gattc"

#include <base/at_exit.h>
#include <base/bind.h>
#include <base/threading/thread.h>
#include <errno.h>
#include <hardware/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device/include/controller.h"


#include "btcore/include/bdaddr.h"
#include "btif_common.h"
#include "btif_util.h"

#if (defined(BLE_INCLUDED) && (BLE_INCLUDED == TRUE))

#include <hardware/bt_gatt.h>

#include "bta_api.h"
#include "bta_gatt_api.h"
#include "btif_config.h"
#include "btif_dm.h"
#include "btif_gatt.h"
#include "btif_gatt_multi_adv_util.h"
#include "btif_gatt_util.h"
#include "btif_storage.h"
#include "btif_storage.h"
#include "osi/include/log.h"
#include "vendor_api.h"

using base::Bind;
using base::Owned;

extern bt_status_t do_in_jni_thread(const base::Closure& task);

/*******************************************************************************
**  Constants & Macros
********************************************************************************/

#define CHECK_BTGATT_INIT() if (bt_gatt_callbacks == NULL)\
    {\
        LOG_WARN(LOG_TAG, "%s: BTGATT not initialized", __FUNCTION__);\
        return BT_STATUS_NOT_READY;\
    } else {\
        LOG_VERBOSE(LOG_TAG, "%s", __FUNCTION__);\
    }

#define BLE_RESOLVE_ADDR_MSB                 0x40   /* bit7, bit6 is 01 to be resolvable random */
#define BLE_RESOLVE_ADDR_MASK                0xc0   /* bit 6, and bit7 */
#define BTM_BLE_IS_RESOLVE_BDA(x)           ((x[0] & BLE_RESOLVE_ADDR_MASK) == BLE_RESOLVE_ADDR_MSB)

typedef enum {
    BTIF_GATTC_REGISTER_APP = 1000,
    BTIF_GATTC_UNREGISTER_APP,
    BTIF_GATTC_SCAN_FILTER_CONFIG
} btif_gattc_event_t;

#define BTIF_GATT_MAX_OBSERVED_DEV 40

#define BTIF_GATT_OBSERVE_EVT   0x1000
#define BTIF_GATTC_RSSI_EVT     0x1001
#define BTIF_GATTC_SCAN_FILTER_EVT  0x1003
#define BTIF_GATTC_SCAN_PARAM_EVT   0x1004

#define ENABLE_BATCH_SCAN 1
#define DISABLE_BATCH_SCAN 0

/*******************************************************************************
**  Local type definitions
********************************************************************************/
typedef struct
{
    uint8_t report_format;
    uint16_t data_len;
    uint8_t num_records;
    uint8_t *p_rep_data;
} btgatt_batch_reports;

typedef struct
{
    uint8_t  status;
    uint8_t  client_if;
    uint8_t  action;
    uint8_t  avbl_space;
    uint8_t  lost_timeout;
    tBLE_ADDR_TYPE addr_type;
    uint8_t  batch_scan_full_max;
    uint8_t  batch_scan_trunc_max;
    uint8_t  batch_scan_notify_threshold;
    tBTA_BLE_BATCH_SCAN_MODE scan_mode;
    uint32_t scan_interval;
    uint32_t scan_window;
    tBTA_BLE_DISCARD_RULE discard_rule;
    btgatt_batch_reports  read_reports;
} btgatt_batch_track_cb_t;

typedef tBTA_DM_BLE_PF_FILT_PARAMS btgatt_adv_filt_param_t;

typedef struct
{
    uint8_t     client_if;
    uint8_t     action;
    tBTA_DM_BLE_PF_COND_TYPE filt_type;
    bt_bdaddr_t bd_addr;
    uint8_t     value[BTGATT_MAX_ATTR_LEN];
    uint8_t     value_len;
    uint8_t     filt_index;
    uint16_t    conn_id;
    uint16_t    company_id_mask;
    bt_uuid_t   uuid;
    bt_uuid_t   uuid_mask;
    uint8_t     value_mask[BTGATT_MAX_ATTR_LEN];
    uint8_t     value_mask_len;
    uint8_t     has_mask;
    uint8_t     addr_type;
    uint8_t     status;
    tBTA_DM_BLE_PF_AVBL_SPACE avbl_space;
    tBTA_DM_BLE_SCAN_COND_OP cond_op;
    btgatt_adv_filt_param_t adv_filt_param;
} btgatt_adv_filter_cb_t;

typedef struct
{
    uint8_t     value[BTGATT_MAX_ATTR_LEN];
    uint8_t     inst_id;
    bt_bdaddr_t bd_addr;
    btgatt_srvc_id_t srvc_id;
    btgatt_srvc_id_t incl_srvc_id;
    btgatt_gatt_id_t char_id;
    btgatt_gatt_id_t descr_id;
    uint16_t    handle;
    bt_uuid_t   uuid;
    bt_uuid_t   uuid_mask;
    uint16_t    conn_id;
    uint16_t    len;
    uint16_t    mask;
    uint32_t    scan_interval;
    uint32_t    scan_window;
    uint8_t     client_if;
    uint8_t     action;
    uint8_t     is_direct;
    uint8_t     search_all;
    uint8_t     auth_req;
    uint8_t     write_type;
    uint8_t     status;
    uint8_t     addr_type;
    uint8_t     start;
    uint8_t     has_mask;
    int8_t      rssi;
    uint8_t     flag;
    tBT_DEVICE_TYPE device_type;
    btgatt_transport_t transport;
} __attribute__((packed)) btif_gattc_cb_t;

typedef struct
{
    bt_bdaddr_t bd_addr;
    uint16_t    min_interval;
    uint16_t    max_interval;
    uint16_t    timeout;
    uint16_t    latency;
} btif_conn_param_cb_t;

typedef struct
{
    bt_bdaddr_t bd_addr;
    BOOLEAN     in_use;
}__attribute__((packed)) btif_gattc_dev_t;

typedef struct
{
    btif_gattc_dev_t remote_dev[BTIF_GATT_MAX_OBSERVED_DEV];
    uint8_t            addr_type;
    uint8_t            next_storage_idx;
}__attribute__((packed)) btif_gattc_dev_cb_t;

/*******************************************************************************
**  Static variables
********************************************************************************/

extern const btgatt_callbacks_t *bt_gatt_callbacks;
static btif_gattc_dev_cb_t  btif_gattc_dev_cb;
static btif_gattc_dev_cb_t  *p_dev_cb = &btif_gattc_dev_cb;
static uint8_t rssi_request_client_if;

/*******************************************************************************
**  Static functions
********************************************************************************/

static bt_status_t btif_gattc_multi_adv_disable(int client_if);
static void btif_multi_adv_stop_cb(void *data)
{
    int client_if = PTR_TO_INT(data);
    btif_gattc_multi_adv_disable(client_if); // Does context switch
}

static btgattc_error_t btif_gattc_translate_btm_status(tBTM_STATUS status)
{
    switch(status)
    {
       case BTM_SUCCESS:
       case BTM_SUCCESS_NO_SECURITY:
            return BT_GATTC_COMMAND_SUCCESS;

       case BTM_CMD_STARTED:
            return BT_GATTC_COMMAND_STARTED;

       case BTM_BUSY:
            return BT_GATTC_COMMAND_BUSY;

       case BTM_CMD_STORED:
            return BT_GATTC_COMMAND_STORED;

       case BTM_NO_RESOURCES:
            return BT_GATTC_NO_RESOURCES;

       case BTM_MODE_UNSUPPORTED:
       case BTM_WRONG_MODE:
       case BTM_MODE4_LEVEL4_NOT_SUPPORTED:
            return BT_GATTC_MODE_UNSUPPORTED;

       case BTM_ILLEGAL_VALUE:
       case BTM_SCO_BAD_LENGTH:
            return BT_GATTC_ILLEGAL_VALUE;

       case BTM_UNKNOWN_ADDR:
            return BT_GATTC_UNKNOWN_ADDR;

       case BTM_DEVICE_TIMEOUT:
            return BT_GATTC_DEVICE_TIMEOUT;

       case BTM_FAILED_ON_SECURITY:
       case BTM_REPEATED_ATTEMPTS:
       case BTM_NOT_AUTHORIZED:
            return BT_GATTC_SECURITY_ERROR;

       case BTM_DEV_RESET:
       case BTM_ILLEGAL_ACTION:
            return BT_GATTC_INCORRECT_STATE;

       case BTM_BAD_VALUE_RET:
            return BT_GATTC_INVALID_CONTROLLER_OUTPUT;

       case BTM_DELAY_CHECK:
            return BT_GATTC_DELAYED_ENCRYPTION_CHECK;

       case BTM_ERR_PROCESSING:
       default:
          return BT_GATTC_ERR_PROCESSING;
    }
}

static void btapp_gattc_req_data(UINT16 event, char *p_dest, char *p_src)
{
    tBTA_GATTC *p_dest_data = (tBTA_GATTC*) p_dest;
    tBTA_GATTC *p_src_data = (tBTA_GATTC*) p_src;

    if (!p_src_data || !p_dest_data)
       return;

    // Copy basic structure first
    maybe_non_aligned_memcpy(p_dest_data, p_src_data, sizeof(*p_src_data));

    // Allocate buffer for request data if necessary
    switch (event)
    {
        case BTA_GATTC_READ_CHAR_EVT:
        case BTA_GATTC_READ_DESCR_EVT:

            if (p_src_data->read.p_value != NULL)
            {
                p_dest_data->read.p_value = (tBTA_GATT_UNFMT *)osi_malloc(sizeof(tBTA_GATT_UNFMT));

                memcpy(p_dest_data->read.p_value, p_src_data->read.p_value,
                       sizeof(tBTA_GATT_UNFMT));

                // Allocate buffer for att value if necessary
                if (p_src_data->read.p_value->len > 0 &&
                    p_src_data->read.p_value->p_value != NULL) {
                    p_dest_data->read.p_value->p_value =
                        (UINT8 *)osi_malloc(p_src_data->read.p_value->len);
                    memcpy(p_dest_data->read.p_value->p_value,
                           p_src_data->read.p_value->p_value,
                           p_src_data->read.p_value->len);
                }
            } else {
                BTIF_TRACE_WARNING("%s :Src read.p_value ptr is NULL for event  0x%x",
                                    __FUNCTION__, event);
                p_dest_data->read.p_value = NULL;

            }
            break;

        default:
            break;
    }
}

static void btapp_gattc_free_req_data(UINT16 event, tBTA_GATTC *p_data)
{
    switch (event)
    {
        case BTA_GATTC_READ_CHAR_EVT:
        case BTA_GATTC_READ_DESCR_EVT:
            if (p_data != NULL && p_data->read.p_value != NULL)
            {
                if (p_data->read.p_value->len > 0)
                    osi_free_and_reset((void **)&p_data->read.p_value->p_value);

                osi_free_and_reset((void **)&p_data->read.p_value);
            }
            break;

        default:
            break;
    }
}

static void btif_gattc_init_dev_cb(void)
{
    memset(p_dev_cb, 0, sizeof(btif_gattc_dev_cb_t));
}

static void btif_gattc_add_remote_bdaddr (BD_ADDR p_bda, uint8_t addr_type)
{
    uint8_t i;
    for (i = 0; i < BTIF_GATT_MAX_OBSERVED_DEV; i++)
    {
        if (!p_dev_cb->remote_dev[i].in_use )
        {
            memcpy(p_dev_cb->remote_dev[i].bd_addr.address, p_bda, BD_ADDR_LEN);
            p_dev_cb->addr_type = addr_type;
            p_dev_cb->remote_dev[i].in_use = TRUE;
            LOG_VERBOSE(LOG_TAG, "%s device added idx=%d", __FUNCTION__, i  );
            break;
        }
    }

    if ( i == BTIF_GATT_MAX_OBSERVED_DEV)
    {
        i= p_dev_cb->next_storage_idx;
        memcpy(p_dev_cb->remote_dev[i].bd_addr.address, p_bda, BD_ADDR_LEN);
        p_dev_cb->addr_type = addr_type;
        p_dev_cb->remote_dev[i].in_use = TRUE;
        LOG_VERBOSE(LOG_TAG, "%s device overwrite idx=%d", __FUNCTION__, i  );
        p_dev_cb->next_storage_idx++;
        if (p_dev_cb->next_storage_idx >= BTIF_GATT_MAX_OBSERVED_DEV)
               p_dev_cb->next_storage_idx = 0;
    }
}

static BOOLEAN btif_gattc_find_bdaddr (BD_ADDR p_bda)
{
    uint8_t i;
    for (i = 0; i < BTIF_GATT_MAX_OBSERVED_DEV; i++)
    {
        if (p_dev_cb->remote_dev[i].in_use &&
            !memcmp(p_dev_cb->remote_dev[i].bd_addr.address, p_bda, BD_ADDR_LEN))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void btif_gattc_update_properties ( btif_gattc_cb_t *p_btif_cb )
{
    uint8_t remote_name_len;
    uint8_t *p_eir_remote_name=NULL;
    bt_bdname_t bdname;

    p_eir_remote_name = BTM_CheckEirData(p_btif_cb->value,
                                         BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &remote_name_len);

    if (p_eir_remote_name == NULL)
    {
        p_eir_remote_name = BTM_CheckEirData(p_btif_cb->value,
                                BT_EIR_SHORTENED_LOCAL_NAME_TYPE, &remote_name_len);
    }

    if (p_eir_remote_name)
    {
        memcpy(bdname.name, p_eir_remote_name, remote_name_len);
        bdname.name[remote_name_len]='\0';

        LOG_DEBUG(LOG_TAG, "%s BLE device name=%s len=%d dev_type=%d", __FUNCTION__, bdname.name,
              remote_name_len, p_btif_cb->device_type  );
        btif_dm_update_ble_remote_properties( p_btif_cb->bd_addr.address,   bdname.name,
                                               p_btif_cb->device_type);
    }
}

static void btif_gattc_upstreams_evt(uint16_t event, char* p_param)
{
    LOG_VERBOSE(LOG_TAG, "%s: Event %d", __FUNCTION__, event);

    tBTA_GATTC *p_data = (tBTA_GATTC*) p_param;
    switch (event)
    {
        case BTA_GATTC_REG_EVT:
        {
            bt_uuid_t app_uuid;
            bta_to_btif_uuid(&app_uuid, &p_data->reg_oper.app_uuid);
            HAL_CBACK(bt_gatt_callbacks, client->register_client_cb
                , p_data->reg_oper.status
                , p_data->reg_oper.client_if
                , &app_uuid
            );
            break;
        }

        case BTA_GATTC_DEREG_EVT:
            break;

        case BTA_GATTC_READ_CHAR_EVT:
        {
            btgatt_read_params_t data;
            set_read_value(&data, &p_data->read);

            HAL_CBACK(bt_gatt_callbacks, client->read_characteristic_cb
                , p_data->read.conn_id, p_data->read.status, &data);
            break;
        }

        case BTA_GATTC_WRITE_CHAR_EVT:
        case BTA_GATTC_PREP_WRITE_EVT:
        {
            HAL_CBACK(bt_gatt_callbacks, client->write_characteristic_cb,
                p_data->write.conn_id, p_data->write.status, p_data->write.handle);
            break;
        }

        case BTA_GATTC_EXEC_EVT:
        {
            HAL_CBACK(bt_gatt_callbacks, client->execute_write_cb
                , p_data->exec_cmpl.conn_id, p_data->exec_cmpl.status
            );
            break;
        }

        case BTA_GATTC_SEARCH_CMPL_EVT:
        {
            HAL_CBACK(bt_gatt_callbacks, client->search_complete_cb
                , p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
            break;
        }

        case BTA_GATTC_READ_DESCR_EVT:
        {
            btgatt_read_params_t data;
            set_read_value(&data, &p_data->read);

            HAL_CBACK(bt_gatt_callbacks, client->read_descriptor_cb
                , p_data->read.conn_id, p_data->read.status, &data);
            break;
        }

        case BTA_GATTC_WRITE_DESCR_EVT:
        {
            HAL_CBACK(bt_gatt_callbacks, client->write_descriptor_cb,
                p_data->write.conn_id, p_data->write.status, p_data->write.handle);
            break;
        }

        case BTA_GATTC_NOTIF_EVT:
        {
            btgatt_notify_params_t data;

            bdcpy(data.bda.address, p_data->notify.bda);
            memcpy(data.value, p_data->notify.value, p_data->notify.len);

            data.handle = p_data->notify.handle;
            data.is_notify = p_data->notify.is_notify;
            data.len = p_data->notify.len;

            HAL_CBACK(bt_gatt_callbacks, client->notify_cb, p_data->notify.conn_id, &data);

            if (p_data->notify.is_notify == FALSE)
                BTA_GATTC_SendIndConfirm(p_data->notify.conn_id, p_data->notify.handle);

            break;
        }

        case BTA_GATTC_OPEN_EVT:
        {
            bt_bdaddr_t bda;
            bdcpy(bda.address, p_data->open.remote_bda);

            HAL_CBACK(bt_gatt_callbacks, client->open_cb, p_data->open.conn_id
                , p_data->open.status, p_data->open.client_if, &bda);

            if (GATT_DEF_BLE_MTU_SIZE != p_data->open.mtu && p_data->open.mtu)
            {
                HAL_CBACK(bt_gatt_callbacks, client->configure_mtu_cb, p_data->open.conn_id
                    , p_data->open.status , p_data->open.mtu);
            }

            if (p_data->open.status == BTA_GATT_OK)
                btif_gatt_check_encrypted_link(p_data->open.remote_bda, p_data->open.transport);
            break;
        }

        case BTA_GATTC_CLOSE_EVT:
        {
            bt_bdaddr_t bda;
            bdcpy(bda.address, p_data->close.remote_bda);
            HAL_CBACK(bt_gatt_callbacks, client->close_cb, p_data->close.conn_id
                , p_data->status, p_data->close.client_if, &bda);
            break;
        }

        case BTA_GATTC_ACL_EVT:
            LOG_DEBUG(LOG_TAG, "BTA_GATTC_ACL_EVT: status = %d", p_data->status);
            /* Ignore for now */
            break;

        case BTA_GATTC_CANCEL_OPEN_EVT:
            break;

        case BTIF_GATT_OBSERVE_EVT:
        {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t*) p_param;
            uint8_t remote_name_len;
            uint8_t *p_eir_remote_name=NULL;
            bt_device_type_t dev_type;
            bt_property_t properties;

            p_eir_remote_name = BTM_CheckEirData(p_btif_cb->value,
                                         BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &remote_name_len);

            if (p_eir_remote_name == NULL)
            {
                p_eir_remote_name = BTM_CheckEirData(p_btif_cb->value,
                                BT_EIR_SHORTENED_LOCAL_NAME_TYPE, &remote_name_len);
            }

            if ((p_btif_cb->addr_type != BLE_ADDR_RANDOM) || (p_eir_remote_name))
            {
               if (!btif_gattc_find_bdaddr(p_btif_cb->bd_addr.address))
               {
                  btif_gattc_add_remote_bdaddr(p_btif_cb->bd_addr.address, p_btif_cb->addr_type);
                  btif_gattc_update_properties(p_btif_cb);
               }
            }

             dev_type = (bt_device_type_t) p_btif_cb->device_type;
             BTIF_STORAGE_FILL_PROPERTY(&properties,
                        BT_PROPERTY_TYPE_OF_DEVICE, sizeof(dev_type), &dev_type);
             btif_storage_set_remote_device_property(&(p_btif_cb->bd_addr), &properties);

            btif_storage_set_remote_addr_type( &p_btif_cb->bd_addr, p_btif_cb->addr_type);

            HAL_CBACK(bt_gatt_callbacks, client->scan_result_cb,
                      &p_btif_cb->bd_addr, p_btif_cb->rssi, p_btif_cb->value);
            break;
        }

        case BTIF_GATTC_RSSI_EVT:
        {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->read_remote_rssi_cb, p_btif_cb->client_if,
                      &p_btif_cb->bd_addr, p_btif_cb->rssi, p_btif_cb->status);
            break;
        }

        case BTA_GATTC_LISTEN_EVT:
        {
            HAL_CBACK(bt_gatt_callbacks, client->listen_cb
                , p_data->reg_oper.status
                , p_data->reg_oper.client_if
            );
            break;
        }

        case BTA_GATTC_CFG_MTU_EVT:
        {
            HAL_CBACK(bt_gatt_callbacks, client->configure_mtu_cb, p_data->cfg_mtu.conn_id
                , p_data->cfg_mtu.status , p_data->cfg_mtu.mtu);
            break;
        }

        case BTA_GATTC_MULT_ADV_ENB_EVT:
        {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t*) p_param;
            if (0xFF != p_btif_cb->inst_id)
                btif_multi_adv_add_instid_map(p_btif_cb->client_if, p_btif_cb->inst_id, false);
            HAL_CBACK(bt_gatt_callbacks, client->multi_adv_enable_cb
                    , p_btif_cb->client_if
                    , p_btif_cb->status
                );
            btif_multi_adv_timer_ctrl(p_btif_cb->client_if,
                                      (p_btif_cb->status == BTA_GATT_OK) ?
                                      btif_multi_adv_stop_cb : NULL);
            break;
        }

        case BTA_GATTC_MULT_ADV_UPD_EVT:
        {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->multi_adv_update_cb
                , p_btif_cb->client_if
                , p_btif_cb->status
            );
            btif_multi_adv_timer_ctrl(p_btif_cb->client_if,
                                      (p_btif_cb->status == BTA_GATT_OK) ?
                                      btif_multi_adv_stop_cb : NULL);
            break;
        }

        case BTA_GATTC_MULT_ADV_DATA_EVT:
         {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t*) p_param;
            btif_gattc_clear_clientif(p_btif_cb->client_if, FALSE);
            HAL_CBACK(bt_gatt_callbacks, client->multi_adv_data_cb
                , p_btif_cb->client_if
                , p_btif_cb->status
            );
            break;
        }

        case BTA_GATTC_MULT_ADV_DIS_EVT:
        {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t*) p_param;
            btif_gattc_clear_clientif(p_btif_cb->client_if, TRUE);
            HAL_CBACK(bt_gatt_callbacks, client->multi_adv_disable_cb
                , p_btif_cb->client_if
                , p_btif_cb->status
            );
            break;
        }

        case BTA_GATTC_ADV_DATA_EVT:
        {
            btif_gattc_cleanup_inst_cb(STD_ADV_INSTID, FALSE);
            /* No HAL callback available */
            break;
        }

        case BTA_GATTC_CONGEST_EVT:
            HAL_CBACK(bt_gatt_callbacks, client->congestion_cb
                , p_data->congest.conn_id
                , p_data->congest.congested
            );
            break;

        case BTA_GATTC_BTH_SCAN_CFG_EVT:
        {
            btgatt_batch_track_cb_t *p_data = (btgatt_batch_track_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->batchscan_cfg_storage_cb
                , p_data->client_if
                , p_data->status
            );
            break;
        }

        case BTA_GATTC_BTH_SCAN_ENB_EVT:
        {
            btgatt_batch_track_cb_t *p_data = (btgatt_batch_track_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->batchscan_enb_disable_cb
                    , ENABLE_BATCH_SCAN
                    , p_data->client_if
                    , p_data->status);
            break;
        }

        case BTA_GATTC_BTH_SCAN_DIS_EVT:
        {
            btgatt_batch_track_cb_t *p_data = (btgatt_batch_track_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->batchscan_enb_disable_cb
                    , DISABLE_BATCH_SCAN
                    , p_data->client_if
                    , p_data->status);
            break;
        }

        case BTA_GATTC_BTH_SCAN_THR_EVT:
        {
            btgatt_batch_track_cb_t *p_data = (btgatt_batch_track_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->batchscan_threshold_cb
                    , p_data->client_if);
            break;
        }

        case BTA_GATTC_BTH_SCAN_RD_EVT:
        {
            btgatt_batch_track_cb_t *p_data = (btgatt_batch_track_cb_t*) p_param;
            uint8_t *p_rep_data = NULL;

            if (p_data->read_reports.data_len > 0 && NULL != p_data->read_reports.p_rep_data)
            {
                p_rep_data = (uint8_t *)osi_malloc(p_data->read_reports.data_len);
                memcpy(p_rep_data, p_data->read_reports.p_rep_data, p_data->read_reports.data_len);
            }

            HAL_CBACK(bt_gatt_callbacks, client->batchscan_reports_cb
                    , p_data->client_if, p_data->status, p_data->read_reports.report_format
                    , p_data->read_reports.num_records, p_data->read_reports.data_len, p_rep_data);
            osi_free(p_rep_data);
            break;
        }

        case BTA_GATTC_SCAN_FLT_CFG_EVT:
        {
            btgatt_adv_filter_cb_t *p_btif_cb = (btgatt_adv_filter_cb_t*) p_param;
            HAL_CBACK(bt_gatt_callbacks, client->scan_filter_cfg_cb, p_btif_cb->action,
                      p_btif_cb->client_if, p_btif_cb->status, p_btif_cb->cond_op,
                      p_btif_cb->avbl_space);
            break;
        }

        case BTA_GATTC_SCAN_FLT_PARAM_EVT:
        {
            btgatt_adv_filter_cb_t *p_data = (btgatt_adv_filter_cb_t*) p_param;
            BTIF_TRACE_DEBUG("BTA_GATTC_SCAN_FLT_PARAM_EVT: %d, %d, %d, %d",p_data->client_if,
                p_data->action, p_data->avbl_space, p_data->status);
            HAL_CBACK(bt_gatt_callbacks, client->scan_filter_param_cb
                    , p_data->action, p_data->client_if, p_data->status
                    , p_data->avbl_space);
            break;
        }

        case BTA_GATTC_SCAN_FLT_STATUS_EVT:
        {
            btgatt_adv_filter_cb_t *p_data = (btgatt_adv_filter_cb_t*) p_param;
            BTIF_TRACE_DEBUG("BTA_GATTC_SCAN_FLT_STATUS_EVT: %d, %d, %d",p_data->client_if,
                p_data->action, p_data->status);
            HAL_CBACK(bt_gatt_callbacks, client->scan_filter_status_cb
                    , p_data->action, p_data->client_if, p_data->status);
            break;
        }

        case BTA_GATTC_ADV_VSC_EVT:
        {
            btgatt_track_adv_info_t *p_data = (btgatt_track_adv_info_t*)p_param;
            btgatt_track_adv_info_t adv_info_data;

            memset(&adv_info_data, 0, sizeof(btgatt_track_adv_info_t));

            btif_gatt_move_track_adv_data(&adv_info_data, p_data);
            HAL_CBACK(bt_gatt_callbacks, client->track_adv_event_cb, &adv_info_data);
            break;
        }

        case BTIF_GATTC_SCAN_PARAM_EVT:
        {
            btif_gattc_cb_t *p_btif_cb = (btif_gattc_cb_t *)p_param;
            HAL_CBACK(bt_gatt_callbacks, client->scan_parameter_setup_completed_cb,
                      p_btif_cb->client_if, btif_gattc_translate_btm_status(p_btif_cb->status));
            break;
        }

        default:
            LOG_ERROR(LOG_TAG, "%s: Unhandled event (%d)!", __FUNCTION__, event);
            break;
    }

    btapp_gattc_free_req_data(event, p_data);
}

static void bta_gattc_cback(tBTA_GATTC_EVT event, tBTA_GATTC *p_data)
{
    bt_status_t status = btif_transfer_context(btif_gattc_upstreams_evt,
                    (uint16_t) event, (char*) p_data, sizeof(tBTA_GATTC), btapp_gattc_req_data);
    ASSERTC(status == BT_STATUS_SUCCESS, "Context transfer failed!", status);
}

static void bta_gattc_multi_adv_cback(tBTA_BLE_MULTI_ADV_EVT event, UINT8 inst_id,
                                    void *p_ref, tBTA_STATUS call_status)
{
    btif_gattc_cb_t btif_cb;
    tBTA_GATTC_EVT upevt;
    uint8_t client_if = 0;

    if (NULL == p_ref)
    {
        BTIF_TRACE_WARNING("%s Invalid p_ref received",__FUNCTION__);
    }
    else
    {
        client_if = *(UINT8 *) p_ref;
    }

    BTIF_TRACE_DEBUG("%s -Inst ID %d, Status:%x, client_if:%d",__FUNCTION__,inst_id, call_status,
                       client_if);
    btif_cb.status = call_status;
    btif_cb.client_if = client_if;
    btif_cb.inst_id = inst_id;

    switch(event)
    {
        case BTA_BLE_MULTI_ADV_ENB_EVT:
            upevt = BTA_GATTC_MULT_ADV_ENB_EVT;
            break;

        case BTA_BLE_MULTI_ADV_DISABLE_EVT:
            upevt = BTA_GATTC_MULT_ADV_DIS_EVT;
            break;

        case BTA_BLE_MULTI_ADV_PARAM_EVT:
            upevt = BTA_GATTC_MULT_ADV_UPD_EVT;
            break;

        case BTA_BLE_MULTI_ADV_DATA_EVT:
            upevt = BTA_GATTC_MULT_ADV_DATA_EVT;
            break;

        default:
            return;
    }

    bt_status_t status = btif_transfer_context(btif_gattc_upstreams_evt, (uint16_t) upevt,
                        (char*) &btif_cb, sizeof(btif_gattc_cb_t), NULL);
    ASSERTC(status == BT_STATUS_SUCCESS, "Context transfer failed!", status);
}

static void bta_gattc_set_adv_data_cback(tBTA_STATUS call_status)
{
    UNUSED(call_status);
    btif_gattc_cb_t btif_cb;
    btif_cb.status = call_status;
    btif_cb.action = 0;
    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_ADV_DATA_EVT,
                          (char*) &btif_cb, sizeof(btif_gattc_cb_t), NULL);
}

static void bta_batch_scan_setup_cb (tBTA_BLE_BATCH_SCAN_EVT evt,
                                            tBTA_DM_BLE_REF_VALUE ref_value, tBTA_STATUS status)
{
    UINT8 upevt = 0;
    btgatt_batch_track_cb_t btif_scan_track_cb;

    btif_scan_track_cb.status = status;
    btif_scan_track_cb.client_if = ref_value;
    BTIF_TRACE_DEBUG("bta_batch_scan_setup_cb-Status:%x, client_if:%d, evt=%d",
            status, ref_value, evt);

    switch(evt)
    {
        case BTA_BLE_BATCH_SCAN_ENB_EVT:
        {
           upevt = BTA_GATTC_BTH_SCAN_ENB_EVT;
           break;
        }

        case BTA_BLE_BATCH_SCAN_DIS_EVT:
        {
           upevt = BTA_GATTC_BTH_SCAN_DIS_EVT;
           break;
        }

        case BTA_BLE_BATCH_SCAN_CFG_STRG_EVT:
        {
           upevt = BTA_GATTC_BTH_SCAN_CFG_EVT;
           break;
        }

        case BTA_BLE_BATCH_SCAN_DATA_EVT:
        {
           upevt = BTA_GATTC_BTH_SCAN_RD_EVT;
           break;
        }

        case BTA_BLE_BATCH_SCAN_THRES_EVT:
        {
           upevt = BTA_GATTC_BTH_SCAN_THR_EVT;
           break;
        }

        default:
            return;
    }

    btif_transfer_context(btif_gattc_upstreams_evt, upevt,(char*) &btif_scan_track_cb,
                          sizeof(btgatt_batch_track_cb_t), NULL);

}

static void bta_batch_scan_threshold_cb(tBTA_DM_BLE_REF_VALUE ref_value)
{
    btgatt_batch_track_cb_t btif_scan_track_cb;
    btif_scan_track_cb.status = 0;
    btif_scan_track_cb.client_if = ref_value;

    BTIF_TRACE_DEBUG("%s - client_if:%d",__FUNCTION__, ref_value);

    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_BTH_SCAN_THR_EVT,
                          (char*) &btif_scan_track_cb, sizeof(btif_gattc_cb_t), NULL);
}

static void bta_batch_scan_reports_cb(tBTA_DM_BLE_REF_VALUE ref_value, UINT8 report_format,
                                            UINT8 num_records, UINT16 data_len,
                                            UINT8* p_rep_data, tBTA_STATUS status)
{
    btgatt_batch_track_cb_t btif_scan_track_cb;
    memset(&btif_scan_track_cb, 0, sizeof(btgatt_batch_track_cb_t));
    BTIF_TRACE_DEBUG("%s - client_if:%d, %d, %d, %d",__FUNCTION__, ref_value, status, num_records,
                                    data_len);

    btif_scan_track_cb.status = status;

    btif_scan_track_cb.client_if = ref_value;
    btif_scan_track_cb.read_reports.report_format = report_format;
    btif_scan_track_cb.read_reports.data_len = data_len;
    btif_scan_track_cb.read_reports.num_records = num_records;

    if (data_len > 0)
    {
        btif_scan_track_cb.read_reports.p_rep_data = (UINT8 *)osi_malloc(data_len);
        memcpy(btif_scan_track_cb.read_reports.p_rep_data, p_rep_data, data_len);
        osi_free(p_rep_data);
    }

    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_BTH_SCAN_RD_EVT,
        (char*) &btif_scan_track_cb, sizeof(btgatt_batch_track_cb_t), NULL);

    if (data_len > 0)
        osi_free_and_reset((void **)&btif_scan_track_cb.read_reports.p_rep_data);
}

static void bta_scan_results_cb (tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH *p_data)
{
    btif_gattc_cb_t btif_cb;
    uint8_t len;

    switch (event)
    {
        case BTA_DM_INQ_RES_EVT:
        {
            bdcpy(btif_cb.bd_addr.address, p_data->inq_res.bd_addr);
            btif_cb.device_type = p_data->inq_res.device_type;
            btif_cb.rssi = p_data->inq_res.rssi;
            btif_cb.addr_type = p_data->inq_res.ble_addr_type;
            btif_cb.flag = p_data->inq_res.flag;
            if (p_data->inq_res.p_eir)
            {
                memcpy(btif_cb.value, p_data->inq_res.p_eir, 62);
                if (BTM_CheckEirData(p_data->inq_res.p_eir, BTM_EIR_COMPLETE_LOCAL_NAME_TYPE,
                                      &len))
                {
                    p_data->inq_res.remt_name_not_required  = TRUE;
                }
            }
        }
        break;

        case BTA_DM_INQ_CMPL_EVT:
        {
            BTIF_TRACE_DEBUG("%s  BLE observe complete. Num Resp %d",
                              __FUNCTION__,p_data->inq_cmpl.num_resps);
            return;
        }

        default:
        BTIF_TRACE_WARNING("%s : Unknown event 0x%x", __FUNCTION__, event);
        return;
    }
    btif_transfer_context(btif_gattc_upstreams_evt, BTIF_GATT_OBSERVE_EVT,
                                 (char*) &btif_cb, sizeof(btif_gattc_cb_t), NULL);
}

static void bta_track_adv_event_cb(tBTA_DM_BLE_TRACK_ADV_DATA *p_track_adv_data)
{
    btgatt_track_adv_info_t btif_scan_track_cb;
    BTIF_TRACE_DEBUG("%s",__FUNCTION__);
    btif_gatt_move_track_adv_data(&btif_scan_track_cb,
                (btgatt_track_adv_info_t*)p_track_adv_data);

    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_ADV_VSC_EVT,
                          (char*) &btif_scan_track_cb, sizeof(btgatt_track_adv_info_t), NULL);
}

static void btm_read_rssi_cb (tBTM_RSSI_RESULTS *p_result)
{
    btif_gattc_cb_t btif_cb;

    bdcpy(btif_cb.bd_addr.address, p_result->rem_bda);
    btif_cb.rssi = p_result->rssi;
    btif_cb.status = p_result->status;
    btif_cb.client_if = rssi_request_client_if;
    btif_transfer_context(btif_gattc_upstreams_evt, BTIF_GATTC_RSSI_EVT,
                                 (char*) &btif_cb, sizeof(btif_gattc_cb_t), NULL);
}

static void bta_scan_param_setup_cb(tGATT_IF client_if, tBTM_STATUS status)
{
    btif_gattc_cb_t btif_cb;

    btif_cb.status = status;
    btif_cb.client_if = client_if;
    btif_transfer_context(btif_gattc_upstreams_evt, BTIF_GATTC_SCAN_PARAM_EVT,
                          (char *)&btif_cb, sizeof(btif_gattc_cb_t), NULL);
}

static void bta_scan_filt_cfg_cb(tBTA_DM_BLE_PF_ACTION action, tBTA_DM_BLE_SCAN_COND_OP cfg_op,
                                tBTA_DM_BLE_PF_AVBL_SPACE avbl_space, tBTA_STATUS status,
                                tBTA_DM_BLE_REF_VALUE ref_value)
{
    btgatt_adv_filter_cb_t btif_cb;
    btif_cb.status = status;
    btif_cb.action = action;
    btif_cb.cond_op = cfg_op;
    btif_cb.avbl_space = avbl_space;
    btif_cb.client_if = ref_value;
    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_SCAN_FLT_CFG_EVT,
                          (char*) &btif_cb, sizeof(btgatt_adv_filter_cb_t), NULL);
}

static void bta_scan_filt_param_setup_cb(UINT8 action_type,
                                        tBTA_DM_BLE_PF_AVBL_SPACE avbl_space,
                                        tBTA_DM_BLE_REF_VALUE ref_value, tBTA_STATUS status)
{
    btgatt_adv_filter_cb_t btif_cb;

    btif_cb.status = status;
    btif_cb.action = action_type;
    btif_cb.client_if = ref_value;
    btif_cb.avbl_space = avbl_space;
    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_SCAN_FLT_PARAM_EVT,
                          (char*) &btif_cb, sizeof(btgatt_adv_filter_cb_t), NULL);
}

static void bta_scan_filt_status_cb(UINT8 action, tBTA_STATUS status,
                                    tBTA_DM_BLE_REF_VALUE ref_value)
{
    btgatt_adv_filter_cb_t btif_cb;

    btif_cb.status = status;
    btif_cb.action = action;
    btif_cb.client_if = ref_value;
    btif_transfer_context(btif_gattc_upstreams_evt, BTA_GATTC_SCAN_FLT_STATUS_EVT,
                          (char*) &btif_cb, sizeof(btgatt_adv_filter_cb_t), NULL);
}

static void btgattc_handle_event(uint16_t event, char* p_param)
{
    tBT_UUID                   uuid;

    btif_gattc_cb_t* p_cb = (btif_gattc_cb_t*) p_param;
    if (!p_cb) return;

    LOG_VERBOSE(LOG_TAG, "%s: Event %d", __FUNCTION__, event);

    switch (event)
    {
        case BTIF_GATTC_REGISTER_APP:
            btif_to_bta_uuid(&uuid, &p_cb->uuid);
            btif_gattc_incr_app_count();
            BTA_GATTC_AppRegister(&uuid, bta_gattc_cback);
            break;

        case BTIF_GATTC_UNREGISTER_APP:
            btif_gattc_clear_clientif(p_cb->client_if, TRUE);
            btif_gattc_decr_app_count();
            BTA_GATTC_AppDeregister(p_cb->client_if);
            break;

        case BTIF_GATTC_SCAN_FILTER_CONFIG:
        {
            btgatt_adv_filter_cb_t *p_adv_filt_cb = (btgatt_adv_filter_cb_t *) p_param;
            tBTA_DM_BLE_PF_COND_PARAM cond;
            memset(&cond, 0, sizeof(cond));

            switch (p_adv_filt_cb->filt_type)
            {
                case BTA_DM_BLE_PF_ADDR_FILTER: // 0
                    bdcpy(cond.target_addr.bda, p_adv_filt_cb->bd_addr.address);
                    cond.target_addr.type = p_adv_filt_cb->addr_type;
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                              p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                              &cond, bta_scan_filt_cfg_cb,
                                              p_adv_filt_cb->client_if);
                    break;

                case BTA_DM_BLE_PF_SRVC_DATA: // 1
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                            p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                            NULL, bta_scan_filt_cfg_cb, p_adv_filt_cb->client_if);
                    break;

                case BTA_DM_BLE_PF_SRVC_UUID: // 2
                {
                    tBTA_DM_BLE_PF_COND_MASK uuid_mask;

                    cond.srvc_uuid.p_target_addr = NULL;
                    cond.srvc_uuid.cond_logic = BTA_DM_BLE_PF_LOGIC_AND;
                    btif_to_bta_uuid(&cond.srvc_uuid.uuid, &p_adv_filt_cb->uuid);

                    cond.srvc_uuid.p_uuid_mask = NULL;
                    if (p_adv_filt_cb->has_mask)
                    {
                        btif_to_bta_uuid_mask(&uuid_mask, &p_adv_filt_cb->uuid_mask);
                        cond.srvc_uuid.p_uuid_mask = &uuid_mask;
                    }
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                              p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                              &cond, bta_scan_filt_cfg_cb,
                                              p_adv_filt_cb->client_if);
                    break;
                }

                case BTA_DM_BLE_PF_SRVC_SOL_UUID: // 3
                {
                    cond.solicitate_uuid.p_target_addr = NULL;
                    cond.solicitate_uuid.cond_logic = BTA_DM_BLE_PF_LOGIC_AND;
                    btif_to_bta_uuid(&cond.solicitate_uuid.uuid, &p_adv_filt_cb->uuid);
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                              p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                              &cond, bta_scan_filt_cfg_cb,
                                              p_adv_filt_cb->client_if);
                    break;
                }

                case BTA_DM_BLE_PF_LOCAL_NAME: // 4
                {
                    cond.local_name.data_len = p_adv_filt_cb->value_len;
                    cond.local_name.p_data = p_adv_filt_cb->value;
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                              p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                              &cond, bta_scan_filt_cfg_cb,
                                              p_adv_filt_cb->client_if);
                    break;
                }

                case BTA_DM_BLE_PF_MANU_DATA: // 5
                {
                    cond.manu_data.company_id = p_adv_filt_cb->conn_id;
                    cond.manu_data.company_id_mask = p_adv_filt_cb->company_id_mask;
                    cond.manu_data.data_len = p_adv_filt_cb->value_len;
                    cond.manu_data.p_pattern = p_adv_filt_cb->value;
                    cond.manu_data.p_pattern_mask = p_adv_filt_cb->value_mask;
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                              p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                              &cond, bta_scan_filt_cfg_cb,
                                              p_adv_filt_cb->client_if);
                    break;
                }

                case BTA_DM_BLE_PF_SRVC_DATA_PATTERN: //6
                {
                    cond.srvc_data.data_len = p_adv_filt_cb->value_len;
                    cond.srvc_data.p_pattern = p_adv_filt_cb->value;
                    cond.srvc_data.p_pattern_mask = p_adv_filt_cb->value_mask;
                    BTA_DmBleCfgFilterCondition(p_adv_filt_cb->action,
                                                p_adv_filt_cb->filt_type, p_adv_filt_cb->filt_index,
                                                &cond, bta_scan_filt_cfg_cb,
                                                p_adv_filt_cb->client_if);
                   break;
                }

                default:
                    LOG_ERROR(LOG_TAG, "%s: Unknown filter type (%d)!", __FUNCTION__, p_cb->action);
                    break;
            }
            break;
        }

        default:
            LOG_ERROR(LOG_TAG, "%s: Unknown event (%d)!", __FUNCTION__, event);
            break;
    }
}

/*******************************************************************************
**  Client API Functions
********************************************************************************/

static bt_status_t btif_gattc_register_app(bt_uuid_t *uuid)
{
    CHECK_BTGATT_INIT();
    btif_gattc_cb_t btif_cb;
    memcpy(&btif_cb.uuid, uuid, sizeof(bt_uuid_t));
    return btif_transfer_context(btgattc_handle_event, BTIF_GATTC_REGISTER_APP,
                                 (char*) &btif_cb, sizeof(btif_gattc_cb_t), NULL);
}

static bt_status_t btif_gattc_unregister_app(int client_if )
{
    CHECK_BTGATT_INIT();
    btif_gattc_cb_t btif_cb;
    btif_cb.client_if = (uint8_t) client_if;
    return btif_transfer_context(btgattc_handle_event, BTIF_GATTC_UNREGISTER_APP,
                                 (char*) &btif_cb, sizeof(btif_gattc_cb_t), NULL);
}

static bt_status_t btif_gattc_scan(bool start) {
  CHECK_BTGATT_INIT();
  if (start) {
    btif_gattc_init_dev_cb();
    return do_in_jni_thread(Bind(&BTA_DmBleObserve, TRUE, 0,
                                 (tBTA_DM_SEARCH_CBACK *)bta_scan_results_cb));
  } else {
    return do_in_jni_thread(Bind(&BTA_DmBleObserve, FALSE, 0, nullptr));
  }
}

static void btif_gattc_open_impl(int client_if, BD_ADDR address,
                                 bool is_direct, int transport_p) {
  // Ensure device is in inquiry database
  int addr_type = 0;
  int device_type = 0;
  tBTA_GATT_TRANSPORT transport = (tBTA_GATT_TRANSPORT)BTA_GATT_TRANSPORT_LE;

  if (btif_get_address_type(address, &addr_type) &&
      btif_get_device_type(address, &device_type) &&
      device_type != BT_DEVICE_TYPE_BREDR) {
    BTA_DmAddBleDevice(address, addr_type, device_type);
  }

  // Check for background connections
  if (!is_direct) {
    // Check for privacy 1.0 and 1.1 controller and do not start background
    // connection if RPA offloading is not supported, since it will not
    // connect after change of random address
    if (!controller_get_interface()->supports_ble_privacy() &&
        (addr_type == BLE_ADDR_RANDOM) &&
        BTM_BLE_IS_RESOLVE_BDA(address)) {
      tBTM_BLE_VSC_CB vnd_capabilities;
      BTM_BleGetVendorCapabilities(&vnd_capabilities);
      if (!vnd_capabilities.rpa_offloading) {
        HAL_CBACK(bt_gatt_callbacks, client->open_cb, 0, BT_STATUS_UNSUPPORTED,
                  client_if, (bt_bdaddr_t*) &address);
        return;
      }
    }
    BTA_DmBleSetBgConnType(BTM_BLE_CONN_AUTO, NULL);
  }

  // Determine transport
  if (transport_p != GATT_TRANSPORT_AUTO) {
    transport = transport_p;
  } else {
    switch (device_type) {
      case BT_DEVICE_TYPE_BREDR:
        transport = BTA_GATT_TRANSPORT_BR_EDR;
        break;

      case BT_DEVICE_TYPE_BLE:
        transport = BTA_GATT_TRANSPORT_LE;
        break;

      case BT_DEVICE_TYPE_DUMO:
        if (transport == GATT_TRANSPORT_LE)
          transport = BTA_GATT_TRANSPORT_LE;
        else
          transport = BTA_GATT_TRANSPORT_BR_EDR;
        break;
    }
  }

  // Connect!
  BTIF_TRACE_DEBUG("%s Transport=%d, device type=%d", __func__, transport,
                   device_type);
  BTA_GATTC_Open(client_if, address, is_direct, transport);
}

static bt_status_t btif_gattc_open(int client_if, const bt_bdaddr_t *bd_addr,
                                   bool is_direct, int transport) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(&btif_gattc_open_impl, client_if, base::Owned(address), is_direct,
           transport));
}

static void btif_gattc_close_impl(int client_if, BD_ADDR address,
                                  int conn_id) {
  // Disconnect established connections
  if (conn_id != 0)
    BTA_GATTC_Close(conn_id);
  else
    BTA_GATTC_CancelOpen(client_if, address, TRUE);

  // Cancel pending background connections (remove from whitelist)
  BTA_GATTC_CancelOpen(client_if, address, FALSE);
}

static bt_status_t btif_gattc_close(int client_if, const bt_bdaddr_t *bd_addr,
                                    int conn_id) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(&btif_gattc_close_impl, client_if, base::Owned(address), conn_id));
}

static bt_status_t btif_gattc_listen(int client_if, bool start) {
  CHECK_BTGATT_INIT();
#if (defined(BLE_PERIPHERAL_MODE_SUPPORT) && \
     (BLE_PERIPHERAL_MODE_SUPPORT == TRUE))
  return do_in_jni_thread(Bind(&BTA_GATTC_Listen, client_if, start, nullptr));
#else
  return do_in_jni_thread(Bind(&BTA_GATTC_Broadcast, client_if, start));
#endif
}

static void btif_gattc_set_adv_data_impl(btif_adv_data_t *p_adv_data) {
  const int cbindex = CLNT_IF_IDX;
  if (cbindex >= 0 && btif_gattc_copy_datacb(cbindex, p_adv_data, false)) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    if (!p_adv_data->set_scan_rsp) {
      BTA_DmBleSetAdvConfig(p_multi_adv_data_cb->inst_cb[cbindex].mask,
                            &p_multi_adv_data_cb->inst_cb[cbindex].data,
                            bta_gattc_set_adv_data_cback);
    } else {
      BTA_DmBleSetScanRsp(p_multi_adv_data_cb->inst_cb[cbindex].mask,
                          &p_multi_adv_data_cb->inst_cb[cbindex].data,
                          bta_gattc_set_adv_data_cback);
    }
  } else {
    BTIF_TRACE_ERROR("%s: failed to get instance data cbindex: %d", __func__,
                     cbindex);
  }
}

static bt_status_t btif_gattc_set_adv_data(
    int client_if, bool set_scan_rsp, bool include_name, bool include_txpower,
    int min_interval, int max_interval, int appearance,
    uint16_t manufacturer_len, char *manufacturer_data,
    uint16_t service_data_len, char *service_data, uint16_t service_uuid_len,
    char *service_uuid) {
  CHECK_BTGATT_INIT();

  btif_adv_data_t *adv_data = new btif_adv_data_t;

  btif_gattc_adv_data_packager(
      client_if, set_scan_rsp, include_name, include_txpower, min_interval,
      max_interval, appearance, manufacturer_len, manufacturer_data,
      service_data_len, service_data, service_uuid_len, service_uuid, adv_data);

  return do_in_jni_thread(
      Bind(&btif_gattc_set_adv_data_impl, base::Owned(adv_data)));
}

static bt_status_t btif_gattc_refresh(int client_if,
                                      const bt_bdaddr_t *bd_addr) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(&BTA_GATTC_Refresh, base::Owned(address)));
}

static bt_status_t btif_gattc_search_service(int conn_id,
                                             bt_uuid_t *filter_uuid) {
  CHECK_BTGATT_INIT();

  if (filter_uuid) {
    tBT_UUID *uuid = new tBT_UUID;
    btif_to_bta_uuid(uuid, filter_uuid);
    return do_in_jni_thread(
        Bind(&BTA_GATTC_ServiceSearchRequest, conn_id, base::Owned(uuid)));
  } else {
    return do_in_jni_thread(
        Bind(&BTA_GATTC_ServiceSearchRequest, conn_id, nullptr));
  }
}

void btif_gattc_get_gatt_db_impl(int conn_id) {
  btgatt_db_element_t *db = NULL;
  int count = 0;
  BTA_GATTC_GetGattDb(conn_id, 0x0000, 0xFFFF, &db, &count);

  HAL_CBACK(bt_gatt_callbacks, client->get_gatt_db_cb, conn_id, db, count);
  osi_free(db);
}

static bt_status_t btif_gattc_get_gatt_db(int conn_id) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&btif_gattc_get_gatt_db_impl, conn_id));
}

static bt_status_t btif_gattc_read_char(int conn_id, uint16_t handle,
                                        int auth_req) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(&BTA_GATTC_ReadCharacteristic, conn_id, handle, auth_req));
}

static bt_status_t btif_gattc_read_char_descr(int conn_id, uint16_t handle,
                                              int auth_req) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(&BTA_GATTC_ReadCharDescr, conn_id, handle, auth_req));
}

static bt_status_t btif_gattc_write_char(int conn_id, uint16_t handle,
                                         int write_type, int len, int auth_req,
                                         char *p_value) {
  CHECK_BTGATT_INIT();

  len = len > BTGATT_MAX_ATTR_LEN ? BTGATT_MAX_ATTR_LEN : len;
  // callback will own this value and free it.
  UINT8 *value = new UINT8(len);
  memcpy(value, p_value, len);

  return do_in_jni_thread(Bind(&BTA_GATTC_WriteCharValue, conn_id, handle,
                               write_type, len, base::Owned(value), auth_req));
}

static bt_status_t btif_gattc_write_char_descr(int conn_id, uint16_t handle,
                                               int write_type, int len,
                                               int auth_req, char *p_value) {
  len = len > BTGATT_MAX_ATTR_LEN ? BTGATT_MAX_ATTR_LEN : len;

  // callback will own this value and free it
  // TODO(jpawlowski): This one is little hacky because of unfmt type,
  // make it accept len an val like BTA_GATTC_WriteCharValue
  tBTA_GATT_UNFMT *value = (tBTA_GATT_UNFMT *)new UINT8(sizeof(UINT16) + len);
  value->len = len;
  value->p_value = ((UINT8 *)value) + 2;
  memcpy(value->p_value, p_value, len);

  return do_in_jni_thread(Bind(&BTA_GATTC_WriteCharDescr, conn_id, handle,
                               write_type, Owned(value), auth_req));
}

static bt_status_t btif_gattc_execute_write(int conn_id, int execute) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(&BTA_GATTC_ExecuteWrite, conn_id, (uint8_t)execute));
}

static void btif_gattc_reg_for_notification_impl(tBTA_GATTC_IF client_if,
                                          const BD_ADDR bda, UINT16 handle) {
  tBTA_GATT_STATUS status = BTA_GATTC_RegisterForNotifications(
      client_if, const_cast<UINT8 *>(bda), handle);

  //TODO(jpawlowski): conn_id is currently unused
  HAL_CBACK(bt_gatt_callbacks, client->register_for_notification_cb,
            /* conn_id */ 0, 1, status, handle);
}

static bt_status_t btif_gattc_reg_for_notification(int client_if,
                                                   const bt_bdaddr_t *bd_addr,
                                                   uint16_t handle) {
  CHECK_BTGATT_INIT();

  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_reg_for_notification_impl), client_if,
           base::Owned(address), handle));
}

static void btif_gattc_dereg_for_notification_impl(tBTA_GATTC_IF client_if,
                                            const BD_ADDR bda, UINT16 handle) {
  tBTA_GATT_STATUS status = BTA_GATTC_DeregisterForNotifications(
      client_if, const_cast<UINT8 *>(bda), handle);

  //TODO(jpawlowski): conn_id is currently unused
  HAL_CBACK(bt_gatt_callbacks, client->register_for_notification_cb,
            /* conn_id */ 0, 0, status, handle);
}

static bt_status_t btif_gattc_dereg_for_notification(int client_if,
                                                     const bt_bdaddr_t *bd_addr,
                                                     uint16_t handle) {
  CHECK_BTGATT_INIT();

  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_dereg_for_notification_impl),
           client_if, base::Owned(address), handle));
}

static bt_status_t btif_gattc_read_remote_rssi(int client_if,
                                               const bt_bdaddr_t *bd_addr) {
  CHECK_BTGATT_INIT();
  rssi_request_client_if = client_if;
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(base::IgnoreResult(&BTM_ReadRSSI),
                               base::Owned(address),
                               (tBTM_CMPL_CB *)btm_read_rssi_cb));
}

static bt_status_t btif_gattc_configure_mtu(int conn_id, int mtu) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&BTA_GATTC_ConfigureMTU), conn_id, mtu));
}

void btif_gattc_conn_parameter_update_impl(const BD_ADDR addr, int min_interval,
                                           int max_interval, int latency,
                                           int timeout) {
  if (BTA_DmGetConnectionState(const_cast<UINT8 *>(addr)))
    BTA_DmBleUpdateConnectionParams(const_cast<UINT8 *>(addr), min_interval,
                                    max_interval, latency, timeout);
  else
    BTA_DmSetBlePrefConnParams(const_cast<UINT8 *>(addr), min_interval,
                               max_interval, latency, timeout);
}

static bt_status_t btif_gattc_conn_parameter_update(const bt_bdaddr_t *bd_addr,
                                                    int min_interval,
                                                    int max_interval,
                                                    int latency, int timeout) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_conn_parameter_update_impl),
           bd_addr->address, min_interval, max_interval, latency, timeout));
}

static void btif_gattc_scan_filter_param_setup_impl(
    int client_if, uint8_t action, int filt_index,
    tBTA_DM_BLE_PF_FILT_PARAMS *adv_filt_param) {
  if (1 == adv_filt_param->dely_mode)
    BTA_DmBleTrackAdvertiser(client_if, bta_track_adv_event_cb);
  BTA_DmBleScanFilterSetup(action, filt_index, adv_filt_param, NULL,
                           bta_scan_filt_param_setup_cb, client_if);
}

static bt_status_t btif_gattc_scan_filter_param_setup(
    btgatt_filt_param_setup_t filt_param) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s", __FUNCTION__);

  tBTA_DM_BLE_PF_FILT_PARAMS *adv_filt_param = new tBTA_DM_BLE_PF_FILT_PARAMS;
  adv_filt_param->feat_seln = filt_param.feat_seln;
  adv_filt_param->list_logic_type = filt_param.list_logic_type;
  adv_filt_param->filt_logic_type = filt_param.filt_logic_type;
  adv_filt_param->rssi_high_thres = filt_param.rssi_high_thres;
  adv_filt_param->rssi_low_thres = filt_param.rssi_low_thres;
  adv_filt_param->dely_mode = filt_param.dely_mode;
  adv_filt_param->found_timeout = filt_param.found_timeout;
  adv_filt_param->lost_timeout = filt_param.lost_timeout;
  adv_filt_param->found_timeout_cnt = filt_param.found_timeout_cnt;
  adv_filt_param->num_of_tracking_entries = filt_param.num_of_tracking_entries;

  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_scan_filter_param_setup_impl),
           filt_param.client_if, filt_param.action, filt_param.filt_index,
           base::Owned(adv_filt_param)));
}

static bt_status_t btif_gattc_scan_filter_add_remove(int client_if, int action,
                              int filt_type, int filt_index, int company_id,
                              int company_id_mask, const bt_uuid_t *p_uuid,
                              const bt_uuid_t *p_uuid_mask, const bt_bdaddr_t *bd_addr,
                              char addr_type, int data_len, char* p_data, int mask_len,
                              char* p_mask)
{
    CHECK_BTGATT_INIT();
    btgatt_adv_filter_cb_t btif_filt_cb;
    memset(&btif_filt_cb, 0, sizeof(btgatt_adv_filter_cb_t));
    BTIF_TRACE_DEBUG("%s, %d, %d", __FUNCTION__, action, filt_type);

    /* If data is passed, both mask and data have to be the same length */
    if (data_len != mask_len && NULL != p_data && NULL != p_mask)
        return BT_STATUS_PARM_INVALID;

    btif_filt_cb.client_if = client_if;
    btif_filt_cb.action = action;
    btif_filt_cb.filt_index = filt_index;
    btif_filt_cb.filt_type = filt_type;
    btif_filt_cb.conn_id = company_id;
    btif_filt_cb.company_id_mask = company_id_mask ? company_id_mask : 0xFFFF;
    if (bd_addr)
        bdcpy(btif_filt_cb.bd_addr.address, bd_addr->address);

    btif_filt_cb.addr_type = addr_type;
    btif_filt_cb.has_mask = (p_uuid_mask != NULL);

    if (p_uuid != NULL)
        memcpy(&btif_filt_cb.uuid, p_uuid, sizeof(bt_uuid_t));
    if (p_uuid_mask != NULL)
        memcpy(&btif_filt_cb.uuid_mask, p_uuid_mask, sizeof(bt_uuid_t));
    if (p_data != NULL && data_len != 0)
    {
        memcpy(btif_filt_cb.value, p_data, data_len);
        btif_filt_cb.value_len = data_len;
        memcpy(btif_filt_cb.value_mask, p_mask, mask_len);
        btif_filt_cb.value_mask_len = mask_len;
    }
    return btif_transfer_context(btgattc_handle_event, BTIF_GATTC_SCAN_FILTER_CONFIG,
                                 (char*) &btif_filt_cb, sizeof(btgatt_adv_filter_cb_t), NULL);
}

static bt_status_t btif_gattc_scan_filter_clear(int client_if, int filter_index) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s: filter_index: %d", __FUNCTION__, filter_index);

  return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition,
                               BTA_DM_BLE_SCAN_COND_CLEAR,
                               BTA_DM_BLE_PF_TYPE_ALL, filter_index, nullptr,
                               &bta_scan_filt_cfg_cb, client_if));
}

static bt_status_t btif_gattc_scan_filter_enable(int client_if, bool enable) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s: enable: %d", __FUNCTION__, enable);

  uint8_t action = enable ? 1: 0;

  return do_in_jni_thread(Bind(&BTA_DmEnableScanFilter, action,
                               &bta_scan_filt_status_cb, client_if));
}

static bt_status_t btif_gattc_set_scan_parameters(int client_if,
                                                  int scan_interval,
                                                  int scan_window) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(BTA_DmSetBleScanParams, client_if, scan_interval, scan_window,
           BTM_BLE_SCAN_MODE_ACTI,
           (tBLE_SCAN_PARAM_SETUP_CBACK)bta_scan_param_setup_cb));
}

static int btif_gattc_get_device_type( const bt_bdaddr_t *bd_addr )
{
    int device_type = 0;
    char bd_addr_str[18] = {0};

    bdaddr_to_string(bd_addr, bd_addr_str, sizeof(bd_addr_str));
    if (btif_config_get_int(bd_addr_str, "DevType", &device_type))
        return device_type;
    return 0;
}

static void btif_gattc_multi_adv_enable_impl(int client_if, int min_interval,
                                             int max_interval, int adv_type,
                                             int chnl_map, int tx_power,
                                             int timeout_s) {
  tBTA_BLE_ADV_PARAMS param;
  param.adv_int_min = min_interval;
  param.adv_int_max = max_interval;
  param.adv_type = adv_type;
  param.channel_map = chnl_map;
  param.adv_filter_policy = 0;
  param.tx_power = tx_power;

  int cbindex = -1;
  int arrindex = btif_multi_adv_add_instid_map(client_if, INVALID_ADV_INST, true);
  if (arrindex >= 0)
    cbindex = btif_gattc_obtain_idx_for_datacb(client_if, CLNT_IF_IDX);

  if (cbindex >= 0 && arrindex >= 0) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    memcpy(&p_multi_adv_data_cb->inst_cb[cbindex].param, &param,
           sizeof(tBTA_BLE_ADV_PARAMS));
    p_multi_adv_data_cb->inst_cb[cbindex].timeout_s = timeout_s;
    BTIF_TRACE_DEBUG("%s, client_if value: %d", __FUNCTION__,
                     p_multi_adv_data_cb->clntif_map[arrindex + arrindex]);
    BTA_BleEnableAdvInstance(
        &(p_multi_adv_data_cb->inst_cb[cbindex].param),
        bta_gattc_multi_adv_cback,
        &(p_multi_adv_data_cb->clntif_map[arrindex + arrindex]));
  } else {
    // let the error propagate up from BTA layer
    BTIF_TRACE_ERROR("%s invalid index arrindex: %d, cbindex: %d",
                     __func__, arrindex, cbindex);
    BTA_BleEnableAdvInstance(&param, bta_gattc_multi_adv_cback, NULL);
  }
}

static bt_status_t btif_gattc_multi_adv_enable(int client_if, int min_interval,
                                               int max_interval, int adv_type,
                                               int chnl_map, int tx_power,
                                               int timeout_s) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(btif_gattc_multi_adv_enable_impl, client_if,
                               min_interval, max_interval, adv_type, chnl_map,
                               tx_power, timeout_s));
}

static void btif_gattc_multi_adv_update_impl(int client_if, int min_interval,
                                             int max_interval, int adv_type,
                                             int chnl_map, int tx_power) {
  tBTA_BLE_ADV_PARAMS param;
  param.adv_int_min = min_interval;
  param.adv_int_max = max_interval;
  param.adv_type = adv_type;
  param.channel_map = chnl_map;
  param.adv_filter_policy = 0;
  param.tx_power = tx_power;

  int inst_id = btif_multi_adv_instid_for_clientif(client_if);
  int cbindex = btif_gattc_obtain_idx_for_datacb(client_if, CLNT_IF_IDX);
  if (inst_id >= 0 && cbindex >= 0) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    memcpy(&p_multi_adv_data_cb->inst_cb[cbindex].param, &param,
           sizeof(tBTA_BLE_ADV_PARAMS));
    BTA_BleUpdateAdvInstParam((UINT8)inst_id,
                              &(p_multi_adv_data_cb->inst_cb[cbindex].param));
  } else {
    BTIF_TRACE_ERROR("%s invalid index in BTIF_GATTC_UPDATE_ADV", __FUNCTION__);
  }
}

static bt_status_t btif_gattc_multi_adv_update(int client_if, int min_interval,
                                               int max_interval, int adv_type,
                                               int chnl_map, int tx_power,
                                               int timeout_s) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(btif_gattc_multi_adv_update_impl, client_if,
                               min_interval, max_interval, adv_type, chnl_map,
                               tx_power));
}

static void btif_gattc_multi_adv_setdata_impl(btif_adv_data_t *p_adv_data) {
  int cbindex =
      btif_gattc_obtain_idx_for_datacb(p_adv_data->client_if, CLNT_IF_IDX);
  int inst_id = btif_multi_adv_instid_for_clientif(p_adv_data->client_if);
  if (inst_id >= 0 && cbindex >= 0 &&
      btif_gattc_copy_datacb(cbindex, p_adv_data, true)) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    BTA_BleCfgAdvInstData((UINT8)inst_id, p_adv_data->set_scan_rsp,
                          p_multi_adv_data_cb->inst_cb[cbindex].mask,
                          &p_multi_adv_data_cb->inst_cb[cbindex].data);
  } else {
    BTIF_TRACE_ERROR(
        "%s: failed to get invalid instance data: inst_id:%d cbindex:%d",
        __func__, inst_id, cbindex);
  }
}

static bt_status_t btif_gattc_multi_adv_setdata(
    int client_if, bool set_scan_rsp, bool include_name, bool incl_txpower,
    int appearance, int manufacturer_len, char *manufacturer_data,
    int service_data_len, char *service_data, int service_uuid_len,
    char *service_uuid) {
  CHECK_BTGATT_INIT();

  btif_adv_data_t *multi_adv_data_inst = new btif_adv_data_t;

  const int min_interval = 0;
  const int max_interval = 0;

  btif_gattc_adv_data_packager(client_if, set_scan_rsp, include_name,
                               incl_txpower, min_interval, max_interval,
                               appearance, manufacturer_len, manufacturer_data,
                               service_data_len, service_data, service_uuid_len,
                               service_uuid, multi_adv_data_inst);

  return do_in_jni_thread(Bind(&btif_gattc_multi_adv_setdata_impl,
                               base::Owned(multi_adv_data_inst)));
}

static void btif_gattc_multi_adv_disable_impl(int client_if) {
  int inst_id = btif_multi_adv_instid_for_clientif(client_if);
  if (inst_id >= 0)
    BTA_BleDisableAdvInstance((UINT8)inst_id);
  else
    BTIF_TRACE_ERROR("%s invalid instance ID", __func__);
}

static bt_status_t btif_gattc_multi_adv_disable(int client_if) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(btif_gattc_multi_adv_disable_impl, client_if));
}

static bt_status_t btif_gattc_cfg_storage(int client_if,
                                          int batch_scan_full_max,
                                          int batch_scan_trunc_max,
                                          int batch_scan_notify_threshold) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(BTA_DmBleSetStorageParams, batch_scan_full_max, batch_scan_trunc_max,
           batch_scan_notify_threshold,
           (tBTA_BLE_SCAN_SETUP_CBACK *)bta_batch_scan_setup_cb,
           (tBTA_BLE_SCAN_THRESHOLD_CBACK *)bta_batch_scan_threshold_cb,
           (tBTA_BLE_SCAN_REP_CBACK *)bta_batch_scan_reports_cb,
           (tBTA_DM_BLE_REF_VALUE)client_if));
}

static bt_status_t btif_gattc_enb_batch_scan(int client_if, int scan_mode,
                                             int scan_interval, int scan_window,
                                             int addr_type, int discard_rule) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(BTA_DmBleEnableBatchScan, scan_mode,
                               scan_interval, scan_window, discard_rule,
                               addr_type, client_if));
}

static bt_status_t btif_gattc_dis_batch_scan(int client_if) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(BTA_DmBleDisableBatchScan, client_if));
}

static bt_status_t btif_gattc_read_batch_scan_reports(int client_if,
                                                      int scan_mode) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(BTA_DmBleReadScanReports, scan_mode, client_if));
}

extern bt_status_t btif_gattc_test_command_impl(int command, btgatt_test_params_t* params);

static bt_status_t btif_gattc_test_command(int command, btgatt_test_params_t* params)
{
    return btif_gattc_test_command_impl(command, params);
}

const btgatt_client_interface_t btgattClientInterface = {
    btif_gattc_register_app,
    btif_gattc_unregister_app,
    btif_gattc_scan,
    btif_gattc_open,
    btif_gattc_close,
    btif_gattc_listen,
    btif_gattc_refresh,
    btif_gattc_search_service,
    btif_gattc_read_char,
    btif_gattc_write_char,
    btif_gattc_read_char_descr,
    btif_gattc_write_char_descr,
    btif_gattc_execute_write,
    btif_gattc_reg_for_notification,
    btif_gattc_dereg_for_notification,
    btif_gattc_read_remote_rssi,
    btif_gattc_scan_filter_param_setup,
    btif_gattc_scan_filter_add_remove,
    btif_gattc_scan_filter_clear,
    btif_gattc_scan_filter_enable,
    btif_gattc_get_device_type,
    btif_gattc_set_adv_data,
    btif_gattc_configure_mtu,
    btif_gattc_conn_parameter_update,
    btif_gattc_set_scan_parameters,
    btif_gattc_multi_adv_enable,
    btif_gattc_multi_adv_update,
    btif_gattc_multi_adv_setdata,
    btif_gattc_multi_adv_disable,
    btif_gattc_cfg_storage,
    btif_gattc_enb_batch_scan,
    btif_gattc_dis_batch_scan,
    btif_gattc_read_batch_scan_reports,
    btif_gattc_test_command,
    btif_gattc_get_gatt_db
};

#endif
