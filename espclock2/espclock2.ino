/*
 * espclock2.ino
 *
 * Copyright 2018 Victor Chew
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

// Application Javascript to be injected into WiFiManager's config page
#define QUOTE(...) #__VA_ARGS__
const char* jscript = QUOTE(
  <script>
    document.addEventListener('DOMContentLoaded', tzinit, false); 
    function tzinit() {
      document.getElementById("timezone").value = Intl.DateTimeFormat().resolvedOptions().timeZone;
    }
  </script>
);

/*
 * PHP script can be hosted on your own server:
 *
 * <?php
 * if (isset($_REQUEST['tz'])) {
 *   $time = new DateTime();
 *   $time->setTimezone(new DateTimeZone($_REQUEST['tz']));
 *   print $time->format('h:i:s');
 * }
 * ?>
 *
 * Or you can use the version hosted at http://node10.vpslinker.com/espclock2.php
 */

#define I2C_SLAVE_ADDR 0x12
#define CMD_START_CLOCK 0x2
#define CMD_STOP_CLOCK 0x4
#define CMD_SET_CLOCK 0x6
#define CMD_SET_NETTIME 0x8
#define DEFAULT_SCRIPT_URL "http://node10.vpslinker.com/espclock2.php?tz=[tz]"

bool shouldSaveConfig = false;
byte netHH, netMM, netSS;
char param_tz[64], param_url[256], buf_timezone[64], buf_clockTime[10], buf_scriptUrl[256] = DEFAULT_SCRIPT_URL;
os_timer_t watchdog_timer;

void debug(const char *format, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  Serial.println(buf);
}

void watchdogCallback(void *pArg) {
  digitalWrite(BUILTIN_LED, HIGH);
  pinMode(BUILTIN_LED, INPUT_PULLUP);
  debug("No user input for 5 mins; Sleeping 60 mins...");
  ESP.deepSleep(60*60*1000000UL, WAKE_RF_DEFAULT);
}

// Called when WifiManager 
void saveConfigCallback () {
  shouldSaveConfig = true;
}

// Read config parameters from the SPIFFS filesystem
// Returns false if config file cannot be loaded
bool loadConfig() {
  if (SPIFFS.exists("/config.json")) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      size_t size = configFile.size();
      std::unique_ptr<char[]> buf(new char[size]);
      configFile.readBytes(buf.get(), size);
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.parseObject(buf.get());
      if (json.success()) {
        debug("Loaded config file");
        if (json.containsKey("tz")) strcpy(param_tz, json["tz"]);
        if (json.containsKey("url")) strcpy(param_url, json["url"]);
        if (strlen(param_tz) == 0) strcpy(param_tz, "UTC");
        if (strlen(param_url) == 0) strcpy(param_url, DEFAULT_SCRIPT_URL);
        debug("param_tz = %s", param_tz);
        debug("param_url = %s", param_url);
        return true;
      }
    }
  }
  debug("Failed to load config file");
  return false;
}

// Write config parameters to the SPIFFS filesystem
void saveConfig() {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["tz"] = param_tz;
    json["url"] = param_url;
    debug("Saving config file...");
    debug("param_tz = %s", param_tz);
    debug("param_url = %s", param_url);
    File configFile = SPIFFS.open("/config.json", "w");
    if (configFile) {
      json.printTo(configFile);
      configFile.close();
    } else {
      debug("Failed to open config file for writing");
    }
}

