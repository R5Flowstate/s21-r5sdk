//=============================================================================//
//
// Purpose: weapon_ammo_pool_mod.h implementation. See that header.
//
//=============================================================================//
#include "core/stdafx.h"


#include "weapon_ammo_pool_mod.h"
#include "game/shared/weapon_legendary_ext.h"
#include "public/tier1/sdk_parse.h"

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <cstring>

//-----------------------------------------------------------------------------
// CWeaponX layout -- the SERVER weapon class. The dedi binary also carries the
// listen-server client class C_WeaponX, whose code is byte-identical but whose
// offsets and WeaponInfo allocation size differ; none of these numbers are valid
// on that half.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t WEAPONX_OFF_MOD_BITS     = 0x157C; // u32 bit i = mod record i
static constexpr ptrdiff_t WEAPONX_OFF_WEAPON_INFO  = 0x15A8; // WeaponInfo*
static constexpr ptrdiff_t WEAPONX_OFF_WEAPON_NAME  = 0x15B0; // char[0x41], set by PrecacheWeaponInfo

// WeaponInfo layout (server)
static constexpr ptrdiff_t WINFO_OFF_AMMO_POOL_TYPE = 0x268;  // i32 pool index
static constexpr ptrdiff_t WINFO_OFF_MOD_COUNT      = 0x4550; // u32, <= 31
static constexpr int       WINFO_MAX_MODS          = 31;

//-----------------------------------------------------------------------------
// ConVars
//-----------------------------------------------------------------------------
static ConVar bridge_weapon_alt_ammo_pool("bridge_weapon_alt_ammo_pool", "1",
	FCVAR_RELEASE,
	"Let a weapon mod override ammo_pool_type (S3 cannot mod that field natively).");

