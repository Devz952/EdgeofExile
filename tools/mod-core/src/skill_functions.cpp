// New server skill "do" functions (skills.txt column srvdofunc), D2R 3.3.0 build 93847.
//
// How it works
// ------------
// When a skill's cast frame is reached, the server looks up skills.txt's `srvdofunc` value in a table of function
// pointers (d2r::SrvDoTable, RVA 0x238ea00, 191 slots) and calls that slot as
//     int64_t fn(Game* game, Unit* caster, int32_t skillId, int32_t skillLevel)   -> non-zero = the skill ran.
// D2R 3.3 uses slots up to 171. Slots 172-190 are empty (null pointers), so a plugin can put its own functions there.
// In skills.txt, set `srvdofunc` of your skill to the SLOT NUMBER; that is all the data side needs:
//     skills.txt:  skill=MyFan ... srvdofunc=172 srvmissilea=<missile> calc1=<count> calc2=<spread degrees> ...
// (The client side still needs a cltdofunc that animates/creates client missiles, or 0; server-created missiles are
// sent to clients by the game itself.)
//
// Installing: PatchWriteU64(rva of the slot, expected bytes, 8, function address). The expected bytes are eight zero
// bytes: D2RLoader refuses the write if the slot is not empty, so the plugin never overwrites a D2R function or a
// function another plugin already put there. Each slot is reported in the plugin's status command.
//
// Adding a function: write it below with the SrvDoFn signature (noexcept, never throw, check every pointer), add a
// row to Functions[] with a free slot (172-190), and set that number as srvdofunc in skills.txt.
#include "skill_functions.h"

#include "d2r.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using d2r::At;
using d2r::Game;
using d2r::Unit;

using SrvDoFn = int64_t (*)(Game* game, Unit* unit, int32_t skillId, int32_t level) noexcept;

constexpr double Pi = 3.14159265358979323846;

// D2R's Teleport server do function (knowledge/d2r-93847-addresses.md, build 93847).
constexpr int SrvDoTeleportSlot = 27;
constexpr uint64_t RvaSrvDoTeleport = 0x554c40;

// Example (slot 172): a fan of missiles.
//   calc1          number of missiles (at least 1)
//   calc2          total spread of the fan in degrees (0 or less: 30)
//   srvmissilea    the missile
// The missiles start at the caster and fly towards points spread evenly on an arc around the aim direction (caster
// -> skill target). Built from the same helpers as mxl-d2r's missile row (D2Sigma 158), which works in game.
auto SrvDoMissileFan(Game* game, Unit* unit, int32_t skillId, int32_t level) noexcept -> int64_t {
	if (game == nullptr || unit == nullptr) {
		return 0;
	}
	const uint8_t* skill = d2r::api.getSkillRecord(d2r::DataFlag(game), skillId);
	if (skill == nullptr) {
		return 0;
	}
	// Mark the unit as running a skill (every D2R/D2Sigma do function that creates missiles does this first).
	d2r::api.setUnitSkillFlag(unit, 1);

	// Where the player aimed (a point; for a unit target, its position).
	int32_t tx = 0, ty = 0;
	if (d2r::api.skillTargetXY(game, unit, &tx, &ty) == 0) {
		return 1;  // no target point: the cast happened, nothing to create
	}
	int32_t ux = 0, uy = 0;
	if (!d2r::UnitXY(unit, &ux, &uy)) {
		return 0;
	}

	const int32_t missile = At<int16_t>(skill, d2r::SkSrvMissileA);
	if (!d2r::ValidMissile(game, missile)) {
		return 0;
	}
	int32_t count = d2r::EvalCalc(game, unit, skill, d2r::SkCalc1, skillId, level);
	count = count < 1 ? 1 : (count > 64 ? 64 : count);  // a broken formula must not spawn thousands of missiles
	int32_t spread = d2r::EvalCalc(game, unit, skill, d2r::SkCalc2, skillId, level);
	spread = spread <= 0 ? 30 : (spread > 360 ? 360 : spread);

	// Aim direction, normalised to 20 subtiles so the rotated points stay precise for close targets.
	double dx = tx - ux, dy = ty - uy;
	const double length = std::sqrt(dx * dx + dy * dy);
	if (length < 0.5) {
		dx = 1.0;  // target on the caster: fire towards +x
		dy = 0.0;
	} else {
		dx /= length;
		dy /= length;
	}
	constexpr double AimLength = 20.0;

	d2r::MissileParams params;
	std::memset(&params, 0, sizeof(params));
	params.flags = 0x820;  // as mxl-d2r's missile row: start at the origin unit, fly to targetX/targetY
	params.owner = unit;
	params.origin = unit;
	params.missile = missile;
	params.skill = skillId;
	params.skillLevel = level;

	const double first = -spread / 2.0;
	const double step = count > 1 ? static_cast<double>(spread) / (count - 1) : 0.0;
	for (int32_t i = 0; i < count; ++i) {
		const double angle = (count > 1 ? first + step * i : 0.0) * Pi / 180.0;
		const double c = std::cos(angle), s = std::sin(angle);
		params.targetX = ux + static_cast<int32_t>(std::lround((dx * c - dy * s) * AimLength));
		params.targetY = uy + static_cast<int32_t>(std::lround((dx * s + dy * c) * AimLength));
		d2r::api.createMissile(game, &params);
	}
	return 1;
}

