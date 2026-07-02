# ESP32 GNSS Rover

Firmware Rover dùng ESP32U nhận dữ liệu hiệu chỉnh RTCM từ Base qua ESP-NOW Long Range, chuyển dữ liệu vào UM980 qua UART và gửi dữ liệu trạng thái/GNSS lên MQTT.

## Phạm vi dự án

Repository này chỉ xây dựng firmware cho **Rover**. Firmware Base không nằm trong repository, nhưng Base phải tuân thủ giao thức gói RTCM được mô tả bên dưới.

Dự án không còn sử dụng phần cứng hoặc thư viện LoRa. ESP32U sử dụng radio Wi-Fi tích hợp cho ESP-NOW và kết nối trực tiếp với UM980 bằng các chân GPIO UART.

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
    uint16_t magic;            // 0x5254 ("RT")
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

## Thay đổi source dự kiến

### File ESP-NOW/RTCM mới

| File | Trách nhiệm |
|---|---|
| `include/protocol/RtcmEspNowProtocol.h` | Định nghĩa wire protocol, hằng số và giới hạn packet |
| `include/hardware/Espnow_handler.h` | API khởi tạo ESP-NOW và receive callback |
| `src/hardware/Espnow_handler.cpp` | Cấu hình STA/LR, peer và đưa packet vào queue |
| `include/functions/Rtcm_EspNow_Handler.h` | API reassembly/validate RTCM |
| `src/functions/Rtcm_EspNow_Handler.cpp` | Ghép fragment, kiểm tra CRC24Q và ghi vào UM980 |

### File cần sửa

| File | Nội dung |
|---|---|
| `include/Top_Lvl_Config.h` | Thay lựa chọn LoRa bằng `ESP_NOW`; sửa thống nhất macro board |
| `include/Prog_Config.h` | Thêm MAC Base, channel, LR rate, PMK/LMK, GPIO UART và kích thước queue |
| `include/helper.h` | Include handler ESP-NOW/RTCM mới |
| `src/main.cpp` | Khởi tạo Wi-Fi trước ESP-NOW; thay task LoRa bằng task nhận/reassembly RTCM |
| `src/helper.cpp` | Cập nhật health counter và bỏ mọi nhánh LoRa |
| `platformio.ini` | Tạo environment ESP32U Rover, bỏ environment/test/dependency LoRa |

### File LoRa cần loại bỏ

| File |
|---|
| `include/hardware/Lora_handler.h` |
| `src/hardware/Lora_handler.cpp` |
| `include/functions/Nmea_Handler_LoRa.h` |
| `src/functions/Nmea_Handler_LoRa.cpp` |

Thư viện Heltec LoRa, macro LoRaWAN, cấu hình RF LoRa và các bài test LoRa cũng phải được gỡ khỏi build sau khi environment ESP-NOW biên dịch thành công.

## Thứ tự triển khai

1. Xác nhận model ESP32U và chọn GPIO UART không xung đột boot/flash.
2. Tạo environment PlatformIO cho ESP32U Rover và sửa macro board hiện đang không thống nhất.
3. Cài đặt `RtcmEspNowProtocol` cùng unit test cho kích thước header và fragment boundary.
4. Cài đặt Wi-Fi STA + MQTT + ESP-NOW trên cùng channel với chế độ `11B/G/N | LR`.
5. Cài đặt unicast peer cố định và LR PHY rate 250 Kbps.
6. Cài đặt receive callback và FreeRTOS Queue.
7. Cài đặt reassembly, timeout, sequence tracking và CRC24Q.
8. Ghi RTCM hợp lệ vào `Serial1` bằng API nhị phân.
9. Thêm health counter và log debug trên `Serial`.
10. Kiểm thử end-to-end với Base, UM980 và MQTT cùng hoạt động.
11. Xóa hoàn toàn source, dependency, environment và test LoRa.

## Tiêu chí hoàn thành

- Rover nhận đúng RTCM qua ESP-NOW LR và UM980 đạt trạng thái RTK theo yêu cầu.
- MQTT và ESP-NOW hoạt động đồng thời trên cùng STA/channel.
- Không có log hoặc byte ngoài RTCM được ghi vào UART của UM980.
- Frame thiếu fragment, sai độ dài hoặc sai CRC không được chuyển xuống UM980.
- Rover phục hồi được sau khi Base reboot (`streamId` đổi), Wi-Fi reconnect hoặc ESP-NOW tạm mất kết nối.
- Có thống kê packet loss, queue overflow, CRC error và tuổi frame gần nhất để chẩn đoán ngoài thực địa.
