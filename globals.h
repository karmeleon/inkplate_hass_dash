#ifndef GLOBALS
#define GLOBALS
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



#endif