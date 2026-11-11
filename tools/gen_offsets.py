#!/usr/bin/env python3
"""Generate managed field offsets and candidate static globals from an IL2CPP dump.

Inputs: types.csv, fields.csv, methods.csv, manifest.json and the matching
GameAssembly.dll. Field offsets come from the dump. Static-global candidates
come from RIP-relative references in method bytes; vote counts are evidence
for inspection, not proof that a candidate identifies an active instance.

The legacy default header path is 2_src_cheat/robrun_offsets.h. In this public
tree, supply --header 2_src_port/robrun_offsets.h explicitly. The JSON report
is written to offsets/robrun_offsets.json. See BUILD.md for commands and inputs.
"""

import argparse
import collections
import json
import hashlib
import re
import struct
import sys
from pathlib import Path

# Field specification.
# Each row maps a macro name to a managed class and field name.
FIELD_SPEC = [
    # World and entity graph.
    ("RR_OFF_WORLD_ENTITIES",       "WorldModel",              "WorldEntitiesModel"),
    ("RR_OFF_WEM_BATTLE_CHARS",     "WorldEntitiesModel",      "_battleCharacters"),
    ("RR_OFF_WEM_VEHICLES",         "WorldEntitiesModel",      "_vehicles"),
    # Character model.
    ("RR_OFF_CHAR_ID",              "BattleCharacterModel",    "Id"),
    ("RR_OFF_CHAR_DATA",            "BattleCharacterModel",    "Data"),
    ("RR_OFF_CHAR_CONFIG",          "BattleCharacterModel",    "Config"),
    ("RR_OFF_CHAR_TRANSFORM",       "BattleCharacterModel",    "Transform"),
    ("RR_OFF_CHAR_REPL_TRANSFORM",  "BattleCharacterModel",    "ReplicatedTransform"),
    ("RR_OFF_CHAR_ISDEAD",          "BattleCharacterModel",    "IsDead"),
    ("RR_OFF_CHAR_PRIVATE",         "BattleCharacterModel",    "PrivateModel"),
    # Transform models.
    # CharacterTransformModel stores plain floats in the sampled build.
    # Resolve their offsets from this dump rather than copying sample values.
    ("RR_OFF_CTM_X",                "CharacterTransformModel", "X"),
    ("RR_OFF_CTM_Y",                "CharacterTransformModel", "Y"),
    ("RR_OFF_CTM_Z",                "CharacterTransformModel", "Z"),
    ("RR_OFF_CTM_PITCH",            "CharacterTransformModel", "Pitch"),
    ("RR_OFF_CTM_YAW",              "CharacterTransformModel", "Yaw"),
    ("RR_OFF_CTM_ROLL",             "CharacterTransformModel", "Roll"),
    ("RR_OFF_TRANSFORM_X",          "TransformModel",          "_x"),
    ("RR_OFF_TRANSFORM_Y",          "TransformModel",          "_y"),
    ("RR_OFF_TRANSFORM_Z",          "TransformModel",          "_z"),
    ("RR_OFF_TRANSFORM_RX",         "TransformModel",          "_rx"),
    ("RR_OFF_TRANSFORM_RY",         "TransformModel",          "_ry"),
    ("RR_OFF_TRANSFORM_RZ",         "TransformModel",          "_rz"),
    ("RR_OFF_RTRANSFORM_POS",       "CharacterReplicatedTransformModel", "_position"),
    ("RR_OFF_RTRANSFORM_RX",        "CharacterReplicatedTransformModel", "_rx"),
    ("RR_OFF_RTRANSFORM_RY",        "CharacterReplicatedTransformModel", "_ry"),
    ("RR_OFF_RTRANSFORM_RZ",        "CharacterReplicatedTransformModel", "_rz"),
    # Shared, private and public character data.
    ("RR_OFF_DATA_COMMON",          "BattleCharacterData",      "CommonData"),
    ("RR_OFF_DATA_PRIVATE",         "BattleCharacterData",      "PrivateData"),
    ("RR_OFF_DATA_PUBLIC",          "BattleCharacterData",      "PublicData"),
    ("RR_OFF_COMMON_ROLE",          "BattleCharacterCommonData","Role"),
    ("RR_OFF_PRIVATE_HEALTH",       "BattleCharacterPrivateData","Health"),
    ("RR_OFF_PUBLIC_ARMOR",         "BattleCharacterPublicData","HasArmor"),
    ("RR_OFF_CHAR_WEAPON",          "BattleCharacterModel",     "Weapon"),
    ("RR_OFF_CHAR_CROUCH",          "BattleCharacterModel",     "CrouchModel"),
    ("RR_OFF_CHAR_STANCE",          "BattleCharacterModel",     "BattleStanceModel"),
    ("RR_OFF_CHAR_PBM",             "BattleCharacterModel",     "PlayerBattleModel"),
    ("RR_OFF_WEAPON_ITEM",          "WeaponModel",              "_itemInHands"),
    ("RR_OFF_CROUCH_BOOL",          "CrouchModel",              "_isCrouching"),
    ("RR_OFF_STANCE_REF",           "BattleStanceModel",        "_battleStance"),
    ("RR_OFF_PBM_DATA",             "PlayerBattleModel",        "Data"),
    # Camera fields.
    ("RR_OFF_CAM_POS",              "BattleCameraModel",       "Position"),
    ("RR_OFF_CAM_ROT",              "BattleCameraModel",       "Orientation"),
    ("RR_OFF_CAM_FWD",              "BattleCameraModel",       "Forward"),
    ("RR_OFF_CAM_RIGHT",            "BattleCameraModel",       "Right"),
    ("RR_OFF_CAM_UP",               "BattleCameraModel",       "Up"),
    ("RR_OFF_CAM_DIST",             "BattleCameraModel",       "DistanceToFollowTarget"),
    # Health model fields.
    ("RR_OFF_HEALTH_VALUE",         "HealthModel",             "_health"),
    ("RR_OFF_HEALTH_DEAD",          "HealthModel",             "_isDead"),
    ("RR_OFF_HEALTH_MAX",           "HealthModel",             "_maxAmount"),
]

