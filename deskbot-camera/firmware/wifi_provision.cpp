#include "wifi_provision.h"

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#if __has_include("wifi_defaults.h")
#include "wifi_defaults.h"
#else
#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID ""
#endif
#ifndef WIFI_DEFAULT_PASSWORD
#define WIFI_DEFAULT_PASSWORD ""
#endif
#endif

namespace {

constexpr char kApSsid[] = "Deskbot_Camera";
constexpr char kPrefsNs[] = "deskbot_wifi";
constexpr char kPrefsSsidKey[] = "ssid";
constexpr char kPrefsPassKey[] = "pass";
constexpr int kMaxReconnectAttempts = 20;

WebServer server(80);
bool done_config = false;
String ssid;
String password;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>WiFi 配置</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 20px;
      background-color: #f5f5f5;
      color: #333;
    }
    .container {
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
      background-color: white;
      border-radius: 8px;
      box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
    }
    h1 {
      color: #0066cc;
      text-align: center;
      margin-bottom: 30px;
    }
    h2 {
      color: #009688;
      border-bottom: 1px solid #eee;
      padding-bottom: 10px;
    }
    .info-section {
      margin-bottom: 30px;
    }
    .status {
      background-color: #e8f5e9;
      padding: 15px;
      border-radius: 5px;
      margin: 20px 0;
    }
    .footer {
      text-align: center;
      margin-top: 30px;
      font-size: 0.9em;
      color: #666;
    }
    button {
      background-color: #4CAF50;
      border: none;
      color: white;
      padding: 10px 20px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 16px;
      margin: 10px 2px;
      cursor: pointer;
      border-radius: 4px;
    }
    #networks-list {
      list-style-type: none;
      padding: 0;
    }
    .network-item {
      padding: 12px 15px;
      border-bottom: 1px solid #ddd;
      cursor: pointer;
      transition: background-color 0.3s;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .network-item:hover {
      background-color: #f0f0f0;
    }
    .network-item.selected {
      background-color: #e3f2fd;
    }
    .wifi-strength {
      margin-left: 10px;
      font-size: 0.9em;
      color: #666;
    }
    .password-form {
      margin-top: 20px;
      padding: 15px;
      background-color: #f9f9f9;
      border-radius: 5px;
      display: none;
    }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 8px;
      margin: 8px 0;
      box-sizing: border-box;
      border: 1px solid #ddd;
      border-radius: 4px;
    }
    .message {
      padding: 10px;
      margin: 10px 0;
      border-radius: 4px;
    }
    .success {
      background-color: #d4edda;
      color: #155724;
    }
    .error {
      background-color: #f8d7da;
      color: #721c24;
    }
    .spinner {
      border: 4px solid rgba(0, 0, 0, 0.1);
      width: 20px;
      height: 20px;
      border-radius: 50%;
      border-top: 4px solid #007bff;
      animation: spin 1s linear infinite;
      display: inline-block;
      margin-right: 10px;
      vertical-align: middle;
    }
    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }
    .hidden {
      display: none;
    }
    .scan-btn {
      background-color: #007bff;
      margin-bottom: 20px;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>WiFi 配置</h1>

    <div class="status">
      <p><strong>状态:</strong> Deskbot Camera 准备配置 Wi-Fi 网络</p>
    </div>

    <div class="info-section">
      <h2>可用 Wi-Fi 网络</h2>
      <p>请选择一个网络连接：</p>

      <button id="scan-btn" class="scan-btn" onclick="scanNetworks()">
        <span id="scan-spinner" class="spinner hidden"></span>
        <span id="scan-text">扫描网络</span>
      </button>

      <div id="message" class="message hidden"></div>

      <ul id="networks-list"></ul>

      <div id="password-form" class="password-form">
        <h3 id="selected-network">网络名称</h3>
        <form id="wifi-form">
          <input type="hidden" id="ssid-input" name="ssid">
          <label for="password-input">密码：</label>
          <input type="password" id="password-input" name="password" placeholder="请输入密码">
          <button type="submit">保存配置</button>
        </form>
      </div>
    </div>

    <div class="footer">
      <p>Open-Deskbot | &copy; 2026</p>
    </div>
  </div>

  <script>
    let selectedNetwork = null;

    function scanNetworks() {
      const scanBtn = document.getElementById('scan-btn');
      const scanSpinner = document.getElementById('scan-spinner');
      const scanText = document.getElementById('scan-text');
      const messageDiv = document.getElementById('message');
      const networksList = document.getElementById('networks-list');

      scanSpinner.classList.remove('hidden');
      scanText.innerText = '扫描中...';
      scanBtn.disabled = true;
      messageDiv.classList.add('hidden');
      networksList.innerHTML = '';
      document.getElementById('password-form').style.display = 'none';

      fetch('/scan-wifi')
        .then(response => response.json())
        .then(data => {
          scanSpinner.classList.add('hidden');
          scanText.innerText = '扫描网络';
          scanBtn.disabled = false;

          if (data.length === 0) {
            messageDiv.innerHTML = '未找到网络';
            messageDiv.className = 'message error';
            messageDiv.classList.remove('hidden');
            return;
          }

          data.forEach(network => {
            const listItem = document.createElement('li');
            listItem.className = 'network-item';
            listItem.setAttribute('data-ssid', network.ssid);

            let strengthText = '';
            if (network.rssi > -50) {
              strengthText = '强';
            } else if (network.rssi > -70) {
              strengthText = '优';
            } else if (network.rssi > -80) {
              strengthText = '中';
            } else {
              strengthText = '弱';
            }

            listItem.innerHTML = `
              <span>${network.ssid}</span>
              <span class="wifi-strength">${strengthText} (${network.rssi} dBm)</span>
            `;

            listItem.addEventListener('click', () => selectNetwork(network.ssid));
            networksList.appendChild(listItem);
          });
        })
        .catch(error => {
          scanSpinner.classList.add('hidden');
          scanText.innerText = '扫描网络';
          scanBtn.disabled = false;

          messageDiv.innerHTML = '扫描网络错误: ' + error.message;
          messageDiv.className = 'message error';
          messageDiv.classList.remove('hidden');
        });
    }

    function selectNetwork(ssid) {
      selectedNetwork = ssid;

      const networkItems = document.querySelectorAll('.network-item');
      networkItems.forEach(item => {
        if (item.getAttribute('data-ssid') === ssid) {
          item.classList.add('selected');
        } else {
          item.classList.remove('selected');
        }
      });

      const passwordForm = document.getElementById('password-form');
      document.getElementById('selected-network').innerText = ssid;
      document.getElementById('ssid-input').value = ssid;
      passwordForm.style.display = 'block';
      document.getElementById('password-input').focus();
    }

    document.getElementById('wifi-form').addEventListener('submit', function(e) {
      e.preventDefault();

      const ssid = document.getElementById('ssid-input').value;
      const password = document.getElementById('password-input').value;
      const messageDiv = document.getElementById('message');
      const networksList = document.getElementById('networks-list');

      if (!ssid) {
        messageDiv.innerHTML = '请选择一个网络';
        messageDiv.className = 'message error';
        messageDiv.classList.remove('hidden');
        return;
      }

      messageDiv.innerHTML = '保存配置中...';
      messageDiv.className = 'message';
      messageDiv.classList.remove('hidden');

      fetch('/save-wifi', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/x-www-form-urlencoded',
        },
        body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
      })
      .then(response => response.json())
      .then(data => {
        if (data.success) {
          messageDiv.innerHTML = 'WiFi 设置成功！';
          messageDiv.className = 'message success';
          networksList.innerHTML = '';
        } else {
          messageDiv.innerHTML = '错误: ' + data.message;
          messageDiv.className = 'message error';
        }
      })
      .catch(error => {
        messageDiv.innerHTML = '保存配置错误: ' + error.message;
        messageDiv.className = 'message error';
      });
    });

    window.onload = function() {
      setTimeout(scanNetworks, 1000);
    };
  </script>