static ConVar bridge_weapon_alt_ammo_pool_log("bridge_weapon_alt_ammo_pool_log", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log every ammo-pool mod resolve and per-weapon pool switch. [ALT-AMMO]");

//-----------------------------------------------------------------------------
// Engine symbols
//-----------------------------------------------------------------------------
// Both return a value the engine's callers consume, so pass it through verbatim
// rather than narrowing it.
static void* (*v_CWeaponX_PrecacheWeaponInfo)(void* pWeaponX) = nullptr;
static int64_t (*v_CWeaponX_RecalcModdedSettings)(void* pWeaponX) = nullptr;

// Address of the registry global; its value is the array base (stride 24).
static void** g_ppWeaponInfoRegistry = nullptr;
static const uint16_t* g_pAmmoPoolTypeCount = nullptr;
static const char** g_pAmmoPoolTypeNames = nullptr;

static bool s_bPatternsOk = false;

//-----------------------------------------------------------------------------
// Per-weapon-file config (keyed on base WeaponInfo*)
//-----------------------------------------------------------------------------
struct AmmoPoolModOverride_t
{
	int m_nModBit;
	int m_nAltPoolType;
	std::string m_szModName;
	std::string m_szPoolName;
};

struct AmmoPoolWeaponConfig_t
{
	bool m_bResolved = false;
	bool m_bDisabled = false;
	int m_nParsedModCount = 0;
	int m_nBasePoolType = -1;
	std::string m_szWeaponName;
	std::vector<AmmoPoolModOverride_t> m_overrides;
	std::vector<std::string> m_modNames; // bit index -> name, for [ALT-AMMO-MODS]
};

static std::unordered_map<void*, AmmoPoolWeaponConfig_t> s_baseConfigs;

//-----------------------------------------------------------------------------
// Clone cache: (base, altPoolType) -> clone; freed at LevelShutdown
//-----------------------------------------------------------------------------
struct AmmoPoolInfoClone_t
{
	void* m_pBase;
	int m_nAltPoolType;
	void* m_pClone;
};

static std::vector<AmmoPoolInfoClone_t> s_clones;

// Last modBits per weapon entity for change-only [ALT-AMMO-MODS] logging.
static std::unordered_map<void*, uint32_t> s_lastModBits;

//-----------------------------------------------------------------------------
// Name registry: linear scan (filled at runtime from ammo_pool_types.txt)
//-----------------------------------------------------------------------------
static int ResolveAmmoPoolTypeIndex(const char* pszName)
{
	if (!pszName || !pszName[0] || !g_pAmmoPoolTypeCount || !g_pAmmoPoolTypeNames)
		return -1;

	const uint16_t nCount = *g_pAmmoPoolTypeCount;
	for (uint16_t i = 0; i < nCount; ++i)
	{
		const char* psz = g_pAmmoPoolTypeNames[i];
		if (psz && !_stricmp(psz, pszName))
			return static_cast<int>(i);
	}
	return -1;
}

static const char* AmmoPoolTypeName(int nIdx)
{
	if (!g_pAmmoPoolTypeCount || !g_pAmmoPoolTypeNames || nIdx < 0)
		return "?";
	const uint16_t nCount = *g_pAmmoPoolTypeCount;
	if (static_cast<uint16_t>(nIdx) >= nCount || !g_pAmmoPoolTypeNames[nIdx])
		return "?";
	return g_pAmmoPoolTypeNames[nIdx];
}

//-----------------------------------------------------------------------------
// Weapon .txt parser -- walk Mods children for ammo_pool_type overrides
//-----------------------------------------------------------------------------
static AmmoPoolWeaponConfig_t ParseWeaponAmmoPoolMods(const std::string& weaponClassName)
{
	AmmoPoolWeaponConfig_t cfg;
	cfg.m_szWeaponName = weaponClassName;

	if (weaponClassName.find("..") != std::string::npos ||
		weaponClassName.find('/') != std::string::npos ||
		weaponClassName.find('\\') != std::string::npos)
	{
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] Rejected invalid weapon classname '%s'\n",
			weaponClassName.c_str());
		cfg.m_bDisabled = true;
		cfg.m_bResolved = true;
		return cfg;
	}

	const char* searchPaths[] = {
		"platform/scripts/weapons/"
	};

	std::ifstream file;
	std::string lastPath;
	for (const char* basePath : searchPaths)
	{
		lastPath = std::string(basePath) + weaponClassName + ".txt";
		file.clear();
		file.open(lastPath);
		if (file.is_open())
			break;
	}

	if (!file.is_open())
	{
		// Relative paths resolve against the process working directory, so a miss
		// is a deployment question, not a "this weapon has no mods" answer. Staying
		// silent here would make it indistinguishable from a clean parse that found
		// nothing -- which is exactly the question asked when the feature is quiet.
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] could not open weapon KV for '%s' (last tried '%s') -- "
			"no ammo-pool overrides for it\n",
			weaponClassName.c_str(), lastPath.c_str());
		cfg.m_bDisabled = true;
		cfg.m_bResolved = true;
		return cfg;
	}

	int depth = 0;
	bool inMods = false;
	int modsBaseDepth = -1;
	int currentModBit = -1;
	int currentModDepth = -1;
	std::string currentModName;
	std::string pendingKey;
	bool hasPendingBlockKey = false;
	int modCount = 0;
	std::string basePoolName;

	std::string line;
	while (std::getline(file, line))
	{
		const size_t commentPos = line.find("//");
		if (commentPos != std::string::npos)
			line = line.substr(0, commentPos);

		Sdk_TrimWhitespace(line);
		if (line.empty())
			continue;

		size_t pos = 0;
		std::string key, value;
		const bool gotKey = ParseQuotedString(line, pos, key);
		const bool gotValue = gotKey && ParseQuotedString(line, pos, value);

		for (const char c : line)
		{
			if (c == '{')
			{
				++depth;
				if (hasPendingBlockKey)
				{
					if (!inMods && pendingKey == "Mods")
					{
						inMods = true;
						modsBaseDepth = depth;
					}
					else if (inMods && depth == modsBaseDepth + 1)
					{
						if (modCount < WINFO_MAX_MODS)
						{
							currentModBit = modCount;
							currentModName = pendingKey;
							currentModDepth = depth;
							cfg.m_modNames.push_back(pendingKey);
							++modCount;
						}
						else
						{
							currentModBit = -1;
							currentModDepth = -1;
						}
					}
					hasPendingBlockKey = false;
				}
			}
			else if (c == '}')
			{
				if (inMods && currentModBit >= 0 && depth == currentModDepth)
				{
					currentModBit = -1;
					currentModDepth = -1;
					currentModName.clear();
				}
				if (inMods && depth == modsBaseDepth)
				{
					inMods = false;
					modsBaseDepth = -1;
				}
				--depth;
			}
		}

		if (gotKey && !gotValue)
		{
			hasPendingBlockKey = true;
			pendingKey = key;
		}
		else if (gotKey && gotValue)
		{
			hasPendingBlockKey = false;
			if (inMods && currentModBit >= 0 && key == "ammo_pool_type")
			{
				AmmoPoolModOverride_t ov;
				ov.m_nModBit = currentModBit;
				ov.m_szModName = currentModName;
				ov.m_szPoolName = value;
				ov.m_nAltPoolType = -1; // resolve later once name registry is live
				cfg.m_overrides.push_back(std::move(ov));
			}
			else if (!inMods && key == "ammo_pool_type" && basePoolName.empty())
			{
				basePoolName = value;
			}
		}
	}

	file.close();

	cfg.m_nParsedModCount = modCount;
	cfg.m_bResolved = true;

	// Resolve pool names now if the registry is already filled.
	if (!basePoolName.empty())
		cfg.m_nBasePoolType = ResolveAmmoPoolTypeIndex(basePoolName.c_str());

	// An unresolved name is KEPT, not dropped: the config is cached per weapon and
	// the pool-name registry fills at runtime, so dropping here would permanently
	// disable an override that becomes resolvable a moment later. The fixup treats
	// a negative pool as "no override".
	for (AmmoPoolModOverride_t& ov : cfg.m_overrides)
	{
		ov.m_nAltPoolType = ResolveAmmoPoolTypeIndex(ov.m_szPoolName.c_str());
		if (ov.m_nAltPoolType < 0)
		{
			Warning(eDLL_T::SERVER,
				"[ALT-AMMO] '%s' mod[%d] '%s' pool name '%s' not in the engine pool registry yet -- will retry\n",
				weaponClassName.c_str(), ov.m_nModBit, ov.m_szModName.c_str(),
				ov.m_szPoolName.c_str());
		}
	}

	return cfg;
}

