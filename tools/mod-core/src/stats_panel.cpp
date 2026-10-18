// Edge of Exile advanced stat page (D2R 3.3.0 build 93847 under D2RLoader 1.3.1).
//
// Fills D2R's own Advanced Stats panel (the arrow button on the character sheet). The mod ships
// data/global/ui/layouts/advancedstatspanelhd.json = the stock panel plus EoE* text rows (gen_stats_layout.py); while the
// panel is visible the plugin hides the stock list and refreshes the rows four times a second on the UI thread from
// the local player's unit:
//   breakpoints      FCR / FHR / FBR: current frames and the next breakpoint, per class (+ the class's special
//                    table: Lightning, spear/staff, 1h swinging, Holy Shield, Vampire form), IAS and FRW
//   resistances      after the difficulty penalty (normal 0, nightmare -40, hell -100), the cap (75 + max, at most
//                    95) and how far over/under the cap the gear is; poison length and curse reduction
//   defense          damage reduction % and flat, magic damage reduction, absorb, freeze protection
//   item find        magic find with its effective unique/set/rare values, gold find, experience
//   offense          crushing blow, deadly strike, open wounds, leech, -enemy resistance, +elemental damage, skills
//   sustain          replenish life, mana regeneration, life/mana per kill, attacker takes damage, damage to mana
//
// Game functions (entry bytes checked before use; the same functions ESR's monster display plugin calls):
//   0x8b2d0   () -> local player index (mov eax, [global]; ret)
//   0x9a480   (index) -> the client's player unit for that index (index < 8)
//   0x405790  () -> the local server game (single player / TCP host), called on the game thread only;
//             game +0x104 = difficulty
//   0x2f5020  (unit, stat, layer) -> total stat value (d2r::api.unitStat)
//   0x856220  (widget, const char* name) -> child widget (hooked to learn the native Advanced Stats panel pointer:
//             the first parent that has an "EoESummary" child)
//   0x86dd50  (text box, &{const char*, size_t}) -> set the text box's text
// D2RLoader 1.3.1-beta's WidgetService has no setText (64-byte table), so text goes through D2R's own setter, as ESR's
// monster display does on the same build. The loader's handles are still used to hide the stock list.
#include "stats_panel.h"

#include "d2r.h"
#include "stats_panel_layout.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using d2r::At;
using d2r::Game;
using d2r::Unit;

constexpr const char* PanelName = "AdvancedStatsPanel";
const char* const StockWidgets[] = {"ListContainer", "ScrollBar", "ScrollBackground"};
constexpr int StockWidgetCount = 3;
constexpr ULONGLONG RefreshMs = 250;

constexpr uint64_t RvaLocalPlayerIndex = 0x8b2d0;
constexpr uint8_t LocalPlayerIndexEntry[] = {0x8b, 0x05, 0x2e, 0x84, 0x99, 0x02, 0xc3};
constexpr uint64_t RvaClientPlayer = 0x9a480;
constexpr uint8_t ClientPlayerEntry[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x83, 0xf9, 0x08};
constexpr uint64_t RvaLocalServerGame = 0x405790;
constexpr uint8_t LocalServerGameEntry[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x15, 0x85, 0x68, 0x69,
                                            0x02, 0x33, 0xc9, 0x48, 0x8d, 0x42, 0x01, 0x48, 0x83, 0xf8,
                                            0x01, 0x48, 0x0f, 0x47, 0xca, 0xe8, 0x42, 0x0b, 0x00, 0x00};
constexpr int GameDifficulty = 0x104;
constexpr uint64_t RvaChildByName = 0x856220;
// mov [rsp+10],rbx / mov [rsp+18],rsi / push rdi / sub rsp,20 (15 bytes, whole instructions) + mov rbx,[rcx+58]
constexpr uint8_t ChildByNameEntry[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                        0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x59, 0x58};
constexpr uint32_t ChildByNameHookBytes = 15;
constexpr uint64_t RvaSetText = 0x86dd50;

struct TextView {
	const char* data;
	size_t size;
};
using ChildByNameFn = void* (*)(void* parent, const char* name) noexcept;
using SetTextFn = void (*)(void* textBox, const TextView* text) noexcept;
ChildByNameFn g_childByName = nullptr;  // trampoline to D2R's original
SetTextFn g_setText = nullptr;
std::atomic<void*> g_nativePanel {nullptr};
std::atomic<void*> g_uiRoot {nullptr};  // D2R's top-level UI widget: parent of every panel (seen looking up panels by name)
std::atomic<long> g_rootCaptures {0};
std::atomic<long> g_captures {0};
std::atomic<long> g_hookCalls {0};
// Diagnostics: distinct child names the hook saw (first 96), written only by the hook under a tiny spin lock.
constexpr int SeenMax = 96;
char g_seen[SeenMax][24] = {};
int g_seenCount = 0;
volatile long g_seenLock = 0;
void RememberName(const char* name) noexcept {
	if (g_seenCount >= SeenMax || _InterlockedCompareExchange(&g_seenLock, 1, 0) != 0) {
		return;
	}
	bool known = false;
	for (int i = 0; i < g_seenCount && !known; ++i) {
		known = std::strncmp(g_seen[i], name, sizeof(g_seen[i]) - 1) == 0;
	}
	if (!known && g_seenCount < SeenMax) {
		std::snprintf(g_seen[g_seenCount++], sizeof(g_seen[0]), "%s", name);
	}
	_InterlockedExchange(&g_seenLock, 0);
}

