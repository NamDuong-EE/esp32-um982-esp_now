# ESP32 GNSS Rover

Firmware dùng cho ESP32U nhận dữ liệu hiệu chỉnh RTCM từ Base qua ESP-NOW Long Range, kiểm tra/gom fragment rồi ghi UM980/982 qua UART. Repo này chỉ giữ phần **Rover**; firmware **Base** ở repo riêng.

Phiên bản thử nghiệm hiện tại chỉ đang tập trung kiểm tra khả năng nhận gói tin RTCM của base, chưa có tính năng kết nối đến NTRIP Caster và cập nhập trạng thái lên MQTT

Trong phiên bản thử nghiệm hiện tại rover chạy ở chế độ mặc định không kết nối với Wi-Fi router/AP. ESP32 bật Wi-Fi ở interface `WIFI_STA` để thiết lập giao thức ESP-NOW.

```text
Base repo riêng ── ESP-NOW Long Range ──> ESP32U Rover ── UART ──> UM980/982 Rover
```

## Tiến độ triển khai

- [x] Firmware ESP32U Rover biên dịch thành công bằng PlatformIO.
- [x] Wire protocol ESP-NOW/RTCM, chia fragment và CRC24Q đã có unit test.
- [x] Field mode: không kết nối router/AP, không MQTT mặc định.
- [x] Wi-Fi STA radio cho ESP-NOW LR, peer unicast, FreeRTOS Queue và reassembly đã được cài đặt.
- [x] Chỉ frame RTCM hoàn chỉnh, đúng CRC mới được ghi nhị phân vào UART UM980/982.
- [x] Health counter và log debug qua Serial đã được thêm.
- [x] Source, dependency, environment, board definition và test LoRa/Heltec đã được loại bỏ.
- [x] Xác nhận GPIO16/17 đúng với PCB ESP32U thực tế.
- [x] Đã bỏ MAC Base hard-code; Rover chỉ lấy MAC Base từ NVS/Preferences sau pairing.
- [x] Rà soát README/code ngày 2026-07-07: cấu hình Rover hiện khớp mô tả ESP-NOW LR, ACK ứng dụng, timeout 1500 ms, UART TX buffer 2048 byte và health counter.
- [x] Chạy lại PlatformIO ngày 2026-07-07 bằng `C:\Users\admin\.platformio\penv\Scripts\pio.exe`: firmware build SUCCESS và native unit test 4/4 PASSED.
- [x] Thêm chế độ debug web SoftAP tùy chọn tại `192.168.4.1`, lấy lat/lon/RTK/số vệ tinh từ GGA và hiển thị thêm health ESP-NOW/RTCM.
- [x] Thống nhất với README Base ngày 2026-07-08 về kiến trúc pairing động: bấm nút vật lý, broadcast discovery trong cửa sổ ngắn, confirm rồi lưu MAC vào NVS/Preferences và chạy unicast.
- [x] Triển khai pairing động phía Rover: packet `PAIR_DISCOVERY`/`PAIR_RESPONSE`/`PAIR_CONFIRM`, `network_id`, `pairing_key/auth_tag`, nút pairing và lưu MAC Base vào NVS/Preferences.
- [x] Triển khai phần Base tương ứng để broadcast `PAIR_DISCOVERY`, nhận `PAIR_RESPONSE`, gửi `PAIR_CONFIRM`, lưu MAC Rover và gửi multi-unicast tới tối đa 5 Rover.
- [ ] Provision PMK/LMK và bật `ESPNOW_ENCRYPTION_ENABLED` khi triển khai bảo mật.
- [ ] Kiểm thử end-to-end với Base repo riêng + ESP32U Rover + UM980/982.

## Kiến trúc Rover

### ESP-NOW field mode

- Rover chạy `WIFI_STA`.
- Mặc định không gọi `WiFi.begin()`, không kết nối router/AP và không khởi chạy MQTT.
- Base và Rover phải dùng cùng `ESPNOW_WIFI_CHANNEL` trong `include/Prog_Config.h`; mặc định là channel 6.
- Wi-Fi bật các protocol `WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR`.
- ESP-NOW dùng LR PHY rate 250 Kbps mặc định để ưu tiên tầm xa.
- Rover chỉ nhận packet runtime từ MAC Base đã pair và lưu trong NVS/Preferences.
- Kiến trúc pairing động đã thống nhất với Base theo hướng broadcast discovery bằng nút vật lý, sau đó lưu MAC và chuyển sang unicast.
- Trạng thái/health mặc định chỉ log ra Serial USB.

Luồng khởi tạo:

