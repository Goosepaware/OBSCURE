"""Create an OBS dshow source for OBScure Virtual Camera and screenshot it."""
from __future__ import annotations

import asyncio
import base64
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from obs_ws import connect

SCENE = "OBScureVcamTest"
INPUT = "OBScure Virtual Camera"
OUT = Path(r"D:\program\OBScure\out\phase1\obs_vcam.png")


async def pick(obs, input_name: str, prop: str, wanted: str) -> None:
    try:
        items = await obs.request(
            "GetInputPropertiesListPropertyItems",
            {"inputName": input_name, "propertyName": prop},
        )
    except Exception as exc:
        print(f"{prop}: query failed: {exc}")
        return
    for item in items.get("propertyItems", []):
        name = str(item.get("itemName", ""))
        value = item.get("itemValue")
        if wanted.lower() in name.lower():
            print(f"{prop}: choose {name!r} -> {value}")
            await obs.request(
                "SetInputSettings",
                {"inputName": input_name, "inputSettings": {prop: value}},
            )
            return
    print(f"{prop}: no match for {wanted!r}; items:")
    for item in items.get("propertyItems", [])[:30]:
        print("  ", item.get("itemName"), item.get("itemValue"))


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

    settings = {
        "active": True,
    }
    await obs.request(
        "CreateInput",
        {
            "sceneName": SCENE,
            "inputName": INPUT,
            "inputKind": "dshow_input",
            "inputSettings": settings,
            "sceneItemEnabled": True,
        },
    )
    await pick(obs, INPUT, "video_device_id", "OBScure")
    await obs.request(
        "SetInputSettings",
        {
            "inputName": INPUT,
            "inputSettings": {"res_type": 1, "resolution": "1920x1080"},
        },
    )
    await pick(obs, INPUT, "frame_interval", "60")
    await pick(obs, INPUT, "video_format", "NV12")

    final = await obs.request("GetInputSettings", {"inputName": INPUT})
    print("settings:", final.get("inputSettings"))

    shot = await obs.request(
        "GetSourceScreenshot",
        {
            "sourceName": INPUT,
            "imageFormat": "png",
            "imageWidth": 1920,
            "imageHeight": 1080,
            "compressionQuality": 90,
        },
    )
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(base64.b64decode(shot["imageData"]))
    print("saved", OUT)


if __name__ == "__main__":
    asyncio.run(main())
