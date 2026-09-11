//=============================================================================//
//
// Purpose: S3->S21 activity ID translation map builder + translator.
// See activity_s3_to_s21.h for context. Map is built once at dedi
// host-state init after the activity loader finishes.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/convar.h"
#include "tier1/utlstring.h"
#include "activity.h"
#include "activity_s3_to_s21.h"
#include "activity_s21_enum.h"
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

// Map sized for the S3 engine table plus the script-added activities loaded by
// LoadCustomActivitiesFromFile (activity_types.txt now restores the 92 S21 names
// the S3 engine lacks, so the dedi table runs ~1024). 2048 keeps headroom.
static constexpr int kS3MapSize = 2048;
static int  s_s3ToS21[kS3MapSize];
static int  s_s3MaxIdAtBuild = -1;
static bool s_ready = false;
static int  s_mappedCount = 0;
static int  s_unmappedCount = 0;

// [WEAP-ACT-C2S] Reverse map, S21->S3. Sized to the embedded S21 enum table
// (kS21ActivityCount, currently 975) with headroom; built as a companion pass
// in the same Bridge_BuildS3ToS21ActivityMap walk below.
static constexpr int kS21MapSize = 2048;
static int  s_s21ToS3[kS21MapSize];

// Activity ids are 1-based; 0 cannot resolve to a real animation and fits the
// bounded-int SendProps (-1 widens to 0xFFFFFFFF).
static constexpr int kActivityUnmapped = 0;

// Default ON. Unmapped names emit 0; tables are dense so a raw id is a wrong
// real activity. 0 plays nothing instead.
static ConVar bridge_act_xlat_unmapped("bridge_act_xlat_unmapped", "1", FCVAR_RELEASE,
	"What to send when an activity has no counterpart on the other engine. "
	"1 = unbound sentinel (plays nothing). 0 = legacy raw pass-through (the id "
	"resolves to a different, valid activity on the far side).");

//-----------------------------------------------------------------------------
// Purpose: id<=0 is already "no activity" -- pass through, do not rewrite.
//-----------------------------------------------------------------------------
static inline int Activity_Unmapped(const int id)
{
	if (id <= 0)
		return id;

	return bridge_act_xlat_unmapped.GetBool() ? kActivityUnmapped : id;
}

int Bridge_TranslateS3ActivityToS21(int s3_id)
{
	if (!s_ready) return Activity_Unmapped(s3_id);
	if (s3_id <= 0 || s3_id >= kS3MapSize) return s3_id;
	const int s21 = s_s3ToS21[s3_id];
	return (s21 >= 0) ? s21 : Activity_Unmapped(s3_id);
}

int Bridge_TranslateS21ActivityToS3(int s21_id)
{
	if (!s_ready) return Activity_Unmapped(s21_id);
	if (s21_id <= 0 || s21_id >= kS21MapSize) return s21_id;
	const int s3 = s_s21ToS3[s21_id];
	return (s3 >= 0) ? s3 : Activity_Unmapped(s21_id);
}