```text
WiFi.mode(WIFI_STA)
        ↓
Bật WIFI_PROTOCOL_11B/G/N/LR
        ↓
Đặt ESP-NOW channel cố định từ ESPNOW_WIFI_CHANNEL
        ↓
Khởi tạo ESP-NOW peer unicast với Base
        ↓
Nhận fragment RTCM vào FreeRTOS Queue
        ↓
Reassembly, kiểm tra CRC24Q
        ↓
Serial1.write(frame, frameLength) vào UM980/982
```

### Kiến trúc pairing động Base/Rover

Mục tiêu của pairing động là Base và Rover tự tìm MAC của nhau mà không bị lẫn với ESP32/ESP-NOW khác cùng khu vực. Discovery chỉ dùng trong giai đoạn ghép cặp, không dùng để gửi RTCM thường xuyên.

Chính sách đã thống nhất với repo Base: **pair theo nút vật lý**.

#### Normal mode

- Rover đọc MAC Base đã lưu trong NVS/Preferences và chỉ chấp nhận RTCM runtime từ MAC đó.
- Nếu chưa có MAC đã lưu, Rover vẫn khởi động ESP-NOW để chờ pairing nhưng không nhận RTCM runtime unicast.
- Base đọc MAC Rover đã lưu trong NVS/Preferences và chỉ gửi RTCM unicast tới MAC đó.
- Không gửi broadcast discovery khi đang chạy bình thường.
- Runtime RTCM/ACK giai đoạn đầu vẫn giữ protocol v1 hiện tại: data header 16 byte, ACK 12 byte. `network_id` chỉ dùng trong packet pairing trước; sau khi pairing ổn định mới cân nhắc nâng runtime header lên v2 để thêm `network_id`.
- RTCM gốc không bị sửa; mọi header ESP-NOW phải được Rover bỏ trước khi ghi dữ liệu RTCM nhị phân xuống UART cho UM980/982.

#### Pairing mode

Pairing mode chỉ mở trong một cửa sổ ngắn, ví dụ 60 giây, khi người dùng bấm/giữ nút vật lý trên cả Base và Rover cần ghép.

1. Người dùng bấm nút pairing trên Base để Base vào pairing mode.
2. Người dùng bấm nút pairing trên đúng Rover muốn ghép. Các Rover khác không ở pairing mode sẽ không trả lời discovery.
3. Base thêm broadcast peer `FF:FF:FF:FF:FF:FF` và gửi `PAIR_DISCOVERY` định kỳ, ví dụ 500 ms/lần.
4. Rover chỉ xử lý discovery nếu đang ở pairing mode và packet hợp lệ.
5. Rover trả lời unicast `PAIR_RESPONSE` về MAC của Base lấy từ ESP-NOW receive callback.
6. Base nhận response hợp lệ đầu tiên, thêm Rover làm peer unicast, rồi gửi `PAIR_CONFIRM`.
7. Base lưu MAC Rover vào NVS/Preferences và thoát pairing mode.
8. Rover chỉ lưu MAC Base sau khi nhận `PAIR_CONFIRM` hợp lệ, đúng nonce, đúng `network_id` và đúng `auth_tag`, rồi thoát pairing mode.
9. Từ thời điểm này Base và Rover dùng unicast cho RTCM/ACK; broadcast không còn dùng trong normal mode.

#### Packet pairing thống nhất

Các packet pairing không mang RTCM. Chúng là packet điều khiển riêng của protocol ESP-NOW:

```text
packetType:
    1 = RTCM_DATA
    2 = FRAME_ACK
    3 = PAIR_DISCOVERY
    4 = PAIR_RESPONSE
    5 = PAIR_CONFIRM

role:
    1 = BASE
    2 = ROVER
```

Common header tối thiểu cho mọi packet:

```cpp
struct EspNowCommonHeader {
    uint16_t magic;      // 0x5452
    uint8_t  version;    // 1
    uint8_t  packetType;
};
```

Packet pairing đề xuất:

```text
PAIR_DISCOVERY:
    common header
    role = BASE
    network_id
    base_device_id
    base_nonce
    pairing_window_ms
    auth_tag

PAIR_RESPONSE:
    common header
    role = ROVER
    network_id
    rover_device_id
    rover_nonce
    base_nonce_echo
    auth_tag

PAIR_CONFIRM:
    common header
    role = BASE
    network_id
    base_nonce
    rover_nonce
    auth_tag
```

`network_id` là ID dùng chung cho một hệ thống quan trắc. `auth_tag` phải được tạo từ `pairing_key` dùng chung giữa firmware Base/Rover, ví dụ HMAC hoặc một hàm xác thực nhẹ hơn nếu muốn giữ code nhỏ. Không nên chỉ dựa vào MAC vì thiết bị lạ vẫn có thể nghe broadcast.

