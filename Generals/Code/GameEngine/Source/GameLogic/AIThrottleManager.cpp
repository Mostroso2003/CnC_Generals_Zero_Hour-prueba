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
// AIThrottleManager.cpp
// OPTIMIZATION TIER 2.1: AI Throttling System Implementation
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"
#include "GameLogic/AIThrottleManager.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/GameLogic.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameClient/Camera.h"
#include "Common/GlobalData.h"

// Static instance
AIThrottleManager* AIThrottleManager::s_instance = NULL;

//-------------------------------------------------------------------------------------------------
// AIThrottleConfig - Default Configuration
//-------------------------------------------------------------------------------------------------
AIThrottleConfig::AIThrottleConfig()
{
	enabled = TRUE;  // Enabled by default (can be disabled in GameData.ini)

	// Distance thresholds (squared values for faster comparison)
	// CRITICAL: < 400 units (20^2 = 400) - always in combat range
	// HIGH:     < 1600 units (40^2 = 1600) - visible on typical zoom
	// MEDIUM:   < 6400 units (80^2 = 6400) - medium zoom out
	// LOW:      < 25600 units (160^2 = 25600) - far zoom
	// VERY_LOW: >= 25600 units - very far or outside viewport

	criticalDistanceSq = 400.0f;		// 20 units
	highDistanceSq = 1600.0f;				// 40 units
	mediumDistanceSq = 6400.0f;			// 80 units
	lowDistanceSq = 25600.0f;				// 160 units

	// Update intervals (in frames @ 30 FPS)
	updateInterval[AI_PRIORITY_CRITICAL] = 1;		// Every frame (no throttle)
	updateInterval[AI_PRIORITY_HIGH] = 2;				// Every 2 frames (15 FPS)
	updateInterval[AI_PRIORITY_MEDIUM] = 5;			// Every 5 frames (6 FPS)
	updateInterval[AI_PRIORITY_LOW] = 10;				// Every 10 frames (3 FPS)
	updateInterval[AI_PRIORITY_VERY_LOW] = 20;	// Every 20 frames (1.5 FPS)

	// Adaptive throttling
	adaptiveThrottling = TRUE;
	targetFrameTimeMs = 30;					// 33 FPS target
	maxThrottleMultiplier = 3;			// Can triple intervals under heavy load
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Singleton Management
//-------------------------------------------------------------------------------------------------
AIThrottleManager* AIThrottleManager::getInstance()
{
	if (!s_instance)
	{
		s_instance = NEW AIThrottleManager();
	}
	return s_instance;
}

void AIThrottleManager::destroyInstance()
{
	if (s_instance)
	{
		DELETE s_instance;
		s_instance = NULL;
	}
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Constructor/Destructor
//-------------------------------------------------------------------------------------------------
AIThrottleManager::AIThrottleManager()
{
	m_currentFrame = 0;
	m_frameTimeIndex = 0;
	m_currentThrottleMultiplier = 1;

	// Initialize frame time buffer
	for (int i = 0; i < 30; ++i)
	{
		m_recentFrameTimes[i] = 30;  // Default to 30ms
	}

	// Initialize stats
	m_stats.totalAIs = 0;
	m_stats.criticalAIs = 0;
	m_stats.highAIs = 0;
	m_stats.mediumAIs = 0;
	m_stats.lowAIs = 0;
	m_stats.veryLowAIs = 0;
	m_stats.updatesThisFrame = 0;
	m_stats.updatesSaved = 0;
	m_stats.savedPercentage = 0.0f;

	// Use default configuration
	init(AIThrottleConfig());
}

AIThrottleManager::~AIThrottleManager()
{
	// Nothing to clean up
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Initialization
//-------------------------------------------------------------------------------------------------
void AIThrottleManager::init(const AIThrottleConfig& config)
{
	m_config = config;
	reset();
}

void AIThrottleManager::reset()
{
	m_currentFrame = 0;
	m_frameTimeIndex = 0;
	m_currentThrottleMultiplier = 1;

	// Reset stats
	m_stats.totalAIs = 0;
	m_stats.criticalAIs = 0;
	m_stats.highAIs = 0;
	m_stats.mediumAIs = 0;
	m_stats.lowAIs = 0;
	m_stats.veryLowAIs = 0;
	m_stats.updatesThisFrame = 0;
	m_stats.updatesSaved = 0;
	m_stats.savedPercentage = 0.0f;
}

void AIThrottleManager::setConfig(const AIThrottleConfig& config)
{
	m_config = config;
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Per-Frame Update
//-------------------------------------------------------------------------------------------------
void AIThrottleManager::update()
{
	m_currentFrame = TheGameLogic->getFrame();

	// Update adaptive throttle multiplier if enabled
	if (m_config.adaptiveThrottling)
	{
		m_currentThrottleMultiplier = calculateAdaptiveMultiplier();
	}
	else
	{
		m_currentThrottleMultiplier = 1;
	}

	// Reset per-frame stats
	m_stats.updatesThisFrame = 0;
	m_stats.totalAIs = 0;
	m_stats.criticalAIs = 0;
	m_stats.highAIs = 0;
	m_stats.mediumAIs = 0;
	m_stats.lowAIs = 0;
	m_stats.veryLowAIs = 0;
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Should Update?
//-------------------------------------------------------------------------------------------------
Bool AIThrottleManager::shouldUpdateThisFrame(AIUpdateInterface* ai)
{
	// Check global configuration first
	if (TheGlobalData && !TheGlobalData->m_enableAIThrottling)
	{
		// Throttling disabled globally in GameData.ini
		return TRUE;
	}

	if (!m_config.enabled)
	{
		// Throttling disabled, always update
		return TRUE;
	}

	if (!ai)
	{
		return FALSE;
	}

	Object* obj = ai->getObject();
	if (!obj)
	{
		return FALSE;
	}

	// Get priority for this AI
	Coord3D cameraPos = getCameraPosition();
	AIUpdatePriority priority = calculatePriority(ai, cameraPos);

	// Update stats
	m_stats.totalAIs++;
	switch (priority)
	{
		case AI_PRIORITY_CRITICAL:  m_stats.criticalAIs++; break;
		case AI_PRIORITY_HIGH:      m_stats.highAIs++; break;
		case AI_PRIORITY_MEDIUM:    m_stats.mediumAIs++; break;
		case AI_PRIORITY_LOW:       m_stats.lowAIs++; break;
		case AI_PRIORITY_VERY_LOW:  m_stats.veryLowAIs++; break;
		default: break;
	}

	// Get update interval for this priority
	Int interval = m_config.updateInterval[priority];

	// Apply adaptive multiplier (except for CRITICAL priority)
	if (priority != AI_PRIORITY_CRITICAL && m_currentThrottleMultiplier > 1)
	{
		interval *= m_currentThrottleMultiplier;
	}

	// Ensure interval is at least 1
	if (interval < 1)
	{
		interval = 1;
	}

	// Check if we should update this frame
	// Use object ID as stagger to distribute updates across frames
	UnsignedInt stagger = obj->getID() % interval;
	Bool shouldUpdate = (m_currentFrame % interval) == stagger;

	if (shouldUpdate)
	{
		m_stats.updatesThisFrame++;
	}
	else
	{
		m_stats.updatesSaved++;
	}

	// Calculate saved percentage
	if (m_stats.totalAIs > 0)
	{
		m_stats.savedPercentage = (Real)m_stats.updatesSaved / (Real)m_stats.totalAIs * 100.0f;
	}

	return shouldUpdate;
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Calculate Priority
//-------------------------------------------------------------------------------------------------
AIUpdatePriority AIThrottleManager::calculatePriority(AIUpdateInterface* ai, const Coord3D& cameraPos) const
{
	if (!ai)
	{
		return AI_PRIORITY_VERY_LOW;
	}

	Object* obj = ai->getObject();
	if (!obj)
	{
		return AI_PRIORITY_VERY_LOW;
	}

	// CRITICAL priority: In combat or attacking
	if (isInCombat(ai))
	{
		return AI_PRIORITY_CRITICAL;
	}

	// CRITICAL priority: Selected by player
	if (obj->isSelected())
	{
		return AI_PRIORITY_CRITICAL;
	}

	// Calculate distance to camera (squared for speed)
	const Coord3D* objPos = obj->getPosition();
	Real dx = objPos->x - cameraPos.x;
	Real dy = objPos->y - cameraPos.y;
	Real distanceSq = dx * dx + dy * dy;

	// Determine priority based on distance
	if (distanceSq < m_config.criticalDistanceSq)
	{
		return AI_PRIORITY_CRITICAL;
	}
	else if (distanceSq < m_config.highDistanceSq)
	{
		return AI_PRIORITY_HIGH;
	}
	else if (distanceSq < m_config.mediumDistanceSq)
	{
		return AI_PRIORITY_MEDIUM;
	}
	else if (distanceSq < m_config.lowDistanceSq)
	{
		return AI_PRIORITY_LOW;
	}
	else
	{
		return AI_PRIORITY_VERY_LOW;
	}
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Get Priority
//-------------------------------------------------------------------------------------------------
AIUpdatePriority AIThrottleManager::getPriority(AIUpdateInterface* ai) const
{
	Coord3D cameraPos = getCameraPosition();
	return calculatePriority(ai, cameraPos);
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Force Update Next Frame
//-------------------------------------------------------------------------------------------------
void AIThrottleManager::forceUpdateNextFrame(AIUpdateInterface* ai)
{
	// This is handled automatically by combat detection in shouldUpdateThisFrame()
	// No explicit tracking needed since combat status changes priority to CRITICAL
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Is In Combat?
//-------------------------------------------------------------------------------------------------
Bool AIThrottleManager::isInCombat(AIUpdateInterface* ai) const
{
	if (!ai)
	{
		return FALSE;
	}

	// Check if attacking
	if (ai->isAttacking())
	{
		return TRUE;
	}

	Object* obj = ai->getObject();
	if (!obj)
	{
		return FALSE;
	}

	// Check if recently damaged (within last 5 seconds = 150 frames @ 30 FPS)
	UnsignedInt framesSinceDamaged = TheGameLogic->getFrame() - obj->getRecentlyDamagedFrame();
	if (framesSinceDamaged < 150)
	{
		return TRUE;
	}

	// Check if has a current victim
	Object* victim = ai->getCurrentVictim();
	if (victim && !victim->isEffectivelyDead())
	{
		return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Get Camera Position
//-------------------------------------------------------------------------------------------------
Coord3D AIThrottleManager::getCameraPosition() const
{
	Coord3D result(0, 0, 0);

	// Get the main view/camera
	if (TheDisplay && TheDisplay->getView())
	{
		const Camera* camera = TheDisplay->getView()->getCamera();
		if (camera)
		{
			result = camera->getPosition();
		}
	}

	return result;
}

//-------------------------------------------------------------------------------------------------
// AIThrottleManager - Calculate Adaptive Multiplier
//-------------------------------------------------------------------------------------------------
Int AIThrottleManager::calculateAdaptiveMultiplier() const
{
	if (!m_config.adaptiveThrottling)
	{
		return 1;
	}

	// Calculate average frame time over last 30 frames
	Int totalFrameTime = 0;
	for (int i = 0; i < 30; ++i)
	{
		totalFrameTime += m_recentFrameTimes[i];
	}
	Int avgFrameTime = totalFrameTime / 30;

	// If we're hitting target, no multiplier
	if (avgFrameTime <= m_config.targetFrameTimeMs)
	{
		return 1;
	}

	// Calculate how far over target we are
	Int overTarget = avgFrameTime - m_config.targetFrameTimeMs;

	// Apply multiplier based on how much we're over
	// For every 10ms over target, add 1 to multiplier
	Int multiplier = 1 + (overTarget / 10);

	// Clamp to max multiplier
	if (multiplier > m_config.maxThrottleMultiplier)
	{
		multiplier = m_config.maxThrottleMultiplier;
	}

	return multiplier;
}
