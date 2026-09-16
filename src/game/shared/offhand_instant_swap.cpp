//=============================================================================//
// Purpose: return-address-scoped OR-escape on IsOffhandInInteruptResumeableState.
//=============================================================================//
#include "core/stdafx.h"
#include "offhand_instant_swap.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/module.h"
#include "public/tier0/tier0_iface.h"
#include "thirdparty/detours/include/detours.h"

#include <intrin.h>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

static char (*v_IsOffhandInInteruptResumeableState)(int64_t pWeapon) = nullptr;
static const void* s_pGateRetAddr = nullptr; // dispatcher call site + 5 (see GetFun)

//-----------------------------------------------------------------------------
static ConVar bridge_offhand_instant_swap("bridge_offhand_instant_swap", "1", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Honor the S21 'offhand_instant_swap_to_offhand' weapon KV in the S3 offhand "
	"dispatcher's switch-away gate (S3 predates the modvar; without this, "
	"entry-shim tacticals like Vantage's block their real weapon's activation).");

//-----------------------------------------------------------------------------
// One-time weapon-KV scan. Runs at detour bring-up, never from the hook: it
// opens and reads every .txt under the weapons dir (~350 files), which on the
// frame thread stalls the simulation long enough to exhaust the ack ring.
//-----------------------------------------------------------------------------
static bool s_bScanned = false;
static std::vector<std::string> s_instantSwapClasses;

static void OffhandISwap_ScanWeaponKVs(void)
{
	namespace fs = std::filesystem;

	const char* const pszDir = "platform/scripts/weapons";

	std::error_code ec;
	if (!fs::is_directory(pszDir, ec) || ec)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] weapons KV dir not found (cwd-relative "
			"platform/scripts/weapons) -- instant-swap escape inert\n");
		s_bScanned = true;
		return;
	}

	static const char* const kKey = "\"offhand_instant_swap_to_offhand\"";
	int nScanned = 0;

	for (const fs::directory_entry& entry : fs::directory_iterator(pszDir, ec))
	{
		if (ec)
			break;

		if (!entry.is_regular_file() || entry.path().extension() != ".txt")
			continue;

		static constexpr uintmax_t kMaxWeaponKVBytes = 256u * 1024u;
		std::error_code sizeEc;
		const uintmax_t nSize = entry.file_size(sizeEc);
		if (sizeEc || nSize > kMaxWeaponKVBytes)
		{
			Warning(eDLL_T::SERVER,
				"[OFFHAND-ISWAP] skipping oversized weapons KV '%s' (%llu bytes)\n",
				entry.path().string().c_str(),
				static_cast<unsigned long long>(nSize));
			continue;
		}

		nScanned++;

		std::ifstream file(entry.path(), std::ios::binary);
		if (!file.is_open())
			continue;

		std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();

		const size_t keyPos = contents.find(kKey);
		if (keyPos == std::string::npos)
			continue;

		// Honor the BASE-level flag only. Keys after "Mods" are mid-flight mods.
		const size_t modsPos = contents.find("\"Mods\"");
		if (modsPos != std::string::npos && keyPos > modsPos)
			continue;

		// Find the value: the next double-quoted token after the key's closing quote.
		size_t q1 = contents.find('"', keyPos + strlen(kKey));
		if (q1 == std::string::npos)
			continue;

		const size_t q2 = contents.find('"', q1 + 1);
		if (q2 == std::string::npos)
			continue;

		const std::string value = contents.substr(q1 + 1, q2 - q1 - 1);
		if (value == "1")
			s_instantSwapClasses.push_back(entry.path().stem().string());
	}

	std::string classList;
	for (size_t i = 0; i < s_instantSwapClasses.size(); i++)
	{
		if (i)
			classList += ", ";
		classList += s_instantSwapClasses[i];
	}

	Msg(eDLL_T::SERVER, "[OFFHAND-ISWAP] scanned %d weapon KV files, %d instant-swap class(es)%s%s\n",
		nScanned, static_cast<int>(s_instantSwapClasses.size()),
		s_instantSwapClasses.empty() ? "" : ": ", classList.c_str());

	s_bScanned = true;
}

//-----------------------------------------------------------------------------
// Reads the weapon's classname (server CWeapon m_weaponName[65], the same
// offset snapshot_diag.cpp's WEAPINV_WEAPNAME_OFF uses) and checks it against
// the instant-swap class list gathered by OffhandISwap_ScanWeaponKVs.
//-----------------------------------------------------------------------------
static constexpr uintptr_t OFFHANDISWAP_WEAPNAME_OFF = 0x15B0; // server CWeapon m_weaponName[65]

