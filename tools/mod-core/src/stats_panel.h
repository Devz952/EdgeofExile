#pragma once

#include <D2RLPlugin/api.h>

#include <cstddef>

// Edge of Exile advanced stat page: a plugin panel (hotkey K, rebindable in Controls; console "eoestats") that shows
// cast/hit-recovery/block breakpoints, resistances after the difficulty penalty, magic find with its effective values,
// and other combat stats of the local player. Register from D2RLoaderLoadPlugin.
auto InstallStatsPanel(const D2RL::PluginContext* context, char* report, size_t reportSize) noexcept -> bool;