#### Cấu hình pairing dự kiến

Các tên cấu hình nên thống nhất với repo Base trong `include/Prog_Config.h`:

```cpp
inline constexpr bool ESPNOW_PAIRING_ENABLED = true;
inline constexpr int PAIRING_BUTTON_PIN = 0; // Chốt theo PCB thực tế trước khi dùng
inline constexpr uint32_t PAIRING_WINDOW_MS = 60000;
inline constexpr uint32_t PAIR_DISCOVERY_INTERVAL_MS = 500;
inline constexpr uint32_t PAIR_CONFIRM_TIMEOUT_MS = 3000;
inline constexpr uint32_t ESPNOW_NETWORK_ID = 0xA1700001;
inline constexpr uint8_t ESPNOW_PAIRING_KEY[16] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
```

`PAIRING_BUTTON_PIN` phải được chốt theo PCB Rover thực tế. Không nên dùng nút boot nếu thao tác pairing có thể làm người dùng vô tình đưa board vào bootloader khi reset/nạp firmware.

Rover hiện đã triển khai phần pairing phía nhận:

- Khi khởi động, Rover ưu tiên đọc MAC Base từ NVS namespace `espnow`, key `base_mac`.
- Nếu NVS chưa có MAC, Rover chờ pairing và không dùng MAC hard-code.
- Giữ nút pairing `PAIRING_BUTTON_PIN` trong `PAIRING_BUTTON_HOLD_MS` để mở pairing window.
- Trong pairing window, Rover nhận `PAIR_DISCOVERY` hợp lệ, gửi `PAIR_RESPONSE` unicast về source MAC của Base, chờ `PAIR_CONFIRM`, rồi lưu MAC Base vào NVS.
- Sau khi lưu, ACK và packet RTCM runtime dùng MAC Base vừa pair.
- ESP-NOW receive callback chỉ copy packet pairing vào pending state; validate/auth/log/send response/lưu NVS chạy trong `espnowLoop()` để không giữ Wi-Fi callback lâu.

#### Quy tắc chống lẫn thiết bị

- Cùng channel mới thấy nhau, nhưng cùng channel chưa đủ để pair.
- Chỉ thiết bị đang ở pairing mode mới trả lời discovery.
- Packet phải đúng `magic`, `version`, `packetType`, `role`.
- Packet phải đúng `network_id`.
- Packet phải có `auth_tag` hợp lệ từ `pairing_key`.
- Rover chỉ lưu MAC Base sau `PAIR_CONFIRM`, không lưu ngay khi thấy `PAIR_DISCOVERY`.
- Rover đã pair sẽ không ghi đè MAC Base đang lưu nếu người dùng không bấm nút pairing/re-pair.
- Runtime RTCM vẫn kiểm tra source MAC đã pair/lưu trong NVS trước khi đưa packet vào queue reassembly.

### Kết quả review pipeline Base/Rover ngày 2026-07-06

Kết luận sau tối ưu: wire protocol Base/Rover khớp về data header 16 byte, ACK 12 byte, payload fragment 234 byte, `streamId`, `frameSequence`, CRC24Q, channel 6 và LR 250 Kbps. Với RTCM 1 Hz, băng thông trung bình đủ; hai phía đã có buffer/queue, deadline và telemetry để phát hiện backlog.

Các phần đã làm tốt:

1. ESP-NOW receive callback chỉ kiểm tra/copy packet vào FreeRTOS queue rồi thoát.
2. Queue dài 16 packet, chiếm khoảng 4 KB và chứa được ba RTCM frame kích thước cực đại cùng một fragment.
3. Task riêng ghép fragment không phụ thuộc thứ tự nhận, dùng bitmap để loại fragment trùng.
4. Header, kích thước fragment, RTCM preamble/length và CRC24Q đều được kiểm tra trước khi ghi UART.
5. Rover chỉ nhận đúng MAC Base đã cấu hình và dùng cùng channel/LR với Base.

Các tối ưu đã triển khai:

