//=============================================================================
//
// Purpose: baked S21 traversal-camera curves. M = SA * P(cycle) * inv(SA).
// File: platform/cfg/mantle_boost_curves.txt. 101 samples, cycle 0..1 inclusive.
//
//=============================================================================
#ifndef MANTLE_BOOST_CURVES_H
#define MANTLE_BOOST_CURVES_H

#include "mathlib/mathlib.h"
#include <cstdio>
#include <cmath>

constexpr int MB_CURVE_TRAVERSAL_COUNT = 13;
constexpr int MB_CURVE_SAMPLES         = 101;   // cycle step 0.01, endpoints inclusive
constexpr int MB_CURVE_FILE_VERSION    = 1;

struct MBCurveSet_t
{
	bool  m_bLoadAttempted;
	bool  m_bLoaded[MB_CURVE_TRAVERSAL_COUNT];
	float m_P[MB_CURVE_TRAVERSAL_COUNT][MB_CURVE_SAMPLES][9];
};

inline MBCurveSet_t& MBCurves(void)
{
	static MBCurveSet_t s = {};
	return s;
}

inline const char* MBCurves_FilePath(void)
{
	return "platform/cfg/mantle_boost_curves.txt";
}

inline void MBCurves_Load(void)
{
	MBCurveSet_t& set = MBCurves();
	if (set.m_bLoadAttempted)
		return;
	set.m_bLoadAttempted = true;

	FILE* const f = fopen(MBCurves_FilePath(), "r");
	if (!f)
		return;

	int nVersion = 0;
	if (fscanf(f, "mb_curves %d", &nVersion) != 1 || nVersion != MB_CURVE_FILE_VERSION)
	{
		fclose(f);
		return;
	}

	int nTravState = 0;
	while (fscanf(f, " travstate %d", &nTravState) == 1)
	{
		const bool bValid = nTravState >= 0 && nTravState < MB_CURVE_TRAVERSAL_COUNT;
		float row[9];
		bool bRowsOk = true;
		for (int i = 0; i < MB_CURVE_SAMPLES && bRowsOk; ++i)
		{
			for (int j = 0; j < 9; ++j)
			{
				if (fscanf(f, "%f", &row[j]) != 1 || !isfinite(row[j]))
				{
					bRowsOk = false;
					break;
				}
			}
			if (bRowsOk && bValid)
			{
				for (int j = 0; j < 9; ++j)
					set.m_P[nTravState][i][j] = row[j];
			}
		}
		if (!bRowsOk)
			break;
		if (bValid)
			set.m_bLoaded[nTravState] = true;
	}
	fclose(f);
}

inline bool MBCurves_Save(void)
{
	const MBCurveSet_t& set = MBCurves();
	FILE* const f = fopen(MBCurves_FilePath(), "w");
	if (!f)
		return false;

	fprintf(f, "mb_curves %d\n", MB_CURVE_FILE_VERSION);
	for (int t = 0; t < MB_CURVE_TRAVERSAL_COUNT; ++t)
	{
		if (!set.m_bLoaded[t])
			continue;
		fprintf(f, "travstate %d\n", t);
		for (int i = 0; i < MB_CURVE_SAMPLES; ++i)
		{
			const float* const p = set.m_P[t][i];
			fprintf(f, "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
		}
	}
	fclose(f);
	return true;
}

inline bool MBCurves_Has(int nTravState)
{
	MBCurves_Load();
	return nTravState >= 0 && nTravState < MB_CURVE_TRAVERSAL_COUNT
		&& MBCurves().m_bLoaded[nTravState];
}

// P(cycle) into a rotation-only matrix3x4_t, component-lerped between samples.
inline void MBCurves_SampleP(int nTravState, float flCycle, matrix3x4_t& out)
{
	if (flCycle < 0.0f) flCycle = 0.0f;
	if (flCycle > 1.0f) flCycle = 1.0f;

	const float flPos = flCycle * float(MB_CURVE_SAMPLES - 1);
	int i0 = static_cast<int>(flPos);
	if (i0 > MB_CURVE_SAMPLES - 2)
		i0 = MB_CURVE_SAMPLES - 2;
	const float t = flPos - float(i0);

	const float* const a = MBCurves().m_P[nTravState][i0];
	const float* const b = MBCurves().m_P[nTravState][i0 + 1];
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c < 3; ++c)
		{
			const int j = r * 3 + c;
			out[r][c] = a[j] + (b[j] - a[j]) * t;
		}
		out[r][3] = 0.0f;
	}
}