// Hook on D2R's child lookup. Every panel looks its children up by name when it is built (and D2RLoader's own widget
// lookups may go through it too); the panel that owns "EoESummary" is our Advanced Stats panel.
auto ChildByNameHook(void* parent, const char* name) noexcept -> void* {
	void* child = g_childByName(parent, name);
	g_hookCalls.fetch_add(1, std::memory_order_relaxed);
	if (name != nullptr) {
		RememberName(name);
		// Top-level panels that exist in every session are looked up from the UI root.
		if (child != nullptr && parent != nullptr &&
		    (std::strcmp(name, "TooltipsPanel") == 0 || std::strcmp(name, "PanelBordersPanel") == 0 ||
		     std::strcmp(name, "ConsolePanel") == 0) &&
		    g_uiRoot.load(std::memory_order_relaxed) != parent) {
			g_uiRoot.store(parent);
			g_rootCaptures.fetch_add(1);
		}
	}
	if (parent != nullptr && name != nullptr && parent != g_nativePanel.load(std::memory_order_relaxed)) {
		bool mine = false;
		if (std::strcmp(name, "EoESummary") == 0) {
			mine = child != nullptr;
		} else if (std::strcmp(name, "ListContainer") == 0 || std::strcmp(name, "ScrollBar") == 0 ||
		           std::strcmp(name, "ScrollBackground") == 0) {
			mine = g_childByName(parent, "EoESummary") != nullptr;
		}
		if (mine) {
			g_nativePanel.store(parent);
			g_captures.fetch_add(1);
		}
	}
	return child;
}

using LocalPlayerIndexFn = int32_t (*)() noexcept;
using ClientPlayerFn = Unit* (*)(int32_t index) noexcept;
using LocalServerGameFn = Game* (*)() noexcept;

LocalPlayerIndexFn g_playerIndex = nullptr;
ClientPlayerFn g_clientPlayer = nullptr;
LocalServerGameFn g_serverGame = nullptr;

const D2RL::PluginContext* g_ctx = nullptr;
const D2RL::WidgetService* g_widgets = nullptr;
const D2RL::ThreadService* g_threads = nullptr;
bool g_hasWidgetState = false;
bool g_loggedFirstFill = false;

std::atomic<int> g_difficulty {-1};  // -1 unknown (joined TCP/IP client, or not read yet)
ULONGLONG g_lastRefresh = 0;

// --- Stat ids (itemstatcost.txt rows) -------------------------------------------------------------------------------
enum Stat : int32_t {
	StLevel = 12,
	StNormalDr = 34, StMagicDr = 35, StDr = 36,
	StMagicRes = 37, StMaxMagicRes = 38, StFireRes = 39, StMaxFireRes = 40, StLightRes = 41, StMaxLightRes = 42,
	StColdRes = 43, StMaxColdRes = 44, StPoisonRes = 45, StMaxPoisonRes = 46,
	StLifeLeech = 60, StManaLeech = 62,
	StReplenishLife = 74, StManaRegen = 27, StThorns = 78, StGoldFind = 79, StMagicFind = 80,
	StClassSkills = 83, StExperience = 85, StLifePerKill = 86,
	StIas = 93, StFrw = 96, StFhr = 99, StFbr = 102, StFcr = 105,
	StCurseRes = 109, StPoisonLength = 110, StDamageToMana = 114, StHalfFreeze = 118, StArPercent = 119,
	StAllSkills = 127, StLightThorns = 128, StOpenWounds = 135, StCrushingBlow = 136, StManaPerKill = 138,
	StDeadlyStrike = 141,
	StAbsorbFirePct = 142, StAbsorbFire = 143, StAbsorbLightPct = 144, StAbsorbLight = 145,
	StAbsorbMagicPct = 146, StAbsorbMagic = 147, StAbsorbColdPct = 148, StAbsorbCold = 149,
	StCannotBeFrozen = 153,
	StPierceCold = 305, StPierceFire = 306, StPierceLight = 307, StPiercePoison = 308,
	StFireMastery = 329, StLightMastery = 330, StColdMastery = 331, StPoisonMastery = 332,
	StPassiveFirePierce = 333, StPassiveLightPierce = 334, StPassiveColdPierce = 335, StPassivePoisonPierce = 336,
	StMagicMastery = 357, StPassiveMagicPierce = 358,
};