1. Timeout reassembly tăng lên 1500 ms, lớn hơn deadline một attempt của Base là 1000 ms. Fragment 0 trùng của lần retry làm mới thời điểm bắt đầu frame.
2. Rover gửi ACK ứng dụng theo `streamId + frameSequence` sau khi ghép đủ, CRC đúng và `Serial1.write()` nhận đủ byte.
3. Sequence đã hoàn thành nếu được gửi lại sẽ chỉ tạo ACK mới, không ghi RTCM lần hai vào UART.
4. UART TX buffer tăng lên 2048 byte trước `Serial1.begin()`, đủ chứa RTCM frame cực đại 1029 byte mà không giữ task reassembly trong toàn bộ thời gian phát vật lý.
5. Health JSON đã có packet nhận/sai nguồn/sai header, queue high-water/overflow, duplicate, timeout, sequence gap, UART write error và ACK queued/fail.
6. Unit test có thêm cấu trúc và validate ACK; tổng cộng 4 test protocol.

Phần còn lại cần kiểm chứng:

1. Reassembly vẫn giữ một frame vì Base gửi tuần tự, không xen kẽ frame. Nếu Base đổi sang nhiều frame in-flight thì Rover phải có nhiều slot.
2. Chưa có unit test đầy đủ cho state machine out-of-order, missing fragment, timeout, sequence wrap, queue overflow và UART failure.
3. ACK xác nhận frame đã vào UART TX buffer, không xác nhận UM980/UM982 đã chuyển sang RTK Float/Fixed.
4. Giữ UART 115200 và LR 250 Kbps cho tới khi telemetry phần cứng chứng minh cần tăng tốc.


### MQTT tùy chọn

MQTT vẫn còn trong code để debug hoặc nhận lệnh cấu hình khi cần, nhưng mặc định tắt:

```cpp
inline constexpr bool WIFI_CONNECT_TO_ROUTER_ENABLED = false;
inline constexpr bool ROVER_MQTT_ENABLED = false;
inline constexpr bool MQTT_PUBLISH_HEALTH_ENABLED = false;
```

Nếu muốn bật lại MQTT:

1. Đặt `ROVER_MQTT_ENABLED = true`.
2. Đặt `WIFI_CONNECT_TO_ROUTER_ENABLED = true`.
3. Cấu hình `WIFI_SSID`, `WIFI_PASSWORD` và thông tin MQTT.
4. Khóa router 2.4 GHz cùng channel với `ESPNOW_WIFI_CHANNEL`.

### Debug web SoftAP tùy chọn

Mặc định debug web tắt để field mode gọn nhất. Khi cần xem nhanh trạng thái Rover ngoài hiện trường, bật trong `include/Prog_Config.h`:

```cpp
#define DEBUG_WEB_ENABLED 1
```

Khi bật, Rover chạy `WIFI_AP_STA`, mở SoftAP riêng và host trang debug tại:

```text
SSID: ESP32-Rover-Debug
Password: 12345678
URL: http://192.168.4.1
API: http://192.168.4.1/api/status
```

SoftAP dùng cùng `ESPNOW_WIFI_CHANNEL` với ESP-NOW để tránh đổi channel radio. Trang HTML poll JSON mỗi 1 giây và chỉ lấy dữ liệu GNSS từ câu GGA:

- `lat`, `lon`
- `rtk_status` là số GGA fix quality, ví dụ `4` hoặc `5`
- `satellites`
- `last_gga_age_ms`

Phần health trên trang hiển thị thêm `espnow_ready`, `rtcm_frames`, `last_rtcm_age_ms`, CRC error, queue overflow, sequence gap, ACK và free heap.

## Kết nối ESP32U với UM980/982

| ESP32U | UM980/982 | Ghi chú |
|---|---|---|
| GPIO16 / RX | TX | UM980/982 gửi NMEA về ESP32U |
| GPIO17 / TX | RX | ESP32U gửi RTCM/lệnh vào UM980/982 |
| GND | GND | Bắt buộc chung mass |

Lưu ý:

- Mức logic UART phải tương thích 3.3 V.
- UART GNSS mặc định là `115200`, cấu hình `SERIAL_8N1`.
- UART TX buffer được đặt 2048 byte trước khi mở `Serial1`.
- RTCM là dữ liệu nhị phân, bắt buộc dùng `Serial1.write(data, length)`.
- Không ghi log/debug vào `Serial1`; mọi log chỉ ra `Serial` USB.

## Giao thức chia gói RTCM qua ESP-NOW

Đây là hợp đồng giao tiếp giữa repo Base riêng và Rover repo này.

Lưu ý pairing động: giai đoạn đầu chỉ thêm packet điều khiển `PAIR_DISCOVERY`/`PAIR_RESPONSE`/`PAIR_CONFIRM`; data header RTCM 16 byte và ACK 12 byte vẫn giữ nguyên để không phá pipeline RTCM đã test. Sau khi pairing ổn định mới cân nhắc nâng protocol runtime lên version mới để thêm `network_id` vào data/ACK.

