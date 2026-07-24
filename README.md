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
- [x] Thêm hai operating mode build-time: Rover thường và Rover trung gian (Relay).
- [x] Relay nhận/kiểm tra RTCM từ Base, ghi UART local rồi đưa frame hoàn chỉnh vào queue downstream để chia fragment và multi-unicast tới tối đa 5 Rover con.
- [x] Thêm pairing downstream; Relay đóng vai trò Base đối với Rover con và lưu danh sách `child0`...`child4` riêng trong NVS.
- [x] Tách ACK thành hai liên kết độc lập `Base ↔ Relay` và `Relay ↔ Rover con`, có timeout/retry/health riêng.
- [x] Tách debug web Normal mode và Relay mode; Relay web hiển thị riêng upstream, downstream và relay queue.
- [ ] Provision PMK/LMK và bật `ESPNOW_ENCRYPTION_ENABLED` khi triển khai bảo mật.
- [ ] Kiểm thử end-to-end với Base repo riêng + ESP32U Rover + UM980/982.
- [ ] Kiểm thử phần cứng topology `Base → Relay Rover → Child Rover`, gồm pairing, reset nguồn, retry và mất liên kết downstream.
- [x] Telemetry ngược `Rover → Relay/Base`: đổi GGA sang ECEF, gửi 1 Hz và chỉ giữ snapshot mới nhất trong RAM.
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

### Telemetry ECEF Rover → Base trực tiếp và qua Relay

Rover parse `$GNGGA` hoặc `$GPGGA`, lấy UTC/fix quality và đổi WGS84 geodetic sang ECEF. Latitude, longitude và height chỉ dùng nội bộ; wire protocol và MQTT không truyền LLH.

```text
UM980/982 Rover ── GGA ──> ESP32 Rover
                              │ ROVER_ECEF_STATUS · type 6 · 40 byte · 1 Hz
                              ▼
ESP32 Base ── latest ECEF trong RAM

Child Rover ── ROVER_ECEF_STATUS ──> Relay
Relay ── RELAYED_ROVER_ECEF_STATUS · type 7 · 48 byte ──> Base
```

Nguyên tắc hiện tại:

1. Wire protocol là version `2`; Base, Relay và mọi Rover phải được nạp đồng bộ.
2. ECEF X/Y/Z dùng `int64_t` scale `10000`, tương ứng `0,0001 m`; log và lệnh `MODE BASE X Y Z` in bốn chữ số sau dấu phẩy.
3. Type `6` mang sequence, GNSS milliseconds-of-day, `correctionStreamId`, `fixQuality` và ECEF. Type `7` giữ nguyên dữ liệu rồi thêm MAC Rover con.
4. `correctionStreamId` là stream RTCM gần nhất Rover đã ghép/ghi hoàn tất. Base dùng stream cùng GNSS time để không cộng correction sai epoch.
5. Rover gửi tối đa một snapshot mỗi giây. Telemetry có ưu tiên thấp hơn pairing và RTCM ACK; khi TX bận có thể bỏ snapshot hiện tại, không retry backlog.
6. Relay có một slot latest-only cho mỗi child và forward round-robin. NVS chỉ lưu MAC pairing, không lưu lịch sử ECEF.
7. Khi Relay nhận command ID `4`, Relay và các child chạy `CONFIG RTK DISABLE`: ngừng nhận correction nhưng vẫn parse GGA/gửi ECEF để Base quan sát fresh `fix_quality` khác 4/5. Khi Base gửi command ID `5`, Relay fan-out `CONFIG RTK USER_DEFAULTS`; RTCM chỉ được nhận lại sau resume.

Packet v2 không tương thích firmware LLH cũ. Nạp lẫn phiên bản sẽ bị loại ở bước validate `version/length`.

Luồng khởi tạo:

```text
Đặt Wi-Fi mode cuối cùng: WIFI_STA hoặc WIFI_AP_STA
        ↓
Bật WIFI_PROTOCOL_11B/G/N/LR và channel cố định
        ↓
Khởi tạo ESP-NOW core, shared TX manager và đặt LR PHY rate
        ↓
Nạp/thêm peer Base, sau đó peer Child/broadcast nếu Relay mode
        ↓
Khởi động debug SoftAP nếu DEBUG_WEB_ENABLED=1
        ↓
Nhận fragment RTCM vào FreeRTOS Queue
        ↓
Reassembly, kiểm tra CRC24Q
        ↓
Serial1.write(frame, frameLength) vào UM980/982
```

### Chế độ Rover trung gian (Relay mode)

Relay mode được chọn lúc build bằng environment `esp32u_rover_relay`. Rover con dùng firmware Normal mode và pair với Relay. Base dành tối đa 30 slot ECEF trong RAM: 5 peer trực tiếp, mỗi peer có thể là Relay với tối đa 5 Rover con.

