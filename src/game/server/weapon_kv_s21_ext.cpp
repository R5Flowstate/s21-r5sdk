//=============================================================================//
//
// Purpose: weapon_kv_s21_ext.h implementation. See that header.
//
//=============================================================================//
#include "core/stdafx.h"


#include "weapon_kv_s21_ext.h"
#include "tier1/keyvalues.h"
#include "filesystem/filesystem.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

static ConVar bridge_weapon_kv_s21ext("bridge_weapon_kv_s21ext", "1", FCVAR_RELEASE,
	"Read S21-only weapon KV keys from scripts/weapons/<name>.txt. 0 = all defaults.");

static std::mutex s_extMutex;
static std::unordered_map<std::string, WeaponKVS21Ext_t> s_extCache;

static bool KvValueStartsWithStar(KeyValues* pKey)
{
	if (!pKey)
		return false;

	const char* const psz = pKey->GetString();
	return psz && psz[0] == '*';
}

static bool TryReadFloatKey(KeyValues* pBlock, const char* pszKey, float* pOut)
{
	KeyValues* const pKey = pBlock->FindKey(pszKey, false);
	if (!pKey || KvValueStartsWithStar(pKey))
		return false;

	const char* const psz = pKey->GetString();
	if (!psz || !psz[0])
		return false;

	char* pszEnd = nullptr;
	const float fl = strtof(psz, &pszEnd);
	if (pszEnd == psz)
		return false;

	*pOut = fl;
	return true;
}

static bool TryReadBoolKey(KeyValues* pBlock, const char* pszKey, bool* pOut)
{
	KeyValues* const pKey = pBlock->FindKey(pszKey, false);
	if (!pKey || KvValueStartsWithStar(pKey))
		return false;

	*pOut = pKey->GetInt(nullptr, 0) != 0;
	return true;
}

static KeyValues* FindWeaponDataBlock(KeyValues* pKv)
{
	if (!pKv)
		return nullptr;

	const char* const pszName = pKv->GetName();
	if (pszName && _stricmp(pszName, "WeaponData") == 0)
		return pKv;

	return pKv->FindKey("WeaponData", false);
}

static bool WeaponNameIsSafe(const char* pszWeaponName)
{
	if (!pszWeaponName || !pszWeaponName[0])
		return false;

	if (V_strlen(pszWeaponName) > 64)
		return false;

	if (V_strstr(pszWeaponName, "..")
		|| strchr(pszWeaponName, '/')
		|| strchr(pszWeaponName, '\\'))
	{
		return false;
	}

	return true;
}

static void NoteFoundKey(char* pszFound, int nFoundMax, int* pFoundCount, const char* pszKey)
{
	if (pszFound[0])
		V_strncat(pszFound, " ", nFoundMax);
	V_strncat(pszFound, pszKey, nFoundMax);
	++(*pFoundCount);
}

static WeaponKVS21Ext_t ParseWeaponKVS21Ext(const char* pszWeaponName)
{
	WeaponKVS21Ext_t ext;

	if (!FileSystem())
		return ext;

	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "scripts/weapons/%s.txt", pszWeaponName);

	KeyValues* const pKv = new KeyValues("WeaponData");
	if (!pKv->LoadFromFile(FileSystem(), szPath, "GAME"))
	{
		pKv->DeleteThis();
		return ext;
	}

	KeyValues* const pData = FindWeaponDataBlock(pKv);
	if (!pData)
	{
		pKv->DeleteThis();
		return ext;
	}

	char szFound[512];
	szFound[0] = '\0';
	int nFound = 0;

	if (TryReadFloatKey(pData, "projectile_inherit_base_velocity_scale", &ext.flInheritBaseVelocityScale))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_inherit_base_velocity_scale");
	if (TryReadFloatKey(pData, "projectile_air_friction", &ext.flAirFriction))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_air_friction");
	if (TryReadFloatKey(pData, "projectile_air_friction_2", &ext.flAirFriction2))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_air_friction_2");
	if (TryReadFloatKey(pData, "projectile_air_friction_final", &ext.flAirFrictionFinal))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_air_friction_final");
	if (TryReadFloatKey(pData, "projectile_gravity_scale_2", &ext.flGravityScale2))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_gravity_scale_2");
	if (TryReadFloatKey(pData, "projectile_gravity_scale_time_2", &ext.flGravityScaleTime2))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_gravity_scale_time_2");
	if (TryReadFloatKey(pData, "projectile_gravity_scale_final", &ext.flGravityScaleFinal))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_gravity_scale_final");
	if (TryReadFloatKey(pData, "projectile_gravity_scale_time_final", &ext.flGravityScaleTimeFinal))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "projectile_gravity_scale_time_final");
	if (TryReadFloatKey(pData, "spread_min_kick", &ext.flSpreadMinKick))
	{
		ext.bHasSpreadMinKick = true;
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "spread_min_kick");
	}
	if (TryReadBoolKey(pData, "spread_update_hipfire_in_ads", &ext.bSpreadUpdateHipfireInAds))
		NoteFoundKey(szFound, sizeof(szFound), &nFound, "spread_update_hipfire_in_ads");

	pKv->DeleteThis();

	if (nFound > 0)
	{
		ext.bLoaded = true;
		Msg(eDLL_T::SERVER, "[WPNKV-S21] '%s' %s\n", pszWeaponName, szFound);
	}

	return ext;
}

const WeaponKVS21Ext_t& WeaponKVS21Ext_Get(const char* pszWeaponName)
{
	static const WeaponKVS21Ext_t s_defaults;

	if (!bridge_weapon_kv_s21ext.GetBool())
		return s_defaults;

	if (!WeaponNameIsSafe(pszWeaponName))
		return s_defaults;

	std::lock_guard<std::mutex> lock(s_extMutex);

	const auto it = s_extCache.find(pszWeaponName);
	if (it != s_extCache.end())
		return it->second;

	WeaponKVS21Ext_t ext = ParseWeaponKVS21Ext(pszWeaponName);
	const auto inserted = s_extCache.emplace(pszWeaponName, ext);
	return inserted.first->second;
}

void WeaponKVS21Ext_LevelShutdown(void)
{
	std::lock_guard<std::mutex> lock(s_extMutex);
	s_extCache.clear();
}