### Nguyên tắc

- ESP-NOW v1 tối đa 250 byte mỗi packet.
- Một RTCM3 frame tối đa 1029 byte gồm 3 byte header, tối đa 1023 byte payload và 3 byte CRC24Q.
- Base phải tách đúng từng RTCM3 frame trước khi chia fragment.
- Header protocol dài 16 byte; payload ESP-NOW mỗi fragment tối đa 234 byte.
- Một RTCM frame tối đa cần 5 fragment.
- Tất cả số nguyên nhiều byte trên wire dùng little-endian.
- `fragmentCount = ceil(frameLength / 234)`.
- Mọi fragment trừ fragment cuối phải có `payloadLength = 234`.

### Cấu trúc packet

```cpp
#pragma pack(push, 1)
struct RtcmEspNowHeader {
    uint16_t magic;            // 0x5452; wire little-endian là 0x52, 0x54 ("RT")
    uint8_t  version;          // 1
    uint8_t  packetType;       // 1 = RTCM_DATA
    uint16_t streamId;         // Thay đổi mỗi lần Base khởi động
    uint32_t frameSequence;    // Tăng 1 sau mỗi RTCM frame
    uint16_t frameLength;      // Tổng độ dài RTCM frame: 6..1029 byte
    uint8_t  fragmentIndex;    // 0..fragmentCount-1
    uint8_t  fragmentCount;    // 1..5
    uint16_t payloadLength;    // 1..234 byte
};
#pragma pack(pop)

static_assert(sizeof(RtcmEspNowHeader) == 16);
```

ACK Rover gửi ngược về Base sau khi frame được chấp nhận vào UART:

```cpp
struct RtcmEspNowAck {
    uint16_t magic;          // 0x5452
    uint8_t  version;        // 1
    uint8_t  packetType;     // 2 = FRAME_ACK
    uint16_t streamId;
    uint32_t frameSequence;
    uint8_t  status;         // 1 = WRITTEN
    uint8_t  reserved;
};

static_assert(sizeof(RtcmEspNowAck) == 12);
```

Độ dài packet gửi thực tế:

```text
sizeof(RtcmEspNowHeader) + payloadLength
```

Không gửi toàn bộ buffer 250 byte nếu fragment cuối không dùng hết payload.

### Quy tắc phía Base repo riêng

1. Tìm preamble RTCM3 `0xD3`.
2. Đọc trường độ dài 10 bit để xác định toàn bộ RTCM frame.
3. Kiểm tra CRC24Q trước khi gửi.
4. Chia frame thành các fragment tối đa 234 byte.
5. Gửi fragment theo thứ tự tăng dần.
6. Nếu send callback báo lỗi, retry ngắn; nếu vẫn lỗi thì bỏ frame hiện tại.
7. Tăng `frameSequence` sau mỗi frame, kể cả frame bị bỏ.
8. Tạo `streamId` mới khi Base khởi động lại.
9. Base phải gửi unicast tới MAC STA của Rover.

### Quy tắc phía Rover

ESP-NOW receive callback chỉ làm việc ngắn:

1. Đọc tối thiểu common header để biết `magic`, `version` và `packetType`.
2. Nếu là packet pairing và Rover đang ở pairing mode, xử lý theo state machine pairing; không yêu cầu source MAC đã lưu.
3. Nếu là RTCM runtime, kiểm tra MAC nguồn phải đúng MAC Base đã pair/lưu trong NVS.
4. Kiểm tra độ dài packet, `magic`, `version` và giới hạn header.
5. Copy packet RTCM hợp lệ vào FreeRTOS Queue.
6. Thoát callback; không ghi UART, log dài, truy cập NVS hoặc gọi `delay()`.

Task reassembly:

1. Nhóm fragment theo `streamId` và `frameSequence`.
2. Lưu fragment theo `fragmentIndex`, dùng bitmap để phát hiện fragment đã có.
3. Bỏ fragment trùng.
4. Nếu fragment không nhất quán thì bỏ toàn bộ frame.
5. Nếu queue tràn hoặc quá thời gian 1500 ms, bỏ frame đang ghép.
6. Khi đủ fragment, kiểm tra `frameLength`, preamble RTCM3 và CRC24Q.
7. Chỉ khi hợp lệ mới gọi `Serial1.write(frame, frameLength)`.
8. Khi UART nhận đủ frame, gửi ACK ứng dụng về Base; sequence hoàn thành bị gửi lại chỉ được ACK lại, không ghi UART lần hai.

## Source chính

