#include "Inkplate.h"
#include "globals.h"

void renderScreen(Inkplate* displayPtr, display_item_t (* displayItemsPtr)[20], byte displayItemCount, time_t renderTime) {
  // WARNING: all variables used in this function _must_ be saved to RTC RAM to ensure
  // partial updates work!
  displayPtr->clearDisplay();
  displayPtr->setCursor(0, 0);
  displayPtr->setTextSize(4);
  displayPtr->printf("Time: %d\n", renderTime);
  Serial.printf("Drawing %d display items\n", displayItemCount);
  //displayPtr->printf("Battery: %.2f V, WiFi: %d dBm\n", displayPtr->readBattery(), WiFi.RSSI());
  // TODO: replace this with something remotely pleasant to look at
  for (int i = 0; i < displayItemCount; i++) {
    display_item_t* displayItem = &(*displayItemsPtr)[i];
    displayPtr->print(displayItem->lineRef);
    displayPtr->print(" (");
    displayPtr->print(displayItem->lineName);
    displayPtr->print(") towards ");
    displayPtr->println(displayItem->destination);

    for (int j = 0; j < displayItem->arrivalCount; j++) {
      arrival_t* arrival = &displayItem->arrivals[j];
      displayPtr->print("    ");
      displayPtr->print((arrival->expectedArrivalTime - renderTime) / 60);
      displayPtr->print(" min ");
      switch (arrival->occupancy) {
        case FULL:
          displayPtr->print("(full)");
          break;
        case STANDING_AVAILABLE:
          displayPtr->print("(standing)");
          break;
        case SEATS_AVAILABLE:
          displayPtr->print("(seats)");
          break;
        default:
          displayPtr->print("(unknown)");
      }
      displayPtr->println();
    }
  }
}