```text
Base đã pair
    │ RTCM_DATA / FRAME_ACK upstream
    ▼
Relay Rover
    ├── Reassembly + CRC24Q + ghi UM980/982 local
    └── Queue frame hoàn chỉnh, chia fragment và gửi downstream
                                      │
                                      ▼
                              Child Rover (Normal mode)
                              chỉ pair với Relay, không pair Base
```

Relay giữ peer upstream và danh sách peer downstream độc lập trong namespace NVS `espnow`:

- `base_mac`: peer upstream, được học khi Relay hoạt động như Rover đối với Base.
- `child_count` và `child0`...`child4`: tối đa 5 peer downstream, được học từng Rover khi Relay hoạt động như Base. Firmware tự chuyển mục `child_mac` của phiên bản cũ thành `child0` trong lần khởi động đầu tiên.

Quy tắc hoạt động:

1. Relay chỉ nhận `RTCM_DATA` runtime từ `base_mac`.
2. Frame phải được ghép đủ và đúng CRC24Q, sau đó được ghi vào UART local trước khi đưa vào relay queue.
3. Relay giữ nguyên `streamId` và `frameSequence`, chia lại frame theo payload 234 byte rồi multi-unicast tuần tự tới từng Rover con.
4. Rover con xử lý Relay như Base bình thường, ghi RTCM vào UART và gửi `FRAME_ACK` về Relay.
5. ACK upstream và downstream độc lập. Lỗi/mất Rover con không làm Base gửi lại RTCM vào UART local của Relay; lỗi downstream được Relay retry và ghi vào health counter riêng.
6. Mọi packet downstream gồm pairing control và fragment RTCM đều phải nhận ESP-NOW send callback thành công; `esp_now_send()` chỉ trả về queued không còn được xem là đã phát thành công.
7. Phiên bản hiện tại hỗ trợ tối đa 5 Rover con, queue downstream dài 3 frame và tối đa 3 lần gửi toàn frame cho từng Rover (lần đầu + 2 retry). ACK phải khớp cả MAC Rover, `streamId` và `frameSequence`.
8. Base, Relay và Rover con phải dùng cùng `ESPNOW_WIFI_CHANNEL` và cùng cấu hình LR PHY.
9. Khi mở pairing downstream, Relay dừng gửi RTCM, xóa các frame downstream cũ và chỉ dành TX cho pairing. Frame RTCM mới nhận trong cửa sổ này vẫn được ghi vào UART local nhưng không được đưa vào queue rover con.
10. Sau hai frame lỗi liên tiếp, riêng Rover con đó được đưa vào cooldown 3 giây. Relay tiếp tục gửi tới các Rover còn hoạt động để một thiết bị mất sóng không chặn toàn bộ danh sách.

Pairing dùng chung nút vật lý nhưng không mở hai state machine đồng thời:

- Relay chưa có `base_mac`: giữ nút để mở upstream pairing với Base.
- Relay đã có `base_mac`: giữ nút 1,5 giây để mở downstream pairing; Relay broadcast `PAIR_DISCOVERY`, nhận `PAIR_RESPONSE`, gửi `PAIR_CONFIRM` rồi thêm Rover vào slot còn trống. Lặp lại thao tác cho từng Rover; pair lại cùng MAC không tạo mục trùng.
- Giữ liên tục nút trên Relay trong 20 giây để xóa toàn bộ danh sách Rover con khỏi RAM, peer ESP-NOW và NVS. `base_mac` upstream không bị xóa.
- Khi pair Rover con, Base thật không cần vào pairing mode. Rover con phải mở pairing mode như một Rover thường.

Các tham số chính trong `include/Prog_Config.h`:

```cpp
ROVER_RELAY_MODE
RELAY_MAX_CHILDREN
RELAY_QUEUE_LENGTH
RELAY_ACK_TIMEOUT_MS
ESPNOW_TX_MUTEX_TIMEOUT_MS
ESPNOW_TX_CALLBACK_TIMEOUT_MS
ESPNOW_FORCE_LR_RATE
ESPNOW_USE_LR_250KBPS
RELAY_FRAME_RETRY_COUNT
RELAY_FRAGMENT_SEND_RETRY_COUNT
RELAY_FAILED_FRAME_BACKOFF_MS
RELAY_DISCOVERY_INTERVAL_MS
RELAY_CHILD_CLEAR_HOLD_MS
RELAY_CHILD_FAILURES_BEFORE_COOLDOWN
RELAY_CHILD_FAILURE_COOLDOWN_MS
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

### Nhận lệnh đổi UM980 giữa Rover và temporary Base - V1

Rover không cần kết nối MQTT. Base nhận MQTT command rồi gửi ESP-NOW unicast packet type `8` tới Rover trực tiếp đã chọn. Rover chỉ chấp nhận command semantic Fixed ECEF, trở về Rover hoặc reset RTK có đúng source MAC Base, `network_id`, COM2, tham số hợp lệ và `auth_tag`; firmware không chạy chuỗi lệnh UART tùy ý từ mạng.

Khi nhận `switch_to_base_fixed_ecef`, task `GNSS Command` khóa `gnssTxMutex` và ghi 14 bước xuống `Serial1`/UM980 COM2:

```text
1  unlogall
2  mode base <X_m> <Y_m> <Z_m>
3  gpgga com2 1
4  rtcm1006 com2 1
5  rtcm1033 com2 1
6  rtcm1074 com2 1
7  rtcm1124 com2 1
8  rtcm1084 com2 1
9  rtcm1094 com2 1
10 rtcm1042 com2 1
11 rtcm1019 com2 1
12 rtcm1020 com2 1
13 rtcm1045 com2 1
14 saveconfig
```

Sau mỗi bước Rover cập nhật `completed_step`; sau bước cuối gửi packet result type `9` về Base. Request lặp với cùng source MAC, transaction ID và command ID chỉ gửi lại result gần nhất, không chạy lại chuỗi. Nếu chuyển sang Base thành công, Rover ngừng nhận RTCM correction và ngừng gửi ECEF kiểu Rover; COM2 chuyển từ parser NMEA sang parser RTCM uplink cho tới khi nhận `switch_to_rover` thành công hoặc reset.

Action `switch_to_base_fixed_ecef` dùng command ID `3`. Base chỉ phát lệnh sau khi ECEF mới của Rover đạt `fix_quality=4`, rồi gửi nguyên X/Y/Z fixed-point scale `10000` trong request type `8` dài 44 byte. Rover validate biên ECEF/auth rồi ghi chuỗi 14 bước như trên.

X/Y/Z được in bốn chữ số thập phân. UM980 nhận biết ECEF khi tham số đầu nằm ngoài miền latitude. Firmware không còn command ID hoặc đường thực thi Base theo thời gian; Base vẫn yêu cầu fixed guard 3000 ms và hai chu kỳ RTCM đầy đủ trước khi chuyển nguồn.

Tham chiếu cú pháp: [Unicore N4 High Precision Commands Manual](https://en.unicore.com/uploads/file/Unicore%20Reference%20Commands%20Manual%20For%20N4%20High%20Precision%20Products_V2_EN_R1.6.pdf).

Temporary Base không phát RTCM trực tiếp tới Rover khác. Firmware tách RTCM3 từ luồng COM2, kiểm preamble/length/CRC24Q rồi đưa frame vào queue 3 phần tử ưu tiên dữ liệu mới. Mỗi frame được chia payload tối đa 234 byte và unicast về đúng `base_mac` đã pair bằng packet type `10 = TEMP_RTCM_DATA`; Base trả type `11 = TEMP_RTCM_ACK` theo `streamId + frameSequence`. Uplink retry fragment tối đa 3 attempt và retry nguyên frame một lần. ACK hop này độc lập với ACK correction thông thường từ Rover về Base.

Khi Base gốc đang ở `TEMP_PREPARING`, correction local vẫn phục vụ các Rover khác. Sau fixed guard 3000 ms và hai chu kỳ đủ `1006/1074/1084/1094/1124`, Base vào `TEMP_RESETTING`: dừng RTCM cũ và gửi command ID `4`. Rover chạy `config rtk disable`, xóa snapshot GGA/ECEF cũ, giữ parser GGA/ECEF hoạt động nhưng chặn ghi RTCM xuống COM2; chỉ fresh status non-RTK sau ACK mới mở gate. Khi cohort hoàn tất, command ID `5` chạy `config rtk user_defaults`; Base chỉ bật RTCM của nhánh sau ACK resume rồi vào `TEMP_ACTIVE`.

Trong Relay mode, cả command DISABLE và RESUME được fan-out tới toàn bộ child đã pair. Relay chỉ xác nhận gửi request thành công; Base vẫn đợi status riêng của từng MAC con để phát hiện child không rời trạng thái RTK hoặc mất nguồn.

Action `switch_to_rover` ghi 4 bước để hoàn nguyên:

```text
1 unlogall
2 mode rover survey
3 gpgga com2 1
4 saveconfig
```

Sau khi ghi thành công, Rover hạ cờ `promoted_to_base`, mở lại parser NMEA/ECEF và trả result `4/4`; Base nhận result rồi bật lại gửi RTCM. Cú pháp này yêu cầu UM980 Build7923+ hoặc UM982 Build7650+ theo Commands Manual N4 của Unicore.

Giới hạn V1: `uart_sequence_written` chỉ xác nhận ESP32 đã ghi đủ byte xuống UART, chưa parse phản hồi `OK/ERROR` của UM980. Việc xác nhận temp source sẵn sàng do Base gốc thực hiện từ tập RTCM nhận được. Trạng thái đổi vai trò chỉ nằm trong RAM; reset ESP32 sẽ trở về Rover. Firmware không dùng `FRESET`.

### Debug web SoftAP tùy chọn

Debug web được điều khiển trong `include/Prog_Config.h` và hiện đang tắt mặc định:

```cpp
#define DEBUG_WEB_ENABLED 0
```

Khi bật, Rover chạy `WIFI_AP_STA`. Hai operating mode dùng hai giao diện và API riêng nhưng vẫn dùng chung một radio, một SoftAP và IP `192.168.4.1`; firmware không mở hai web server đồng thời.

Normal mode:

```text
SSID: ESP32-Rover-Debug
Password: 123456789
URL: http://192.168.4.1
API: http://192.168.4.1/api/status
```

Relay mode:

```text
SSID: ESP32-Rover-Relay
Password: 123456789
URL: http://192.168.4.1
API: http://192.168.4.1/api/relay/status
```

SoftAP dùng cùng `ESPNOW_WIFI_CHANNEL` với ESP-NOW để tránh đổi channel radio. Cả hai trang poll JSON mỗi 1 giây và lấy dữ liệu GNSS local từ câu GGA:

SoftAP luôn được khởi động sau khi Wi-Fi mode/channel, ESP-NOW LR rate và các peer đã được cấu hình. Đây là cùng thứ tự dùng ở luồng pairing ổn định ban đầu; SoftAP không chen giữa bước khởi tạo core và bước đăng ký peer.

- `lat`, `lon`, `height_m`, `ellipsoid_height_m`
- `fix_quality` là số GGA fix quality, ví dụ `4` hoặc `5`
- `satellites`
- `last_gga_age_ms`

Khi dùng ứng dụng Android tại `android-debug-viewer`, không cần bật SoftAP. Firmware mặc định phát dòng `[DEBUG_STATUS]` qua USB serial mỗi giây với cùng dữ liệu GNSS/RTCM của Web Debug; `[HEALTH]` chi tiết vẫn giữ chu kỳ 30 giây.

Có thể tắt riêng `[DEBUG_STATUS]` bằng build flag `-DSERIAL_DEBUG_STATUS_ENABLED=0` mà không tắt `[HEALTH]`. Đặt lại `1` để bật. Khi khởi động firmware sẽ log `DEBUG_STATUS enabled=yes/no`.

Normal web hiển thị health Rover hiện tại: ESP-NOW ready, Base pairing, RTCM frame, CRC, queue, sequence gap, ACK và free heap.

Relay web tách health của hai liên kết thành:

- Upstream: Base MAC/pairing, frame nhận, CRC, queue, ACK gửi Base và tuổi RTCM cuối.
- Downstream: Child MAC/pairing, frame queued/sent/ACKed, fragment, retry, ACK timeout, send failure và frame bị bỏ khi chưa có child.

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
| `include/hardware/Espnow_tx_manager.h` | API gửi ESP-NOW tuần tự và kết quả TX chi tiết |
| `src/hardware/Espnow_tx_manager.cpp` | Một mutex/send callback dùng chung cho ACK, pairing và Relay fragment |
| `include/hardware/Relay_handler.h` | API và health model của downstream Relay |
| `src/hardware/Relay_handler.cpp` | Pair/lưu child, relay queue, chia fragment, gửi/retry và nhận ACK downstream |
| `include/functions/Rtcm_EspNow_Handler.h` | API reassembly/validate RTCM |
| `src/functions/Rtcm_EspNow_Handler.cpp` | Ghép fragment, kiểm tra CRC24Q và ghi vào UM980/982 |
| `include/functions/Gnss_Command_Handler.h` | API queue, task và health của remote GNSS command |
| `src/functions/Gnss_Command_Handler.cpp` | Validate request, ghi chuỗi COM2 và gửi result về Base |
| `src/main.cpp` | Entry point firmware Rover |
| `src/helper.cpp` | Parse NMEA, health counter, log Serial |
| `include/hardware/DebugWeb_handler.h` | API debug web SoftAP tùy chọn |
| `src/hardware/DebugWeb_handler.cpp` | Chọn Normal web hoặc Relay web tại `192.168.4.1` theo operating mode |
| `include/Prog_Config.h` | GPIO UART, operating mode, channel, pairing, relay, MQTT và PMK/LMK |
| `platformio.ini` | Environment Normal, Relay và native protocol test |

## Build và nạp firmware Rover

### Cấu hình trước khi build

Mở `include/Prog_Config.h` và kiểm tra:

1. `ESPNOW_WIFI_CHANNEL`: Base và Rover phải giống nhau.
2. `WIFI_CONNECT_TO_ROUTER_ENABLED = false`.
3. `ROVER_MQTT_ENABLED = false`.
4. `RX_GNSS`, `TX_GNSS` và `GNSS_BAUD`.
5. MAC Base không còn cấu hình hard-code. Rover đọc MAC Base từ NVS/Preferences sau pairing. Nếu chưa pair, ESP-NOW vẫn khởi động để chờ pairing nhưng RTCM runtime unicast chưa hoạt động.
6. Chọn `esp32u_rover_espnow` cho Rover thường/Rover con hoặc `esp32u_rover_relay` cho Rover trung gian.

### PlatformIO CLI

```powershell
# Build Rover
pio run -e esp32u_rover_espnow