// Slot 173: Teleport, then a nova where the caster lands (Edge of Exile "Arcane Warp").
//   srvmissilea    the nova missile (a ring element like vanilla 'nova'; its damage comes from its Skill column)
//   calc1          number of missiles in the ring (1-64; 0 or empty: 64)
// D2R's own Teleport do function (slot 27, read from the table when the plugin installs) does the move with all of
// its rules: no Teleport in town, levels.txt Teleport 0/2, blocked destinations. When the caster actually moved (or
// Teleport reports success), the nova goes off from the caster's new position, the same way the missile fan above
// creates missiles. The first TraceCasts casts are written to the plugin log (return value, positions, missile,
// how many missiles the game created) so a missing nova can be diagnosed from the log.
SrvDoFn g_teleport = nullptr;
const D2RL::PluginContext* g_log = nullptr;
volatile long g_traced = 0;
constexpr long TraceCasts = 40;

void Trace(const char* text) noexcept {
	if (g_log != nullptr && g_traced < TraceCasts) {
		++g_traced;
		g_log->LogInfo(text);
	}
}

auto SrvDoTeleportNova(Game* game, Unit* unit, int32_t skillId, int32_t level) noexcept -> int64_t {
	char line[256];
	if (game == nullptr || unit == nullptr || g_teleport == nullptr) {
		Trace("teleport nova: no game/unit/teleport function");
		return 0;
	}
	const uint8_t* skill = d2r::api.getSkillRecord(d2r::DataFlag(game), skillId);
	if (skill == nullptr) {
		std::snprintf(line, sizeof(line), "teleport nova: skill %d has no record", skillId);
		Trace(line);
		return 0;
	}
	int32_t bx = -1, by = -1;
	d2r::UnitXY(unit, &bx, &by);
	const int64_t ran = g_teleport(game, unit, skillId, level);

	int32_t ux = -1, uy = -1;
	const bool havePos = d2r::UnitXY(unit, &ux, &uy);
	const bool moved = havePos && (ux != bx || uy != by);
	const int32_t missile = At<int16_t>(skill, d2r::SkSrvMissileA);
	const bool validMissile = d2r::ValidMissile(game, missile);
	int32_t count = d2r::EvalCalc(game, unit, skill, d2r::SkCalc1, skillId, level);
	count = count < 1 ? 64 : (count > 64 ? 64 : count);

	int32_t created = 0;
	if ((ran != 0 || moved) && havePos && validMissile) {
		d2r::api.setUnitSkillFlag(unit, 1);
		d2r::MissileParams params;
		std::memset(&params, 0, sizeof(params));
		params.flags = 0x820;  // start at the origin unit (the caster, now at the destination), fly to targetX/targetY
		params.owner = unit;
		params.origin = unit;
		params.missile = missile;
		params.skill = skillId;
		params.skillLevel = level;

		constexpr double AimLength = 20.0;  // subtiles; only the direction matters, the missile's Range sets the radius
		for (int32_t i = 0; i < count; ++i) {
			const double angle = 2.0 * Pi * i / count;
			params.targetX = ux + static_cast<int32_t>(std::lround(std::cos(angle) * AimLength));
			params.targetY = uy + static_cast<int32_t>(std::lround(std::sin(angle) * AimLength));
			created += d2r::api.createMissile(game, &params) != nullptr ? 1 : 0;
		}
	}
	std::snprintf(line, sizeof(line),
	              "teleport nova: skill %d lvl %d | teleport returned %lld | pos %d,%d -> %d,%d (moved %d) | missile %d "
	              "(valid %d) | ring %d, created %d",
	              skillId, level, static_cast<long long>(ran), bx, by, ux, uy, moved ? 1 : 0, missile,
	              validMissile ? 1 : 0, count, created);
	Trace(line);
	return ran != 0 ? ran : (moved ? 1 : 0);
}

