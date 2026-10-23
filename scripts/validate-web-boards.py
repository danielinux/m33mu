#!/usr/bin/env python3
"""Validate the supplied board sprite pack and local electrical-map overrides."""

import argparse
import json
import math
import os
import sys
import zipfile


def fail(message):
    print("board validation: " + message, file=sys.stderr)
    return 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sprite_pack", help="path to m33mu_board_sprites.zip")
    args = parser.parse_args()

    try:
        archive = zipfile.ZipFile(args.sprite_pack)
        manifest = json.loads(archive.read("manifest.json"))
    except (OSError, KeyError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        return fail("cannot read manifest: " + str(exc))

    boards = manifest.get("boards")
    if not isinstance(boards, list) or len(boards) != 15:
        return fail("manifest must list all 15 supported CPU profiles")
    cpu_ids = [board.get("cpu") for board in boards]
    if len(set(cpu_ids)) != len(cpu_ids) or None in cpu_ids:
        return fail("manifest CPU identifiers must be unique and non-empty")
    if manifest.get("rules", {}).get("wiringRequiresVerifiedElectricalMapping") is not True:
        return fail("manifest must require verified mapping before wiring")
    if manifest.get("rules", {}).get("defaultElectricalEnabled") is not False:
        return fail("unverified board contacts must be disabled by default")

    for summary in boards:
        cpu = summary["cpu"]
        try:
            layout = json.loads(archive.read("layout/" + cpu + ".json"))
            archive.getinfo("svg/" + cpu + ".svg")
            archive.getinfo("png/" + cpu + ".png")
        except (KeyError, json.JSONDecodeError) as exc:
            return fail(cpu + " has a missing or invalid layout/art asset: " + str(exc))
        if layout.get("cpu") != cpu:
            return fail(cpu + " layout CPU does not match its manifest entry")
        viewbox = layout.get("pixelViewbox")
        if not isinstance(viewbox, list) or len(viewbox) != 4:
            return fail(cpu + " has an invalid pixelViewbox")
        x0, y0, width, height = viewbox
        if width <= 0 or height <= 0:
            return fail(cpu + " has a non-positive viewBox size")

        connector_ids = set()
        total = 0
        for connector in layout.get("connectors", []):
            connector_id = connector.get("id")
            if not connector_id or connector_id in connector_ids:
                return fail(cpu + " has a missing or duplicate connector ID")
            connector_ids.add(connector_id)
            numbers = set()
            for contact in connector.get("contacts", []):
                total += 1
                number = contact.get("number")
                x, y = contact.get("x"), contact.get("y")
                if not isinstance(number, int) or number < 1 or number in numbers:
                    return fail(cpu + "/" + connector_id + " has an invalid or duplicate contact number")
                numbers.add(number)
                if not all(isinstance(v, (int, float)) and math.isfinite(v) for v in (x, y)):
                    return fail(cpu + "/" + connector_id + " contact has invalid coordinates")
                if not (x0 <= x <= x0 + width and y0 <= y <= y0 + height):
                    return fail(cpu + "/" + connector_id + " contact is outside the viewBox")
                if contact.get("mapping_verified") is not True and contact.get("signal") is not None:
                    return fail(cpu + "/" + connector_id + " has an unverified non-null signal")
        if total != summary.get("contacts"):
            return fail(cpu + " contact count differs from the manifest")

    override_path = os.path.join(os.path.dirname(__file__), "..", "web", "boards", "nucleo-h563zi-pins.json")
    try:
        override = json.load(open(override_path, encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return fail("cannot read H563 pin mapping: " + str(exc))
    if override.get("cpu") != "stm32h563" or override.get("boardId") != "nucleo-h563zi":
        return fail("H563 pin mapping identifies the wrong target")
    override_contacts = override.get("contacts", [])
    seen = set()
    for contact in override_contacts:
        key = (contact.get("connectorId"), contact.get("physicalPadNumber"))
        if not key[0] or not isinstance(key[1], int) or key[1] < 1 or key in seen:
            return fail("H563 pin mapping has an invalid or duplicate physical contact")
        seen.add(key)
        if contact.get("electricalEnabled") and not (
            contact.get("signalMappingVerified") and contact.get("artworkAnchorVerified")
        ):
            return fail("H563 contact is enabled before both mapping and artwork review")
    if not override_contacts:
        return fail("H563 pin mapping must declare at least one contact")

    print("board validation: 15 layouts and " + str(len(override_contacts)) + " H563 mappings valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
