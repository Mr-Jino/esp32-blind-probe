/* =====================================================================
   ESP32 盲探 blind-probe v1
   ---------------------------------------------------------------------
   给新手用的"量线"工具：不猜你插的是什么模块，只回答三个问题——
     1) 每根候选引脚是高、是低、还是悬空？（有没有东西挂着）
     2) I2C 总线上有没有设备应答？（报地址，型号自己查）
     3) SPI Flash 能不能读出 ID？（板子和 SPI 是不是活的）

   输出协议：每个阶段一行 ##RESULT + JSON，给网页控制台解析。
   协议文档见仓库 protocol.md，网页见 web/index.html。

   默认针脚表针对 ESP32-S3（避开 USB 19/20、Flash/PSRAM 26~37、UART0 43/44）。
   换板子改 PROBE_PINS 和 I2C_PAIRS 两个表即可。

   串口发字母 r 可以重跑一次体检。
   ===================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <esp_flash.h>
#include <esp_system.h>

#define PROBE_VERSION 1

/* ---------- 可改：候选引脚表（按板子改这里） ---------- */
static const uint8_t PROBE_PINS[] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 47, 48
};
static const int N_PROBE = sizeof(PROBE_PINS) / sizeof(PROBE_PINS[0]);

/* ---------- 可改：I2C 盲扫的 SDA/SCL 组合 ---------- */
static const uint8_t I2C_PAIRS[][2] = {
  {8, 9}, {9, 8}, {1, 2}, {2, 1}, {3, 4}, {4, 5},
  {5, 6}, {6, 7}, {17, 18}, {41, 42}
};
static const int N_PAIR = sizeof(I2C_PAIRS) / sizeof(I2C_PAIRS[0]);

/* ==================== 输出（协议见 protocol.md） ==================== */

// 同时写 UART0（CH340/CP2102 那个口）和 USB CDC（原生 USB 口），
// 新手插哪个口都能看到，不用猜。
static void logf(const char* fmt, ...) {
  char buf[640];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial0.print(buf);
  Serial.print(buf);
  Serial0.flush();
  Serial.flush();
}

static String escJson(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n')        o += "\\n";
    else if (c == '\r')        o += "\\r";
    else if (c == '\t')        o += "\\t";
    else                       o += c;
  }
  return o;
}

static char g_verdicts[8];   // 每个阶段的判定登记，给阶段 6 汇总用

static void emitResult(int stage, const char* title, const char* verdict,
                       const String& pinsCsv, const String& detail, const String& raw) {
  if (stage >= 1 && stage <= 7) g_verdicts[stage] = verdict[0];  // 'p' / 'w' / 'f'

  // pinsCsv: "GPIO4,GPIO5" 形式，这里包成 JSON 数组
  String arr = "[";
  if (pinsCsv.length()) {
    int start = 0;
    while (true) {
      int comma = pinsCsv.indexOf(',', start);
      String item = (comma < 0) ? pinsCsv.substring(start) : pinsCsv.substring(start, comma);
      item.trim();
      if (item.length()) {
        if (arr.length() > 1) arr += ",";
        arr += "\"" + escJson(item) + "\"";
      }
      if (comma < 0) break;
      start = comma + 1;
    }
  }
  arr += "]";

  logf("##RESULT {\"stage\":%d,\"title\":\"%s\",\"verdict\":\"%s\",\"pins\":%s,\"detail\":\"%s\",\"raw\":\"%s\"}\n",
       stage, title, verdict, arr.c_str(),
       escJson(detail).c_str(), escJson(raw).c_str());
}

/* ==================== 阶段 1：芯片握手 ==================== */

static void stage1() {
  String detail = String(ESP.getChipModel()) + " 双核 " + String(ESP.getCpuFreqMHz()) + "MHz";
  String raw = String("flash=") + (ESP.getFlashChipSize() / 1024 / 1024) + "MB" +
               " psram=" + (ESP.getPsramSize() / 1024 / 1024) + "MB" +
               " sdk=" + ESP.getSdkVersion();
  emitResult(1, "芯片与供电握手", "pass",
             "3V3,GND",
             detail + "；能跑到这里说明供电正常",
             raw);
}

