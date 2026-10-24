#!/usr/bin/env python3
"""Generate the NUCLEO-H563ZI (MB1404 H563ZI-C01) workbench artwork and layout.

Geometry is drawn at 10 px/mm on the 70.00 x 133.34 mm outline from UM3115
Rev 3 Fig. 6. CN7 and CN9 contact centres were measured on the Fig. 6 board
photo (11.54 x 11.57 px/mm, morpho pitch 2.54 mm check, 1.6 % radial
perspective correction for the female housings, +/-0.5 mm); pin 1 is top-left,
odd pins left, numbering downward (Fig. 6 silkscreen, Fig. 17). Morpho CN11/CN12
columns and first row were measured the same way. CN8/CN10 positions and every
other feature are illustrative only and carry no electrical identity.

Every Zio (CN7-CN10) and morpho (CN11/CN12) contact carries its UM3115 Rev 3
Table 21-25 net. Contacts on a non-default solder bridge, debug/clock pins and
unmodelled nets stay visual-only; web/boards/nucleo-h563zi-pins.json mirrors it.

Usage: scripts/gen-h563-board.py   (writes web/public/boards/{svg,layout}/stm32h563.*)
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PX = 10.0
W, H = 70.00, 133.34
PITCH = 2.54

# Pin-1 centres (mm), all measured on the UM3115 Fig. 6 photo. CN8 and CN10
# were measured later with the same calibration; their columns agree with
# CN9 and CN7 to 0.1 mm.
CONNECTORS = [
    # id, kind, rows, pin1 x, pin1 y, measured, description
    ("CN7", "zio-female", 10, 56.60, 32.51, True, "ST Zio, 2x10 female"),
    ("CN9", "zio-female", 15, 10.57, 64.61, True, "ST Zio, 2x15 female"),
    ("CN8", "zio-female", 8, 10.57, 41.91, True, "ST Zio, 2x8 female"),
    ("CN10", "zio-female", 17, 56.60, 59.69, True, "ST Zio, 2x17 female"),
    ("CN11", "morpho-male-unpopulated", 35, 3.16, 31.67, True, "ST morpho, 2x35, not fitted by default"),
    ("CN12", "morpho-male-unpopulated", 35, 64.28, 31.67, True, "ST morpho, 2x35, not fitted by default"),
]

# UM3115 Rev 3 Tables 21-24 (Zio) and Table 25 (morpho): contact -> SoC pad
# or board net. Board nets: GND, 3V3 and 5V are modelled; VDD, IOREF and AVDD
# are the 3V3 domain (SB16 default, VDDA from 3V3); AGND is GND.
ZIO = {
    "CN7": ["PC6", "PB8", "PB15", "PB9", "PB13", "VREFP", "PB12", "GND", "PA15", "PA5",
            "PC7", "PG9", "PB5", "PB5", "PB3", "PD14", "PG10", "PD15", "PB4", "PF3"],
    "CN8": ["NC", "PC8", "IOREF", "PC9", "NRST", "PC10", "3V3", "PC11", "5V", "PC12",
            "GND", "PD2", "GND", "PG2", "VIN", "PG3"],
    "CN9": ["PA6", "PD7", "PC0", "PD6", "PC3", "PD5", "PB1", "PD4", "PC2", "PD3",
            "PF11", "GND", "PB2", "PE2", "PE9", "PE4", "PF2", "PE5", "PF1", "PE6",
            "PF0", "PE3", "GND", "PF8", "PD0", "PF7", "PD1", "PF9", "PG0", "PG1"],
    "CN10": ["AVDD", "PG12", "AGND", "PE9", "GND", "PE11", "PF12", "PE14", "PF13", "PE13",
             "PF14", "PG14", "PG6", "PB6", "PB2", "PB7", "GND", "PE8", "PD13", "PE7",
             "PD12", "GND", "PD11", "PE10", "PE2", "PE12", "GND", "PE6", "PA0", "PE15",
             "PB0", "PB10", "PE0", "PA3"],
}
CN11 = ["PC10", "PC11", "PC12", "PD2", "VDD", "5V_EXT", "BOOT0", "GND", "PF6", "NC",
        "PF7", "IOREF", "PA13", "NRST", "PA14", "3V3", "PA15", "5V", "GND", "GND",
        "PB7", "GND", "PC13", "VIN", "PC14", "NC", "PC15", "PA0", "PH0", "PA1",
        "PH1", "PA4", "VBAT", "PB0", "PC2", "PC1", "PC3", "PC0", "PD4", "PD3",
        "PD5", "PG2", "PD6", "PG3", "PD7", "PE2", "PE3", "PE4", "GND", "PE5",
        "PF1", "PF2", "PF0", "PF8", "PD1", "PF9", "PD0", "PG1", "PG0", "GND",
        "NC", "PE6", "PG9", "PG15", "PG12", "PG10", "NC", "PG13", "PD9", "PG11"]
CN12 = ["PC9", "PC8", "PB8", "PC6", "PB9", "PC5", "VREFP", "5V_STLK", "GND", "PD8",
        "PA5", "PA12", "PA6", "PA11", "PA7", "PB12", "PB6", "NC", "PC7", "GND",
        "PA9", "PB2", "PA8", "PB1", "PB10", "PB15", "PB4", "PB14", "PB5", "PB13",
        "PB3", "AGND", "PA10", "PC4", "PA2", "PF5", "PA3", "PF4", "GND", "PE8",
        "PD13", "PF10", "PD12", "PE7", "PD11", "PD14", "PE10", "PD15", "PE12", "PF14",
        "PE14", "PE9", "PE15", "GND", "PE13", "PE11", "PF13", "PF3", "PF12", "PF15",
        "PG14", "PF11", "GND", "PE0", "PD10", "PG8", "PG7", "PG5", "PG4", "PG6"]
NETS = {**{c: dict(enumerate(v, 1)) for c, v in ZIO.items()},
        "CN11": dict(enumerate(CN11, 1)), "CN12": dict(enumerate(CN12, 1))}
RAIL_ALIAS = {"GND": "GND", "AGND": "GND", "3V3": "3V3", "VDD": "3V3", "IOREF": "3V3",
              "AVDD": "3V3", "5V": "5V"}
# Contacts that stay visual-only, with the reason (UM3115 Rev 3 sections 9-11,
# Table 17). Unmodelled nets (NC, VIN, NRST, BOOT0, VBAT, VREFP, 5V_EXT,
# 5V_STLK) are always visual-only.
DISABLED = {
    ("CN7", 3): "PB15 reaches CN7-3 only through SB10; it is the RMII_TXD1 line by default",
    ("CN7", 5): "PB13 reaches CN7-5 only through SB12; it is UCPD_CC1 by default",
    ("CN7", 13): "PB5 reaches CN7-13 only through SB17 (CN7-14 carries it by default)",
    ("CN11", 13): "PA13 is SWDIO, wired to the on-board STLINK-V3EC",
    ("CN11", 15): "PA14 is SWCLK, wired to the on-board STLINK-V3EC",
    ("CN11", 25): "PC14 is the LSE crystal input (X2) by default",
    ("CN11", 27): "PC15 is the LSE crystal output (X2) by default",
    ("CN11", 29): "PH0 receives the STLINK-V3EC 8 MHz MCO by default",
    ("CN11", 31): "PH1 is OSC_OUT; HSE pins are not GPIO by default",
    ("CN12", 10): "PD8 reaches CN12-10 only through SB13; it is the VCP USART3_TX by default",
    ("CN12", 12): "PA12 is USB_DP on CN13 by default (SB22 off)",
    ("CN12", 14): "PA11 is USB_DM on CN13 by default (SB21 off)",
}
# Board loads on a pad that the bench does not model electrically yet.
NOTES = {
    "PB0": "LD1 green user LED (SB43 default)", "PF4": "LD2 yellow user LED",
    "PG4": "LD3 red user LED", "PC13": "B1 user button (SB54 default)",
    "PD9": "VCP USART3_RX: driven by the STLINK-V3EC on the real board",
    "PA1": "Ethernet PHY RMII_REF_CLK", "PA2": "Ethernet PHY RMII_MDIO",
    "PC1": "Ethernet PHY RMII_MDC", "PA7": "Ethernet PHY RMII_CRS_DV",
    "PC4": "Ethernet PHY RMII_RXD0", "PC5": "Ethernet PHY RMII_RXD1",
    "PG11": "Ethernet PHY RMII_TX_EN", "PG13": "Ethernet PHY RMII_TXD0",
    "PB15": "Ethernet PHY RMII_TXD1", "PB13": "UCPD_CC1 / Ethernet RMII_TXD1 via JP6",
    "PB14": "UCPD_CC2", "PC2": "A4 default (SB52); PB9 via SB53",
    "PF11": "A5 default (SB57); PB8 via SB60",
}
# Function of selected SoC pads on this board (Tables 21-24).
FUNCTIONS = {
    "PB8": "I2C1_SCL", "PB9": "I2C1_SDA", "PA5": "SPI1_SCK", "PG9": "SPI1_MISO",
    "PB5": "SPI1_MOSI/TIM3_CH2", "PD14": "SPI1_CS/TIM4_CH3", "PD15": "TIM4_CH4",
    "PA6": "ADC12_INP3", "PD7": "USART2_CK",
    "PD6": "USART2_RX", "PD5": "USART2_TX", "PD4": "USART2_RTS", "PD3": "USART2_CTS",
    "PF1": "I2C2_SCL", "PF0": "I2C2_SDA", "PF2": "I2C2_SMBA",
    "PB6": "LPUART1_TX", "PB7": "LPUART1_RX", "PE9": "TIM1_CH1", "PE11": "TIM1_CH2",
    "PE13": "TIM1_CH3", "PE8": "TIM1_CH1N", "PE10": "TIM1_CH2N", "PE12": "TIM1_CH3N",
    "PE7": "TIM1_ETR", "PA0": "TIM2_CH1", "PB10": "TIM2_CH3", "PA3": "TIM2_CH4",
    "PB0": "TIM3_CH3", "PE0": "TIM4_ETR", "PD0": "FDCAN1_RX", "PD1": "FDCAN1_TX",
    "PC0": "ADC12_INP10", "PC3": "ADC12_INP13", "PB1": "ADC12_INP5", "PC2": "ADC12_INP12",
    "PF11": "ADC1_INP2", "PF12": "ADC1_INP6", "PF13": "ADC2_INP2", "PF14": "ADC2_INP6",
    "PD8": "USART3_TX (VCP)", "PD9": "USART3_RX (VCP)",
}
# On-board user LEDs (UM3115 Rev 3 section 11.2): anode driven by the pad.
LEDS = [("LD1", "PB0", "#2fb34a"), ("LD2", "PF4", "#e9b300"), ("LD3", "PG4", "#d62d2d")]


def electrical(cid, n):
    """(signal, enabled, note) for one contact."""
    net = NETS[cid][n]
    if net in RAIL_ALIAS:
        return RAIL_ALIAS[net], True, None if net == RAIL_ALIAS[net] else f"{net} (board {RAIL_ALIAS[net]} net)"
    if not re.fullmatch(r"P[A-I][0-9]+", net):
        return None, False, f"{net}: not modelled on the bench"
    if (cid, n) in DISABLED:
        return net, False, DISABLED[(cid, n)]
    return net, True, NOTES.get(net)


def contacts(cid, rows, x1, y1, measured):
    out = []
    for n in range(1, rows * 2 + 1):
        row = (n - 1) // 2
        col = (n - 1) % 2
        x = round((x1 + col * PITCH) * PX, 2)
        y = round((y1 + row * PITCH) * PX, 2)
        soc, enabled, note = electrical(cid, n)
        out.append({
            "number": n, "contactId": f"{cid}-{n}", "x": x, "y": y,
            "signal": soc, "net": NETS[cid][n], "mapping_verified": True,
            "artworkAnchorVerified": measured,
            "electricalEnabled": enabled,
            **({"function": FUNCTIONS[soc]} if soc in FUNCTIONS else {}),
            **({"note": note} if note else {}),
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
    # User LEDs are drawn unlit; the workbench lights them from the pad.
    for i, (name, _, _) in enumerate(LEDS):
        add(f'<rect x="{290 + i * 40}" y="1000" width="16" height="10" fill="#3a3d42"/>')
        add(f'<text x="{298 + i * 40}" y="994" font-family="sans-serif" font-size="10" text-anchor="middle" fill="#2b5cab">{name}</text>')
    for cid, kind, rows, x1, y1, measured, _ in CONNECTORS:
        x0 = (x1 - PITCH / 2) * PX
        y0 = (y1 - PITCH / 2) * PX
        cw = 2 * PITCH * PX
        ch = rows * PITCH * PX
        if kind == "zio-female":
            add(f'<rect x="{x0-2:g}" y="{y0-2:g}" width="{cw+4:g}" height="{ch+4:g}" rx="2" fill="#16181b"/>')
            # Label beside the housing, on the board-centre side, so stacked
            # headers do not overlap.
            left = x0 + cw / 2 < W * PX / 2
            add(f'<text x="{x0 + cw + 6 if left else x0 - 6:g}" y="{y0 + 12:g}" font-family="sans-serif" '
                f'font-size="16" text-anchor="{"start" if left else "end"}" fill="#2b5cab">{cid}</text>')
        else:
            add(f'<text x="{x0 + cw / 2:g}" y="{y0 - 8:g}" font-family="sans-serif" font-size="16" '
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
        "geometryStatus": "outline-to-scale; CN7-CN12 measured from UM3115 Rev 3 Fig. 6 (+/-0.5 mm); other features illustrative",
        "electricalMapStatus": "verified: every Zio and morpho contact from UM3115 Rev 3 Tables 21-25; contacts with a non-default solder bridge, debug/clock or unmodelled net stay visual-only (see note)",
        "warning": "Only contacts marked electricalEnabled can be wired; the others are visual locators.",
        "electricalEnabledByDefault": False,
        "generator": "scripts/gen-h563-board.py",
        "leds": [{"id": name, "pad": pad, "color": color, "x": 298 + i * 40, "y": 1005, "w": 16, "h": 10,
                  "activeHigh": True} for i, (name, pad, color) in enumerate(LEDS)],
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
    # Reviewed mapping, regenerated from the same tables.
    pins_path = ROOT / "web" / "boards" / "nucleo-h563zi-pins.json"
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    out = []
    for cid, _, rows, x1, y1, measured, _ in CONNECTORS:
        for c in contacts(cid, rows, x1, y1, measured):
            entry = {"connectorId": cid, "physicalPadNumber": c["number"], "net": c["net"]}
            if c["signal"] in ("GND", "3V3", "5V"):
                entry["rail"] = c["signal"]
            elif c["signal"]:
                entry["socPad"] = c["signal"]
            entry["peripheralFunctions"] = [c["function"]] if "function" in c else []
            entry.update({"signalMappingVerified": True, "artworkAnchorVerified": measured,
                          "electricalEnabled": c["electricalEnabled"], "artworkContactId": c["contactId"]})
            if "note" in c:
                entry["note"] = c["note"]
            out.append(entry)
    pins["contacts"] = out
    pins["review"]["note"] = (
        "Every Zio (CN7-CN10) and morpho (CN11/CN12) contact mapped from UM3115 Rev 3 Tables 21-25, "
        "generated by scripts/gen-h563-board.py. Contacts that need a non-default solder bridge (Table 17), "
        "SWD/clock pins and unmodelled nets (NC, VIN, NRST, BOOT0, VBAT, VREFP, 5V_EXT, 5V_STLK) stay "
        "visual-only; on-board loads are listed per contact. Contact centres are on the measured 2.54 mm "
        "grid from Fig. 6 (+/-0.5 mm); a CAD-exact check against the MB1404 schematic remains open.")
    pins_path.write_text(json.dumps(pins, indent=1) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
