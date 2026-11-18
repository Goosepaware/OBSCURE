"""Tiny OBS WebSocket 5.x helper for automated Spout source testing."""
from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import sys
import uuid

import websockets

URL = "ws://127.0.0.1:4455"
PASSWORD = "RxOzdI06zs8cuinL"


class Obs:
    def __init__(self, ws):
        self.ws = ws

    async def request(self, request_type: str, data: dict | None = None) -> dict:
        rid = uuid.uuid4().hex
        await self.ws.send(
            json.dumps(
                {
                    "op": 6,
                    "d": {
                        "requestType": request_type,
                        "requestId": rid,
                        "requestData": data or {},
                    },
                }
            )
        )
        while True:
            msg = json.loads(await self.ws.recv())
            if msg.get("op") != 7:
                continue
            if msg["d"].get("requestId") != rid:
                continue
            if not msg["d"].get("requestStatus", {}).get("result"):
                raise RuntimeError(f"{request_type}: {msg['d']}")
            return msg["d"].get("responseData", {})


async def connect() -> Obs:
    ws = await websockets.connect(URL, max_size=64 * 1024 * 1024)
    hello = json.loads(await ws.recv())
    if hello.get("op") != 0:
        raise RuntimeError(f"unexpected hello: {hello}")
    d = hello["d"]
    identify = {"rpcVersion": 1}
    if d.get("authentication"):
        salt = d["authentication"]["salt"]
        challenge = d["authentication"]["challenge"]
        secret = base64.b64encode(
            hashlib.sha256((PASSWORD + salt).encode()).digest()
        ).decode()
        auth = base64.b64encode(
            hashlib.sha256((secret + challenge).encode()).digest()
        ).decode()
        identify["authentication"] = auth
    await ws.send(json.dumps({"op": 1, "d": identify}))
    identified = json.loads(await ws.recv())
    if identified.get("op") != 2:
        raise RuntimeError(f"identify failed: {identified}")
    return Obs(ws)


async def main() -> None:
    obs = await connect()
    print("version:", await obs.request("GetVersion"))
    kinds = await obs.request("GetInputKindList")
    print("input kinds:", kinds)
    scenes = await obs.request("GetSceneList")
    print("scenes:", json.dumps(scenes, ensure_ascii=False)[:2000])
    for kind in kinds.get("inputKinds", []):
        if "spout" in kind.lower():
            try:
                defaults = await obs.request("GetInputDefaultSettings", {"inputKind": kind})
                print(f"defaults[{kind}]:", json.dumps(defaults, ensure_ascii=False)[:2000])
            except Exception as exc:
                print(f"defaults[{kind}] failed:", exc)


if __name__ == "__main__":
    asyncio.run(main())
