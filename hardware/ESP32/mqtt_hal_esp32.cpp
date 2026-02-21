#include <sstream>
#include <algorithm>
#include <vector>
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
bool wifiCredentialsLoaded = false;
std::string savedWifiSsid = "";
std::string savedWifiPassword = "";
constexpr const char* kWifiPrefsNs = "omotev2wifi";
constexpr const char* kWifiSsidKey = "ssid";
constexpr const char* kWifiPasswordKey = "password";

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
  WiFi.begin(ssid.c_str(), password.c_str());
  return true;
}

bool getIsWifiConnected_HAL() {
  return isWifiConnected;
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
    announce_wifi_connected(true);
    Serial.printf("WiFi connected, IP address: %s\r\n", WiFi.localIP().toString().c_str());

  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    isWifiConnected = false;
    announce_wifi_connected(false);
    // automatically try to reconnect
    Serial.printf("WiFi got disconnected. Will try to reconnect.\r\n");
    start_wifi_with_effective_credentials();

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
  start_wifi_with_effective_credentials();
  WiFi.setSleep(true);
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

  if (WiFi.isConnected()) {
    if (mqttClient.connected()) {
      return true;
    } else {
      // try to connect to mqtt server
      mqttClient.setBufferSize(512);   // default is 256
      //mqttClient.setKeepAlive(15);     // default is 15   Client will send MQTTPINGREQ to keep connection alive
      //mqttClient.setSocketTimeout(15); // default is 15   This determines how long the client will wait for incoming data when it expects data to arrive - for example, whilst it is in the middle of reading an MQTT packet.
      mqttClient.setServer(MQTT_SERVER, MQTT_SERVER_PORT); // MQTT initialization
      
      std::string mqttClientName = std::string(MQTT_CLIENTNAME) + "_esp32_" + std::string(WiFi.macAddress().c_str());
      if (mqttClient.connect(mqttClientName.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.printf("  Successfully connected to MQTT broker\r\n");
    
        mqtt_subscribeTopics();

      } else {
        Serial.printf("  MQTT connection failed (but WiFi is available). Will try later ...\r\n");

      }
      return mqttClient.connected();
    }
  } else {
    // Serial.printf("  No connection to MQTT server, because WiFi ist not connected.\r\n");
    return false;
  }  
}

unsigned long reconnectInterval = 100;
// in order to do reconnect immediately ...
unsigned long lastReconnectAttempt = millis() - reconnectInterval - 1;
void mqtt_loop_HAL() {
  if (!mqttClient.connected()) {
    unsigned long currentMillis = millis();
    if ((currentMillis - lastReconnectAttempt) > reconnectInterval) {
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

  if (checkMQTTconnection()) {
    // Serial.printf("Sending mqtt payload to topic \"%s\": %s\r\n", topic, payload);
      
    if (mqttClient.publish(topic, payload)) {
      // Serial.printf("Publish ok\r\n");
      return true;
    }
    else {
      Serial.printf("Publish failed\r\n");
    }
  } else {
    Serial.printf("  Cannot publish mqtt message, because checkMQTTconnection failed (WiFi or mqtt is not connected)\r\n");
  }
  return false;
}

void wifi_shutdown_HAL() {
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
  WiFi.disconnect();
  return clear_wifi_credentials_internal();
}

bool wifi_connect_saved_HAL() {
  return start_wifi_with_effective_credentials();
}

#endif
