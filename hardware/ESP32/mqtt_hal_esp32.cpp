#include <sstream>
#include <algorithm>
#include <vector>
#include <Arduino.h>
#include <cstring>
#include <time.h>
#include "WiFi.h"
#include <PubSubClient.h>
#include <Preferences.h>
#include "mqtt_hal_esp32.h"
#if (ENABLE_KEYBOARD_BLE == 1)
#include "keyboard_ble_hal_esp32.h"
#endif
#include "secrets.h"

#if (ENABLE_WIFI_AND_MQTT == 1)
WiFiClient espClient;
PubSubClient mqttClient(espClient);
bool isWifiConnected = false;
bool wifiShutdownRequested = false;
bool wifiCredentialsLoaded = false;
std::string savedWifiSsid = "";
std::string savedWifiPassword = "";
bool mqttConfigLoaded = false;
std::string savedMqttHost = "";
uint16_t savedMqttPort = 0;
std::string savedMqttUser = "";
std::string savedMqttPass = "";
std::string savedMqttClientName = "";
constexpr const char* kWifiPrefsNs = "omotev2wifi";
constexpr const char* kWifiSsidKey = "ssid";
constexpr const char* kWifiPasswordKey = "password";
constexpr const char* kMqttPrefsNs = "omotev2mqtt";
constexpr const char* kMqttHostKey = "host";
constexpr const char* kMqttPortKey = "port";
constexpr const char* kMqttUserKey = "user";
constexpr const char* kMqttPassKey = "pass";
constexpr const char* kMqttClientKey = "client";
constexpr const char* kDefaultPlaceholderMqttServer = "IPAddressOfYourBroker";
constexpr const char* kNtpServer1 = "pool.ntp.org";
constexpr const char* kNtpServer2 = "time.nist.gov";
constexpr const char* kNtpServer3 = "time.google.com";
constexpr unsigned long kWifiReconnectThrottleMs = 3000;
constexpr unsigned long kMqttReconnectBaseIntervalMs = 5000;
constexpr unsigned long kMqttReconnectMaxIntervalMs = 60000;
constexpr unsigned long kMqttFailureLogIntervalMs = 10000;
constexpr unsigned long kMqttConfigLogIntervalMs = 60000;
constexpr unsigned long kTimeSyncLogIntervalMs = 30000;
unsigned long lastWifiReconnectAttemptMs = 0;
unsigned long mqttReconnectIntervalMs = kMqttReconnectBaseIntervalMs;
unsigned long lastReconnectAttempt = 0;
unsigned long lastMqttFailureLogMs = 0;
unsigned long lastMqttConfigLogMs = 0;
unsigned long lastTimeSyncRequestMs = 0;
unsigned long lastTimeSyncLogMs = 0;

tAnnounceWiFiconnected_cb thisAnnounceWiFiconnected_cb = NULL;
void set_announceWiFiconnected_cb_HAL(tAnnounceWiFiconnected_cb pAnnounceWiFiconnected_cb) {
  thisAnnounceWiFiconnected_cb = pAnnounceWiFiconnected_cb;  
}

tAnnounceSubscribedTopics_cb thisAnnounceSubscribedTopics_cb = NULL;
void set_announceSubscribedTopics_cb_HAL(tAnnounceSubscribedTopics_cb pAnnounceSubscribedTopics_cb) {
  thisAnnounceSubscribedTopics_cb = pAnnounceSubscribedTopics_cb;  
}

static void announce_wifi_connected(bool connected) {
  if (thisAnnounceWiFiconnected_cb != NULL) {
    thisAnnounceWiFiconnected_cb(connected);
  }
}

static void announce_subscribed_topic(const std::string& topic, const std::string& payload) {
  if (thisAnnounceSubscribedTopics_cb != NULL) {
    thisAnnounceSubscribedTopics_cb(topic, payload);
  }
}

static bool is_default_placeholder_ssid(const std::string& ssid) {
  return ssid == "YourWifiSSID";
}

static std::string trim_copy(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) ++start;
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) --end;
  return in.substr(start, end - start);
}

