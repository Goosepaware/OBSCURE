"""Create a temporary Spout2 source in OBS and capture a screenshot."""
from __future__ import annotations

import asyncio
import base64
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from obs_ws import connect

OUT = Path(r"D:\program\OBScure\out\phase1\obs_spout_test.png")
SCENE = "OBScureTestScene"
INPUT = "OBScureTest Spout"
SENDER = "OBScureTest"


async def main() -> None:
    obs = await connect()
    scenes = await obs.request("GetSceneList")
    names = [s["sceneName"] for s in scenes["scenes"]]
    print("existing scenes:", names)
    if SCENE in names:
        await obs.request("RemoveScene", {"sceneName": SCENE})
    await obs.request("CreateScene", {"sceneName": SCENE})

    try:
        await obs.request("RemoveInput", {"inputName": INPUT})
    except Exception:
        pass

    await obs.request(
        "CreateInput",
        {
            "sceneName": SCENE,
            "inputName": INPUT,
            "inputKind": "spout_capture",
            "inputSettings": {"spoutsenders": SENDER, "tickspeedlimit": 100},
            "sceneItemEnabled": True,
        },
    )
    await obs.request("SetCurrentProgramScene", {"sceneName": SCENE})
    await asyncio.sleep(1.5)
    print("input settings:", await obs.request("GetInputSettings", {"inputName": INPUT}))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    await obs.request(
        "SaveSourceScreenshot",
        {
            "sourceName": INPUT,
            "imageFilePath": str(OUT).replace("\\", "/"),
            "imageFormat": "png",
            "imageWidth": 960,
            "imageHeight": 540,
        },
    )
    print("screenshot:", OUT, OUT.stat().st_size if OUT.exists() else "missing")

    # leave the scene/input in place for manual inspection; the next run removes them


if __name__ == "__main__":
    asyncio.run(main())
