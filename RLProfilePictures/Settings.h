// Settings.h
#pragma once
#include <nlohmann/json.hpp>

struct Settings {
	bool showEpic = true;
	bool showSteam = true;
	bool showPSN = true;
	bool showXbox = true;
	bool showSwitch = true;

    // Enable JSON serialization/deserialization for the new fields
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Settings, showEpic, showSteam, showPSN, showXbox, showSwitch)
};