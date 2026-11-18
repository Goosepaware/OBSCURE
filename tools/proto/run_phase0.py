"""Phase-0 bitrate sweep: obfuscate -> encode -> decode -> measure.

Usage examples:
  python run_phase0.py --smoke
  python run_phase0.py --configs c32_g2,c16_g1 --bitrates 10,6,4,2.5 --res 1080p
  python run_phase0.py --configs c32_g2,c16_g1 --bitrates 6,4 --res 720p
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obscure as ob

ROOT = Path(r"D:\program\OBScure")
FFMPEG = ROOT / "ffmpeg.exe"
SRC = ROOT / "sample" / "Counter-strike 2 2026.07.14 - 04.32.40.12.DVR_2.mp4"

OUT = ROOT / "out" / "phase0"
WORK = ROOT / "work" / "phase0"
VID = OUT / "videos"
CONTACT = OUT / "contacts"
COMPARE = OUT / "compare"

FPS = 60
START_SEC = 2.0

CONFIGS: dict[str, tuple[int, int, float]] = {
    "c32_g2": (32, 2, 1.00),
    "c32_g2_s120": (32, 2, 1.20),
    "c32_g2_s130": (32, 2, 1.30),
    "c32_g2_s140": (32, 2, 1.40),
    "c32_g2_s160": (32, 2, 1.60),
    "c16_g1": (16, 1, 1.00),
    "c16_g2": (16, 2, 1.00),
    "c16_g1_s120": (16, 1, 1.20),
}

RESOLUTIONS = {
    "1080p": (1920, 1080),
    "720p": (1280, 720),
}


@dataclass
class Variant:
    config: str
    cell: int
    guard: int
    scale: float
    res: str
    width: int
    height: int
    bitrate: float
    path: Path

    @property
    def tag(self) -> str:
        return f"{self.config}_{self.res}_{self.bitrate:g}M"


def ensure_dirs() -> None:
    for d in (OUT, WORK, VID, CONTACT, COMPARE):
        d.mkdir(parents=True, exist_ok=True)


def frame_generator(start_sec: float, count: int) -> Iterable[np.ndarray]:
    cap = cv2.VideoCapture(str(SRC))
    if not cap.isOpened():
        raise RuntimeError(f"cannot open {SRC}")
    cap.set(cv2.CAP_PROP_POS_MSEC, start_sec * 1000.0)
    n = 0
    while n < count:
        ok, frame = cap.read()
        if not ok:
            break
        yield frame
        n += 1
    cap.release()


def intermediate_path(config: str, seconds: float) -> Path:
    return WORK / f"intermediate_{config}_{seconds:g}s.mkv"


def make_intermediate(config: str, seconds: float) -> Path:
    cell, guard, scale = CONFIGS[config]
    layout = ob.Layout(cell=cell, guard=guard, key="obscure-demo-key")
    key = ob.derive_key(layout)
    border = ob.build_border(layout, key, extra_scale=scale)
    path = intermediate_path(config, seconds)
    if path.exists():
        return path

    count = int(round(seconds * FPS))
    cmd = [
        str(FFMPEG), "-y", "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "bgr24",
        "-s", f"{layout.canvas_w}x{layout.canvas_h}", "-r", str(FPS),
        "-i", "-", "-an",
        "-c:v", "libx264rgb", "-qp", "0", "-preset", "ultrafast",
        str(path),
    ]
    print(f"[intermediate] {config}: {count} frames -> {path.name}")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    t0 = time.time()
    try:
        for i, frame in enumerate(frame_generator(START_SEC, count)):
            content = ob.obfuscate_content(frame, layout, key, extra_scale=scale)
            transport = ob.compose_transport(content, border, layout)
            proc.stdin.write(transport.tobytes())
            if (i + 1) % 60 == 0:
                print(f"  {i + 1}/{count}  {time.time() - t0:.1f}s")
        proc.stdin.close()
        err = proc.stderr.read().decode("utf-8", "ignore")
        rc = proc.wait()
        if rc != 0:
            raise RuntimeError(f"ffmpeg intermediate failed: {err[-2000:]}")
    finally:
        if proc.poll() is None:
            proc.kill()
    print(f"[intermediate] done in {time.time() - t0:.1f}s")
    return path


def encode_variant(intermediate: Path, v: Variant) -> None:
    if v.path.exists():
        return
    br = f"{v.bitrate:g}M"
    buf = f"{v.bitrate * 2:g}M"
    cmd = [
        str(FFMPEG), "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(intermediate), "-an",
        "-c:v", "libx264", "-preset", "veryfast",
        "-b:v", br, "-minrate", br, "-maxrate", br, "-bufsize", buf,
        "-x264-params", "nal-hrd=cbr",
        "-pix_fmt", "yuv420p", "-g", "60", "-keyint_min", "60", "-sc_threshold", "0",
    ]
    if (v.width, v.height) != (1920, 1080):
        cmd += ["-vf", f"scale={v.width}:{v.height}:flags=lanczos"]
    cmd += ["-movflags", "+faststart", str(v.path)]
    print(f"[encode] {v.tag}")
    t0 = time.time()
    subprocess.run(cmd, check=True)
    print(f"  done in {time.time() - t0:.1f}s  {v.path.stat().st_size / 1e6:.1f} MB")


def make_panel(img: np.ndarray, label: str, w: int = 640, h: int = 360) -> np.ndarray:
    resized = cv2.resize(img, (w, h), interpolation=cv2.INTER_AREA)
    bar = np.zeros((24, w, 3), np.uint8)
    cv2.putText(bar, label, (8, 17), cv2.FONT_HERSHEY_SIMPLEX, 0.46, (240, 240, 240), 1, cv2.LINE_AA)
    return np.vstack([bar, resized])


def evaluate(v: Variant, seconds: float, make_compare: bool) -> dict:
    cell, guard, scale = CONFIGS[v.config]
    layout = ob.Layout(cell=cell, guard=guard, key="obscure-demo-key")
    key = ob.derive_key(layout)
    intermediate = intermediate_path(v.config, seconds)

    cap_int = cv2.VideoCapture(str(intermediate))
    cap_dec = cv2.VideoCapture(str(v.path))
    cap_src = cv2.VideoCapture(str(SRC))
    cap_src.set(cv2.CAP_PROP_POS_MSEC, START_SEC * 1000.0)
    if not (cap_int.isOpened() and cap_dec.isOpened() and cap_src.isOpened()):
        raise RuntimeError(f"cannot open captures for {v.tag}")

    n_expected = int(round(seconds * FPS))
    detect_every = 60   # 1 Hz, matches the intended live decoder behavior
    metric_every = 10   # 36 metric samples per 6 s
    contact_at = {60, 180, 300}

    compare_proc = None
    compare_path = COMPARE / f"{v.tag}.mp4"
    if make_compare:
        cmd = [
            str(FFMPEG), "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", "1920x384", "-r", str(FPS),
            "-i", "-", "-an", "-c:v", "libx264", "-preset", "veryfast",
            "-crf", "18", "-pix_fmt", "yuv420p", str(compare_path),
        ]
        compare_proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    H_last = None
    marker_attempts = marker_ok = 0
    meta_attempts = meta_ok = 0
    h_errs: list[float] = []
    scales: list[float] = []
    margins: list[int] = []
    psnrs: list[float] = []
    ssims: list[float] = []
    obf_psnrs: list[float] = []
    obf_ssims: list[float] = []
    contact_rows: list[np.ndarray] = []
    frames = 0

    t0 = time.time()
    while frames < n_expected:
        ok_i, trans = cap_int.read()
        ok_d, dec = cap_dec.read()
        ok_s, src = cap_src.read()
        if not (ok_i and ok_d and ok_s):
            break

        H = H_last
        if frames % detect_every == 0:
            H_det, found, err = ob.detect_homography(dec)
            marker_attempts += 1
            if H_det is not None:
                marker_ok += 1
                H_last = H_det
                H = H_det
                h_errs.append(err)
                scales.append(float(np.sqrt(abs(np.linalg.det(H_det[:2, :2])))))
                if frames % (detect_every * 3) == 0:
                    meta, agree = ob.decode_metadata(dec, H_det, layout)
                    meta_attempts += 1
                    if meta is not None and meta.get("key_id") == key.key_id:
                        meta_ok += 1
            else:
                H = H_last

        if H is None:
            H = np.eye(3, dtype=np.float64)

        # adapt the decoder's guard-band inset to the capture scale:
        # at 720p a 2px 1080p guard becomes 1.33px and bleeds into tiles.
        scale_est = float(np.sqrt(abs(np.linalg.det(H[:2, :2]))))
        if scale_est >= 0.95:
            margin = 0
        elif scale_est >= 0.65:
            margin = 1
        else:
            margin = 2
        margins.append(margin)

        canon = ob.warp_to_canonical(dec, H, layout)
        restored = ob.deobfuscate_content(
            ob.crop_content(canon, layout), layout, key, margin=margin
        )

        if frames % metric_every == 0:
            ref = ob.make_reference(src, layout, extra_scale=scale)
            psnrs.append(ob.psnr(restored, ref))
            ssims.append(ob.ssim(restored, ref))
            obf = cv2.resize(
                ob.crop_content(canon, layout),
                (layout.ref_w, layout.ref_h),
                interpolation=cv2.INTER_AREA,
            )
            obf_psnrs.append(ob.psnr(obf, ref))
            obf_ssims.append(ob.ssim(obf, ref))

        row = np.hstack(
            [
                make_panel(trans, f"ORIGINAL  {v.tag}", 640, 360),
                make_panel(dec, f"OBSCURED  {v.res} {v.bitrate:g} Mbps", 640, 360),
                make_panel(restored, f"RESTORED  f{frames}", 640, 360),
            ]
        )
        if compare_proc is not None:
            compare_proc.stdin.write(row.tobytes())
        if frames in contact_at:
            contact_rows.append(row)

        frames += 1
        if frames % 120 == 0:
            print(f"  eval {v.tag}: {frames}/{n_expected}  {time.time() - t0:.1f}s")

    cap_int.release()
    cap_dec.release()
    cap_src.release()
    if compare_proc is not None:
        compare_proc.stdin.close()
        compare_proc.wait()

    if contact_rows:
        cv2.imwrite(str(CONTACT / f"{v.tag}.png"), np.vstack(contact_rows))

    size_mb = v.path.stat().st_size / 1e6
    actual_kbps = v.path.stat().st_size * 8 / (max(1, frames) / FPS) / 1000.0
    res = {
        "config": v.config,
        "cell": v.cell,
        "guard": v.guard,
        "scale": v.scale,
        "res": v.res,
        "bitrate_mbps": v.bitrate,
        "path": str(v.path),
        "frames": frames,
        "size_mb": round(size_mb, 2),
        "actual_kbps": round(actual_kbps, 1),
        "marker_attempts": marker_attempts,
        "marker_ok": marker_ok,
        "marker_rate": round(marker_ok / max(1, marker_attempts), 4),
        "meta_attempts": meta_attempts,
        "meta_ok": meta_ok,
        "meta_rate": round(meta_ok / max(1, meta_attempts), 4),
        "h_err_mean_px": round(float(np.mean(h_errs)) if h_errs else 999.0, 4),
        "capture_scale": round(float(np.mean(scales)) if scales else 0.0, 4),
        "decoder_margin": int(round(float(np.mean(margins)))) if margins else 0,
        "psnr_mean": round(float(np.mean(psnrs)) if psnrs else 0.0, 2),
        "ssim_mean": round(float(np.mean(ssims)) if ssims else 0.0, 4),
        "obf_psnr_mean": round(float(np.mean(obf_psnrs)) if obf_psnrs else 0.0, 2),
        "obf_ssim_mean": round(float(np.mean(obf_ssims)) if obf_ssims else 0.0, 4),
        "elapsed_s": round(time.time() - t0, 1),
    }
    print(f"[eval] {v.tag}: {json.dumps(res, ensure_ascii=False)}")
    return res


def parse_list(text: str) -> list[str]:
    return [x.strip() for x in text.split(",") if x.strip()]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--configs", default="c32_g2,c16_g1")
    parser.add_argument("--bitrates", default="10,6,4,2.5")
    parser.add_argument("--res", default="1080p")
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--compare", default="6,4")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--skip-eval", action="store_true")
    args = parser.parse_args()

    ensure_dirs()
    configs = parse_list(args.configs)
    bitrates = [float(x) for x in parse_list(args.bitrates)]
    resolutions = parse_list(args.res)
    compare_set = {float(x) for x in parse_list(args.compare)} if args.compare else set()
    if args.smoke:
        configs = ["c32_g2"]
        bitrates = [6.0]
        resolutions = ["1080p"]
        args.seconds = 1.0
        compare_set = {6.0}

    report = []
    for config in configs:
        if config not in CONFIGS:
            raise SystemExit(f"unknown config {config}")
        cell, guard, scale = CONFIGS[config]
        intermediate = make_intermediate(config, args.seconds)
        for res_name in resolutions:
            width, height = RESOLUTIONS[res_name]
            for br in bitrates:
                v = Variant(
                    config=config, cell=cell, guard=guard, scale=scale,
                    res=res_name, width=width, height=height, bitrate=br,
                    path=VID / f"{config}_{res_name}_{br:g}M.mp4",
                )
                encode_variant(intermediate, v)
                if not args.skip_eval:
                    report.append(evaluate(v, args.seconds, br in compare_set))

    report_path = OUT / "report.json"
    existing = []
    if report_path.exists():
        try:
            existing = json.loads(report_path.read_text(encoding="utf-8"))
        except Exception:
            existing = []
    # replace entries with same tag
    by_tag = {f"{r['config']}_{r['res']}_{r['bitrate_mbps']:g}M": r for r in existing}
    for r in report:
        by_tag[f"{r['config']}_{r['res']}_{r['bitrate_mbps']:g}M"] = r
    report_path.write_text(json.dumps(list(by_tag.values()), indent=2), encoding="utf-8")
    print(f"report -> {report_path}")


if __name__ == "__main__":
    main()
