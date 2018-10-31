/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "nordic_spp_le_streamer.c"

// *****************************************************************************
/* EXAMPLE_START(nordic_spp_le_streamer): LE Streamer - Stream data over GATT.
 *
 * @text All newer operating systems provide GATT Client functionality.
 * This example shows how to get a maximal throughput via BLE:
 * - send whenever possible,
 * - use the max ATT MTU.
 *
 * @text In theory, we should also update the connection parameters, but we already get
 * a connection interval of 30 ms and there's no public way to use a shorter 
 * interval with iOS (if we're not implementing an HID device).
 *
 * @text Note: To start the streaming, run the example.
 * On remote device use some GATT Explorer, e.g. LightBlue, BLExplr to enable notifications.
 */
 // *****************************************************************************

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "nordic_spp_le_streamer.h"
#include "ble/gatt-service/nordic_spp_service_server.h"

#define REPORT_INTERVAL_MS 3000
#define MAX_NR_CONNECTIONS 3 

static void  packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

const uint8_t adv_data[] = {
    // Flags general discoverable, BR/EDR not supported
    2, BLUETOOTH_DATA_TYPE_FLAGS, 0x06, 
    // Name
    8, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'n', 'R', 'F',' ', 'S', 'P', 'P',
    // UUID ...
    17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS, 0x6e, 0x40, 0x0, 0x1, 0xc3, 0x52, 0x11, 0xe5, 0x95, 0x3d, 0x0, 0x2, 0xa5, 0xd5, 0xc5, 0x1b,
};
const uint8_t adv_data_len = sizeof(adv_data);

static btstack_packet_callback_registration_t hci_event_callback_registration;

// support for multiple clients
typedef struct {
    char name;
    int le_notification_enabled;
    hci_con_handle_t connection_handle;
    int  counter;
    char test_data[200];
    int  test_data_len;
    uint32_t test_data_sent;
    uint32_t test_data_start;
    btstack_context_callback_registration_t send_request;
} nordic_spp_le_streamer_connection_t;

static nordic_spp_le_streamer_connection_t nordic_spp_le_streamer_connections[MAX_NR_CONNECTIONS];

// round robin sending
static int connection_index;

static void init_connections(void){
    // track connections
    int i;
    for (i=0;i<MAX_NR_CONNECTIONS;i++){
        nordic_spp_le_streamer_connections[i].connection_handle = HCI_CON_HANDLE_INVALID;
        nordic_spp_le_streamer_connections[i].name = 'A' + i;
    }
}

static nordic_spp_le_streamer_connection_t * connection_for_conn_handle(hci_con_handle_t conn_handle){
    int i;
    for (i=0;i<MAX_NR_CONNECTIONS;i++){
        if (nordic_spp_le_streamer_connections[i].connection_handle == conn_handle) return &nordic_spp_le_streamer_connections[i];
    }
    return NULL;
}

static void next_connection_index(void){
    connection_index++;
    if (connection_index == MAX_NR_CONNECTIONS){
        connection_index = 0;
    }
}

/*
 * @section Track throughput
 * @text We calculate the throughput by setting a start time and measuring the amount of 
 * data sent. After a configurable REPORT_INTERVAL_MS, we print the throughput in kB/s
 * and reset the counter and start time.
 */

/* LISTING_START(tracking): Tracking throughput */

static void test_reset(nordic_spp_le_streamer_connection_t * context){
    context->test_data_start = btstack_run_loop_get_time_ms();
    context->test_data_sent = 0;
}

static void test_track_sent(nordic_spp_le_streamer_connection_t * context, int bytes_sent){
    context->test_data_sent += bytes_sent;
    // evaluate
    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t time_passed = now - context->test_data_start;
    if (time_passed < REPORT_INTERVAL_MS) return;
    // print speed
    int bytes_per_second = context->test_data_sent * 1000 / time_passed;
    printf("%c: %"PRIu32" bytes sent-> %u.%03u kB/s\n", context->name, context->test_data_sent, bytes_per_second / 1000, bytes_per_second % 1000);

    // restart
    context->test_data_start = now;
    context->test_data_sent  = 0;
}
/* LISTING_END(tracking): Tracking throughput */

/* 
 * @section Packet Handler
 *
 * @text The packet handler is used to stop the notifications and reset the MTU on connect
 * It would also be a good place to request the connection parameter update as indicated 
 * in the commented code block.
 */

