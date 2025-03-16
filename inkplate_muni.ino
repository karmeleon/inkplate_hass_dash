#include "Inkplate.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <rom/rtc.h>       // Include ESP32 library for RTC (needed for rtc_get_reset_reason() function)
#include <esp_wifi.h>
#include "driver/adc.h"

#include "config.h"
#include "globals.h"
#include "render.h"

#define API_URL_BASE "http://511proxy.wn.zone/transit/StopMonitoring?agency=SF&api_key="
#define API_URL API_URL_BASE API_KEY
#define API_DELIMITER "|"

#define MAX_PARTIAL_UPDATES 5

#define uS_TO_S_FACTOR 1000000

RTC_DATA_ATTR display_item_t displayItems[20];
RTC_DATA_ATTR byte displayItemCount;
RTC_DATA_ATTR time_t renderTime;
RTC_DATA_ATTR float batteryVoltage;
RTC_DATA_ATTR int rssi;
RTC_DATA_ATTR byte numPartialUpdatesSinceClear;
RTC_DATA_ATTR time_t lastNTPSync;

Inkplate display(INKPLATE_1BIT);

HTTPClient http;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
StaticJsonDocument<144> filter;

// we decide this once per render so we don't render a normal update,
// then sleep for 8 hours because the time rolled over into the night period
// while we rendered the normal update
bool isNight;

String concatInts(const int* ints, size_t size) {
  char** strings = (char**)malloc(size * sizeof(char*));
  for (int i = 0; i < size; i++) {
    char tmp[8];
    itoa(ints[i], tmp, 10);
    char* string = strdup(tmp);
    strcpy(string, tmp);
    strings[i] = string;
  }
  String output = concatStrings((const char**)strings, size);
  for (int i = 0; i < size; i++) {
    free(strings[i]);
  }
  free(strings);
  return output;
}

String concatStrings(const char** strings, size_t size) {
  String out;
  for (int i = 0; i < size; i++) {
    const char* currentString = strings[i];
    out += currentString;
    if (i != size - 1) {
      out += API_DELIMITER;
    }
  }
  return out;
}

bool stopIsDesired(const char* stopStr) {
  int stop = atoi(stopStr);
  for (const int &stopId : STOP_IDS) {
    if (stopId == stop) {
      return true;
    }
  }
  return false;
}

bool lineIsDesired(const char* line) {
  for (const char* lineRef : LINE_REFS) {
    if (strcmp(lineRef, line) == 0) {
      return true;
    }
  }
  return false;
}

Direction strToDirection(const char* directionString) {
  if (strcmp(directionString, "IB") == 0) {
    return INBOUND;
  }
  if (strcmp(directionString, "OB") == 0) {
    return OUTBOUND;
  }
  if (strcmp(directionString, "W") == 0) {
    return WEST;
  }
  if (strcmp(directionString, "S") == 0) {
    return SOUTH;
  }
  if (strcmp(directionString, "E") == 0) {
    return EAST;
  }
  if (strcmp(directionString, "N") == 0) {
    return NORTH;
  }
  return UNKNOWN_DIRECTION;
}

Occupancy strToOccupancy(const char* occupancyString) {
  if (occupancyString == nullptr) {
    return UNKNOWN_OCCUPANCY;
  }
  if (strcmp(occupancyString, "seatsAvailable") == 0) {
    return SEATS_AVAILABLE;
  }
  if (strcmp(occupancyString, "standingAvailable") == 0) {
    return STANDING_AVAILABLE;
  }
  if (strcmp(occupancyString, "full") == 0) {
    return FULL;
  }
  return UNKNOWN_OCCUPANCY;
}

// chatgpt wizardry to make up for the lack of timegm
time_t parseISOTimestamp(const char* timestamp) {
  struct tm timeStruct = {};
  
  // Parse the ISO 8601 time string (assuming UTC because of "Z")
  strptime(timestamp, "%Y-%m-%dT%H:%M:%SZ", &timeStruct);

  // Convert to time_t (this assumes local time)
  time_t localEpoch = mktime(&timeStruct);

  // Get the true UTC offset at that moment
  struct tm localTime = *localtime(&localEpoch);
  struct tm utcTime = *gmtime(&localEpoch);
  time_t timezoneOffset = difftime(mktime(&localTime), mktime(&utcTime));

  // Adjust back to true UTC
  return localEpoch + timezoneOffset;
}