# Build Rover trung gian
pio run -e esp32u_rover_relay

# Upload tự động tìm cổng COM
pio run -e esp32u_rover_espnow -t upload

# Upload firmware Relay
pio run -e esp32u_rover_relay -t upload

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
3. Chọn environment `esp32u_rover_espnow` cho Normal mode hoặc `esp32u_rover_relay` cho Relay mode.
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
[ESP-NOW] Ready, STA channel=6, TX rate=default, peer=<none>
[MQTT] Da tat theo cau hinh ROVER_MQTT_ENABLED=false
[SETUP] Khoi dong hoan tat
```

Nếu `DEBUG_WEB_ENABLED=1`, log sẽ có thêm:

```text
[WIFI] Debug web bat; Wi-Fi mode AP+STA
[ESP-NOW] Reuse preconfigured Wi-Fi radio, channel=6
[ESP-NOW][TX] Shared TX manager ready
[ESP-NOW] Core ready, TX rate=default
[DEBUG_WEB] mode=normal SSID=ESP32-Rover-Debug IP=192.168.4.1 channel=6 MAC=XX:XX:XX:XX:XX:XX
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

Relay mode sau khi đã pair Base:

```text
[RELAY] No stored child MAC; hold pairing button after Base pairing
[RELAY] Downstream relay ready
[DEBUG_WEB] mode=relay SSID=ESP32-Rover-Relay IP=192.168.4.1 channel=6 MAC=XX:XX:XX:XX:XX:XX
[RELAY][PAIR] Child pairing mode ON for 60000 ms
[RELAY][PAIR] Child pairing mode OFF: paired
[RELAY][PAIR] Paired child XX:XX:XX:XX:XX:XX
```

