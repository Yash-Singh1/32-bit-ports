#!/usr/bin/env python3
"""Generate compact ABI facts from Valve's Steamworks 1.53a JSON and matching headers.

Usage: generate_steam_abi.py JSON HEADER_DIRECTORY OUTPUT_INC
Also supply SDK 1.32's client header as isteamclient017.h and SDK 1.52's input
header as isteaminput005.h in HEADER_DIRECTORY.

Inputs are development-only; the loader/converter do not download SDK files.
Reference: Steamworks SDK 1.53a, 37e939ea0d938b43a0466d0aa6ed4619c300a7c7
at https://github.com/rlabrecque/SteamworksSDK (Valve's SDK mirror).
"""
import json, re, sys
from pathlib import Path

api = json.loads(Path(sys.argv[1]).read_text())
typedefs = {x["typedef"]: x["type"] for x in api["typedefs"]}
enums = {x["enumname"] for x in api["enums"]}
for interface in api["interfaces"]:
    for enum in interface.get("enums", []):
        enums.add(enum.get("fqname", interface["classname"] + "::" + enum["enumname"]))
structs = {x["struct"]: x["fields"] for x in api["structs"]}
structs.update({x["struct"]: x["fields"] for x in api["callback_structs"]})


def scalar(t):
    seen = set()
    while t in typedefs and t not in seen:
        seen.add(t)
        t = typedefs[t]
    return t


def safe_struct(t, seen=()):
    t = scalar(re.sub(r"\s*\[.*", "", t.replace("const ", "").strip()))
    if t in ("CSteamID", "CGameID"):
        return True
    if "*" in t or "&" in t:
        return False
    t = re.sub(r"\s*\[.*", "", t)
    if t not in structs:
        return (
            t
            in (
                "void",
                "bool",
                "char",
                "signed char",
                "unsigned char",
                "short",
                "unsigned short",
                "int",
                "unsigned int",
                "float",
                "double",
                "long long",
                "unsigned long long",
            )
            or t in enums
        )
    if t in seen:
        return False
    return all(safe_struct(f["fieldtype"], seen + (t,)) for f in structs[t])


def code(t, result=False, output_string=False):
    t = t.replace("const ", "").strip()
    if t in ("CSteamID", "CGameID"):
        return "q"
    if t == "SteamAPIWarningMessageHook_t":
        return "W"
    if t == "SteamAPI_CheckCallbackRegistered_t":
        return "K"
    if t == "SteamInputActionEventCallbackPointer":
        return "E"
    if t == "HServerListRequest":
        return "h"
    if t == "ISteamMatchmakingServerListResponse *" and not result:
        return "R"
    if t == "MatchMakingKeyValuePair_t **" and not result:
        return "F"
    if result and (t.startswith("ISteam") and t.endswith("*") or t == "void *"):
        return "o"
    if result and t == "char *":
        return "s"
    t = scalar(t)
    aggregates = {"InputDigitalActionData_t": "D", "ControllerDigitalActionData_t": "D",
                  "InputAnalogActionData_t": "A", "ControllerAnalogActionData_t": "A",
                  "InputMotionData_t": "M", "ControllerMotionData_t": "M",
                  "SteamIPAddress_t": "I", "SteamPartyBeaconLocation_t": "L"}
    if t in aggregates:
        return aggregates[t]
    if "*" in t or "&" in t:
        # Remove exactly one indirection. rstrip(" *&") silently erased all
        # pointer depth, allowing native writes through four-byte guest cells.
        base = re.sub(r"[ *&]+$", "", t)
        depth = t.count("*") + t.count("&")
        if depth == 2 and base == "char" and output_string and not result:
            return "S"
        if depth != 1:
            return "?"
        if base == "SteamParamStringArray_t" and not result:
            return "T"
        if base.startswith("ISteam") or not safe_struct(base):
            return "?"
        return "p" if not result else "?"
    if t == "void":
        return "v"
    if t == "bool":
        return "b"
    if t in ("uint64", "unsigned long long", "int64", "long long"):
        return "q"
    if t == "float":
        return "f"
    if t == "double":
        return "d"
    if t in ("unsigned char", "char", "signed char"):
        return "b"
    if t in ("uint16", "unsigned short", "short", "int16"):
        return "u"
    if (
        t in ("int", "unsigned int", "uint32", "int32", "long", "unsigned long")
        or t in enums
    ):
        return "i"
    return "?"


