//=============================================================================//
//
// Purpose: The shdr destructor walks a1+24 as ID3D11* COM objects and calls
//          Release. A permutation that Create*Shader never filled still holds
//          the rpak PagePtr encoding (offset<<32)|page. Observed crash value
//          0x30000000001 is page 1 / offset 768. Releasing that AV-reads it.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier0/memvalidate.h"
#include "tier1/cvar.h"
#include "materialsystem/shader_teardown.h"
#include "ebisusdk/EbisuSDK.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static constexpr unsigned __int64 USER_ADDR_LIMIT = 0x00007FFFFFFFFFFFULL;
static constexpr unsigned __int64 USER_ADDR_FLOOR = 0x0000000000010000ULL;

static ConVar sdk_shader_teardown_guard("sdk_shader_teardown_guard", "1", FCVAR_RELEASE,
	"Skip non-COM pointers in the shdr destructor walk. 0 = pass-through.");

static int s_logBudget = 32;

static bool IsPlausibleComObject(const unsigned __int64 p)
{
	if (p < USER_ADDR_FLOOR || p > USER_ADDR_LIMIT)
		return false;
	if (p & 7)
		return false;
	// Unrelocated PagePtrs live in the low 44 bits ((offset<<32)|page), but
	// so does the whole heap when high-entropy ASLR is off. Cold path: let
	// VirtualQuery separate a real object (readable, readable vtable) from one.
	if ((p >> 44) == 0)
	{
		if (!Mem_IsReadable(reinterpret_cast<const void*>(p), 8))
			return false;
		const unsigned __int64 vt = *reinterpret_cast<const unsigned __int64*>(p);
		return vt >= USER_ADDR_FLOOR && vt <= USER_ADDR_LIMIT &&
			Mem_IsReadable(reinterpret_cast<const void*>(vt), 8);
	}
	return true;
}

static unsigned int ShaderProgramCount(__int64 a1)
{
	unsigned int count = *reinterpret_cast<const unsigned char*>(a1 + 9);
	if (count != 0xFF)
		return count;

	int n = (*reinterpret_cast<const unsigned char*>(a1 + 10) != 0) + 1;
	int n1 = 2 * n;
	if (!*reinterpret_cast<const unsigned char*>(a1 + 11))
		n1 = n;
	int n2 = 2 * n1;
	if (!*reinterpret_cast<const unsigned char*>(a1 + 12))
		n2 = n1;
	int n3 = 2 * n2;
	if (!*reinterpret_cast<const unsigned char*>(a1 + 13))
		n3 = n2;
	count = static_cast<unsigned int>(2 * n3);
	if (!*reinterpret_cast<const unsigned char*>(a1 + 16))
		count = static_cast<unsigned int>(n3);
	return count;
}

static void __fastcall Hook_ShaderDestroy(__int64 a1)
{
	if (!a1)
		return;

	if (!sdk_shader_teardown_guard.GetBool())
	{
		if (v_ShaderDestroy)
			v_ShaderDestroy(a1);
		return;
	}

	if (*reinterpret_cast<const unsigned char*>(a1 + 17))
		return;

	const unsigned int count = ShaderProgramCount(a1);
	const unsigned __int64 array =
		*reinterpret_cast<const unsigned __int64*>(a1 + 24);
	if (!array || !count)
		return;

	for (unsigned int i = 0; i < count; ++i)
	{
		const unsigned __int64 obj =
			*reinterpret_cast<const unsigned __int64*>(array + 24ull * i);
		if (!obj)
			continue;

		if (!IsPlausibleComObject(obj))
		{
			if (s_logBudget > 0)
			{
				--s_logBudget;
				const char* name = nullptr;
				__try { name = *reinterpret_cast<const char* const*>(a1); }
				__except (EXCEPTION_EXECUTE_HANDLER) { name = nullptr; }
				Warning(eDLL_T::CLIENT,
					"[SHDR-TEAR] skip slot %u ptr=0x%llX name='%s'\n",
					i, obj, name ? name : "?");
			}
			continue;
		}

		__try
		{
			void** const vt = *reinterpret_cast<void***>(obj);
			const auto release = reinterpret_cast<void(__fastcall*)(unsigned __int64)>(vt[2]);
			release(obj);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (s_logBudget > 0)
			{
				--s_logBudget;
				Warning(eDLL_T::CLIENT,
					"[SHDR-TEAR] Release faulted slot %u ptr=0x%llX\n", i, obj);
			}
		}
	}
}

void VShaderTeardownGuardS21::GetFun(void) const
{
	// DX12 shaders are bytecode blobs, not COM objects; the destructor has no Release walk.
	if (SDK_IsDx12Exe())
		return;

	// Unique: push rsi / sub rsp / cmp [rcx+11h] / mov rsi,rcx / jnz / movzx eax,[rcx+9]
	Module_FindPattern(g_GameDll,
		"40 56 48 83 EC ?? 80 79 ?? ?? 48 8B F1 75 ?? 0F B6 41")
		.GetPtr(v_ShaderDestroy);

	if (!v_ShaderDestroy)
		Warning(eDLL_T::CLIENT,
			"[SHDR-TEAR] pattern UNRESOLVED -- guard NOT installed\n");
}

void VShaderTeardownGuardS21::Detour(const bool bAttach) const
{
	if (v_ShaderDestroy)
		DetourSetup(&v_ShaderDestroy, &Hook_ShaderDestroy, bAttach);
}
