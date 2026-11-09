#if defined(ARDUINO_M5Stick_C_Plus2) || defined(ARDUINO_M5Stick_C_Plus) || defined(ARDUINO_M5Stick_C) || defined(ARDUINO_M5Stack_Tough) || defined(ARDUINO_M5Stick_S3)
#include <M5Unified.h>
#else
#include <Arduino.h>
#endif

#ifdef ARDUINO_ARCH_ESP8266
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <HardwareSerial.h>

#include <PubSubClient.h>
#include <ArduinoOTA.h>

#if __has_include("my_setup.h")
#include "my_setup.h"
#else
#include "setup.h"
#endif

#include "mqttserial.h"
#include "converters.h"
#include "comm.h"
#include "homeassistant.h"
#include "display.h"
#include "mqtt.h"
#include "restart.h"

Converter converter;
char registryIDs[32]; //Holds the registries to query
bool busy = false;

bool contains(char array[], int size, int value)
{
  for (int i = 0; i < size; i++)
  {
    if (array[i] == value)
      return true;
  }
  return false;
}

//Converts to string and add the value to the JSON message
void updateValues(char regID)
{
  LabelDef *labels[128];
  int num = 0;
  converter.getLabels(regID, labels, num);
  for (int i = 0; i < num; i++)
  {
    bool alpha = false;
    for (size_t j = 0; j < strlen(labels[i]->asString); j++)
    {
      char c = labels[i]->asString[j];
      if (!isdigit(c) && c!='.' && !(c=='-' && j==0)){
        alpha = true;
        break;
      }
    }

    #ifdef ONEVAL_ONETOPIC
    char topicBuff[128] = MQTT_OneTopic;
    strcat(topicBuff,labels[i]->label);
    client.publish(topicBuff, labels[i]->asString);

    #else
    if (alpha){      

      snprintf(jsonbuff + strlen(jsonbuff), MAX_MSG_SIZE - strlen(jsonbuff), "\"%s\":\"%s\",", labels[i]->label, labels[i]->asString);
    }
    else{//number, no quotes
      snprintf(jsonbuff + strlen(jsonbuff), MAX_MSG_SIZE - strlen(jsonbuff), "\"%s\":%s,", labels[i]->label, labels[i]->asString);
    }
    #endif
  }
}

uint16_t loopcount =0;

//Non blocking: also called while waiting for WiFi/MQTT so the buttons
//keep working even when there is no connection.
void handleScreen()
{
  displayTick();
}

void extraLoop()
{
  client.loop();
  ArduinoOTA.handle();
  while (busy)
  { //Stop processing during OTA
    ArduinoOTA.handle();
  }
  handleScreen();
}

#ifdef ARDUINO_ARCH_ESP8266
void get_wifi_bssid(const char *ssid, uint8_t *bssid, uint32_t *wifi_channel)
{
  bssid = nullptr;
  int n = WiFi.scanNetworks(false, true);

  if (n < 1) // no networks found
    return;

  // sort networks on RSSI value
  int indices[n];
  for (int i = 0; i < n; i++)
  {
    indices[i] = i;
  }

  for (int i = 0; i < n; i++)
  {
    for (int j = i + 1; j < n; j++)
    {
      if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i]))
      {
        std::swap(indices[i], indices[j]);
      }
    }
  }

  // loop through result and match highest RSSI SSID
  for (int i = 0; i < n; i++)
  {
    char scan_ssid[33]; // ssid can be up to 32chars, => plus null term
    strlcpy(scan_ssid, WiFi.SSID(indices[i]).c_str(), sizeof(scan_ssid));

    if (strcmp(ssid, scan_ssid) == 0)
    {
      if (WiFi.BSSID(indices[i]) != 0)
      {
        bssid = WiFi.BSSID(indices[i]);
        *wifi_channel = WiFi.channel();
        return;
      }
      else
      {
        return;
      }
    }
  }
}
#endif

