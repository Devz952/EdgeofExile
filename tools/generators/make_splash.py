# Edge of Exile melee splash: properties splash25 / splash50 / splash75 / splash100.
# Same mechanism as D2R Reimagined's splash (works on D2R 3.x): a "chance to cast on striking" stat that only fires on
# melee hits (itemevent domeleedamage, itemeventfunc 20) casting a hidden item skill (srvdofunc 8) that drops an
# exploding missile on the target (pSrvHitFunc 44, radius sHitPar1) dealing SrcDamage/128 of the weapon hit.
# The property fixes the skill in its param (func 21, val = skill << 6 | level 1); the item's min/max is the chance.
import csv, json, os, sys
MOD, REIM, FD, OUT = sys.argv[1:5]
TIERS = [25, 50, 75, 100]
FIRST_SKILL = 430
STR0 = 28116

def rd(p): return list(csv.reader(open(p, encoding="latin1", newline=""), delimiter="\t", quoting=csv.QUOTE_NONE))
def wr(p, rows):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="latin1", newline="") as f:
        for r in rows: f.write("\t".join(r) + "\r\n")
def asdict(h, r): return {k: v for k, v in zip(h, r) if v}
def build(h, fields):
    row = [""] * len(h)
    for k, v in fields.items():
        if k in h: row[h.index(k)] = v
    if "*eol" in h: row[h.index("*eol")] = "0"
    return row

rs = rd(f"{REIM}/skills.txt"); rsk = asdict(rs[0], next(r for r in rs if r[0] == "proc_SplashDamage"))
rm = rd(f"{REIM}/missiles.txt"); rmh = rm[0]
rmis = asdict(rmh, next(r for r in rm if r[0] == "proc_splashdamage_50"))
rexp = asdict(rmh, next(r for r in rm if r[0] == "proc_splash_explode"))