static bool is_default_placeholder_mqtt_server(const std::string& server) {
  return server.empty() || server == kDefaultPlaceholderMqttServer;
}

static void load_mqtt_config_once() {
  if (mqttConfigLoaded) return;
  Preferences prefs;
  if (prefs.begin(kMqttPrefsNs, true)) {
    savedMqttHost = std::string(prefs.getString(kMqttHostKey, "").c_str());
    savedMqttPort = static_cast<uint16_t>(prefs.getUInt(kMqttPortKey, 0));
    savedMqttUser = std::string(prefs.getString(kMqttUserKey, "").c_str());
    savedMqttPass = std::string(prefs.getString(kMqttPassKey, "").c_str());
    savedMqttClientName = std::string(prefs.getString(kMqttClientKey, "").c_str());
    prefs.end();
  }
  mqttConfigLoaded = true;
}

static bool save_mqtt_config_internal(const std::string& host, uint16_t port, const std::string& user,
                                      const std::string& pass, const std::string& client_name) {
  Preferences prefs;
  if (!prefs.begin(kMqttPrefsNs, false)) return false;
  prefs.putString(kMqttHostKey, host.c_str());
  prefs.putUInt(kMqttPortKey, static_cast<uint32_t>(port));
  prefs.putString(kMqttUserKey, user.c_str());
  prefs.putString(kMqttPassKey, pass.c_str());
  prefs.putString(kMqttClientKey, client_name.c_str());
  prefs.end();
  savedMqttHost = host;
  savedMqttPort = port;
  savedMqttUser = user;
  savedMqttPass = pass;
  savedMqttClientName = client_name;
  mqttConfigLoaded = true;
  return true;
}

static bool clear_mqtt_config_internal() {
  Preferences prefs;
  if (!prefs.begin(kMqttPrefsNs, false)) return false;
  prefs.remove(kMqttHostKey);
  prefs.remove(kMqttPortKey);
  prefs.remove(kMqttUserKey);
  prefs.remove(kMqttPassKey);
  prefs.remove(kMqttClientKey);
  prefs.end();
  savedMqttHost.clear();
  savedMqttPort = 0;
  savedMqttUser.clear();
  savedMqttPass.clear();
  savedMqttClientName.clear();
  mqttConfigLoaded = true;
  return true;
}

static void get_effective_mqtt_config(std::string* out_host, uint16_t* out_port, std::string* out_user,
                                      std::string* out_pass, std::string* out_client_name) {
  load_mqtt_config_once();
  if (out_host != nullptr) {
    *out_host = !savedMqttHost.empty() ? savedMqttHost : std::string(MQTT_SERVER);
  }
  if (out_port != nullptr) {
    if (savedMqttPort > 0) {
      *out_port = savedMqttPort;
    } else {
      *out_port = static_cast<uint16_t>(MQTT_SERVER_PORT);
    }
  }
  if (out_user != nullptr) {
    *out_user = !savedMqttUser.empty() ? savedMqttUser : std::string(MQTT_USER);
  }
  if (out_pass != nullptr) {
    *out_pass = !savedMqttPass.empty() ? savedMqttPass : std::string(MQTT_PASS);
  }
  if (out_client_name != nullptr) {
    *out_client_name = !savedMqttClientName.empty() ? savedMqttClientName : std::string(MQTT_CLIENTNAME);
    if (out_client_name->empty()) {
      *out_client_name = "OMOTE";
    }
  }
}

static bool has_valid_mqtt_config() {
  std::string host;
  uint16_t port = 0;
  get_effective_mqtt_config(&host, &port, nullptr, nullptr, nullptr);
  return !is_default_placeholder_mqtt_server(host) && (port > 0);
}

static void request_time_sync_internal(const char* reason) {
  const unsigned long now = millis();
  if (now - lastTimeSyncRequestMs < 2000) return;
  lastTimeSyncRequestMs = now;

  const char* tz = getenv("TZ");
  if (tz == nullptr || strlen(tz) == 0) {
    tz = "UTC0";
  }
  configTzTime(tz, kNtpServer1, kNtpServer2, kNtpServer3);

  bool should_log = (reason != nullptr && reason[0] != '\0');
  if (!should_log && (now - lastTimeSyncLogMs >= kTimeSyncLogIntervalMs)) {
    should_log = true;
  }
  if (should_log) {
    if (reason != nullptr && reason[0] != '\0') {
      Serial.printf("NTP sync requested (%s)\r\n", reason);
    } else {
      Serial.printf("NTP sync requested\r\n");
    }
    lastTimeSyncLogMs = now;
  }
}

