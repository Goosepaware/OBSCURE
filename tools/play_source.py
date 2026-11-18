"""Loop the sample clip in a normal Win32 window for WGC/window-capture tests."""
from __future__ import annotations

import sys
import time
from pathlib import Path

import cv2

SRC = Path(r"D:\program\OBScure\sample\Counter-strike 2 2026.07.14 - 04.32.40.12.DVR_2.mp4")
WINDOW = "OBScure Source"


def main() -> None:
    cap = cv2.VideoCapture(str(SRC))
    if not cap.isOpened():
        raise SystemExit("cannot open sample")
    cv2.namedWindow(WINDOW, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(WINDOW, 1280, 720)
    cv2.moveWindow(WINDOW, 80, 80)
    while True:
        ok, frame = cap.read()
        if not ok:
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            continue
        cv2.imshow(WINDOW, frame)
        key = cv2.waitKey(16) & 0xFF
        if key == 27:
            break
    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
