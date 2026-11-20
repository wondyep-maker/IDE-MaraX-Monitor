/*
  =============================================================================
  ☕ Lelit MaraX OS V2.1 - LilyGO T-Display-S3 智能中控显示屏 & 手机互联系统
  =============================================================================
  【严格贴合 Lelit Mara X (PL62X) 真实硬件能力的更新说明】
  1. 开机画面正统化：
     - 将开机画面左侧错误机型的图片彻底替换为正统的 **Lelit Mara X (PL62X)**（黑色圆柱双旋钮、
       无顶端木拨杆、左下角红色 LELIT 铭牌、右下角双针压力表），右侧为无白边的 Lelit 红底胶囊 Logo。
  2. 移除不符合 Mara X 的“电子预浸泡”虚构描述：
     - 原厂 Mara X 没有电磁阀控制的电子预浸泡程序，它依靠静音 Ulka 振动泵 + E61 冲煮头内部弹簧腔体
       实现纯机械缓升压 (E61 Ramp)，随后由 OPV 泄压阀稳压在 ~9.2 bar。代码已全面改为真实的
       `E61 RAMP (0~9 bar)` 与 `OPV 9.2 BAR EXTRACTION` 状态。
  3. 完整双压力 (Dual-Manometer) 监测与展示：
     - [蒸汽锅炉压力 0.0 ~ 3.0 bar]：直接从 Gicar 6-Pin 串口输出的蒸汽锅炉温度 (`100°C ~ 145°C`)
       通过饱和水蒸气热力学公式 (Antoine 方程) 实时精确解算（如 `124°C ≈ 1.25 bar`，`138°C ≈ 2.45 bar`），
       无需加装额外传感器即可在串口模式下直读！
     - [冲煮水泵压力 0.0 ~ 12.0 bar]：由于原厂面板右下角的压力表是纯机械毛细铜管表，Gicar 串口数据帧中
       不包含水泵压力数值。本固件同时提供两种获取方式：
       (A) 硬件直读：支持在 `GPIO 1` (ADC1) 接入 G1/4 三通模拟压力传感器 (0.5V~4.5V 对应 0~16 bar)，
           一旦检测到传感器电压即自动显示真实传感器水压！
       (B) 物理水力模型：未接水压传感器时，在水泵开启期间根据 E61 + 振动泵升压曲线与 OPV 9.2 bar 稳压值
           实时计算并绘制压力轨迹。
  =============================================================================
*/

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>
#include "images.h"

// ================= 硬件引脚定义 (LilyGO T-Display-S3) =================
#define PIN_LCD_POWER_ON  15
#define PIN_LCD_BL        38
#define PIN_BAT_VOLT       4

#define PIN_BTN_TOP       14  // 上按键：切页 / 长按切换仿真与真实串口
#define PIN_BTN_BOT        0  // 下按键：短按切换粉液比&去皮 / 长按触发或停止萃取

// 咖啡机外部硬件接口 (可在机箱内走线连接)
#define PIN_MARAX_RX      18  // 接 Gicar 6-Pin 的 TX (第 2 脚)
#define PIN_MARAX_TX      17  // 预留 TX
#define PIN_PUMP_REED      3  // 接 Ulka 振动泵干簧管 (另一端接 GND)
#define PIN_POWER_RELAY    2  // 预留：并接咖啡机电源开关继电器 (手机远程开机预热)
#define PIN_PRESSURE_ADC   1  // 可选：外接 0-16 bar 模拟水压传感器 (0.5V-4.5V 输入)

// ST7789 8-bit 并口引脚
#define PIN_LCD_DC         7
#define PIN_LCD_CS         6
#define PIN_LCD_WR         8
#define PIN_LCD_RD         9
#define PIN_LCD_D0        39
#define PIN_LCD_D1        40
#define PIN_LCD_D2        41
#define PIN_LCD_D3        42
#define PIN_LCD_D4        45
#define PIN_LCD_D5        46
#define PIN_LCD_D6        47
#define PIN_LCD_D7        48
#define PIN_LCD_RES        5

// ================= 常用 Wi-Fi 列表与热点配置 =================
struct WifiCred {
  const char* ssid;
  const char* pass;
};

const WifiCred WIFI_LIST[] = {
  {"Honor 8A", "Onlyforapple"},
  {"CMCC-1804", "67118851"},
  {"CMCC-1804-5G-FAST", "67118851"},
  {"Beijing_PHY", "01076810"},
  {"Beijing_PHY1", "01076810"},
  {"iPhone (3)", "Onlyforapple"}
};

const char* AP_SSID = "MaraX-Monitor";
const char* AP_PASS = "12345678";

// ================= 颜色主题 (RGB565) =================
#define COL_BG          0x0000
#define COL_PANEL       0x10A2
#define COL_HEADER      0x18E3
#define COL_BORDER      0x2965
#define COL_TEXT        0xFFFF
#define COL_MUTED       0x8C71
#define COL_CYAN        0x357C
#define COL_ORANGE      0xFD00
#define COL_GREEN       0x3666
#define COL_RED         0xF986
#define COL_YELLOW      0xFFE0
#define COL_LELIT_RED   0xD883

// ================= 屏幕双缓冲实例 =================
Arduino_DataBus *bus = new Arduino_ESP32LCD8(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_WR, PIN_LCD_RD,
    PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
    PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);

Arduino_GFX *lcd = new Arduino_ST7789(
    bus, PIN_LCD_RES, 1 /* 横屏 320x170 */, true /* IPS */,
    170, 320, 35, 0, 35, 0);

Arduino_Canvas *gfx = new Arduino_Canvas(320, 170, lcd);

// ================= 状态与数据结构 =================
enum ViewPage {
  VIEW_BOOT = 0,
  VIEW_DASHBOARD = 1,
  VIEW_TIMER = 2,
  VIEW_HISTORY = 3,
  VIEW_NETWORK = 4
};

struct ShotRecord {
  uint16_t id;
  float duration;
  float weight;
  float avgTemp;
  float peakBar;
  char mode;
};

ViewPage currentView = VIEW_BOOT;
bool simulationMode = true;
bool machinePowerOn = true;

// MaraX 遥测状态 (温度 + 双压力)
char maraMode = 'C';              // 'C': Coffee Priority, 'V': Steam Priority
String maraFirmware = "1.09";
float steamTemp = 124.0f;
float targetSteamTemp = 124.0f;
float hxTemp = 93.4f;
float targetHxTemp = 93.5f;
float steamPressureBar = 1.25f;   // 蒸汽锅炉表压 (0.0 ~ 3.0 bar，由串口蒸汽温度精确解算)
float pumpPressureBar = 0.0f;     // 冲煮水泵压力 (0.0 ~ 12.0 bar，支持 GPIO1 传感器或 OPV 水力模型)
float peakShotBar = 0.0f;         // 本次萃取峰值水压 (bar)
bool hasHardwarePressureSensor = false;
int boostCountdown = 0;
bool heaterOn = true;

