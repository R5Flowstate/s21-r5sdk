//=============================================================================//
//
// Purpose: client S3->S21 activity translator. Binary-search over the static table.
//
//=============================================================================//
#include "core/stdafx.h"
#include "activity_s3_to_s21_client.h"
#include "activity_s3_to_s21_table.h"

int Bridge_TranslateS3ActivityToS21_Static(int s3_id)
{
	// Binary search the sorted table; identity if not found
	int lo = 0, hi = kActS3ToS21TableCount - 1;
	while (lo <= hi)
	{
		const int mid = lo + (hi - lo) / 2;
		const int mid_s3 = kActS3ToS21Table[mid].s3;
		if (mid_s3 == s3_id) return kActS3ToS21Table[mid].s21;
		if (mid_s3 < s3_id)  lo = mid + 1;
		else                 hi = mid - 1;
	}
	return s3_id;
}
