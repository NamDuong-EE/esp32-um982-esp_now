#include "functions/Ecef_Geodesy.h"

#include <Arduino.h>
#include <cmath>

#include "protocol/RtcmEspNowProtocol.h"

bool geodeticToEcefScaled(double latitudeDegrees,
                          double longitudeDegrees,
                          double ellipsoidHeightM,
                          int64_t& xScaled,
                          int64_t& yScaled,
                          int64_t& zScaled)
{
    if (!std::isfinite(latitudeDegrees) ||
        !std::isfinite(longitudeDegrees) ||
        !std::isfinite(ellipsoidHeightM) ||
        latitudeDegrees < -90.0 || latitudeDegrees > 90.0 ||
        longitudeDegrees < -180.0 || longitudeDegrees > 180.0) {
        return false;
    }

    constexpr double semiMajorAxisM = 6378137.0;
    constexpr double flattening = 1.0 / 298.257223563;
    constexpr double degreesToRadians = 0.017453292519943295769;
    const double latitude = latitudeDegrees * degreesToRadians;
    const double longitude = longitudeDegrees * degreesToRadians;
    const double sinLatitude = std::sin(latitude);
    const double cosLatitude = std::cos(latitude);
    const double sinLongitude = std::sin(longitude);
    const double cosLongitude = std::cos(longitude);
    const double eccentricitySquared = flattening * (2.0 - flattening);
    const double primeVerticalRadius =
        semiMajorAxisM /
        std::sqrt(1.0 - eccentricitySquared * sinLatitude * sinLatitude);

    const double xM =
        (primeVerticalRadius + ellipsoidHeightM) *
        cosLatitude * cosLongitude;
    const double yM =
        (primeVerticalRadius + ellipsoidHeightM) *
        cosLatitude * sinLongitude;
    const double zM =
        (primeVerticalRadius * (1.0 - eccentricitySquared) +
         ellipsoidHeightM) * sinLatitude;
    xScaled = static_cast<int64_t>(
        std::llround(xM * rtcm_espnow::ECEF_SCALE));
    yScaled = static_cast<int64_t>(
        std::llround(yM * rtcm_espnow::ECEF_SCALE));
    zScaled = static_cast<int64_t>(
        std::llround(zM * rtcm_espnow::ECEF_SCALE));
    return xScaled >= -rtcm_espnow::ECEF_SCALED_LIMIT &&
           xScaled <= rtcm_espnow::ECEF_SCALED_LIMIT &&
           yScaled >= -rtcm_espnow::ECEF_SCALED_LIMIT &&
           yScaled <= rtcm_espnow::ECEF_SCALED_LIMIT &&
           zScaled >= -rtcm_espnow::ECEF_SCALED_LIMIT &&
           zScaled <= rtcm_espnow::ECEF_SCALED_LIMIT;
}

bool parseGgaUtcMsOfDay(const String& utcField, uint32_t& millisecondsOfDay)
{
    if (utcField.length() < 6) {
        return false;
    }
    const int hours = utcField.substring(0, 2).toInt();
    const int minutes = utcField.substring(2, 4).toInt();
    const double seconds = utcField.substring(4).toDouble();
    if (hours < 0 || hours > 23 ||
        minutes < 0 || minutes > 59 ||
        !std::isfinite(seconds) || seconds < 0.0 || seconds >= 60.0) {
        return false;
    }
    millisecondsOfDay =
        static_cast<uint32_t>(hours) * 3600000UL +
        static_cast<uint32_t>(minutes) * 60000UL +
        static_cast<uint32_t>(std::llround(seconds * 1000.0));
    if (millisecondsOfDay >= rtcm_espnow::GNSS_MILLISECONDS_PER_DAY) {
        millisecondsOfDay = rtcm_espnow::GNSS_MILLISECONDS_PER_DAY - 1;
    }
    return true;
}
