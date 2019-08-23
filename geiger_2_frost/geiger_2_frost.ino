
/*
 * Sketch for Wemos mini D1 ESP8266 board in combination with a Geiger counter board
 * 
 * For building see FS note below!!
 * 
 * TODO:
 * - bigger circular buffer  (like 2 or 3 minutes)?
 * - sent also to radmon (https://radmon.org/index.php/forum/pyradmon/937-pyradmon-reborn-v2-0-0)
 *   curl -v "https://radmon.org/radmon.php?user=your_user&password=your_password&function=submit&datetime=2019-08-20%2021:57:34&value=18&unit=CPM"
 * - 
 */

// FS = File System to be able to save config
//
// IMPORTANT !!!! 
// You need to build this sketch with "Tools/Flash Size/4M (1M SPIFFS)"
// OR at list NOT(!) "Tools/Flash Size/4M (NO SPIFFS)" as then you will not have a filesystem
//
//this needs to be first, or it all crashes and burns... (according to this WifiManager example)
#include <FS.h>                   // https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html
                                  // https://github.com/pellepl/spiffs/

//needed to create json for config save AND frost messages
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

// https://arduino-esp8266.readthedocs.io/
#include <ESP8266WiFi.h>          // https://github.com/esp8266/Arduino

//needed for WifiManager library  // https://github.com/tzapu/WiFiManager
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

// For MQTT pub/sub messaging
#include <PubSubClient.h>         // https://github.com/knolleary/pubsubclient

//for LED status                  // From core, to blink led
#include <Ticker.h>
Ticker ticker;

//To be able to use networktime for timestamps
#include <ezTime.h>                // https://github.com/ropg/ezTime

// define your default values here
// if there are different values in the saved config.json, they are overwritten.
char mqtt_server[40] = "zuidt.nl";
char mqtt_port[6]    = "1884";
char mqtt_topic[60]  = "v1.0/Datastreams(1)/Observations";  // = "v1.0/Datastreams(1)/Observations";

#define PIN_TICK           D2      // VIN connected Pin

#define RESET_SETTINGS_PIN D8      // pin to use for resetting (connect D8 with 3V)

#define LOG_PERIOD         60      // mqtt log period

static char esp_id[16];            // esp-id is used as unique mqtt-client ID

static WiFiManager wifiManager;
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);


void toggle_led()
{
  //toggle builtin blue led state
  int state = digitalRead(BUILTIN_LED);  // get the current state of GPIO1 pin
  digitalWrite(BUILTIN_LED, !state);     // set pin to the opposite state
}

// TODO: what is this for???
//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.1, toggle_led);
}

// interrupt (connect 3V to RESET_SETTINGS_PIN for a short moment
ICACHE_RAM_ATTR static void reset_settings(void){
 Serial.print("Resetting wifimanager settings! Plz connect to http://192.168.4.1/ ");
 // cleanup (memory) settings of wifimanager
 wifiManager.resetSettings();
 setup_wifi();
}

// the total count value
static volatile unsigned long counts = 0;
// working variables
static int secondcounts[60];
static unsigned long int secidx_prev = 0;
static unsigned long int count_prev = 0;
static unsigned long int second_prev = 0;

// interrupt routine for geiger counter
ICACHE_RAM_ATTR static void tube_impulse(void)
{
  counts++;
  Serial.print(".");
  //check to inspect the circular buffer
  //Serial.println(counts);
  //int c = 0;
  //for(int i = 0; i < 60; i++)
  //{
  //  Serial.print(secondcounts[i]);
  //  Serial.print(",");
  //  c+=secondcounts[i];
  //}
  //Serial.println();
  //Serial.println(c);
}


/*
 * Setup/Use the FS / filesystem
 * Try to read/write the parameter config '/config.json' from FS
 */

//flag for saving data, only when config changed (via wifi interface)
bool save_config = false;

