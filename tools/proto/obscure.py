"""
OBScure phase-0 prototype.

Compression-robust visual obfuscation:
  - resize source to guard-free content resolution
  - split into cells
  - keyed permutation + dihedral transform + channel permutation/inversion
  - place tiles into 32px cells with a guard border
  - compose a 1920x1080 transport frame with ArUco corner markers and metadata strips

The inverse is exact in the absence of lossy compression: codec errors stay local
inside each tile because no diffusion / global transform is used.
"""

from __future__ import annotations

import hashlib
import zlib
from dataclasses import dataclass, asdict
from typing import Optional

import cv2
import numpy as np

# ---------------------------------------------------------------------------
# layout
# ---------------------------------------------------------------------------


@dataclass
class Layout:
    canvas_w: int = 1920
    canvas_h: int = 1080
    content_x: int = 160
    content_y: int = 92
    content_w: int = 1600
    content_h: int = 896
    cell: int = 32
    guard: int = 2
    key: str = "obscure-demo-key"

    @property
    def cols(self) -> int:
        return self.content_w // self.cell

    @property
    def rows(self) -> int:
        return self.content_h // self.cell

    @property
    def inner(self) -> int:
        return self.cell - 2 * self.guard

    @property
    def nblocks(self) -> int:
        return self.cols * self.rows

    @property
    def ref_w(self) -> int:
        return self.cols * self.inner

    @property
    def ref_h(self) -> int:
        return self.rows * self.inner

    def validate(self) -> None:
        assert self.content_w % self.cell == 0, "content_w must be cell-aligned"
        assert self.content_h % self.cell == 0, "content_h must be cell-aligned"
        assert self.inner >= 8, "guard too large for cell"
        assert self.content_x >= 0 and self.content_y >= 0
        assert self.content_x + self.content_w <= self.canvas_w
        assert self.content_y + self.content_h <= self.canvas_h


# marker geometry: 64x64 marker inside a 80x80 white quiet box
MARKER_SIZE = 64
MARKER_QUIET = 8
MARKER_BOX = MARKER_SIZE + 2 * MARKER_QUIET
MARKER_POS = {
    0: (80, 12),      # top-left quiet box top-left
    1: (1760, 12),    # top-right
    2: (1760, 988),   # bottom-right
    3: (80, 988),     # bottom-left
}

# metadata strips: 190 x 6 modules of 8x8 px
META_X0 = 200
META_Y0_TOP = 20
META_Y0_BOT = 1012
META_COLS = 190
META_ROWS = 6
META_MODULE = 8
META_PACKET_BITS = 208
META_COPIES = 5

ARUCO_DICT = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50)
_ARUCO_PARAMS = cv2.aruco.DetectorParameters()
_ARUCO_PARAMS.cornerRefinementMethod = cv2.aruco.CORNER_REFINE_SUBPIX
_ARUCO_PARAMS.cornerRefinementWinSize = 5
_ARUCO_PARAMS.cornerRefinementMaxIterations = 50
_ARUCO_PARAMS.cornerRefinementMinAccuracy = 0.01
ARUCO_DETECTOR = cv2.aruco.ArucoDetector(ARUCO_DICT, _ARUCO_PARAMS)


def marker_rect(ident: int) -> tuple[int, int, int, int]:
    x, y = MARKER_POS[ident]
    return x + MARKER_QUIET, y + MARKER_QUIET, MARKER_SIZE, MARKER_SIZE


def marker_corners_canonical(offset: float = -0.5) -> dict[int, np.ndarray]:
    """Marker corners in the transport canvas, order TL,TR,BR,BL."""
    out: dict[int, np.ndarray] = {}
    for ident in MARKER_POS:
        x, y, w, h = marker_rect(ident)
        x += offset
        y += offset
        out[ident] = np.array(
            [[x, y], [x + w, y], [x + w, y + h], [x, y + h]], dtype=np.float32
        )
    return out


# ---------------------------------------------------------------------------
# key material
# ---------------------------------------------------------------------------


@dataclass
class KeyMaterial:
    perm: np.ndarray      # dest cell -> source cell
    dih: np.ndarray       # dest cell -> dihedral code
    color: np.ndarray     # dest cell -> color code
    key_id: int
    seed: bytes


