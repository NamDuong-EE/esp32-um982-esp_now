# ESP32 GNSS Rover

Firmware Rover dùng ESP32U nhận dữ liệu hiệu chỉnh RTCM từ Base qua ESP-NOW Long Range, chuyển dữ liệu vào UM980 qua UART và gửi dữ liệu trạng thái/GNSS lên MQTT.

## Phạm vi dự án

Repository này chỉ xây dựng firmware cho **Rover**. Firmware Base không nằm trong repository, nhưng Base phải tuân thủ giao thức gói RTCM được mô tả bên dưới.

Dự án không còn sử dụng phần cứng hoặc thư viện LoRa. ESP32U sử dụng radio Wi-Fi tích hợp cho ESP-NOW và kết nối trực tiếp với UM980 bằng các chân GPIO UART.

## Trạng thái triển khai

- [x] Firmware ESP32U Rover biên dịch thành công bằng PlatformIO.
- [x] Wire protocol ESP-NOW/RTCM, chia fragment và CRC24Q đã có unit test.
- [x] Wi-Fi STA, MQTT, ESP-NOW LR, peer unicast, FreeRTOS Queue và reassembly đã được cài đặt.
- [x] Chỉ frame RTCM hoàn chỉnh, đúng CRC mới được ghi nhị phân vào UART UM980.
- [x] Health counter và xử lý reconnect Wi-Fi/refresh ESP-NOW peer đã được thêm.
- [x] Source, dependency, environment, board definition và test LoRa/Heltec đã được loại bỏ.
- [ ] Xác nhận GPIO16/17 đúng với PCB ESP32U thực tế.
- [ ] Điền MAC STA của Base vào `ESPNOW_BASE_MAC` trong `include/Prog_Config.h`.
- [ ] Provision PMK/LMK và bật `ESPNOW_ENCRYPTION_ENABLED` khi triển khai bảo mật.
- [ ] Kiểm thử end-to-end trên Base + ESP32U Rover + UM980 + MQTT.

```text
Base ── ESP-NOW Long Range ──> ESP32U Rover ── UART GPIO ──> UM980
                                      │
                                      └── Wi-Fi ──> MQTT
```

## Các quyết định kiến trúc

### ESP-NOW và MQTT

- Rover chạy `WIFI_STA`.
- Cùng interface STA được dùng cho kết nối router/MQTT và ESP-NOW.
- Wi-Fi thông thường dùng `WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N`.
- ESP-NOW giữa Base và Rover dùng thêm `WIFI_PROTOCOL_LR`.
- Cấu hình đầy đủ của STA là `11B/G/N | LR`, không dùng chế độ LR-only vì router thông thường không hỗ trợ giao thức LR độc quyền của Espressif.
- Router, Rover và Base phải dùng cùng một kênh Wi-Fi 2.4 GHz. Router phải được cấu hình kênh cố định, không để Auto Channel.
- Rover kết nối router trước, đọc channel hiện tại rồi mới khởi tạo ESP-NOW.
- PHY rate ESP-NOW mặc định là LR 250 Kbps để ưu tiên tầm xa. Có thể thử LR 500 Kbps sau khi đo thực địa.
- Trong lúc STA scan hoặc reconnect Wi-Fi, ESP-NOW có thể mất gói. Firmware phải ghi nhận lỗi và không chuyển frame RTCM thiếu dữ liệu vào UM980.

Luồng khởi tạo dự kiến:

```text
WiFi.mode(WIFI_STA)
        ↓
Kết nối router và lấy WiFi.channel()
        ↓
Bật WIFI_PROTOCOL_11B/G/N/LR trên WIFI_IF_STA
        ↓
Khởi tạo ESP-NOW và peer
        ↓
Khởi tạo queue nhận RTCM
        ↓
Khởi chạy MQTT và các task ứng dụng
```

### Peer và bảo mật

- Dữ liệu RTCM được gửi bằng unicast tới MAC cố định của Rover.
- Rover chỉ chấp nhận packet từ MAC Base đã cấu hình.
- Phiên bản đầu dùng MAC cấu hình tĩnh; chưa triển khai broadcast discovery.
- Khi bật mã hóa, Base và Rover dùng chung PMK/LMK được provision trước.
- `ESP_NOW_SEND_SUCCESS` chỉ xác nhận ở tầng MAC, không đảm bảo task ứng dụng đã xử lý packet.

### Kết nối ESP32U với UM980

