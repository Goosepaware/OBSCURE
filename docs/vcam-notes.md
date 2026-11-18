# 虚拟摄像头实现说明

## 组成

| 部分 | 文件 | 作用 |
| --- | --- | --- |
| 发送端 | `src/live/vcam_sink.cpp` | 把 D3D11 输出纹理复制到共享内存 |
| 注册 | `src/live/vcam_registry.cpp` | 在 HKCU 写入 DirectShow 设备信息 |
| 滤镜 | `src/vcam/obscure_filter.cpp` | DirectShow 源滤镜，读取共享内存并输出视频帧 |
| 上游基础 | `third_party/UnityCapture/Source` | UnityCapture 的共享内存协议和 DirectShow 基类 |
| 构建 | `CMakeLists.txt` 的 `obscure-vcam` | 生成 `obscure-vcam.dll` |

## 设备信息

- 名称：`OBScure Virtual Camera`
- CLSID：`{5c2cd55c-92ad-4999-8666-912bd3e70010}`
- 注册位置：`HKCU\Software\Classes\CLSID\{...}\InprocServer32`
- DLL 路径：`<exe目录>\vcam\obscure-vcam.dll`
- 每用户注册，不需要管理员权限

## 共享内存协议

与 UnityCapture 兼容：

- Mutex：`UnityCapture_Mutx`
- 请求事件：`UnityCapture_Want`
- 发送事件：`UnityCapture_Sent`
- 映射：`UnityCapture_Data`

头部布局（36 字节）：

```
DWORD maxSize;
int width;
int height;
int stride;      // 像素数，不是字节 pitch
int format;      // 0 = RGBA8
int resizeMode;  // 0 = 关闭，1 = 线性缩放
int mirrorMode;  // 0 = 关闭
int timeout;     // 毫秒
```

## 输出格式

滤镜按以下顺序枚举：

1. **NV12**（首选，OBS 32 需要）
2. **YUY2**
3. RGB24

分辨率列表来自 `_media[]`，包含 1920×1080、1280×720 等。
发送端固定发送 1920×1080 RGBA，`resizeMode=1` 时由滤镜缩放到目标分辨率。

## 重新注册

GUI 启动编码并勾选虚拟摄像头时会自动调用 `vcam_install()`。
注册信息写在 HKCU，不需要管理员权限。

不要用 `regsvr32`：上游的 `DllRegisterServer` 会写 HKLM，并注册成
`Unity Video Capture`，和本项目的每用户注册混在一起。

## 验证

```bat
ffmpeg -list_devices true -f dshow -i dummy
ffmpeg -list_options true -f dshow -i "video=OBScure Virtual Camera"
ffmpeg -f dshow -video_size 1920x1080 -framerate 60 -pixel_format nv12 ^
  -i "video=OBScure Virtual Camera" -frames:v 120 test.mp4
```

已通过：1920×1080 / 60fps / NV12，1280×720 / 60fps / NV12。

OBS 32.0.2 实测：添加“视频采集设备”，设备 `OBScure Virtual Camera`，
格式 NV12、1920×1080、60fps，`videoActive=true`，画面方向正确。
OBS 自己的虚拟摄像头也已注册，可作备用。

Spout2 插件注意：OBS 32.0.2 只能用 **1.10.0**；1.12.0 起要求 OBS 32.1+。

## 已知注意点

- 滤镜只输出 NV12、YUY2、RGB24，不再输出 ARGB32。
- OBS 32 如果仍然报 `0x80040207`，先检查是否选到了 RGB；把格式改成 NV12。
- 修改滤镜后需要重新编译 `obscure-vcam`，并确保 `build\vcam\obscure-vcam.dll` 已更新。
- 已经被应用加载的 DLL 不会被覆盖，测试前先关闭 OBS/ffmpeg。
