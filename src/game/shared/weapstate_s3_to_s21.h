//=============================================================================
//
// Purpose: dedi -> S21 m_weapState ordinals (insert ENERGIZE=8 and COOLDOWN_OVERHEAT=22)
//          and m_customActivityFlags bits.
// No reverse map: a client rewrite masks or corrupts the prediction compare.
//
//=============================================================================
#ifndef WEAPSTATE_S3_TO_S21_H
#define WEAPSTATE_S3_TO_S21_H

// Max output is 23, so the dedi DT_WeaponX m_weapState SendProp (offset 0x1234,
// nBits 5 unsigned) carries the translated range with no widening.
inline int WeapState_S3ToS21(const int s3)
{
	if (s3 < 0 || s3 > 21)
		return s3;                          // outside dedi's range: pass through untouched

	return s3 + (s3 >= 8) + (s3 >= 21);
}

// WCAF_* custom-activity flags. S21 inserted ALLOW_START_WHILE_SPRINTING and
// ALLOW_JUMP_LAND and moved PLAYRAISEONCOMPLETE to the top bit:
//   flag                       dedi    S21
//   ISINTERRUPTIBLE            0x01  0x01
//   PLAYRAISEONCOMPLETE        0x02  0x80
//   DISABLEWEAPON              0x04  0x02
//   ALLOW_WHILE_SPRINTING      0x08  0x04
//   TOGGLE                     0x10  0x20
//   KEEPMELEESTATE             0x20  0x40
// Covers the engine's own starts; script starts keep the S21 flags they
// passed (WeaponCustomAct_WireFlags), including the S21-only bits.
inline int WeapCustomActFlags_S3ToS21(const int s3)
{
	int s21 = s3 & 0x01;
	if (s3 & 0x02) s21 |= 0x80;
	if (s3 & 0x04) s21 |= 0x02;
	if (s3 & 0x08) s21 |= 0x04;
	if (s3 & 0x10) s21 |= 0x20;
	if (s3 & 0x20) s21 |= 0x40;
	return s21;
}

#endif // WEAPSTATE_S3_TO_S21_H