// --- Breakpoint tables ------------------------------------------------------------------------------------------
struct Bp {
	int16_t pct;
	int16_t frames;
};
struct Table {
	const char* label;  // shown in front of the value ("Cast rate", "  Lightning")
	const Bp* rows;
	int count;
};
#define TABLE(label, arr) Table {label, arr, static_cast<int>(sizeof(arr) / sizeof(arr[0]))}
constexpr Table None {nullptr, nullptr, 0};

// Faster cast rate
constexpr Bp FcrAma[] = {{0, 19}, {7, 18}, {14, 17}, {22, 16}, {32, 15}, {48, 14}, {68, 13}, {99, 12}, {152, 11}};
constexpr Bp FcrSor[] = {{0, 13}, {9, 12}, {20, 11}, {37, 10}, {63, 9}, {105, 8}, {200, 7}};
constexpr Bp FcrSorLight[] = {{0, 19}, {7, 18}, {15, 17}, {23, 16}, {35, 15}, {52, 14}, {78, 13}, {117, 12}, {194, 11}};
constexpr Bp FcrNec[] = {{0, 15}, {9, 14}, {18, 13}, {30, 12}, {48, 11}, {75, 10}, {125, 9}};
constexpr Bp FcrNecVampire[] = {{0, 23}, {6, 22}, {11, 21}, {18, 20}, {24, 19}, {35, 18},
                                {48, 17}, {65, 16}, {86, 15}, {120, 14}, {180, 13}};
constexpr Bp FcrBar[] = {{0, 13}, {9, 12}, {20, 11}, {37, 10}, {63, 9}, {105, 8}, {200, 7}};
constexpr Bp FcrDru[] = {{0, 18}, {4, 17}, {10, 16}, {19, 15}, {30, 14}, {46, 13}, {68, 12}, {99, 11}, {163, 10}};
constexpr Bp FcrDruWolf[] = {{0, 16}, {6, 15}, {14, 14}, {26, 13}, {40, 12}, {60, 11}, {95, 10}, {157, 9}};
constexpr Bp FcrAss[] = {{0, 16}, {8, 15}, {16, 14}, {27, 13}, {42, 12}, {65, 11}, {102, 10}, {174, 9}};
// Faster hit recovery
constexpr Bp FhrAma[] = {{0, 11}, {6, 10}, {13, 9}, {20, 8}, {32, 7}, {52, 6}, {86, 5}, {174, 4}, {600, 3}};
constexpr Bp FhrSor[] = {{0, 15}, {5, 14}, {9, 13}, {14, 12}, {20, 11}, {30, 10},
                         {42, 9}, {60, 8}, {86, 7}, {142, 6}, {280, 5}};
constexpr Bp FhrNec[] = {{0, 13}, {5, 12}, {10, 11}, {16, 10}, {26, 9}, {39, 8}, {56, 7}, {86, 6}, {152, 5}, {377, 4}};
constexpr Bp FhrNecVampire[] = {{0, 15}, {2, 14}, {6, 13}, {10, 12}, {16, 11}, {24, 10},
                                {34, 9}, {48, 8}, {72, 7}, {117, 6}, {208, 5}};
constexpr Bp Fhr9[] = {{0, 9}, {7, 8}, {15, 7}, {27, 6}, {48, 5}, {86, 4}, {200, 3}};  // pal, bar, ass
constexpr Bp FhrPalSpear[] = {{0, 13}, {3, 12}, {7, 11}, {13, 10}, {20, 9}, {32, 8}, {48, 7}, {75, 6}, {129, 5}, {280, 4}};
constexpr Bp FhrDruSwing[] = {{0, 14}, {3, 13}, {7, 12}, {13, 11}, {19, 10}, {29, 9},
                              {42, 8}, {63, 7}, {99, 6}, {174, 5}, {456, 4}};
// Faster block rate
constexpr Bp Fbr5[] = {{0, 5}, {13, 4}, {32, 3}, {86, 2}, {600, 1}};  // ama (other weapons), pal, ass
constexpr Bp FbrAmaSwing[] = {{0, 17}, {4, 16}, {6, 15}, {11, 14}, {15, 13}, {23, 12}, {29, 11},
                              {40, 10}, {56, 9}, {80, 8}, {120, 7}, {200, 6}, {480, 5}};
constexpr Bp FbrSor[] = {{0, 9}, {7, 8}, {15, 7}, {27, 6}, {48, 5}, {86, 4}, {200, 3}};
constexpr Bp Fbr11[] = {{0, 11}, {6, 10}, {13, 9}, {20, 8}, {32, 7}, {52, 6}, {86, 5}, {174, 4}, {600, 3}};
constexpr Bp FbrPalHolyShield[] = {{0, 2}, {86, 1}};
constexpr Bp FbrBar[] = {{0, 7}, {9, 6}, {20, 5}, {42, 4}, {86, 3}, {280, 2}};

struct ClassInfo {
	const char* name;
	Table fcr, fcrAlt, fhr, fhrAlt, fbr, fbrAlt;
};