| File | Trách nhiệm |
|---|---|
| `include/protocol/RtcmEspNowProtocol.h` | Định nghĩa wire protocol, hằng số và giới hạn packet |
| `src/protocol/RtcmEspNowProtocol.cpp` | Validate header, fragment boundary và CRC24Q |
| `include/hardware/Espnow_handler.h` | API khởi tạo ESP-NOW receive phía Rover |
| `src/hardware/Espnow_handler.cpp` | Cấu hình STA/LR, peer Base và đưa packet vào queue |
| `include/functions/Rtcm_EspNow_Handler.h` | API reassembly/validate RTCM |
| `src/functions/Rtcm_EspNow_Handler.cpp` | Ghép fragment, kiểm tra CRC24Q và ghi vào UM980/982 |
| `src/main.cpp` | Entry point firmware Rover |
| `src/helper.cpp` | Parse NMEA, health counter, log Serial |
| `include/hardware/DebugWeb_handler.h` | API debug web SoftAP tùy chọn |
| `src/hardware/DebugWeb_handler.cpp` | Host HTML/API debug tại `192.168.4.1` khi `DEBUG_WEB_ENABLED=1` |
| `include/Prog_Config.h` | GPIO UART, MAC Base, channel, MQTT tùy chọn, PMK/LMK |
| `platformio.ini` | Environment `esp32u_rover_espnow` và native protocol test |

## Build và nạp firmware Rover

### Cấu hình trước khi build

Mở `include/Prog_Config.h` và kiểm tra:

1. `ESPNOW_WIFI_CHANNEL`: Base và Rover phải giống nhau.
2. `WIFI_CONNECT_TO_ROUTER_ENABLED = false`.
3. `ROVER_MQTT_ENABLED = false`.
4. `RX_GNSS`, `TX_GNSS` và `GNSS_BAUD`.
5. MAC Base không còn cấu hình hard-code. Rover đọc MAC Base từ NVS/Preferences sau pairing. Nếu chưa pair, ESP-NOW vẫn khởi động để chờ pairing nhưng RTCM runtime unicast chưa hoạt động.

### PlatformIO CLI

```powershell
# Build Rover
pio run -e esp32u_rover_espnow

# Upload tự động tìm cổng COM
pio run -e esp32u_rover_espnow -t upload

# Hoặc chỉ định cổng
pio run -e esp32u_rover_espnow -t upload --upload-port COM5

# Mở Serial Monitor
pio device monitor --port COM5 --baud 115200
```

Nếu `pio` chưa có trong `PATH`:

```powershell
python -m platformio run -e esp32u_rover_espnow -t upload --upload-port COM5
python -m platformio device monitor --port COM5 --baud 115200
```

### Visual Studio Code + PlatformIO

1. Mở thư mục gốc repository, nơi có `platformio.ini`.
2. Cắm ESP32U và xác định cổng COM trong Device Manager.
3. Chọn environment `esp32u_rover_espnow`.
4. Nhấn **Build**.
5. Nhấn **Upload**.
6. Nếu dừng ở `Connecting...`, giữ **BOOT**, nhấn **EN/RESET**, thả **EN/RESET**, rồi thả **BOOT** khi bắt đầu ghi.
7. Mở Serial Monitor ở 115200 baud.

## Log mong đợi

```text
[GNSS] UART1 baud=115200 RX=16 TX=17
[WIFI] Khong ket noi router/AP; chi dung STA radio cho ESP-NOW
[WIFI] Local STA MAC: XX:XX:XX:XX:XX:XX
[WIFI] ESP-NOW fixed channel: 6
[ESP-NOW] Ready, STA channel=6, LR=250 Kbps
[MQTT] Da tat theo cau hinh ROVER_MQTT_ENABLED=false
[SETUP] Khoi dong hoan tat
```

Nếu `DEBUG_WEB_ENABLED=1`, log sẽ có thêm:

```text
[WIFI] Debug web bat; Wi-Fi mode AP+STA
[DEBUG_WEB] SoftAP SSID=ESP32-Rover-Debug IP=192.168.4.1 channel=6 MAC=XX:XX:XX:XX:XX:XX
```

Khi `ESPNOW_PAIRING_ENABLED=true`, log pairing mong đợi:

```text
[PAIR] Pairing button GPIO=0 active_low hold_ms=1500
[PAIR] Loaded Base MAC from NVS: XX:XX:XX:XX:XX:XX
```

Hoặc nếu chưa pair lần nào:

```text
[PAIR] No stored Base MAC; hold pairing button to pair
[ESP-NOW][WARN] Chua co MAC Base; chi cho pairing mode
```

Khi giữ nút pairing đủ lâu:

```text
[PAIR] Rover pairing mode ON for 60000 ms, network_id=0xA1700001
[PAIR] PAIR_DISCOVERY hop le, da gui PAIR_RESPONSE toi XX:XX:XX:XX:XX:XX
[PAIR] PAIR_CONFIRM hop le tu Base XX:XX:XX:XX:XX:XX
[PAIR] Saved Base MAC to NVS and switched runtime peer to XX:XX:XX:XX:XX:XX
```

Nếu Rover chưa pair, hãy giữ nút pairing trên Rover và kích hoạt pairing trên Base để hai bên lưu MAC vào NVS.

## Kiểm tra sau khi nạp

1. Base repo riêng và Rover phải cùng `ESPNOW_WIFI_CHANNEL`.
2. Base phải gửi unicast tới MAC STA của Rover được in ở log `Local STA MAC`.
3. Rover health: `rtcm_frames` phải tăng và `last_rtcm_age_ms` phải được cập nhật.
4. `rtcm_crc_errors`, `rtcm_queue_overflow` và `rtcm_sequence_gaps` lý tưởng bằng 0.
5. UM980/982 Rover phải nhận RTCM và chuyển sang RTK Float/Fixed.

## Xử lý lỗi thường gặp

| Hiện tượng | Kiểm tra |
|---|---|
| Không thấy cổng COM | Đổi cáp USB, cổng USB hoặc cài driver CP210x/CH340 |
| Upload timeout | Chọn đúng COM và dùng nút BOOT/EN |
| ESP-NOW không Ready | Kiểm tra PMK/LMK, `ESPNOW_WIFI_CHANNEL` và log pairing |
| Có ESP-NOW Ready nhưng không có RTCM | Pair Base/Rover trước, kiểm tra cùng channel và Base gửi tới MAC Rover đã lưu |
| CRC error tăng | Kiểm tra protocol phía Base, `frameLength`, fragment index/count và CRC24Q |
| UM980/982 không nhận correction | Kiểm tra TX/RX nối chéo, chung GND, baud và mức logic UART |
| Serial Monitor ký tự rác | Đặt monitor baud 115200 |

## Thứ tự triển khai tiếp theo

1. [x] Xác nhận GPIO16/17 trên PCB ESP32U thực tế.
2. [x] Bỏ `ESPNOW_BASE_MAC` hard-code; MAC Base chỉ đến từ pairing/NVS.
3. [ ] Thêm `C:\Users\admin\.platformio\penv\Scripts` vào PATH nếu muốn gọi trực tiếp `pio` trong shell mới.
4. [x] Đồng bộ hằng số pairing phía Rover: `PAIR_DISCOVERY`, `PAIR_RESPONSE`, `PAIR_CONFIRM`, `role`, `network_id`, `pairing_key/auth_tag`.
5. [x] Thêm cấu hình nút pairing phía Rover: GPIO, debounce/hold, cửa sổ pairing, timeout và log trạng thái.
6. [x] Thêm NVS/Preferences phía Rover để lưu/đọc MAC Base đã pair, không dùng fallback MAC hard-code.
7. [x] Sửa ESP-NOW receive callback phía Rover: packet pairing được xử lý khi đang pairing mode; packet RTCM runtime vẫn bắt buộc đúng MAC Base đã pair.
8. [x] Triển khai state machine Rover cho `PAIR_DISCOVERY` → `PAIR_RESPONSE` → `PAIR_CONFIRM` và chỉ lưu MAC Base sau confirm hợp lệ.
9. [x] Triển khai Base-side pairing packet theo đúng struct/auth tag của Rover.
10. [ ] Kiểm thử pairing 1 Base - 1 Rover trên phần cứng, reset nguồn hai bên và xác nhận normal mode tự dùng MAC đã lưu.
11. [ ] Kiểm thử pair lần lượt 1 Base - nhiều Rover; Base hiện hỗ trợ lưu tối đa 5 Rover và gửi RTCM multi-unicast.
12. [ ] Sau khi pairing ổn định, cân nhắc runtime protocol v2 để thêm `network_id` vào data/ACK header.
12. [ ] Provision PMK/LMK và bật `ESPNOW_ENCRYPTION_ENABLED` khi triển khai bảo mật.
13. [ ] Kiểm thử end-to-end Base repo riêng → ESP32U Rover → UM980/982.
14. [ ] Đo tầm xa LR 250 Kbps, sau đó thử LR 500 Kbps nếu cần.

## Kết quả kiểm tra phần mềm gần nhất