## Kiểm tra sau khi nạp

1. Base repo riêng và Rover phải cùng `ESPNOW_WIFI_CHANNEL`.
2. Base phải gửi unicast tới MAC STA của Rover được in ở log `Local STA MAC`.
3. Rover health: `rtcm_frames` phải tăng và `last_rtcm_age_ms` phải được cập nhật.
4. `rtcm_crc_errors`, `rtcm_queue_overflow` và `rtcm_sequence_gaps` lý tưởng bằng 0.
5. UM980/982 Rover phải nhận RTCM và chuyển sang RTK Float/Fixed.

Kiểm tra thêm với Relay mode:

1. Pair Base với Relay trước; xác nhận Relay đã có `base_mac`.
2. Không mở pairing mode trên Base; lần lượt mở pairing trên Relay và từng Rover con. Log `Child ... index=... count=...` phải tăng đến tối đa 5 và pair lại cùng MAC không tăng `count`.
3. Relay debug JSON phải có `child_count` đúng và mảng `children` chứa MAC/counter riêng của từng Rover.
4. `frames_received`, `frames_queued` và `frames_acked` phải tăng; `ack_timeouts`, `queue_overflow` và `send_failures` lý tưởng bằng 0.
5. Tắt một Rover con: các Rover còn lại vẫn phải nhận correction; counter `frames_skipped_cooldown` chỉ tăng cho Rover lỗi.
6. Giữ nút Relay liên tục 20 giây: log phải báo `[RELAY][CHILD_CLEAR]`, `child_count=0` sau đó restart vẫn không nạp lại child cũ.
7. Cả UM980/982 local của Relay và UM980/982 của các Rover con phải nhận correction.

## Xử lý lỗi thường gặp

