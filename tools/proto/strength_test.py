"""Compare hiding strength / restore quality across cell, guard and downscale."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obscure as ob

ROOT = Path(r"D:\program\OBScure")
SRC = ROOT / "sample" / "Counter-strike 2 2026.07.14 - 04.32.40.12.DVR_2.mp4"
OUT = ROOT / "out" / "phase0" / "strength"

CONFIGS = [
    ("c32_g2_s100", 32, 2, 1.00),
    ("c32_g2_s140", 32, 2, 1.40),
    ("c32_g2_s160", 32, 2, 1.60),
    ("c16_g1_s100", 16, 1, 1.00),
    ("c16_g1_s120", 16, 1, 1.20),
    ("c16_g2_s100", 16, 2, 1.00),
    ("c16_g2_s120", 16, 2, 1.20),
    ("c32_g3_s100", 32, 3, 1.00),
]


def frame_at(t: float) -> np.ndarray:
    cap = cv2.VideoCapture(str(SRC))
    cap.set(cv2.CAP_PROP_POS_MSEC, t * 1000)
    ok, f = cap.read()
    cap.release()
    assert ok
    return f


def panel(img: np.ndarray, w: int, h: int, label: str) -> np.ndarray:
    img = cv2.resize(img, (w, h), interpolation=cv2.INTER_AREA)
    bar = np.zeros((24, w, 3), np.uint8)
    cv2.putText(bar, label, (6, 17), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
    return np.vstack([bar, img])


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    frame = frame_at(2.0)
    rows = []
    report = {}

    for name, cell, guard, scale in CONFIGS:
        layout = ob.Layout(cell=cell, guard=guard, key="obscure-demo-key")
        key = ob.derive_key(layout)
        border = ob.build_border(layout, key, extra_scale=scale)
        content = ob.obfuscate_content(frame, layout, key, extra_scale=scale)
        transport = ob.compose_transport(content, border, layout)
        ref = ob.make_reference(frame, layout, extra_scale=scale)

        H, found, err = ob.detect_homography(transport)
        canon = ob.warp_to_canonical(transport, H, layout)
        restored = ob.deobfuscate_content(ob.crop_content(canon, layout), layout, key)
        obf_small = cv2.resize(content, (layout.ref_w, layout.ref_h), interpolation=cv2.INTER_AREA)

        rep = {
            "cell": cell,
            "guard": guard,
            "scale": scale,
            "grid": f"{layout.cols}x{layout.rows}",
            "blocks": layout.nblocks,
            "ref": f"{layout.ref_w}x{layout.ref_h}",
            "markers": sorted(found.keys()),
            "h_err": err,
            "obf_psnr": ob.psnr(obf_small, ref),
            "obf_ssim": ob.ssim(obf_small, ref),
            "restored_psnr": ob.psnr(restored, ref),
            "restored_ssim": ob.ssim(restored, ref),
        }
        report[name] = rep
        print(name, json.dumps(rep))

        cv2.imwrite(str(OUT / f"{name}_obfuscated.png"), content)
        cv2.imwrite(str(OUT / f"{name}_transport.png"), transport)
        cv2.imwrite(str(OUT / f"{name}_restored.png"), restored)

        left = panel(content, 480, 269, f"{name} OBSCURED  {layout.cols}x{layout.rows}")
        right = panel(restored, 480, 269, f"{name} RESTORED  ref {layout.ref_w}x{layout.ref_h}")
        rows.append(np.hstack([left, right]))

    grid = np.vstack(rows)
    cv2.imwrite(str(OUT / "strength_grid.png"), grid)
    (OUT / "strength_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