- UM980 TX nối với GPIO RX UART của ESP32U.
- UM980 RX nối với GPIO TX UART của ESP32U.
- Hai thiết bị phải nối chung GND và dùng mức logic tương thích 3.3 V.
- GPIO RX/TX được khai báo trong `include/Prog_Config.h`; không phụ thuộc vào chân LoRa cũ.
- UART GNSS mặc định chạy ở 115200 baud, cấu hình `SERIAL_8N1`.
- Dữ liệu RTCM là dữ liệu nhị phân: phải dùng `Serial1.write(data, length)`, không dùng `print()`, `println()` hoặc chuỗi kết thúc bằng `\0`.
- Mọi log debug chỉ được ghi ra `Serial` USB, tuyệt đối không ghi vào `Serial1` nối với UM980.

## Giao thức chia gói RTCM qua ESP-NOW

### Nguyên tắc

- ESP-NOW v1 được chọn làm mức tương thích cơ sở, tối đa 250 byte mỗi packet.
- Một RTCM3 frame có tối đa 1023 byte payload, cộng 3 byte header và 3 byte CRC24Q, tổng cộng tối đa 1029 byte.
- Base phải tách đúng từng RTCM3 frame trước khi chia fragment. Không trộn byte của hai RTCM frame trong cùng một nhóm fragment.
- Header giao thức có kích thước 16 byte; phần dữ liệu tối đa là 234 byte.
- Một RTCM frame tối đa cần 5 fragment.
- Tất cả số nguyên nhiều byte trên wire dùng little-endian.
- `fragmentCount` phải bằng `ceil(frameLength / 234)`.
- Mọi fragment trừ fragment cuối phải có `payloadLength = 234`; fragment cuối chứa phần byte còn lại. Nhờ đó offset luôn bằng `fragmentIndex * 234`, kể cả khi packet đến sai thứ tự.

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

