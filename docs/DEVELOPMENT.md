# 开发、验证与诊断

[返回首页](../README.md)

## 工具链

固定基线为 ESP-IDF v5.5.3 / ESP32-C3。激活官方 ESP-IDF 环境后，从仓库根目录执行 `idf.py build`。不依赖父仓库、外部业务组件或本机绝对路径。

`idf.py menuconfig` → `IINE L1241 gamepad demo`：

| 配置 | 默认 | 含义 |
| --- | --- | --- |
| `IINE_PEER_NAME` | `Xbox Wireless Controller` | 附加精确名称；Controller / Gamepad / IINE 关键词及广播 HID 服务仍参与筛选 |
| `IINE_PEER_ADDRESS` | 空 | 可选 BLE 地址过滤；随机地址可能变化，建议默认留空 |
| `IINE_LOG_RAW` | 开 | 输出变化的原始输入；解析失败时保留诊断 |

默认只连接用户在屏幕选择的设备。最多 16 项，可翻页；合并同一地址的广播和扫描响应。名字仅用于筛选，不能证明兼容。

## 可选构建脚本

已激活 ESP-IDF 时：

```sh
python build.py
python build.py --menuconfig
python build.py --port COM3 --flash --monitor
```

未使用官方终端时，使用已经安装 ESP-IDF Python 依赖的环境，并显式传入 SDK 和工具目录：

```powershell
python build.py --idf C:/Espressif/esp-idf-v5.5.3 --tools C:/Espressif/tools
```

`--flash` 使用 `app-flash`；默认只构建，烧录和监控都要求显式 `--port`。只设置子进程环境，不修改系统 PATH。它不自动安装 ESP-IDF 依赖、不自动探测设备分区。请先阅读 [烧录说明](HARDWARE.md#烧录与分区)。

## 主机测试

Linux / WSL 安装 GCC 和 Python 3 后运行：

```sh
python3 tools/test_host.py
```

| 测试 | 覆盖 |
| --- | --- |
| `test_hid_input` | 多 Report ID、符号位、非对齐字段、释放、方向空状态、Push/Pop、截断、20,000 个随机描述符 |
| `test_iine_protocol` | 已知帧、CRC、长度、最大帧、请求白名单、增量状态、组合事件、原子更新、358 条实机输入 |
| `test_gamepad_ui` | 选择 / 取消配对、容量、轴归一化、断线清零、UTF-8、页面切换、按下提示、条带越界与整帧一致性 |

编译选项包含 `-Wall -Wextra -Werror -fsanitize=address,undefined`。产物位于忽略目录 `build/host/`。CI 同时运行这些测试和 ESP-IDF 构建；成功的构建提供应用镜像 artifact，不含合并 Flash 镜像。

## 字体与预览

只有修改 UI 中文或生成预览时才需要额外 Python 工具。请使用项目虚拟环境：

```sh
python3 -m venv .venv
# Linux / WSL
. .venv/bin/activate
# Windows PowerShell 则使用 .venv\Scripts\Activate.ps1
python -m pip install -r requirements-tools.txt
python tools/fetch_font.py
python tools/generate_font.py
python tools/test_host.py --preview
python tools/make_preview.py
```

`test_host.py` 使用 GCC + sanitizer，建议在 Linux / WSL 执行；Windows 也可在 WSL 生成 PPM，再用 Windows 虚拟环境生成 PNG。

字体下载固定 upstream commit 和 SHA-256，缓存到 `build/`。生成的 `main/ui_font.inc` 已提交，普通编译不下载字体。完整 OTF 不进入仓库。字体来源及许可见 [第三方说明](../THIRD_PARTY_NOTICES.md)。

图片使用和设备一致的 C 渲染器，但输入为模拟数据，不能替代真机测试。

首页封面与动图只展示界面页面，不添加设备外壳。Linux / WSL 中先生成页面和 64 帧模拟输入，再用上述虚拟环境生成图片：

```sh
python tools/fetch_font.py
gcc -std=c11 -Wall -Wextra -Werror -I main \
    main/gamepad_ui.c main/gamepad_render.c tests/render_preview.c \
    -o build/render_preview
build/render_preview build
gcc -std=c11 -Wall -Wextra -Werror -I main \
    main/gamepad_ui.c main/gamepad_render.c tools/render_readme.c \
    -lm -o build/render_readme
build/render_readme build
python tools/readme_art.py
```

输出位于 `docs/assets/`，包括封面、动图及静态替代图；字体与逐帧 PPM 留在被忽略的 `build/` 中，可在生成后删除。生成过程不更改固件代码。

## 串口与状态

优先使用 `idf.py -p COM3 monitor`。仅采集日志、避免主动切换 DTR/RTS 时：

```sh
python tools/serial_capture.py --port COM3 --seconds 60
```

该辅助脚本需要 `pyserial`。使用 `--output path` 保存日志；公开反馈前应删除完整 BLE 地址和周边设备信息。不要公开 NVS 镜像或绑定密钥。

IINE 路径关注 `IINE identity`、`measured input decoder enabled`、`RAW` 和 `INPUT`。标准 HID 路径关注 `Report Map`、`Subscribed input report`、`READY`；普通 Button Usage 是编号，不能直接当成 A/B/X/Y。

USB 输出使用队列，不阻塞蓝牙回调；积压时可能丢日志，屏幕也按刷新帧显示最新状态。这不是每个高速边沿都无损记录的采集器。

## 故障排查

| 现象 | 排查方向 |
| --- | --- |
| 没有 `IINE GAMEPAD` | 重新配对 + A；检查电量和其他主机抢连；尝试列表的“查看所有蓝牙设备” |
| 配对或加密失败 | 取消此设备配对后重试；固件使用 No-IO Just Works，无密码输入界面 |
| 私有协议未识别 | 查看型号、身份回复与固件版本；不强行套用 L1241 布局 |
| 已连接但无输入 | 配置通道可能存在而测试输入尚未开启；检查握手、测试页维持和接收计数 |
| 标准 HID `RAW ONLY` / `skipped` | 描述符含当前不支持的字段；原始报文可见不代表所有控件已解析 |
| 睡眠或掉线 | 屏幕清除输入并返回列表；唤醒后选中重连 |
| LCD 黑屏或花屏 | 核对引脚、面板型号、复位电路、SPI 时钟、背光及串口启动错误 |
| 工牌按键错误 | 核对 GPIO0 分压电路、ADC 校准与电压阈值 |

## 已完成验证与边界

2026-10-12，0.3.1 在 8 MB ESP32-C3 工牌上完成 L1241 输入验证和用户验收：双摇杆、12 按键、十字键、组合 / 松开、数据页切换、断开重连均正常。现场反馈：“显示和重连都正常”。

公开仓库整理时保留了这套 BLE 与输入映射逻辑，移除了本机路径，并将字体替换为可分发的 Noto 点阵子集。公开字体版另做主机测试、预览检查和重新编译；没有将换字体后的镜像再次烧入已验收设备。不能将原设备验收等同于每一块开发板、每个固件版本均兼容。

标准 HID 路径支持首个 HID 服务、最多 24 个特征、1024 字节 Report Map、64 字节输入和 96 个字段。不支持的数组、相对输入和厂商字段保留原始诊断。只允许一个手柄连接。