void addArrivalToArray(JsonDocument arrival) {
  int stopId = atoi(arrival["MonitoringRef"]);
  const char* lineRef = arrival["MonitoredVehicleJourney"]["LineRef"];
  const char* lineName = arrival["MonitoredVehicleJourney"]["PublishedLineName"];
  Direction direction = strToDirection(arrival["MonitoredVehicleJourney"]["DirectionRef"]);
  const char* destination = arrival["MonitoredVehicleJourney"]["DestinationName"];

  display_item_t* displayItemToAddArrivalTo = 0;
  
  // walk through array looking for an entry with matching stopId, lineRef, and direction
  for (int i = 0; i < displayItemCount; i++) {
    display_item_t* displayItem = &displayItems[i];

    if (
      displayItem->stopId == stopId
      && strcmp(lineRef, displayItem->lineRef) == 0
      && strcmp(destination, displayItem->destination) == 0
      && direction == displayItem->direction
    ) {
      // we found a matching struct
      if (displayItem->arrivalCount == (sizeof(displayItem->arrivals) / sizeof(arrival_t))) {
        // we already have the max number of arrivals in our array, so just ignore this
        return;
      }

      displayItemToAddArrivalTo = displayItem;
      break;
    }
  }

  // if we didn't find a display item that matches this arrival...
  if (displayItemToAddArrivalTo == 0) {
    // ... and we're out of room for new ones...
    if (displayItemCount == (sizeof(displayItems) / sizeof(display_item_t))) {
      // ... just ignore it.
      return;
    }

    // otherwise, initialize a new display item to add it to
    
    displayItemToAddArrivalTo = &displayItems[displayItemCount];
    
    displayItemToAddArrivalTo->stopId = stopId;
    strlcpy(displayItemToAddArrivalTo->destination, destination, sizeof(displayItemToAddArrivalTo->destination));
    strlcpy(displayItemToAddArrivalTo->lineRef, lineRef, sizeof(displayItemToAddArrivalTo->lineRef));
    strlcpy(displayItemToAddArrivalTo->lineName, lineName, sizeof(displayItemToAddArrivalTo->lineName));
    displayItemToAddArrivalTo->direction = direction;
    displayItemToAddArrivalTo->arrivalCount = 0;
    displayItemCount++;
  }
  
  // add this arrival to the array
  const char* arrivalTimeStr = arrival["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedArrivalTime"];

  displayItemToAddArrivalTo->arrivals[displayItemToAddArrivalTo->arrivalCount].occupancy = strToOccupancy(arrival["MonitoredVehicleJourney"]["Occupancy"]);
  displayItemToAddArrivalTo->arrivals[displayItemToAddArrivalTo->arrivalCount].expectedArrivalTime = parseISOTimestamp(arrivalTimeStr);
  displayItemToAddArrivalTo->arrivalCount++;
}

void setupJSONFilter() {
  filter["MonitoringRef"] = true;

  JsonObject filter_MonitoredVehicleJourney = filter.createNestedObject("MonitoredVehicleJourney");
  filter_MonitoredVehicleJourney["LineRef"] = true;
  filter_MonitoredVehicleJourney["DirectionRef"] = true;
  filter_MonitoredVehicleJourney["DestinationName"] = true;
  filter_MonitoredVehicleJourney["Occupancy"] = true;
  filter_MonitoredVehicleJourney["PublishedLineName"] = true;
  filter_MonitoredVehicleJourney["MonitoredCall"]["ExpectedArrivalTime"] = true;
}

void maybeUpdateTimeFromNTP() {
  if (!display.rtcIsSet() || display.rtcGetEpoch() - lastNTPSync > (60 * 60 * 24)) {
    Serial.println("Setting clock from NTP");
    timeClient.begin();
    bool ntpTimeUpdateSuccessful = timeClient.update();
    time_t ntpEpochTime = timeClient.getEpochTime();
    Serial.printf("pre-update RTC time %d, NTP time %d, NTP update result %d\n", display.rtcGetEpoch(), ntpEpochTime, ntpTimeUpdateSuccessful);

    if (ntpTimeUpdateSuccessful) {
      display.rtcSetEpoch(ntpEpochTime);
      lastNTPSync = ntpEpochTime;
    } else {
      Serial.println("Couldn't fetch time from NTP, trying again next cycle.");
    }
  }
}

void initTime() {
  // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();

  time_t now = display.rtcGetEpoch();
  tm* local = localtime(&now);
  char timeStr[10];
  strftime(timeStr, 10, "%I:%M %p", local);

  Serial.printf("RTC time %d / local time: %s\n", display.rtcGetEpoch(), timeStr);
}

