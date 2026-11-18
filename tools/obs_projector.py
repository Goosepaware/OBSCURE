"""Open OBS windowed program projector and print its window title."""
from __future__ import annotations

import asyncio
import ctypes
import sys
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from obs_ws import connect

user32 = ctypes.windll.user32
EnumWindows = user32.EnumWindows
EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def titles() -> list[str]:
    out: list[str] = []

    @EnumWindowsProc
    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                out.append(buf.value)
        return True

    EnumWindows(cb, 0)
    return out


async def main() -> None:
    obs = await connect()
    try:
        print(
            await obs.request(
                "OpenVideoMixProjector",
                {
                    "videoMixType": "OBS_WEBSOCKET_VIDEO_MIX_TYPE_PROGRAM",
                    "monitorIndex": -1,
                },
            )
        )
    except Exception as exc:
        print("open error:", exc)
    await asyncio.sleep(2)
    for t in titles():
        if "OBS" in t or "投影" in t or "Projector" in t:
            print("WINDOW:", t)


if __name__ == "__main__":
    asyncio.run(main())
