#!/usr/bin/env python3
"""Validate web contracts and representative accepted/rejected payloads."""

import json
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = ROOT / "web" / "project"
FORMAT_CHECKER = FormatChecker()


def load_schema(name):
    schema = json.loads((SCHEMA_DIR / name).read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    return schema


def check(schema, value, expected_valid, description):
    errors = list(Draft202012Validator(schema, format_checker=FORMAT_CHECKER).iter_errors(value))
    if bool(errors) == expected_valid:
        details = "; ".join(error.message for error in errors[:3])
        raise SystemExit(f"schema check failed for {description}: {details or 'unexpectedly valid'}")


def main():
    project_schema = load_schema("project.schema.json")
    project = {
        "schemaVersion": 1,
        "projectId": "schema-smoke",
        "simulator": {"cpu": "stm32h563", "firmwareFormat": "elf", "firmwareSha256": None},
        "boards": [], "contacts": [], "nets": [], "components": [], "instruments": []
    }
    check(project_schema, project, True, "minimal project")
    project_bad = {**project, "simulator": {**project["simulator"], "cpu": "unknown"}}
    check(project_schema, project_bad, False, "unsupported project CPU")

    project_v2_schema = load_schema("project-v2.schema.json")
    for path in sorted((ROOT / "web" / "public" / "examples").glob("*.m33mu.json")):
        check(project_v2_schema, json.loads(path.read_text(encoding="utf-8")), True,
              f"v2 example {path.name}")
    v2_bad = json.loads((ROOT / "web" / "public" / "examples" /
                         "opamp-gain.m33mu.json").read_text(encoding="utf-8"))
    v2_bad["instruments"]["scope"]["trigger"] = {
        "channel": 2, "edge": "up", "level": "1 V"}
    check(project_v2_schema, v2_bad, False, "invalid v2 scope trigger")

    pad_schema = load_schema("board-pad.schema.json")
    pad = json.loads((ROOT / "web" / "boards" / "nucleo-h563zi-pins.json").read_text(encoding="utf-8"))
    check(pad_schema, pad, True, "H563 pin evidence")
    pad_bad = json.loads(json.dumps(pad))
    pad_bad["contacts"][0]["artworkAnchorVerified"] = False
    check(pad_schema, pad_bad, False, "enabled contact without verified artwork anchor")

    capability_schema = load_schema("capabilities.schema.json")
    capabilities = {"schemaVersion": 1, "cpu": "stm32h563", "firmwareFormats": ["elf"], "sessionCount": 1, "features": {"instructionStep": True, "analogPadVoltage": False}}
    check(capability_schema, capabilities, True, "H563 capability response")

    worker_schema = load_schema("worker-message.schema.json")
    request = {"schemaVersion": 1, "kind": "request", "requestId": 1, "operation": "step", "payload": {}}
    check(worker_schema, request, True, "worker request")
    request_bad = {**request, "operation": "eval"}
    check(worker_schema, request_bad, False, "unknown worker operation")
    bench_request = {"schemaVersion": 1, "kind": "request", "requestId": 2, "operation": "createBench",
                     "payload": {"netlist": {"schemaVersion": 1, "devices": [
                         {"kind": "resistor", "id": "r1", "a": "n", "b": "gnd", "ohms": 1000}]},
                         "probes": [{"id": "v", "kind": "voltage", "net": "n"}]}}
    check(worker_schema, bench_request, True, "createBench request")
    opamp_request = json.loads(json.dumps(bench_request))
    opamp_request["payload"]["netlist"]["devices"] = [
        {"kind": "opamp", "id": "u1", "plus": "p", "minus": "m", "out": "o",
         "vcc": "vcc", "vee": "gnd",
         "model": {"gain": 100000, "rOut": 20, "rIn": 1000000000, "headroom": 0.2}}
    ]
    check(worker_schema, opamp_request, True, "op-amp createBench request")
    check(worker_schema, {**bench_request, "payload": {"netlist": {"schemaVersion": 1, "devices": [
        {"kind": "transformer", "id": "t"}]}}}, False, "unsupported device kind")
    check(worker_schema, {**bench_request, "operation": "runUntil", "payload": {"targetNs": "-5"}},
          False, "negative runUntil target")

    print("web schema validation: 5 schemas valid; v1/v2 projects, board, capability and worker fixtures pass")


if __name__ == "__main__":
    main()
