#include <core/stdafx.h>
#include <core/init.h>
#include <launcher/prx.h>

#ifndef CLIENT_DLL
void VPRX::Detour(const bool bAttach) const
{
	//DetourSetup(&v_exit_or_terminate_process, &h_exit_or_terminate_process, bAttach);
}
#endif // !CLIENT_DLL
