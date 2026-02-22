#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#if (ENABLE_WIFI_AND_MQTT == 1)

void init_mqtt_HAL(void);
bool getIsWifiConnected_HAL();
void mqtt_loop_HAL();
bool publishMQTTMessage_HAL(const char *topic, const char *payload);
bool mqtt_is_configured_HAL();
bool mqtt_is_connected_HAL();
bool mqtt_get_broker_config_HAL(std::string* out_host, uint16_t* out_port, std::string* out_user,
                                std::string* out_pass, std::string* out_client_name);
bool mqtt_set_broker_config_HAL(const std::string& host, uint16_t port, const std::string& user,
                                const std::string& pass, const std::string& client_name);
bool mqtt_clear_broker_config_HAL();
void wifi_request_time_sync_HAL();
void wifi_shutdown_HAL();
bool wifi_scan_networks_HAL(std::vector<std::string>* out_ssids);
bool wifi_set_credentials_HAL(const std::string& ssid, const std::string& password);
bool wifi_get_saved_credentials_HAL(std::string* out_ssid, std::string* out_password);
bool wifi_clear_saved_credentials_HAL();
bool wifi_connect_saved_HAL();

typedef void (*tAnnounceWiFiconnected_cb)(bool connected);
void set_announceWiFiconnected_cb_HAL(tAnnounceWiFiconnected_cb pAnnounceWiFiconnected_cb);
typedef void (*tAnnounceSubscribedTopics_cb)(std::string topic, std::string payload);
void set_announceSubscribedTopics_cb_HAL(tAnnounceSubscribedTopics_cb pAnnounceSubscribedTopics_cb);

#endif
