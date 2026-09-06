#include "functions/Gnss_Command_Verifier.h"

#include <cctype>
#include <cstring>

namespace {
constexpr std::size_t MAX_OBSERVATION_LINE = 1024;

bool contains(const char* text, const char* token)
{
    return std::strstr(text, token) != nullptr;
}

bool parseGgaQuality(const char* gga, uint8_t& quality)
{
    unsigned field = 0;
    const char* cursor = gga;
    while (*cursor != '\0' && field < 6) {
        if (*cursor++ == ',') {
            ++field;
        }
    }
    if (field != 6 || !std::isdigit(static_cast<unsigned char>(*cursor))) {
        return false;
    }
    unsigned value = 0;
    while (std::isdigit(static_cast<unsigned char>(*cursor))) {
        value = value * 10U + static_cast<unsigned>(*cursor++ - '0');
    }
    if (value > 255U) {
        return false;
    }
    quality = static_cast<uint8_t>(value);
    return true;
}
} // namespace

GnssLineObservation parseGnssCommandObservation(const char* data,
                                                std::size_t length)
{
    GnssLineObservation observation{};
    if (data == nullptr || length == 0) {
        return observation;
    }

    char line[MAX_OBSERVATION_LINE + 1] = {};
    const std::size_t copyLength =
        length > MAX_OBSERVATION_LINE ? MAX_OBSERVATION_LINE : length;
    for (std::size_t index = 0; index < copyLength; ++index) {
        const unsigned char value = static_cast<unsigned char>(data[index]);
        line[index] = value >= 0x20 && value <= 0x7e
                          ? static_cast<char>(std::toupper(value))
                          : ' ';
    }

    const bool isResponse = contains(line, "RESPONSE");
    observation.responseOk =
        isResponse && (contains(line, "RESPONSE: OK") ||
                       contains(line, "RESPONSE : OK"));
    observation.responseError =
        (isResponse && (contains(line, "ERROR") || contains(line, "FAIL"))) ||
        contains(line, "RESPONSE: INVALID");

    if (contains(line, "#MODE") || contains(line, ";MODE ")) {
        if (contains(line, "MODE ROVER")) {
            observation.mode = GnssObservedMode::Rover;
        } else if (contains(line, "MODE BASE")) {
            observation.mode = GnssObservedMode::Base;
        }
    }

    const char* gga = std::strstr(line, "$GPGGA,");
    if (gga == nullptr) {
        gga = std::strstr(line, "$GNGGA,");
    }
    if (gga != nullptr) {
        uint8_t quality = 0;
        if (parseGgaQuality(gga, quality)) {
            observation.hasGga = true;
            observation.ggaQuality = quality;
        }
    }
    return observation;
}
