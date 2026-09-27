#ifndef AGENT_LINK_H
#define AGENT_LINK_H

// Dedi: once per host frame in HS_RUN. Client: around _Host_RunFrame, so
// console commands queued in Begin run inside that frame and End collects
// what they printed.
void AgentLink_FrameBegin(void);
void AgentLink_FrameEnd(void);

// True on the host frame thread while an agent request runs.
bool AgentLink_IsCapturing(void);

class CSquirrelVM;
void AgentLink_RegisterScriptFunctions(CSquirrelVM* s);

#endif // AGENT_LINK_H