# Classes used for candidate static-global searches in method bytes.
STATIC_CLASSES = [
    "WorldModel",
    "BattleCameraModel",
    "WorldEntitiesModel",
    "BattleCharacterModel",
]

MAX_METHODS_PER_CLASS = 400
SCAN_BYTES = 48


def load_csv(path):
    import csv
    with path.open(encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


class PE:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit("not a PE file")
        self.base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        sec_at = pe + 24 + opt_size
        self.sections = []
        for i in range(nsec):
            at = sec_at + i * 40
            vsize, rva, rawsize, raw = struct.unpack_from("<IIII", data, at + 8)
            flags = struct.unpack_from("<I", data, at + 36)[0]
            self.sections.append({
                "name": data[at:at + 8].rstrip(b"\0").decode("latin1"),
                "vsize": vsize, "rva": rva, "rawsize": rawsize, "raw": raw,
                "flags": flags,
                "exec": bool(flags & 0x20000000),
                "write": bool(flags & 0x80000000),
                "read": bool(flags & 0x40000000),
            })

    def section_of_rva(self, rva):
        for s in self.sections:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
                return s
        return None

    def file_offset(self, rva):
        s = self.section_of_rva(rva)
        if not s:
            return None
        off = s["raw"] + (rva - s["rva"])
        return off if 0 <= off < len(self.data) else None

    def bytes_at_rva(self, rva, n):
        off = self.file_offset(rva)
        if off is None:
            return b""
        return self.data[off:off + n]


def rip_targets(chunk, rva):
    """Yield (instruction_rva, target_rva) for mov/lea reg,[rip+disp32]."""
    i = 0
    while i + 7 <= len(chunk):
        b0, b1, b2 = chunk[i], chunk[i + 1], chunk[i + 2]
        if b0 in (0x48, 0x4C) and b1 in (0x8B, 0x8D, 0x8A) and (b2 & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", chunk, i + 3)[0]
            yield rva + i, rva + i + 7 + disp
            i += 7
            continue
        i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    ap.add_argument("--dump", default=None)
    ap.add_argument("--header", default=None, help="output header path")
    args = ap.parse_args()

    root = Path(args.root)
    dump = Path(args.dump) if args.dump else root / "analysis" / "dump"
    out_h = Path(args.header) if args.header else Path(__file__).resolve().parent.parent / "2_src_cheat" / "robrun_offsets.h"
    out_j = Path(__file__).resolve().parent.parent / "offsets" / "robrun_offsets.json"
    out_j.parent.mkdir(parents=True, exist_ok=True)

    types = load_csv(dump / "types.csv")
    fields = load_csv(dump / "fields.csv")
    methods = load_csv(dump / "methods.csv")
    binary = (root / "GameAssembly.dll").read_bytes()
    manifest = json.loads((dump / "manifest.json").read_text(encoding="utf-8"))
    if hashlib.sha256(binary).hexdigest() != manifest["input_sha256"]["GameAssembly.dll"]:
        raise SystemExit("GameAssembly.dll does not match dump manifest")
    pe = PE(binary)

    by_type = collections.defaultdict(list)
    for f in fields:
        by_type[f["type_declaration"]].append(f)

    def class_rows(name):
        return [k for k in by_type if re.search(r"\bclass\s+" + re.escape(name) + r"\b", k)]

    resolved = {}
    missing = []
    for macro, cls, fld in FIELD_SPEC:
        cands = class_rows(cls)
        want = fld
        hit = None
        owner = cls
        for k in cands:
            for row in by_type[k]:
                decl = row["declaration"]
                # Match compiler-generated backing fields, such as <Bar>k__BackingField.
                names = re.findall(r"<(\w+)>k__BackingField", decl)
                plain = decl.split(";")[0].strip().split()[-1] if decl.strip() else ""
                if want in names or plain == want or plain.lstrip("_") == want.lstrip("_"):
                    if row["offset_hex"]:
                        hit = row
                        owner = cls
                        break
            if hit:
                break
        if hit:
            resolved[macro] = {
                "value": int(hit["offset_hex"], 16),
                "class": owner,
                "field": want,
                "declaration": hit["declaration"],
            }
        else:
            missing.append((macro, cls, fld))

    if missing:
        raise SystemExit("Missing fields: " + ", ".join(f"{c}.{f}" for _, c, f in missing))

    # Candidate static globals.
    methods_by_class = collections.defaultdict(list)
    for m in methods:
        methods_by_class[m["type_declaration"]].append(m)

    statics = {}
    for cls in STATIC_CLASSES:
        rows = [k for k in methods_by_class if re.search(r"\bclass\s+" + re.escape(cls) + r"\b", k)]
        votes = collections.Counter()
        samples = 0
        for k in rows:
            for m in methods_by_class[k][:MAX_METHODS_PER_CLASS]:
                if not m["rva"] or m["rva"] == "-1":
                    continue
                rva = int(m["rva"], 16)
                chunk = pe.bytes_at_rva(rva, SCAN_BYTES)
                if not chunk:
                    continue
                samples += 1
                for instr_rva, target in rip_targets(chunk, rva):
                    sec = pe.section_of_rva(target)
                    if sec and not sec["exec"] and sec["write"]:
                        votes[target] += 1
        best = votes.most_common(1)
        statics[cls] = {
            "rva": best[0][0] if best else 0,
            "votes": best[0][1] if best else 0,
            "methods_scanned": samples,
            "candidates": [{"rva": r, "votes": v} for r, v in votes.most_common(8)],
        }

    # Write the C header.
    lines = [
        "/* robrun_offsets.h - GENERATED by tools/gen_offsets.py. Do not edit.",
        " *",
        f" * Source: {dump.as_posix()}",
        f" *   IL2CPP metadata {manifest['metadata_version']}, x86_64.",
        f" *   GameAssembly SHA-256: {manifest['input_sha256']['GameAssembly.dll']}",
        " *",
        " * Field offsets come from the build's own field-offset registration",
        " * table (fields.csv). Regenerate after every game patch.",
        " */",
        "#ifndef ROBRUN_OFFSETS_H",
        "#define ROBRUN_OFFSETS_H",
        "",
    ]
    width = max(len(m) for m, *_ in FIELD_SPEC) + 2
    for macro, _, _ in FIELD_SPEC:
        r = resolved.get(macro)
        if r:
            lines.append(f"#define {macro.ljust(width)}0x{r['value']:X}u   /* {r['class'].split(' ')[-1]}.{r['field']} */")
        else:
            lines.append(f"#define {macro.ljust(width)}0x0u   /* NOT FOUND in dump - patch the spec */")
    lines.append("")
    lines.append("/* Static-field globals (module RVA of the .data pointer). 0 = the scan")
    lines.append(" * found no confident candidate; probe it live instead. */")
    for cls in STATIC_CLASSES:
        s = statics[cls]
        macro = "RR_SF_" + re.sub(r"[^A-Z0-9]", "_", cls.upper())
        lines.append(f"#define {macro.ljust(width)}0x{s['rva']:X}u   /* {s['votes']}/{s['methods_scanned']} method refs */")
    lines += ["", "#endif /* ROBRUN_OFFSETS_H */", ""]
    out_h.write_text("\n".join(lines), encoding="utf-8")

    # Write field provenance and candidate evidence as JSON.
    payload = {
        "game": "ROB RUN",
        "unity": manifest.get("unity_version"),
        "input_sha256": manifest["input_sha256"],
        "metadata_version": 39,
        "pe_preferred_base": hex(pe.base),
        "dump": str(dump),
        "fields": resolved,
        "missing": [{"macro": m, "class": c, "field": f} for m, c, f in missing],
        "static_field_globals": statics,
    }
    out_j.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(f"fields resolved : {len(resolved)}/{len(FIELD_SPEC)}")
    for m, c, f in missing:
        print(f"  MISSING {m}: {c}.{f}")
    print("static globals  :")
    for cls, s in statics.items():
        print(f"  {cls:22s} rva=0x{s['rva']:X} votes={s['votes']} scanned={s['methods_scanned']}")
    print(f"wrote {out_h}")
    print(f"wrote {out_j}")
    return 0


if __name__ == "__main__":
    sys.exit(main())