bool getNetworkTime() {
  debug("Getting network time...");
  String url = param_url;
  String tzstr = param_tz;
  tzstr.replace("/", "%2F");
  url.replace("[tz]", tzstr);
  debug("HTTP GET: %s", url.c_str());
  HTTPClient http;
  http.begin(url.c_str());
  int rc = http.GET();
  debug("HTTP return code: %d", rc);
  if (rc > 0) {
    if (rc == HTTP_CODE_OK) {
      String payload = http.getString();
      debug("HTTP return payload: %s", payload.c_str());
      if (payload.length() == 10) {
        String hh = payload.substring(0, 2);
        String mm = payload.substring(3, 5);
        String ss = payload.substring(6, 8);
        netHH = hh.toInt();
        netMM = mm.toInt();
        netSS = ss.toInt();
        if (netHH >= 12) netHH -= 12;
        if (netHH < 0 || netHH > 23) netHH = 0;
        if (netMM < 0 || netMM > 59) netMM = 0;
        if (netSS < 0 || netSS > 59) netSS = 0;
        return true;
      }
    }
  } else {
    debug("HTTP GET failed");
  }
  return false;
}

/*
 * 0-2: set_nettime_clock_time
 * 3-5: set_nettime_net_time
 * 6-8: last_msg_time
 * 9-10: period
 * 11-13: difft, diffc, ocr1a
 * 14-15: vcc
 * 16-22: eeprom (5 bytes + stored checksum) + calculated checksum
 * 23-25: cur_clock_time
 * 26-28: cur_net_time
 * 29: last_msg_type
 */
void print_stats() {

/* Uncomment for debugging ATtiny85 component
   
  byte stats[30];
  Wire.requestFrom(I2C_SLAVE_ADDR, 15);
  Wire.readBytes(stats, 15);
  Wire.requestFrom(I2C_SLAVE_ADDR, 15); 
  Wire.readBytes(stats+15, 15);
  int period = (int)stats[10] << 8 | stats[9];
  int vcc = (int)stats[15] << 8 | stats[14];
  Serial.print("STATS: "); for (int i=0; i<sizeof(stats); i++) { Serial.print(stats[i], HEX); Serial.print(' '); } Serial.println(' ');
  debug("       cur_clock_time = %02d:%02d:%02d; cur_net_time = %02d:%02d:%02d; last_msg_time = %02d:%02d:%02d (%u)", 
    stats[23], stats[24], stats[25], stats[26], stats[27], stats[28], stats[6], stats[7], stats[8], stats[29]);
  debug("       set_nettime_clock_time = %02d:%02d:%02d; set_nettime_net_time = %02d:%02d:%02d; ",
    stats[0], stats[1], stats[2], stats[3], stats[4], stats[5]);
  debug("       period = %d, difft = %d; diffc = %d; ocr1a = %u; vcc = %d", 
    period, (int8_t)stats[11], (int8_t)stats[12], stats[13], vcc);
  debug("       eeprom = %u %u %u %u %u %u (%u)", 
    stats[16], stats[17], stats[18], stats[19], stats[20], stats[21], stats[22]);

*/
}