| Hiện tượng | Kiểm tra |
|---|---|
| Không thấy cổng COM | Đổi cáp USB, cổng USB hoặc cài driver CP210x/CH340 |
| Upload timeout | Chọn đúng COM và dùng nút BOOT/EN |
| ESP-NOW không Ready | Kiểm tra PMK/LMK, `ESPNOW_WIFI_CHANNEL` và log pairing |
| Có ESP-NOW Ready nhưng không có RTCM | Pair Base/Rover trước, kiểm tra cùng channel và Base gửi tới MAC Rover đã lưu |
| Relay nhận RTCM nhưng Rover con không nhận | Kiểm tra `relay_child_count`, mảng `children`, relay queue, ACK timeout/cooldown và bảo đảm Rover con pair với Relay chứ không pair Base |
| Nhấn nút trên Relay nhưng mở upstream pairing | Relay chưa có `base_mac`; phải pair Relay với Base trước rồi mới pair child |
| Relay queue overflow | Kiểm tra ACK từ Child, giảm retry hoặc tốc độ RTCM, và xác nhận ba thiết bị cùng channel/LR rate |
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
12. [x] Nâng runtime protocol lên v2 cho telemetry ECEF và command reset đồng bộ.
12. [ ] Provision PMK/LMK và bật `ESPNOW_ENCRYPTION_ENABLED` khi triển khai bảo mật.
13. [ ] Kiểm thử end-to-end Base repo riêng → ESP32U Rover → UM980/982.
14. [ ] Đo tầm xa LR 250 Kbps, sau đó thử LR 500 Kbps nếu cần.
15. [x] Thêm build environment `esp32u_rover_relay` và operating mode compile-time.
16. [x] Thêm NVS danh sách tối đa 5 Rover con, downstream pairing, giữ nút 20 giây để xóa toàn bộ, relay queue, multi-unicast, retry/cooldown và ACK độc lập theo MAC.
17. [x] Thêm Normal debug web và Relay debug web/API riêng.
18. [ ] Kiểm thử phần cứng `Base → Relay → 5 Child`, bao gồm reset nguồn và tự nạp lại cả `base_mac`/danh sách child.
19. [ ] Kiểm thử mất nguồn/mất sóng lần lượt từng Rover con để xác nhận upstream và các child còn lại vẫn hoạt động, cooldown/health downstream báo đúng.
20. [x] Triển khai V1 nhận lệnh ESP-NOW từ Base để cấu hình UM980 thành Temporary Base Fixed ECEF qua COM2 và trả application result.
21. [x] Thêm action `switch_to_rover` với chuỗi 4 lệnh COM2 và đồng bộ bật lại RTCM/ECEF sau application result.
22. [x] Triển khai Temporary Base parser RTCM trên COM2, queue/retry/ACK uplink một hop về Base gốc; không phát correction trực tiếp tới Rover khác.
23. [x] Bổ sung ellipsoidal height từ GGA và command ID `3` để cấu hình Temporary Base bằng ECEF RTK Fixed.
24. [x] Telemetry type `6/7` chỉ dùng ECEF scale `10000`, GNSS time và correction stream ID.
25. [x] Command ID `4` giữ RTK bằng `CONFIG RTK DISABLE`; command ID `5` resume bằng `CONFIG RTK USER_DEFAULTS`. Relay fan-out cả hai lệnh tới mọi child đã pair.

## Kết quả kiểm tra phần mềm gần nhất

### Cập nhật 2026-07-22

- Đã triển khai `TemporaryBaseUplink`: khi `promoted_to_base`, COM2 được parse RTCM3 nhị phân, CRC đúng mới vào queue; packet type `10/11`, stream/sequence, fragment retry và application ACK độc lập với correction downstream.
- Đã nối cờ vai trò với uplink: chuyển sang Base tạo stream mới và dừng LLH/RTCM input vai trò Rover; `switch_to_rover` tắt uplink, xóa queue/parser và mở lại NMEA/LLH.
- Health Serial thêm `[TEMP_BASE][UPLINK][HEALTH]` với byte UART, frame parse/CRC, queue overflow, sent/drop, fragment failure, retry và ACK timeout.
- Build xác nhận: Normal SUCCESS, RAM 47.712 byte (14,6%), Flash 797.653 byte (60,9%); Relay SUCCESS, RAM 48.360 byte (14,8%), Flash 819.117 byte (62,5%); native protocol test **8/8 PASSED**. Chưa kiểm thử handover trên phần cứng trong lượt này.
- Packet LLH type `6` đã thêm `fixQuality` từ GGA và tăng từ 20 lên 21 byte; packet Relay type `7` giữ 28 byte bằng cách dùng một byte reserved. Relay chuyển tiếp nguyên trạng và Base publish `fix_quality` dạng số lên MQTT.
- Mốc build trước khi thêm uplink: Normal SUCCESS, RAM 46.592 byte (14,2%), Flash 795.173 byte (60,7%); Relay SUCCESS, RAM 47.240 byte (14,4%), Flash 816.541 byte (62,3%); native protocol test **8/8 PASSED**.

### Cập nhật 2026-07-23

- Parser GGA đã đọc geoid separation và tạo `ellipsoid_height_m = height_m + geoid_separation_m`; LLH type `6` tăng lên 25 byte và Relay type `7` tăng lên 32 byte.
- Command request type `8` dài 44 byte, dùng command ID `3` và ECEF X/Y/Z dạng `int64` millimetre. Rover ghi `mode base X Y Z` lên COM2 rồi bật GGA/RTCM và `saveconfig`.
- Đã loại bỏ hoàn toàn command Base theo thời gian, trường duration và lệnh `MODE BASE TIME`; allowlist runtime chỉ còn Fixed ECEF và trở về Rover. Packet cũ bị từ chối.
- Build ECEF-only Normal SUCCESS: RAM 47.736 byte (14,6%), Flash 798.553 byte (60,9%). Build Relay SUCCESS: RAM 48.408 byte (14,8%), Flash 820.053 byte (62,6%). Native protocol test **8/8 PASSED**.
- Chưa kiểm thử lệnh fixed ECEF và handover trên UM980 thật; cần nạp đồng bộ Base, Relay và toàn bộ Rover vì wire packet đã đổi kích thước.

