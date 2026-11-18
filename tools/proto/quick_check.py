"""Lossless round-trip check for the phase-0 transform and transport frame."""

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
OUT = ROOT / "out" / "phase0"
WORK = ROOT / "work"


def read_frame(path: Path, t_sec: float) -> np.ndarray:
    cap = cv2.VideoCapture(str(path))
    cap.set(cv2.CAP_PROP_POS_MSEC, t_sec * 1000)
    ok, frame = cap.read()
    cap.release()
    if not ok:
        raise RuntimeError(f"cannot read {path} at {t_sec}s")
    return frame


def panel(img: np.ndarray, w: int, h: int, label: str) -> np.ndarray:
    img = cv2.resize(img, (w, h), interpolation=cv2.INTER_AREA)
    cv2.rectangle(img, (0, 0), (w, 26), (0, 0, 0), -1)
    cv2.putText(img, label, (8, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)

    layout = ob.Layout(cell=32, guard=2, key="obscure-demo-key")
    key = ob.derive_key(layout)
    border = ob.build_border(layout, key)

    frame = read_frame(SRC, 2.0)
    content = ob.obfuscate_content(frame, layout, key)
    transport = ob.compose_transport(content, border, layout)
    ref = ob.make_reference(frame, layout)

    cv2.imwrite(str(OUT / "quick_original.png"), frame)
    cv2.imwrite(str(OUT / "quick_transport.png"), transport)
    cv2.imwrite(str(OUT / "quick_content_obfuscated.png"), content)

    H, found, err = ob.detect_homography(transport)
    meta, agree = ob.decode_metadata(transport, H, layout)

    # exactness with identity (isolates transform math from capture alignment)
    restored_id = ob.deobfuscate_content(ob.crop_content(transport, layout), layout, key)
    diff_id = np.abs(restored_id.astype(np.int16) - ref.astype(np.int16))

    # realistic path through the detected homography
    canon = ob.warp_to_canonical(transport, H, layout)
    restored = ob.deobfuscate_content(ob.crop_content(canon, layout), layout, key)
    diff = np.abs(restored.astype(np.int16) - ref.astype(np.int16))

    # obfuscated content compared against the reference
    obf_view = cv2.resize(content, (layout.ref_w, layout.ref_h), interpolation=cv2.INTER_AREA)

    metrics = {
        "layout": ob.layout_dict(layout),
        "key_id": f"{key.key_id:08X}",
        "markers_found": sorted(found.keys()),
        "homography": None if H is None else H.tolist(),
        "homography_reproj_err_px": err,
        "metadata": meta,
        "metadata_vote_agreement": agree,
        "identity_exact": bool(diff_id.max() == 0),
        "identity_psnr": ob.psnr(restored_id, ref),
        "homography_max_abs_diff": int(diff.max()),
        "homography_psnr": ob.psnr(restored, ref),
        "obfuscated_psnr": ob.psnr(obf_view, ref),
        "homography_ssim": ob.ssim(restored, ref),
        "obfuscated_ssim": ob.ssim(obf_view, ref),
    }
    (OUT / "quick_metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(json.dumps(metrics, indent=2))

    # visual sheets
    W, Hh = 640, 360
    original_panel = panel(frame, W, Hh, "ORIGINAL")
    transport_panel = panel(transport, W, Hh, "OBSCURED TRANSPORT")
    restored_panel = panel(restored, W, Hh, "RESTORED (lossless)")
    sheet = np.hstack([original_panel, transport_panel, restored_panel])
    cv2.imwrite(str(OUT / "quick_sheet.png"), sheet)

    content_panel = panel(obf_view, W, Hh, "OBFUSCATED CONTENT")
    ref_panel = panel(ref, W, Hh, "REFERENCE")
    restored2 = panel(restored, W, Hh, "RESTORED")
    sheet2 = np.hstack([ref_panel, content_panel, restored2])
    cv2.imwrite(str(OUT / "quick_content_sheet.png"), sheet2)

    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
