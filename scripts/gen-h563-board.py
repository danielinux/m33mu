#!/usr/bin/env python3
"""Generate the NUCLEO-H563ZI (MB1404 H563ZI-C01) workbench artwork and layout.

Geometry is drawn at 10 px/mm on the 70.00 x 133.34 mm outline from UM3115
Rev 3 Fig. 6. CN7 and CN9 contact centres were measured on the Fig. 6 board
photo (11.54 x 11.57 px/mm, morpho pitch 2.54 mm check, 1.6 % radial
perspective correction for the female housings, +/-0.5 mm); pin 1 is top-left,
odd pins left, numbering downward (Fig. 6 silkscreen, Fig. 17). Morpho CN11/CN12
columns and first row were measured the same way. CN8/CN10 positions and every
other feature are illustrative only and carry no electrical identity.

Only CN7 pin 10 (PA5) and CN9 pin 1 (PA6) are electrically enabled, per
web/boards/nucleo-h563zi-pins.json.

Usage: scripts/gen-h563-board.py   (writes web/public/boards/{svg,layout}/stm32h563.*)
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PX = 10.0
W, H = 70.00, 133.34
PITCH = 2.54

# Pin-1 centres (mm). CN7/CN9/CN11/CN12 measured; CN8/CN10 illustrative.
CONNECTORS = [
    # id, kind, rows, pin1 x, pin1 y, measured, description
    ("CN7", "zio-female", 10, 56.60, 32.51, True, "ST Zio, 2x10 female"),
    ("CN9", "zio-female", 15, 10.57, 64.61, True, "ST Zio, 2x15 female"),
    ("CN8", "zio-female", 8, 10.57, 41.30, False, "ST Zio, 2x8 female (position illustrative)"),
    ("CN10", "zio-female", 17, 56.60, 61.00, False, "ST Zio, 2x17 female (position illustrative)"),
    ("CN11", "morpho-male-unpopulated", 35, 3.16, 31.67, True, "ST morpho, 2x35, not fitted by default"),
    ("CN12", "morpho-male-unpopulated", 35, 64.28, 31.67, True, "ST morpho, 2x35, not fitted by default"),
]

ENABLED = {("CN7", 10): "PA5", ("CN9", 1): "PA6"}
# Same nets, verified in UM3115 Table 25, but morpho headers are not fitted
# by default, so they stay disabled for wiring.
KNOWN = {("CN12", 11): "PA5", ("CN12", 13): "PA6"}


def contacts(cid, rows, x1, y1, measured):
    out = []
    for n in range(1, rows * 2 + 1):
        row = (n - 1) // 2
        col = (n - 1) % 2
        x = round((x1 + col * PITCH) * PX, 2)
        y = round((y1 + row * PITCH) * PX, 2)
        soc = ENABLED.get((cid, n)) or KNOWN.get((cid, n))
        enabled = (cid, n) in ENABLED
        out.append({
            "number": n, "contactId": f"{cid}-{n}", "x": x, "y": y,
            "signal": soc, "mapping_verified": soc is not None,
            "artworkAnchorVerified": measured and soc is not None,
            "electricalEnabled": enabled,
        })
    return out


def svg():
    s = []
    add = s.append
    w, h = W * PX, H * PX
    add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:g} {h:g}" width="{w:g}" height="{h:g}">')
    add('<title>NUCLEO-H563ZI (MB1404 H563ZI-C01), top view, original m33mu illustration</title>')
    add('<defs><radialGradient id="hole"><stop offset="0" stop-color="#050505"/><stop offset="1" stop-color="#2a2a2a"/></radialGradient></defs>')
    add(f'<rect x="1" y="1" width="{w-2:g}" height="{h-2:g}" rx="18" fill="#eef1f6" stroke="#9aa6b8" stroke-width="2"/>')
    # ST-LINK section boundary and parts.
    add(f'<rect x="150" y="0" width="400" height="230" fill="#e4e8f0"/>')
    add('<line x1="20" y1="232" x2="680" y2="232" stroke="#2b5cab" stroke-width="2" stroke-dasharray="10 6"/>')
    add('<rect x="305" y="0" width="90" height="70" rx="8" fill="#b8bec8" stroke="#6c7480" stroke-width="2"/>')
    add('<rect x="300" y="110" width="100" height="100" fill="#1f2328"/>')
    # Main MCU (LQFP144, 20 mm) roughly central.
    add('<g transform="translate(350 640) rotate(45)"><rect x="-100" y="-100" width="200" height="200" fill="#202428"/>'
        '<circle cx="-80" cy="-80" r="6" fill="#3a3f45"/></g>')
    # Bottom: RJ45 and user USB-C.
    add('<rect x="380" y="1150" width="160" height="183" fill="#c9ced6" stroke="#6c7480" stroke-width="2"/>')
    add('<rect x="180" y="1270" width="90" height="63" rx="8" fill="#b8bec8" stroke="#6c7480" stroke-width="2"/>')
    # Buttons and LEDs (illustrative).
    for cx, fill in ((200, "#3b73c8"), (500, "#202020")):
        add(f'<rect x="{cx-35}" y="1060" width="70" height="70" rx="8" fill="#d0d4da"/><circle cx="{cx}" cy="1095" r="22" fill="{fill}"/>')
    for i, color in enumerate(("#2fb34a", "#e9b300", "#d62d2d")):
        add(f'<rect x="{290 + i * 40}" y="1000" width="16" height="10" fill="{color}"/>')
    for cid, kind, rows, x1, y1, measured, _ in CONNECTORS:
        x0 = (x1 - PITCH / 2) * PX
        y0 = (y1 - PITCH / 2) * PX
        cw = 2 * PITCH * PX
        ch = rows * PITCH * PX
        if kind == "zio-female":
            add(f'<rect x="{x0-2:g}" y="{y0-2:g}" width="{cw+4:g}" height="{ch+4:g}" rx="2" fill="#16181b"/>')
            label_y = y0 - 8
        else:
            label_y = y0 - 8
        add(f'<text x="{x0 + cw / 2:g}" y="{label_y:g}" font-family="sans-serif" font-size="16" '
            f'text-anchor="middle" fill="#2b5cab">{cid}</text>')
        for c in contacts(cid, rows, x1, y1, measured):
            if kind == "zio-female":
                add(f'<rect x="{c["x"]-8:g}" y="{c["y"]-8:g}" width="16" height="16" fill="url(#hole)"/>')
            else:
                shape = "rect" if c["number"] == 1 else "circle"
                if shape == "rect":
                    add(f'<rect x="{c["x"]-9:g}" y="{c["y"]-9:g}" width="18" height="18" fill="#c99a3b"/>')
                else:
                    add(f'<circle cx="{c["x"]:g}" cy="{c["y"]:g}" r="9" fill="#c99a3b"/>')
                add(f'<circle cx="{c["x"]:g}" cy="{c["y"]:g}" r="4.5" fill="#1a1a1a"/>')
    add('<text x="350" y="1020" font-family="sans-serif" font-size="18" text-anchor="middle" fill="#2b5cab">MB1404</text>')
    add('</svg>')
    return "\n".join(s) + "\n"


def layout():
    return {
        "schemaVersion": 1, "cpu": "stm32h563", "boardName": "NUCLEO-H563ZI",
        "boardRevision": "MB1404 H563ZI-C01",
        "boardFamily": "nucleo144", "manufacturer": "STMicroelectronics",
        "manufacturerBoardVerified": True,
        "manufacturerSource": "https://www.st.com/en/evaluation-tools/nucleo-h563zi.html",
        "artwork": "../svg/stm32h563.svg", "pixelViewbox": [0, 0, W * PX, H * PX],
        "pixelsPerMm": PX, "pinLabelArtwork": False,
        "geometryStatus": "outline-to-scale; CN7/CN9/CN11/CN12 measured from UM3115 Rev 3 Fig. 6 (+/-0.5 mm); other features illustrative",
        "electricalMapStatus": "verified for CN7-10 (PA5) and CN9-1 (PA6) only",
        "warning": "Only CN7 pin 10 (PA5) and CN9 pin 1 (PA6) are electrically enabled. Other contacts are visual locators.",
        "electricalEnabledByDefault": False,
        "generator": "scripts/gen-h563-board.py",
        "connectors": [
            {"id": cid, "kind": kind, "rows": rows, "columns": 2,
             "description": desc, "geometryMeasured": measured,
             "contacts": contacts(cid, rows, x1, y1, measured)}
            for cid, kind, rows, x1, y1, measured, desc in CONNECTORS
        ],
    }


def main():
    base = ROOT / "web" / "public" / "boards"
    (base / "svg" / "stm32h563.svg").write_text(svg(), encoding="utf-8")
    (base / "layout" / "stm32h563.json").write_text(
        json.dumps(layout(), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