//Stops all WiFi attempts until the button is pressed again, so a wrong password or a
//missing AP does not keep hammering the access point with connection attempts.
void pauseWifi()
{
  WiFi.disconnect(true);//true: also turns the radio off
  Serial.println("\nWiFi stopped from the button. Press it again to retry.");
  displayLog("WiFi stopped. %s to retry", BUTTON_HINT);
  while (!displayTick(false))//the front button resumes; a press on a dark screen only wakes it
  {
    delay(20);
  }
  Serial.println("Retrying WiFi...");
  displayLog("WiFi: retrying %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PWD, 0, 0, true);
}

void checkWifi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  noteActivity(ACT_WIFI);

  unsigned long lostTime = millis();
  unsigned long lastAttempt = millis();
  unsigned long lastDot = 0;
  displayWake();//Show we lost connection; keep the screen usable during the outage
  displayShowLog(true);
  displayLog("WiFi: connecting to %s", WIFI_SSID);
#ifdef HAS_M5_SCREEN
  displayLog("%s to stop retrying", BUTTON_HINT);
#endif
  while (WiFi.status() != WL_CONNECTED)
  {
    if (displayTick(false))//Keep the buttons responsive while disconnected
    { //The user asked to stop retrying
      pauseWifi();
      lostTime = lastAttempt = millis();
    }
    delay(50);
    if (millis() - lastDot >= 500)
    {
      Serial.print(".");
      lastDot = millis();
    }
    if (millis() - lastAttempt >= 15000)
    { //Auto-reconnect is not making it: force a full scan so we reattach to the strongest AP
      Serial.println("\nStill disconnected. Rescanning for strongest AP...");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PWD, 0, 0, true);
      lastAttempt = millis();
    }
    if (millis() - lostTime >= 120000)
    { //Still no WiFi after 2 min: reboot in case the WiFi stack is wedged
      Serial.printf("Tried connecting for 120 sec, rebooting now.");
      displayLog("No WiFi for 2 min, rebooting");
      restart_board();
    }
  }
  displayLog("WiFi OK: %s", WiFi.localIP().toString().c_str());
  displayShowLog(false);
}

#ifndef ARDUINO_ARCH_ESP8266
//With several APs sharing the same SSID, the ESP32 stays associated to its AP
//until the link fully drops, even if a much closer AP is available. Periodically
//check the signal and reattach to a significantly stronger AP of the same SSID.
#define ROAM_CHECK_INTERVAL 60000UL
#define ROAM_RSSI_THRESHOLD -75 //Only consider roaming when weaker than this
#define ROAM_MIN_IMPROVEMENT 8  //dB gain required to switch AP
unsigned long lastRoamCheck = 0;

void checkWifiRoaming()
{
  if (WiFi.status() != WL_CONNECTED || millis() - lastRoamCheck < ROAM_CHECK_INTERVAL)
    return;
  lastRoamCheck = millis();

  int32_t currentRSSI = WiFi.RSSI();
  if (currentRSSI >= ROAM_RSSI_THRESHOLD)
    return;

  noteActivity(ACT_WIFI_SCAN);
  int16_t n = WiFi.scanNetworks(false, true);
  int16_t bestIndex = -1;
  int32_t bestRSSI = currentRSSI + ROAM_MIN_IMPROVEMENT;
  for (int16_t i = 0; i < n; i++)
  {
    if (WiFi.SSID(i) == WIFI_SSID && WiFi.RSSI(i) > bestRSSI
        && memcmp(WiFi.BSSID(i), WiFi.BSSID(), 6) != 0)
    {
      bestRSSI = WiFi.RSSI(i);
      bestIndex = i;
    }
  }
  if (bestIndex >= 0)
  {
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(bestIndex), 6);
    int32_t channel = WiFi.channel(bestIndex);
    mqttSerial.printf("WiFi weak (%ddBm), roaming to stronger AP (%ddBm, ch%d)\n", currentRSSI, bestRSSI, channel);
    WiFi.scanDelete();
    noteActivity(ACT_WIFI_ROAM);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PWD, channel, bssid, true);
    checkWifi();
  }
  else
  {
    WiFi.scanDelete();
  }
}
#else
void checkWifiRoaming(){}
#endif

