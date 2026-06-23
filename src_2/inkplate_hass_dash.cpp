#define CORE_DEBUG_LEVEL ARDUHAL_LOG_LEVEL_VERBOSE

#include <Arduino.h>
#include "Inkplate.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <rom/rtc.h>       // Include ESP32 library for RTC (needed for rtc_get_reset_reason() function)
#include <esp_wifi.h>
#include "driver/adc.h"

#include "config.h"
#include "globals.h"
#include "render.h"

#define uS_TO_S_FACTOR 1000000

RTC_DATA_ATTR time_t lastNTPSync;

Inkplate display(INKPLATE_3BIT);

HTTPClient http;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// we decide this once per render so we don't render a normal update,
// then sleep for 8 hours because the time rolled over into the night period
// while we rendered the normal update
bool isNight;

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

void maybeUpdateTimeFromNTP() {
  if (!display.rtc.isSet() || display.rtc.getEpoch() - lastNTPSync > (60 * 60 * 24)) {
    log_d("Setting clock from NTP");
    timeClient.begin();
    bool ntpTimeUpdateSuccessful = timeClient.update();
    time_t ntpEpochTime = timeClient.getEpochTime();
    log_d("pre-update RTC time %d, NTP time %d, NTP update result %d\n", display.rtc.getEpoch(), ntpEpochTime, ntpTimeUpdateSuccessful);

    if (ntpTimeUpdateSuccessful) {
      display.rtc.setEpoch(ntpEpochTime);
      lastNTPSync = ntpEpochTime;
    } else {
      log_w("Couldn't fetch time from NTP, trying again next cycle.");
    }

    timeClient.end();
  }
}

void initTime() {
  // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();

  time_t now = display.rtc.getEpoch();
  tm* local = localtime(&now);
  char timeStr[10];
  strftime(timeStr, 10, "%I:%M %p", local);

  log_d("RTC time %d / local time: %s\n", display.rtc.getEpoch(), timeStr);
}

void setAlarmForNextUpdate() {
  display.rtc.getRtcData();
  // clear any old timers that may have been set before
  display.rtc.clearTimerFlag();
  time_t currentTime = display.rtc.getEpoch();
  tm* timeStruct = localtime(&currentTime);

  if (isNight) {
    // this assumes that the night starts and ends on the same calendar day
    timeStruct->tm_sec = 0;
    timeStruct->tm_min = 0;
    timeStruct->tm_hour = NIGHT_TIME_END_HOUR;
  } else {
    timeStruct->tm_sec = UPDATE_PERIOD_S;
  }
  time_t alarmTime = mktime(timeStruct);
  int timeToSleep = alarmTime - currentTime;
  if (timeToSleep < 0) {
    timeToSleep = 10;
  } else if (timeToSleep > 60 * 60 * 2 && !isNight) {
    // max 2 hours
    timeToSleep = 60 * 60 * 2;
  }
  // cast to uint64 to prevent an integer overflow
  esp_sleep_enable_timer_wakeup(((uint64_t)timeToSleep) * uS_TO_S_FACTOR);
  log_d("See ya in %d seconds! alarm: %d, current: %d\n", timeToSleep, alarmTime, currentTime);
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
  // ** this may increase power usage and isn't necessary, but leaving uncommented for development
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, LOW);

  // Go to sleep
  esp_deep_sleep_start();
}

bool isNightTime() {
  time_t currentTime = display.rtc.getEpoch();
  tm* timeStruct = localtime(&currentTime);
  int hour = timeStruct->tm_hour;

  return hour >= NIGHT_TIME_START_HOUR && hour < NIGHT_TIME_END_HOUR;
}

void setup() {
  display.begin();
  Serial.begin(9600);
  display.selectDisplayMode(INKPLATE_3BIT);
  display.setRotation(3);
  initTime();
  log_d("Initting...");
  // check if we just woke up from deep sleep
  if (rtc_get_reset_reason(0) == DEEPSLEEP_RESET) {
    log_d("Resuming from deep sleep.");
  } else {
    log_d("Waking up from something other than deep sleep.");
  }

  float batteryVoltage = display.readBattery();
  log_d("Good morning, the battery voltage is %.2f V\n", batteryVoltage);

  isNight = isNightTime();

  // if we're in designated night hours, just render the placeholder image
  // and sleep until morning
  if (isNight) {
    renderSleepImage(&display);
    display.display();

    goToSleep();
  }

  display.rtc.clearAlarmFlag();

  log_d("Attempting to init wifi...");
  delay(200);
  WiFi.begin(SSID, WIFI_PASS);
  short wifiTimeElapsed = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeElapsed < 10000) {
    delay(100);
    wifiTimeElapsed += 100;
    Serial.print('.');
  }
  if (WiFi.status() != WL_CONNECTED) {
    log_e("Couldn't connect to wifi after %d ms. Giving up.\n", wifiTimeElapsed);
    goToSleep();
  }
  Serial.println(" connected!");

  maybeUpdateTimeFromNTP();

  // download and draw our image
  display.image.draw(IMAGE_URL, 0, 0, false, false);
  log_d("drawn image to buffer, displaying now");
  display.display();

  log_d("image drawn!");

  #ifdef WEBHOOK_URL
  log_d("Posting data to webhook");

  int rssi = WiFi.RSSI();

  http.begin(WEBHOOK_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  char formData[50];
  snprintf(formData, 50, "voltage=%.2f&rssi=%d", batteryVoltage, rssi);
  int code = http.POST(formData);

  log_d("Got code %d from webhook\n", code);
  http.end();
  #endif

  goToSleep();
}

void loop() {}
