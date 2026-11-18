"""Write a static RGBA orientation pattern into the UnityCapture shared memory."""
from __future__ import annotations

import ctypes
import struct
import time
from ctypes import wintypes

import numpy as np

k32 = ctypes.windll.kernel32
k32.OpenMutexW.restype = wintypes.HANDLE
k32.CreateEventW.restype = wintypes.HANDLE
k32.OpenEventW.restype = wintypes.HANDLE
k32.OpenFileMappingW.restype = wintypes.HANDLE
k32.MapViewOfFile.restype = ctypes.c_void_p

SYNCHRONIZE = 0x00100000
EVENT_MODIFY_STATE = 0x0002
FILE_MAP_WRITE = 0x0002

W, H = 1920, 1080
mutex = k32.OpenMutexW(SYNCHRONIZE, False, "UnityCapture_Mutx")
if not mutex:
    raise SystemExit("mutex not found")
want = k32.CreateEventW(None, False, False, "UnityCapture_Want")
sent = None
mapping = None
for _ in range(100):
    sent = k32.OpenEventW(EVENT_MODIFY_STATE, False, "UnityCapture_Sent")
    mapping = k32.OpenFileMappingW(FILE_MAP_WRITE, False, "UnityCapture_Data")
    if sent and mapping:
        break
    time.sleep(0.05)
if not sent or not mapping:
    raise SystemExit("events/mapping not found")
view = k32.MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0)
if not view:
    raise SystemExit("MapViewOfFile failed")

img = np.zeros((H, W, 4), dtype=np.uint8)
img[:, :, 3] = 255
# Distinct quadrants: TL red, TR green, BL blue, BR white.
img[: H // 2, : W // 2] = (255, 0, 0, 255)
img[: H // 2, W // 2 :] = (0, 255, 0, 255)
img[H // 2 :, : W // 2] = (0, 0, 255, 255)
img[H // 2 :, W // 2 :] = (255, 255, 255, 255)
raw = img.tobytes()

for i in range(180):
    k32.WaitForSingleObject(mutex, 1000)
    header = struct.pack("<Iiiiiiii", 66355200, W, H, W, 0, 0, 0, 1000)
    ctypes.memmove(view, header, len(header))
    ctypes.memmove(view + 32, raw, len(raw))
    k32.ReleaseMutex(mutex)
    k32.SetEvent(sent)
    k32.WaitForSingleObject(want, 0)
    time.sleep(1 / 30)
print("pattern sent")
