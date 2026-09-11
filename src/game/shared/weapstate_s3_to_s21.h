//=============================================================================
//
// Purpose: S3 -> S21 m_weapState ordinals (insert ENERGIZE=8 and COOLDOWN_OVERHEAT=22).
// No reverse map: a client rewrite masks or corrupts the prediction compare.
//
//=============================================================================
#ifndef WEAPSTATE_S3_TO_S21_H
#define WEAPSTATE_S3_TO_S21_H

// Max output is 23, so the S3 DT_WeaponX m_weapState SendProp (offset 0x1234,
// nBits 5 unsigned) carries the translated range with no widening.
inline int WeapState_S3ToS21(const int s3)
{
	if (s3 < 0 || s3 > 21)
		return s3;                          // outside S3's range: pass through untouched

	return s3 + (s3 >= 8) + (s3 >= 21);
}

#endif // WEAPSTATE_S3_TO_S21_H
