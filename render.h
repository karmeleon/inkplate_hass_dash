#include "Inkplate.h"
#include "globals.h"
#include "fonts/OpenSansBold96pt7b.h"
#include "fonts/OpenSansSemiBold96pt7b.h"
#include "fonts/OpenSansRegular48pt7b.h"
#include "fonts/OpenSansSemiBold24pt7b.h"
#include "fonts/Cheltenham_Condensed_Bold_Italic24pt7b.h"
#include "img.h"

#define SCREEN_HEIGHT 800
#define SCREEN_WIDTH 600
#define STATUS_BAR_HEIGHT 30
#define EFFECTIVE_SCREEN_HEIGHT (SCREEN_HEIGHT - STATUS_BAR_HEIGHT)
#define CAPSULE_HEIGHT 115

#define ITEMS_ON_SCREEN 4

// font, color, and size should be set before calling this fn
void drawCenteredText(Inkplate* displayPtr, short centerX, short centerY, const char* content, bool customFont) {
  int16_t x1, y1;
  uint16_t width, height;

  displayPtr->getTextBounds(content, 0, 0, &x1, &y1, &width, &height);

  short cursorX = centerX - (width / 2);
  short cursorY = centerY - (customFont ? (height / -2) : (height / 2));

  displayPtr->setCursor(cursorX, cursorY);
  displayPtr->print(content);
}

void drawCapsule(Inkplate* displayPtr, short x, short y, short diameter, short width, const char* content) {
  displayPtr->fillRoundRect(x, y, width, diameter, diameter / 2, BLACK);
  displayPtr->setTextSize(1);
  displayPtr->setTextColor(WHITE);
  displayPtr->setFont(&Open_Sans_Bold_96);
  drawCenteredText(displayPtr, x + width / 2, y + diameter / 2, content, true);
  displayPtr->setFont(NULL);
}

void renderArrivalDetails(Inkplate* displayPtr, arrival_t* arrivalPtr, short x, short y, time_t renderTime) {
  // draw filled bar
  short fillHeight;
  switch (arrivalPtr->occupancy) {
    case SEATS_AVAILABLE:
      fillHeight = CAPSULE_HEIGHT * 0.2;
      break;
    case STANDING_AVAILABLE:
      fillHeight = CAPSULE_HEIGHT * 0.5;
      break;
    case FULL:
      fillHeight = CAPSULE_HEIGHT * 0.9;
      break;
    case UNKNOWN_OCCUPANCY:
      fillHeight = 0;
      break;
  }

  displayPtr->drawRoundRect(x, y, 35, CAPSULE_HEIGHT, 5, BLACK);
  displayPtr->fillRoundRect(x, y + (CAPSULE_HEIGHT - fillHeight), 35, fillHeight, 5, BLACK);
  
  // draw arrival time
  displayPtr->setTextSize(1);
  displayPtr->setTextColor(BLACK);
  displayPtr->setFont(&Open_Sans_SemiBold_96);

  char stringifiedTime[5];
  itoa((arrivalPtr->expectedArrivalTime - renderTime) / 60, stringifiedTime, 10);
  //drawCenteredText(displayPtr, x + 105, y + (CAPSULE_HEIGHT / 2), stringifiedTime, true);
  displayPtr->setCursor(x + 45, y + (CAPSULE_HEIGHT * 0.8));
  displayPtr->print(stringifiedTime);
  displayPtr->setFont(NULL);
}

void renderDisplayItem(Inkplate* displayPtr, display_item_t* displayItemPtr, byte itemIndex, time_t renderTime) {
  short startY = itemIndex * (EFFECTIVE_SCREEN_HEIGHT / ITEMS_ON_SCREEN) + STATUS_BAR_HEIGHT;
  short endY = (itemIndex + 1) * (EFFECTIVE_SCREEN_HEIGHT / ITEMS_ON_SCREEN) + STATUS_BAR_HEIGHT - 1;

  drawCapsule(displayPtr, 20, startY + 20, CAPSULE_HEIGHT, SCREEN_WIDTH / 3, displayItemPtr->lineRef);

  for (byte i = 0; i < min((byte) 2, displayItemPtr->arrivalCount); i++) {
    renderArrivalDetails(displayPtr, &displayItemPtr->arrivals[i], (SCREEN_WIDTH / 3 + 40) + i * 180, startY + 20, renderTime);
  }

  displayPtr->setTextSize(1);
  displayPtr->setCursor(20, endY - 15);
  displayPtr->setTextColor(BLACK);
  displayPtr->setFont(&Open_Sans_Regular_48);
  displayPtr->print(displayItemPtr->destination);
  displayPtr->setFont(NULL);

  displayPtr->drawThickLine(0, endY, SCREEN_WIDTH - 1, endY, BLACK, 1);
}

void renderStatusBar(Inkplate* displayPtr, time_t renderTime, float batteryVoltage, int rssi) {
  displayPtr->setTextColor(BLACK);
  displayPtr->setTextSize(1);
  displayPtr->setFont(&Open_Sans_SemiBold_24);
  displayPtr->setCursor(25, STATUS_BAR_HEIGHT - 5);

  tm* localTime = localtime(&renderTime);
  char timeStr[10];
  strftime(timeStr, 10, "%I:%M %p", localTime);
  displayPtr->print(timeStr);

  // TODO: calculate a rough battery percentage and display either it or a lil battery icon
  displayPtr->setCursor(SCREEN_WIDTH - 110, STATUS_BAR_HEIGHT - 5);
  displayPtr->printf("%1.2f V", batteryVoltage);
  displayPtr->setFont(NULL);

  displayPtr->drawThickLine(0, STATUS_BAR_HEIGHT, SCREEN_WIDTH - 1, STATUS_BAR_HEIGHT, BLACK, 1);
}

void renderScreen(Inkplate* displayPtr, display_item_t (* displayItemsPtr)[20], byte displayItemCount, time_t renderTime, float batteryVoltage, int rssi) {
  // WARNING: all variables used in this function _must_ be saved to RTC RAM to ensure
  // partial updates work!
  displayPtr->clearDisplay();
  
  displayPtr->setCursor(0, 0);
  
  Serial.printf("Drawing %d display items\n", displayItemCount);
  renderStatusBar(displayPtr, renderTime, batteryVoltage, rssi);
  for (byte i = 0; i < displayItemCount; i++) {
    display_item_t* displayItem = &(*displayItemsPtr)[i];

    renderDisplayItem(displayPtr, displayItem, i, renderTime);
  }
}

void renderSleepImage(Inkplate* displayPtr) {
  displayPtr->clearDisplay();
  displayPtr->drawBitmap3Bit(0, 50, cute_orange_cat_sleeping_clipa, cute_orange_cat_sleeping_clipa_w, cute_orange_cat_sleeping_clipa_h);
  displayPtr->setFont(&Cheltenham_Condensed_Bold_Italic24pt7b);
  displayPtr->setTextColor(BLACK);
  displayPtr->setCursor(3, 50);
  displayPtr->print("SEE YOU SPACE COWBOY ...");
  displayPtr->setCursor(350, 790);
  // can technically not be AM but let's be real
  displayPtr->printf("... AT %d AM", NIGHT_TIME_END_HOUR);
}