void Bridge_BuildS3ToS21ActivityMap()
{
	if (s_ready) return;                        // idempotent

	if (!v_ActivityList_GetActivityName_Server || !g_pMaxActivityId_Server || *g_pMaxActivityId_Server < 100)
	{
		// Called every host frame until the server-side activity table is populated.
		// Rate-limit the warning -- once after the first ~5s of retries, then every
		// few thousand frames (== minutes). Avoids log spam if init never completes.
		static int s_retries = 0;
		++s_retries;
		if (s_retries == 300 || (s_retries % 9000) == 0)
		{
			Warning(eDLL_T::ENGINE,
				"[ACT-XLAT] server activity table still not ready after %d frames "
				"(GetActivityName_Server=%p, g_pMaxActivityId_Server=%p, *maxId=%d)\n",
				s_retries,
				(void*)v_ActivityList_GetActivityName_Server,
				(void*)g_pMaxActivityId_Server,
				g_pMaxActivityId_Server ? *g_pMaxActivityId_Server : -1);
		}
		return;
	}

	for (int i = 0; i < kS3MapSize; ++i)
		s_s3ToS21[i] = -1;
	for (int i = 0; i < kS21MapSize; ++i)
		s_s21ToS3[i] = -1;

	// Duplicate names would make emplace keep the first id and mis-map later use.
	std::unordered_map<std::string, int> s21ByName;
	s21ByName.reserve(kS21ActivityCount * 2);
	int dupNames = 0;
	for (int j = 0; j < kS21ActivityCount; ++j)
	{
		if (!s21ByName.emplace(kS21Activities[j].name, kS21Activities[j].id).second)
		{
			++dupNames;
			Warning(eDLL_T::ENGINE,
				"[ACT-XLAT] embedded S21 table has duplicate name '%s' (id %d kept, %d dropped)\n",
				kS21Activities[j].name, s21ByName[kS21Activities[j].name], kS21Activities[j].id);
		}
	}

	const int s3Max = *g_pMaxActivityId_Server;
	int mapped = 0, unmapped = 0, invalid = 0, revCollisions = 0;
	const int walkMax = (s3Max < kS3MapSize - 1) ? s3Max : (kS3MapSize - 1);

	// Cap the per-name spew; the whole point is that this list should be short.


	for (int s3 = 0; s3 <= walkMax; ++s3)
	{
		const char* name = v_ActivityList_GetActivityName_Server(s3);
		if (!name || !name[0] || strcmp(name, "(invalid activity index)") == 0)
		{
			++invalid;
			continue;                            // gap slots stay -1
		}

		auto it = s21ByName.find(name);
		if (it == s21ByName.end())
		{
			// No S21 counterpart -- leave -1 so the unmapped policy applies.
			++unmapped;
			continue;
		}

		const int s21 = it->second;
		s_s3ToS21[s3] = s21;
		++mapped;

		if (s21 >= 0 && s21 < kS21MapSize)
		{
			// Reverse entry. Two S3 ids resolving to one S21 id would make the
			// C2S direction arbitrary (last write wins), so keep the first and
			// report rather than silently overwrite.
			if (s_s21ToS3[s21] < 0)
				s_s21ToS3[s21] = s3;
			else
			{
				++revCollisions;
				Warning(eDLL_T::ENGINE,
					"[ACT-XLAT] reverse collision on S21 id %d ('%s'): keeping S3 %d, ignoring S3 %d\n",
					s21, name, s_s21ToS3[s21], s3);
			}
		}
	}

	// C2S coverage: S21 activities with no S3 source. The build reports both
	// directions so a reverse hole shows up in the boot log even while nothing
	// consumes it yet.
	int revMissing = 0;
	for (int j = 0; j < kS21ActivityCount; ++j)
	{
		const int id = kS21Activities[j].id;
		if (id > 0 && id < kS21MapSize && s_s21ToS3[id] < 0)
			++revMissing;
	}

	s_s3MaxIdAtBuild = s3Max;
	s_mappedCount = mapped;
	s_unmappedCount = unmapped;
	s_ready = true;

	Msg(eDLL_T::ENGINE,
		"[ACT-XLAT] S3->S21 activity map built: %d mapped, %d unmapped, %d invalid-slot "
		"(S3 max=%d, S21 table=%d entries)\n",
		mapped, unmapped, invalid, s3Max, kS21ActivityCount);
	Msg(eDLL_T::ENGINE,
		"[ACT-XLAT] reverse S21->S3: %d S21 activities have no S3 source, %d collision(s); "
		"unmapped policy=%s\n",
		revMissing, revCollisions,
		bridge_act_xlat_unmapped.GetBool() ? "sentinel" : "raw pass-through");

	// A large reverse hole means activity_types.txt is missing S21 names again --
	// that file feeds the DEDI, so the test is whether the S3 engine has the
	// name, never whether the client does. See the header of activity_types.txt.
	if (revMissing > 0)
		Warning(eDLL_T::ENGINE,
			"[ACT-XLAT] %d S21 activities are unreachable on this dedi -- add them to "
			"platform/scripts/activity_types.txt (see internal notes)\n",
			revMissing);

	if (dupNames > 0)
		Warning(eDLL_T::ENGINE,
			"[ACT-XLAT] embedded S21 table had %d duplicate name(s) -- regenerate it\n", dupNames);
}