### Cập nhật 2026-07-24

- [x] Nâng protocol lên version `2`: `ROVER_ECEF_STATUS` dài 40 byte, `RELAYED_ROVER_ECEF_STATUS` dài 48 byte, ECEF `int64_t` scale `10000`.
- [x] GGA parser thêm GNSS milliseconds-of-day; ECEF dùng WGS84 và ellipsoidal height, lượng tử ở `0,0001 m`.
- [x] Telemetry gửi `correctionStreamId` để Base chỉ hiệu chỉnh Rover đang dùng đúng RTCM stream/epoch.
- [x] Đổi command ID `4` sang `config rtk disable`, xóa snapshot cũ và đặt `rtk_correction_held=1`; Rover vẫn parse GGA và gửi ECEF non-RTK nhưng từ chối ghi RTCM cũ xuống COM2.
- [x] Thêm command ID `5` chạy `config rtk user_defaults`, bỏ hold và chỉ nhận RTCM sau khi Base nhận application result.
- [x] Thêm `rtk_correction_held` vào Serial health và web debug Normal/Relay để quan sát trực tiếp hai pha DISABLE/RESUME.
- [x] Relay fan-out cả DISABLE/RESUME có auth tới tối đa 5 child; Base đợi fresh non-RTK status riêng từng child trước handover.
- [x] Build Normal SUCCESS: RAM 47.788/327.680 byte (14,6%), Flash 806.025/1.310.720 byte (61,5%).
- [x] Build Relay SUCCESS: RAM 48.532/327.680 byte (14,8%), Flash 828.989/1.310.720 byte (63,2%).
- [x] Native protocol test 8/8 PASSED.
- [ ] Chưa kiểm thử handover/reset cohort và correction ECEF end-to-end trên UM980 thật.

### Cập nhật 2026-07-21

- Đã thêm packet command/result type `8/9` dùng chung với Base, có `network_id`, transaction ID và pairing auth tag.
- Đã thêm queue/task `GNSS Command`, chuỗi 14 lệnh COM2, khóa UART dùng chung, deduplicate transaction gần nhất và health log `[ROVER][GNSS_CMD][HEALTH]`.
- Sau khi ghi chuỗi thành công, firmware tạm dừng RTCM input/LLH output của vai trò Rover trong RAM. Chưa xác nhận trên phần cứng UM980 và chưa biến ESP32 thành Base runtime hoàn chỉnh.
- Build xác nhận sau khi thêm hai chiều: Normal `esp32u_rover_espnow` SUCCESS, RAM 46.592/327.680 byte (14,2%), Flash 794.905/1.310.720 byte (60,6%); Relay `esp32u_rover_relay` SUCCESS, RAM 47.232 byte (14,4%), Flash 816.241 byte (62,3%). Native protocol test **8/8 PASSED**, gồm cả command ID về Rover, tham số và auth tamper.
- Đã thêm command ID `2` để chuyển temporary Base về `mode rover survey`, bật lại nhận RTCM/gửi LLH trong RAM và trả result 4 bước về Base.