- Lần kiểm tra phần mềm gần nhất: 2026-07-08.
- PlatformIO Core: **6.1.19** tại `C:\Users\admin\.platformio\penv\Scripts\pio.exe`.
- PlatformIO `esp32u_rover_espnow`: **SUCCESS**.
- RAM: 46,256 / 327,680 byte (14.1%) với `DEBUG_WEB_ENABLED=0`.
- Flash: 772,245 / 1,310,720 byte (58.9%) với `DEBUG_WEB_ENABLED=0`.
- Native unit test: **5/5 PASSED**.
- Artifact `.pio/build/esp32u_rover_espnow/firmware.bin` hiện có kích thước 778,816 byte, cập nhật lần cuối 2026-07-07 10:19.
- MAC Base hard-code đã được loại bỏ; cần pair Base/Rover để có MAC trong NVS trước khi chạy RTCM runtime.
- Tối ưu ngày 2026-07-06 đã đồng bộ timeout 1500 ms với deadline Base, thêm ACK ứng dụng/chống ghi trùng, UART TX buffer 2048 byte và telemetry đầy đủ. Vẫn cần test state machine và kiểm thử RTK end-to-end trên phần cứng.
- Rà soát ngày 2026-07-07: `pio` chưa có trong PATH của shell hiện tại, nhưng chạy trực tiếp bằng đường dẫn trong `.platformio\penv\Scripts` thành công.
- Debug web đã được build thử với `DEBUG_WEB_ENABLED=1`: **SUCCESS**, RAM 46,672 byte (14.2%), Flash 809,057 byte (61.7%).
- Đã set cứng công suất phát WiFi/ESP-NOW của Rover bằng `WiFi.setTxPower(WIFI_POWER_19_5dBm)` ngay sau khi bật STA/AP+STA radio. Firmware đọc lại `esp_wifi_get_max_tx_power()` và in log `[WIFI] TX power fixed raw=... dBm=...` để xác nhận runtime.
- Đã build xác nhận sau khi set TX power 19.5 dBm: `esp32u_rover_espnow` SUCCESS, RAM 46,696/327,680 byte (14.3%), Flash 810,205/1,310,720 byte (61.8%).
- Đã thêm RSSI ESP-NOW Base vào web debug Rover: dùng promiscuous callback lọc source MAC của Base, cập nhật `espnow_rssi_dbm` trong `/api/status` và hiển thị card `Base RSSI` trên trang `192.168.4.1`.
- Đã build xác nhận sau khi thêm RSSI web debug và bỏ `espnow_rssi_age_ms`/`espnow_rssi_samples`: `esp32u_rover_espnow` SUCCESS, RAM 46,704/327,680 byte (14.3%), Flash 811,597/1,310,720 byte (61.9%).
- Đã triển khai Rover-side pairing động ngày 2026-07-08: packet pairing riêng, auth tag nhẹ bằng `ESPNOW_PAIRING_KEY`, nút pairing, NVS lưu MAC Base và runtime ACK/RTCM dùng MAC active. Native unit test tăng lên **5/5 PASSED**.
- Đã build xác nhận sau pairing Rover-side: `esp32u_rover_espnow` SUCCESS, RAM 46,896/327,680 byte (14.3%), Flash 820,241/1,310,720 byte (62.6%).
- Đã đồng bộ với Base-side pairing đã triển khai: Base broadcast `PAIR_DISCOVERY`, Rover trả lời `PAIR_RESPONSE`, nhận `PAIR_CONFIRM` rồi lưu MAC Base. Build Rover `esp32u_rover_espnow` vẫn **SUCCESS** RAM 46,896/327,680 byte (14.3%), Flash 820,241/1,310,720 byte (62.6%); native protocol test **5/5 PASSED**.
- Đã xóa MAC Base hard-code khỏi Rover ngày 2026-07-08: không còn `ESPNOW_BASE_MAC`, không còn fallback MAC tĩnh; Rover chờ pairing nếu NVS chưa có MAC Base. Web debug cũng bỏ `base_mac_stored` khỏi bảng hiển thị vì `base_provisioned` hiện chỉ đúng khi đã có MAC từ NVS. Build Rover `esp32u_rover_espnow` **SUCCESS** RAM 46,896/327,680 byte (14.3%), Flash 820,073/1,310,720 byte (62.6%); native protocol test **5/5 PASSED**.
- Web debug đang bật lại `ESPNOW_RSSI_MONITOR_ENABLED=true` để hiển thị RSSI Base trên trang debug. Nếu một số client báo lỗi khi vào SoftAP, có thể thử đổi password SoftAP hoặc tạm đặt flag này về `false` để tách lỗi kết nối khỏi RSSI sniffing.