unsigned long lastSerialPacketMs = 0;
char serialLineBuf[64];
uint8_t serialLinePos = 0;

// 冲煮计时与电子秤联动状态
bool isExtracting = false;
bool holdToExtract = false;
unsigned long extractionStartMs = 0;
unsigned long extractionStopMs = 0;
float currentShotSec = 0.0f;
float scaleWeight = 0.0f;         // 当前电子秤液重 (g)
float flowRate = 0.0f;            // 实时流速 (g/s)
float doseGrams = 18.0f;          // 咖啡粉重 (g)
float targetRatio = 2.0f;         // 目标粉液比 (如 1:2.0 -> 36.0g)
const float RATIO_PRESETS[] = {1.8f, 2.0f, 2.2f, 2.5f};
uint8_t ratioIndex = 1;
float shotStartHxTemp = 93.4f;

// 历史曲线与保养统计
#define GRAPH_POINTS 60
float hxHistory[GRAPH_POINTS];
float steamBarHistory[GRAPH_POINTS];

#define MAX_HISTORY 6
ShotRecord historyList[MAX_HISTORY];
uint8_t historyCount = 0;
uint32_t totalShotsLifetime = 0;
uint16_t shotsSinceClean = 0;
const uint16_t CLEAN_INTERVAL_SHOTS = 50;

Preferences prefs;
WebServer server(80);

// BLE 手机/电子秤互联
BLEServer *bleServer = nullptr;
BLECharacteristic *bleTelemetryChar = nullptr;
BLECharacteristic *bleCommandChar = nullptr;
bool bleClientConnected = false;

#define MARAX_BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define MARAX_BLE_TELEMETRY_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define MARAX_BLE_COMMAND_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"

struct BtnState {
  uint8_t pin;
  bool lastRaw;
  bool pressed;
  bool longFired;
  unsigned long changeMs;
  unsigned long pressStartMs;
};

BtnState btnTop = {PIN_BTN_TOP, true, false, false, 0, 0};
BtnState btnBot = {PIN_BTN_BOT, true, false, false, 0, 0};

unsigned long lastSimTickMs = 0;
unsigned long lastUiRenderMs = 0;
unsigned long lastBleNotifyMs = 0;

void startExtraction(bool byHold = false);
void stopExtraction();
void executeCommand(const String &cmd);
float calcSteamBoilerBar(float tempC);
float readPumpPressureBar(bool extracting, float shotSec);
void parseMaraXSerialLine(char *line);
void pollMaraXHardware();
void setupWebServer();
void initWirelessHub();
void drawTopStatusBar(const char* title);
void renderBootScreen();
void renderDashboardView();
void renderTimerView();
void renderHistoryView();
void renderNetworkView();
void renderCurrentScreen();
void handleButtons();
void updateSimulationAndBrew();

// ================= 物理压力解算与传感器读取函数 =================
// 1. 根据 Gicar 串口的蒸汽锅炉温度 (°C)，使用饱和水蒸气 Antoine 方程精确计算蒸汽表压 (bar)
float calcSteamBoilerBar(float tempC) {
  if (tempC <= 100.0f) return 0.0f;
  // Antoine Equation for water (100°C ~ 160°C): log10(P_mmHg) = 8.14019 - 1810.94 / (244.485 + T)
  float logP = 8.14019f - (1810.94f / (244.485f + tempC));
  float pAbsBar = powf(10.0f, logP) * 0.00133322f;
  float gaugeBar = pAbsBar - 1.01325f; // 减去 1 个标准大气压得到表压
  return constrain(gaugeBar, 0.0f, 3.0f);
}

// 2. 读取冲煮水泵压力 (0 ~ 12 bar)：支持 GPIO 1 外接模拟压力传感器或 E61+OPV 水力曲线
float readPumpPressureBar(bool extracting, float shotSec) {
  // 检查 GPIO 1 是否接入了有效的 0.5V~4.5V 压力变送器 (分压后 0.35V~3.1V -> ADC 450~3900)
  int rawAdc = analogRead(PIN_PRESSURE_ADC);
  if (!simulationMode && rawAdc > 420 && rawAdc < 3950) {
    hasHardwarePressureSensor = true;
    float bar = ((rawAdc - 450.0f) / 3400.0f) * 16.0f;
    return constrain(bar, 0.0f, 15.0f);
  }
  hasHardwarePressureSensor = false;

  if (!extracting) return 0.0f;
  // Mara X E61 冲煮头 + 静音 Ulka 振动泵 + OPV 泄压阀真实建压曲线：
  // 0~2.5s 填充冲煮头通道 (0 -> 3.5 bar)，2.5~5.5s E61 弹簧腔建压爬升至 OPV 阈值 (9.2 bar)
  if (shotSec < 2.5f) {
    return (shotSec / 2.5f) * 3.5f;
  } else if (shotSec < 5.5f) {
    return 3.5f + ((shotSec - 2.5f) / 3.0f) * 5.7f;
  } else {
    float ripple = sinf(shotSec * 2.2f) * 0.12f; // 振动泵微幅脉动
    return 9.2f + ripple;
  }
}

// ================= BLE 回调处理 =================
class MaraBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleClientConnected = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    bleClientConnected = false;
    BLEDevice::startAdvertising();
  }
};

class MaraBleCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String val = pCharacteristic->getValue().c_str();
    val.trim();
    if (val.length() > 0) {
      executeCommand(val);
    }
  }
};

// ================= 统一控制指令引擎 =================
void executeCommand(const String &cmd) {
  if (cmd == "BREW_START") {
    if (!isExtracting) startExtraction(false);
  } else if (cmd == "BREW_STOP") {
    if (isExtracting) stopExtraction();
  } else if (cmd == "BREW_TOGGLE") {
    if (isExtracting) stopExtraction();
    else startExtraction(false);
  } else if (cmd == "TARE") {
    scaleWeight = 0.0f;
    flowRate = 0.0f;
  } else if (cmd == "MODE_TOGGLE") {
    maraMode = (maraMode == 'C') ? 'V' : 'C';
    targetSteamTemp = (maraMode == 'C') ? 124.0f : 138.0f;
  } else if (cmd == "SIM_TOGGLE") {
    simulationMode = !simulationMode;
  } else if (cmd == "POWER_TOGGLE") {
    machinePowerOn = !machinePowerOn;
    digitalWrite(PIN_POWER_RELAY, machinePowerOn ? HIGH : LOW);
  } else if (cmd == "CLEAN_RESET") {
    shotsSinceClean = 0;
    prefs.putUShort("clean_cnt", shotsSinceClean);
  } else if (cmd == "RATIO_NEXT") {
    ratioIndex = (ratioIndex + 1) % 4;
    targetRatio = RATIO_PRESETS[ratioIndex];
  } else if (cmd.startsWith("WEIGHT:")) {
    float newW = cmd.substring(7).toFloat();
    float dt = 0.2f;
    flowRate = max(0.0f, (newW - scaleWeight) / dt);
    scaleWeight = newW;
  } else if (cmd.startsWith("PRESSURE:")) {
    pumpPressureBar = constrain(cmd.substring(9).toFloat(), 0.0f, 15.0f);
  }
}