void write_fs_config(){
  Serial.println("Trying to write config.json to FS...");
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to OPEN config file for writing json");
  }
  else{
    // see https://arduinojson.org/v6/doc/upgrade/
    DynamicJsonDocument doc(1024);
    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_port"] = mqtt_port;
    doc["mqtt_topic"] = mqtt_topic;
    // show in monitor
    serializeJsonPretty(doc, Serial);
    Serial.println();
    // write to configFile
    if (serializeJson(doc, configFile) == 0) {
      Serial.println("Failed to serialize json to config file");
    }
    else{
      Serial.println("Serialized json to config file OK");
    }
    configFile.close();
  }
}

void read_fs_config(){

  //clean FS, for testing
  //SPIFFS.format();

  if (SPIFFS.begin()) {  
    Serial.println("Mounted the file system");
    
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("Reading existing config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("Opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        
        //DynamicJsonBuffer jsonBuffer;
        //JsonObject& json = jsonBuffer.parseObject(buf.get());

        DynamicJsonDocument doc(1024);
        //JsonObject json = doc.parsObject(buf.get());
        auto error = deserializeJson(doc, buf.get());
        if (error){
          Serial.println("Error parsing json config...");
        }
        else{
          Serial.println("Parsed json config OK:");
          serializeJsonPretty(doc, Serial);
          Serial.println();
          strcpy(mqtt_server, doc["mqtt_server"]);
          strcpy(mqtt_port, doc["mqtt_port"]);
          strcpy(mqtt_topic, doc["mqtt_topic"]);

        }
        configFile.close();
      }
    }
  } else {
    Serial.println("Failed to mount FS: so no config read from FS");
  }
  //end read
}


// handle the setup of the wifi 
// in a separate function to be able to REdo setup after interrup 
// via 3V-RESET_SETTINGS_PIN connection/shortage
void setup_wifi(){
  
  WiFiManagerParameter custom_mqtt_server("server", "MQTT server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_topic("topic", "MQTT publish topic", mqtt_topic, 60);

  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);
  
  //set config save notify callback... not being called?
  //wifiManager.setSaveConfigCallback(saveConfigCallback);

  //adding our custom parameters here (to be shown in wifi settings dialog)
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_topic);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect("GEIGER-CONFIG-ME-192-168-4-1")) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("Connected to wifi... :-) ");

  // Trying to setup eztime / networktime
  if (waitForSync(10)){
    Serial.println("UTC: " + UTC.dateTime());
  }
  else{
    // TODO: should we bail out here, else wrong datetimes to frost/servers ??
    Serial.println("UTC... Time not yet available (but we should not come here as we ARE connected???) ...");
  }  

  // TODO: only set save_config to true when we went through the access point step?
  save_config = true;
  
  ticker.detach();
  //keep LED on (LOW) (or off (HIGH)...)
  digitalWrite(BUILTIN_LED, HIGH);

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());

  Serial.print("Parameters: using as MQTT host: ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.print(mqtt_port);
  Serial.print(" using topic: ");
  Serial.println(mqtt_topic);
}


static bool mqtt_send(const char *topic, const char *value, bool retained)
{
    digitalWrite(BUILTIN_LED, LOW); // ON
    bool result = false;
    if (!mqttClient.connected()) {
        Serial.print("Connecting MQTT...");
        mqttClient.setServer(mqtt_server, String(mqtt_port).toInt()); // mmm is this ok to go from char* to int ??
        //result = mqttClient.connect(esp_id, topic, 0, retained, "offline");
        result = mqttClient.connect(esp_id);
        Serial.println(result ? "OK" : "FAIL");
    }
    if (mqttClient.connected()) {

        // {"result": 24.57, "resultTime":"2019-08-07T12:39:12.209Z"}
        // https://arduinojson.org/v6/assistant/ size on ESP32/ESP8266 32+43 = 75
        // see https://arduinojson.org/v6/example/generator/
        const size_t capacity = JSON_OBJECT_SIZE(2);
        DynamicJsonDocument doc(capacity);
        doc["result"] = value;
        doc["resultTime"] = UTC.dateTime(W3C).c_str(); // "2019-08-07T12:39:12.209Z"
        /* TODO
        if ( (doc["resultTime"].length()) < 20){
          Serial.print("Time string length < 20 (probably not valid): ");         
          Serial.println(doc["resultTime"]);
          result = false;
          return result;
        }*/
                        
        char buffer[512];
        size_t n = serializeJson(doc, buffer);
        Serial.println("Publishing:");
        //Serial.print(value);
        serializeJsonPretty(doc, Serial);
        Serial.println();        
        Serial.print("to ");
        Serial.print(topic);
        Serial.print(" ... ");
        result = mqttClient.publish(topic, buffer, retained);
        Serial.println(result ? "OK" : "FAIL");
        if (result){
          // NO RESULT: LED stays ON == NO mqtt connection
          digitalWrite(BUILTIN_LED, HIGH); // OFF     
        }
    }
    else{
      digitalWrite(BUILTIN_LED, LOW); // ON
    }
    return result;
}