- Lần kiểm tra phần mềm gần nhất: 2026-07-23, sau khi thêm Temporary Base fixed ECEF.
- PlatformIO Core: **6.1.19** tại `C:\Users\admin\.platformio\penv\Scripts\pio.exe`.
- PlatformIO `esp32u_rover_espnow`: **SUCCESS**, RAM 47,736/327,680 byte (14.6%), Flash 798,553/1,310,720 byte (60.9%) ở Normal mode.
- PlatformIO `esp32u_rover_relay`: **SUCCESS**, RAM 48,408/327,680 byte (14.8%), Flash 820,053/1,310,720 byte (62.6%) ở Relay mode.
- Native unit test: **8/8 PASSED**.
- MAC Base hard-code đã được loại bỏ; cần pair Base/Rover để có MAC trong NVS trước khi chạy RTCM runtime.
- Tối ưu ngày 2026-07-06 đã đồng bộ timeout 1500 ms với deadline Base, thêm ACK ứng dụng/chống ghi trùng, UART TX buffer 2048 byte và telemetry đầy đủ. Vẫn cần test state machine và kiểm thử RTK end-to-end trên phần cứng.
- Rà soát ngày 2026-07-07: `pio` chưa có trong PATH của shell hiện tại, nhưng chạy trực tiếp bằng đường dẫn trong `.platformio\penv\Scripts` thành công.
- Debug web đã được build thử với `DEBUG_WEB_ENABLED=1`: **SUCCESS**, RAM 46,672 byte (14.2%), Flash 809,057 byte (61.7%).
- Đã set cứng công suất phát WiFi/ESP-NOW của Rover bằng `WiFi.setTxPower(WIFI_POWER_19_5dBm)` ngay sau khi bật STA/AP+STA radio. Firmware đọc lại `esp_wifi_get_max_tx_power()` và in log `[WIFI] TX power fixed raw=... dBm=...` để xác nhận runtime.
- Đã build xác nhận sau khi set TX power 19.5 dBm: `esp32u_rover_espnow` SUCCESS, RAM 46,696/327,680 byte (14.3%), Flash 810,205/1,310,720 byte (61.8%).
- Đã xóa hiển thị và thu thập RSSI ESP-NOW ngày 2026-07-15: Normal/Relay web và JSON API không còn trường RSSI; promiscuous monitor cùng các trường stats liên quan cũng được loại bỏ.
- Đã triển khai Rover-side pairing động ngày 2026-07-08: packet pairing riêng, auth tag nhẹ bằng `ESPNOW_PAIRING_KEY`, nút pairing, NVS lưu MAC Base và runtime ACK/RTCM dùng MAC active. Native unit test tăng lên **5/5 PASSED**.
- Đã build xác nhận sau pairing Rover-side: `esp32u_rover_espnow` SUCCESS, RAM 46,896/327,680 byte (14.3%), Flash 820,241/1,310,720 byte (62.6%).
- Đã đồng bộ với Base-side pairing đã triển khai: Base broadcast `PAIR_DISCOVERY`, Rover trả lời `PAIR_RESPONSE`, nhận `PAIR_CONFIRM` rồi lưu MAC Base. Build Rover `esp32u_rover_espnow` vẫn **SUCCESS** RAM 46,896/327,680 byte (14.3%), Flash 820,241/1,310,720 byte (62.6%); native protocol test **5/5 PASSED**.
- Đã xóa MAC Base hard-code khỏi Rover ngày 2026-07-08: không còn `ESPNOW_BASE_MAC`, không còn fallback MAC tĩnh; Rover chờ pairing nếu NVS chưa có MAC Base. Web debug cũng bỏ `base_mac_stored` khỏi bảng hiển thị vì `base_provisioned` hiện chỉ đúng khi đã có MAC từ NVS. Build Rover `esp32u_rover_espnow` **SUCCESS** RAM 46,896/327,680 byte (14.3%), Flash 820,073/1,310,720 byte (62.6%); native protocol test **5/5 PASSED**.
- Triển khai Relay mode ngày 2026-07-14: Base protocol v1 không đổi; Relay lưu riêng `base_mac`/danh sách child, chỉ forward frame sau reassembly/CRC/UART local, giữ `streamId + frameSequence`, có queue downstream, retry và ACK riêng. Normal/Relay debug web đã tách theo build environment. Hai firmware build SUCCESS và native protocol test **5/5 PASSED**; topology phần cứng chưa được xác nhận đầy đủ.
- Mở rộng multi-child ngày 2026-07-17: một Relay lưu tối đa 5 Rover con trong NVS, tự migrate `child_mac` cũ, multi-unicast RTCM với ACK khớp MAC và cooldown từng child, chuyển tiếp LLH latest-only round-robin, xuất health theo mảng `children`; giữ nút 20 giây xóa toàn bộ child nhưng giữ nguyên Base upstream. Base tăng sức chứa LLH trong RAM lên tối đa 30 nguồn.
- Sau log phần cứng cho thấy Rover con có `pair_discovery_rx=0`, Relay sender được đổi sang send callback đồng bộ như Base: broadcast discovery, confirm và mọi fragment đều chờ kết quả radio, dùng mutex và retry. Log pairing mới phân biệt `PAIR_DISCOVERY radio TX confirmed` với send callback failure.
- Khắc phục xung đột TX ngày 2026-07-15: mọi `esp_now_send()` của ACK upstream, pairing và fragment downstream đi qua một TX manager/mutex/send callback duy nhất; ACK Base được gửi trước khi queue frame cho Child. Relay có backoff 1000 ms sau frame lỗi và tách counter `send_immediate_errors`, `send_callback_timeouts`, `send_delivery_failures`, `backoff_events`.
- Ổn định lại boot/pairing ngày 2026-07-15: cấu hình radio → `esp_now_init()`/TX manager/LR rate → nạp Base peer → nạp Child/broadcast peer → khởi động SoftAP tùy chọn → tạo task. SoftAP trở lại bước cuối như luồng pairing ổn định ban đầu; pairing downstream tạm dừng toàn bộ TX RTCM và xóa queue cũ để discovery không tranh radio với retry. Mặc định `ROVER_RELAY_MODE_ENABLED=0`; chỉ environment `esp32u_rover_relay` ép Relay mode.