/* ==================== 阶段 2：引脚普查 ==================== */
/* 每根脚读三次：悬空读 / 上拉读 / 下拉读。
   上拉读高 + 下拉读高 = 被外部拉高（挂着东西，输出高）
   上拉读低 + 下拉读低 = 被外部拉低（挂着东西，输出低 / 接地）
   上拉读高 + 下拉读低 = 悬空（什么都没接）
   上拉读低            = 强拉低（疑似对地短路或外设死顶）      */

static char pinState(uint8_t pin) {          // H=外部拉高 L=外部拉低 F=悬空 g=强拉低 h=强拉高
  pinMode(pin, INPUT_PULLUP);  delayMicroseconds(30);
  bool up = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN); delayMicroseconds(30);
  bool down = digitalRead(pin);
  pinMode(pin, INPUT);

  if (up && down)  return 'H';
  if (!up && !down) return 'L';
  if (up && !down) return 'F';
  return down ? 'h' : 'g';
}

static char g_pinStates[64];

static void stage2() {
  int nH = 0, nL = 0, nF = 0, nBad = 0;
  String raw, pulledUp, pulledLow, floating, stuck;

  for (int i = 0; i < N_PROBE; i++) {
    uint8_t p = PROBE_PINS[i];
    char st = pinState(p);
    g_pinStates[i] = st;
    if (i) raw += " ";
    raw += "IO" + String(p) + "=" + st;
    if (st == 'H') { nH++; if (pulledUp.length()) pulledUp += " "; pulledUp += "IO" + String(p); }
    else if (st == 'L') { nL++; if (pulledLow.length()) pulledLow += " "; pulledLow += "IO" + String(p); }
    else if (st == 'F') { nF++; }
    else { nBad++; if (stuck.length()) stuck += " "; stuck += "IO" + String(p); }
  }

  String detail = "候选 " + String(N_PROBE) + " 根：外部拉高 " + nH +
                  " · 外部拉低 " + nL + " · 悬空 " + nF + " · 疑似短路 " + nBad;
  if (pulledUp.length())   detail += "；拉高: " + pulledUp;
  if (pulledLow.length())  detail += "；拉低: " + pulledLow;
  if (stuck.length())      detail += "；强拉低: " + stuck + "（检查是否接反/接死）";

  String pins = pulledUp + (pulledUp.length() && pulledLow.length() ? "," : "") + pulledLow;
  emitResult(2, "引脚普查（高/低/悬空）", nBad ? "warn" : "pass", pins, detail, raw);
}

/* ==================== 阶段 3：I2C 盲扫 ==================== */

static void stage3() {
  String foundAll, pairsTried, raw;
  int nDev = 0;

  for (int p = 0; p < N_PAIR; p++) {
    uint8_t sda = I2C_PAIRS[p][0], scl = I2C_PAIRS[p][1];
    if (p) raw += " ";
    raw += "sda" + String(sda) + "/scl" + String(scl) + ":";

    Wire.begin((int)sda, (int)scl, 100000);
    Wire.setTimeOut(2);                      // 每个地址最多等 2ms，别拖
    int hit = 0;
    for (uint8_t a = 1; a < 120; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        hit++; nDev++;
        char buf[8]; snprintf(buf, sizeof(buf), "0x%02X", a);
        if (foundAll.length()) foundAll += " ";
        foundAll += buf;
        if (raw.length() && raw[raw.length()-1] != ':') raw += ",";
        raw += buf;
      }
    }
    if (!hit) raw += "-";
    Wire.end();

    if (hit) {
      if (pairsTried.length()) pairsTried += " ";
      pairsTried += "SDA=" + String(sda) + "/SCL=" + String(scl);
      break;                                 // 找到一组就收工，报告地址
    }
  }

  String detail;
  const char* verdict;
  if (nDev) {
    verdict = "pass";
    detail = "在 " + pairsTried + " 上发现 " + String(nDev) + " 个设备：" + foundAll +
             "（型号对照地址表自查，工具不猜）";
  } else {
    verdict = "warn";
    detail = "试了 " + String(N_PAIR) + " 组 SDA/SCL 都没设备应答——没接 I2C 模块，或 SDA/SCL 不在候选表里";
  }
  emitResult(3, "I2C 总线盲扫", verdict, pairsTried, detail, raw);
}