static void load_wifi_credentials_once() {
  if (wifiCredentialsLoaded) return;
  Preferences prefs;
  if (prefs.begin(kWifiPrefsNs, true)) {
    savedWifiSsid = std::string(prefs.getString(kWifiSsidKey, "").c_str());
    savedWifiPassword = std::string(prefs.getString(kWifiPasswordKey, "").c_str());
    prefs.end();
  }
  wifiCredentialsLoaded = true;
}

static bool save_wifi_credentials_internal(const std::string& ssid, const std::string& password) {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNs, false)) return false;
  prefs.putString(kWifiSsidKey, ssid.c_str());
  prefs.putString(kWifiPasswordKey, password.c_str());
  prefs.end();
  savedWifiSsid = ssid;
  savedWifiPassword = password;
  wifiCredentialsLoaded = true;
  return true;
}

static bool clear_wifi_credentials_internal() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNs, false)) return false;
  prefs.remove(kWifiSsidKey);
  prefs.remove(kWifiPasswordKey);
  prefs.end();
  savedWifiSsid.clear();
  savedWifiPassword.clear();
  wifiCredentialsLoaded = true;
  return true;
}

static bool get_effective_wifi_credentials(std::string* out_ssid, std::string* out_password) {
  if (out_ssid == NULL || out_password == NULL) return false;
  load_wifi_credentials_once();
  if (!savedWifiSsid.empty()) {
    *out_ssid = savedWifiSsid;
    *out_password = savedWifiPassword;
    return true;
  }

  const std::string compiledSsid = WIFI_SSID;
  if (compiledSsid.empty() || is_default_placeholder_ssid(compiledSsid)) return false;
  *out_ssid = compiledSsid;
  *out_password = WIFI_PASSWORD;
  return true;
}

static bool start_wifi_with_effective_credentials() {
  std::string ssid;
  std::string password;
  if (!get_effective_wifi_credentials(&ssid, &password)) {
    Serial.printf("WiFi settings missing. Configure SSID/password in Settings -> WiFi.\r\n");
    return false;
  }

  WiFi.mode(WIFI_STA);
  wifiShutdownRequested = false;
  lastWifiReconnectAttemptMs = millis();
  WiFi.begin(ssid.c_str(), password.c_str());
  return true;
}

bool getIsWifiConnected_HAL() {
  return isWifiConnected;
}

bool mqtt_is_configured_HAL() {
  return has_valid_mqtt_config();
}

bool mqtt_is_connected_HAL() {
  return mqttClient.connected();
}

bool mqtt_get_broker_config_HAL(std::string* out_host, uint16_t* out_port, std::string* out_user,
                                std::string* out_pass, std::string* out_client_name) {
  if (out_host == NULL || out_port == NULL || out_user == NULL || out_pass == NULL || out_client_name == NULL) {
    return false;
  }
  get_effective_mqtt_config(out_host, out_port, out_user, out_pass, out_client_name);
  return true;
}

bool mqtt_set_broker_config_HAL(const std::string& host, uint16_t port, const std::string& user,
                                const std::string& pass, const std::string& client_name) {
  const std::string normalized_host = host;
  if (trim_copy(normalized_host).empty() || port == 0) return false;
  if (!save_mqtt_config_internal(trim_copy(normalized_host), port, user, pass, trim_copy(client_name))) return false;
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }
  mqttReconnectIntervalMs = kMqttReconnectBaseIntervalMs;
  lastReconnectAttempt = 0;
  return true;
}

bool mqtt_clear_broker_config_HAL() {
  if (!clear_mqtt_config_internal()) return false;
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }
  mqttReconnectIntervalMs = kMqttReconnectBaseIntervalMs;
  lastReconnectAttempt = 0;
  return true;
}