// Class ids as in playerclass.txt / charstats.txt (the "Expansion" separator row has no id).
const ClassInfo Classes[] = {
	{"Amazon", TABLE("Cast rate", FcrAma), None, TABLE("Hit recovery", FhrAma), None,
	 TABLE("Block", Fbr5), TABLE("  1h swinging", FbrAmaSwing)},
	{"Sorceress", TABLE("Cast rate", FcrSor), TABLE("  Lightning", FcrSorLight), TABLE("Hit recovery", FhrSor), None,
	 TABLE("Block", FbrSor), None},
	{"Necromancer", TABLE("Cast rate", FcrNec), TABLE("  Vampire", FcrNecVampire), TABLE("Hit recovery", FhrNec),
	 TABLE("  Vampire", FhrNecVampire), TABLE("Block", Fbr11), None},
	{"Paladin", TABLE("Cast rate", FcrNec), None, TABLE("Hit recovery", Fhr9), TABLE("  Spear/staff", FhrPalSpear),
	 TABLE("Block", Fbr5), TABLE("  Holy Shield", FbrPalHolyShield)},
	{"Barbarian", TABLE("Cast rate", FcrBar), None, TABLE("Hit recovery", Fhr9), None, TABLE("Block", FbrBar), None},
	{"Druid", TABLE("Cast rate", FcrDru), TABLE("  Werewolf", FcrDruWolf), TABLE("Hit recovery", FhrNec),
	 TABLE("  1h swinging", FhrDruSwing), TABLE("Block", Fbr11), None},
	{"Assassin", TABLE("Cast rate", FcrAss), None, TABLE("Hit recovery", Fhr9), None, TABLE("Block", Fbr5), None},
	{"Warlock", TABLE("Cast rate", FcrNec), None, TABLE("Hit recovery", FhrNec), None, TABLE("Block", Fbr11), None},
};
constexpr int ClassCount = static_cast<int>(sizeof(Classes) / sizeof(Classes[0]));

const char* const Difficulties[] = {"Normal", "Nightmare", "Hell"};
constexpr int ResistPenalty[] = {0, -40, -100};

// --- Widgets ----------------------------------------------------------------------------------------------------
enum : int {
	WSummary = 0,
	WFooter = 1,
	WHeader0 = 2,                           // 6 headers
	WLabel0 = WHeader0 + StatsSections,     // StatsRows labels
	WValue0 = WLabel0 + StatsRows,          // StatsRows values
	WidgetCount = WValue0 + StatsRows,
};

struct WidgetSlot {
	void* native;     // D2R text box
	char shown[160];  // last text set, so unchanged rows cost nothing
};
WidgetSlot g_slots[WidgetCount] = {};
void* g_boundPanel = nullptr;  // native panel the slots were resolved against
D2RL::Widgets::WidgetHandle g_panelWidget = D2RL::Widgets::InvalidHandle;
D2RL::Widgets::WidgetHandle g_stock[StockWidgetCount] = {};

void WidgetName(int slot, char* out, size_t size) noexcept {
	if (slot == WSummary) {
		std::snprintf(out, size, "EoESummary");
	} else if (slot == WFooter) {
		std::snprintf(out, size, "EoEFooter");
	} else if (slot < WLabel0) {
		std::snprintf(out, size, "EoEH%d", slot - WHeader0);
	} else if (slot < WValue0) {
		std::snprintf(out, size, "EoEL%d", slot - WLabel0);
	} else {
		std::snprintf(out, size, "EoEV%d", slot - WValue0);
	}
}

// Loader handles of the stock list widgets, to hide them (setWidgetVisible exists on 1.3.1).
void FindStockHandles() noexcept {
	if (g_widgets->findPanel(g_ctx, PanelName, &g_panelWidget) != D2RL::Widgets::Result::Success) {
		g_panelWidget = D2RL::Widgets::InvalidHandle;
		return;
	}
	for (int i = 0; i < StockWidgetCount; ++i) {
		if (g_widgets->findWidget(g_ctx, g_panelWidget, StockWidgets[i], &g_stock[i]) != D2RL::Widgets::Result::Success) {
			g_stock[i] = D2RL::Widgets::InvalidHandle;
		}
	}
	D2RL::Widgets::WidgetHandle summary = D2RL::Widgets::InvalidHandle;  // may trigger the hook's capture
	g_widgets->findWidget(g_ctx, g_panelWidget, "EoESummary", &summary);
}

// Resolve the EoE text boxes on the captured native panel. False while no panel is known.
void* g_stockNative[StockWidgetCount] = {};