// ONE TIME SETUP
void setup() {
  
  Serial.begin(115200);
  Serial.println("\n******\nGEIGER\n******");

  //set led pin as output
  pinMode(BUILTIN_LED, OUTPUT);
  //start with led ON (low)
  digitalWrite(BUILTIN_LED, LOW);

  SPIFFS.begin();
  
  // get/set ESP id (used as mqtt client id)
  sprintf(esp_id, "%08X", ESP.getChipId());
  Serial.print("ESP ID: ");
  Serial.println(esp_id);

  attachInterrupt(digitalPinToInterrupt(RESET_SETTINGS_PIN), reset_settings, RISING);
  
  read_fs_config();
  
  setup_wifi();

  // TODO: for now ALWAYS save the config
  // because I cannot determine when we have a new config or not...
  //if (save_config){
    write_fs_config();
  //}

  // start counting
  memset(secondcounts, 0, sizeof(secondcounts));
  Serial.println("Start counting ...");
  pinMode(PIN_TICK, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_TICK), tube_impulse, FALLING);
}

// MAIN LOOP
void loop() {
    // update the circular buffer every second
    unsigned long int second = millis() / 1000;
    unsigned long int secidx = second % 60;
    if (secidx != secidx_prev) {
        //Serial.println(secidx);
        // new second, store the counts from the last second
        unsigned long int count = counts;
        secondcounts[secidx_prev] = count - count_prev;
        count_prev = count;
        secidx_prev = secidx;
    }
    // report every LOG_PERIOD
    //if ((second - second_prev) > LOG_PERIOD) {  // BUG?? always 61 (if LOG_PERIOD = 60)
    if ((second - second_prev) >= LOG_PERIOD) {
        /*Serial.print(second);
        Serial.print("-");
        Serial.print(second_prev);
        Serial.print("=");
        Serial.println(second - second_prev);*/
        Serial.println("");
        // calculate sum
        int cpm = 0;
        for (int i = 0; i < 60; i++) {
            cpm += secondcounts[i];
        }

        // send over MQTT
        // to prevent sending a zero, AND to let the tube warm up a little
        // only sent the first cmp count AFTER the SECOND log period
        // that is when second_prev > 0
        // also check for values < 10 (restarting etc etc)
        if (cpm < 10){
          Serial.print("Value < 10 (probably not valid): ");         
          Serial.println(cpm);
        }
        else if (second_prev > 0){        
          char message[16];
          //snprintf(message, sizeof(message), "%d cpm", cpm);
          snprintf(message, sizeof(message), "%d", cpm);
          if (!mqtt_send(mqtt_topic, message, true)) {
              Serial.println("Restarting ESP...");
              ESP.restart();
          }
        }
        else{
          Serial.print("First cpm calculated: ");
          Serial.println(cpm);
          Serial.println("But skip for sending, because this is the first (cold/maybe zero) one...");
        }

        second_prev = second;
    }
    // keep MQTT alive
    mqttClient.loop();
}
