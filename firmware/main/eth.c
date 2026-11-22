#include "eth.h"

#include <stdatomic.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

static atomic_bool s_link_up;
static atomic_bool s_has_ip;

bool eth_link_up(void)
{
    return atomic_load(&s_link_up);
}

bool eth_has_ip(void)
{
    return atomic_load(&s_has_ip);
}

static void log_ioctl_err(const char *what, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ioctl %s: %s", what, esp_err_to_name(err));
    }
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_eth_handle_t h = *(esp_eth_handle_t *)data;

    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {0};
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        log_ioctl_err("MAC", esp_eth_ioctl(h, ETH_CMD_G_MAC_ADDR, mac));
        log_ioctl_err("speed", esp_eth_ioctl(h, ETH_CMD_G_SPEED, &speed));
        log_ioctl_err("duplex", esp_eth_ioctl(h, ETH_CMD_G_DUPLEX_MODE, &duplex));
        atomic_store(&s_link_up, true);
        ESP_LOGI(TAG, "link up %s %s, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 speed == ETH_SPEED_100M ? "100M" : "10M",
                 duplex == ETH_DUPLEX_FULL ? "full" : "half",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        atomic_store(&s_link_up, false);
        atomic_store(&s_has_ip, false);
        ESP_LOGW(TAG, "link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "stopped");
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    atomic_store(&s_has_ip, true);
    ESP_LOGI(TAG, "IP " IPSTR " mask " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip),
             IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
}

esp_err_t eth_start(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_GW_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_GW_ETH_PHY_RST_GPIO;

    /* RMII data pins and the 50 MHz clock input (GPIO50) use the P4 defaults,
     * which match the Waveshare board. */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = CONFIG_GW_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = CONFIG_GW_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_FAIL, TAG, "MAC create failed");

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (phy == NULL) {
        mac->del(mac);
        ESP_LOGE(TAG, "IP101 PHY create failed");
        return ESP_FAIL;
    }

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t h = NULL;
    esp_err_t err = esp_eth_driver_install(&cfg, &h);
    if (err != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        ESP_LOGE(TAG, "driver install: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "netif create failed");

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(h);
    ESP_RETURN_ON_FALSE(glue != NULL, ESP_FAIL, TAG, "netif glue create failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, glue), TAG, "netif attach");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   on_eth_event, NULL),
                        TAG, "register ETH_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   on_got_ip, NULL),
                        TAG, "register IP_EVENT");
    ESP_RETURN_ON_ERROR(esp_eth_start(h), TAG, "eth start");
    return ESP_OK;
}