void wifi_request_time_sync_HAL() {
  request_time_sync_internal("manual");
}

// WiFi status event
void WiFiEvent(WiFiEvent_t event){
  //Serial.printf("[WiFi-event] event: %d\r\n", event);
  if(event == ARDUINO_EVENT_WIFI_STA_GOT_IP){
    // connection to MQTT server will be done in checkMQTTconnection()
    // mqttClient.setServer(MQTT_SERVER, 1883); // MQTT initialization
    // mqttClient.connect("OMOTE"); // Connect using a client id

  }

  // Set status bar icon based on WiFi status
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP || event == ARDUINO_EVENT_WIFI_STA_GOT_IP6) {
    isWifiConnected = true;
    wifiShutdownRequested = false;
    mqttReconnectIntervalMs = kMqttReconnectBaseIntervalMs;
    announce_wifi_connected(true);
    Serial.printf("WiFi connected, IP address: %s\r\n", WiFi.localIP().toString().c_str());
    request_time_sync_internal("wifi connected");

  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    isWifiConnected = false;
    announce_wifi_connected(false);
    if (wifiShutdownRequested || WiFi.getMode() == WIFI_OFF) {
      // Intentional disconnect during sleep/power down.
      return;
    }
    // automatically try to reconnect, throttled
    const unsigned long now = millis();
    if (now - lastWifiReconnectAttemptMs >= kWifiReconnectThrottleMs) {
      Serial.printf("WiFi disconnected. Reconnecting...\r\n");
      start_wifi_with_effective_credentials();
    }

  } else {
    // e.g. ARDUINO_EVENT_WIFI_STA_CONNECTED or many others
    // connected is not enough, will wait for IP
    isWifiConnected = false;
    announce_wifi_connected(false);

  }
}

void init_mqtt_HAL(void) {
  // Setup WiFi
  WiFi.setHostname("OMOTE"); //define hostname
  WiFi.onEvent(WiFiEvent);
  wifiShutdownRequested = false;
  mqttReconnectIntervalMs = kMqttReconnectBaseIntervalMs;
  lastReconnectAttempt = 0;
  lastMqttFailureLogMs = 0;
  lastMqttConfigLogMs = 0;
  start_wifi_with_effective_credentials();
  WiFi.setSleep(true);
  request_time_sync_internal("boot");
}

std::string subscribeTopicOMOTEtest = "OMOTE/test";
// For connecting to one or several BLE clients
std::string subscribeTopicOMOTE_BLEstartAdvertisingForAll        = "OMOTE/BLE/startAdvertisingForAll";
std::string subscribeTopicOMOTE_BLEstartAdvertisingWithWhitelist = "OMOTE/BLE/startAdvertisingWithWhitelist";
std::string subscribeTopicOMOTE_BLEstartAdvertisingDirected      = "OMOTE/BLE/startAdvertisingDirected";
std::string subscribeTopicOMOTE_BLEstopAdvertising               = "OMOTE/BLE/stopAdvertising";
std::string subscribeTopicOMOTE_BLEprintConnectedClients         = "OMOTE/BLE/printConnectedClients";
std::string subscribeTopicOMOTE_BLEdisconnectAllClients          = "OMOTE/BLE/disconnectAllClients";
std::string subscribeTopicOMOTE_BLEprintBonds                    = "OMOTE/BLE/printBonds";
std::string subscribeTopicOMOTE_BLEdeleteBonds                   = "OMOTE/BLE/deleteBonds";

