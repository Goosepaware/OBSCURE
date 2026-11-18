"""Create an OBS Window Capture source for the OBScure output window and screenshot it."""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from obs_ws import connect

SCENE = "OBScureWindowTest"
INPUT = "OBScure Output Window"
TITLE = "OBScure Output 1920x1080"
CLASS = "OBScureOutputWindow"
EXE = "obscure-gui.exe"
OUT = Path(r"D:\program\OBScure\out\phase1\obs_window_capture.png")


async def main() -> None:
    obs = await connect()
    scenes = await obs.request("GetSceneList")
    names = [s["sceneName"] for s in scenes["scenes"]]
    if SCENE not in names:
        await obs.request("CreateScene", {"sceneName": SCENE})
    try:
        await obs.request("RemoveInput", {"inputName": INPUT})
    except Exception:
        pass

    defaults = await obs.request("GetInputDefaultSettings", {"inputKind": "window_capture"})
    print("defaults:", defaults)
    await obs.request(
        "CreateInput",
        {
            "sceneName": SCENE,
            "inputName": INPUT,
            "inputKind": "window_capture",
            "inputSettings": {},
            "sceneItemEnabled": True,
        },
    )
    props = await obs.request(
        "GetInputPropertiesListPropertyItems",
        {"inputName": INPUT, "propertyName": "window"},
    )
    items = props.get("propertyItems", [])
    print("window items:")
    for item in items:
        print(" ", item)
    chosen = None
    for item in items:
        value = str(item.get("itemValue", ""))
        if TITLE in value or CLASS in value:
            chosen = value
            break
    if chosen is None:
        print("matching window not found")
        return
    print("chosen:", chosen)
    await obs.request(
        "SetInputSettings",
        {
            "inputName": INPUT,
            "inputSettings": {"window": chosen, "method": 2, "cursor": False},
            "overlay": True,
        },
    )
    await obs.request("SetCurrentProgramScene", {"sceneName": SCENE})
    await asyncio.sleep(1.5)
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


if __name__ == "__main__":
    asyncio.run(main())