def derive_key(layout: Layout) -> KeyMaterial:
    seed = hashlib.sha256(
        f"{layout.key}|v1|{layout.cols}x{layout.rows}|{layout.cell}|{layout.guard}".encode(
            "utf-8"
        )
    ).digest()
    rng = np.random.default_rng(int.from_bytes(seed[:8], "little"))
    n = layout.nblocks
    perm = rng.permutation(n).astype(np.int32)  # dest -> source
    dih = rng.integers(0, 8, size=n, dtype=np.uint8)
    color = rng.integers(0, 8, size=n, dtype=np.uint8)
    key_id = int.from_bytes(
        hashlib.sha256(b"OBSCure-key-id|" + layout.key.encode("utf-8")).digest()[:4],
        "big",
    )
    return KeyMaterial(perm=perm, dih=dih, color=color, key_id=key_id, seed=seed)


# ---------------------------------------------------------------------------
# per-tile transforms
# ---------------------------------------------------------------------------


def apply_dihedral(arr: np.ndarray, code: int) -> np.ndarray:
    """Apply a dihedral transform to the last spatial axes.

    arr: (..., H, W, C), square H/W. Returns a new array (rot90 may be a view).
    """
    if code == 0:
        return arr
    if code == 1:
        return np.rot90(arr, 1, axes=(-3, -2))
    if code == 2:
        return np.rot90(arr, 2, axes=(-3, -2))
    if code == 3:
        return np.rot90(arr, 3, axes=(-3, -2))
    if code == 4:
        return np.flip(arr, axis=-3)
    if code == 5:
        return np.rot90(np.flip(arr, axis=-3), 1, axes=(-3, -2))
    if code == 6:
        return np.rot90(np.flip(arr, axis=-3), 2, axes=(-3, -2))
    if code == 7:
        return np.rot90(np.flip(arr, axis=-3), 3, axes=(-3, -2))
    raise ValueError(code)


_INV_DIH: dict[int, int] = {}


def inverse_dihedral_code(code: int) -> int:
    if not _INV_DIH:
        probe = np.arange(16, dtype=np.int32).reshape(4, 4, 1)
        for c in range(8):
            t = apply_dihedral(probe, c)
            for inv in range(8):
                if np.array_equal(apply_dihedral(t, inv), probe):
                    _INV_DIH[c] = inv
                    break
        assert len(_INV_DIH) == 8
    return _INV_DIH[code]


def apply_color(arr: np.ndarray, code: int) -> np.ndarray:
    """BGR channel permutation / inversion. All codes are self-inverse."""
    if code == 0:
        return arr
    if code == 1:
        return 255 - arr
    if code == 2:
        return arr[..., [2, 1, 0]]
    if code == 3:
        return 255 - arr[..., [2, 1, 0]]
    if code == 4:
        return arr[..., [0, 2, 1]]
    if code == 5:
        return 255 - arr[..., [0, 2, 1]]
    if code == 6:
        return arr[..., [1, 0, 2]]
    if code == 7:
        return 255 - arr[..., [1, 0, 2]]
    raise ValueError(code)


# ---------------------------------------------------------------------------
# forward / inverse
# ---------------------------------------------------------------------------


def make_reference(frame_bgr: np.ndarray, layout: Layout, extra_scale: float = 1.0) -> np.ndarray:
    """The image the decoder should recover at guard-free resolution."""
    if extra_scale > 1.0 + 1e-6:
        sw = max(1, int(round(layout.ref_w / extra_scale)))
        sh = max(1, int(round(layout.ref_h / extra_scale)))
        small = cv2.resize(frame_bgr, (sw, sh), interpolation=cv2.INTER_AREA)
        return cv2.resize(small, (layout.ref_w, layout.ref_h), interpolation=cv2.INTER_LINEAR)
    return cv2.resize(frame_bgr, (layout.ref_w, layout.ref_h), interpolation=cv2.INTER_AREA)