struct RtcmEspNowPacket {
    RtcmEspNowHeader header;
    uint8_t payload[234];
};
```

Độ dài truyền thực tế phải là:

```text
sizeof(RtcmEspNowHeader) + payloadLength
```

Không gửi toàn bộ `sizeof(RtcmEspNowPacket)` nếu fragment cuối không dùng hết payload.

### Ý nghĩa trường

| Trường | Quy tắc |
|---|---|
| `magic` | Loại bỏ packet không thuộc giao thức của dự án |
| `version` | Cho phép thay đổi protocol trong tương lai |
| `packetType` | Hiện chỉ hỗ trợ `RTCM_DATA = 1` |
| `streamId` | Phân biệt sequence mới sau khi Base reboot |
| `frameSequence` | Phát hiện frame mất, trùng hoặc đến sai thứ tự |
| `frameLength` | Cấp phát/kiểm tra vùng reassembly và xác nhận tổng độ dài |
| `fragmentIndex` | Vị trí fragment trong RTCM frame |
| `fragmentCount` | Tổng fragment của RTCM frame, tối đa 5 |
| `payloadLength` | Số byte payload thực sự có trong packet |

### Quy tắc phía Base

Phần này là hợp đồng giao tiếp; code Base không thuộc repository này.

1. Tìm preamble RTCM3 `0xD3`.
2. Đọc trường độ dài 10 bit để xác định toàn bộ RTCM frame.
3. Kiểm tra CRC24Q trước khi gửi.
4. Chia frame thành các fragment tối đa 234 byte.
5. Gửi fragment theo thứ tự tăng dần và chỉ gửi fragment kế tiếp sau send callback của fragment trước.
6. Nếu send callback báo lỗi, retry tối đa một lần khi frame còn mới; nếu vẫn lỗi thì bỏ toàn bộ frame.
7. Tăng `frameSequence` sau mỗi frame, kể cả frame bị bỏ.
8. Tạo `streamId` mới khi Base khởi động lại.

### Quy tắc phía Rover

ESP-NOW receive callback chạy trong Wi-Fi task ưu tiên cao nên chỉ thực hiện công việc ngắn:

1. Kiểm tra MAC nguồn, độ dài packet, `magic`, `version` và các giới hạn header.
2. Copy packet vào FreeRTOS Queue.
3. Thoát callback; không ghi UART, log dài, truy cập NVS hoặc gọi `delay()` trong callback.

Task reassembly xử lý packet từ queue:

1. Nhóm fragment theo `streamId` và `frameSequence`.
2. Lưu fragment theo `fragmentIndex` và dùng bitmap để nhận biết fragment đã có.
3. Bỏ qua fragment trùng.
4. Kiểm tra `fragmentCount`, `payloadLength` và offset theo `frameLength`; nếu bất kỳ fragment nào không nhất quán thì bỏ toàn bộ frame.
5. Nếu queue tràn hoặc quá thời gian 500 ms, bỏ toàn bộ frame đang ghép.
6. Khi đủ fragment, kiểm tra tổng số byte bằng `frameLength`.
7. Kiểm tra preamble, độ dài RTCM3 và CRC24Q của frame hoàn chỉnh.
8. Chỉ khi hợp lệ mới gọi `Serial1.write(frame, frameLength)`.

Không gửi ACK ứng dụng cho từng fragment trong phiên bản đầu. Unicast ESP-NOW đã có xác nhận tầng MAC; với RTK thời gian thực, ưu tiên frame mới hơn retry kéo dài một frame cũ.

### Queue và bộ nhớ

- Queue đề xuất chứa tối thiểu 16 packet để hấp thụ các burst RTCM.
- Mỗi packet trong queue phải lưu cả header, payload và độ dài nhận thực tế.
- Bộ đệm reassembly tối thiểu 1029 byte.
- Khi queue đầy, drop packet mới; frame thiếu fragment sau đó phải timeout và không được chuyển xuống UM980.
- Các counter cần theo dõi: packet nhận, packet sai nguồn, packet sai header, queue overflow, fragment trùng, frame timeout, frame mất sequence, CRC lỗi và frame đã chuyển thành công vào UM980.

## Source đã triển khai

### File ESP-NOW/RTCM mới

| Trạng thái | File | Trách nhiệm |
|---|---|---|
| ✅ | `include/protocol/RtcmEspNowProtocol.h` | Định nghĩa wire protocol, hằng số và giới hạn packet |
| ✅ | `src/protocol/RtcmEspNowProtocol.cpp` | Validate header, fragment boundary và CRC24Q |
| ✅ | `include/hardware/Espnow_handler.h` | API khởi tạo ESP-NOW và receive callback |
| ✅ | `src/hardware/Espnow_handler.cpp` | Cấu hình STA/LR, peer và đưa packet vào queue |
| ✅ | `include/functions/Rtcm_EspNow_Handler.h` | API reassembly/validate RTCM |
| ✅ | `src/functions/Rtcm_EspNow_Handler.cpp` | Ghép fragment, kiểm tra CRC24Q và ghi vào UM980 |

### File cần sửa

| Trạng thái | File | Nội dung |
|---|---|---|
| ✅ | `include/Top_Lvl_Config.h` | Chỉ giữ cấu hình Rover Wi-Fi + ESP-NOW |
| ✅ | `include/Prog_Config.h` | Thêm MAC Base, LR rate, PMK/LMK, GPIO UART và kích thước queue |
| ✅ | `include/helper.h` | Include handler ESP-NOW/RTCM mới |
| ✅ | `src/main.cpp` | Khởi tạo Wi-Fi trước ESP-NOW; chạy task nhận/reassembly RTCM |
| ✅ | `src/helper.cpp` | Health counter ESP-NOW/RTCM và bỏ nhánh LoRa/NTRIP |
| ✅ | `platformio.ini` | Environment `esp32u_rover_espnow` và native protocol test |

### File LoRa đã loại bỏ

| Trạng thái | File |
|---|---|
| ✅ Đã xóa | `include/hardware/Lora_handler.h` |
| ✅ Đã xóa | `src/hardware/Lora_handler.cpp` |
| ✅ Đã xóa | `include/functions/Nmea_Handler_LoRa.h` |
| ✅ Đã xóa | `src/functions/Nmea_Handler_LoRa.cpp` |

Thư viện Heltec LoRa, macro LoRaWAN, cấu hình RF, board variant và các bài test LoRa cũng đã được gỡ sau khi environment ESP-NOW biên dịch thành công.

## Nạp firmware Rover vào ESP32U

### 1. Chuẩn bị

- Máy tính đã cài Visual Studio Code và extension PlatformIO IDE, hoặc PlatformIO CLI.
- Cáp USB có truyền dữ liệu; một số cáp chỉ cấp nguồn sẽ không tạo cổng COM.
- Driver USB-UART phù hợp với board, thường là CP210x hoặc CH340.
- ESP32U được nối với UM980 theo bảng dưới đây. TX và RX phải nối chéo.

| ESP32U | UM980 | Ghi chú |
|---|---|---|
| GPIO16 / RX | TX | UM980 gửi NMEA về ESP32U |
| GPIO17 / TX | RX | ESP32U gửi RTCM/lệnh vào UM980 |
| GND | GND | Bắt buộc chung mass |

Chỉ nối các chân UART khi mức logic của UM980 tương thích 3.3 V. Cấp nguồn cho board UM980 đúng theo tài liệu phần cứng của board; không mặc định lấy nguồn trực tiếp từ chân 3.3 V của ESP32U.

### 2. Cấu hình trước khi build

Mở `include/Prog_Config.h` và kiểm tra:

1. `WIFI_SSID` và `WIFI_PASSWORD`.
2. Thông tin MQTT nếu Rover cần gửi dữ liệu lên broker.
3. `RX_GNSS`, `TX_GNSS` và `GNSS_BAUD` đúng với phần cứng.
4. Điền MAC STA của Base vào `ESPNOW_BASE_MAC`, ví dụ:

```cpp
inline constexpr uint8_t ESPNOW_BASE_MAC[6] = {
    0x24, 0x6F, 0x28, 0x12, 0x34, 0x56
};
```

Base phải in MAC bằng `WiFi.macAddress()` sau khi chạy `WiFi.mode(WIFI_STA)`. Cần dùng đúng MAC của interface STA mà Base dùng để gửi ESP-NOW. Nếu `ESPNOW_BASE_MAC` vẫn là sáu byte `0x00`, firmware vẫn nạp được nhưng ESP-NOW sẽ không khởi động.

Mặc định mã hóa đang tắt. Chỉ đặt `ESPNOW_ENCRYPTION_ENABLED = true` sau khi đã điền PMK và LMK 16 byte giống phía Base. Không đưa khóa thật lên repository công khai.

Router 2.4 GHz phải được khóa một channel cố định. Rover lấy channel từ router và Base phải sử dụng cùng channel đó.

### 3. Nạp bằng Visual Studio Code + PlatformIO

1. Mở đúng thư mục gốc của repository, nơi có `platformio.ini`.
2. Cắm ESP32U vào USB và xác định cổng COM trong Device Manager.
3. Ở thanh trạng thái PlatformIO, chọn environment `esp32u_rover_espnow`. Đây cũng là environment mặc định.
4. Nhấn **Build** để biên dịch trước.
5. Nhấn **Upload** để nạp firmware.
6. Nếu PlatformIO dừng ở `Connecting...`, giữ nút **BOOT**, nhấn **EN/RESET** một lần, thả **EN/RESET**, sau đó thả **BOOT** khi quá trình ghi bắt đầu.
7. Sau khi Upload báo `SUCCESS`, mở **Serial Monitor** ở 115200 baud.

Nếu máy có nhiều cổng COM, có thể thêm tạm vào environment trong `platformio.ini`:

```ini
upload_port = COM5
monitor_port = COM5
```

Thay `COM5` bằng cổng thực tế và không commit cấu hình COM cá nhân nếu repository được dùng trên nhiều máy.

### 4. Nạp bằng PlatformIO CLI

Mở PowerShell tại thư mục repository rồi chạy:

```powershell
# Build firmware
pio run -e esp32u_rover_espnow

