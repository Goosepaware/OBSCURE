"""Save a full-size OBS source screenshot and crop the transport content."""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import cv2

sys.path.insert(0, str(Path(__file__).resolve().parent))
from obs_ws import connect

FULL = Path(r"D:\program\OBScure\out\phase1\live_full.png")
CROP = Path(r"D:\program\OBScure\out\phase1\live_content.png")
SOURCE = "OBScureTest Spout"


async def main() -> None:
    obs = await connect()
    await obs.request(
        "SaveSourceScreenshot",
        {
            "sourceName": SOURCE,
            "imageFilePath": str(FULL).replace("\\", "/"),
            "imageFormat": "png",
            "imageWidth": 1920,
            "imageHeight": 1080,
        },
    )
    im = cv2.imread(str(FULL))
    print("full:", im.shape if im is not None else None)
    if im is not None:
        cv2.imwrite(str(CROP), im[92 : 92 + 896, 160 : 160 + 1600])
        print("crop:", CROP)


if __name__ == "__main__":
    asyncio.run(main())
