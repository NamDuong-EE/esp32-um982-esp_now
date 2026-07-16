#ifndef ESPNOW_TX_MANAGER_H
#define ESPNOW_TX_MANAGER_H

#include <cstddef>
#include <cstdint>

enum class EspNowTxResult : uint8_t {
    Success = 0,
    NotReady,
    InvalidArgument,
    MutexTimeout,
    QueueError,
    CallbackTimeout,
    DeliveryFailed,
};

bool espnowTxSetup();
EspNowTxResult espnowTxSend(const uint8_t destinationMac[6],
                            const uint8_t* data,
                            std::size_t length);
const char* espnowTxResultToString(EspNowTxResult result);

#endif
