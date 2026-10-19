<p align="right">
  <a href="zcode-pet.md">English</a> · <strong>简体中文</strong>
</p>

# zcode-pet 桌面伴宠

把 macOS 桌面宠物 [zcode-pet](https://github.com/zhaoliang21/zcode-pet) 镜像到本设备的派生玩法。Mac 守护进程是任务计数/状态的唯一权威；设备端只做「状态镜像 + 帧播放器」——本地零任务推断，桌面与设备语义不会分叉。

## 数据流

```text
zcode-pet 守护进程（Mac）                  ESP32-C3 设备（本固件）
├─ PetDisplayState 快照                   ├─ zcp_bridge.c   NimBLE 外设，常驻广播
│  (mode/tier/run/unread/mid)            │   · GATT 服务 5A1C0001-…，TX notify / RX write
├─ DeviceBridge.swift（CoreBluetooth     │   · status → 镜像 + ACK，10s 看门狗
│  中心：扫描 → 连接 → 订阅）            │   · hello 每 5s；OK 键 → read notify
│  · status 每 2s（write-with-response， ├─ zcp_pet.c      ZPF1 解码 + 2× 上采样 → lv_image @ 8fps
│    preferred MTU 256）                 ├─ zcp_app.c      Boot/Pet/About 页 + 浮层 + 按键
│  · read 消息 → 桌面已读路径            └─ zcp_strings.c  闭集文案表
└─ --export-esp32 离线烘焙 ZPF1 clip（入库本仓库）
```

语义源：守护进程的 `PetDisplayState` 快照同时喂桌面宠物文案与 BLE `status` 消息，设备文案与 Mac 文案是同一来源的两个投影。`mid` 字段兼任设备端动画/文案选择器。

## 传输（BLE GATT）

设备是常驻广播的 NimBLE 外设；Mac 是 CoreBluetooth 中心。不配对、不加密、无白名单——载荷只有计数与闭集文案 id（v1 有意取舍，记录于此）。广播包带 flags + 128bit 服务 UUID；完整设备名 `zcode-pet` 放 scan response。断开后设备立即以 60ms 快间隔重广播 30s，之后回落 320ms 慢间隔——Mac 回归时重连远快于 1s，待机功耗又可控。

## 协议（单行 JSON，status ≤512B）

| 消息 | 方向 | GATT 载体 | 载荷 |
| --- | --- | --- | --- |
| `hello` | 设备 → Mac（notify 每 5s） | TX（≤64B） | `{"v":1,"t":"hello","name":"zcode-pet","fw":"1.0.0"}` |
| `status` | Mac → 设备（write-with-response；每 2s 周期兜底 + 0.25s 节流的变化即时补发） | RX（≤512B） | `{"v":1,"t":"status","seq":N,"mode":"idle\|working\|celebrate\|error","tier":0..3,"run":N,"unread":N,"mid":M,"n":可选,"ts":毫秒}` |
| `ack` | 设备 → Mac（notify） | TX | `{"v":1,"t":"ack","seq":N}` |
| `read` | 设备 → Mac（notify，OK 键） | TX | `{"v":1,"t":"read"}` |

- UUID（两侧硬编码同常量；`check_esp32_bridge.sh [4/4]` 逐字节 diff）：服务 `5A1C0001-7BCD-4E0F-9A2B-3D5E6F8091A2`、TX `5A1C0002-…`、RX `5A1C0003-…`。
- MTU：preferred MTU 256（`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`），约 170B 的 status 单 PDU 即容纳；协商更低时 CoreBluetooth 自动回落 ATT long write、NimBLE 自动重装——协议层零自定义分块。
- `mid` 取值：1 深睡 / 2 委屈 / 3 打盹 / 4 休息 / 5 长任务 / 6 专注 / 7 N 并行 / 8 N 忙翻 / 9 执行中 / 10 完成 / 11 连击 / 12 蓝牙启动中 / 13 等待 / 14 Mac 掉线 / 15 出错 / 16 发现诊断。1-11 与 15 镜像桌面文案（去 emoji 投影）；12-14 与 16 是设备专属文案。
- 链路规则：TX notify 订阅成功 = online；10s 无任何设备包（`hello`/`ack`/`read`）两侧同时判死——Mac 主动断开并按 0/1/2/5s 退避重扫，设备回等待态（`mid=14`）。GATT 断开设备立即重新广播；Mac 自动重扫。两侧链路自愈，无需按键。

## 按键

| 按键 | 页面 | 动作 |
| --- | --- | --- |
| 上 单击 | Pet | 开/关任务清单浮层（`任务 N · 未读 M`，纯镜像数据） |
| 上 单击 | Pet（BLE 失败） | 重启 BLE 栈 |
| 下 单击 | Pet | 本地摸头动画 2s（不改计数语义） |
| 确定 单击 | Pet（ONLINE） | 发 `read` → 桌面标已读 |
| 确定 单击 | Pet（非 ONLINE） | 蓝牙救援：断开 GATT 回广播态 |
| 确定 单击 | Pet（BLE 失败） | 重启 BLE 栈 |
| 确定 长按 | Pet | 进 About 页 |
| 上 / 下 / 确定 / 确定长按 | About | 回 Pet / 刷新电量 / 重启 BLE 栈 / 回 Pet |

About 页显示设备名 + 固件版本、CW2017 电量（电量计缺席显示 `?`）与按键提示。没有配网页：BLE 无需凭据。UDP 草案里的 Config 页与 `zcp_provision` 已整体移除。

## 动画帧（ZPF1）

每一帧都由桌面矢量引擎离线烘焙——零手绘、零 AI 生图。12 段锁相循环 clip（`ball-*.zpf`，118×76 @ 8fps，348 帧，约 0.32MB）经 `EMBED_FILES` 嵌入，解码后 2× 上采样进静态 RGB565 缓冲（236×152，约 70KB 内部 RAM），经 `lv_image` 变量源渲染。格式：`ZPF1` 头、固定 16 色 RGB565 调色板（index0 透明占位、index15 = 压平底色）、逐帧 `u32` 长度 + 1bpp 透明掩码 RLE + 4bpp 调色板索引。半透明像素压平到 `#1A1A1A`（RGB565 `0x18C3`）；宠物页底色必须同色（`ZCP_BG_HEX`）。

形象级缺口（方案内声明，语义由文案承载）：不耐烦沿用 working 帧 + 「任务跑了好久了」文案；连击庆祝沿用 celebrate clip + 连击文案；拎起姿态无设备场景。

## 文案与字体

所有上屏文案都是闭集且单一来源：`zcode-pet/scripts/device_strings.txt`，两节（`# sync:` 桌面投影，受 zcode-pet 同步断言约束；`# device:` 设备专属文案）。`scripts/gen_device_assets.sh` 重生成 `main/zcp_strings.c/.h`、`assets/fonts/zcp_font_16.c`（16px 子集，`lv_font_conv 1.5.3`，`--no-compress`，163 个码点；字体源 Noto Sans CJK SC Regular，SIL OFL 1.1）与 `assets/fonts/zcp_font_16.codepoints.txt`——码点登记文件，`build_pet.sh` 据此做覆盖检查（文案码点 ⊆ 登记 ∪ 可打印 ASCII，漏跑生成器是构建失败而不是真机豆腐）。`%d` 模板里的数字走 `lv_font_montserrat_14` fallback。zcode-pet 仓库里的同名脚本现在只是转发到本脚本。

## 构建

```bash
scripts/build_pet.sh   # 守卫 + tools/validate.sh --firmware + 产物路径
```

需要 ESP-IDF v5.5.3（`ESP_IDF_PATH`，缺省 `~/esp/esp-idf-v5.5.3`），产出 `build/FoloToy-AI-Passport-full.bin`，可从 0x0 整片刷写。脚本自身绝不烧录。镜像映射与 ZPF 解码器的主机测试在 `tools/validate.sh --static`。

## 部署

```bash
scripts/deploy_pet.sh       # 探测 /dev/cu.usb* → esptool 分段烧录（保 NVS）
```

Mac 侧（守护进程 + LaunchAgent）：`bash zcode-pet/scripts/deploy_pet_mac.sh`；首启会弹 macOS 蓝牙权限授权。

## 真机验证清单

- [ ] Boot 页标题 ~1.2s 后自动进 Pet 页。
- [ ] NimBLE sync 前：显示「蓝牙启动中」（`mid=12`）；人为制造 BLE 失败显示 `蓝牙启动失败(err=%d)`，UP/OK 重启栈恢复。
- [ ] 广播中未连接：「等待 Mac 连接…」+ 等待秒数；60s 后出现诊断文案（`mid=16`）。
- [ ] 守护进程在跑：文案/顶栏跟随任务状态；celebrate/error/余韵 clip 正常播放；帧循环无可见接缝。
- [ ] 退出 Mac 守护进程：~10s 内设备显示「Mac 掉线了」（看门狗 / `mid=14`）。
- [ ] 设备断电重启：Mac 侧数秒内自动重连（快间隔广播）。
- [ ] 所有文案中文渲染完整（无豆腐），数字走 fallback 字体。
- [ ] 上键开/关清单浮层；下键播摸头 clip；OK 发 `read`，桌面未读角标 ~2s 内回落。
- [ ] OK 长按进 About；电量行与电量计一致（或显示 `?`）；OK 重启 BLE 栈后链路自愈。