for bank in ("", "base/"):
    # missiles
    m = rd(f"{MOD}/global/excel/{bank}missiles.txt"); h = m[0]
    assert not any(r[0].startswith("eoe_splash") for r in m)
    nid = len(m) - 1
    e = dict(rexp); e["Missile"] = "eoe_splash_explode"; e["*ID"] = str(nid); nid += 1
    m.append(build(h, e))
    for t in TIERS:
        d = dict(rmis); d["Missile"] = f"eoe_splash_{t}"; d["*ID"] = str(nid); nid += 1
        d["SrcDamage"] = str(t * 128 // 100); d["CltHitSubMissile1"] = "eoe_splash_explode"
        m.append(build(h, d))
    wr(f"{OUT}/global/excel/{bank}missiles.txt", m)
    # skills
    s = rd(f"{MOD}/global/excel/{bank}skills.txt"); h = s[0]
    assert len(s) - 1 == FIRST_SKILL, len(s)
    for i, t in enumerate(TIERS):
        d = dict(rsk); d["skill"] = f"EoE Splash {t}"; d["*Id"] = str(FIRST_SKILL + i)
        d["skilldesc"] = "eoe splash"; d["srvmissilea"] = f"eoe_splash_{t}"; d["cltmissilea"] = f"eoe_splash_{t}"
        s.append(build(h, d))
    wr(f"{OUT}/global/excel/{bank}skills.txt", s)
    # skilldesc
    sd = rd(f"{MOD}/global/excel/{bank}skilldesc.txt"); h = sd[0]
    sd.append(build(h, {"skilldesc": "eoe splash", "SkillPage": "0", "SkillRow": "0", "SkillColumn": "0",
                        "ListRow": "0", "IconCel": "0", "str name": "EoeSplashName", "str short": "EoeSplashName",
                        "str long": "EoeSplashName", "str alt": "EoeSplashName"}))
    wr(f"{OUT}/global/excel/{bank}skilldesc.txt", sd)
    # itemstatcost (current mod copy from the elemental work)
    ist = rd(f"{OUT}/global/excel/{bank}itemstatcost.txt"); h = ist[0]; c = {n: i for i, n in enumerate(h)}
    src = next(r for r in ist if r[0] == "item_skillonhit")
    for t in TIERS:
        r = list(src); r[0] = f"item_eoe_splash{t}"; r[c["*ID"]] = str(len(ist) - 1)
        r[c["itemevent2"]] = ""; r[c["itemeventfunc2"]] = ""   # melee hits only
        r[c["descstrpos"]] = r[c["descstrneg"]] = f"EoeModSplash{t}"
        r[c["advdisplay"]] = ""
        ist.append(r)
    wr(f"{OUT}/global/excel/{bank}itemstatcost.txt", ist)
    print(bank or "rotw", "stats", [x[:2] for x in ist[-4:]])
    # properties
    p = rd(f"{OUT}/global/excel/{bank}properties.txt"); h = p[0]; c = {n: i for i, n in enumerate(h)}
    tmpl = next(r for r in p if r[0] == "fireskill")
    nextid = max(int(x[c["*Id"]]) for x in p[1:] if x[c["*Id"]].isdigit()) + 1
    for i, t in enumerate(TIERS):
        r = [""] * len(h)
        for k in ("*Enabled", "uiRangeType", "*eol"):
            if k in c: r[c[k]] = tmpl[c[k]]
        r[c["code"]] = f"splash{t}"; r[c["*Id"]] = str(nextid); nextid += 1
        r[c["func1"]] = "21"; r[c["stat1"]] = f"item_eoe_splash{t}"; r[c["val1"]] = str(((FIRST_SKILL + i) << 6) | 1)
        if "*Tooltip" in c: r[c["*Tooltip"]] = f"#% Chance to Splash {t}% of Melee Damage"
        if "*Min" in c: r[c["*Min"]] = "Chance %"
        if "*Max" in c: r[c["*Max"]] = "Chance %"
        p.append(r)
    wr(f"{OUT}/global/excel/{bank}properties.txt", p)
    print(bank or "rotw", "props", [x[:7] for x in p[-4:]])

# strings
sid = STR0
for folder in ("strings", "strings-legacy"):
    sid = STR0
    p = f"{OUT}/local/lng/{folder}/item-modifiers.json"
    d = json.load(open(p, encoding="utf-8-sig")); langs = [k for k in d[0] if k not in ("id", "Key")]
    for t in TIERS:
        e = {"id": sid, "Key": f"EoeModSplash{t}"}; e.update({l: f"%d%% Chance to Splash {t}%% of Melee Damage" for l in langs})
        d.append(e); sid += 1
    json.dump(d, open(p, "w", encoding="utf-8-sig", newline="\n"), ensure_ascii=False, indent=2)
    p = f"{MOD}/local/lng/{folder}/skills.json"
    d = json.load(open(p, encoding="utf-8-sig")); langs = [k for k in d[0] if k not in ("id", "Key")]
    e = {"id": sid, "Key": "EoeSplashName"}; e.update({l: "Melee Splash" for l in langs}); d.append(e); sid += 1
    os.makedirs(f"{OUT}/local/lng/{folder}", exist_ok=True)
    json.dump(d, open(f"{OUT}/local/lng/{folder}/skills.json", "w", encoding="utf-8-sig", newline="\n"), ensure_ascii=False, indent=2)
print("next string id", sid)
t = open(f"{OUT}/next_string_id.txt", encoding="utf-8", newline="").read()
open(f"{OUT}/next_string_id.txt", "w", encoding="utf-8", newline="").write(t.replace("28116", str(sid), 1))

# HD missile models
p = f"{MOD}/hd/missiles/missiles.json"; raw = open(p, encoding="utf-8-sig").read()
j = json.loads(raw)
j["eoe_splash_explode"] = "warcry_shockwave"
for t in TIERS: j[f"eoe_splash_{t}"] = "null"
os.makedirs(f"{OUT}/hd/missiles", exist_ok=True)
json.dump(j, open(f"{OUT}/hd/missiles/missiles.json", "w", encoding="utf-8", newline="\n"), indent=4)
