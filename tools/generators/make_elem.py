# Adds +X to Lightning / Cold / Poison / Magic Skills (and keeps Fire) for Edge of Exile.
# Effect: D2's item_elemskill stat with the element as parameter (elemtypes.txt: fire 1, ltng 2, mag 3, cold 4,
# pois 5); the game adds it to skills whose skills.txt EType matches. Text: item_elemskill's own description is
# turned off (it always says "Fire") and each property also sets a display-only stat with the right wording, using
# the same rolled value (func 3).
import csv, io, json, os, sys
FD = sys.argv[1]; OUT = sys.argv[2]
ELEMS = [  # property, elem param, display stat, string key, enUS
    ("fireskill", 1, "item_eoe_fireskill", "ModStr3i", None),
    ("ltngskill", 2, "item_eoe_ltngskill", "EoeModLtngSkill", "%+d to Lightning Skills"),
    ("magskill", 3, "item_eoe_magskill", "EoeModMagSkill", "%+d to Magic Skills"),
    ("coldskill", 4, "item_eoe_coldskill", "EoeModColdSkill", "%+d to Cold Skills"),
    ("poisskill", 5, "item_eoe_poisskill", "EoeModPoisSkill", "%+d to Poison Skills"),
]

def read(p):
    with open(p, encoding="latin1", newline="") as f:
        return list(csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE))

def write(p, rows):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="latin1", newline="") as f:
        for r in rows:
            f.write("\t".join(r) + "\r\n")

for bank in ("", "base/"):
    # itemstatcost
    rows = read(f"{FD}/global/excel/{bank}itemstatcost.txt")
    h = rows[0]; col = {n: i for i, n in enumerate(h)}
    src = next(r for r in rows if r[0] == "item_elemskill")
    assert len(rows) - 1 == 368, len(rows)
    for name, _, stat, key, _ in ELEMS:
        r = list(src)
        r[col["Stat"]] = stat
        r[col["*ID"]] = str(len(rows) - 1)
        for c in ("Send Param Bits", "Save Param Bits", "fCallback", "Add", "Multiply"):
            r[col[c]] = ""
        r[col["descstrpos"]] = key
        r[col["descstrneg"]] = key
        rows.append(r)
    src[col["descfunc"]] = ""   # item_elemskill: effect only, no "Fire Skills" line for every element
    src[col["descpriority"]] = ""
    src[col["descstrpos"]] = ""
    src[col["descstrneg"]] = ""
    write(f"{OUT}/global/excel/{bank}itemstatcost.txt", rows)
    print(bank or "rotw", "itemstatcost rows", len(rows) - 1, [r[0] for r in rows[-5:]])

    # properties
    rows = read(f"{FD}/global/excel/{bank}properties.txt")
    h = rows[0]; col = {n: i for i, n in enumerate(h)}
    def setprop(r, elem, stat):
        r[col["func1"]] = "21"; r[col["stat1"]] = "item_elemskill"; r[col["set1"]] = ""; r[col["val1"]] = str(elem)
        r[col["func2"]] = "3"; r[col["stat2"]] = stat; r[col["set2"]] = ""; r[col["val2"]] = ""
    fire = next(r for r in rows if r[0] == "fireskill")
    nextid = max(int(r[col["*Id"]]) for r in rows[1:] if r[col["*Id"]].isdigit()) + 1
    for name, elem, stat, key, text in ELEMS:
        if name == "fireskill":
            setprop(fire, elem, stat)
            continue
        r = list(fire)
        r[col["code"]] = name
        r[col["*Id"]] = str(nextid); nextid += 1
        setprop(r, elem, stat)
        if "*Tooltip" in col:
            r[col["*Tooltip"]] = text.replace("%+d", "+#")
        rows.append(r)
    write(f"{OUT}/global/excel/{bank}properties.txt", rows)
    print(bank or "rotw", "properties", [r[0] for r in rows[-4:]], "fireskill:", fire[:10])

# strings (both folders)
first_id = 28110
for folder in ("strings", "strings-legacy"):
    with open(f"{FD}/local/lng/{folder}/item-modifiers.json", encoding="utf-8-sig") as f:
        data = json.load(f)
    langs = [k for k in data[0].keys() if k not in ("id", "Key")]
    sid = first_id
    for _, _, _, key, text in ELEMS:
        if text is None:
            continue
        assert not any(e["Key"] == key for e in data)
        e = {"id": sid, "Key": key}
        e.update({l: text for l in langs})
        data.append(e); sid += 1
    os.makedirs(f"{OUT}/local/lng/{folder}", exist_ok=True)
    with open(f"{OUT}/local/lng/{folder}/item-modifiers.json", "w", encoding="utf-8-sig", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(folder, "strings up to", sid - 1)
