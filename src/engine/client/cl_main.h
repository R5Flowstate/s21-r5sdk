#pragma once

inline void(*v_CL_Move)(void);
inline void(*v_CL_SendMove)(void);
inline int(*v_CL_EndMovie)(void);
inline int(*v_CL_ClearState)(void);
inline void(*v_CL_RunPrediction)(void);

inline bool g_bClientDLL = false;

// Returns true if this is a client only build.
inline bool IsClientDLL()
{
	return g_bClientDLL;
}

