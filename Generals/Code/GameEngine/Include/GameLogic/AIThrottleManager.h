/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
// AIThrottleManager.h
// OPTIMIZATION TIER 2.1: AI Throttling System for +40% FPS
// Author: Claude (Anthropic), 2025
//
// This system implements "AI LOD" (Level of Detail) to reduce the frequency
// of AI updates based on:
// 1. Distance from camera (far units update less frequently)
// 2. Combat state (attacking units have higher priority)
// 3. Adaptive throttling (reduces updates under high load)
//
// Expected Impact: +40% FPS with 1,000+ units
////////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef _AI_THROTTLE_MANAGER_H_
#define _AI_THROTTLE_MANAGER_H_

#include "Common/GameCommon.h"
#include "Common/Coord3D.h"

class AIUpdateInterface;
class Object;
class Camera;

//-------------------------------------------------------------------------------------------------
/// AI Update Priority Levels
//-------------------------------------------------------------------------------------------------
enum AIUpdatePriority
{
	AI_PRIORITY_CRITICAL = 0,		///< In combat, always update (every frame)
	AI_PRIORITY_HIGH = 1,				///< Near camera or important (every 2-3 frames)
	AI_PRIORITY_MEDIUM = 2,			///< Medium distance (every 5-7 frames)
	AI_PRIORITY_LOW = 3,				///< Far from camera (every 10-15 frames)
	AI_PRIORITY_VERY_LOW = 4,		///< Very far and idle (every 20-30 frames)

	AI_PRIORITY_COUNT
};

//-------------------------------------------------------------------------------------------------
/// Configuration for AI Throttling System
//-------------------------------------------------------------------------------------------------
struct AIThrottleConfig
{
	Bool enabled;										///< Master enable/disable for throttling

	// Distance thresholds (squared for faster comparison)
	Real criticalDistanceSq;				///< Distance for CRITICAL priority (always < this)
	Real highDistanceSq;						///< Distance for HIGH priority
	Real mediumDistanceSq;					///< Distance for MEDIUM priority
	Real lowDistanceSq;							///< Distance for LOW priority
	// Anything beyond lowDistanceSq is VERY_LOW

	// Update intervals (in frames)
	Int updateInterval[AI_PRIORITY_COUNT];	///< How often to update each priority level

	// Adaptive throttling
	Bool adaptiveThrottling;				///< Enable adaptive throttling based on frame time
	Int targetFrameTimeMs;					///< Target frame time (30ms = 33 FPS)
	Int maxThrottleMultiplier;			///< Max multiplier for intervals under heavy load

	AIThrottleConfig();							///< Constructor with default values
};

//-------------------------------------------------------------------------------------------------
/// AI Throttle Manager - Singleton that manages AI update frequency
//-------------------------------------------------------------------------------------------------
class AIThrottleManager
{
public:
	static AIThrottleManager* getInstance();
	static void destroyInstance();

	/// Initialize the manager with configuration
	void init(const AIThrottleConfig& config);

	/// Reset the manager (called at game start)
	void reset();

	/// Update per-frame (called once per game frame)
	void update();

	/// Should this AI update this frame?
	/// Returns true if the AI should run its update() this frame
	Bool shouldUpdateThisFrame(AIUpdateInterface* ai);

	/// Get the priority level for an AI
	AIUpdatePriority getPriority(AIUpdateInterface* ai) const;

	/// Force an AI to update next frame (for combat events, etc.)
	void forceUpdateNextFrame(AIUpdateInterface* ai);

	/// Get/Set configuration
	const AIThrottleConfig& getConfig() const { return m_config; }
	void setConfig(const AIThrottleConfig& config);

	/// Enable/disable throttling at runtime
	void setEnabled(Bool enabled) { m_config.enabled = enabled; }
	Bool isEnabled() const { return m_config.enabled; }

	/// Get statistics for debugging
	struct Stats
	{
		Int totalAIs;
		Int criticalAIs;
		Int highAIs;
		Int mediumAIs;
		Int lowAIs;
		Int veryLowAIs;
		Int updatesThisFrame;
		Int updatesSaved;
		Real savedPercentage;
	};
	const Stats& getStats() const { return m_stats; }

private:
	AIThrottleManager();
	~AIThrottleManager();

	/// Calculate priority based on distance and state
	AIUpdatePriority calculatePriority(AIUpdateInterface* ai, const Coord3D& cameraPos) const;

	/// Check if AI is in combat
	Bool isInCombat(AIUpdateInterface* ai) const;

	/// Get camera position
	Coord3D getCameraPosition() const;

	/// Calculate adaptive throttle multiplier based on frame time
	Int calculateAdaptiveMultiplier() const;

private:
	static AIThrottleManager* s_instance;

	AIThrottleConfig m_config;
	UnsignedInt m_currentFrame;
	Stats m_stats;

	// Adaptive throttling state
	Int m_recentFrameTimes[30];			///< Ring buffer of recent frame times
	Int m_frameTimeIndex;						///< Index into ring buffer
	Int m_currentThrottleMultiplier;///< Current adaptive multiplier
};

//-------------------------------------------------------------------------------------------------
// Global accessor
//-------------------------------------------------------------------------------------------------
#define TheAIThrottleManager AIThrottleManager::getInstance()

#endif // _AI_THROTTLE_MANAGER_H_
