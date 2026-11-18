"""Find the canonical marker-corner bias that minimizes decode error."""
from __future__ import annotations

import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obscure as ob

ROOT = Path(r"D:\program\OBScure")
SRC = ROOT / "sample" / "Counter-strike 2 2026.07.14 - 04.32.40.12.DVR_2.mp4"


def frame_at(t: float) -> np.ndarray:
    cap = cv2.VideoCapture(str(SRC))
    cap.set(cv2.CAP_PROP_POS_MSEC, t * 1000)
    ok, f = cap.read()
    cap.release()
    assert ok
    return f


def main() -> None:
    layout = ob.Layout(cell=32, guard=2, key="obscure-demo-key")
    key = ob.derive_key(layout)
    border = ob.build_border(layout, key)
    frame = frame_at(2.0)
    content = ob.obfuscate_content(frame, layout, key)
    transport = ob.compose_transport(content, border, layout)
    ref = ob.make_reference(frame, layout)

    for offset in (-1.0, -0.75, -0.5, -0.25, 0.0, 0.25, 0.5):
        H, found, err = ob.detect_homography(transport, offset=offset)
        if H is None:
            print(offset, "no H")
            continue
        canon = ob.warp_to_canonical(transport, H, layout)
        restored = ob.deobfuscate_content(ob.crop_content(canon, layout), layout, key)
        print(
            f"offset={offset:+.2f}  tx={H[0,2]:+.4f} ty={H[1,2]:+.4f} "
            f"reproj={err:.4f}  psnr={ob.psnr(restored, ref):.2f}  "
            f"ssim={ob.ssim(restored, ref):.4f}"
        )


if __name__ == "__main__":
    main()
