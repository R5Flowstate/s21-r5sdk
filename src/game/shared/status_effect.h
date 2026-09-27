//=============================================================================//
//
// Purpose: Status Effect code and script definitions
//
//=============================================================================//
#ifndef STATUS_EFFECT_H
#define STATUS_EFFECT_H

struct StatusEffectTimedData
{
	void* __vftable;
	int seComboVars;
	float seTimeEnd;
	float seEaseOut;
	float sePausedTimeRemaining;
};

struct StatusEffectEndlessData
{
	void* __vftable;
	int seComboVars;
	char gap_c[4];
};

#if !defined(CLIENT_DLL)
// seComboVars on the dedi: severity bits 7-14, generation 15-24, type 25-31;
// once widened, generation 15-23 and type 24-31.
bool StatusEffects_WideTypes(void);

inline int StatusEffect_ComboType(const int comboVars)
{
	return StatusEffects_WideTypes()
		? static_cast<int>(static_cast<uint32_t>(comboVars) >> 24)
		: (comboVars >> 25) & 0x7F;
}
#endif // !CLIENT_DLL

#endif // STATUS_EFFECT_H
