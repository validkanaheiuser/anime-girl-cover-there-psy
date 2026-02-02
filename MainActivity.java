package com.mockgps.app;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.GeolocationPermissions;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.InputStreamReader;

public class MainActivity extends Activity {

    private WebView webView;
    private Handler handler = new Handler(Looper.getMainLooper());

    private static final String CONFIG_PATH = "/data/adb/modules/mockgps/location.conf";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Fullscreen immersive
        getWindow().setFlags(
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        );
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        );

        setContentView(R.layout.activity_main);

        webView = findViewById(R.id.webView);
        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setGeolocationEnabled(true);
        ws.setAllowFileAccess(true);

        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onGeolocationPermissionsShowPrompt(String origin, GeolocationPermissions.Callback callback) {
                callback.invoke(origin, true, false);
            }
        });

        webView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                loadCurrentConfig();
            }
        });

        webView.addJavascriptInterface(new WebBridge(), "Android");
        webView.loadUrl("file:///android_asset/map.html");
    }

    private void loadCurrentConfig() {
        new Thread(() -> {
            String config = rootExec("cat " + CONFIG_PATH + " 2>/dev/null");
            if (config != null && !config.isEmpty()) {
                // Parse config to JSON
                StringBuilder json = new StringBuilder("{");
                String[] lines = config.split("\n");
                boolean first = true;
                for (String line : lines) {
                    String[] parts = line.split("=", 2);
                    if (parts.length == 2) {
                        if (!first) json.append(",");
                        first = false;
                        String key = parts[0].trim();
                        String val = parts[1].trim();
                        if (key.equals("enabled") || key.equals("hidedev")) {
                            json.append("\"").append(key).append("\":").append(val);
                        } else {
                            json.append("\"").append(key).append("\":").append(val);
                        }
                    }
                }
                json.append("}");

                String js = "if(typeof loadConfig==='function')loadConfig(" + json + ");";
                handler.post(() -> webView.evaluateJavascript(js, null));
            }
        }).start();
    }

    class WebBridge {
        @JavascriptInterface
        public void saveConfig(String configText) {
            new Thread(() -> {
                // Write config via su
                String escaped = configText.replace("'", "'\\''");
                String cmd = "echo '" + escaped + "' > " + CONFIG_PATH + " && chmod 644 " + CONFIG_PATH;
                String result = rootExec(cmd);
                handler.post(() -> {
                    if (result != null) {
                        Toast.makeText(MainActivity.this, "Config saved ✓", Toast.LENGTH_SHORT).show();
                    } else {
                        Toast.makeText(MainActivity.this, "Root access required!", Toast.LENGTH_LONG).show();
                    }
                });
            }).start();
        }

        @JavascriptInterface
        public void showToast(String msg) {
            handler.post(() -> Toast.makeText(MainActivity.this, msg, Toast.LENGTH_SHORT).show());
        }

        @JavascriptInterface
        public String readConfig() {
            String result = rootExec("cat " + CONFIG_PATH + " 2>/dev/null");
            return result != null ? result : "";
        }
    }

    private String rootExec(String cmd) {
        try {
            Process su = Runtime.getRuntime().exec("su");
            DataOutputStream out = new DataOutputStream(su.getOutputStream());
            out.writeBytes(cmd + "\n");
            out.writeBytes("exit\n");
            out.flush();

            BufferedReader reader = new BufferedReader(new InputStreamReader(su.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append("\n");
            }
            su.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    @Override
    public void onBackPressed() {
        if (webView.canGoBack()) {
            webView.goBack();
        } else {
            super.onBackPressed();
        }
    }
}
