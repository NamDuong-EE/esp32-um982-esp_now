# Rover OTG Debug Viewer

Ứng dụng Android đọc log UART của rover/relay trực tiếp qua cáp USB OTG. Nó thay thế phần quan sát của Web Debug, vì vậy firmware có thể giữ `DEBUG_WEB_ENABLED=0` để tránh SoftAP tác động đến ESP-NOW.

## Chức năng

- Tự phát hiện CP210x, CH340/CH341, FTDI, PL2303 và USB CDC gốc của ESP32.
- Mở serial ở `115200 8N1`.
- Hiển thị toàn bộ log, lọc log, tự cuộn và đánh màu cảnh báo/lỗi.
- Hiển thị Latitude, Longitude, Height, RTK status và số vệ tinh từ GGA.
- Hiển thị đúng nhóm RTCM của Web Debug: liên kết Base → Rover/Relay và, trong Relay mode, Relay → Rover con.
- Đọc telemetry `[DEBUG_STATUS]` mỗi giây; terminal thô được thu gọn ở cuối màn hình.
- Hoạt động hoàn toàn offline; không cần Wi-Fi và không yêu cầu quyền Internet.

## Cài đặt và sử dụng

1. Cài file APK debug trong `app/build/outputs/apk/debug/app-debug.apk` lên điện thoại Android hỗ trợ USB Host/OTG.
2. Nối ESP32 với điện thoại bằng cáp hoặc đầu chuyển USB OTG có truyền dữ liệu.
3. Mở **Rover OTG Debug**, chấp nhận hộp thoại cấp quyền USB.
4. Nếu ứng dụng chưa tự nối, nhấn **Kết nối**. Reset ESP32 để xem trọn log khởi động.

Mỗi lần ứng dụng kết nối một thiết bị USB serial. Để xem relay và rover con đồng thời cần hai điện thoại, hoặc một thiết bị Android có hub OTG và phiên bản ứng dụng bổ sung bộ chọn nhiều cổng.

## Build

Yêu cầu JDK 17 trở lên và Android SDK Platform 35:

```powershell
.\gradlew.bat assembleDebug
```

APK được tạo tại `app/build/outputs/apk/debug/app-debug.apk`.

## Giới hạn

- Ứng dụng chỉ đọc log, không gửi lệnh vào ESP32.
- Dashboard cần firmware có dòng `[DEBUG_STATUS]`; terminal thô vẫn hiển thị được với firmware cũ.
- Một số cáp USB chỉ cấp nguồn và không có dây dữ liệu; loại cáp đó sẽ không được phát hiện.
