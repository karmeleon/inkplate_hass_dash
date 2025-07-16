#include "Inkplate.h"
#include "globals.h"
#include "fonts/Cheltenham_Condensed_Bold_Italic24pt7b.h"
#include "img.h"

#define SCREEN_HEIGHT 800
#define SCREEN_WIDTH 600

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