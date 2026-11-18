# OBScure

实时画面混淆 / 解混淆工具。编码端捕获窗口、显示器或视频文件，把画面中心切成带密钥的碎片并加上四角定位点和元数据；解码端捕获播放器窗口或文件，识别定位点后还原画面。

当前是 **v0.2 可测试版**：统一 GUI、文件闭环、实时 GPU 编码/解码、窗口选择与实时预览、输出窗口、Spout 已跑通。独立虚拟摄像头已支持 NV12/YUY2，1080p60 采集验证通过。

## 图形界面（推荐）

```bat
build\obscure-gui.exe
```

一个窗口，顶部切换 **编码 / 解码**。

### 编码模式

1. 左侧选择输入源：
   - **窗口**：点“选择窗口...”，列表里选目标窗口，右侧有实时预览。
   - **显示器**：下拉选择。
   - **视频文件**：选择源视频，循环播放。
2. 选择输出：
   - **输出窗口**：默认开启。精确 1920×1080 无边框，默认置于底层。
   - **Spout**：需要 OBS 的 Spout2 插件；发送名默认 `OBScure`。
   - **录制到文件**：把传输帧写成 H.264 MP4，路径可选。
3. 选择画质档位、填写共享密钥，点“开始混淆”。
4. 预览上方可切换“显示混淆输出”，确认四角标记和元数据。

### 解码模式

1. 选择输入源：文件 / 窗口 / 显示器。
2. 文件模式默认按“精确传输帧”解码；如果输入是播放器录屏（有缩放/黑边），勾选“播放器录屏（启用定位）”。
3. 点“开始解码”，预览显示还原画面；底部显示锁定、四角数量、keyID、margin、检测耗时。
4. 可勾选“独立预览窗口”，用于全屏/副屏播放。

## OBS 接入

### 输出窗口（零插件，推荐）

1. 编码端开启“输出窗口”，点“开始混淆”。
2. OBS 添加 **窗口采集**，选择 `OBScure Output 1920x1080`。
3. 采集方法选 **Windows 10 (1903 and newer)**（WGC）。输出窗口被 OBS 挡住也能采集。
4. 不要在传输帧上叠加摄像头、弹幕、贴纸，否则会遮挡定位点或污染内容。

### Spout（需要插件）

1. 安装 Spout2 插件（本机 OBS 32.0.2 已验证 1.10.0）。
2. 编码端勾选 Spout，填写发送名。
3. OBS 添加 **Spout2 Capture**，选择同名发送端。

### 虚拟摄像头

当前状态：`OBScure Virtual Camera` 已注册为每用户 DirectShow 设备，优先输出 **NV12**，其次 YUY2，最后 RGB24。ffmpeg 以 1920×1080 / 60fps / NV12 采集已通过。

临时替代：
- 在 OBS 里添加“视频采集设备”，选择 **OBScure Virtual Camera**，分辨率 1920×1080、帧率 60、格式 NV12。
- 也可以用“输出窗口”让 OBS 采集，再启动 OBS 自带的虚拟摄像头。

## 命令行（保留）

```bat
build\obscure-cli.exe encode ^
  --input source.mp4 --output transport.mp4 ^
  --config c32_g2_s120 --bitrate 8

build\obscure-cli.exe decode ^
  --input transport.mp4 --output restored.mp4 ^
  --config c32_g2_s120
```

- 编码输出：1920×1080、60fps、H.264 CBR 传输帧
- 解码输出：1400×784 还原画面
- CLI 仍建议使用英文路径；GUI 已处理中文/空格路径

## 参数与画质

传输框：

- 画布 1920×1080
- 混淆区 1600×896（50×28 个 32px 块，保护边 2px）
- 四角 ArUco 4×4_50 标记，64×64
- 上下元数据条、左右校准条
- 有效还原分辨率 1400×784（约 784p）

推荐档位：

| 档位 | 配置 | 码率 | 说明 |
|---|---|---|---|
| 高质量 | `c32_g2_s100` | 10 Mbps | 最清晰，低码率下块噪声更明显 |
| **默认** | **`c32_g2_s120`** | **8 Mbps** | 画质/隐藏平衡 |
| 隐私/低码率 | `c32_g2_s140` | 6 Mbps | 更软，文字碎片更少 |
| 强隐私 | `c16_g2_s100` | 10 Mbps | 隐藏最强，块噪声更明显 |

720p 源解码时会自动内缩 1px，避免保护边混叠。

## 构建

依赖：

- Windows 11 + Windows SDK 10.0.26100
- Visual Studio 2022 Build Tools（MSVC v143，`/MT`）
- CMake + Ninja（VS Build Tools 自带）
- OpenCV 4.10 静态库（源码在 `third_party/opencv`，安装到 `third_party/opencv-install`）
- Spout2 SDK（`third_party/Spout2`）
- ImGui（`third_party/imgui`）
- FFmpeg CLI（`ffmpeg.exe`）

构建：

```bat
work\build_cli.bat
```

产物：

- `build\obscure-gui.exe`：统一 GUI
- `build\obscure-cli.exe`：文件编码/解码 CLI
- `build\obscure-decode.exe`：旧版 GPU 解码预览
- `build\obscure-live.exe`：旧版实时编码 → Spout
- `build\obscure-vcam.dll`：DirectShow 虚拟摄像头滤镜（NV12/YUY2/RGB）

## 已知限制

- 混淆不是密码学加密：防肉眼、截图、OCR 和普通识别；不承诺对抗掌握原片、能做块匹配的攻击者。
- 8 Mbps 下还原画面仍有块边缘压缩纹；10 Mbps 明显更干净，这是 H.264 在分块边界上的固有限制。
- 16px 块在 6–10 Mbps 下块噪声偏大，不建议作为默认。
- 浏览器/硬件覆盖层窗口可能被 WGC 捕获为黑屏；优先用普通播放器窗口、OBS 投影或 `--monitor`。
- 虚拟摄像头已通过 ffmpeg 1080p60 NV12 采集验证，OBS 32 待重装后验证。
- v1 只支持 SDR；HDR 需 tonemap；DRM 窗口可能捕获黑屏。
- 还没有安装包；当前是本地测试版。

## 目录

```text
src/core/       布局、密钥、元数据、边框、CPU 变换、ArUco 检测
src/live/       WGC 捕获、D3D11 管线、输出窗口、Spout、虚拟摄像头发送端
src/gui/        ImGui + Win32 + D3D11 统一界面
src/vcam/       独立 DirectShow 虚拟摄像头滤镜（NV12/YUY2/RGB）
shaders/        transport.hlsl、decode.hlsl、blit.hlsl
tools/proto/    Phase 0 Python 原型与码率测试
tools/          OBS WebSocket / 测试脚本
docs/           Phase 0/1/2 报告与计划
out/            测试产物
```

## 后续

1. 用 OBS 32 验证 OBScure Virtual Camera 的 NV12/60fps 连接。
2. 统一 GUI 的设置持久化、日志/崩溃提示、全屏预览细节。
3. 安装包与便携版。
4. 平台转码/二次编码全矩阵，再调画质与码率。
