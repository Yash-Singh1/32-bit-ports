#!/usr/bin/env python3
"""Check generated pointer ABI rules with a tiny original SDK-shaped fixture."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

native = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="lp32-steam-abi-") as directory:
    root = Path(directory)
    params = [
        ("StringOutput", {"paramtype": "char **", "out_string": ""}, "S"),
        ("PointerArray", {"paramtype": "char **"}, "?"),
        ("ConstPointerArray", {"paramtype": "const char **"}, "?"),
        ("PointerReference", {"paramtype": "int *&"}, "?"),
        ("AliasArray", {"paramtype": "StringPointers"}, "?"),
        ("PointerStruct", {"paramtype": "PointerData *"}, "?"),
        ("PlainBuffer", {"paramtype": "char *"}, "p"),
        ("ScalarOutput", {"paramtype": "uint32 *"}, "p"),
        ("Tags", {"paramtype": "SteamParamStringArray_t *"}, "T"),
        ("UnknownEnum", {"paramtype": "ENotAnEnum"}, "?"),
        ("ScopedEnum", {"paramtype": "ISteamClient::EButton"}, "i"),
    ]
    api = {"typedefs": [{"typedef": "StringPointers", "type": "char **"},
                        {"typedef": "uint32", "type": "unsigned int"}],
           "enums": [], "structs": [{"struct": "PointerData",
               "fields": [{"fieldtype": "char *"}]}], "callback_structs": [],
           "interfaces": [{"classname": "ISteamClient",
               "enums": [{"enumname": "EButton", "fqname": "ISteamClient::EButton"}], "methods": [
               {"methodname": name, "returntype": "bool", "params": [param]}
               for name, param, _ in params]}]}
    (root / "api.json").write_text(json.dumps(api))
    (root / "isteamclient.h").write_text("class ISteamClient {\n" + "".join(
        "virtual bool " + name + "();\n" for name, _, _ in params) + "};\n")
    for name in ("isteamclient017.h", "isteaminput005.h"):
        (root / name).write_text("")
    subprocess.run([sys.executable, str(native / "tools/generate_steam_abi.py"),
                    str(root / "api.json"), str(root), str(root / "abi.inc")], check=True)
    generated = (root / "abi.inc").read_text()
    for name, _, expected in params:
        assert f'{{"{name}", \'b\', "{expected}"}}' in generated, (name, generated)
    timeline = [("SetTimelineStateDescription", "pf"), ("ClearTimelineStateDescription", "f"),
                ("AddTimelineEvent", "pppiffi"), ("SetTimelineGameMode", "i")]
    for name, arguments in timeline:
        assert f'{{"{name}", \'v\', "{arguments}"}}' in generated, (name, generated)
    assert '"STEAMTIMELINE_INTERFACE_V001"' in generated

    records = [("GetDigitalActionData", "InputDigitalActionData_t", "D"),
               ("GetAnalogActionData", "InputAnalogActionData_t", "A"),
               ("GetMotionData", "InputMotionData_t", "M"),
               ("GetPublicIP", "SteamIPAddress_t", "I"),
               ("UnversionedObject", "ISteamThing *", "?")]
    api["interfaces"].append({"classname": "ISteamInput", "version_string": "SteamInput006",
        "methods": [{"methodname": name, "returntype": ret, "params": []} for name, ret, _ in records]})
    (root / "isteaminput.h").write_text('#define STEAMINPUT_INTERFACE_VERSION "SteamInput006"\nclass ISteamInput {\n' +
        "".join("virtual " + ret + " " + name + "();\n" for name, ret, _ in records) + "};\n")
    (root / "isteaminput005.h").write_text("virtual InputAnalogActionData_t GetAnalogActionData();\n")
    (root / "api.json").write_text(json.dumps(api))
    command = [sys.executable, str(native / "tools/generate_steam_abi.py"),
               str(root / "api.json"), str(root), str(root / "abi.inc")]
    subprocess.run(command, check=True)
    generated = (root / "abi.inc").read_text()
    for name, _, expected in records:
        assert f'{{"{name}", \'{expected}\', ""}}' in generated, (name, generated)
    assert generated.count('{"GetAnalogActionData", \'A\', ""}') == 2, generated
    api["interfaces"][-1]["version_string"] = "SteamInput999"
    (root / "api.json").write_text(json.dumps(api))
    mismatch = subprocess.run(command, capture_output=True, text=True)
    assert mismatch.returncode != 0 and "SDK version mismatch" in mismatch.stderr, mismatch
    assert (root / "abi.inc").read_text() == generated
print("Steam ABI generation: PASS (pointer depth, string outputs, enums, packed returns, legacy ordinals, Timeline001, SDK version validation)")
