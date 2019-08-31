
/*
 * Sketch for Wemos mini D1 ESP8266 board in combination with a Geiger counter board
 * 
 * For building see FS note below!!
 * 
 * TODO:
 * - bigger circular buffer  (like 2 or 3 minutes)?
 * - sent also to radmon (https://radmon.org/index.php/forum/pyradmon/937-pyradmon-reborn-v2-0-0)
 *   curl -v "https://radmon.org/radmon.php?user=your_user&password=your_password&function=submit&datetime=2019-08-20%2021:57:34&value=18&unit=CPM"
 * - think about resultTime vs phenomenonTime...
 * - check if we can sent results as numbers (instead of strings) in json
 * - instead of sending an value every minute, also make it possible (if driving) to sent every x meter
 * 
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

#define WIFI_SSD "Geiger 192.168.4.1"


// For MQTT pub/sub messaging
// NOTE IMPORTANT
// we are sending pretty long json messages, see note https://github.com/knolleary/pubsubclient
// you need to enlarge MQTT_MAX_PACKET_SIZE (default 128) in PubSubClient.h to 512 or so
// ELSE ALL GPS MESSAGES WILL FAIL
/*
  #ifndef MQTT_MAX_PACKET_SIZE
  #define MQTT_MAX_PACKET_SIZE 512
  #endif
*/

#include <PubSubClient.h>         // https://github.com/knolleary/pubsubclient


// For ublox/gps connection
#include <NMEAGPS.h>              // https://github.com/SlashDevin/NeoGPS
#include <GPSport.h>
// LET OP!!!!
// wemos/eps heeft voor zover ik werkend krijg maar 1 Serial poort: Serial
// Onderstaande is nodig, met RT->RX en RX->RT connected (en 3V en G)
#define gpsPort Serial
#define GPS_PORT_NAME "Serial"
#define Serial Serial

NMEAGPS  gps; // This parses the GPS characters
gps_fix  fix; // This holds on to the latest values

int sats = -999;
float lat = -999;
float lon = -999;


#include <Adafruit_SSD1306.h>          // https://github.com/adafruit/Adafruit_SSD1306
// Screen setup:
// https://www.instructables.com/id/Wemos-D1-Mini-096-SSD1306-OLED-Display-Using-SPI/
//  Wemos  -> screen
//  G      -> GND
//  3.3V   -> VCC
//  D5     -> D0
//  D7     -> D1
//  D3     -> RES
//  D1     -> DC
//  D4     -> CS
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
// If using software SPI (the default case):
#define OLED_MOSI   D7
#define OLED_CLK   D5
#define OLED_DC    D1
#define OLED_CS    D4
#define OLED_RESET D3

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RESET, OLED_CS);

String l1 = "";  // txtsize 2 = 10 chars
String l2 = "";  // txtsize 1 = 21 chars
String l3 = "";
String l4 = "";
String l5 = "";
//String DOTS = ".....................";

//for LED status                  // From core, to blink led
#include <Ticker.h>
Ticker ticker;


//To be able to use networktime for timestamps
#include <ezTime.h>                // https://github.com/ropg/ezTime
Timezone nl_timezone;


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
static volatile unsigned long total_count = 0;
// working variables
static int secondcounts[60];
static unsigned long int secidx_prev = 0;
static unsigned long int count_prev = 0;
static unsigned long int second_prev = 0;