</body>
</html>
)rawliteral";

bool wifi_defaults_configured() {
  return WIFI_DEFAULT_SSID[0] != '\0';
}

bool load_wifi_from_prefs() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, true)) {
    return false;
  }
  ssid = prefs.getString(kPrefsSsidKey, "");
  password = prefs.getString(kPrefsPassKey, "");
  prefs.end();
  ssid.trim();
  password.trim();
  if (ssid.length() == 0) {
    return false;
  }
  Serial.printf("[wifi] loaded saved ssid=%s\r\n", ssid.c_str());
  return true;
}

bool save_wifi_to_prefs(const String& new_ssid, const String& new_password) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return false;
  }
  prefs.putString(kPrefsSsidKey, new_ssid);
  prefs.putString(kPrefsPassKey, new_password);
  prefs.end();
  return true;
}

void clear_wifi_prefs() {
  Preferences prefs;
  if (prefs.begin(kPrefsNs, false)) {
    prefs.clear();
    prefs.end();
    Serial.println("[wifi] cleared saved credentials");
  }
}

bool load_wifi_defaults() {
  if (!wifi_defaults_configured()) {
    return false;
  }
  ssid = WIFI_DEFAULT_SSID;
  password = WIFI_DEFAULT_PASSWORD;
  Serial.printf("[wifi] trying defaults ssid=%s\r\n", ssid.c_str());
  return true;
}