auto BindWidgets() noexcept -> bool {
	void* root = g_uiRoot.load();
	if (root != nullptr) {
		// With the root known, trust only what it holds now: a closed / rebuilt panel never leaves a stale pointer.
		void* fromRoot = g_childByName(root, PanelName);
		g_nativePanel.store(fromRoot != nullptr && g_childByName(fromRoot, "EoESummary") != nullptr ? fromRoot : nullptr);
	}
	void* panel = g_nativePanel.load();
	if (panel == nullptr) {
		g_boundPanel = nullptr;
		return false;
	}
	if (panel == g_boundPanel && g_slots[WSummary].native != nullptr &&
	    g_childByName(panel, "EoESummary") == g_slots[WSummary].native) {
		return true;
	}
	char name[16];
	for (int i = 0; i < WidgetCount; ++i) {
		WidgetName(i, name, sizeof(name));
		g_slots[i].native = g_childByName(panel, name);
		g_slots[i].shown[0] = '\x01';  // force the first write
		g_slots[i].shown[1] = '\0';
	}
	if (g_slots[WSummary].native == nullptr) {
		g_nativePanel.compare_exchange_strong(panel, nullptr);
		g_boundPanel = nullptr;
		return false;
	}
	for (int i = 0; i < StockWidgetCount; ++i) {
		g_stockNative[i] = g_childByName(panel, StockWidgets[i]);
	}
	g_boundPanel = panel;
	return true;
}

void Set(int slot, const char* text) noexcept {
	WidgetSlot& w = g_slots[slot];
	if (w.native == nullptr || std::strncmp(w.shown, text, sizeof(w.shown) - 1) == 0) {
		return;
	}
	const TextView view {text, std::strlen(text)};
	g_setText(w.native, &view);
	std::snprintf(w.shown, sizeof(w.shown), "%s", text);
}

// One row: label + value.
void Row(int row, const char* label, const char* fmt, ...) noexcept {
	if (row < 0 || row >= StatsRows) {
		return;
	}
	char value[160];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(value, sizeof(value), fmt, args);
	va_end(args);
	Set(WLabel0 + row, label);
	Set(WValue0 + row, value);
}

void Blank(int row) noexcept {
	Set(WLabel0 + row, "");
	Set(WValue0 + row, "");
}

// --- Values -----------------------------------------------------------------------------------------------------
Unit* g_unit = nullptr;

auto S(int32_t stat, int32_t layer = 0) noexcept -> int32_t {
	return d2r::api.unitStat(g_unit, stat, layer);
}

void BreakpointRow(int row, const Table& t, int32_t value) noexcept {
	if (t.count == 0) {
		return;
	}
	int i = 0;
	while (i + 1 < t.count && value >= t.rows[i + 1].pct) {
		++i;
	}
	if (i + 1 < t.count) {
		Row(row, t.label, "%d%% = %df, next %d%% (%df)", value, t.rows[i].frames, t.rows[i + 1].pct,
		    t.rows[i + 1].frames);
	} else {
		Row(row, t.label, "%d%% = %df (max)", value, t.rows[i].frames);
	}
}

void ResistRow(int row, const char* label, int32_t raw, int32_t maxBonus, int32_t penalty) noexcept {
	const int32_t effective = raw + penalty;
	int32_t cap = 75 + maxBonus;
	cap = cap > 95 ? 95 : cap;
	int32_t shown = effective > cap ? cap : effective;
	shown = shown < -100 ? -100 : shown;
	if (effective >= cap) {
		Row(row, label, "%d%% / %d%%  (+%d over)", shown, cap, effective - cap);
	} else {
		Row(row, label, "%d%% / %d%%  (%d short)", shown, cap, cap - effective);
	}
}

auto Effective(int32_t mf, int32_t factor) noexcept -> int32_t {
	return mf > 0 ? mf * factor / (mf + factor) : mf;
}

void ShowMessage(const char* message) noexcept {
	Set(WSummary, message);
	for (int i = 0; i < StatsSections; ++i) {
		Set(WHeader0 + i, "");
	}
	for (int r = 0; r < StatsRows; ++r) {
		Blank(r);
	}
}