def obfuscate_content(
    frame_bgr: np.ndarray,
    layout: Layout,
    key: KeyMaterial,
    extra_scale: float = 1.0,
    use_color: bool = True,
    use_dihedral: bool = True,
) -> np.ndarray:
    layout.validate()
    img = make_reference(frame_bgr, layout, extra_scale)
    rows, cols, inner = layout.rows, layout.cols, layout.inner

    tiles = (
        img.reshape(rows, inner, cols, inner, 3)
        .transpose(0, 2, 1, 3, 4)
        .reshape(-1, inner, inner, 3)
    )
    out = np.empty_like(tiles)

    if use_dihedral:
        for code in range(8):
            idx = np.where(key.dih == code)[0]
            if idx.size:
                out[idx] = apply_dihedral(tiles[key.perm[idx]], code)
    else:
        out[:] = tiles[key.perm]

    if use_color:
        for code in range(8):
            idx = np.where(key.color == code)[0]
            if idx.size:
                out[idx] = apply_color(out[idx], code)

    content = np.full((layout.content_h, layout.content_w, 3), 12, np.uint8)
    cell_view = content.reshape(rows, layout.cell, cols, layout.cell, 3)
    g = layout.guard
    cell_view[:, g : g + inner, :, g : g + inner, :] = (
        out.reshape(rows, cols, inner, inner, 3).transpose(0, 2, 1, 3, 4)
    )
    return content


def deobfuscate_content(
    content: np.ndarray,
    layout: Layout,
    key: KeyMaterial,
    use_color: bool = True,
    use_dihedral: bool = True,
    margin: int = 0,
) -> np.ndarray:
    rows, cols, inner, cell, g = (
        layout.rows,
        layout.cols,
        layout.inner,
        layout.cell,
        layout.guard,
    )
    sample = inner - 2 * margin
    assert sample >= 8, "margin too large"
    g += margin
    cells = content.reshape(rows, cell, cols, cell, 3)
    cap = (
        cells[:, g : g + sample, :, g : g + sample, :]
        .transpose(0, 2, 1, 3, 4)
        .reshape(-1, sample, sample, 3)
        .copy()
    )

    if use_color:
        for code in range(8):
            idx = np.where(key.color == code)[0]
            if idx.size:
                cap[idx] = apply_color(cap[idx], code)

    restored = np.empty_like(cap)
    if use_dihedral:
        for code in range(8):
            idx = np.where(key.dih == code)[0]
            if idx.size:
                inv = inverse_dihedral_code(code)
                restored[key.perm[idx]] = apply_dihedral(cap[idx], inv)
    else:
        restored[key.perm] = cap

    img = (
        restored.reshape(rows, cols, sample, sample, 3)
        .transpose(0, 2, 1, 3, 4)
        .reshape(rows * sample, cols * sample, 3)
    )
    if sample != inner:
        img = cv2.resize(img, (layout.ref_w, layout.ref_h), interpolation=cv2.INTER_LINEAR)
    return img


def crop_content(transport: np.ndarray, layout: Layout) -> np.ndarray:
    y, x = layout.content_y, layout.content_x
    return transport[y : y + layout.content_h, x : x + layout.content_w]