void setup_http_server() {
  done_config = false;

  Serial.printf("[wifi] opening AP %s\r\n", kApSsid);
  WiFi.softAP(kApSsid);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", index_html);
  });

  server.on("/scan-wifi", HTTP_GET, []() {
    String json = "[";
    int n = WiFi.scanNetworks();

    for (int i = 0; i < n; ++i) {
      if (i > 0) json += ",";
      json += "{";
      json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i));
      json += "}";
    }
    json += "]";

    server.send(200, "application/json", json);
    WiFi.scanDelete();
  });

  server.on("/save-wifi", HTTP_POST, []() {
    String new_ssid = server.arg("ssid");
    String new_password = server.arg("password");

    if (new_ssid.length() == 0) {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"SSID cannot be empty\"}");
      return;
    }

    if (!save_wifi_to_prefs(new_ssid, new_password)) {
      server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save credentials\"}");
      return;
    }

    Serial.printf("[wifi] credentials saved ssid=%s\r\n", new_ssid.c_str());
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi configuration saved\"}");
    done_config = true;
  });

  server.begin();

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[wifi] config portal http://%s SSID=%s\r\n", ip.toString().c_str(), kApSsid);
}

void config_wifi() {
  Serial.println("[wifi] enter config mode");
  WiFi.disconnect();
  setup_http_server();

  while (!done_config) {
    server.handleClient();
    delay(10);
  }

  server.close();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect();
  Serial.println("[wifi] config saved, reconnecting...");
}

}  // namespace

bool wifi_provision_connect() {
  int connection_attempts = 0;
  bool using_saved = false;

  Serial.println("[wifi] connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    using_saved = false;
    if (load_wifi_from_prefs()) {
      using_saved = true;
    } else if (load_wifi_defaults()) {
      using_saved = false;
    } else {
      config_wifi();
      connection_attempts = 0;
      continue;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    WiFi.setSleep(false);
    connection_attempts++;
    Serial.print(".");
    delay(1000);

    if (connection_attempts > kMaxReconnectAttempts) {
      if (using_saved) {
        clear_wifi_prefs();
      } else {
        Serial.println("\r\n[wifi] default credentials failed, enter config mode");
        WiFi.disconnect();
        connection_attempts = 0;
        ssid = "";
        password = "";
        config_wifi();
      }
    }
  }

  Serial.println("");
  Serial.printf("[wifi] connected IP=%s RSSI=%d dBm\r\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}