void Refresh() noexcept {
	if (g_panelWidget == D2RL::Widgets::InvalidHandle) {
		FindStockHandles();
	}
	if (!BindWidgets()) {
		return;
	}
	const int32_t index = g_playerIndex();
	g_unit = index >= 0 && index < 8 ? g_clientPlayer(index) : nullptr;
	if (g_unit == nullptr || At<int32_t>(g_unit, d2r::UnitType) != 0) {
		g_unit = nullptr;
		ShowMessage("No character in game");
		return;
	}
	const int32_t classId = At<int32_t>(g_unit, 0x08);
	const ClassInfo* cls = classId >= 0 && classId < ClassCount ? &Classes[classId] : nullptr;
	const int difficulty = g_difficulty.load();
	const bool knownDifficulty = difficulty >= 0 && difficulty <= 2;
	const int32_t penalty = knownDifficulty ? ResistPenalty[difficulty] : 0;
	char text[160];

	std::snprintf(text, sizeof(text), "Level %d %s  -  %s", S(StLevel), cls ? cls->name : "(unknown class)",
	              knownDifficulty ? Difficulties[difficulty] : "difficulty unknown");
	Set(WSummary, text);
	Set(WFooter, "Updates live. Esc closes.");

	// Breakpoints
	Set(WHeader0 + 0, "Breakpoints");
	int row = 0;
	const int32_t fcr = S(StFcr), fhr = S(StFhr), fbr = S(StFbr);
	if (cls != nullptr) {
		const Table* tables[6] = {&cls->fcr, &cls->fcrAlt, &cls->fhr, &cls->fhrAlt, &cls->fbr, &cls->fbrAlt};
		const int32_t values[6] = {fcr, fcr, fhr, fhr, fbr, fbr};
		for (int i = 0; i < 6; ++i) {
			if (tables[i]->count > 0) {
				BreakpointRow(row++, *tables[i], values[i]);
			}
		}
	} else {
		Row(row++, "Cast rate", "%d%%", fcr);
		Row(row++, "Hit recovery", "%d%%", fhr);
		Row(row++, "Block", "%d%%", fbr);
	}
	Row(row++, "IAS / FRW", "%d%% / %d%%", S(StIas), S(StFrw));
	while (row < 7) {
		Blank(row++);
	}

	// Resistances
	if (knownDifficulty) {
		std::snprintf(text, sizeof(text), "Resistances (%s: %d)", Difficulties[difficulty], penalty);
	} else {
		std::snprintf(text, sizeof(text), "Resistances (no difficulty penalty applied)");
	}
	Set(WHeader0 + 1, text);
	ResistRow(7, "Fire", S(StFireRes), S(StMaxFireRes), penalty);
	ResistRow(8, "Cold", S(StColdRes), S(StMaxColdRes), penalty);
	ResistRow(9, "Lightning", S(StLightRes), S(StMaxLightRes), penalty);
	ResistRow(10, "Poison", S(StPoisonRes), S(StMaxPoisonRes), penalty);
	ResistRow(11, "Magic", S(StMagicRes), S(StMaxMagicRes), 0);
	Row(12, "Poison/curse len", "-%d%% / -%d%%", S(StPoisonLength), S(StCurseRes));

	// Defense
	Set(WHeader0 + 2, "Defense");
	const int32_t dr = S(StDr);
	Row(13, "Dmg reduced", dr > 50 ? "%d%% (cap 50), %d flat" : "%d%%, %d flat", dr, S(StNormalDr));
	Row(14, "Magic dmg red.", "%d", S(StMagicDr));
	Row(15, "Absorb %", "F%d C%d L%d M%d", S(StAbsorbFirePct), S(StAbsorbColdPct), S(StAbsorbLightPct),
	    S(StAbsorbMagicPct));
	Row(16, "Absorb flat", "F%d C%d L%d M%d", S(StAbsorbFire), S(StAbsorbCold), S(StAbsorbLight),
	    S(StAbsorbMagic));
	Row(17, "Freeze", "%s", S(StCannotBeFrozen) > 0 ? "Cannot be frozen"
	                        : (S(StHalfFreeze) > 0 ? "Half freeze duration" : "No protection"));

	// Item find
	Set(WHeader0 + 3, "Item Find");
	const int32_t mf = S(StMagicFind);
	Row(18, "Magic find", "%d%% (U%d S%d R%d)", mf, Effective(mf, 250), Effective(mf, 500),
	    Effective(mf, 600));
	Row(19, "Gold find", "%d%%", S(StGoldFind));
	Row(20, "Experience", "%+d%%", S(StExperience));

	// Offense
	Set(WHeader0 + 4, "Offense");
	Row(21, "Crushing blow", "%d%%", S(StCrushingBlow));
	Row(22, "Deadly strike", "%d%%", S(StDeadlyStrike));
	Row(23, "Open wounds", "%d%%", S(StOpenWounds));
	Row(24, "Leech life/mana", "%d%% / %d%%", S(StLifeLeech), S(StManaLeech));
	Row(25, "-Enemy res %", "F%d C%d L%d P%d M%d", S(StPierceFire) + S(StPassiveFirePierce),
	    S(StPierceCold) + S(StPassiveColdPierce), S(StPierceLight) + S(StPassiveLightPierce),
	    S(StPiercePoison) + S(StPassivePoisonPierce), S(StPassiveMagicPierce));
	Row(26, "+Skill dmg %", "F%d C%d L%d P%d M%d", S(StFireMastery), S(StColdMastery),
	    S(StLightMastery), S(StPoisonMastery), S(StMagicMastery));
	Row(27, "Skills / AR", "+%d all, +%d class, AR+%d%%", S(StAllSkills),
	    classId >= 0 ? S(StClassSkills, classId) : 0, S(StArPercent));

	// Sustain
	Set(WHeader0 + 5, "Sustain");
	Row(28, "Replenish life", "%d", S(StReplenishLife));
	Row(29, "Mana regen", "%d%%", S(StManaRegen));
	Row(30, "Life/mana kill", "%d / %d", S(StLifePerKill), S(StManaPerKill));
	Row(31, "Thorns", "%d, lightning %d", S(StThorns), S(StLightThorns));
	Row(32, "Dmg to mana", "%d%%", S(StDamageToMana));
	g_unit = nullptr;
	for (void* widget : g_stockNative) {
		if (widget != nullptr) {
			using SetVisibleFn = void (*)(void* self, bool visible) noexcept;
			const auto vtable = *reinterpret_cast<SetVisibleFn* const*>(widget);
			vtable[0x50 / sizeof(void*)](widget, false);
		}
	}
	if (!g_loggedFirstFill) {
		g_loggedFirstFill = true;
		char line[160];
		std::snprintf(line, sizeof(line), "mod-core: stats filled the Advanced Stats panel (panel found %ld time(s))",
		              g_captures.load());
		g_ctx->LogInfo(line);
	}
}