void callback(char* topic, byte* payload, unsigned int length) {
  // handle message arrived
  std::string topicReceived(topic);
  std::string strPayload(reinterpret_cast<const char *>(payload), length);
  Serial.printf("MQTT: received topic %s with payload %s\r\n", topicReceived.c_str(), strPayload.c_str());

  if (topicReceived == subscribeTopicOMOTEtest) {
    // Do whatever you want here, if it is ESP32 hardware related.
    // ...

    // Or forward the topic to "void receiveMQTTmessage_cb" in the "commandHandler.cpp", if it is not ESP32 hardware related
    announce_subscribed_topic(topicReceived, strPayload);

  #if (ENABLE_KEYBOARD_BLE == 1)
  } else if (topicReceived == subscribeTopicOMOTE_BLEstartAdvertisingForAll) {
    keyboardBLE_startAdvertisingForAll_HAL();  
  } else if (topicReceived == subscribeTopicOMOTE_BLEstartAdvertisingWithWhitelist) {
    keyboardBLE_startAdvertisingWithWhitelist_HAL(strPayload);  
  } else if (topicReceived == subscribeTopicOMOTE_BLEstartAdvertisingDirected) {
    // the payload are two values, separated by comma: peerAddress and isRandomAddress 
    std::stringstream ss(strPayload);
    if (ss.good())  {
      std::string peerAddress;
      std::getline(ss, peerAddress, ',');
      
      if (ss.good())  {
        std::string isRandomAddressStr;
        std::getline(ss, isRandomAddressStr, ',');
        bool isRandomAddress = false;
        if (isRandomAddressStr == "true") {
          isRandomAddress = true;  
        }
        keyboardBLE_startAdvertisingDirected_HAL(peerAddress, isRandomAddress);  
      }
    }
  } else if (topicReceived == subscribeTopicOMOTE_BLEstopAdvertising) {
    keyboardBLE_stopAdvertising_HAL();  
  } else if (topicReceived == subscribeTopicOMOTE_BLEprintConnectedClients) {
    keyboardBLE_printConnectedClients_HAL();
  } else if (topicReceived == subscribeTopicOMOTE_BLEdisconnectAllClients) {
    keyboardBLE_disconnectAllClients_HAL();  
  } else if (topicReceived == subscribeTopicOMOTE_BLEprintBonds) {
    keyboardBLE_printBonds_HAL();  
  } else if (topicReceived == subscribeTopicOMOTE_BLEdeleteBonds) {
    keyboardBLE_deleteBonds_HAL();  
  #endif

  } else {
    // forward all other topics to the commandHandler
    announce_subscribed_topic(topicReceived, strPayload);

  }
}

void mqtt_subscribeTopics() {
  mqttClient.setCallback(&callback);

  mqttClient.subscribe(subscribeTopicOMOTEtest.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEstartAdvertisingForAll.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEstartAdvertisingWithWhitelist.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEstartAdvertisingDirected.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEstopAdvertising.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEprintConnectedClients.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEdisconnectAllClients.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEprintBonds.c_str());
  mqttClient.subscribe(subscribeTopicOMOTE_BLEdeleteBonds.c_str());
  Serial.printf("  Successfully subscribed to MQTT topics\r\n");

}

