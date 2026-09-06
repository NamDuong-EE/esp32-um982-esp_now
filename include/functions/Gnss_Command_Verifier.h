#ifndef GNSS_COMMAND_VERIFIER_H
#define GNSS_COMMAND_VERIFIER_H

#include <cstddef>
#include <cstdint>

enum class GnssObservedMode : uint8_t {
    Unknown = 0,
    Rover = 1,
    Base = 2,
};

struct GnssLineObservation {
    bool responseOk = false;
    bool responseError = false;
    GnssObservedMode mode = GnssObservedMode::Unknown;
    bool hasGga = false;
    uint8_t ggaQuality = 0;
};

GnssLineObservation parseGnssCommandObservation(const char* data,
                                                std::size_t length);

#endif // GNSS_COMMAND_VERIFIER_H