// --- Threads ----------------------------------------------------------------------------------------------------
void __cdecl ReadDifficulty(const D2RL::PluginContext*, void*) noexcept {
	Game* game = g_serverGame != nullptr ? g_serverGame() : nullptr;
	const int value = game != nullptr ? At<uint8_t>(game, GameDifficulty) : -1;
	g_difficulty.store(value >= 0 && value <= 2 ? value : -1);
}

// True while the stock Advanced Stats panel is on screen. Without getWidgetState (older loaders) a found panel counts.
auto PanelVisible() noexcept -> bool {
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	if (g_widgets->findPanel(g_ctx, PanelName, &panel) != D2RL::Widgets::Result::Success) {
		g_panelWidget = D2RL::Widgets::InvalidHandle;
		// D2RLoader 1.3.1 cannot look up stock panels; ask D2R through the captured UI root instead.
		void* root = g_uiRoot.load();
		return root != nullptr ? g_childByName(root, PanelName) != nullptr : g_nativePanel.load() != nullptr;
	}
	if (!g_hasWidgetState) {
		return true;
	}
	D2RL::Widgets::State state {};
	return g_widgets->getWidgetState(g_ctx, panel, &state) == D2RL::Widgets::Result::Success && state.visible;
}

bool g_wasVisible = false;
std::atomic<ULONGLONG> g_lastTick {0};  // last time Tick ran; 0 = never
ULONGLONG g_lastDifficultyRead = 0;

// Runs on every root-widget update (re-queued by itself); does real work every RefreshMs.
int g_diagLogged = 0;
ULONGLONG g_lastDiag = 0;

// Every 10 s while the panel is not bound, log what the loader and the hook see (at most 6 times).
void Diagnose(ULONGLONG now) noexcept {
	if (g_boundPanel != nullptr || g_diagLogged >= 6 || now - g_lastDiag < 10000) {
		return;
	}
	g_lastDiag = now;
	++g_diagLogged;
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle, summary = D2RL::Widgets::InvalidHandle,
	                            list = D2RL::Widgets::InvalidHandle;
	const auto panelResult = g_widgets->findPanel(g_ctx, PanelName, &panel);
	auto summaryResult = D2RL::Widgets::Result::NotFound, listResult = D2RL::Widgets::Result::NotFound;
	if (panelResult == D2RL::Widgets::Result::Success) {
		summaryResult = g_widgets->findWidget(g_ctx, panel, "EoESummary", &summary);
		listResult = g_widgets->findWidget(g_ctx, panel, "ListContainer", &list);
	}
	char line[512];
	std::snprintf(line, sizeof(line),
	              "mod-core: stats diag %d: findPanel(AdvancedStatsPanel) %u, EoESummary %u, ListContainer %u | "
	              "child lookups hooked %ld, panel captures %ld, native panel %p, ui root %p (%ld), root->AdvancedStatsPanel %p",
	              g_diagLogged, static_cast<unsigned>(panelResult), static_cast<unsigned>(summaryResult),
	              static_cast<unsigned>(listResult), g_hookCalls.load(), g_captures.load(), g_nativePanel.load(), g_uiRoot.load(),
	              g_rootCaptures.load(), g_uiRoot.load() ? g_childByName(g_uiRoot.load(), PanelName) : nullptr);
	g_ctx->LogInfo(line);
	if (g_diagLogged == 2 || g_diagLogged == 6) {
		size_t used = std::snprintf(line, sizeof(line), "mod-core: stats diag names (%d):", g_seenCount);
		for (int i = 0; i < g_seenCount; ++i) {
			if (used + 26 >= sizeof(line)) {
				g_ctx->LogInfo(line);
				used = std::snprintf(line, sizeof(line), "mod-core: stats diag names:");
			}
			used += std::snprintf(line + used, sizeof(line) - used, " %s", g_seen[i]);
		}
		g_ctx->LogInfo(line);
	}
}