bool checkMQTTconnection() {
  if (WiFi.getMode() == WIFI_OFF || wifiShutdownRequested) {
    return false;
  }
  if (!WiFi.isConnected()) {
    return false;
  }
  if (!has_valid_mqtt_config()) {
    const unsigned long now = millis();
    if (now - lastMqttConfigLogMs >= kMqttConfigLogIntervalMs) {
      lastMqttConfigLogMs = now;
      Serial.printf("MQTT disabled: configure MQTT_SERVER in secrets_override.h\r\n");
    }
    return false;
  }
  if (mqttClient.connected()) {
    return true;
  }

  std::string mqtt_host;
  uint16_t mqtt_port = 0;
  std::string mqtt_user;
  std::string mqtt_pass;
  std::string mqtt_client_name;
  get_effective_mqtt_config(&mqtt_host, &mqtt_port, &mqtt_user, &mqtt_pass, &mqtt_client_name);

  // try to connect to mqtt server
  mqttClient.setBufferSize(512);   // default is 256
  mqttClient.setServer(mqtt_host.c_str(), mqtt_port); // MQTT initialization

  std::string mqttClientName = mqtt_client_name + "_esp32_" + std::string(WiFi.macAddress().c_str());
  if (mqttClient.connect(mqttClientName.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
    Serial.printf("  Successfully connected to MQTT broker\r\n");
    mqtt_subscribeTopics();
    mqttReconnectIntervalMs = kMqttReconnectBaseIntervalMs;
    return true;
  }

  // Exponential backoff (capped) and throttled logging to prevent error spam.
  mqttReconnectIntervalMs = std::min(mqttReconnectIntervalMs * 2, kMqttReconnectMaxIntervalMs);
  const unsigned long now = millis();
  if (now - lastMqttFailureLogMs >= kMqttFailureLogIntervalMs) {
    lastMqttFailureLogMs = now;
    Serial.printf("  MQTT connect failed. Retrying in %lu ms...\r\n", mqttReconnectIntervalMs);
  }
  return false;
}
void mqtt_loop_HAL() {
  if (wifiShutdownRequested || WiFi.getMode() == WIFI_OFF || !WiFi.isConnected()) {
    return;
  }
  if (!has_valid_mqtt_config()) {
    return;
  }
  if (!mqttClient.connected()) {
    unsigned long currentMillis = millis();
    if ((currentMillis - lastReconnectAttempt) > mqttReconnectIntervalMs) {
      lastReconnectAttempt = currentMillis;
      // Attempt to reconnect
      checkMQTTconnection();
    }
  }  

  if (mqttClient.connected()) {
    mqttClient.loop();
  }
}

bool publishMQTTMessage_HAL(const char *topic, const char *payload){
  if (wifiShutdownRequested || WiFi.getMode() == WIFI_OFF) {
    Serial.printf("MQTT publish skipped: WiFi is off\r\n");
    return false;
  }
  if (!has_valid_mqtt_config()) {
    Serial.printf("MQTT publish skipped: broker not configured\r\n");
    return false;
  }
  const char* safe_topic = (topic != nullptr) ? topic : "";
  const char* safe_payload = (payload != nullptr) ? payload : "";
  if (checkMQTTconnection()) {
    Serial.printf("MQTT publish attempt: topic=\"%s\" payload=\"%s\"\r\n", safe_topic, safe_payload);
      
    if (mqttClient.publish(safe_topic, safe_payload)) {
      Serial.printf("MQTT publish ok: %s\r\n", safe_topic);
      return true;
    }
    else {
      const unsigned long now = millis();
      if (now - lastMqttFailureLogMs >= kMqttFailureLogIntervalMs) {
        lastMqttFailureLogMs = now;
        Serial.printf("MQTT publish failed: %s\r\n", safe_topic);
      }
    }
  }
  return false;
}

void wifi_shutdown_HAL() {
  wifiShutdownRequested = true;
  isWifiConnected = false;
  announce_wifi_connected(false);
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
}

bool wifi_scan_networks_HAL(std::vector<std::string>* out_ssids) {
  if (out_ssids == NULL) return false;
  out_ssids->clear();

  WiFi.mode(WIFI_STA);
  int count = WiFi.scanNetworks();
  if (count < 0) {
    return false;
  }

  out_ssids->reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    std::string ssid = std::string(WiFi.SSID(i).c_str());
    if (ssid.empty()) continue;
    if (std::find(out_ssids->begin(), out_ssids->end(), ssid) != out_ssids->end()) continue;
    out_ssids->push_back(ssid);
  }
  WiFi.scanDelete();
  return true;
}

bool wifi_set_credentials_HAL(const std::string& ssid, const std::string& password) {
  if (ssid.empty()) return false;
  if (!save_wifi_credentials_internal(ssid, password)) {
    return false;
  }

  wifiShutdownRequested = false;
  WiFi.disconnect();
  delay(50);
  return start_wifi_with_effective_credentials();
}

bool wifi_get_saved_credentials_HAL(std::string* out_ssid, std::string* out_password) {
  if (out_ssid == NULL || out_password == NULL) return false;
  load_wifi_credentials_once();
  *out_ssid = savedWifiSsid;
  *out_password = savedWifiPassword;
  return !savedWifiSsid.empty();
}

bool wifi_clear_saved_credentials_HAL() {
  wifiShutdownRequested = false;
  WiFi.disconnect();
  return clear_wifi_credentials_internal();
}

bool wifi_connect_saved_HAL() {
  wifiShutdownRequested = false;
  return start_wifi_with_effective_credentials();
}

#endif