static void EnsureOverridesResolved(AmmoPoolWeaponConfig_t& cfg)
{
	// Name registry fills at runtime; retry unresolved names once it has entries.
	if (!g_pAmmoPoolTypeCount || *g_pAmmoPoolTypeCount == 0)
		return;

	for (AmmoPoolModOverride_t& ov : cfg.m_overrides)
	{
		if (ov.m_nAltPoolType < 0 && !ov.m_szPoolName.empty())
			ov.m_nAltPoolType = ResolveAmmoPoolTypeIndex(ov.m_szPoolName.c_str());
	}
}

static AmmoPoolWeaponConfig_t* GetOrBuildConfig(void* pBase, void* pWeaponX)
{
	auto it = s_baseConfigs.find(pBase);
	if (it != s_baseConfigs.end())
	{
		EnsureOverridesResolved(it->second);
		return &it->second;
	}

	const char* pszName = reinterpret_cast<const char*>(
		reinterpret_cast<const char*>(pWeaponX) + WEAPONX_OFF_WEAPON_NAME);
	std::string weaponName;
	if (pszName && pszName[0])
	{
		// Forced null at name[0x40]; still bound the read.
		weaponName.assign(pszName, strnlen(pszName, 0x40));
	}

	AmmoPoolWeaponConfig_t cfg;
	if (!weaponName.empty())
		cfg = ParseWeaponAmmoPoolMods(weaponName);
	else
		cfg.m_bResolved = true;

	// Order check: parsed Mods child count vs engine mod count at pInfo+0x4538.
	if (!cfg.m_overrides.empty() && pBase)
	{
		const uint32_t engineModCount = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<const char*>(pBase) + WINFO_OFF_MOD_COUNT);
		if (static_cast<uint32_t>(cfg.m_nParsedModCount) != engineModCount)
		{
			Warning(eDLL_T::SERVER,
				"[ALT-AMMO] '%s' mod-count mismatch: file=%d engine=%u -- overrides DISABLED\n",
				weaponName.c_str(), cfg.m_nParsedModCount, engineModCount);
			cfg.m_bDisabled = true;
			cfg.m_overrides.clear();
		}
	}

	// Prefer live base pool from the POD if we did not get a file default.
	if (cfg.m_nBasePoolType < 0 && pBase)
	{
		cfg.m_nBasePoolType = *reinterpret_cast<const int32_t*>(
			reinterpret_cast<const char*>(pBase) + WINFO_OFF_AMMO_POOL_TYPE);
	}

	if (bridge_weapon_alt_ammo_pool_log.GetBool())
	{
		const uint32_t engineModCount = pBase
			? *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<const char*>(pBase) + WINFO_OFF_MOD_COUNT)
			: 0u;
		Msg(eDLL_T::SERVER,
			"[ALT-AMMO] config '%s' base=%p fileMods=%d engineMods=%u overrides=%zu basePool=%d%s\n",
			weaponName.c_str(), pBase, cfg.m_nParsedModCount, engineModCount,
			cfg.m_overrides.size(), cfg.m_nBasePoolType,
			cfg.m_bDisabled ? " DISABLED" : "");
		for (const AmmoPoolModOverride_t& ov : cfg.m_overrides)
		{
			Msg(eDLL_T::SERVER,
				"[ALT-AMMO] '%s' mod[%d] '%s' -> pool '%s'(%d), base pool %d (engine modCount=%u)\n",
				weaponName.c_str(), ov.m_nModBit, ov.m_szModName.c_str(),
				ov.m_szPoolName.c_str(), ov.m_nAltPoolType,
				cfg.m_nBasePoolType, engineModCount);
		}
	}

	auto inserted = s_baseConfigs.emplace(pBase, std::move(cfg));
	return &inserted.first->second;
}