void updateDisplayGlobals() {
  rssi = WiFi.RSSI();
  batteryVoltage = display.readBattery();
  renderTime = display.rtcGetEpoch();
}

int compareDisplayItems(const void* displayItemAVoid, const void* displayItemBVoid) {
  display_item_t* displayItemA = (display_item_t*)displayItemAVoid;
  display_item_t* displayItemB = (display_item_t*)displayItemBVoid;
  // sort display items by stop ID, then by line, then by direction
  // sort arrivals in each display item by soonest first
  int stopIdDiff = displayItemA->stopId - displayItemB->stopId;
  if (stopIdDiff != 0) {
    return stopIdDiff;
  }
  int lineDiff = strcmp((const char*)&displayItemA->lineRef, (const char*)&displayItemB->lineRef);
  if (lineDiff != 0) {
    return lineDiff;
  }
  return displayItemA->direction - displayItemB->direction;
}

int compareArrivals(const void* arrivalA, const void* arrivalB) {
  return ((arrival_t*)arrivalA)->expectedArrivalTime - ((arrival_t*)arrivalB)->expectedArrivalTime;
}

void sortDisplayItems() {
  qsort(&displayItems, displayItemCount, sizeof(display_item_t), compareDisplayItems);
  for (int i = 0; i < displayItemCount; i++) {
    display_item_t* item = &displayItems[i];
    qsort(item->arrivals, item->arrivalCount, sizeof(arrival_t), compareArrivals);
  }
}

void setAlarmForNextUpdate() {
  display.rtcGetRtcData();
  // clear any old timers that may have been set before
  display.rtcDisableTimer();
  time_t currentTime = display.rtcGetEpoch();
  tm* timeStruct = localtime(&currentTime);

  if (isNight) {
    // this assumes that the night starts and ends on the same calendar day
    timeStruct->tm_sec = 0;
    timeStruct->tm_min = 0;
    timeStruct->tm_hour = NIGHT_TIME_END_HOUR;
  } else {
    timeStruct->tm_sec = 0;
    timeStruct->tm_min += 1;
  }
  time_t alarmTime = mktime(timeStruct);
  int timeToSleep = alarmTime - currentTime;
  if (timeToSleep < 0) {
    timeToSleep = 10;
  } else if (timeToSleep > 60 && !isNight) {
    timeToSleep = 60;
  }
  // using this function instead of display.rtcSetAlarm() doesn't use the ext0 wakeup slot,
  // so we can wake up by either timer or the wake button on the device
  esp_sleep_enable_timer_wakeup(timeToSleep * uS_TO_S_FACTOR);
  Serial.printf("See ya in %d seconds! alarm: %d, current: %d\n", timeToSleep, alarmTime, currentTime);
}

void callRenderFn() {
  renderScreen(&display, &displayItems, displayItemCount, renderTime, batteryVoltage, rssi);
}

void goToSleep() {
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  // explicitly stoppping the wifi before deep sleeping supposedly reduces power a lot
  // may have been fixed with newer arduino esp32 builds, but doesn't hurt to disable it twice
  //adc_power_off();
  esp_wifi_stop();
  
  setAlarmForNextUpdate();

  Serial.flush();

  // Enable wakeup from deep sleep on gpio 36 (wake button)
  // ** this may increase power usage and isn't necessary, comment out for now
  // esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, LOW);

  // Go to sleep
  esp_deep_sleep_start();
}

bool isNightTime() {
  time_t currentTime = display.rtcGetEpoch();
  tm* timeStruct = localtime(&currentTime);
  int hour = timeStruct->tm_hour;

  return hour >= NIGHT_TIME_START_HOUR && hour < NIGHT_TIME_END_HOUR;
}