// interrupt routine for geiger counter
ICACHE_RAM_ATTR static void tube_impulse(void)
{
  total_count++;
  Serial.print(".");
  //check to inspect the circular buffer
  //Serial.println(total_count);
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

  l2 = "Connect to wifi ssd:";
  l3 = " "+String(WIFI_SSD);
  l4 = "Browse to 192.168.4.1";
  to_display(5, "Set parameters there");

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect(WIFI_SSD)) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    //ESP.reset();
    delay(1000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("Connected to wifi... :-) ");
  l2 = "";
  l2 = "Connected to wifi !!";
  l4 = "";
  l4 = "";

  // Trying to setup eztime / networktime
  if (waitForSync(10)){
    Serial.println("UTC: " + UTC.dateTime());
  }
  else{
    // TODO: should we bail out here, else wrong datetimes to frost/servers ??
    Serial.println("UTC... Time not yet available (but we should not come here as we ARE connected???) ...");
  }
  nl_timezone.setLocation(F("Europe/Amsterdam"));

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

        /* TODO
        if ( (doc["resultTime"].length()) < 20){
          Serial.print("Time string length < 20 (probably not valid): ");         
          Serial.println(doc["resultTime"]);
          result = false;
          return result;
        }*/
        DynamicJsonDocument doc(1024);
        doc["result"] = String(value).toInt(); // mmm is this ok to go from char* to int ??;
        doc["resultTime"] = UTC.dateTime(W3C).c_str(); // "2019-08-07T12:39:12.209Z"    
        
        if (sats == -999)
        {
          // Mmm no location or GPS ...
          
          // {"result": 24.57, "resultTime":"2019-08-07T12:39:12.209Z"}
          // https://arduinojson.org/v6/assistant/ size on ESP32/ESP8266 32+43 = 75
          // see https://arduinojson.org/v6/example/generator/
          //const size_t capacity = JSON_OBJECT_SIZE(2);
          //DynamicJsonDocument doc(capacity);
          //doc["result"] = value;
          //doc["resultTime"] = UTC.dateTime(W3C).c_str(); // "2019-08-07T12:39:12.209Z"                       
        }
        else{
          // YES we have a valid location from GPS
          // {"resultTime":"2019-02-01T19:09:00.000Z","result" : 20,"FeatureOfInterest":{"name":"GPSlokatie","description":"sats:7,time:19:08:00","encodingType":"application/vnd.geo+json","feature":{"type":"Point","coordinates":[4.648184,52.397121]}}}
          JsonObject FeatureOfInterest = doc.createNestedObject("FeatureOfInterest");
          FeatureOfInterest["name"] = "GPS";
          FeatureOfInterest["description"] = "GPS";
          JsonObject FeatureOfInterest_properties = FeatureOfInterest.createNestedObject("properties");
          FeatureOfInterest_properties["sats"] = sats;
          FeatureOfInterest["encodingType"] = "application/vnd.geo+json";
          JsonObject FeatureOfInterest_feature = FeatureOfInterest.createNestedObject("feature");
          FeatureOfInterest_feature["type"] = "Point";
          JsonArray FeatureOfInterest_feature_coordinates = FeatureOfInterest_feature.createNestedArray("coordinates");
          FeatureOfInterest_feature_coordinates.add(lon);
          FeatureOfInterest_feature_coordinates.add(lat);
        }
        
        char buffer[512];
        size_t n = serializeJson(doc, buffer);
        Serial.println("Publishing:");
        //Serial.print(value
        serializeJsonPretty(doc, Serial);
        Serial.println();        
        Serial.print("to ");
        Serial.print(topic);
        Serial.print(" ... ");
        result = mqttClient.publish(topic, buffer, retained);     
        
        if (result){
          to_display(4, "MQTT: OK sent: " + String(value));
          Serial.println("OK");
          digitalWrite(BUILTIN_LED, HIGH); // OFF     
        }
        else{
          Serial.println("FAIL");
          l3 = "";
          to_display(4, ("MQTT: FAIL to sent...") );
          // NO RESULT: LED stays ON == NO mqtt connection
        }
    }
    else{
      digitalWrite(BUILTIN_LED, LOW); // ON
    }
    return result;
}

// ONE TIME SETUP
void setup() {
  
  Serial.begin(9600);
  Serial.println("\n******\nGEIGERrr\n******");

  //set led pin as output
  pinMode(BUILTIN_LED, OUTPUT);
  //start with led ON (low)
  digitalWrite(BUILTIN_LED, LOW);

  // setup ssd1306 screen
  display.begin(SSD1306_SWITCHCAPVCC);
  display.setTextColor(1);  // Pixel color, one of: BLACK (0), WHITE(1) or INVERT(2)

  to_display(1, "GEIGERrr..");
  

  SPIFFS.begin();
  
  // get/set ESP id (used as mqtt client id)
  sprintf(esp_id, "%08X", ESP.getChipId());
  Serial.print("ESP ID: ");
  Serial.println(esp_id);

  attachInterrupt(digitalPinToInterrupt(RESET_SETTINGS_PIN), reset_settings, RISING);
  
  read_fs_config();

  setup_wifi();

  l2 = "";
  l4 = "";
  l5 = "";
  to_display(3, "Writing config...");
  // TODO: for now ALWAYS save the config
  // because I cannot determine when we have a new config or not...
  //if (save_config){
    write_fs_config();
  //}

  Serial.println('Starting Serial connection for GPS...');
  while (!Serial){
    Serial.println('Waiting for Serial connection...');
  }
  gpsPort.begin(9600);  

  // start counting
  memset(secondcounts, 0, sizeof(secondcounts));
  Serial.println("Start counting ...");
  to_display(5, "Starting to count ...");
  pinMode(PIN_TICK, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_TICK), tube_impulse, FALLING);
  delay(3000);
}

