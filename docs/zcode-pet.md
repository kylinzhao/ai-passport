<p align="right">
  <a href="zcode-pet.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# zcode-pet Desktop Pet Companion

A derivative application that mirrors the [zcode-pet](https://github.com/zhaoliang21/zcode-pet) macOS desktop pet onto this device. The Mac daemon stays the single authority for task counting and states; the device is a **state mirror + frame player** — it never counts tasks locally, so desktop and device semantics cannot drift.

## Data flow

```text
zcode-pet daemon (Mac)                     ESP32-C3 device (this firmware)
├─ PetDisplayState snapshot                ├─ zcp_bridge.c   NimBLE peripheral, always advertising
│  (mode/tier/run/unread/mid)             │   · GATT service 5A1C0001-…, TX notify / RX write
├─ DeviceBridge.swift (CoreBluetooth      │   · status → mirror + ACK, 10 s watchdog
│  central: scan → connect → subscribe)   │   · hello every 5 s; OK key → read notify
│  · status every 2 s (write-with-        ├─ zcp_pet.c      ZPF1 decode + 2× upscale → lv_image @ 8 fps
│    response, preferred MTU 256)         ├─ zcp_app.c      Boot/Pet/About pages, overlay, keys
│  · read message → desktop read path     └─ zcp_strings.c  closed-set text table
└─ --export-esp32 bakes ZPF1 clips (offline, committed to this repository)
```

State source: the daemon's `PetDisplayState` snapshot feeds both the desktop pet caption and the BLE `status` message, so the caption on the device and on the Mac are two projections of one source. The `mid` field doubles as the animation/caption selector on the device.

## Transport (BLE GATT)

The device is a NimBLE peripheral that advertises **all the time**; the Mac is the CoreBluetooth central. No pairing, no encryption, no whitelist — the payload is counters and closed-set caption ids only (a deliberate v1 trade-off recorded here). Advertising carries flags + the 128-bit service UUID; the complete device name `zcode-pet` lives in the scan response. After a disconnect the device re-advertises at a 60 ms fast interval for 30 s, then falls back to 320 ms — so a returning Mac reconnects in well under a second, while idle power stays reasonable.

## Protocol (single-line JSON, status ≤512 B)

| Message | Direction | GATT carrier | Payload |
| --- | --- | --- | --- |
| `hello` | device → Mac (notify every 5 s) | TX (≤64 B) | `{"v":1,"t":"hello","name":"zcode-pet","fw":"1.0.0"}` |
| `status` | Mac → device (write-with-response; every 2 s plus 0.25 s-throttled change pushes) | RX (≤512 B) | `{"v":1,"t":"status","seq":N,"mode":"idle\|working\|celebrate\|error","tier":0..3,"run":N,"unread":N,"mid":M,"n":opt,"ts":ms}` |
| `ack` | device → Mac (notify) | TX | `{"v":1,"t":"ack","seq":N}` |
| `read` | device → Mac (notify, OK key) | TX | `{"v":1,"t":"read"}` |

- UUIDs (identical constants on both sides; `check_esp32_bridge.sh [4/4]` byte-diffs them): Service `5A1C0001-7BCD-4E0F-9A2B-3D5E6F8091A2`, TX `5A1C0002-…`, RX `5A1C0003-…`.
- MTU: preferred MTU 256 (`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`), a single PDU carries the ~170 B status. If negotiation lands lower, CoreBluetooth automatically falls back to ATT long write and NimBLE reassembles — the protocol has no custom fragmentation.
- `mid` values: 1 sleep / 2 sulky / 3 drowsy / 4 resting / 5 long task / 6 focused / 7 N parallel / 8 N overloaded / 9 running / 10 done / 11 combo / 12 BLE starting / 13 waiting / 14 Mac lost / 15 error / 16 discovery diagnostics. 1-11 and 15 mirror the desktop captions (emoji-free projection); 12-14 and 16 are device-only strings.
- Link rules: TX notify subscribed = online; 10 s without any device packet (`hello`/`ack`/`read`) marks the link dead on both sides — the Mac cancels the connection and rescans (0/1/2/5 s backoff), the device returns to the waiting state (`mid=14`). A GATT disconnect re-advertises immediately; the Mac rescans automatically. Both sides self-heal without key presses.

## Keys

| Key | Page | Action |
| --- | --- | --- |
| UP click | Pet | Toggle the task-list overlay (`N tasks · M unread`, mirror data only) |
| UP click | Pet (BLE failed) | Restart the BLE stack |
| DOWN click | Pet | Local head-pat animation for 2 s (no counter semantics) |
| OK click | Pet (online) | Send `read` → desktop marks tasks read |
| OK click | Pet (not online) | BLE rescue: drop the GATT link, resume advertising |
| OK click | Pet (BLE failed) | Restart the BLE stack |
| OK long | Pet | Enter the About page |
| UP / DOWN / OK / OK long | About | Back to Pet / refresh battery / restart BLE stack / back to Pet |

The About page shows the device name + firmware, the CW2017 battery SOC (`?` when the fuel gauge is absent), and the key hints. There is no provisioning page: BLE needs no credentials. The Config page and `zcp_provision` from the UDP draft are gone.

## Animation frames (ZPF1)

All frames are baked offline by the desktop vector engine — no hand-drawn or AI-generated art. Twelve phase-locked loop clips (`ball-*.zpf`, 118×76 @ 8 fps, 348 frames, ≈0.32 MB) are embedded via `EMBED_FILES`, decoded and upsampled 2× into a static RGB565 buffer (236×152, ~70 KB internal RAM) rendered through an `lv_image` variable source. Format: `ZPF1` header, fixed 16-color RGB565 palette (index 0 transparent placeholder, index 15 = flatten background), per-frame `u32` length + 1bpp transparency-mask RLE + 4bpp palette indices. Semi-transparent pixels are flattened onto `#1A1A1A` (RGB565 `0x18C3`); the pet page background must use the same constant (`ZCP_BG_HEX`).

Known shape-level gaps (by design, semantics carried by captions): the impatient look reuses working frames with the "task has been running for a long time" caption; combo celebrations reuse the celebrate clip with the combo caption; the dangle pose has no device scenario.

## Text and fonts

Every on-screen string is a closed set with a single source: `zcode-pet/scripts/device_strings.txt`, two sections (`# sync:` desktop projections guarded by the zcode-pet sync assertion; `# device:` device-only UI text). `scripts/gen_device_assets.sh` regenerates `main/zcp_strings.c/.h`, `assets/fonts/zcp_font_16.c` (16 px subset, `lv_font_conv 1.5.3`, `--no-compress`, 163 codepoints; font: Noto Sans CJK SC Regular, SIL OFL 1.1) and `assets/fonts/zcp_font_16.codepoints.txt` — the registration file that `build_pet.sh` checks coverage against (string codepoints ⊆ registration ∪ printable ASCII, so a forgotten regeneration fails the build instead of rendering tofu). Digits in `%d` templates render through the `lv_font_montserrat_14` fallback. The identically named script in the zcode-pet repository now just delegates to this one.

## Build

```bash
scripts/build_pet.sh   # guards + tools/validate.sh --firmware + artifact paths
```

The script requires ESP-IDF v5.5.3 (`ESP_IDF_PATH`, default `~/esp/esp-idf-v5.5.3`) and produces `build/FoloToy-AI-Passport-full.bin`, flashable from offset 0x0. It never flashes by itself. Host tests for the mirror mapping and the ZPF decoder run inside `tools/validate.sh --static`.

## Deploy

```bash
scripts/deploy_pet.sh       # probe /dev/cu.usb* → esptool sectioned flash (keeps NVS)
```

Mac side (daemon + LaunchAgent): `bash zcode-pet/scripts/deploy_pet_mac.sh`; the first launch triggers the macOS Bluetooth permission prompt.

## Device verification checklist

- [ ] Boot page shows the title for ~1.2 s, then the Pet page.
- [ ] Before NimBLE sync: "BLE starting" (`mid=12`); a forced BLE failure shows `BLE failed (err=%d)` and UP/OK restarts the stack.
- [ ] Advertising without a Mac: "Waiting for Mac…" with waited seconds; after 60 s the diagnostic (`mid=16`) appears.
- [ ] With the daemon running: caption/top bar track task state; celebrate/error/error-aftermath clips play; pet frames loop without visible seams.
- [ ] Quit the Mac daemon: within ~10 s the device shows "Mac lost" (watchdog / `mid=14`).
- [ ] Device power-cycled: the Mac reconnects automatically within a few seconds (fast advertising).
- [ ] Every caption renders complete CJK glyphs (no tofu), digits render via the fallback font.
- [ ] UP opens/closes the list overlay; DOWN plays the pat clip; OK sends `read` and the desktop unread badge drops within ~2 s.
- [ ] OK long enters About; the battery row matches the fuel gauge (or shows `?`); OK restarts the BLE stack and the link recovers.
