package asia.aitogy.roverdebug;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.view.WindowManager;

import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import androidx.core.content.ContextCompat;

import org.json.JSONObject;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;

public final class MainActivity extends Activity
        implements SerialInputOutputManager.Listener {

    private static final String ACTION_USB_PERMISSION =
            "asia.aitogy.roverdebug.USB_PERMISSION";
    private static final int BAUD_RATE = 115200;
    private static final int MAX_PARTIAL_LINE_LENGTH = 16 * 1024;

    private final Object serialLock = new Object();
    private final StringBuilder partialLine = new StringBuilder();

    private WebView webView;
    private UsbManager usbManager;
    private UsbSerialPort serialPort;
    private SerialInputOutputManager ioManager;
    private UsbDevice connectedDevice;
    private volatile boolean pageReady;
    private volatile String latestStatus = "USB not connected";
    private volatile boolean latestConnected;

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final String action = intent.getAction();
            final UsbDevice device = getUsbDevice(intent);
            if (ACTION_USB_PERMISSION.equals(action)) {
                if (device != null &&
                        intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    connectDevice(device);
                } else {
                    postStatus("USB access denied", false);
                }
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                postStatus("USB device detected", false);
                connectFirstAvailableDevice();
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action) &&
                    device != null && connectedDevice != null &&
                    device.getDeviceId() == connectedDevice.getDeviceId()) {
                disconnectSerial("USB device detached");
            }
        }
    };

    @Override
    @SuppressLint("SetJavaScriptEnabled")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        webView = new WebView(this);
        setContentView(webView);

        final WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(true);
        settings.setAllowContentAccess(false);
        webView.setWebViewClient(new WebViewClient());
        webView.setWebChromeClient(new WebChromeClient());
        webView.addJavascriptInterface(new AndroidBridge(), "Android");
        webView.loadUrl("file:///android_asset/index.html");

        final IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        ContextCompat.registerReceiver(
                this,
                usbReceiver,
                filter,
                ContextCompat.RECEIVER_EXPORTED);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        final UsbDevice device = getUsbDevice(intent);
        if (device != null) {
            connectDevice(device);
        }
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(usbReceiver);
        disconnectSerial(null);
        if (webView != null) {
            webView.removeJavascriptInterface("Android");
            webView.destroy();
        }
        super.onDestroy();
    }

    @Override
    public void onNewData(byte[] data) {
        if (data == null || data.length == 0) {
            return;
        }
        final String chunk = new String(data, StandardCharsets.UTF_8);
        synchronized (serialLock) {
            partialLine.append(chunk);
            int newlineIndex;
            while ((newlineIndex = partialLine.indexOf("\n")) >= 0) {
                String line = partialLine.substring(0, newlineIndex);
                partialLine.delete(0, newlineIndex + 1);
                if (line.endsWith("\r")) {
                    line = line.substring(0, line.length() - 1);
                }
                postSerialLine(line);
            }
            if (partialLine.length() > MAX_PARTIAL_LINE_LENGTH) {
                final String overflow = partialLine.toString();
                partialLine.setLength(0);
                postSerialLine(overflow);
            }
        }
    }

    @Override
    public void onRunError(Exception error) {
        final String detail = error == null || error.getMessage() == null
                ? "unknown cause"
                : error.getMessage();
        runOnUiThread(() -> disconnectSerial("USB serial error: " + detail));
    }

    private void connectFirstAvailableDevice() {
        final List<UsbSerialDriver> drivers =
                UsbSerialProber.getDefaultProber().findAllDrivers(usbManager);
        if (drivers.isEmpty()) {
            postStatus("No CP210x/CH340/FTDI/CDC device found over OTG", false);
            return;
        }
        connectDriver(drivers.get(0));
    }

    private void connectDevice(UsbDevice device) {
        final UsbSerialDriver driver = UsbSerialProber.getDefaultProber().probeDevice(device);
        if (driver == null) {
            postStatus(String.format(Locale.ROOT,
                    "Unsupported USB device: VID=%04X PID=%04X",
                    device.getVendorId(), device.getProductId()), false);
            return;
        }
        connectDriver(driver);
    }

    private void connectDriver(UsbSerialDriver driver) {
        final UsbDevice device = driver.getDevice();
        if (!usbManager.hasPermission(device)) {
            final Intent permissionIntent =
                    new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
            final PendingIntent pendingIntent = PendingIntent.getBroadcast(
                    this,
                    0,
                    permissionIntent,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            usbManager.requestPermission(device, pendingIntent);
            postStatus("Waiting for USB permission…", false);
            return;
        }

        disconnectSerial(null);
        final UsbDeviceConnection connection = usbManager.openDevice(device);
        if (connection == null) {
            postStatus("Unable to open USB device", false);
            return;
        }
        if (driver.getPorts().isEmpty()) {
            connection.close();
            postStatus("USB serial device has no port", false);
            return;
        }

        final UsbSerialPort port = driver.getPorts().get(0);
        try {
            port.open(connection);
            port.setParameters(
                    BAUD_RATE,
                    8,
                    UsbSerialPort.STOPBITS_1,
                    UsbSerialPort.PARITY_NONE);
            try {
                port.setDTR(true);
                port.setRTS(true);
            } catch (UnsupportedOperationException ignored) {
                // Some USB-UART bridges do not expose modem control lines.
            }

            final SerialInputOutputManager manager =
                    new SerialInputOutputManager(port, this);
            synchronized (serialLock) {
                serialPort = port;
                ioManager = manager;
                connectedDevice = device;
                partialLine.setLength(0);
            }
            manager.start();
            postStatus(String.format(Locale.ROOT,
                    "Connected to %s · VID=%04X PID=%04X · %d baud",
                    driver.getClass().getSimpleName(),
                    device.getVendorId(),
                    device.getProductId(),
                    BAUD_RATE), true);
        } catch (Exception error) {
            try {
                port.close();
            } catch (IOException ignored) {
            }
            postStatus("Unable to configure USB serial: " + error.getMessage(), false);
        }
    }

    private void disconnectSerial(String status) {
        final SerialInputOutputManager manager;
        final UsbSerialPort port;
        synchronized (serialLock) {
            manager = ioManager;
            port = serialPort;
            ioManager = null;
            serialPort = null;
            connectedDevice = null;
            partialLine.setLength(0);
        }
        if (manager != null) {
            manager.stop();
        }
        if (port != null) {
            try {
                port.close();
            } catch (IOException ignored) {
            }
        }
        if (status != null) {
            postStatus(status, false);
        }
    }

    private void postSerialLine(String line) {
        if (!pageReady || webView == null) {
            return;
        }
        final String script = "window.onSerialLine(" + JSONObject.quote(line) + ");";
        webView.post(() -> webView.evaluateJavascript(script, null));
    }

    private void postStatus(String status, boolean connected) {
        latestStatus = status;
        latestConnected = connected;
        if (!pageReady || webView == null) {
            return;
        }
        final String script = "window.onUsbStatus(" + JSONObject.quote(status) + "," +
                (connected ? "true" : "false") + ");";
        webView.post(() -> webView.evaluateJavascript(script, null));
    }

    @SuppressWarnings("deprecation")
    private static UsbDevice getUsbDevice(Intent intent) {
        if (intent == null) {
            return null;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        }
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }

    public final class AndroidBridge {
        @JavascriptInterface
        public void ready() {
            pageReady = true;
            postStatus(latestStatus, latestConnected);
            runOnUiThread(() -> {
                final UsbDevice attached = getUsbDevice(getIntent());
                if (attached != null) {
                    connectDevice(attached);
                } else {
                    connectFirstAvailableDevice();
                }
            });
        }

        @JavascriptInterface
        public void connect() {
            runOnUiThread(MainActivity.this::connectFirstAvailableDevice);
        }

        @JavascriptInterface
        public void disconnect() {
            runOnUiThread(() -> disconnectSerial("Disconnected"));
        }

    }
}
