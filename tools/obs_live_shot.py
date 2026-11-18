"""Save the current OBS Spout source screenshot."""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from obs_ws import connect

OUT = Path(r"D:\program\OBScure\out\phase1\live_obs_shot.png")
SOURCE = "OBScureTest Spout"


async def main() -> None:
    obs = await connect()
    scenes = await obs.request("GetSceneList")
    print("program:", scenes.get("currentProgramSceneName"))
    print("source settings:", await obs.request("GetInputSettings", {"inputName": SOURCE}))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    await obs.request(
        "SaveSourceScreenshot",
        {
            "sourceName": SOURCE,
            "imageFilePath": str(OUT).replace("\\", "/"),
            "imageFormat": "png",
            "imageWidth": 960,
            "imageHeight": 540,
        },
    )
    print("saved:", OUT, OUT.stat().st_size if OUT.exists() else "missing")


if __name__ == "__main__":
    asyncio.run(main())