void setup_wifi()
{
  delay(10);
  // We start by connecting to a WiFi network
  mqttSerial.printf("Connecting to %s\n", WIFI_SSID);

  #if defined(WIFI_IP) && defined(WIFI_GATEWAY) && defined(WIFI_SUBNET)
    IPAddress local_IP(WIFI_IP);
    IPAddress gateway(WIFI_GATEWAY);
    IPAddress subnet(WIFI_SUBNET);

    #ifdef WIFI_PRIMARY_DNS
      IPAddress primaryDNS(WIFI_PRIMARY_DNS);
    #else
      IPAddress primaryDNS();
    #endif

    #ifdef WIFI_SECONDARY_DNS
      IPAddress secondaryDNS(WIFI_SECONDARY_DNS);
    #else
      IPAddress secondaryDNS();
    #endif

    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      mqttSerial.println("Failed to set static ip!");
    }
  #endif

  uint8_t *bssid = nullptr;
  uint32_t wifi_channel = 0;
#ifdef ARDUINO_ARCH_ESP8266
  get_wifi_bssid(WIFI_SSID, bssid, &wifi_channel);
#else //assume ESP32
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
#endif
    
  if (bssid != nullptr)
  {
    WiFi.begin(WIFI_SSID, WIFI_PWD, wifi_channel, bssid);
  }
  else
  {
    WiFi.begin(WIFI_SSID, WIFI_PWD, 0, 0, true);
  }
  WiFi.setAutoReconnect(true);
  checkWifi();
  mqttSerial.printf("Connected. IP Address: %s\n", WiFi.localIP().toString().c_str());
}

void initRegistries(){
    //getting the list of registries to query from the selected values
  for (size_t i = 0; i < sizeof(registryIDs); i++)
  {
    registryIDs[i]=0xff;
  }

  int i = 0;
  for (auto &&label : labelDefs)
  {
    if (!contains(registryIDs, sizeof(registryIDs), label.registryID))
    {
      mqttSerial.printf("Adding registry 0x%2x to be queried.\n", label.registryID);
      registryIDs[i++] = label.registryID;
    }
  }
  displayLog("Reading %d values from %d registers", (int)(sizeof(labelDefs) / sizeof(labelDefs[0])), i);
  if (i == 0)
  {
    mqttSerial.printf("ERROR - No values selected in the include file. Stopping.\n");
    displayLog("No values selected in the definition");
    while (true)
    {
      extraLoop();
    }
  }
}

