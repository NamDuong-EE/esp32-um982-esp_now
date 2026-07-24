#ifndef ECEF_GEODESY_H
#define ECEF_GEODESY_H

#include <Arduino.h>
#include <cstdint>

bool geodeticToEcefScaled(double latitudeDegrees,
                          double longitudeDegrees,
                          double ellipsoidHeightM,
                          int64_t& xScaled,
                          int64_t& yScaled,
                          int64_t& zScaled);
bool parseGgaUtcMsOfDay(const String& utcField, uint32_t& millisecondsOfDay);

#endif