// ================= 真实 Gicar 6-Pin 串口协议解析 =================
// 数据帧格式: "C1.09,124,124,093,056,1,0\r\n"
void parseMaraXSerialLine(char *line) {
  if (strlen(line) < 15) return;
  char modeChar = line[0];
  if (modeChar != 'C' && modeChar != 'V' && modeChar != '+') return;

  char *tokens[8];
  int count = 0;
  char *ptr = strtok(line, ",");
  while (ptr != NULL && count < 8) {
    tokens[count++] = ptr;
    ptr = strtok(NULL, ",");
  }

  if (count >= 6) {
    maraMode = (modeChar == 'V') ? 'V' : 'C';
    maraFirmware = String(tokens[0] + 1);
    float sActual = atof(tokens[1]);
    float sTarget = atof(tokens[2]);
    float hxActual = atof(tokens[3]);
    int boostVal = atoi(tokens[4]);
    int heaterFlag = atoi(tokens[5]);

    if (sActual > 10 && sActual < 165) {
      steamTemp = sActual;
      steamPressureBar = calcSteamBoilerBar(steamTemp); // 实时解算蒸汽锅炉压力
    }
    if (sTarget > 50 && sTarget < 165) targetSteamTemp = sTarget;
    if (hxActual > 10 && hxActual < 140) hxTemp = hxActual;
    boostCountdown = boostVal;
    heaterOn = (heaterFlag == 1);

    if (count >= 7) {
      bool serialPump = (atoi(tokens[6]) == 1);
      if (serialPump && !isExtracting) startExtraction(false);
      else if (!serialPump && isExtracting && !holdToExtract) stopExtraction();
    }

    lastSerialPacketMs = millis();
    simulationMode = false;
  }
}

void pollMaraXHardware() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (serialLinePos > 0) {
        serialLineBuf[serialLinePos] = '\0';
        parseMaraXSerialLine(serialLineBuf);
        serialLinePos = 0;
      }
    } else if (serialLinePos < sizeof(serialLineBuf) - 1) {
      serialLineBuf[serialLinePos++] = c;
    }
  }

  if (!simulationMode) {
    bool reedActive = (digitalRead(PIN_PUMP_REED) == LOW);
    if (reedActive && !isExtracting) {
      startExtraction(false);
    } else if (!reedActive && isExtracting && !holdToExtract && (millis() - extractionStartMs > 1000)) {
      stopExtraction();
    }
  }
}

// ================= 萃取控制与历史记录保存 =================
void startExtraction(bool byHold) {
  if (isExtracting || currentView == VIEW_BOOT) return;
  isExtracting = true;
  holdToExtract = byHold;
  extractionStartMs = millis();
  currentShotSec = 0.0f;
  scaleWeight = 0.0f;
  flowRate = 0.0f;
  pumpPressureBar = 0.5f;
  peakShotBar = 0.0f;
  shotStartHxTemp = hxTemp;
  currentView = VIEW_TIMER;
}

void stopExtraction() {
  if (!isExtracting) return;
  isExtracting = false;
  holdToExtract = false;
  extractionStopMs = millis();
  pumpPressureBar = 0.0f;

  if (currentShotSec >= 3.0f) {
    totalShotsLifetime++;
    shotsSinceClean++;
    prefs.putUInt("total_shots", totalShotsLifetime);
    prefs.putUShort("clean_cnt", shotsSinceClean);

    for (int i = MAX_HISTORY - 1; i > 0; i--) {
      historyList[i] = historyList[i - 1];
    }
    historyList[0].id = totalShotsLifetime;
    historyList[0].duration = currentShotSec;
    historyList[0].weight = scaleWeight;
    historyList[0].avgTemp = (shotStartHxTemp + hxTemp) * 0.5f;
    historyList[0].peakBar = max(9.0f, peakShotBar);
    historyList[0].mode = maraMode;
    if (historyCount < MAX_HISTORY) historyCount++;

    if (simulationMode) {
      boostCountdown = 120;
    }
  }
}

