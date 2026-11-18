# OBScure 项目规则

先遵守全局规则：`C:\Users\34887\.config\opencode\AGENTS.md`。

## 绝对禁止

- 禁止对 `D:\`、`D:\program`、`D:\program\OBScure` 使用任何递归删除。
- 禁止 `rmdir /s /q`、`rd /s /q`、`Remove-Item -Recurse`。
- 禁止把 `D:\` 设为工作目录。
- 禁止为了“清理”而删除 `third_party`、`src`、`docs`、`shaders`、`work`。

## 允许的清理方式

- 构建产物：只清理 `D:\program\OBScure\build` 内的中间文件，逐项、按扩展名。
- 其他文件：移动到 `D:\_trash\<时间戳>\`，不要删除。

## 构建

```bat
work\build_cli.bat
```

产物在 `build\`。FFmpeg 是 shared 构建，`ffmpeg.exe` 必须和
`avcodec-63.dll` 等 DLL 放在同一目录。

## 当前重点

- OBS 32.0.2 安装在 `D:\obs-studio`
- 虚拟摄像头滤镜源码在 `src\vcam\`，发送端在 `src\live\vcam_sink.cpp`
- 滤镜输出优先 NV12，其次 YUY2，最后 RGB32