//-----------------------------------------------------------------------------
// Clone helpers
//-----------------------------------------------------------------------------
static void* CloneToBase(void* pCur)
{
	if (!pCur)
		return nullptr;

	for (const AmmoPoolInfoClone_t& c : s_clones)
	{
		if (c.m_pClone == pCur)
			return c.m_pBase;
	}
	return pCur;
}

static void* GetOrCreateClone(void* pBase, int nAltPoolType)
{
	for (const AmmoPoolInfoClone_t& c : s_clones)
	{
		if (c.m_pBase == pBase && c.m_nAltPoolType == nAltPoolType)
			return c.m_pClone;
	}

	const size_t nSize = WeaponLegendaryExt_ServerWeaponInfoSize();
	void* pClone = malloc(nSize);
	if (!pClone)
	{
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] malloc(0x%zX) failed for WeaponInfo clone -- pool override skipped\n",
			nSize);
		return pBase;
	}

	memcpy(pClone, pBase, nSize);
	*reinterpret_cast<int32_t*>(
		reinterpret_cast<char*>(pClone) + WINFO_OFF_AMMO_POOL_TYPE) = nAltPoolType;

	AmmoPoolInfoClone_t entry;
	entry.m_pBase = pBase;
	entry.m_nAltPoolType = nAltPoolType;
	entry.m_pClone = pClone;
	s_clones.push_back(entry);
	return pClone;
}