static bool OffhandISwap_WeaponHasFlag(int64_t pWeapon, char (&szName)[65])
{
	szName[0] = '\0';

	// Never scan from here -- this runs inside the simulation.
	if (!s_bScanned)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] weapon KV scan never ran at bring-up "
				"-- instant-swap escape inert\n");
		}
		return false;
	}

	if (s_instantSwapClasses.empty())
		return false;

	const char* const pszSrc = reinterpret_cast<const char*>(pWeapon + OFFHANDISWAP_WEAPNAME_OFF);

	if (!pszSrc[0] || !isprint(static_cast<unsigned char>(pszSrc[0])))
		return false;

	size_t i = 0;
	for (; i < 64 && pszSrc[i] != '\0'; i++)
		szName[i] = pszSrc[i];
	szName[i] = '\0';

	for (const std::string& className : s_instantSwapClasses)
	{
		if (strcmp(szName, className.c_str()) == 0)
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
static char Hook_IsOffhandInInteruptResumeableState(int64_t pWeapon)
{
	const char result = v_IsOffhandInInteruptResumeableState(pWeapon);
	if (result)
		return result;

	if (_ReturnAddress() != s_pGateRetAddr) // only the dispatcher's switch-away gate; other callers keep bare semantics
		return result;

	if (!pWeapon || !bridge_offhand_instant_swap.GetBool())
		return result;

	char szName[65];
	if (!OffhandISwap_WeaponHasFlag(pWeapon, szName))
		return result;

	Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] switch-away gate override: active offhand '%s' has "
		"offhand_instant_swap_to_offhand -- allowing swap to pressed candidate\n", szName);

	return 1;
}

//-----------------------------------------------------------------------------
void VOffhandInstantSwap::GetAdr(void) const
{
	LogFunAdr("IsOffhandInInteruptResumeableState", v_IsOffhandInInteruptResumeableState);
	LogVarAdr("OffhandISwapGateRetAddr", s_pGateRetAddr);
}

void VOffhandInstantSwap::GetFun(void) const
{
	// IsOffhandInInteruptResumeableState prologue (fireMode/weapState gate).
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 83 B9 50 27 00 00 03 75 4F 8B 91 34 12 00 00")
		.GetPtr(v_IsOffhandInInteruptResumeableState);

	// Dispatcher switch-away gate call site (modvar-union path).
	CMemory gate = Module_FindPattern(g_GameDll,
		"41 8B 8C 24 50 27 00 00 8D 41 FF 83 F8 04 77 3F 4D 3B EC 75 2C 83 F9 05 74 10 49 8B CC E8");

	if (gate)
		s_pGateRetAddr = gate.Offset(0x22).RCast<const void*>(); // E8 at +0x1D; return address = +0x22

	Msg(eDLL_T::SERVER, "[OFFHAND-ISWAP] GetFun() resolution: predicate=%p gateRetAddr=%p\n",
		reinterpret_cast<void*>(v_IsOffhandInInteruptResumeableState), s_pGateRetAddr);

	if (!v_IsOffhandInInteruptResumeableState)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] PATTERN MISS -- "
			"IsOffhandInInteruptResumeableState unresolved, instant-swap escape will NOT fire.\n");
	}

	if (!s_pGateRetAddr)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] PATTERN MISS -- dispatcher switch-away gate call "
			"site unresolved, instant-swap escape will NOT fire.\n");
	}
}

void VOffhandInstantSwap::GetVar(void) const
{
	if (!s_bScanned)
		OffhandISwap_ScanWeaponKVs();
}

void VOffhandInstantSwap::Detour(const bool bAttach) const
{
	// Without the gate return address the override cannot be scoped to the
	// single -parity call site -- attaching would wrongly widen all 4 S3
	// callers of the predicate. Skip loudly instead.
	if (!v_IsOffhandInInteruptResumeableState || !s_pGateRetAddr)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] Detour() skipped -- missing resolved "
			"predicate and/or gate return address (see GetFun() warnings above).\n");
		return;
	}

	DetourSetup(&v_IsOffhandInInteruptResumeableState, &Hook_IsOffhandInInteruptResumeableState, bAttach);
}
