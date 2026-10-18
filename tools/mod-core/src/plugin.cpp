// D2RLoader plugin entry point (template). Copy plugin-template/ into your mod project and extend it.
//
// Target: D2R 3.3.0 build 93847 under D2RLoader 1.3.1, PluginSDK 0.3.0.
//
// What it does:
//  - declares itself Shared (server + client behaviour), ModScopedOnly (loads only from
//    <game>/mods/<Mod>/d2rloader/plugins/ while that mod runs) and NativeHooks (patches game code);
//  - checks the game image (PE TimeDateStamp). Every RVA in d2r.h belongs to one image; on any other image the engine
//    changes stay off and the log says so, so a D2R/D2RLoader update cannot make the plugin patch random code;
//  - reads the diagnosis switch "<plugin>-off.txt" next to the DLL: words naming features to leave out;
//  - installs each feature and logs one line per feature;
//  - registers the console command "<plugin>" that prints the status of every feature.
//
// Adding a feature: write Install<Feature>(context, report, size) in its own file, add a Feature row in Load() with a
// one-word name (the word the off-file uses), and it shows up in the log and the status command.
#include <D2RLPlugin/api.h>

#include "d2r.h"
#include "hook_example.h"
#include "skill_functions.h"
#include "stats_panel.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef MODKIT_PLUGIN_NAME
#define MODKIT_PLUGIN_NAME "mod-core"
#endif
#ifndef MODKIT_PLUGIN_VERSION
#define MODKIT_PLUGIN_VERSION "0.1.0"
#endif

namespace {

constexpr const char* PluginName = MODKIT_PLUGIN_NAME;
constexpr const char* PluginVersion = MODKIT_PLUGIN_VERSION;
constexpr uint32_t ExpectedImage = 0x6ab3782cU;  // TimeDateStamp of D2R 3.3.0 build 93847 under D2RLoader 1.3.1

const D2RL::PluginContext* g_context = nullptr;

constexpr D2RL::PluginInfo Info {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = MODKIT_PLUGIN_NAME,
	.name        = MODKIT_PLUGIN_NAME,
	.version     = MODKIT_PLUGIN_VERSION,
	.author      = "d2r-modkit",
	.description = "Engine changes for this mod (skill functions, hooks).",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::ModScopedOnly | D2RL::PluginFlags::NativeHooks,
};

auto ImageStamp(const D2RL::PluginContext* context) noexcept -> uint32_t {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(context->exeBase);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(context->exeBase + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp;
}

auto BuildMatches(const D2RL::PluginContext* context) noexcept -> bool {
	return context->exeBase != 0 && ImageStamp(context) == ExpectedImage;
}

// --- Diagnosis switch ---------------------------------------------------------------------------------------------
// "<plugin>-off.txt" next to the DLL (e.g. mods/<Mod>/d2rloader/plugins/mod-core-off.txt) lists feature words to leave
// out, separated by spaces, commas or lines, case-insensitive. Delete the file for normal play. Use it to bisect a
// crash or regression: leave half the features out, restart the game, repeat.
char g_offList[256] = "";

void ReadOffList() noexcept {
	HMODULE self = nullptr;
	char path[MAX_PATH] = "";
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(&ReadOffList), &self) ||
	    GetModuleFileNameA(self, path, MAX_PATH) == 0) {
		return;
	}
	// <dir>\<name>.dll -> <dir>\<name>-off.txt (the DLL's own name, so a renamed plugin reads its own file)
	char* dot = std::strrchr(path, '.');
	char* slash = std::strrchr(path, '\\');
	if (dot == nullptr || slash == nullptr || dot < slash || static_cast<size_t>(dot - path) + 9 >= sizeof(path)) {
		return;
	}
	strcpy_s(dot, sizeof(path) - static_cast<size_t>(dot - path), "-off.txt");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
		return;
	}
	const size_t n = std::fread(g_offList, 1, sizeof(g_offList) - 1, f);
	g_offList[n] = '\0';
	std::fclose(f);
	for (char* c = g_offList; *c != '\0'; ++c) {
		*c = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a')
		                              : (std::isalpha(static_cast<unsigned char>(*c)) ? *c : ' ');
	}
}

