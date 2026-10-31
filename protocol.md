# 串口输出协议（固件 ↔ 网页）

网页**只解析**一种行前缀，其余行全部忽略（但仍会原样收进报告的"原始串口输出"附录）。

## RESULT 行

```text
##RESULT {"stage":1,"title":"供电与芯片握手","verdict":"pass","pins":["3V3","GND"],"detail":"检测到 ESP32-S3 rev v0.2","raw":"chip=ESP32-S3 rev0.2"}
```

## 字段约定

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `stage` | number | ✅ | 1~6，对应页面上的六张阶段卡 |
| `title` | string | ✅ | 阶段名，如 `供电与芯片握手` |
| `verdict` | string | ✅ | `pass`（绿勾）/ `fail`（红叉）/ `warn`（黄叹号），大小写不敏感 |
| `pins` | string[] | 建议 | 涉及的引脚，如 `["SDA=8","SCL=9"]` |
| `detail` | string | 建议 | 给人看的一句话说明，网页表格与 report.md 都会展示 |
| `raw` | string | 可选 | 该阶段的原始读数（HEX、寄存器值等） |

## 固件侧打印示例（Arduino）

```cpp
void emit(int stage, const char* title, const char* verdict,
          const char* pins, const char* detail, const char* raw) {
  Serial.printf("##RESULT {\"stage\":%d,\"title\":\"%s\",\"verdict\":\"%s\","
                "\"pins\":[%s],\"detail\":\"%s\",\"raw\":\"%s\"}\n",
                stage, title, verdict, pins, detail, raw);
}

// 用法
emit(1, "供电与芯片握手", "pass", "\"3V3\",\"GND\"", "Flash 16MB / PSRAM 8MB", "chip=ESP32-S3");
```

## 解析容错

- JSON 解析失败 → 该行忽略，不会让页面报错
- `stage` 不是 1~6 → 忽略
- `verdict` 未知值 → 按 `warn`（黄叹号）显示
- 六个阶段**顺序无所谓**，同 `stage` 后到覆盖先到
- 全部 6 个 stage 到齐后，状态栏显示 `体检完成 · 6/6`
