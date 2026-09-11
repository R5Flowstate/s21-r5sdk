#ifndef CLIENT_GAMEPAD_H
#define CLIENT_GAMEPAD_H

enum WeaponScopeZoomLevel_e // TODO: move to shared game scripts!
{
	kScope1X = 0,
	kScope2X,
	kScope3X,
	kScope4X,
	kScope6X,
	kScope8X,
	kScope10X,
	kScopeUnused,

	kScopeCount // NOTE: not a scope!
};

extern bool GamePad_UseAdvancedAdsScalarsPerScope();
extern float GamePad_GetAdvancedAdsScalarForOptic(const WeaponScopeZoomLevel_e opticType);

inline void (*GamePad_LoadAimAssistScripts)();

struct AimCurveConfig_s // Move to gamepad!
{
	int field_0;
	int field_4;
	int field_8;
	int field_C;
	int field_10;
	int field_14;
	char unknown[224];
};

inline float (*GamePad_CalcOuterDeadzoneCustom)(float a1);
inline float (*GamePad_CalcOuterDeadzone)(AimCurveConfig_s* curve, float a2);




#endif // CLIENT_GAMEPAD_H