//-----------------------------------------------------------------------------
// Fixup: retarget weapon+0x16B8 after Precache / RecalcModdedSettings
//-----------------------------------------------------------------------------
static void WeaponAmmoPoolMod_Fixup(void* pWeaponX)
{
	static bool s_bFirstCall = true;
	if (s_bFirstCall)
	{
		s_bFirstCall = false;
		if (bridge_weapon_alt_ammo_pool_log.GetBool())
		{
			Msg(eDLL_T::SERVER, "[ALT-AMMO] fixup first call (enabled=%d patternsOk=%d)\n",
				bridge_weapon_alt_ammo_pool.GetBool() ? 1 : 0, s_bPatternsOk ? 1 : 0);
		}
	}

	if (!bridge_weapon_alt_ammo_pool.GetBool() || !s_bPatternsOk || !pWeaponX)
		return;

	void** ppInfo = reinterpret_cast<void**>(
		reinterpret_cast<char*>(pWeaponX) + WEAPONX_OFF_WEAPON_INFO);
	void* pCur = *ppInfo;
	if (!pCur)
		return;

	void* pBase = CloneToBase(pCur);
	if (!pBase)
		return;

	AmmoPoolWeaponConfig_t* pCfg = GetOrBuildConfig(pBase, pWeaponX);
	if (!pCfg || pCfg->m_bDisabled || pCfg->m_overrides.empty())
	{
		*ppInfo = pBase;
		return;
	}

	const uint32_t modBits = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pWeaponX) + WEAPONX_OFF_MOD_BITS);

	// Highest set bit with an override wins (engine applies ascending; later wins).
	int nAltPool = -1;
	int nWinningBit = -1;
	for (const AmmoPoolModOverride_t& ov : pCfg->m_overrides)
	{
		if (ov.m_nModBit < 0 || ov.m_nModBit >= 32)
			continue;
		if (modBits & (1u << ov.m_nModBit))
		{
			if (ov.m_nModBit >= nWinningBit)
			{
				nWinningBit = ov.m_nModBit;
				nAltPool = ov.m_nAltPoolType;
			}
		}
	}

	const int nOldPool = *reinterpret_cast<const int32_t*>(
		reinterpret_cast<const char*>(pCur) + WINFO_OFF_AMMO_POOL_TYPE);

	if (nAltPool < 0)
	{
		*ppInfo = pBase;
	}
	else
	{
		*ppInfo = GetOrCreateClone(pBase, nAltPool);
	}

	const int nNewPool = *reinterpret_cast<const int32_t*>(
		reinterpret_cast<const char*>(*ppInfo) + WINFO_OFF_AMMO_POOL_TYPE);

	if (bridge_weapon_alt_ammo_pool_log.GetBool())
	{
		if (nOldPool != nNewPool)
		{
			Msg(eDLL_T::SERVER,
				"[ALT-AMMO] %s modBits=0x%08X pool %d('%s') -> %d('%s')\n",
				pCfg->m_szWeaponName.c_str(), modBits,
				nOldPool, AmmoPoolTypeName(nOldPool),
				nNewPool, AmmoPoolTypeName(nNewPool));
		}

		// Active mod name set on bit change (CAR ammo_type_swap sticky-mod probe).
		auto lastIt = s_lastModBits.find(pWeaponX);
		const bool bitsChanged = (lastIt == s_lastModBits.end()) || (lastIt->second != modBits);
		if (bitsChanged)
		{
			s_lastModBits[pWeaponX] = modBits;

			// Build "name,name,..." for set bits that we know names for.
			char szMods[512];
			szMods[0] = '\0';
			size_t used = 0;
			for (int i = 0; i < pCfg->m_nParsedModCount && i < 32; ++i)
			{
				if (!(modBits & (1u << i)))
					continue;
				const char* nm = (i < static_cast<int>(pCfg->m_modNames.size()))
					? pCfg->m_modNames[i].c_str()
					: "?";
				const int n = _snprintf_s(szMods + used, sizeof(szMods) - used, _TRUNCATE,
					"%s%s", used ? "," : "", nm);
				if (n > 0)
					used += static_cast<size_t>(n);
			}

			Msg(eDLL_T::SERVER,
				"[ALT-AMMO-MODS] %s modBits=0x%08X active={%s}\n",
				pCfg->m_szWeaponName.c_str(), modBits, szMods);
		}
	}
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
static void* __fastcall Hook_CWeaponX_PrecacheWeaponInfo(void* pWeaponX)
{
	void* const result = v_CWeaponX_PrecacheWeaponInfo(pWeaponX);
	WeaponAmmoPoolMod_Fixup(pWeaponX);
	return result;
}

static int64_t __fastcall Hook_CWeaponX_RecalcModdedSettings(void* pWeaponX)
{
	const int64_t result = v_CWeaponX_RecalcModdedSettings(pWeaponX);
	WeaponAmmoPoolMod_Fixup(pWeaponX);
	return result;
}

//-----------------------------------------------------------------------------
// IDetour
//-----------------------------------------------------------------------------
void VWeaponAmmoPoolMod::GetAdr(void) const
{
	LogFunAdr("CWeaponX::PrecacheWeaponInfo", v_CWeaponX_PrecacheWeaponInfo);
	LogFunAdr("CWeaponX::RecalculateModdedSettings", v_CWeaponX_RecalcModdedSettings);
	LogVarAdr("g_pWeaponInfoRegistry", g_ppWeaponInfoRegistry);
	LogVarAdr("g_pAmmoPoolTypeCount", g_pAmmoPoolTypeCount);
	LogVarAdr("g_pAmmoPoolTypeNames", g_pAmmoPoolTypeNames);
}

