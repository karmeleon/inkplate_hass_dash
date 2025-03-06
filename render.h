#include "Inkplate.h"
#include "globals.h"

#define SCREEN_HEIGHT 800
#define SCREEN_WIDTH 600
#define STATUS_BAR_HEIGHT 30
#define EFFECTIVE_SCREEN_HEIGHT (SCREEN_HEIGHT - STATUS_BAR_HEIGHT)

#define ITEMS_ON_SCREEN 4

void drawCapsule(Inkplate* displayPtr, short x, short y, short diameter, short width, char* content) {
  displayPtr->fillRoundRect(x, y, width, diameter, diameter / 2, BLACK);
  displayPtr->setTextSize(10);
  displayPtr->setTextColor(WHITE);
  // TODO: center this
  displayPtr->setCursor(x + diameter / 4, y + diameter / 5);
  displayPtr->print(content);
}

void renderDisplayItem(Inkplate* displayPtr, display_item_t* displayItemPtr, byte itemIndex, time_t renderTime) {
  short startY = itemIndex * (EFFECTIVE_SCREEN_HEIGHT / ITEMS_ON_SCREEN) + STATUS_BAR_HEIGHT;
  short endY = (itemIndex + 1) * (EFFECTIVE_SCREEN_HEIGHT / ITEMS_ON_SCREEN) + STATUS_BAR_HEIGHT - 1;

  drawCapsule(displayPtr, 20, startY + 20, 124, SCREEN_WIDTH / 3, displayItemPtr->lineRef);

  displayPtr->setTextSize(3);
  displayPtr->setCursor(20, endY - 40);
  displayPtr->setTextColor(BLACK);
  displayPtr->print(displayItemPtr->destination);

  displayPtr->drawThickLine(0, endY, SCREEN_WIDTH - 1, endY, BLACK, 1);
}

void renderStatusBar(Inkplate* displayPtr, time_t renderTime, float batteryVoltage, int rssi) {
  displayPtr->setCursor(0, 0);
  displayPtr->setTextColor(BLACK);
  displayPtr->setTextSize(4);

  tm* localTime = localtime(&renderTime);
  char timeStr[10];
  strftime(timeStr, 10, "%I:%M %p", localTime);
  displayPtr->print(timeStr);

  displayPtr->drawThickLine(0, STATUS_BAR_HEIGHT, SCREEN_WIDTH - 1, STATUS_BAR_HEIGHT, BLACK, 1);
}

void renderScreen(Inkplate* displayPtr, display_item_t (* displayItemsPtr)[20], byte displayItemCount, time_t renderTime, float batteryVoltage, int rssi) {
  // WARNING: all variables used in this function _must_ be saved to RTC RAM to ensure
  // partial updates work!
  displayPtr->clearDisplay();
  displayPtr->setRotation(3);
  displayPtr->setCursor(0, 0);
  
  Serial.printf("Drawing %d display items\n", displayItemCount);
  renderStatusBar(displayPtr, renderTime, batteryVoltage, rssi);
  // TODO: replace this with something remotely pleasant to look at
  for (byte i = 0; i < displayItemCount; i++) {
    display_item_t* displayItem = &(*displayItemsPtr)[i];

    renderDisplayItem(displayPtr, displayItem, i, renderTime);
    // displayPtr->print(displayItem->lineRef);
    // displayPtr->print(" (");
    // displayPtr->print(displayItem->lineName);
    // displayPtr->print(") towards ");
    // displayPtr->println(displayItem->destination);

    // for (int j = 0; j < displayItem->arrivalCount; j++) {
    //   arrival_t* arrival = &displayItem->arrivals[j];
    //   displayPtr->print("    ");
    //   displayPtr->print((arrival->expectedArrivalTime - renderTime) / 60);
    //   displayPtr->print(" min ");
    //   switch (arrival->occupancy) {
    //     case FULL:
    //       displayPtr->print("(full)");
    //       break;
    //     case STANDING_AVAILABLE:
    //       displayPtr->print("(standing)");
    //       break;
    //     case SEATS_AVAILABLE:
    //       displayPtr->print("(seats)");
    //       break;
    //     default:
    //       displayPtr->print("(unknown)");
    //   }
    //   displayPtr->println();
    // }
  }
}