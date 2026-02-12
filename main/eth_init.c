/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ethernet initialization for Olimex ESP32-P4-DevKit.
 * Uses the on-board IP101GR PHY connected via RMII to the ESP32-P4 internal EMAC.
 */

#include "eth_init.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"

static const char *TAG = "eth_init";

/*
 * Olimex ESP32-P4-DevKit Ethernet pin configuration.
 * IP101GR PHY connected via RMII to internal EMAC.
 */
#define ETH_MDC_GPIO       31
#define ETH_MDIO_GPIO      52
#define ETH_PHY_RST_GPIO   51
#define ETH_PHY_ADDR        1

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "  Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "  Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
}

esp_err_t eth_init(void)
{
    /* Initialize TCP/IP stack and event loop (safe to call if already done) */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "TCP/IP init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop create failed");

    /* Create default netif for Ethernet */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    /* Configure MAC (internal EMAC) */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "MAC create failed");

    /* Configure PHY (IP101GR) */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    ESP_RETURN_ON_FALSE(phy, ESP_FAIL, TAG, "PHY create failed");

    /* Install Ethernet driver */
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&config, &eth_handle),
                        TAG, "Driver install failed");

    /* Attach Ethernet driver to TCP/IP stack */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(eth_netif, glue),
                        TAG, "Netif attach failed");

    /* Register event handlers */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                   &eth_event_handler, NULL),
        TAG, "ETH event handler register failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                   &got_ip_event_handler, NULL),
        TAG, "IP event handler register failed");

    /* Start Ethernet — DHCP runs automatically */
    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "Ethernet start failed");

    ESP_LOGI(TAG, "Ethernet initialized (IP101GR PHY, DHCP)");
    ESP_LOGI(TAG, "  MDC=%d MDIO=%d RST=%d PHY_ADDR=%d",
             ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_PHY_RST_GPIO, ETH_PHY_ADDR);

    return ESP_OK;
}