void setup() {
  Serial.begin(115200);
  debug("ESPCLOCK started");

  Wire.begin();
  Wire.setClockStretchLimit(1500); // to accommodate for ATTiny85's slower speed at 1MHz

  // Setup SPIFFS and load config values
  if (!SPIFFS.begin()) {
    debug("Failed to mount filesystem");
    debug("ESPCLOCK halted");
    ESP.deepSleep(0, WAKE_NO_RFCAL);
  }
  loadConfig();

  // If D6 is LOW, we will start in AP mode for configuration
  pinMode(D6, INPUT);
  bool reset = (digitalRead(D6) == LOW);
  if (reset) {
    debug("Reset request detected");
    delay(200);
    Wire.beginTransmission(I2C_SLAVE_ADDR);
    byte stop_clock[] = { CMD_STOP_CLOCK, 0xCC ^ CMD_STOP_CLOCK };
    debug("CMD_STOP_CLOCK (%d)", Wire.write(stop_clock, sizeof(stop_clock)));
    Wire.endTransmission();
  }
 
  // Light up onboard LED to show we are in config mode
  // Set watchdog timer to sleep if config not done within 5 minutes
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, LOW);
  os_timer_setfn(&watchdog_timer, watchdogCallback, NULL);
  os_timer_arm(&watchdog_timer, 5*60*1000, false);

  // Setup WiFiManager
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  if (reset) wifiManager.resetSettings();
  wifiManager.setCustomHeadElement(jscript);
  WiFiManagerParameter form_timezone("timezone", "TZ database timezone code", buf_timezone, sizeof(buf_timezone)-1);
  WiFiManagerParameter form_clockTime("clockTime", "Time on clock (12-hr HHMMSS)", buf_clockTime, sizeof(buf_clockTime)-1);
  WiFiManagerParameter form_scriptUrl("scriptUrl", "URL to espclock2.php", buf_scriptUrl, sizeof(buf_scriptUrl)-1);
  wifiManager.addParameter(&form_clockTime);
  wifiManager.addParameter(&form_timezone);
  wifiManager.addParameter(&form_scriptUrl);
  wifiManager.autoConnect("ESPCLOCK2");

  // At this point, shouldSaveConfig will tell us whether WiFiManager has connected
  // using previously stored SSID/password, or new ones entered into the config portal
  if (shouldSaveConfig) {
    
    // If new SSID/password, we need to save new config values
    strncpy(param_tz, form_timezone.getValue(), sizeof(param_tz) - 1);
    strncpy(param_url, form_scriptUrl.getValue(), sizeof(param_url) - 1);
    saveConfig();

    // If physical clock time has been changed by user, send new time to ATtiny85
    int clockTime = atoi(form_clockTime.getValue());
    int clockMMSS = clockTime % 10000;
    byte clockHH = clockTime / 10000;
    byte clockMM = clockMMSS / 100;
    byte clockSS = clockMMSS % 100;
    while(clockHH >= 12) clockHH -= 12;
    if (clockMM > 59) clockMM = 0;    
    if (clockSS > 59) clockSS = 0;
    debug("CMD_SET_CLOCK = %02d%02d%02d", clockHH, clockMM, clockSS);
    byte setclock[] = { CMD_SET_CLOCK, clockHH, clockMM, clockSS, 0xCC };
    for (int i=0; i<sizeof(setclock)-1; i++) setclock[sizeof(setclock)-1] ^= setclock[i];
    Wire.beginTransmission(I2C_SLAVE_ADDR);
    Wire.write(setclock, sizeof(setclock));
    Wire.endTransmission();
    delay(200);
    print_stats();
  } 

  // Turn off onboard LED to show we are done with config
  // Disable watchdog timer
  digitalWrite(BUILTIN_LED, HIGH);
  pinMode(BUILTIN_LED, INPUT_PULLUP);
  os_timer_disarm(&watchdog_timer);
  
  // Tell ATtiny85 to start running the clock
  Wire.beginTransmission(I2C_SLAVE_ADDR);
  byte start_clock[] = { CMD_START_CLOCK, 0xCC ^ CMD_START_CLOCK };
  debug("CMD_START_CLOCK (%d)", Wire.write(start_clock, sizeof(start_clock)));
  Wire.endTransmission();
  delay(200);
  print_stats();

  // Get network time
  bool rc = getNetworkTime();
  if (rc) {
      byte nettime[] = { CMD_SET_NETTIME, netHH, netMM, netSS, 0xCC };
      for (int i=0; i<sizeof(nettime)-1; i++) nettime[sizeof(nettime)-1] ^= nettime[i];
      Wire.beginTransmission(I2C_SLAVE_ADDR);
      int count = Wire.write(nettime, sizeof(nettime));
      debug("CMD_SET_NETTIME (%d) = %02d:%02d:%02d", count, nettime[1], nettime[2], nettime[3]);
      Wire.endTransmission();
      delay(200);
      print_stats();
  }
 
  // If after reset, deep sleep only 5 minutes to calibrate Timer1 counter on slave
  // Otherwise, deep sleep for 1 hour
  debug("Sleeping %d mins...", reset ? 5 : 60);
  ESP.deepSleep((reset ? 5: 60)*60*1000000UL, WAKE_RF_DEFAULT);
}

void loop() {
  // When ESP8266 wakes up from deep sleep, setup() is called.
}