# Upload tự động tìm cổng COM
pio run -e esp32u_rover_espnow -t upload

# Hoặc chỉ định cổng
pio run -e esp32u_rover_espnow -t upload --upload-port COM5

# Mở Serial Monitor
pio device monitor --port COM5 --baud 115200
```

Nếu lệnh `pio` chưa có trong `PATH`, dùng:

```powershell
python -m platformio run -e esp32u_rover_espnow -t upload --upload-port COM5
python -m platformio device monitor --port COM5 --baud 115200
```

Unit test giao thức RTCM có thể chạy trên máy tính, không cần kết nối ESP32:

```powershell
pio test -e native
```

### 5. Log mong đợi sau khi khởi động

Khi cấu hình đúng, Serial Monitor phải hiển thị các nhóm log tương tự:

```text
[GNSS] UART1 baud=115200 RX=16 TX=17
[WIFI] Ket noi THANH CONG!
[WIFI] Rover STA MAC: XX:XX:XX:XX:XX:XX
[WIFI] Channel: N
[ESP-NOW] Ready, STA channel=N, LR=250 Kbps
[SETUP] Khoi dong hoan tat
```

Nếu thấy:

```text
[ESP-NOW][ERROR] ESPNOW_BASE_MAC chua duoc provision
```

thì phải điền MAC Base trong `Prog_Config.h`, build và upload lại.

### 6. Kiểm tra sau khi nạp

1. Kiểm tra Base và Rover báo cùng channel Wi-Fi.
2. Kiểm tra Base đang gửi unicast tới MAC STA của Rover được in trên Serial Monitor.
3. Quan sát health payload: `rtcm_frames` phải tăng và `last_rtcm_age_ms` phải được cập nhật.
4. `rtcm_crc_errors`, `rtcm_queue_overflow` và `rtcm_sequence_gaps` lý tưởng bằng 0.
5. Kiểm tra UM980 nhận RTCM và chuyển sang trạng thái RTK Float/Fixed.
6. Kiểm tra GGA/KSXT vẫn được publish lên MQTT.

### 7. Xử lý lỗi thường gặp

| Hiện tượng | Kiểm tra |
|---|---|
| Không thấy cổng COM | Đổi cáp USB, cổng USB hoặc cài driver CP210x/CH340 |
| Upload timeout | Chọn đúng COM và thực hiện thao tác nút BOOT/EN |
| ESP-NOW không Ready | Điền MAC Base, kiểm tra PMK/LMK và Wi-Fi đã kết nối |
| Có MQTT nhưng không có RTCM | Base/Rover/router phải cùng channel; kiểm tra MAC đích phía Base |
| CRC error tăng | Kiểm tra protocol Base, `frameLength`, fragment index/count và CRC24Q |
| UM980 không nhận correction | Kiểm tra TX/RX nối chéo, chung GND, baud và mức logic UART |
| Serial Monitor ký tự rác | Đặt monitor baud 115200 |

Không nên chỉ nạp riêng `.pio/build/esp32u_rover_espnow/firmware.bin` bằng công cụ ngoài vì một ESP32 trống còn cần bootloader và partition table đúng địa chỉ. Upload bằng PlatformIO là phương án mặc định và an toàn nhất.

## Thứ tự triển khai

1. [ ] Xác nhận model ESP32U và GPIO UART16/17 trên phần cứng thực tế.
2. [x] Tạo environment PlatformIO cho ESP32U Rover và thống nhất macro board.
3. [x] Cài đặt `RtcmEspNowProtocol` cùng unit test cho header, fragment boundary và CRC24Q.
4. [x] Cài đặt Wi-Fi STA + MQTT + ESP-NOW trên cùng channel với chế độ `11B/G/N | LR`.
5. [x] Cài đặt unicast peer cố định và LR PHY rate 250 Kbps; còn chờ điền MAC Base thực tế.
6. [x] Cài đặt receive callback và FreeRTOS Queue.
7. [x] Cài đặt reassembly, timeout, sequence tracking và CRC24Q.
8. [x] Ghi RTCM hợp lệ vào `Serial1` bằng API nhị phân và mutex UART.
9. [x] Thêm health counter và log debug trên `Serial`.
10. [ ] Kiểm thử end-to-end với Base, UM980 và MQTT cùng hoạt động.
11. [x] Xóa hoàn toàn source, dependency, environment và test LoRa.

## Tiêu chí hoàn thành

- [ ] Rover nhận đúng RTCM qua ESP-NOW LR và UM980 đạt trạng thái RTK theo yêu cầu trên phần cứng.
- [ ] MQTT và ESP-NOW được xác nhận hoạt động đồng thời trên cùng STA/channel ngoài thực địa.
- [x] Code không ghi log debug vào UART UM980; lệnh UM980 và RTCM dùng chung mutex TX.
- [x] Frame thiếu fragment, sai độ dài hoặc sai CRC bị loại trước khi ghi UART.
- [x] Code xử lý `streamId` mới, timeout và refresh peer sau Wi-Fi reconnect.
- [x] Health payload có packet/frame counter, queue overflow, sequence gap, CRC error và tuổi frame RTCM gần nhất.

## Kết quả kiểm tra phần mềm

- PlatformIO `esp32u_rover_espnow`: **SUCCESS**.
- RAM: 46,236 / 327,680 byte (14.1%).
- Flash: 768,289 / 1,310,720 byte (58.6%).
- Native unit test: **3/3 PASSED**.
- Chưa đánh dấu kiểm thử phần cứng vì MAC Base, PMK/LMK và PCB thực tế chưa được cung cấp.
