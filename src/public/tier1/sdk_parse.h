//=============================================================================//
//
// Purpose: non-throwing string parse helpers for SDK config loaders.
//
//=============================================================================//
#ifndef TIER1_SDK_PARSE_H
#define TIER1_SDK_PARSE_H

#include <cmath>
#include <cstdlib>
#include <string>

// strtof / strtol do not throw; reject when no conversion digits were consumed.
// NaN / Inf are not a number the caller asked for -- a weapon KV of "nan"
// would otherwise disable every finite compare that consumes the value.
inline float Sdk_ParseFloat(const std::string& s, const float fallback = 0.0f)
{
	char* end = nullptr;
	const char* const begin = s.c_str();
	const float v = strtof(begin, &end);
	if (end == begin || !std::isfinite(v))
		return fallback;
	return v;
}

inline int Sdk_ParseInt(const std::string& s, const int fallback = 0)
{
	char* end = nullptr;
	const char* const begin = s.c_str();
	const long v = strtol(begin, &end, 10);
	if (end == begin)
		return fallback;
	return static_cast<int>(v);
}

inline void Sdk_TrimWhitespace(std::string& s)
{
	const size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
	{
		s.clear();
		return;
	}
	const size_t end = s.find_last_not_of(" \t\r\n");
	s = s.substr(start, end - start + 1);
}

// Pull the next "quoted" token from line starting at pos; advance pos past it.
inline bool ParseQuotedString(const std::string& line, size_t& pos, std::string& out)
{
	const size_t q1 = line.find('"', pos);
	if (q1 == std::string::npos)
		return false;
	const size_t q2 = line.find('"', q1 + 1);
	if (q2 == std::string::npos)
		return false;
	out = line.substr(q1 + 1, q2 - q1 - 1);
	pos = q2 + 1;
	return true;
}

#endif // TIER1_SDK_PARSE_H