# ---------------------------------------------------------------------------
# metadata
# ---------------------------------------------------------------------------


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []

    def write(self, value: int, width: int) -> None:
        for i in range(width - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def bytes(self) -> bytes:
        while len(self.bits) % 8:
            self.bits.append(1)  # pad with ones (not part of CRC)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for bit in self.bits[i : i + 8]:
                b = (b << 1) | bit
            out.append(b)
        return bytes(out)


def _crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_metadata_bits(
    layout: Layout,
    key_id: int,
    flags: int = 0,
    use_color: bool = True,
    use_dihedral: bool = True,
    extra_scale: float = 1.0,
) -> list[int]:
    """Build a fixed 208-bit packet, repeated META_COPIES times."""
    flags = flags & 0xFF
    if use_color:
        flags |= 0x01
    if use_dihedral:
        flags |= 0x02
    if extra_scale > 1.0 + 1e-6:
        flags |= 0x04

    bw = BitWriter()
    bw.write(0xB5C3, 16)
    bw.write(1, 8)
    bw.write(layout.canvas_w, 12)
    bw.write(layout.canvas_h, 12)
    bw.write(layout.content_x, 12)
    bw.write(layout.content_y, 12)
    bw.write(layout.content_w, 12)
    bw.write(layout.content_h, 12)
    bw.write(layout.cell, 8)
    bw.write(layout.guard, 8)
    bw.write(layout.cols, 8)
    bw.write(layout.rows, 8)
    bw.write(key_id & 0xFFFFFFFF, 32)
    bw.write(flags, 8)
    bw.write(layout.inner, 8)
    bw.write(0, 16)  # reserved
    payload = bw.bytes()
    assert len(payload) == 24, len(payload)
    crc = _crc16_ccitt(payload)
    bw.write(crc, 16)
    packet = bw.bits
    assert len(packet) == META_PACKET_BITS, len(packet)
    return packet * META_COPIES + [0] * (META_COLS * META_ROWS - len(packet) * META_COPIES)


def _draw_metadata_strip(
    canvas: np.ndarray, bits: list[int], x0: int, y0: int
) -> None:
    for i, bit in enumerate(bits[: META_COLS * META_ROWS]):
        r, c = divmod(i, META_COLS)
        x = x0 + c * META_MODULE
        y = y0 + r * META_MODULE
        color = (255, 255, 255) if bit else (0, 0, 0)
        canvas[y : y + META_MODULE, x : x + META_MODULE] = color


def build_border(
    layout: Layout,
    key: KeyMaterial,
    use_color: bool = True,
    use_dihedral: bool = True,
    extra_scale: float = 1.0,
) -> np.ndarray:
    canvas = np.full((layout.canvas_h, layout.canvas_w, 3), 18, np.uint8)

    # content outline
    cv2.rectangle(
        canvas,
        (layout.content_x - 2, layout.content_y - 2),
        (layout.content_x + layout.content_w + 1, layout.content_y + layout.content_h + 1),
        (70, 70, 70),
        1,
    )

    # ArUco markers with quiet zones
    for ident, (x, y) in MARKER_POS.items():
        marker = cv2.aruco.generateImageMarker(ARUCO_DICT, ident, MARKER_SIZE)
        box = np.full((MARKER_BOX, MARKER_BOX), 255, np.uint8)
        box[
            MARKER_QUIET : MARKER_QUIET + MARKER_SIZE,
            MARKER_QUIET : MARKER_QUIET + MARKER_SIZE,
        ] = marker
        canvas[y : y + MARKER_BOX, x : x + MARKER_BOX] = cv2.cvtColor(
            box, cv2.COLOR_GRAY2BGR
        )

    # metadata strips (same packet top and bottom)
    bits = build_metadata_bits(
        layout,
        key.key_id,
        use_color=use_color,
        use_dihedral=use_dihedral,
        extra_scale=extra_scale,
    )
    _draw_metadata_strip(canvas, bits, META_X0, META_Y0_TOP)
    _draw_metadata_strip(canvas, bits, META_X0, META_Y0_BOT)

    # calibration patches: grayscale left, colors right
    for i in range(10):
        v = int(round(i * 255 / 9))
        y = 200 + i * 52
        cv2.rectangle(canvas, (24, y), (24 + 44, y + 40), (v, v, v), -1)
    colors = [
        (0, 0, 255),
        (0, 255, 0),
        (255, 0, 0),
        (0, 255, 255),
        (255, 255, 0),
        (255, 0, 255),
    ]
    for i, c in enumerate(colors):
        y = 200 + i * 80
        cv2.rectangle(canvas, (1780, y), (1780 + 60, y + 60), c, -1)

    # human-readable labels
    label = (
        f"OBSCure v1  {layout.content_w}x{layout.content_h}  "
        f"cell {layout.cell} guard {layout.guard}  key {key.key_id:08X}"
    )
    cv2.putText(
        canvas,
        label,
        (META_X0, 88),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.42,
        (150, 150, 150),
        1,
        cv2.LINE_AA,
    )
    return canvas


def compose_transport(content: np.ndarray, border: np.ndarray, layout: Layout) -> np.ndarray:
    out = border.copy()
    y, x = layout.content_y, layout.content_x
    out[y : y + layout.content_h, x : x + layout.content_w] = content
    return out


# ---------------------------------------------------------------------------
# detection / metadata decode
# ---------------------------------------------------------------------------


def detect_homography(
    transport: np.ndarray, offset: float = -0.5
) -> tuple[Optional[np.ndarray], dict[int, np.ndarray], float]:
    gray = cv2.cvtColor(transport, cv2.COLOR_BGR2GRAY)
    corners, ids, _ = ARUCO_DETECTOR.detectMarkers(gray)
    found: dict[int, np.ndarray] = {}
    if ids is not None:
        for c, ident in zip(corners, ids.flatten()):
            found[int(ident)] = c.reshape(4, 2).astype(np.float32)
    if len(found) < 3:
        return None, found, float("inf")

    canon = marker_corners_canonical(offset)
    src: list[np.ndarray] = []
    dst: list[np.ndarray] = []
    for ident, pts in found.items():
        if ident not in canon:
            continue
        src.extend(canon[ident])
        dst.extend(pts)
    if len(src) < 8:
        return None, found, float("inf")

    src_a = np.asarray(src, dtype=np.float32)
    dst_a = np.asarray(dst, dtype=np.float32)
    if len(src) >= 16:
        # all four markers: plain least squares is more accurate than RANSAC
        H, _ = cv2.findHomography(src_a, dst_a, 0)
    else:
        H, _ = cv2.findHomography(src_a, dst_a, cv2.RANSAC, 3.0)
    if H is None:
        return None, found, float("inf")

    proj = cv2.perspectiveTransform(np.asarray(src, dtype=np.float32).reshape(-1, 1, 2), H)
    err = float(
        np.mean(np.linalg.norm(proj.reshape(-1, 2) - np.asarray(dst, dtype=np.float32), axis=1))
    )
    return H, found, err


def warp_to_canonical(transport: np.ndarray, H: Optional[np.ndarray], layout: Layout) -> np.ndarray:
    if H is None:
        return transport
    inv = np.linalg.inv(H)
    return cv2.warpPerspective(
        transport, inv, (layout.canvas_w, layout.canvas_h), flags=cv2.INTER_LINEAR
    )


def _parse_metadata_packet(bits: list[int]) -> Optional[dict]:
    if len(bits) != META_PACKET_BITS:
        return None

    def rd(pos: int, width: int) -> tuple[int, int]:
        value = 0
        for bit in bits[pos : pos + width]:
            value = (value << 1) | bit
        return value, pos + width

    pos = 0
    magic, pos = rd(pos, 16)
    version, pos = rd(pos, 8)
    canvas_w, pos = rd(pos, 12)
    canvas_h, pos = rd(pos, 12)
    content_x, pos = rd(pos, 12)
    content_y, pos = rd(pos, 12)
    content_w, pos = rd(pos, 12)
    content_h, pos = rd(pos, 12)
    cell, pos = rd(pos, 8)
    guard, pos = rd(pos, 8)
    cols, pos = rd(pos, 8)
    rows, pos = rd(pos, 8)
    key_id, pos = rd(pos, 32)
    flags, pos = rd(pos, 8)
    inner, pos = rd(pos, 8)
    reserved, pos = rd(pos, 16)
    crc, pos = rd(pos, 16)
    if magic != 0xB5C3 or version != 1:
        return None
    # bits up to CRC are 24 bytes; rebuild payload to verify CRC
    payload_bits = bits[:192]
    payload = bytearray()
    for i in range(0, 192, 8):
        b = 0
        for bit in payload_bits[i : i + 8]:
            b = (b << 1) | bit
        payload.append(b)
    if _crc16_ccitt(bytes(payload)) != crc:
        return None
    return dict(
        version=version,
        canvas_w=canvas_w,
        canvas_h=canvas_h,
        content_x=content_x,
        content_y=content_y,
        content_w=content_w,
        content_h=content_h,
        cell=cell,
        guard=guard,
        cols=cols,
        rows=rows,
        key_id=key_id,
        flags=flags,
        inner=inner,
        reserved=reserved,
        crc=crc,
    )


def decode_metadata(
    transport: np.ndarray, H: Optional[np.ndarray], layout: Layout, spot: int = 2
) -> tuple[Optional[dict], float]:
    """Sample module centers (through H) and majority-vote the repeated packet."""
    if H is None:
        H = np.eye(3, dtype=np.float64)

    gray = cv2.cvtColor(transport, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape
    all_bits: list[int] = []

    for y0 in (META_Y0_TOP, META_Y0_BOT):
        for i in range(META_COLS * META_ROWS):
            r, c = divmod(i, META_COLS)
            x = META_X0 + (c + 0.5) * META_MODULE
            y = y0 + (r + 0.5) * META_MODULE
            p = H @ np.array([x, y, 1.0])
            px = int(round(p[0] / p[2]))
            py = int(round(p[1] / p[2]))
            x0 = max(0, px - spot)
            x1 = min(w, px + spot + 1)
            y0i = max(0, py - spot)
            y1i = min(h, py + spot + 1)
            if x0 >= x1 or y0i >= y1i:
                all_bits.append(0)
                continue
            patch = gray[y0i:y1i, x0:x1]
            all_bits.append(1 if float(patch.mean()) > 127.0 else 0)

    # split into 2 strips x 5 copies (packet 208 bits)
    copies = []
    per_strip = META_COLS * META_ROWS
    for strip in range(2):
        base = strip * per_strip
        for k in range(META_COPIES):
            start = base + k * META_PACKET_BITS
            copies.append(all_bits[start : start + META_PACKET_BITS])
    if len(copies) != 2 * META_COPIES:
        return None, 0.0

    voted: list[int] = []
    for i in range(META_PACKET_BITS):
        ones = sum(c[i] for c in copies)
        voted.append(1 if ones >= len(copies) / 2 else 0)
    packet = _parse_metadata_packet(voted)
    if packet is None:
        return None, 0.0

    agree = 0
    total = 0
    for c in copies:
        total += 1
        if c == voted:
            agree += 1
    return packet, agree / max(1, total)


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    if a.shape != b.shape:
        b = cv2.resize(b, (a.shape[1], a.shape[0]), interpolation=cv2.INTER_AREA)
    diff = cv2.absdiff(a, b).astype(np.float32)
    mse = float(np.mean(diff * diff))
    if mse <= 1e-12:
        return 99.0
    return float(10.0 * np.log10(255.0 * 255.0 / mse))


def ssim(a: np.ndarray, b: np.ndarray, max_side: int = 700) -> float:
    """Grayscale SSIM at a bounded working size (fast enough for sweeps)."""
    if a.shape != b.shape:
        b = cv2.resize(b, (a.shape[1], a.shape[0]), interpolation=cv2.INTER_AREA)
    if a.shape[1] > max_side:
        scale = max_side / a.shape[1]
        size = (max_side, max(1, int(round(a.shape[0] * scale))))
        a = cv2.resize(a, size, interpolation=cv2.INTER_AREA)
        b = cv2.resize(b, size, interpolation=cv2.INTER_AREA)
    x = cv2.cvtColor(a, cv2.COLOR_BGR2GRAY).astype(np.float32)
    y = cv2.cvtColor(b, cv2.COLOR_BGR2GRAY).astype(np.float32)
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    mx = cv2.GaussianBlur(x, (11, 11), 1.5)
    my = cv2.GaussianBlur(y, (11, 11), 1.5)
    mxx = cv2.GaussianBlur(x * x, (11, 11), 1.5)
    myy = cv2.GaussianBlur(y * y, (11, 11), 1.5)
    mxy = cv2.GaussianBlur(x * y, (11, 11), 1.5)
    sx = mxx - mx * mx
    sy = myy - my * my
    sxy = mxy - mx * my
    num = (2 * mx * my + c1) * (2 * sxy + c2)
    den = (mx * mx + my * my + c1) * (sx + sy + c2)
    return float(np.mean(num / den))


def layout_dict(layout: Layout) -> dict:
    d = asdict(layout)
    d.update(
        cols=layout.cols,
        rows=layout.rows,
        inner=layout.inner,
        nblocks=layout.nblocks,
        ref_w=layout.ref_w,
        ref_h=layout.ref_h,
    )
    return d
