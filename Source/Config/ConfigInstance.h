#pragma once
#include "Pch.h"
#include "PlayerConfig.h"
#include "OverlayConfig.h"
#include "WorldConfig.h"
#include "RadarConfig.h"
#include <cassert>

struct ReadOnlyPoseOptions
{
	bool VisibleBonesOnly = false;
};

class ConfigInstances
{
public:
	PlayerConfig Player = PlayerConfig(LIT("Player"));
	OverlayConfig Overlay = OverlayConfig(LIT("Overlay"));
	WorldConfig World = WorldConfig(LIT("World"));
	ReadOnlyPoseOptions Aim;
	RadarConfig Radar = RadarConfig(LIT("Radar"));

};
inline ConfigInstances ConfigInstance;