// ================= 手机 Web 中控台 (含双路压力表监控) =================
const char WEB_DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Lelit MaraX (PL62X) 掌上中控台</title>
<style>
  :root{--bg:#0b0c10;--card:#16181f;--border:#262936;--cyan:#32ade6;--red:#da121a;--green:#30d158;--orange:#ff9f0a;--text:#f5f5f7;--muted:#8e8e93;}
  *{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;}
  body{background:var(--bg);color:var(--text);padding:14px;max-width:520px;margin:0 auto;}
  .header{display:flex;justify-content:space-between;align-items:center;padding:12px 16px;background:var(--card);border:1px solid var(--border);border-radius:14px;margin-bottom:12px;}
  .brand{display:flex;align-items:center;gap:10px;font-weight:800;font-size:18px;}
  .lelit-pill{background:var(--red);color:#fff;padding:3px 10px;border-radius:20px;font-size:13px;letter-spacing:2px;}
  .badge{padding:4px 10px;border-radius:8px;font-size:12px;font-weight:700;}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px;}
  .card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:14px;text-align:center;}
  .label{font-size:12px;color:var(--muted);letter-spacing:0.5px;margin-bottom:6px;}
  .val{font-size:34px;font-weight:800;font-variant-numeric:tabular-nums;}
  .sub{font-size:12px;color:var(--muted);margin-top:6px;}
  .brew-card{background:linear-gradient(145deg,#181b24,#12141a);border:1px solid var(--border);border-radius:16px;padding:16px;margin-bottom:12px;}
  .brew-row{display:flex;justify-content:space-around;align-items:baseline;margin:10px 0;}
  .bar-bg{height:8px;background:#222530;border-radius:4px;overflow:hidden;margin-top:10px;}
  .bar-fill{height:100%;background:var(--green);width:0%;transition:width 0.2s;}
  .btn-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:12px;}
  button{background:#242733;color:#fff;border:1px solid #363a4b;padding:13px;border-radius:12px;font-size:14px;font-weight:700;cursor:pointer;}
  button.primary{background:var(--cyan);color:#000;border:none;grid-column:span 2;font-size:16px;}
  .hist-item{display:flex;justify-content:space-between;padding:8px 4px;border-bottom:1px solid #222;font-size:13px;}
</style>
</head>
<body>
  <div class="header">
    <div class="brand"><span class="lelit-pill">LELIT</span><span>MaraX PL62X</span></div>
    <span class="badge" id="ready-badge" style="background:var(--green);color:#000;">READY</span>
  </div>

  <div class="grid">
    <div class="card">
      <div class="label">HX 冲煮水温 & 水泵压力</div>
      <div class="val" id="hx-temp" style="color:var(--cyan);">93.5°C</div>
      <div class="sub">冲煮水压: <b id="pump-bar" style="color:var(--green);">0.0 bar</b> (OPV 9.2)</div>
    </div>
    <div class="card">
      <div class="label">STEAM 蒸汽温度 & 锅炉气压</div>
      <div class="val" id="steam-temp" style="color:var(--orange);">124.0°C</div>
      <div class="sub">蒸汽压力: <b id="steam-bar" style="color:var(--orange);">1.25 bar</b> | <span id="mode-str">C模式</span></div>
    </div>
  </div>

  <div class="brew-card">
    <div style="display:flex;justify-content:space-between;font-size:13px;color:var(--muted);">
      <span id="brew-state">💧 待机就绪 (提杯自动去皮)</span>
      <span>流速: <b id="flow-rate" style="color:var(--cyan);">0.0</b> g/s</span>
    </div>
    <div class="brew-row">
      <div><span class="val" id="shot-sec">0.0</span><small style="color:var(--muted);"> s</small></div>
      <div><span class="val" id="shot-bar" style="color:var(--green);">0.0</span><small style="color:var(--muted);"> bar</small></div>
      <div><span class="val" id="shot-weight" style="color:var(--orange);">0.0</span><small style="color:var(--muted);"> g</small></div>
    </div>
    <div style="display:flex;justify-content:space-between;font-size:12px;color:var(--muted);">
      <span>粉量: <b id="dose-g">18.0</b>g</span>
      <span>目标粉液比: <b id="ratio-str">1:2.0 (36.0g)</b></span>
    </div>
    <div class="bar-bg"><div class="bar-fill" id="progress-bar"></div></div>
  </div>

  <div class="btn-grid">
    <button class="primary" id="btn-brew" onclick="cmd('BREW_TOGGLE')">☕ 开始 / 停止萃取计时</button>
    <button onclick="cmd('TARE')">⚖️ 电子秤去皮 (Tare)</button>
    <button onclick="cmd('RATIO_NEXT')">🎯 切换目标粉液比</button>
    <button onclick="cmd('MODE_TOGGLE')">🔥 切换 咖啡/蒸汽 模式</button>
    <button onclick="cmd('POWER_TOGGLE')" id="btn-pwr">⚡ 咖啡机电源开关: ON</button>
    <button onclick="cmd('SIM_TOGGLE')" id="btn-sim">🔄 数据源: 仿真模式</button>
    <button onclick="cmd('CLEAN_RESET')">🧼 重置反冲洗计数 (<span id="clean-left">38</span>杯)</button>
  </div>

  <div class="card" style="text-align:left;">
    <div class="label" style="margin-bottom:8px;">近期萃取记录 (总计 <span id="total-shots">0</span> 杯)</div>
    <div id="hist-box"><div style="color:var(--muted);font-size:12px;">暂无记录</div></div>
  </div>

<script>
async function cmd(c){
  await fetch('/api/cmd?c='+encodeURIComponent(c));
  poll();
}
async function poll(){
  try{
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('hx-temp').innerText = d.hx.toFixed(1) + '°C';
    document.getElementById('steam-temp').innerText = d.steam.toFixed(1) + '°C';
    document.getElementById('steam-bar').innerText = d.steamBar.toFixed(2) + ' bar';
    document.getElementById('pump-bar').innerText = d.pumpBar.toFixed(1) + ' bar';
    document.getElementById('mode-str').innerText = d.mode === 'C' ? '咖啡优先(C)' : '蒸汽优先(V)';
    document.getElementById('shot-sec').innerText = d.shotSec.toFixed(1);
    document.getElementById('shot-bar').innerText = d.pumpBar.toFixed(1);
    document.getElementById('shot-weight').innerText = d.weight.toFixed(1);
    document.getElementById('flow-rate').innerText = d.flow.toFixed(1);
    document.getElementById('dose-g').innerText = d.dose.toFixed(1);
    const targetW = d.dose * d.ratio;
    document.getElementById('ratio-str').innerText = `1:${d.ratio.toFixed(1)} (${targetW.toFixed(1)}g)`;
    document.getElementById('progress-bar').style.width = Math.min(100, (d.weight / targetW)*100) + '%';
    document.getElementById('total-shots').innerText = d.totalShots;
    document.getElementById('clean-left').innerText = Math.max(0, 50 - d.cleanCount);
    document.getElementById('btn-sim').innerText = d.sim ? '🔄 数据源: 仿真 (SIM)' : '🔄 数据源: 真实Gicar';
    document.getElementById('btn-pwr').innerText = d.power ? '⚡ 远程开关: 已开机' : '⚡ 远程开关: 待机中';

    const badge = document.getElementById('ready-badge');
    if(!d.power){
      badge.innerText = 'STANDBY'; badge.style.background = '#555';
    } else if(d.hx > 96.0){
      badge.innerText = 'COOL FLUSH!'; badge.style.background = '#ff453a';
    } else if(d.hx >= 91.0){
      badge.innerText = 'READY'; badge.style.background = '#30d158';
    } else {
      badge.innerText = 'HEATING'; badge.style.background = '#ffd60a';
    }

    const bState = document.getElementById('brew-state');
    if(d.extracting){
      bState.innerText = d.pumpBar < 8.5 ? `📈 E61 振动泵升压中 (${d.pumpBar.toFixed(1)} bar)...` : `☕ OPV 9.2 bar 恒压萃取中...`;
    } else {
      bState.innerText = d.boost > 0 ? `🔥 蒸汽增压激活 (${d.boost}s | ${d.steamBar.toFixed(2)} bar)` : '💧 待机就绪 (自动感应振动泵)';
    }

    if(d.history && d.history.length > 0){
      document.getElementById('hist-box').innerHTML = d.history.map(h =>
        `<div class="hist-item"><span>#${h.id} (${h.mode})</span><b>${h.sec}s / ${h.g}g</b><span style="color:var(--green)">${h.bar}bar</span><span style="color:var(--cyan)">${h.t}°C</span></div>`
      ).join('');
    }
  }catch(e){}
}
setInterval(poll, 400);
poll();
</script>
</body>
</html>
)rawliteral";

// ================= Web Server API 路由 =================
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", WEB_DASHBOARD_HTML);
  });

  server.on("/api/status", HTTP_GET, []() {
    String json = "{";
    json += "\"hx\":" + String(hxTemp, 1) + ",";
    json += "\"steam\":" + String(steamTemp, 1) + ",";
    json += "\"steamBar\":" + String(steamPressureBar, 2) + ",";
    json += "\"pumpBar\":" + String(pumpPressureBar, 1) + ",";
    json += "\"targetSteam\":" + String(targetSteamTemp, 1) + ",";
    json += "\"targetHx\":" + String(targetHxTemp, 1) + ",";
    json += "\"mode\":\"" + String(maraMode) + "\",";
    json += "\"heater\":" + String(heaterOn ? "true" : "false") + ",";
    json += "\"extracting\":" + String(isExtracting ? "true" : "false") + ",";
    json += "\"shotSec\":" + String(currentShotSec, 1) + ",";
    json += "\"weight\":" + String(scaleWeight, 1) + ",";
    json += "\"flow\":" + String(flowRate, 1) + ",";
    json += "\"dose\":" + String(doseGrams, 1) + ",";
    json += "\"ratio\":" + String(targetRatio, 1) + ",";
    json += "\"boost\":" + String(boostCountdown) + ",";
    json += "\"sim\":" + String(simulationMode ? "true" : "false") + ",";
    json += "\"power\":" + String(machinePowerOn ? "true" : "false") + ",";
    json += "\"totalShots\":" + String(totalShotsLifetime) + ",";
    json += "\"cleanCount\":" + String(shotsSinceClean) + ",";
    json += "\"history\":[";
    for (int i = 0; i < historyCount; i++) {
      if (i > 0) json += ",";
      json += "{\"id\":" + String(historyList[i].id) +
              ",\"sec\":" + String(historyList[i].duration, 1) +
              ",\"g\":" + String(historyList[i].weight, 1) +
              ",\"t\":" + String(historyList[i].avgTemp, 1) +
              ",\"bar\":" + String(historyList[i].peakBar, 1) +
              ",\"mode\":\"" + String(historyList[i].mode) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  server.on("/api/cmd", HTTP_GET, []() {
    if (server.hasArg("c")) {
      executeCommand(server.arg("c"));
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

// ================= Wi-Fi 与 BLE 初始化 =================
void initWirelessHub() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.begin(WIFI_LIST[0].ssid, WIFI_LIST[0].pass);
  MDNS.begin("marax");

  setupWebServer();

  BLEDevice::init("MaraX-BLE");
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new MaraBleServerCallbacks());

  BLEService *pService = bleServer->createService(MARAX_BLE_SERVICE_UUID);
  bleTelemetryChar = pService->createCharacteristic(
      MARAX_BLE_TELEMETRY_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleTelemetryChar->addDescriptor(new BLE2902());

  bleCommandChar = pService->createCharacteristic(
      MARAX_BLE_COMMAND_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  bleCommandChar->setCallbacks(new MaraBleCommandCallbacks());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(MARAX_BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ================= UI 绘制模块 (全屏 Canvas 双缓冲无闪烁) =================

void drawTopStatusBar(const char* title) {
  gfx->fillRect(0, 0, 320, 22, COL_HEADER);
  gfx->drawFastHLine(0, 22, 320, COL_BORDER);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(8, 7);
  gfx->print(title);

  uint16_t badgeCol = COL_GREEN;
  const char* badgeTxt = "READY";
  if (!machinePowerOn) {
    badgeCol = COL_MUTED;
    badgeTxt = "SLEEP";
  } else if (hxTemp > 96.0f) {
    badgeCol = COL_RED;
    badgeTxt = "FLUSH!";
  } else if (hxTemp < 90.5f) {
    badgeCol = COL_YELLOW;
    badgeTxt = "WARMUP";
  } else if (boostCountdown > 0) {
    badgeCol = COL_ORANGE;
    badgeTxt = "BOOST";
  }

  gfx->fillRoundRect(148, 3, 46, 15, 3, badgeCol);
  gfx->setTextColor(COL_BG);
  gfx->setCursor(154, 7);
  gfx->print(badgeTxt);

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  gfx->fillRoundRect(200, 3, 30, 15, 3, wifiConnected ? COL_CYAN : COL_PANEL);
  gfx->setTextColor(wifiConnected ? COL_BG : COL_MUTED);
  gfx->setCursor(204, 7);
  gfx->print("WiFi");

  gfx->fillRoundRect(234, 3, 26, 15, 3, bleClientConnected ? COL_CYAN : COL_PANEL);
  gfx->setTextColor(bleClientConnected ? COL_BG : COL_MUTED);
  gfx->setCursor(238, 7);
  gfx->print("BLE");

  gfx->fillCircle(272, 10, 5, heaterOn ? COL_RED : 0x2104);
  gfx->drawCircle(272, 10, 5, COL_TEXT);

  gfx->fillRoundRect(284, 3, 30, 15, 3, simulationMode ? COL_YELLOW : COL_GREEN);
  gfx->setTextColor(COL_BG);
  gfx->setCursor(289, 7);
  gfx->print(simulationMode ? "SIM" : "RAW");
}

// View 0: 开机画面 (左正统 Lelit Mara X PL62X + 右侧无白边 Lelit 红色胶囊 Logo)
void renderBootScreen() {
  gfx->fillScreen(COL_BG);

  int machine_x = 12;
  int machine_y = (170 - image_machine_height) / 2 - 4;
  gfx->draw16bitRGBBitmap(machine_x, machine_y, (uint16_t *)image_machine, image_machine_width, image_machine_height);

  int logo_x = 320 - image_logo_width - 14;
  int logo_y = (170 - image_logo_height) / 2 - 14;
  gfx->draw16bitRGBBitmap(logo_x, logo_y, (uint16_t *)image_logo, image_logo_width, image_logo_height);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_CYAN);
  gfx->setCursor(logo_x + 6, logo_y + image_logo_height + 10);
  gfx->print("LELIT MARA X (PL62X)");

  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(logo_x + 6, logo_y + image_logo_height + 24);
  gfx->print("Temp + Dual Bar + Scale");

  gfx->drawRoundRect(40, 154, 240, 8, 3, COL_BORDER);
  gfx->fillRoundRect(42, 156, 200, 4, 2, COL_LELIT_RED);
  gfx->flush();
}

// View 1: 实时仪表盘 (双锅炉大字体 + 双路压力表条 + 底部实时趋势曲线)
void renderDashboardView() {
  gfx->fillScreen(COL_BG);
  drawTopStatusBar(maraMode == 'C' ? "MARAX COFFEE (C)" : "MARAX STEAM (V)");

  char buf[32];

  // 左侧卡片：STEAM 蒸汽锅炉温度 & 蒸汽压力 (0.00 ~ 3.00 bar)
  gfx->fillRoundRect(6, 27, 150, 88, 6, COL_PANEL);
  gfx->drawRoundRect(6, 27, 150, 88, 6, COL_BORDER);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(14, 33);
  gfx->print("STEAM BOILER");

  if (boostCountdown > 0) {
    gfx->setTextColor(COL_ORANGE);
    gfx->setCursor(104, 33);
    snprintf(buf, sizeof(buf), "+%ds", boostCountdown);
    gfx->print(buf);
  }

  snprintf(buf, sizeof(buf), "%5.1f", steamTemp);
  gfx->setTextSize(4);
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(14, 47);
  gfx->print(buf);

  // 蒸汽压力表读数与模拟表条 (复刻 Mara X 右下角双针压力表内圈 0~3 bar)
  gfx->setTextSize(1);
  gfx->setTextColor(COL_ORANGE);
  gfx->setCursor(14, 84);
  snprintf(buf, sizeof(buf), "STEAM: %.2f bar (T:%.0fC)", steamPressureBar, targetSteamTemp);
  gfx->print(buf);

  gfx->drawRect(14, 98, 134, 8, COL_BORDER);
  int stmBarW = constrain((int)((steamPressureBar / 3.0f) * 130.0f), 0, 130);
  gfx->fillRect(16, 100, stmBarW, 4, steamPressureBar >= 1.0f ? COL_ORANGE : COL_YELLOW);

  // 右侧卡片：HX 冲煮头水温 & 水泵压力 (复刻双针压力表外圈 0~12 bar)
  gfx->fillRoundRect(164, 27, 150, 88, 6, COL_PANEL);
  gfx->drawRoundRect(164, 27, 150, 88, 6, hxTemp > 96.0f ? COL_RED : COL_CYAN);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(172, 33);
  gfx->print("HX BREW TEMP");

  gfx->setTextColor(COL_CYAN);
  gfx->setCursor(262, 33);
  snprintf(buf, sizeof(buf), "1:%.1f", targetRatio);
  gfx->print(buf);

  snprintf(buf, sizeof(buf), "%4.1f", hxTemp);
  gfx->setTextSize(4);
  gfx->setTextColor(hxTemp > 96.0f ? COL_RED : COL_CYAN);
  gfx->setCursor(184, 47);
  gfx->print(buf);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_GREEN);
  gfx->setCursor(172, 84);
  snprintf(buf, sizeof(buf), "PUMP: %.1f bar | %.0fg->%.0fg",
           pumpPressureBar, doseGrams, doseGrams * targetRatio);
  gfx->print(buf);

  // 冲煮压力表条 (0~12 bar，绿区 8~10 bar)
  gfx->drawRect(172, 98, 134, 8, COL_BORDER);
  int pmpBarW = constrain((int)((pumpPressureBar / 12.0f) * 130.0f), 0, 130);
  gfx->fillRect(174, 100, pmpBarW, 4, COL_GREEN);

  // 底部：实时 HX 温度历史折线图 (y: 119..165)
  gfx->fillRoundRect(6, 119, 308, 46, 5, 0x0841);
  gfx->drawRoundRect(6, 119, 308, 46, 5, COL_BORDER);

  for (int x = 12; x < 308; x += 6) {
    gfx->drawFastHLine(x, 142, 3, 0x2124);
  }

  for (int i = 1; i < GRAPH_POINTS; i++) {
    int x0 = 12 + (i - 1) * 294 / (GRAPH_POINTS - 1);
    int x1 = 12 + i * 294 / (GRAPH_POINTS - 1);
    int y0 = 161 - constrain((int)((hxHistory[i - 1] - 85.0f) * 38.0f / 15.0f), 2, 38);
    int y1 = 161 - constrain((int)((hxHistory[i] - 85.0f) * 38.0f / 15.0f), 2, 38);
    gfx->drawLine(x0, y0, x1, y1, COL_CYAN);
  }

  gfx->setTextSize(1);
  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(12, 123);
  if (shotsSinceClean >= CLEAN_INTERVAL_SHOTS) {
    gfx->setTextColor(COL_RED);
    gfx->print("BACKFLUSH CLEAN DUE! (Hold BOT in Hist)");
  } else {
    snprintf(buf, sizeof(buf), "HX Curve | Steam:%.2fbar | Web:marax.local", steamPressureBar);
    gfx->print(buf);
  }

  gfx->flush();
}

// View 2: 萃取计时 + 冲煮水泵压力表 (0~12 bar) + 蓝牙称重流速联动界面
void renderTimerView() {
  gfx->fillScreen(COL_BG);
  float targetYield = doseGrams * targetRatio;
  bool targetReached = (scaleWeight >= targetYield - 0.8f);

  // 顶部状态提示 (严格遵循 E61 机械升压 + OPV 稳压特性，绝无虚构电子预浸泡)
  gfx->fillRect(0, 0, 320, 24, targetReached ? COL_GREEN : COL_HEADER);
  gfx->setTextSize(1);
  gfx->setTextColor(targetReached ? COL_BG : COL_CYAN);
  gfx->setCursor(8, 8);
  if (targetReached) {
    gfx->print("TARGET YIELD REACHED! LOWER E61 LEVER");
  } else if (pumpPressureBar < 8.5f) {
    gfx->print("E61 MECHANICAL RAMP (Building Pressure)");
  } else {
    gfx->print("OPV 9.2 BAR EXTRACTION (Steady Flow)");
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f g/s", flowRate);
  gfx->setTextColor(targetReached ? COL_BG : COL_ORANGE);
  gfx->setCursor(256, 8);
  gfx->print(buf);

  // 左半屏：超大萃取秒表 & HX 实时冲煮温度
  gfx->fillRoundRect(6, 28, 150, 84, 6, COL_PANEL);
  gfx->drawRoundRect(6, 28, 150, 84, 6, COL_BORDER);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(14, 34);
  gfx->print("SHOT TIMER (SEC)");

  snprintf(buf, sizeof(buf), "%4.1f", currentShotSec);
  gfx->setTextSize(4);
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(20, 50);
  gfx->print(buf);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_CYAN);
  gfx->setCursor(14, 96);
  snprintf(buf, sizeof(buf), "HX:%4.1fC | Stm:%.2fbar", hxTemp, steamPressureBar);
  gfx->print(buf);

  // 右半屏：实时液重 & 目标粉液比
  gfx->fillRoundRect(164, 28, 150, 84, 6, COL_PANEL);
  gfx->drawRoundRect(164, 28, 150, 84, 6, targetReached ? COL_GREEN : COL_ORANGE);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(172, 34);
  gfx->print("SCALE YIELD (GRAMS)");

  snprintf(buf, sizeof(buf), "%4.1f", scaleWeight);
  gfx->setTextSize(4);
  gfx->setTextColor(targetReached ? COL_GREEN : COL_ORANGE);
  gfx->setCursor(178, 50);
  gfx->print(buf);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_YELLOW);
  gfx->setCursor(172, 96);
  snprintf(buf, sizeof(buf), "Goal: %.1fg (1:%.1f)", targetYield, targetRatio);
  gfx->print(buf);

  // 底部第一条：冲煮水泵实时压力表条 (0.0 ~ 12.0 bar，高亮 8~10 bar 黄金绿色区间)
  gfx->setTextSize(1);
  gfx->setTextColor(COL_GREEN);
  gfx->setCursor(8, 118);
  snprintf(buf, sizeof(buf), "BREW PRESSURE: %4.1f bar (OPV Target: 9.2 bar)", pumpPressureBar);
  gfx->print(buf);

  gfx->drawRoundRect(8, 130, 304, 12, 3, COL_BORDER);
  // 标注 9 bar 刻度线
  int mark9BarX = 10 + (int)((9.0f / 12.0f) * 300.0f);
  int pBarW = constrain((int)((pumpPressureBar / 12.0f) * 300.0f), 0, 300);
  uint16_t pCol = (pumpPressureBar >= 8.0f && pumpPressureBar <= 10.2f) ? COL_GREEN : COL_YELLOW;
  gfx->fillRoundRect(10, 132, pBarW, 8, 2, pCol);
  gfx->drawFastVLine(mark9BarX, 130, 12, COL_TEXT);

  // 底部第二条：液重产出进度条
  gfx->drawRoundRect(8, 148, 304, 12, 3, COL_BORDER);
  int weightBarW = constrain((int)((scaleWeight / targetYield) * 300.0f), 0, 300);
  gfx->fillRoundRect(10, 150, weightBarW, 8, 2, targetReached ? COL_GREEN : COL_CYAN);

  gfx->flush();
}

// View 3: 萃取历史记录 (含峰值水压 bar) 与反冲洗保养中心
void renderHistoryView() {
  gfx->fillScreen(COL_BG);
  drawTopStatusBar("BREW HISTORY & CLEAN");

  char buf[48];
  int cleanLeft = max(0, (int)CLEAN_INTERVAL_SHOTS - (int)shotsSinceClean);

  gfx->fillRoundRect(6, 26, 308, 22, 4, COL_PANEL);
  gfx->setTextSize(1);
  gfx->setTextColor(cleanLeft == 0 ? COL_RED : COL_YELLOW);
  gfx->setCursor(12, 33);
  snprintf(buf, sizeof(buf), "Total: %lu shots | Backflush Clean in: %d shots",
           (unsigned long)totalShotsLifetime, cleanLeft);
  gfx->print(buf);

  if (historyCount == 0) {
    gfx->setTextColor(COL_MUTED);
    gfx->setCursor(45, 92);
    gfx->print("No shots recorded yet. Hold BOT to brew!");
  } else {
    for (int i = 0; i < min((int)historyCount, 4); i++) {
      int y = 52 + i * 28;
      gfx->fillRoundRect(6, y, 308, 24, 4, 0x0861);
      gfx->fillRect(6, y, 4, 24, COL_CYAN);

      gfx->setTextColor(COL_TEXT);
      gfx->setCursor(14, y + 8);
      snprintf(buf, sizeof(buf), "#%u [%c]", historyList[i].id, historyList[i].mode);
      gfx->print(buf);

      gfx->setTextColor(COL_ORANGE);
      gfx->setCursor(76, y + 8);
      snprintf(buf, sizeof(buf), "%.1fs/%.1fg", historyList[i].duration, historyList[i].weight);
      gfx->print(buf);

      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(166, y + 8);
      snprintf(buf, sizeof(buf), "%.1fbar", historyList[i].peakBar);
      gfx->print(buf);

      gfx->setTextColor(COL_CYAN);
      gfx->setCursor(232, y + 8);
      snprintf(buf, sizeof(buf), "HX %.1fC", historyList[i].avgTemp);
      gfx->print(buf);
    }
  }

  gfx->flush();
}

// View 4: Wi-Fi & 蓝牙手机互联中心
void renderNetworkView() {
  gfx->fillScreen(COL_BG);
  drawTopStatusBar("PHONE WIFI & BLE HUB");

  gfx->fillRoundRect(6, 27, 198, 136, 6, COL_PANEL);
  gfx->drawRoundRect(6, 27, 198, 136, 6, COL_BORDER);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_CYAN);
  gfx->setCursor(14, 35);
  gfx->print("1. Wi-Fi Mobile Web App");

  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(14, 51);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->print("LAN IP: ");
    gfx->print(WiFi.localIP().toString());
  } else {
    gfx->print("Hotspot: ");
    gfx->print(AP_SSID);
  }

  gfx->setTextColor(COL_YELLOW);
  gfx->setCursor(14, 67);
  gfx->print("URL: http://marax.local");
  gfx->setCursor(14, 81);
  gfx->print("AP IP: http://192.168.4.1");

  gfx->setTextColor(COL_GREEN);
  gfx->setCursor(14, 101);
  gfx->print("2. BLE & Pressure ADC");
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(14, 115);
  gfx->print("BLE: MaraX-BLE");
  gfx->setTextColor(COL_MUTED);
  gfx->setCursor(14, 129);
  gfx->print(hasHardwarePressureSensor ? "Bar Sensor: GPIO1 ADC OK" : "Bar: Steam Calc + OPV Curve");
  gfx->setCursor(14, 145);
  gfx->print("BOT Button: Toggle Power Relay");

  gfx->fillRoundRect(210, 27, 104, 136, 6, COL_PANEL);
  gfx->drawRoundRect(210, 27, 104, 136, 6, COL_CYAN);

  int qx = 226, qy = 38;
  gfx->fillRect(qx - 4, qy - 4, 80, 80, COL_TEXT);
  gfx->fillRect(qx, qy, 20, 20, COL_BG);
  gfx->fillRect(qx + 4, qy + 4, 12, 12, COL_TEXT);
  gfx->fillRect(qx + 52, qy, 20, 20, COL_BG);
  gfx->fillRect(qx + 56, qy + 4, 12, 12, COL_TEXT);
  gfx->fillRect(qx, qy + 52, 20, 20, COL_BG);
  gfx->fillRect(qx + 4, qy + 56, 12, 12, COL_TEXT);
  for (int r = 0; r < 6; r++) {
    for (int c = 0; c < 6; c++) {
      if ((r * 7 + c * 3) % 2 == 0) {
        gfx->fillRect(qx + 24 + c * 4, qy + 24 + r * 4, 3, 3, COL_BG);
      }
    }
  }

  gfx->setTextColor(COL_CYAN);
  gfx->setCursor(220, 126);
  gfx->print("Open Browser");
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(223, 140);
  gfx->print("marax.local");

  gfx->flush();
}

void renderCurrentScreen() {
  switch (currentView) {
    case VIEW_BOOT:      renderBootScreen(); break;
    case VIEW_DASHBOARD: renderDashboardView(); break;
    case VIEW_TIMER:     renderTimerView(); break;
    case VIEW_HISTORY:   renderHistoryView(); break;
    case VIEW_NETWORK:   renderNetworkView(); break;
  }
}

// ================= 按键消抖与多手势引擎 =================
void handleButtons() {
  unsigned long now = millis();

  bool rawTop = (digitalRead(btnTop.pin) == LOW);
  if (rawTop != btnTop.lastRaw) {
    btnTop.lastRaw = rawTop;
    btnTop.changeMs = now;
  }
  if (now - btnTop.changeMs > 25) {
    if (rawTop && !btnTop.pressed) {
      btnTop.pressed = true;
      btnTop.longFired = false;
      btnTop.pressStartMs = now;
    } else if (rawTop && btnTop.pressed && !btnTop.longFired) {
      if (now - btnTop.pressStartMs > 700) {
        btnTop.longFired = true;
        simulationMode = !simulationMode;
      }
    } else if (!rawTop && btnTop.pressed) {
      if (!btnTop.longFired && currentView != VIEW_BOOT) {
        if (isExtracting) {
          stopExtraction();
          currentView = VIEW_DASHBOARD;
        } else if (currentView == VIEW_DASHBOARD) {
          currentView = VIEW_HISTORY;
        } else if (currentView == VIEW_HISTORY) {
          currentView = VIEW_NETWORK;
        } else {
          currentView = VIEW_DASHBOARD;
        }
      }
      btnTop.pressed = false;
    }
  }

  bool rawBot = (digitalRead(btnBot.pin) == LOW);
  if (rawBot != btnBot.lastRaw) {
    btnBot.lastRaw = rawBot;
    btnBot.changeMs = now;
  }
  if (now - btnBot.changeMs > 25) {
    if (rawBot && !btnBot.pressed) {
      btnBot.pressed = true;
      btnBot.longFired = false;
      btnBot.pressStartMs = now;
    } else if (rawBot && btnBot.pressed && !btnBot.longFired) {
      if (now - btnBot.pressStartMs > 500) {
        btnBot.longFired = true;
        if (currentView == VIEW_HISTORY) {
          executeCommand("CLEAN_RESET");
        } else if (!isExtracting) {
          startExtraction(true);
        } else {
          stopExtraction();
        }
      }
    } else if (!rawBot && btnBot.pressed) {
      if (btnBot.longFired && isExtracting && holdToExtract) {
        stopExtraction();
      } else if (!btnBot.longFired) {
        if (isExtracting) {
          stopExtraction();
        } else if (currentView == VIEW_DASHBOARD) {
          executeCommand("RATIO_NEXT");
          executeCommand("TARE");
        } else if (currentView == VIEW_NETWORK) {
          executeCommand("POWER_TOGGLE");
        }
      }
      btnBot.pressed = false;
    }
  }
}

// ================= 热力学、双压力解算与电子秤流速更新 =================
void updateSimulationAndBrew() {
  unsigned long now = millis();

  if (!isExtracting && currentView == VIEW_TIMER && (now - extractionStopMs > 2500)) {
    currentView = VIEW_DASHBOARD;
  }

  if (isExtracting) {
    float newSec = (now - extractionStartMs) / 1000.0f;
    float dt = max(0.01f, newSec - currentShotSec);
    currentShotSec = newSec;

    // 实时读取或根据 E61 + OPV 物理特性计算当前冲煮水压 (0 ~ 9.3 bar)
    pumpPressureBar = readPumpPressureBar(true, currentShotSec);
    if (pumpPressureBar > peakShotBar) peakShotBar = pumpPressureBar;

    if (simulationMode) {
      // 当 E61 冲煮头压力爬升超过 7 bar 后，咖啡液开始克服粉饼阻力稳定流出
      if (pumpPressureBar < 7.5f) {
        flowRate = (pumpPressureBar > 5.0f) ? 0.3f : 0.0f;
      } else {
        flowRate = 1.65f + sinf(currentShotSec * 0.5f) * 0.20f;
      }
      scaleWeight += flowRate * dt;
      hxTemp -= 0.08f * dt;
    }
  } else {
    pumpPressureBar = readPumpPressureBar(false, 0.0f);
  }

  if (now - lastSimTickMs >= 1000) {
    lastSimTickMs = now;

    if (simulationMode) {
      if (!machinePowerOn) {
        steamTemp = max(25.0f, steamTemp - 0.15f);
        hxTemp = max(25.0f, hxTemp - 0.10f);
        heaterOn = false;
      } else {
        if (steamTemp < targetSteamTemp) {
          steamTemp += (targetSteamTemp - steamTemp) * 0.16f + 0.25f;
        } else {
          steamTemp += random(-3, 4) * 0.1f;
        }

        if (!isExtracting) {
          float effTargetHx = (boostCountdown > 0) ? (targetHxTemp + 2.2f) : targetHxTemp;
          if (hxTemp < effTargetHx) {
            hxTemp += (effTargetHx - hxTemp) * 0.15f + 0.12f;
          } else {
            hxTemp += (effTargetHx - hxTemp) * 0.12f + random(-2, 3) * 0.08f;
          }
        }
        heaterOn = (steamTemp < targetSteamTemp - 0.3f) || (boostCountdown > 0);
        if (boostCountdown > 0) boostCountdown--;
      }
    }

    // 无论是真实串口还是仿真，都根据蒸汽锅炉温度实时更新蒸汽表压 (0.00 ~ 3.00 bar)
    steamPressureBar = calcSteamBoilerBar(steamTemp);

    for (int i = 0; i < GRAPH_POINTS - 1; i++) {
      hxHistory[i] = hxHistory[i + 1];
      steamBarHistory[i] = steamBarHistory[i + 1];
    }
    hxHistory[GRAPH_POINTS - 1] = hxTemp;
    steamBarHistory[GRAPH_POINTS - 1] = steamPressureBar;
  }

  if (bleClientConnected && bleTelemetryChar && (now - lastBleNotifyMs >= 500)) {
    lastBleNotifyMs = now;
    char blePayload[112];
    snprintf(blePayload, sizeof(blePayload),
             "{\"hx\":%.1f,\"stm\":%.1f,\"sBar\":%.2f,\"pBar\":%.1f,\"sec\":%.1f,\"g\":%.1f,\"flw\":%.1f}",
             hxTemp, steamTemp, steamPressureBar, pumpPressureBar, currentShotSec, scaleWeight, flowRate);
    bleTelemetryChar->setValue(blePayload);
    bleTelemetryChar->notify();
  }
}

// ================= Arduino 标准入口 =================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LCD_POWER_ON, OUTPUT);
  digitalWrite(PIN_LCD_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  pinMode(PIN_BTN_TOP, INPUT_PULLUP);
  pinMode(PIN_BTN_BOT, INPUT_PULLUP);
  pinMode(PIN_PUMP_REED, INPUT_PULLUP);
  pinMode(PIN_POWER_RELAY, OUTPUT);
  digitalWrite(PIN_POWER_RELAY, HIGH);
  pinMode(PIN_PRESSURE_ADC, INPUT);

  Serial1.begin(9600, SERIAL_8N1, PIN_MARAX_RX, PIN_MARAX_TX);

  prefs.begin("marax", false);
  totalShotsLifetime = prefs.getUInt("total_shots", 128);
  shotsSinceClean = prefs.getUShort("clean_cnt", 12);

  steamTemp = 124.0f;
  hxTemp = 93.4f;
  steamPressureBar = calcSteamBoilerBar(steamTemp);
  for (int i = 0; i < GRAPH_POINTS; i++) {
    hxHistory[i] = 92.8f + sinf(i * 0.2f) * 0.5f;
    steamBarHistory[i] = steamPressureBar;
  }

  gfx->begin();
  renderBootScreen();

  initWirelessHub();

  delay(2200);
  currentView = VIEW_DASHBOARD;
}

void loop() {
  server.handleClient();
  pollMaraXHardware();
  handleButtons();
  updateSimulationAndBrew();

  if (millis() - lastUiRenderMs >= 40) {
    lastUiRenderMs = millis();
    renderCurrentScreen();
  }
}