void VWeaponAmmoPoolMod::GetFun(void) const
{
	// CWeaponX::PrecacheWeaponInfo -- binds weapon+0x15A8 at init. The two 0x15xx
	// displacements are what separate this from the client twin, whose body is
	// otherwise the same instruction sequence.
	Module_FindPattern(g_GameDll,
		"48 8B C4 57 48 83 EC 60 8B 91 90 15 00 00 48 8B F9 81 FA FF FF 00 00 "
		"0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??")
		.GetPtr(v_CWeaponX_PrecacheWeaponInfo);
	if (!v_CWeaponX_PrecacheWeaponInfo)
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] CWeaponX::PrecacheWeaponInfo pattern unresolved -- feature disabled\n");

	// CWeaponX::RecalculateModdedSettings -- runs on every mod change.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 30 48 8B F9 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
		"48 8B 97 A8 15 00 00 8B 8F 7C 15 00 00")
		.GetPtr(v_CWeaponX_RecalcModdedSettings);
	if (!v_CWeaponX_RecalcModdedSettings)
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] CWeaponX::RecalculateModdedSettings pattern unresolved -- feature disabled\n");

	s_bPatternsOk = (v_CWeaponX_PrecacheWeaponInfo != nullptr &&
		v_CWeaponX_RecalcModdedSettings != nullptr);
}

void VWeaponAmmoPoolMod::GetVar(void) const
{
	// g_pWeaponInfoRegistry: mov rax, cs:qword / mov rcx,[rax+rcx*8+10h] / mov [rdi+15A8h], rcx
	CMemory regHit = Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? 48 8B 4C C8 10 48 89 8F A8 15 00 00");
	if (regHit)
	{
		g_ppWeaponInfoRegistry = regHit.ResolveRelativeAddress(3, 7).RCast<void**>();
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] g_pWeaponInfoRegistry pattern unresolved\n");
	}

	// ammo-pool-type name registry: movzx r11d, count / lea r10, names
	CMemory namesHit = Module_FindPattern(g_GameDll,
		"44 0F B7 1D ?? ?? ?? ?? 48 8B D8 45 8B CC 45 85 DB 74 ?? 4C 8D 15 ?? ?? ?? ??");
	if (namesHit)
	{
		g_pAmmoPoolTypeCount = namesHit.ResolveRelativeAddress(4, 8).RCast<const uint16_t*>();
		g_pAmmoPoolTypeNames = namesHit.Offset(0x13).ResolveRelativeAddress(3, 7).RCast<const char**>();
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[ALT-AMMO] ammo_pool_type name registry pattern unresolved -- name->index will fail\n");
	}
}

void VWeaponAmmoPoolMod::Detour(const bool bAttach) const
{
	if (v_CWeaponX_PrecacheWeaponInfo)
		DetourSetup(&v_CWeaponX_PrecacheWeaponInfo, &Hook_CWeaponX_PrecacheWeaponInfo, bAttach);
	if (v_CWeaponX_RecalcModdedSettings)
		DetourSetup(&v_CWeaponX_RecalcModdedSettings, &Hook_CWeaponX_RecalcModdedSettings, bAttach);

	if (bAttach && bridge_weapon_alt_ammo_pool_log.GetBool())
		Msg(eDLL_T::SERVER,
			"[ALT-AMMO] Detour attach: Precache=%p Recalc=%p registry=%p poolNames=%p\n",
			reinterpret_cast<void*>(v_CWeaponX_PrecacheWeaponInfo),
			reinterpret_cast<void*>(v_CWeaponX_RecalcModdedSettings),
			reinterpret_cast<void*>(g_ppWeaponInfoRegistry),
			reinterpret_cast<void*>(const_cast<const char**>(g_pAmmoPoolTypeNames)));
}

void WeaponAmmoPoolMod_LevelShutdown(void)
{
	static volatile LONG s_nFlushLog = 0;

	size_t nFreed = 0;
	for (AmmoPoolInfoClone_t& c : s_clones)
	{
		if (!c.m_pClone)
			continue;
		free(c.m_pClone);
		c.m_pClone = nullptr;
		++nFreed;
	}

	const size_t nConfigs = s_baseConfigs.size();
	const size_t nBits = s_lastModBits.size();
	s_clones.clear();
	s_baseConfigs.clear();
	s_lastModBits.clear();

	if (InterlockedIncrement(&s_nFlushLog) <= 8)
		Warning(eDLL_T::SERVER, "[ALT-AMMO] level shutdown flush: clones=%llu configs=%llu modbits=%llu\n",
			static_cast<unsigned long long>(nFreed),
			static_cast<unsigned long long>(nConfigs),
			static_cast<unsigned long long>(nBits));
}

