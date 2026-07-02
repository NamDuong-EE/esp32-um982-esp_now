# ESP32 GNSS Rover

## Tình trạng

Cập nhật: để Spreading factor >= 10 (phải giống nhau ở cả đầu thu và phát), dùng Lora 4.2 để phát, Lora 4.3 để tận dụng RX LNA khi thu. Để công suất phát 24 dBm.

Tầm phát thành công hiện tại: 2m - vẫn cần kéo dài thêm (vì LoRa không thể chỉ ngắn như thế)

---

## Kế hoạch: Thay thế LoRa bằng ESP-NOW

### Lý do thay đổi
- ESP-NOW hỗ trợ tối đa **250 bytes/gói tin** (tương đương LoRa 255 bytes), đủ để truyền các thông điệp RTCM đơn lẻ.
- Throughput cao hơn nhiều so với LoRa (hàng trăm KB/s so với vài KB/s), truyền dữ liệu RTK nhanh hơn.
- Không cần phần cứng LoRa chuyên dụng (chip SX1262), chỉ cần module ESP32 WiFi tiêu chuẩn.
- Sử dụng chế độ **Long Range (LR)** của ESP-NOW để tăng tầm phủ sóng.

### Cấu hình đã chọn
- **Chế độ Long Range**: Bật (`WIFI_PROTOCOL_LR`)
- **Board phần cứng**: ESP32 DevKit (giữ nguyên như dự án hiện tại)
- **MAC Address**: Chưa quyết định (tạm dùng broadcast, sẽ cấu hình sau)

### Các file cần xoá (LoRa cũ)

| File | Lý do |
|------|-------|
| `include/hardware/Lora_handler.h` | Thay bằng `Espnow_handler.h` |
| `src/hardware/Lora_handler.cpp` | Thay bằng `Espnow_handler.cpp` |
| `include/functions/Nmea_Handler_LoRa.h` | Thay bằng `Nmea_Handler_EspNow.h` |
| `src/functions/Nmea_Handler_LoRa.cpp` | Thay bằng `Nmea_Handler_EspNow.cpp` |

### Các file mới (ESP-NOW)

| File | Mô tả |
|------|-------|
| `include/hardware/Espnow_handler.h` | Header cho ESP-NOW hardware handler |
| `src/hardware/Espnow_handler.cpp` | Triển khai ESP-NOW setup + receive (dùng FreeRTOS Queue) |
| `include/functions/Nmea_Handler_EspNow.h` | Header cho hàm đẩy dữ liệu RTK vào chip GNSS |
| `src/functions/Nmea_Handler_EspNow.cpp` | Triển khai ghi dữ liệu RTK từ ESP-NOW vào Serial1 |

### Các file cần sửa đổi

| File | Nội dung thay đổi |
|------|-------------------|
| `include/Top_Lvl_Config.h` | Đổi `LORA_SERIAL` thành `ESP_NOW_SERIAL`, xoá define LoRaWAN |
| `include/Prog_Config.h` | Xoá cấu hình LoRa, thêm cấu hình ESP-NOW (channel, LR mode, buffer size) |
| `include/helper.h` | Thay include LoRa bằng ESP-NOW |
| `src/main.cpp` | Thay `loraSetup()` bằng `espnowSetup()`, thay `loraReceive()` bằng `espnowReceive()` |
| `src/helper.cpp` | Cập nhật comment từ LoRa sang ESP-NOW |
| `platformio.ini` | Cập nhật env, xoá thư viện Heltec LoRa, đổi tên env lora thành espnow |

### Kiến trúc truyền thông trước và sau

```
[Trước]  Base ---(LoRa 433MHz)---> ESP32 Rover ---> UM980
[Sau]    Base ---(ESP-NOW WiFi LR)---> ESP32 Rover ---> UM980
```

### Bug cần sửa kèm theo
- File `Nmea_Handler_LoRa.cpp` hiện tại đang ghi log debug vào `Serial1` (cổng nối chip GNSS) thay vì `Serial` (USB/PC). File ESP-NOW mới sẽ sửa lỗi này.