void setup()
{
  describeLastReset();//read what the previous run was doing before it gets overwritten
  noteActivity(ACT_BOOT);
  Serial.begin(115200);
  displayBegin();
  MySerial.begin(9600, SERIAL_CONFIG, RX_PIN, TX_PIN);
  pinMode(PIN_THERM, OUTPUT);
  // digitalWrite(PIN_THERM, PIN_THERM_ACTIVE_STATE);

#ifdef SAFETY_RELAY_PIN
  pinMode(SAFETY_RELAY_PIN, OUTPUT);
  digitalWrite(SAFETY_RELAY_PIN, !SAFETY_RELAY_ACTIVE_STATE);
#endif

#ifdef PIN_SG1
  //Smartgrid pins - Set first to the inactive state, before configuring as outputs (avoid false triggering when initializing)
  digitalWrite(PIN_SG1, SG_RELAY_INACTIVE_STATE);
  digitalWrite(PIN_SG2, SG_RELAY_INACTIVE_STATE);
  pinMode(PIN_SG1, OUTPUT);
  pinMode(PIN_SG2, OUTPUT);

#endif
#ifdef ARDUINO_M5Stick_C_Plus
  gpio_pulldown_dis(GPIO_NUM_25);
  gpio_pullup_dis(GPIO_NUM_25);
#endif

  EEPROM.begin(10);
  readEEPROM();//Restore previous state
  mqttSerial.print("Setting up wifi...");
  setup_wifi();
  ArduinoOTA.setHostname("ESPAltherma");
  ArduinoOTA.onStart([]() {
    noteActivity(ACT_OTA);
    displayOtaStart();
    busy = true;
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    displayOtaProgress(done, total);
  });
  ArduinoOTA.onEnd([]() {
    displayOtaEnd(true, "Done, restarting...");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    mqttSerial.print("Error on OTA - restarting");
    const char *reason = error == OTA_AUTH_ERROR ? "auth" : error == OTA_BEGIN_ERROR ? "begin"
                       : error == OTA_CONNECT_ERROR ? "connect" : error == OTA_RECEIVE_ERROR ? "receive" : "end";
    char message[32];
    snprintf(message, sizeof(message), "Update failed (%s)", reason);
    displayOtaEnd(false, message);
    delay(2000);//leave the message on screen; the old firmware keeps running after the restart
    restart_board();
  });
  ArduinoOTA.begin();

  #ifdef MQTT_ENCRYPTED
  // Required to establish encrypted connections. 
  // If you want to be more secure here, you can use the CA certificate to allow the wifi client to verify the other party. NOTE: If you use the CA certificate here, then you need to make sure to update it here regulary!
  espClient.setInsecure();
  espClient.setTimeout(5);
  #endif

  client.setBufferSize(MAX_MSG_SIZE); //to support large json message
  client.setCallback(callback);
  client.setServer(MQTT_SERVER, MQTT_PORT);

  auto timeout = espClient.getTimeout();
  Serial.printf("Wifi client timeout: %d\n", timeout);

  mqttSerial.printf("Connecting to MQTT server: %s:%d\n", MQTT_SERVER, MQTT_PORT);
  mqttSerial.begin(&client, "espaltherma/log");
  displayLog("MQTT: connecting to %s", MQTT_SERVER);
  reconnectMqtt();
  mqttSerial.println("OK!");
  mqttSerial.printf("Last reset reason: %s\n", describeLastReset());
  publishCrashReport();

  initRegistries();
  mqttSerial.print("ESPAltherma started!");
}

void waitLoop(uint ms){
      unsigned long start = millis();
      while (millis() < start + ms) //wait .5sec between registries
      {
        extraLoop();
      }
}

void loop()
{
  unsigned long start = millis();
  if (WiFi.status() != WL_CONNECTED)
  { //restart board if needed
    checkWifi();
  }
  checkWifiRoaming();//Move to a stronger AP of the same SSID if signal got weak
  if (!client.connected())
  { //(re)connect to MQTT if needed
    reconnectMqtt();
  }
  //Querying all registries
  int registriesRead = 0, registriesQueried = 0;
  for (size_t i = 0; (i < 32) && registryIDs[i] != 0xFF; i++)
  {
    unsigned char buff[MAX_REPLY_LEN] = {0};
    int tries = 0;
    bool replied;
    noteActivity(ACT_QUERY, registryIDs[i]);
    while (!(replied = queryRegistry(registryIDs[i], buff, PROTOCOL)) && tries++ < 3)
    {
      mqttSerial.println("Retrying...");
      waitLoop(1000);
    }
    unsigned char receivedRegistryID = PROTOCOL == 'S' ? buff[0] : buff[1];
    registriesQueried++;
    displayRegisterRead(registryIDs[i], replied && registryIDs[i] == receivedRegistryID);
    if (registryIDs[i] == receivedRegistryID) //if replied registerID is coherent with the command
    {
      if (replied)
        registriesRead++;
      noteActivity(ACT_DECODE, registryIDs[i]);
      converter.readRegistryValues(buff, PROTOCOL); //process all values from the register
      updateValues(registryIDs[i]);       //send them in mqtt
      //waitLoop(500);//wait .5sec between registries
    }
  }
  noteActivity(ACT_PUBLISH);
  displayCycleDone(registriesRead, registriesQueried);
  sendValues();//Send the full json message
  mqttSerial.printf("Done. Waiting %ld ms...", FREQUENCY - millis() + start);
  noteActivity(ACT_WAIT);
  waitLoop(FREQUENCY - millis() + start);
}