void __cdecl Tick(const D2RL::PluginContext*, void*) noexcept {
	const ULONGLONG now = GetTickCount64();
	Diagnose(now);
	g_lastTick.store(now);
	if (now - g_lastRefresh >= RefreshMs) {
		g_lastRefresh = now;
		const bool visible = PanelVisible();
		if (visible) {
			if (!g_wasVisible) {
				g_panelWidget = D2RL::Widgets::InvalidHandle;  // look the widgets up again after every open
			}
			if (!g_wasVisible || now - g_lastDifficultyRead > 5000) {
				g_lastDifficultyRead = now;
				g_threads->runOnGameThread(g_ctx, ReadDifficulty, nullptr);  // Unavailable on a joined TCP/IP client
			}
			Refresh();
		}
		g_wasVisible = visible;
	}
	g_threads->runOnUiThread(g_ctx, Tick, nullptr);
}

// Safety net: when a character enters a game (UI thread), restart the update if it is not running.
void __cdecl OnGameplay(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
	const ULONGLONG last = g_lastTick.load();
	if (last == 0 || GetTickCount64() - last > 2000) {
		g_threads->runOnUiThread(g_ctx, Tick, nullptr);
	}
}

}  // namespace

auto InstallStatsPanel(const D2RL::PluginContext* context, char* report, size_t reportSize) noexcept -> bool {
	g_ctx = context;
	const bool indexOk = context->CheckExpectedBytes(RvaLocalPlayerIndex, LocalPlayerIndexEntry, sizeof(LocalPlayerIndexEntry));
	const bool playerOk = context->CheckExpectedBytes(RvaClientPlayer, ClientPlayerEntry, sizeof(ClientPlayerEntry));
	const bool gameOk = context->CheckExpectedBytes(RvaLocalServerGame, LocalServerGameEntry, sizeof(LocalServerGameEntry));
	if (!indexOk || !playerOk || d2r::api.unitStat == nullptr) {
		std::snprintf(report, reportSize, "off: player functions differ (index %d, player %d)", indexOk, playerOk);
		return false;
	}
	g_playerIndex = reinterpret_cast<LocalPlayerIndexFn>(context->exeBase + RvaLocalPlayerIndex);
	g_clientPlayer = reinterpret_cast<ClientPlayerFn>(context->exeBase + RvaClientPlayer);
	g_serverGame = gameOk ? reinterpret_cast<LocalServerGameFn>(context->exeBase + RvaLocalServerGame) : nullptr;

	// Report what this loader provides, so a missing service is visible in the log.
	char line[256];
	const D2RL::ServiceQueryResult widgetResult = context->QueryService(&g_widgets);
	const D2RL::ServiceQueryResult threadResult = context->QueryService(&g_threads);
	std::snprintf(line, sizeof(line), "mod-core: stats services: widget %u (size %u, version %u), thread %u (size %u, version %u)",
	              static_cast<unsigned>(widgetResult), g_widgets ? g_widgets->serviceSize : 0,
	              g_widgets ? g_widgets->serviceVersion : 0, static_cast<unsigned>(threadResult),
	              g_threads ? g_threads->serviceSize : 0, g_threads ? g_threads->serviceVersion : 0);
	context->LogInfo(line);
	if (widgetResult != D2RL::ServiceQueryResult::Success ||
	    !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize) ||
	    threadResult != D2RL::ServiceQueryResult::Success ||
	    !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		std::snprintf(report, reportSize, "off: the widget or thread service is missing");
		return false;
	}
	// D2R's child lookup (hooked) and text setter.
	if (!context->CheckExpectedBytes(RvaChildByName, ChildByNameEntry, sizeof(ChildByNameEntry))) {
		std::snprintf(report, reportSize, "off: child lookup 0x%llx differs (hooked by something else?)",
		              static_cast<unsigned long long>(RvaChildByName));
		return false;
	}
	if (!context->InstallInlineHook(RvaChildByName, ChildByNameEntry, ChildByNameHookBytes, ChildByNameHook, &g_childByName)) {
		std::snprintf(report, reportSize, "off: hook on the child lookup refused");
		return false;
	}
	g_setText = reinterpret_cast<SetTextFn>(context->exeBase + RvaSetText);
	g_hasWidgetState = D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceGetWidgetStateFieldEnd);
	const bool queued = g_threads->runOnUiThread(context, Tick, nullptr) == D2RL::Threads::Result::Success;
	bool listener = false;
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success &&
	    D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
		const D2RL::Lifecycle::GameplayEventListener ready {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.flags = 0,
			.kind = D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
			.reserved = 0,
			.callback = OnGameplay,
			.userData = nullptr,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		listener = lifecycle->registerGameplayEventListener(context, &ready, &handle) == D2RL::Lifecycle::Result::Success;
	}
	if (!queued && !listener) {
		std::snprintf(report, reportSize, "off: could not queue the UI update");
		return false;
	}
	std::snprintf(report, reportSize, "on: fills the Advanced Stats panel (character sheet arrow), child lookup hooked%s, difficulty %s",
	              g_hasWidgetState ? "" : " [no widget state: refreshes whenever the panel exists]",
	              gameOk ? "from the local game" : "unknown (server game function differs)");
	return true;
}
