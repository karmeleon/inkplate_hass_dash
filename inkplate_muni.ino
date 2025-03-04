#include "Inkplate.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <rom/rtc.h>       // Include ESP32 library for RTC (needed for rtc_get_reset_reason() function)

#include "config.h"

#define API_URL_BASE "http://api.511.org/transit/StopMonitoring?agency=SF&format=json&api_key="
#define API_URL API_URL_BASE API_KEY

Inkplate display(INKPLATE_1BIT);
HTTPClient http;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
StaticJsonDocument<144> filter;

enum Occupancy {
  SEATS_AVAILABLE,
  STANDING_AVAILABLE,
  FULL,
  // this is for cable cars, etc that don't measure occupancy
  UNKNOWN_OCCUPANCY,
};

enum Direction {
  INBOUND,
  OUTBOUND,
  WEST,
  SOUTH,
  EAST,
  NORTH,
  // i only saw the above 6 in the data, but handle other cases gracefully
  UNKNOWN_DIRECTION,
};

struct arrival_t {
  time_t expectedArrivalTime;
  Occupancy occupancy;
};

struct display_item_t {
  int stopId;
  // number of arrivals currently in arrivals array
  byte arrivalCount;
  // the longest line refs I saw were 4 chars (LBUS, MBUS)
  char lineRef[4];
  char lineName[32];
  Direction direction;
  // should be long enough for most destinations, at least.
  char destination[32];
  arrival_t arrivals[3];
};

// TODO: bigger? smaller?
RTC_DATA_ATTR display_item_t displayItems[20];
RTC_DATA_ATTR byte displayItemCount = 0;

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

time_t parseISOTimestamp(const char* timestamp) {
  tm timeStruct;
  strptime(timestamp, "%FT%T%z", &timeStruct);
  return mktime(&timeStruct);
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

void renderScreen() {
  display.setTextSize(4);
  display.printf("Battery: %.2f V, WiFi: %d dBm\n", display.readBattery(), WiFi.RSSI());
  time_t now = display.rtcGetEpoch();
  // TODO: replace this with something remotely pleasant to look at
  for (int i = 0; i < displayItemCount; i++) {
    display_item_t* displayItem = &displayItems[i];
    display.print(displayItem->lineRef);
    display.print(" (");
    display.print(displayItem->lineName);
    display.print(") towards ");
    display.println(displayItem->destination);

    for (int j = 0; j < displayItem->arrivalCount; j++) {
      arrival_t* arrival = &displayItem->arrivals[j];
      display.print("    ");
      display.print((arrival->expectedArrivalTime - now) / 60);
      display.print(" min ");
      switch (arrival->occupancy) {
        case FULL:
          display.print("(full)");
          break;
        case STANDING_AVAILABLE:
          display.print("(standing)");
          break;
        case SEATS_AVAILABLE:
          display.print("(seats)");
          break;
        default:
          display.print("(unknown)");
      }
      display.println();
    }
  }

  display.partialUpdate();
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

void setTimeFromNTP() {
  timeClient.begin();
  timeClient.update();
  display.rtcSetEpoch(timeClient.getEpochTime());
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
  time_t currentTime = display.rtcGetEpoch();
  tm* timeStruct = gmtime(&currentTime);
  timeStruct->tm_sec = 0;
  timeStruct->tm_min += 1;
  time_t alarmTime = mktime(timeStruct);
  display.rtcSetAlarmEpoch(alarmTime, RTC_ALARM_MATCH_DHHMMSS);
}

void setup() {
  display.begin();
  Serial.begin(9600);

  Serial.printf("clock speed is %d MHz\n", getCpuFrequencyMhz());

  // check if we just woke up from deep sleep
  if (rtc_get_reset_reason(0) == DEEPSLEEP_RESET) {
    Serial.println("eepy");
  }
  // TODO: partial update
  display.clearDisplay();

  setupJSONFilter();
  display.rtcClearAlarmFlag();

  // clear out any old/garbage data in RTC memory
  // TODO: only do this on fresh boot, not wake from sleep
  memset(displayItems, 0, sizeof(displayItems));
  displayItemCount = 0;

  WiFi.begin(SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print('.');
  }
  Serial.println(" connected!");

  setTimeFromNTP();

  http.useHTTP10(true);
  http.begin(API_URL);
  http.addHeader("Accept-Encoding", "identity", true, true);
  int responseCode = http.GET();

  int startTime = display.rtcGetEpoch();

  if (responseCode == HTTP_CODE_OK) {
    WiFiClient payloadStream = http.getStream();
    ReadBufferingStream bufferedPayload(payloadStream, 256);
    Serial.println("Received repsonse, parsing...");
    // seek to the start of the main array
    bufferedPayload.find("\"MonitoredStopVisit\":[");
    StaticJsonDocument<512> currentArrival;
    int numArrivalsDeserialized = 0;
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

    sortDisplayItems();

    // displayItems is now fully populated, render the screen
    renderScreen();
  } else {
    Serial.print("Got HTTP error: ");
    Serial.print(responseCode);
  }
  WiFi.disconnect();

  Serial.printf("did the real work in %d seconds\n", display.rtcGetEpoch() - startTime);
  
  setAlarmForNextUpdate();

  // Enable wakeup from deep sleep on gpio 39 where RTC interrupt is connected
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_39, 0);

  // Go to sleep
  esp_deep_sleep_start();
}

void loop() {}