// True when the off-file names this word (whole words only: "hook" does not match "hooks").
auto Off(const char* part) noexcept -> bool {
	const size_t len = std::strlen(part);
	for (const char* p = std::strstr(g_offList, part); p != nullptr; p = std::strstr(p + 1, part)) {
		if ((p == g_offList || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
			return true;
		}
	}
	return false;
}

// --- Features -----------------------------------------------------------------------------------------------------
constexpr const char* NotInstalled = "off: not the expected game image";
constexpr const char* OffNote = "off: left out by the off-file";

struct Feature {
	const char* word;  // off-file word and label
	char report[512];
};

Feature g_skills {"skills", ""};
Feature g_hook {"hook", ""};
Feature g_stats {"stats", ""};
Feature* const g_features[] = {&g_skills, &g_hook, &g_stats};

void InstallFeatures(const D2RL::PluginContext* context) noexcept {
	char line[640];

	// skills: new srvdofunc functions (skill_functions.cpp)
	if (Off(g_skills.word)) {
		std::snprintf(g_skills.report, sizeof(g_skills.report), "%s", OffNote);
	} else {
		char slots[400];
		const int n = InstallSkillFunctions(context, slots, sizeof(slots));
		std::snprintf(g_skills.report, sizeof(g_skills.report), "%d/%d installed:%s", n, SkillFunctionCount(), slots);
	}

	// hook: inline hook example (hook_example.cpp)
	if (Off(g_hook.word)) {
		std::snprintf(g_hook.report, sizeof(g_hook.report), "%s", OffNote);
	} else {
		InstallHookExample(context, g_hook.report, sizeof(g_hook.report));
	}

	// stats: advanced stat page panel (stats_panel.cpp)
	if (Off(g_stats.word)) {
		std::snprintf(g_stats.report, sizeof(g_stats.report), "%s", OffNote);
	} else {
		InstallStatsPanel(context, g_stats.report, sizeof(g_stats.report));
	}

	for (const Feature* f : g_features) {
		std::snprintf(line, sizeof(line), "%s: %s %s", PluginName, f->word, f->report);
		context->LogInfo(line);
	}
}

// --- Console command ----------------------------------------------------------------------------------------------
auto StatusCommand(D2R::Game::Client* client, const D2RL::ConsoleCommandContext* command, void* userData) noexcept
	-> D2RL::ConsoleCommandResult {
	(void)client;
	(void)userData;
	if (command == nullptr || command->plugin == nullptr || g_context == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char line[640];
	std::snprintf(line, sizeof(line), "%s %s | D2R %s | image 0x%08x (expects 0x%08x, %s) | exe base 0x%llx", PluginName,
	              PluginVersion, g_context->buildVersion ? g_context->buildVersion : "?", ImageStamp(g_context),
	              ExpectedImage, BuildMatches(g_context) ? "match" : "MISMATCH, engine hooks off",
	              static_cast<unsigned long long>(g_context->exeBase));
	command->plugin->WriteConsoleMessage(line);
	if (g_offList[0] != '\0') {
		std::snprintf(line, sizeof(line), "off-file: %s", g_offList);
		command->plugin->WriteConsoleMessage(line);
	}
	for (const Feature* f : g_features) {
		std::snprintf(line, sizeof(line), "%s %s", f->word, f->report);
		command->plugin->WriteConsoleMessage(line);
	}
	std::snprintf(line, sizeof(line), "hook calls so far: %lld", static_cast<long long>(HookExampleCalls()));
	command->plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	g_context = context;
	for (Feature* f : g_features) {
		std::snprintf(f->report, sizeof(f->report), "%s", NotInstalled);
	}
	char line[256];
	std::snprintf(line, sizeof(line), "%s %s loaded (mod %s, D2R %s, image 0x%08x%s)", PluginName, PluginVersion,
	              context->activeMod ? context->activeMod : "?", context->buildVersion ? context->buildVersion : "?",
	              ImageStamp(context), BuildMatches(context) ? "" : "; not the expected image, engine hooks stay off");
	context->LogInfo(line);
	if (BuildMatches(context)) {
		d2r::Bind(context->exeBase);
		ReadOffList();
		if (g_offList[0] != '\0') {
			std::snprintf(line, sizeof(line), "%s: %s-off.txt leaves out: %s", PluginName, PluginName, g_offList);
			context->LogInfo(line);
		}
		InstallFeatures(context);
	}
	return context->RegisterConsoleCommand(PluginName, StatusCommand, "Status of this mod's engine plugin.");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_context = nullptr;
}