/* ==================== 阶段 4：SPI Flash ID ==================== */

static void stage4() {
  uint32_t id = 0;
  esp_err_t err = esp_flash_read_id(NULL, &id);
  uint32_t mbyte = ESP.getFlashChipSize() / 1024 / 1024;

  char idhex[16]; snprintf(idhex, sizeof(idhex), "0x%06lX", (unsigned long)(id & 0xFFFFFF));
  bool ok = (err == ESP_OK && (id & 0xFFFFFF) != 0 && (id & 0xFFFFFF) != 0xFFFFFF);

  String detail = ok
    ? String("JEDEC ID ") + idhex + "，Flash " + mbyte + "MB —— SPI 控制器和 Flash 都在位"
    : String("读不出有效 ID（err=") + (int)err + "）——板子的 SPI 有问题，这不太正常";

  char raw[40]; snprintf(raw, sizeof(raw), "err=%d id=%s", (int)err, idhex);
  emitResult(4, "SPI Flash ID", ok ? "pass" : "fail", "SPI0", detail, raw);
}

/* ==================== 阶段 5：BOOT 键与复位原因 ==================== */

static const char* rstName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "程序崩溃";
    case ESP_RST_INT_WDT:   return "中断看门狗";
    case ESP_RST_TASK_WDT:  return "任务看门狗";
    case ESP_RST_WDT:       return "看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT:  return "欠压复位（供电不稳！）";
    default:                return "其他";
  }
}

static void stage5() {
  pinMode(0, INPUT_PULLUP);
  delayMicroseconds(50);
  bool boot = digitalRead(0) == LOW;
  pinMode(0, INPUT);

  esp_reset_reason_t rr = esp_reset_reason();
  String detail = String("BOOT 键(IO0)：") + (boot ? "按着" : "没按") +
                  "；上一次复位：" + rstName(rr);
  if (rr == ESP_RST_BROWNOUT)
    detail += " —— 如果经常这样，换根 USB 线或换个充电头";
  emitResult(5, "BOOT 键与复位原因", "pass", "IO0", detail,
             String("boot=") + (boot ? 1 : 0) + " rst=" + (int)rr);
}

/* ==================== 阶段 6：综合结论 ==================== */

static void stage6() {
  int pass = 0, warn = 0, fail = 0;
  String raw;
  for (int i = 1; i <= 5; i++) {
    if (i > 1) raw += ",";
    raw += String(i) + ":" + g_verdicts[i];
    if (g_verdicts[i] == 'p') pass++;
    else if (g_verdicts[i] == 'w') warn++;
    else fail++;
  }
  const char* verdict = fail ? "fail" : (warn ? "warn" : "pass");
  String detail = "通过 " + String(pass) + " · 注意 " + warn + " · 未通过 " + fail;
  if (fail) detail += " —— 有硬伤，按上表红叉排查";
  else if (warn) detail += " —— 能用，但黄叹号那几项值得看一眼";
  else detail += " —— 板子很健康";
  emitResult(6, "综合结论", verdict, "", detail, raw);
}

/* ==================== 主流程 ==================== */

static void runProbe() {
  logf("\n=== ESP32 盲探 v%d  |  发送 r 重跑 ===\n", PROBE_VERSION);
  delay(80);

  for (int i = 1; i <= 7; i++) g_verdicts[i] = 'f';   // 先全记为 fail，跑过一个登记一个
  stage1();
  stage2();
  stage3();
  stage4();
  stage5();
  stage6();
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1200) delay(10);   // 等 USB CDC 起来（等不到也无所谓，UART0 照样出）

  runProbe();
}

void loop() {
  // 两个口都要听：网页可能连在 CH340(UART0) 上，也可能连在原生 USB(CDC) 上
  bool rerun = false;
  while (Serial.available())  { int c = Serial.read();  if (c == 'r' || c == 'R') rerun = true; }
  while (Serial0.available()) { int c = Serial0.read(); if (c == 'r' || c == 'R') rerun = true; }
  if (rerun) runProbe();
  delay(20);
}