methods = []
interfaces = []
for obj in api["interfaces"]:
    version = obj.get("version_string")
    entries = obj["methods"]
    if obj["classname"] == "ISteamClient":
        version = "SteamClient020"
    if not version:
        continue
    header_name = obj["classname"].lower() + ".h"
    if obj["classname"] in (
        "ISteamMatchmakingServers",
        "ISteamGameSearch",
        "ISteamParties",
    ):
        header_name = "isteammatchmaking.h"
    header = (Path(sys.argv[2]) / header_name).read_text(encoding="latin1")
    declared_versions = re.findall(r'#define\s+\w*INTERFACE_VERSION\w*\s+"([^"]+)"', header)
    if declared_versions and version not in declared_versions:
        raise ValueError(f"SDK version mismatch: JSON {version}, {header_name} {declared_versions}")
    header = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.S)
    body = re.search(
        r"class\s+" + obj["classname"] + r"\s*\{(.*?)^\};", header, re.M | re.S
    )
    if not body:
        raise ValueError("class body " + obj["classname"])
    byname = {}
    for m in entries:
        byname.setdefault(m["methodname"], []).append(m)
    entries = []
    # JSON omits private virtual slots. Recover every ordinal from the header,
    # including overloaded methods, and trap if a private ABI is unknown.
    for name in re.findall(r"virtual\s+[\w\s*&]+?\b(\w+)\s*\(", body[1]):
        if byname.get(name):
            entries.append(byname[name].pop(0))
            continue
        params = []
        ret = "unsupported_private"
        if name in ("RunFrame", "DestroyAllInterfaces", "ForceMasterServerHeartbeat_DEPRECATED"):
            ret = "void"
        # SDK 1.53a's GameServer014 header includes these two private vtable
        # slots, but steam_api.json omits them. They are real native calls.
        if name == "SetMasterServerHeartbeatInterval_DEPRECATED":
            ret = "void"
            params = [{"paramtype": "int"}]
        if name == "GetCSERIPPort":
            ret = "bool"
            params = [{"paramtype": "uint32 *"}, {"paramtype": "uint16 *"}]
        if name == "InitGameServer":
            ret = "bool"
            params = [
                {"paramtype": t}
                for t in [
                    "uint32",
                    "uint16",
                    "uint16",
                    "uint32",
                    "AppId_t",
                    "const char *",
                ]
            ]
        if name in (
            "DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess",
            "DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess",
        ):
            ret = "void"
            params = [{"paramtype": "unsupported_callback"}]
        if name == "Set_SteamAPI_CCheckCallbackRegisteredInProcess":
            ret = "void"
            params = [{"paramtype": "SteamAPI_CheckCallbackRegistered_t"}]
        entries.append({"methodname": name, "returntype": ret, "params": params})
    # Inline convenience functions in JSON do not occupy vtable slots.
    if not version:
        continue
    start = len(methods)
    for m in entries:
        ret = code(m["returntype"], True)
        # Interface factories take a version string. Other object returns need
        # their own proxy/lifetime contract; an integer argument is not a name.
        if ret == "o" and not (m["methodname"].startswith("GetISteam") and
                               m.get("params") and m["params"][-1]["paramtype"] == "const char *"):
            ret = "?"
        args = "".join(code(p["paramtype"], output_string="out_string" in p)
                       for p in m.get("params", []))
        methods.append((m["methodname"], ret, args))
    interfaces.append((version, start, len(entries)))
# libsteam_api also asks for its older client facade during InitSafe. The
# v1.32 header (3aa76d12fa78) describes SteamClient017's distinct ordinals.
legacy_header = (Path(sys.argv[2]) / "isteamclient017.h").read_text(encoding="latin1")
legacy_names = re.findall(r"virtual\s+[\w\s*&]+?\b(\w+)\s*\(", legacy_header)
client = next(i for i in interfaces if i[0] == "SteamClient020")
client_methods = {m[0]: m for m in methods[client[1]:client[1] + client[2]]}
legacy_start = len(methods)
for name in legacy_names:
    entry = client_methods.get(name, (name, "?", ""))
    if name == "SetLocalIPBinding":
        entry = (name, "v", "iu")
    if name == "GetISteamUnifiedMessages":
        entry = (name, "o", "iip")
    methods.append(entry)
interfaces.append(("SteamClient017", legacy_start, len(legacy_names)))

# Steam Input 005 differs from 006's vtable; derive its old ordinals too.
input_header = (Path(sys.argv[2]) / "isteaminput005.h").read_text(encoding="latin1")
input_header = re.sub(r"/\*.*?\*/|//[^\n]*", "", input_header, flags=re.S)
input_names = re.findall(r"virtual\s+[\w\s*&]+?\b(\w+)\s*\(", input_header)
input_start = len(methods)
input_interface = next((i for i in interfaces if i[0] == "SteamInput006"), None)
input_methods = ({m[0]: m for m in methods[input_interface[1]:input_interface[1] + input_interface[2]]}
                 if input_interface else {})
for name in input_names:
    methods.append(input_methods.get(name, (name, "?", "")))
interfaces.append(("SteamInput005", input_start, len(input_names)))

# This published interface is declared only in isteamgamecoordinator.h.
interfaces.append(("SteamGameCoordinator001", len(methods), 3))
methods += [
    ("SendMessage", "i", "ipi"),
    ("IsMessageAvailable", "b", "p"),
    ("RetrieveMessage", "i", "ppip"),
]
# L4D2's client uses this later public interface, absent from SDK 1.53a.
# Valve's exact V001 declaration is src/public/steam/isteamtimeline.h in
# source-sdk-2013 commit 0759e2e8e179d5352d81d0d4aaded72c1704b7a9.
# Append it so the existing generated method ordinals remain stable.
interfaces.append(("STEAMTIMELINE_INTERFACE_V001", len(methods), 4))
methods += [
    ("SetTimelineStateDescription", "v", "pf"),
    ("ClearTimelineStateDescription", "v", "f"),
    ("AddTimelineEvent", "v", "pppiffi"),
    ("SetTimelineGameMode", "v", "i"),
]
out = [
    "/* Generated ABI descriptions; see tools/generate_steam_abi.py. */",
    "static const struct steam_method steam_methods[] = {",
]
for name, ret, args in methods:
    out.append(f'    {{"{name}", \'{ret}\', "{args}"}},')
out += ["};", "static const struct steam_interface steam_interfaces[] = {"]
for version, start, count in interfaces:
    out.append(f'    {{"{version}", {start}, {count}}},')
out += ["};", "static const int steam_plain_callbacks[] = {"]
for callback in api["callback_structs"]:
    if all(safe_struct(field["fieldtype"]) for field in callback["fields"]):
        out.append(f"    {callback['callback_id']}, /* {callback['struct']} */")
out += [
    "    1701, /* GCMessageAvailable_t */",
    "    1702, /* GCMessageFailed_t */",
    "};",
    "",
]
Path(sys.argv[3]).write_text("\n".join(out))
print(f"{len(interfaces)} interfaces, {len(methods)} methods")