static void GPSloop();
static void GPSloop()
{
  while (gps.available( gpsPort ))
  {
    fix = gps.read();
    if (fix.valid.location)
    {
      sats = fix.satellites;
      lat = fix.latitude();
      lon = fix.longitude();
    }
    // TODO: hit every second... async? interrupt?
    //Serial.print("o");
    /*
    Serial.print( F("Sats: ") );
    Serial.print( fix.satellites );
    Serial.print( ", " );
    
    Serial.print( F("Location: ") );
    if (fix.valid.location) {
      Serial.print( fix.latitude(), 6 );
      Serial.print( ", " );
      Serial.print( fix.longitude(), 6 );
    }
    Serial.println("");
    */
  }

} // GPSloop

void to_display(int line, String txt){
  switch (line){
    case 1:
      l1 = txt;break;
    case 2:
      l2 = txt;break;
    case 3:
      l3 = txt;break;
    case 4:
      l4 = txt;break;
    case 5:
      l5 = txt;      
      /*
      int i = txt.toInt();
      if(i % 2){ 
        l5 = txt + " " + DOTS.substring(0, (((i+1)/2))-1) + ".";// oneven/odd
      }
      else{
        l5 = txt + " " + DOTS.substring(0, i/2); // even 
      }
      */
      break;      
  }
  // To display 
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextSize(2); // 10 chars
  display.println(l1);
  display.setCursor(0,20);
  display.setTextSize(1); // 21 chars
  display.println(l2);
  //display.println();
  display.setCursor(0,32);
  display.println(l3);
  //display.println();
  display.setCursor(0,44);
  display.println(l4);
  display.setCursor(0,54);
  display.println(l5);
  // SHOW IT
  display.display();
}

int cpm = 0;

// MAIN LOOP
void loop() {
    // update the circular buffer every second
    unsigned long int second = millis() / 1000;
    unsigned long int secidx = second % 60;
    if (secidx != secidx_prev) {
        //Serial.println(secidx);
        // new second, store the total_count from the last second
        unsigned long int count = total_count;
        secondcounts[secidx_prev] = count - count_prev;
        
        cpm = (count-count_prev)+cpm;
        if (lat>0){
          l2 = String(lat, 5) + " " + String(lon, 5) + " s" + String(fix.satellites);
        }
        else{
          l2 = "- no gps (yet) -";
        }
        //l3 = UTC.dateTime("d/m H:i:s T").c_str();
        l3 = nl_timezone.dateTime("d/m H:i:s T").c_str();
        if(secidx%10 == 0){
          l4 = String( WiFi.status()==WL_CONNECTED ? "wifi OK - " : "NO WIFI - ") + String( mqttClient.connected() ? "mqtt OK" : "no MQTT yet");
          Serial.println(l4);
        }
        to_display(5, String(secidx)+"s - count: "+String(cpm));
        
        
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
        cpm = 0;
        for (int i = 0; i < 60; i++) {
            cpm += secondcounts[i];
        }

        Serial.print("Sats: ");
        Serial.print(sats);
        Serial.print(" Lat: ");
        Serial.print(lat, 6);
        Serial.print(" Lon: ");
        Serial.println(lon, 6);

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
              //Serial.println("Restarting ESP...");
              //ESP.restart();
              Serial.println("PROBLEM sending count via MQTT !!!");
          }
        }
        else{
          Serial.print("First cpm calculated: ");
          Serial.println(cpm);
          Serial.println("But skip for sending, because this is the first (cold/maybe zero) one...");
        }
        cpm = 0; // RD
        second_prev = second;  // TODO? move this up (to be sure it is rest before the maybe time costing ip stuff?)
    }
    
    // keep MQTT alive
    mqttClient.loop();
    // keep GPS reading
    GPSloop();
    
}
