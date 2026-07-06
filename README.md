# ESP32 GNSS Rover

Firmware Rover dùng ESP32U nhận dữ liệu hiệu chỉnh RTCM từ Base qua ESP-NOW Long Range, kiểm tra/gom fragment rồi ghi nhị phân vào UM980/982 qua UART. Repo này chỉ giữ phần **Rover**; firmware **Base** sẽ được tách sang repository riêng.

Chế độ mặc định ngoài thực địa không kết nối Wi-Fi router/AP và không dùng MQTT. ESP32 vẫn bật Wi-Fi radio ở `WIFI_STA` vì ESP-NOW chạy trên Wi-Fi driver của ESP32.

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
- [ ] Xác nhận GPIO16/17 đúng với PCB ESP32U thực tế.
- [x] Đã điền MAC STA của Base `68:09:47:F8:48:90` vào `ESPNOW_BASE_MAC`.
- [ ] Provision PMK/LMK và bật `ESPNOW_ENCRYPTION_ENABLED` khi triển khai bảo mật.
- [ ] Kiểm thử end-to-end với Base repo riêng + ESP32U Rover + UM980/982.

## Kiến trúc Rover

### ESP-NOW field mode

- Rover chạy `WIFI_STA`.
- Mặc định không gọi `WiFi.begin()`, không kết nối router/AP và không khởi chạy MQTT.
- Base và Rover phải dùng cùng `ESPNOW_WIFI_CHANNEL` trong `include/Prog_Config.h`; mặc định là channel 6.
- Wi-Fi bật các protocol `WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR`.
- ESP-NOW dùng LR PHY rate 250 Kbps mặc định để ưu tiên tầm xa.
- Rover chỉ nhận packet từ `ESPNOW_BASE_MAC` đã cấu hình.
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

### Kết quả review pipeline Base/Rover ngày 2026-07-06

Kết luận: wire protocol của Base và Rover đang khớp về header 16 byte, payload fragment 234 byte, `streamId`, `frameSequence`, CRC24Q, channel 6 và LR 250 Kbps. Với RTCM 1 Hz, băng thông trung bình đủ; Rover đã có queue nên chịu burst tốt hơn Base. Tuy nhiên hệ thống chưa thể coi là tối ưu hoàn chỉnh cho vận hành quan trắc dài hạn.

Các phần đã làm tốt:

1. ESP-NOW receive callback chỉ kiểm tra/copy packet vào FreeRTOS queue rồi thoát.
2. Queue dài 16 packet, chiếm khoảng 4 KB và chứa được ba RTCM frame kích thước cực đại cùng một fragment.
3. Task riêng ghép fragment không phụ thuộc thứ tự nhận, dùng bitmap để loại fragment trùng.
4. Header, kích thước fragment, RTCM preamble/length và CRC24Q đều được kiểm tra trước khi ghi UART.
5. Rover chỉ nhận đúng MAC Base đã cấu hình và dùng cùng channel/LR với Base.

Các điểm cần tối ưu tiếp:

1. **Timeout Base/Rover chưa đồng bộ.** Rover bỏ frame sau 500 ms tính từ fragment đầu, trong khi Base chờ callback tối đa 250 ms và có hai retry. Chỉ một fragment thất bại ba attempt có thể chiếm khoảng 765 ms; Rover có thể timeout một frame mà Base vẫn đang retry. Cần đo callback thực tế rồi đặt một send deadline chung; trong giai đoạn test có thể tăng timeout Rover, nhưng vẫn phải bỏ correction quá cũ.
2. **Chưa có ACK ứng dụng.** Base chỉ nhận send callback mức MAC. Rover chưa gửi ACK sau khi ghép đủ, CRC đúng và `Serial1.write()` thành công, nên Base chưa biết correction đã đi hết đường dữ liệu.
3. **`Serial1.write()` có thể chặn task reassembly.** Với TX buffer mặc định và UART 115200, frame cực đại 1029 byte cần khoảng 89 ms trên wire. Queue 16 packet hiện đủ cho tải 1 Hz thông thường, nhưng cần theo dõi queue high-water/overflow hoặc thêm TX buffer/UART writer task nếu tăng tần suất.
4. **Reassembly chỉ giữ một frame đang ghép.** Điều này đúng với Base hiện tại vì Base gửi stop-and-wait và không xen kẽ hai frame. Nếu Base chuyển sang pipeline nhiều frame, Rover phải dùng nhiều reassembly slot.
5. **Health JSON chưa xuất hết counter đã có.** Cần thêm `packetsReceived`, `packetsInvalidHeader`, `packetsWrongSource`, `duplicateFragments`, `frameTimeouts` và `uartWriteErrors`; hiện log chỉ có frame, CRC, queue overflow, sequence gap và tuổi frame.
6. **Unit test mới kiểm tra protocol cơ bản.** Chưa test reassembly out-of-order, duplicate, missing fragment, timeout, đổi `streamId`, sequence wrap, queue overflow và UART write failure.

Thứ tự triển khai đề xuất:

1. Đồng bộ deadline/timeout giữa Base và Rover, thêm đo thời gian từ fragment đầu tới frame hoàn chỉnh.
2. Mở rộng health counter và queue high-water để kiểm chứng bằng phần cứng.
3. Thêm test cho toàn bộ state machine reassembly.
4. Thêm ACK ứng dụng theo `streamId + frameSequence` sau khi Rover ghi UART thành công.
5. Chỉ tăng UART baud hoặc chuyển LR 500 Kbps khi số đo cho thấy tải thực chạm trần.