/* LISTING_START(packetHandler): Packet Handler */
static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    
    int mtu;
    uint16_t conn_interval;
    nordic_spp_le_streamer_connection_t * context;
    switch (packet_type) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    // BTstack activated, get started
                    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                        printf("To start the streaming, please run the AMB2621 Toolbox to connect.\n");
                    } 
                    break;
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    context = connection_for_conn_handle(hci_event_disconnection_complete_get_connection_handle(packet));
                    if (!context) break;
                    // free connection
                    printf("%c: Disconnect, reason %02x\n", context->name, hci_event_disconnection_complete_get_reason(packet));                    
                    context->le_notification_enabled = 0;
                    context->connection_handle = HCI_CON_HANDLE_INVALID;
                    break;
                case HCI_EVENT_LE_META:
                    switch (hci_event_le_meta_get_subevent_code(packet)) {
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                            // setup new 
                            context = connection_for_conn_handle(HCI_CON_HANDLE_INVALID);
                            if (!context) break;
                            context->counter = 'A';
                            context->test_data_len = ATT_DEFAULT_MTU - 4;   // -1 for nordic 0x01 packet type
                            context->connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                            // print connection parameters (without using float operations)
                            conn_interval = hci_subevent_le_connection_complete_get_conn_interval(packet);
                            printf("%c: Connection Interval: %u.%02u ms\n", context->name, conn_interval * 125 / 100, 25 * (conn_interval & 3));
                            printf("%c: Connection Latency: %u\n", context->name, hci_subevent_le_connection_complete_get_conn_latency(packet));
                            break;
                    }
                    break;  
                case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
                    mtu = att_event_mtu_exchange_complete_get_MTU(packet) - 3;
                    context = connection_for_conn_handle(att_event_mtu_exchange_complete_get_handle(packet));
                    if (!context) break;
                    context->test_data_len = btstack_min(mtu - 3, sizeof(context->test_data));
                    printf("%c: ATT MTU = %u => use test data of len %u\n", context->name, mtu, context->test_data_len);
                    break;
                default:
                    break;
            }
    }
}

/* LISTING_END */
/*
 * @section Streamer
 *
 * @text The streamer function checks if notifications are enabled and if a notification can be sent now.
 * It creates some test data - a single letter that gets increased every time - and tracks the data sent.
 */

 /* LISTING_START(streamer): Streaming code */
static void nordic_can_send(void * some_context){
    UNUSED(some_context);

    // find next active streaming connection
    int old_connection_index = connection_index;
    while (1){
        // active found?
        if ((nordic_spp_le_streamer_connections[connection_index].connection_handle != HCI_CON_HANDLE_INVALID) &&
            (nordic_spp_le_streamer_connections[connection_index].le_notification_enabled)) break;
        
        // check next
        next_connection_index();

        // none found
        if (connection_index == old_connection_index) return;
    }

    nordic_spp_le_streamer_connection_t * context = &nordic_spp_le_streamer_connections[connection_index];

    // create test data
    context->counter++;
    if (context->counter > 'Z') context->counter = 'A';
    memset(context->test_data, context->counter, context->test_data_len);

    // send
    nordic_spp_service_server_send(context->connection_handle, (uint8_t*) context->test_data, context->test_data_len);

    // track
    test_track_sent(context, context->test_data_len);

    // request next send event
    nordic_spp_service_server_request_can_send_now(&context->send_request, context->connection_handle);

    // check next
    next_connection_index();
} 
/* LISTING_END */

static void nordic_data_received(hci_con_handle_t tx_con_handle, const uint8_t * data, uint16_t size){
    nordic_spp_le_streamer_connection_t * context = connection_for_conn_handle(tx_con_handle);

    if (!context) return;

    if (size == 0 && context->le_notification_enabled == 0){
        context->le_notification_enabled = 1;
        test_reset(context);
        context->send_request.callback = &nordic_can_send;
        nordic_spp_service_server_request_can_send_now(&context->send_request, context->connection_handle);
    } else {
        printf("RECV: ");
        printf_hexdump(data, size);
        test_track_sent(context, size);
   }
}

int btstack_main(void);
int btstack_main(void){
    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    l2cap_init();

    // setup LE device DB
    le_device_db_init();

    // setup SM: Display only
    sm_init();

    // setup ATT server
    att_server_init(profile_data, NULL, NULL);    
    
    // setup Nordic SPP service
    nordic_spp_service_server_init(&nordic_data_received);

    // setup advertisements
    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    uint8_t adv_type = 0;
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(adv_data_len, (uint8_t*) adv_data);
    gap_advertisements_enable(1);

    // init client state
    init_connections();

    // turn on!
	hci_power_control(HCI_POWER_ON);
	    
    return 0;
}
/* EXAMPLE_END */
