#ifndef TOP_LVL_CONFIG_H
#define TOP_LVL_CONFIG_H

#ifndef PROGRAM_DEBUG
#define PROGRAM_DEBUG 1
#endif

#ifndef CONNECT_USING_WIFI
#define CONNECT_USING_WIFI 1
#endif

#ifndef CONNECT_USING_4G
#define CONNECT_USING_4G 0
#endif

#if !CONNECT_USING_WIFI
#error "ESP32U Rover ESP-NOW requires Wi-Fi STA"
#endif

#if CONNECT_USING_4G
#error "The ESP32U Rover build no longer includes the 4G transport"
#endif

#define RTCM_TRANSPORT_ESPNOW 1

#endif
