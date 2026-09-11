#ifndef CLOCKDRIFTMGR_H
#define CLOCKDRIFTMGR_H

struct CClockDriftMgr
{
	void Clear();

	enum
	{
		// This controls how much it smooths out the samples from the server.
		NUM_CLOCKDRIFT_SAMPLES = 24
	};

	float m_ClockOffsets[4];
	int m_iCurClockOffset;
	float m_serverFrameTimeScales[NUM_CLOCKDRIFT_SAMPLES];
	int m_serverFrameTimeScaleIndex;
	// Trimmed mean of m_serverFrameTimeScales, clamped to [0.1, 1.0].
	float m_serverFrameTimeScaleAverage;
	// How far the client clock leads the ideal server-derived clock, and the
	// budget AdjustFrameTime may spend removing it. While m_aheadBy > 0 the
	// engine SUBTRACTS from every frame time until the client falls back.
	float m_aheadBy;
	float m_correctWithin;
	unsigned int m_lastPlatTime;
	float m_lastServerTime;
	int m_nServerTick;
	int m_nClientTick;
};
static_assert(sizeof(CClockDriftMgr) == 0x94);

#endif // CLOCKDRIFTMGR_H
