"""Test decoder guard-band inset on 720p and 1080p captures."""
from __future__ import annotations

import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obscure as ob

ROOT = Path(r"D:\program\OBScure")
SRC = ROOT / "sample" / "Counter-strike 2 2026.07.14 - 04.32.40.12.DVR_2.mp4"
OUT = ROOT / "out" / "phase0" / "margin"
START = 2.0


def panel(img: np.ndarray, label: str, w: int = 640, h: int = 360) -> np.ndarray:
    img = cv2.resize(img, (w, h), interpolation=cv2.INTER_AREA)
    bar = np.zeros((24, w, 3), np.uint8)
    cv2.putText(bar, label, (8, 17), cv2.FONT_HERSHEY_SIMPLEX, 0.46, (240, 240, 240), 1, cv2.LINE_AA)
    return np.vstack([bar, img])


def run(video: Path, config: str, res: str) -> None:
    cell, guard, scale = {"c32_g2_s120": (32, 2, 1.2), "c32_g2_s140": (32, 2, 1.4)}[config]
    layout = ob.Layout(cell=cell, guard=guard, key="obscure-demo-key")
    key = ob.derive_key(layout)
    cap = cv2.VideoCapture(str(video))
    src = cv2.VideoCapture(str(SRC))
    src.set(cv2.CAP_PROP_POS_MSEC, START * 1000)
    frames = []
    srcs = []
    idx = 0
    while len(frames) < 360:
        ok, f = cap.read()
        ok2, s = src.read()
        if not (ok and ok2):
            break
        if idx in (60, 180, 300):
            frames.append(f)
            srcs.append(s)
        idx += 1
    cap.release()
    src.release()

    rows = []
    for margin in (0, 1, 2, 3):
        H, _, _ = ob.detect_homography(frames[0])
        canon = ob.warp_to_canonical(frames[0], H, layout)
        vals = []
        panels = []
        for i, (f, s) in enumerate(zip(frames, srcs)):
            c = ob.warp_to_canonical(f, H, layout)
            r = ob.deobfuscate_content(ob.crop_content(c, layout), layout, key, margin=margin)
            ref = ob.make_reference(s, layout, extra_scale=scale)
            vals.append((ob.psnr(r, ref), ob.ssim(r, ref)))
            panels.append(panel(r, f"{config} {res} margin={margin} f{60 + i * 120}"))
        print(
            f"{config} {res} margin={margin}: "
            f"psnr={np.mean([v[0] for v in vals]):.2f} ssim={np.mean([v[1] for v in vals]):.4f}"
        )
        rows.append(np.hstack(panels))

    OUT.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(OUT / f"{config}_{res}_margin_grid.png"), np.vstack(rows))


def main() -> None:
    run(ROOT / "out/phase0/videos/c32_g2_s120_720p_4M.mp4", "c32_g2_s120", "720p")
    run(ROOT / "out/phase0/videos/c32_g2_s120_1080p_8M.mp4", "c32_g2_s120", "1080p")
    run(ROOT / "out/phase0/videos/c32_g2_s140_720p_4M.mp4", "c32_g2_s140", "720p")


if __name__ == "__main__":
    main()