void setup() {
  display.begin();
  Serial.begin(9600);
  // we really don't need much cpu power for this, save the energy
  setCpuFrequencyMhz(80);
  display.setDisplayMode(INKPLATE_1BIT);
  display.setRotation(3);
  initTime();

  bool shouldPartialUpdate = false;
  Serial.println("Initting...");
  // check if we just woke up from deep sleep
  if (rtc_get_reset_reason(0) == DEEPSLEEP_RESET) {
    Serial.println("Resuming from deep sleep.");
    if (numPartialUpdatesSinceClear < MAX_PARTIAL_UPDATES) {
      Serial.println("Preloading screen to prepare for partial update");
      // draw what we drew last time to memory
      callRenderFn();
      // tell the screen code that the stuff we just drew to memory
      // is the same thing as it already has on screen
      display.preloadScreen();
      shouldPartialUpdate = true;
    }
  }
  if (!shouldPartialUpdate) {
    Serial.println("Doing a full update");
    numPartialUpdatesSinceClear = 0;
  }

  isNight = isNightTime();

  if (isNight) {
    display.setDisplayMode(INKPLATE_3BIT);
    renderSleepImage(&display);
    display.display();

    // set this to a high number so we do a full screen refresh next time
    numPartialUpdatesSinceClear = 50;

    setAlarmForNextUpdate();
    goToSleep();
  }

  setupJSONFilter();
  display.rtcClearAlarmFlag();

  // clear out any old/garbage data in RTC memory
  // we've already preloaded the screen, so we don't need this anymore
  memset(displayItems, 0, sizeof(displayItems));
  displayItemCount = 0;

  esp_wifi_start();
  WiFi.begin(SSID, WIFI_PASS);
  short wifiTimeElapsed = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeElapsed < 10000) {
    delay(100);
    wifiTimeElapsed += 100;
    Serial.print('.');
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Couldn't connect to wifi after %d ms. Giving up.\n", wifiTimeElapsed);
    goToSleep();
  }
  Serial.println(" connected!");

  maybeUpdateTimeFromNTP();

  // use http 1.0 so that the HTTP library doesn't add its own (broken) Accept-Encoding header
  http.useHTTP10(true);
  String stopFilter = concatInts(STOP_IDS, sizeof(STOP_IDS) / sizeof(int));
  String lineFilter = concatStrings(LINE_REFS, sizeof(LINE_REFS) / sizeof(char*));
  // should be long enough
  size_t urlSize = 512 * sizeof(char);
  char* URL = (char*)malloc(urlSize);
  snprintf(URL, urlSize, "%s&stopcodes=%s&line_ids=%s", API_URL, stopFilter, lineFilter);
  http.begin(URL);
  http.addHeader("Accept-Encoding", "identity", true, true);
  http.setTimeout(30 * 1000);

  const char* headerKeys[] = {"x-filtered-visit-count"};
  const size_t headerKeysCount = sizeof(headerKeys) / sizeof(headerKeys[0]);
  http.collectHeaders(headerKeys, headerKeysCount);

  int responseCode = http.GET();

  int startTime = display.rtcGetEpoch();

  if (responseCode == HTTP_CODE_OK) {
    int numVisits = atoi(http.header("x-filtered-visit-count").c_str());
    Serial.printf("Found %d visits\n", numVisits);

    WiFiClient payloadStream = http.getStream();
    ReadBufferingStream bufferedPayload(payloadStream, 256);
    Serial.println("Received response, parsing...");
    // seek to the start of the main array
    bufferedPayload.find("\"MonitoredStopVisit\":[");
    StaticJsonDocument<512> currentArrival;
    int numArrivalsDeserialized = 0;
    if (numVisits > 0) {
      do {
        deserializeJson(currentArrival, bufferedPayload, DeserializationOption::Filter(filter));

        const char* stopId = currentArrival["MonitoringRef"];
        const char* lineRef = currentArrival["MonitoredVehicleJourney"]["LineRef"];

        if (stopIsDesired(stopId) && lineIsDesired(lineRef)) {
          addArrivalToArray(currentArrival);
        }
        numArrivalsDeserialized++;
        if (numArrivalsDeserialized % 100 == 0) {
          Serial.printf("Deserialized %d arrivals so far\n", numArrivalsDeserialized);
        }
      } while (bufferedPayload.findUntil(",", "]"));
    } else {
      Serial.println("No visits to load, skipping json parsing");
    }

    sortDisplayItems();

    // save battery, time, wifi signal to RTC ram so we can partial update on it
    updateDisplayGlobals();
    // displayItems is now fully populated, render the screen
    callRenderFn();

    if (shouldPartialUpdate) {
      Serial.println("Doing partial update");
      display.partialUpdate(true);
      numPartialUpdatesSinceClear++;
    } else {
      Serial.println("Doing full update");
      display.display();
    }
  } else {
    Serial.printf("Got HTTP error: %s\n", http.errorToString(responseCode));
  }
  Serial.printf("Downloaded and processed JSON in %d seconds\n", display.rtcGetEpoch() - startTime);
  free(URL);

  goToSleep();
}

void loop() {}
