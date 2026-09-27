//=============================================================================//
//
// Purpose: bounded JSON field access for demo META / EVENT payloads. Reads
//          top-level keys and one level of arrays of objects; never allocates
//          and never trusts a length it did not measure.
//
//=============================================================================//
#ifndef ENGINE_SHARED_DEMO_JSON_H
#define ENGINE_SHARED_DEMO_JSON_H

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace DemoJson
{
	inline size_t SkipWs(const char* s, size_t i, size_t n)
	{
		while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
			++i;
		return i;
	}

	// i points at an opening quote; returns the index past the closing quote, or n.
	inline size_t SkipString(const char* s, size_t i, size_t n)
	{
		++i;
		while (i < n)
		{
			if (s[i] == '\\')
				i += 2;
			else if (s[i] == '"')
				return i + 1;
			else
				++i;
		}
		return n;
	}

	// Skips one value of any type starting at i; returns the index past it.
	inline size_t SkipValue(const char* s, size_t i, size_t n)
	{
		i = SkipWs(s, i, n);
		if (i >= n)
			return n;
		if (s[i] == '"')
			return SkipString(s, i, n);
		if (s[i] == '{' || s[i] == '[')
		{
			int depth = 0;
			while (i < n)
			{
				const char c = s[i];
				if (c == '"')
				{
					i = SkipString(s, i, n);
					continue;
				}
				if (c == '{' || c == '[')
					++depth;
				else if (c == '}' || c == ']')
				{
					if (--depth == 0)
						return i + 1;
				}
				++i;
			}
			return n;
		}
		while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']')
			++i;
		return i;
	}

	// Locates the value of a top-level key inside the object spanning [0, n).
	inline bool FindValue(const char* s, size_t n, const char* key, size_t* pStart, size_t* pEnd)
	{
		if (!s || !key)
			return false;
		size_t i = SkipWs(s, 0, n);
		if (i >= n || s[i] != '{')
			return false;
		++i;

		const size_t keyLen = strlen(key);
		int nSafety = 0;
		while (i < n && ++nSafety < 4096)
		{
			i = SkipWs(s, i, n);
			if (i >= n || s[i] == '}')
				return false;
			if (s[i] != '"')
				return false;
			const size_t keyStart = i + 1;
			const size_t keyEnd = SkipString(s, i, n);
			if (keyEnd >= n)
				return false;
			const bool bMatch = (keyEnd - 1 - keyStart == keyLen) && !memcmp(s + keyStart, key, keyLen);
			i = SkipWs(s, keyEnd, n);
			if (i >= n || s[i] != ':')
				return false;
			i = SkipWs(s, i + 1, n);
			const size_t valEnd = SkipValue(s, i, n);
			if (bMatch)
			{
				*pStart = i;
				*pEnd = valEnd;
				return true;
			}
			i = SkipWs(s, valEnd, n);
			if (i < n && s[i] == ',')
				++i;
		}
		return false;
	}

	inline bool GetString(const char* s, size_t n, const char* key, char* out, size_t outLen)
	{
		if (!out || !outLen)
			return false;
		out[0] = '\0';
		size_t a, b;
		if (!FindValue(s, n, key, &a, &b) || a >= n || s[a] != '"')
			return false;
		size_t o = 0;
		for (size_t i = a + 1; i < b - 1 && o + 1 < outLen; ++i)
		{
			char c = s[i];
			if (c == '\\' && i + 1 < b - 1)
			{
				const char e = s[++i];
				if (e == 'u')
				{
					i += 4;
					c = '?';
				}
				else
					c = (e == 'n' || e == 't' || e == 'r') ? ' ' : e;
			}
			if (static_cast<unsigned char>(c) < 0x20)
				c = ' ';
			out[o++] = c;
		}
		out[o] = '\0';
		return true;
	}

	inline bool GetNumber(const char* s, size_t n, const char* key, double* pOut)
	{
		size_t a, b;
		if (!FindValue(s, n, key, &a, &b) || a >= n)
			return false;

		char tmp[64];
		size_t len = b - a;
		size_t off = 0;
		if (s[a] == '"')
		{
			off = 1;
			len = (len >= 2) ? len - 2 : 0;
		}
		if (len == 0 || len >= sizeof(tmp))
			return false;
		memcpy(tmp, s + a + off, len);
		tmp[len] = '\0';
		char* pEnd = nullptr;
		const double v = strtod(tmp, &pEnd);
		if (pEnd == tmp)
			return false;
		*pOut = v;
		return true;
	}

	inline bool GetObj(const char* s, size_t n, const char* key, size_t* pStart, size_t* pLen)
	{
		size_t a, b;
		if (!FindValue(s, n, key, &a, &b) || a >= n || s[a] != '{')
			return false;
		*pStart = a;
		*pLen = b - a;
		return true;
	}

	// Calls fn(objStart, objLen) for every object element of a top-level array.
	template <typename Fn>
	inline int ForEachObject(const char* s, size_t n, const char* key, Fn fn, int nMax)
	{
		size_t a, b;
		if (!FindValue(s, n, key, &a, &b) || a >= n || s[a] != '[')
			return 0;
		int count = 0;
		size_t i = a + 1;
		while (i < b && count < nMax)
		{
			i = SkipWs(s, i, b);
			if (i >= b || s[i] == ']')
				break;
			const size_t end = SkipValue(s, i, b);
			if (s[i] == '{')
			{
				fn(s + i, end - i);
				++count;
			}
			i = SkipWs(s, end, b);
			if (i < b && s[i] == ',')
				++i;
			else
				break;
		}
		return count;
	}

	// Printable-ASCII JSON string body; everything else becomes '?'.
	inline void Escape(const char* in, char* out, size_t outLen, size_t maxChars = 64)
	{
		if (!out || !outLen)
			return;
		size_t o = 0;
		for (size_t i = 0; in && in[i] && i < maxChars && o + 3 < outLen; ++i)
		{
			const unsigned char c = static_cast<unsigned char>(in[i]);
			if (c == '"' || c == '\\')
			{
				out[o++] = '\\';
				out[o++] = static_cast<char>(c);
			}
			else if (c < 0x20 || c > 0x7E)
				out[o++] = '?';
			else
				out[o++] = static_cast<char>(c);
		}
		out[o] = '\0';
	}
}

#endif // ENGINE_SHARED_DEMO_JSON_H