// delta = MatrixAngles(SA * P(cycle) * SA^T * eyeM).x - eye.x.
inline bool MBCurves_Eval(int nTravState, float flCycle, const Vector3D& vecFwd,
	const QAngle& eyeAngles, float* const pflDelta)
{
	if (!MBCurves_Has(nTravState))
		return false;
	if (vecFwd.LengthSqr() < 1e-6f)
		return false;

	matrix3x4_t P;
	MBCurves_SampleP(nTravState, flCycle, P);

	matrix3x4_t sa;
	VectorMatrix(vecFwd, sa);
	MatrixSetColumn(Vector3D(0.0f, 0.0f, 0.0f), 3, sa);
	matrix3x4_t saT;
	MatrixInvert(sa, saT);   // orthonormal transpose-invert

	matrix3x4_t eyeM;
	AngleMatrix(eyeAngles, eyeM);

	matrix3x4_t a, b, c;
	ConcatTransforms(saT, eyeM, a);   // SA^T * eye
	ConcatTransforms(P, a, b);        // P * SA^T * eye
	ConcatTransforms(sa, b, c);       // SA * P * SA^T * eye

	QAngle outAngles;
	MatrixAngles(c, outAngles);

	const float flDelta = outAngles.x - eyeAngles.x;
	if (!isfinite(flDelta))
		return false;

	*pflDelta = flDelta;
	return true;
}

// Threshold is animation-specific; 1.5 deg does not transfer. Derive from the curve
// so both engines share the same window. min_valid_traversal_frac stays the gate.
inline bool MBCurves_AutoThreshold(int nTravState, float flMinFrac,
	const Vector3D& vecFwd, const QAngle& eyeAngles, float* const pflThreshold)
{
	if (!MBCurves_Has(nTravState) || !pflThreshold)
		return false;
	if (flMinFrac < 0.0f) flMinFrac = 0.0f;
	if (flMinFrac > 1.0f) flMinFrac = 1.0f;

	float flMax = 0.0f;
	for (int i = 0; i < MB_CURVE_SAMPLES; ++i)
	{
		const float flCycle = float(i) / float(MB_CURVE_SAMPLES - 1);
		if (flCycle < flMinFrac)
			continue;

		float flDelta = 0.0f;
		if (!MBCurves_Eval(nTravState, flCycle, vecFwd, eyeAngles, &flDelta))
			return false;

		const float flAbs = fabsf(flDelta);
		if (flAbs > flMax)
			flMax = flAbs;
	}

	// Margin covers the gap between the sampled grid and the cycle the press
	// actually lands on; without it a press between two samples can exceed the
	// max taken across them.
	*pflThreshold = flMax * 1.02f;
	return true;
}

// Bake-side store: the dumper writes P rows here, then marks the state baked
// and rewrites the file. Load-merge happens first so earlier bakes survive.
inline void MBCurves_Store(int nTravState, const float (*pRows)[9])
{
	MBCurves_Load();
	MBCurveSet_t& set = MBCurves();
	if (nTravState < 0 || nTravState >= MB_CURVE_TRAVERSAL_COUNT)
		return;
	for (int i = 0; i < MB_CURVE_SAMPLES; ++i)
	{
		for (int j = 0; j < 9; ++j)
			set.m_P[nTravState][i][j] = pRows[i][j];
	}
	set.m_bLoaded[nTravState] = true;
}

// Dev reload (dedi iterates on a fresh bake without a process restart).
inline void MBCurves_Reload(void)
{
	MBCurveSet_t& set = MBCurves();
	set.m_bLoadAttempted = false;
	for (int t = 0; t < MB_CURVE_TRAVERSAL_COUNT; ++t)
		set.m_bLoaded[t] = false;
	MBCurves_Load();
}

#endif // MANTLE_BOOST_CURVES_H
