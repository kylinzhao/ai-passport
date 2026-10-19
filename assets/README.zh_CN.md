<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

<!-- zcode-pet device assets:begin -->
### zcode-pet 设备资产（由 `scripts/gen_device_assets.sh` 生成，勿手改）

| 文件 | 内容与来源 |
| --- | --- |
| [`../main/assets/pet/`](../main/assets/pet)（12 个 `.zpf`） | 由 zcode-pet 仓库的桌面宠物矢量引擎离线烘焙（`--export-esp32`）的动画帧包；ZPF1 格式：16 色 RGB565 固定调色板 + 1bpp 透明掩码 RLE + 4bpp 索引，118×76 @ 8fps，设备端 2× 上采样；零手绘贴图 |
| [`../main/zcp_strings.c`](../main/zcp_strings.c) | 闭集中文文案表（sync 区 mid + 设备区 mid 与具名符号），生成自 `zcode-pet/scripts/device_strings.txt` 两节全文 |
| [`fonts/zcp_font_16.c`](fonts/zcp_font_16.c) | 16px 中文子集字体；`lv_font_conv` 1.5.3（`--size 16 --bpp 4 --format lvgl --no-compress --lv-font-name zcp_font_16 --lv-include lvgl.h`）；码点 = device_strings.txt 两节全文 + ASCII 可打印集（共 163 码点）；字体源：Noto Sans CJK SC Regular（思源黑体同源，SIL OFL 1.1，允许再分发）；经 `main/CMakeLists.txt` 的 `target_sources` 引用，运行期数字模板由 `lv_font_montserrat_14` fallback 渲染 |

| [`fonts/zcp_font_16.codepoints.txt`](fonts/zcp_font_16.codepoints.txt) | Registered codepoint set of `zcp_font_16.c` (registration baseline for the `build_pet.sh` coverage check: string codepoints ⊆ registration ∪ printable ASCII); regenerated together with the font by `scripts/gen_device_assets.sh` |

- 宠物画布底色常量：`#1A1A1A`（RGB565 `0x18C3`）——半透明像素烘焙时向它压平，宠物页底色必须同色（见 `main/zcp_mirror.h` 的 `ZCP_BG_HEX`）。
- 新增/改动文案：改 `zcode-pet/scripts/device_strings.txt` 后重跑 `scripts/gen_device_assets.sh`；字符白名单与桌面同步断言在 zcode-pet 仓库的 `scripts/check_esp32_bridge.sh`。
- 再生成依赖：macOS `swiftc`、`lv_font_conv`（`npm install -g lv_font_conv`）、经 `FONT_TTF` 指定的中文 TTF（默认 `~/.zcode-pet/fonts/NotoSansCJKsc-Regular.otf`）。
<!-- zcode-pet device assets:end -->