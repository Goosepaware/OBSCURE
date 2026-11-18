# OBScure 阶段 1 报告：C++ 核心、文件闭环、实时编码/解码

日期：2026-11-18  
目标：把 Phase 0 的 Python 算法落成 C++/D3D11 可执行程序，先跑通文件闭环，再跑通 Spout → OBS 和窗口捕获解码。

## 1. 已完成

### 1.1 C++ 核心

- 布局、密钥（SHA-256 + mt19937_64）、置换、二面体、颜色变换、元数据/CRC、ArUco 边框全部移植到 C++。
- 自检 `obscure-cli.exe selftest`：
  - `identity_exact=true`
  - 四角标记 4/4
  - 元数据 keyID 校验通过
  - 8 种二面体/颜色变换往返正确
- 文件闭环：
  - CPU 编码 360 帧 1080p60 → 约 24 fps（离线，单线程 OpenCV）
  - CPU 解码 360 帧 → 约 29 fps
  - 输出 1400×784 还原视频，画面/颜色正确

### 1.2 实时 GPU 编码

`obscure-live.exe`：

- WGC 捕获窗口/显示器，或 `--file` 读取视频
- D3D11 + HLSL `shaders/transport.hlsl`，单 pass 完成降采样 + 分块置换 + 二面体 + 颜色 + 边框合成
- Spout2 发送 1920×1080 BGRA 纹理
- 实测：
  - 渲染 60 fps
  - 捕获 60 fps（显示器）
  - Spout 初始化成功
  - OBS 32.0.2 + Spout2 1.10.0 能创建 Spout2 Capture 并截图验证
- Spout 连通性截图：`out/phase1/obs_spout_test.png`
- GPU 编码输出截图/内容：`out/phase1/live_full.png`、`live_content.png`

### 1.3 实时 GPU 解码

`obscure-decode.exe`：

- 文件、窗口（WGC）、显示器三种输入
- 四角 ArUco 定位 + 单应；检测在独立工作线程，1 Hz，不阻塞预览
- HLSL `shaders/decode.hlsl` 逆分块/逆颜色，直接输出 1400×784
- 自适应 margin：1080p 0，720p 1
- H 接近初始缩放时自动吸附，减少全窗口播放器的块边缘抖动
- Win32 预览窗口；无参数时打开文件选择框
- 实测：
  - 文件 1080p：60 fps，无检测模式画面干净
  - 文件 720p：60 fps，margin=1
  - 窗口捕获 1920×1080：60 fps，四角 4/4，keyID 通过
  - 1 Hz 检测耗时约 140–290 ms（后台线程）

样例：

- `out/phase1/gpu_file_clean/restored_60.png`（1080p 文件）
- `out/phase1/gpu_file_720/restored_60.png`（720p，margin=1）
- `out/phase1/live_player_dump/restored_60.png`（窗口捕获路径）

## 2. 本阶段修掉的关键 bug

1. **C++ 颜色码 4/6 通道交换错误**  
   BGR 中 code 4 应为 R/G 交换（ch1/ch2），code 6 为 G/B 交换（ch0/ch1）；最初写反，导致 GPU 编码 + CPU 解码出现块状偏色。

2. **HLSL 颜色码 2 写成了三循环**  
   `c.brg` 是旋转，不是 R/B 交换；改为 `c.bgr`。

3. **源表解码按字节拆 x/y**  
   `g_srcFromDst` 存的是线性索引 `y*inner+x`，不能按 `x|y<<8` 解；改为 `s % inner` / `s / inner`。

4. **解码 Shader 漏加内容区偏移**  
   canonical 坐标必须加 `contentX/contentY`，否则会采样到边框标签/校准条，输出像“乱码 + 文字碎片”。

5. **检测阻塞渲染**  
   原来在渲染线程每 0.5s 跑一次全分辨率检测，掉到 12–45 fps；改为工作线程 + 1 Hz 提交，预览恢复到 60 fps。

6. **检测单应亚像素误差**  
   对接近“播放器刚好铺满窗口”的场景，单应和初始缩放差异 <2px 时吸附到初始缩放，减少块边缘混叠。

## 3. 当前画面质量

- 8 Mbps 1080p 还原图仍有可见的块边缘压缩纹（保护边 + H.264 去块/量化导致）；10 Mbps 明显更干净。
- 文件 1080p 无检测模式的几何/颜色正确，边缘纹主要是源传输帧的压缩痕迹，不是解码错位。
- 720p 输入使用 margin=1 后，块网格明显减少。
- 16px 块隐藏更强，但 6–10 Mbps 下块噪声大，不建议默认。

## 4. 实时窗口捕获的注意事项

- 解码端窗口捕获已验证“WGC 捕获 → 四角定位 → 逆变换 → 60fps 预览”的完整流程。
- 浏览器硬件覆盖层/部分播放器窗口可能被 WGC 捕获为黑屏；优先使用：
  - 全屏播放器窗口（非浏览器覆盖层）
  - OBS 窗口投影
  - `--monitor` + 第二显示器
- 自制 `transport-player.exe` 在当前 DWM 环境下会偏透明/发灰，属于测试播放器问题；正式播放器窗口需实测。
- OBS 32.0.2 + Spout2 1.10.0 可用；1.13+ 要求 OBS 32.1/33。发送端频繁上下线时旧插件可能不稳定，正式使用保持编码端常驻或升级 OBS。

## 5. 性能

| 项目 | 结果 |
|---|---|
| GPU 编码渲染 | 60 fps |
| 显示器/窗口捕获 | 60 fps（正常 D3D/窗口） |
| Spout 发送 | 60 fps，OBS 可接收 |
| GPU 解码预览 | 60 fps |
| 解码检测 | 1 Hz，140–290 ms/次，后台线程 |
| CPU 文件编码 | 约 24 fps（离线参考实现） |
| CPU 文件解码 | 约 29 fps（离线参考实现） |

CPU/GPU 占用百分比尚未用专用工具（PresentMon/GPUView）精确统计，后续补。

## 6. 下一步

1. 统一 GUI/安装包（编码源选择、密钥、档位、Spout 状态；解码全屏/置顶）。
2. 平台转码全矩阵：1080p→720p、不同 CBR/VBR、二次编码。
3. OBS 33 + 最新 Spout2 插件适配；可选 Media Foundation 虚拟摄像头。
4. 检测线程/单应精度继续优化；考虑 GPU 角点检测。
5. 真实播放器（VLC/MPV/OBS 投影/平台网页全屏）复测窗口捕获。