struct SlotFunction {
	int slot;          // srvdofunc value to put in skills.txt (172-190)
	SrvDoFn function;
	const char* name;  // shown in the log and the status command
};

constexpr SlotFunction Functions[] = {
	{172, SrvDoMissileFan, "missile fan"},
	{173, SrvDoTeleportNova, "teleport nova"},
};

static_assert(sizeof(Functions) / sizeof(Functions[0]) <= d2r::SrvDoSlots - d2r::SrvDoFirstFree);

}  // namespace

auto InstallSkillFunctions(const D2RL::PluginContext* context, char* report, size_t reportSize) noexcept -> int {
	static constexpr uint8_t Empty[8] = {};  // the slot must still be a null pointer
	int installed = 0;
	size_t used = 0;
	if (reportSize > 0) {
		report[0] = '\0';
	}

	g_log = context;
	// D2R's Teleport do function, as the table holds it now (another plugin's replacement is chained, not skipped).
	g_teleport = *reinterpret_cast<const SrvDoFn*>(context->exeBase + d2r::SrvDoTable +
	                                               static_cast<uint64_t>(SrvDoTeleportSlot) * 8);

	for (const SlotFunction& f : Functions) {
		bool ok = f.slot >= d2r::SrvDoFirstFree && f.slot < d2r::SrvDoSlots;
		const bool needsTeleport = f.function == SrvDoTeleportNova;
		if (ok && needsTeleport && g_teleport == nullptr) {
			ok = false;
		}
		if (ok) {
			const uint64_t rva = d2r::SrvDoTable + static_cast<uint64_t>(f.slot) * 8;
			ok = context->PatchWriteU64(rva, Empty, sizeof(Empty), reinterpret_cast<uint64_t>(f.function));
		}
		installed += ok ? 1 : 0;
		if (used < reportSize) {
			const char* note = ok ? "" : (needsTeleport && g_teleport == nullptr ? " FAILED (Teleport slot 27 is empty)"
			                                                                     : " FAILED (slot not empty or out of range)");
			const bool moved = needsTeleport && g_teleport != nullptr &&
			                   reinterpret_cast<uint64_t>(g_teleport) != context->exeBase + RvaSrvDoTeleport;
			const int n = std::snprintf(report + used, reportSize - used, "%s srvdofunc %d %s%s%s", used ? ";" : "",
			                            f.slot, f.name, note, moved ? " (Teleport slot replaced by another plugin)" : "");
			used += n > 0 ? static_cast<size_t>(n) : 0;
			if (used >= reportSize) {
				used = reportSize;
			}
		}
	}
	return installed;
}

auto SkillFunctionCount() noexcept -> int {
	return static_cast<int>(sizeof(Functions) / sizeof(Functions[0]));
}