Giới hạn review: repo Rover không có log phần cứng end-to-end mới nhất, nên chưa xác nhận tỷ lệ queue overflow, latency thực, UM980/982 nhận correction và trạng thái RTK Float/Fixed.

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

## Kết nối ESP32U với UM980/982

| ESP32U | UM980/982 | Ghi chú |
|---|---|---|
| GPIO16 / RX | TX | UM980/982 gửi NMEA về ESP32U |
| GPIO17 / TX | RX | ESP32U gửi RTCM/lệnh vào UM980/982 |
| GND | GND | Bắt buộc chung mass |

Lưu ý:

- Mức logic UART phải tương thích 3.3 V.
- UART GNSS mặc định là `115200`, cấu hình `SERIAL_8N1`.
- RTCM là dữ liệu nhị phân, bắt buộc dùng `Serial1.write(data, length)`.
- Không ghi log/debug vào `Serial1`; mọi log chỉ ra `Serial` USB.

## Giao thức chia gói RTCM qua ESP-NOW

Đây là hợp đồng giao tiếp giữa repo Base riêng và Rover repo này.

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

1. Kiểm tra MAC nguồn, độ dài packet, `magic`, `version` và giới hạn header.
2. Copy packet vào FreeRTOS Queue.
3. Thoát callback; không ghi UART, log dài, truy cập NVS hoặc gọi `delay()`.

Task reassembly:

1. Nhóm fragment theo `streamId` và `frameSequence`.
2. Lưu fragment theo `fragmentIndex`, dùng bitmap để phát hiện fragment đã có.
3. Bỏ fragment trùng.
4. Nếu fragment không nhất quán thì bỏ toàn bộ frame.
5. Nếu queue tràn hoặc quá thời gian 500 ms, bỏ frame đang ghép.
6. Khi đủ fragment, kiểm tra `frameLength`, preamble RTCM3 và CRC24Q.
7. Chỉ khi hợp lệ mới gọi `Serial1.write(frame, frameLength)`.

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
| `include/Prog_Config.h` | GPIO UART, MAC Base, channel, MQTT tùy chọn, PMK/LMK |
| `platformio.ini` | Environment `esp32u_rover_espnow` và native protocol test |

## Build và nạp firmware Rover

### Cấu hình trước khi build

Mở `include/Prog_Config.h` và kiểm tra:

1. `ESPNOW_WIFI_CHANNEL`: Base và Rover phải giống nhau.
2. `WIFI_CONNECT_TO_ROUTER_ENABLED = false`.
3. `ROVER_MQTT_ENABLED = false`.
4. `RX_GNSS`, `TX_GNSS` và `GNSS_BAUD`.
5. Điền MAC STA của Base vào `ESPNOW_BASE_MAC`, ví dụ:

```cpp
inline constexpr uint8_t ESPNOW_BASE_MAC[6] = {
    0x24, 0x6F, 0x28, 0x12, 0x34, 0x56
};
```

Nếu `ESPNOW_BASE_MAC` vẫn là sáu byte `0x00`, firmware vẫn nạp được nhưng ESP-NOW sẽ không khởi động.

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

Nếu thấy:

```text
[ESP-NOW][ERROR] ESPNOW_BASE_MAC chua duoc provision
```

thì điền MAC Base vào `include/Prog_Config.h`, build và upload lại.

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
| ESP-NOW không Ready | Điền `ESPNOW_BASE_MAC`, kiểm tra PMK/LMK và `ESPNOW_WIFI_CHANNEL` |
| Có ESP-NOW Ready nhưng không có RTCM | Base/Rover cùng channel, Base gửi đúng MAC Rover |
| CRC error tăng | Kiểm tra protocol phía Base, `frameLength`, fragment index/count và CRC24Q |
| UM980/982 không nhận correction | Kiểm tra TX/RX nối chéo, chung GND, baud và mức logic UART |
| Serial Monitor ký tự rác | Đặt monitor baud 115200 |

## Thứ tự triển khai tiếp theo

1. [ ] Xác nhận GPIO16/17 trên PCB ESP32U thực tế.
2. [ ] Điền `ESPNOW_BASE_MAC` thật.
3. [ ] Tạo repo Base riêng và triển khai sender theo protocol trong README này.
4. [ ] Kiểm thử end-to-end Base repo riêng → ESP32U Rover → UM980/982.
5. [ ] Đo tầm xa LR 250 Kbps, sau đó thử LR 500 Kbps nếu cần.

## Kết quả kiểm tra phần mềm

- PlatformIO `esp32u_rover_espnow`: **SUCCESS**.
- RAM: 46,196 / 327,680 byte (14.1%).
- Flash: 769,137 / 1,310,720 byte (58.7%).
- Native unit test: **3/3 PASSED**.
- MAC Base đã được provision; vẫn chưa đánh dấu kiểm thử phần cứng vì PMK/LMK, PCB thực tế và log end-to-end chưa được xác nhận.
- Review Base/Rover ngày 2026-07-06 xác nhận protocol tương thích và băng thông đủ cho RTCM 1 Hz; còn cần xử lý timeout 500 ms, ACK ứng dụng, telemetry đầy đủ và test state machine trước khi coi là tối ưu cho vận hành dài hạn.
