#pragma once
//=============================================================================//
//
// Purpose: Challenge-bound connect password wire tag (HMAC-SHA256).
// Client and dedi must ship this header in lockstep.
//
//=============================================================================//

#include "mbedtls/include/mbedtls/sha256.h"

#include <cstdint>
#include <cstring>

inline bool ConnectPw_DeriveKey(const char* const pszPassword, unsigned char key[32])
{
	if (!key || !pszPassword || !pszPassword[0])
		return false;

	size_t n = 0;
	while (pszPassword[n] && n < 256)
		++n;
	if (!n)
		return false;

	return mbedtls_sha256(reinterpret_cast<const unsigned char*>(pszPassword), n, key, 0) == 0;
}

inline bool ConnectPw_HmacSha256(const unsigned char* const key, const size_t keyLen,
	const unsigned char* const msg, const size_t msgLen, unsigned char out[32])
{
	if (!key || !out || (!msg && msgLen))
		return false;

	unsigned char keyUse[64];
	memset(keyUse, 0, sizeof(keyUse));

	if (keyLen > 64)
	{
		if (mbedtls_sha256(key, keyLen, keyUse, 0) != 0)
			return false;
	}
	else
	{
		memcpy(keyUse, key, keyLen);
	}

	unsigned char ipad[64];
	unsigned char opad[64];
	for (int i = 0; i < 64; ++i)
	{
		ipad[i] = static_cast<unsigned char>(keyUse[i] ^ 0x36);
		opad[i] = static_cast<unsigned char>(keyUse[i] ^ 0x5c);
	}

	mbedtls_sha256_context ctx;
	unsigned char inner[32];

	mbedtls_sha256_init(&ctx);
	if (mbedtls_sha256_starts(&ctx, 0) != 0
		|| mbedtls_sha256_update(&ctx, ipad, 64) != 0
		|| mbedtls_sha256_update(&ctx, msg, msgLen) != 0
		|| mbedtls_sha256_finish(&ctx, inner) != 0)
	{
		mbedtls_sha256_free(&ctx);
		return false;
	}
	mbedtls_sha256_free(&ctx);

	mbedtls_sha256_init(&ctx);
	if (mbedtls_sha256_starts(&ctx, 0) != 0
		|| mbedtls_sha256_update(&ctx, opad, 64) != 0
		|| mbedtls_sha256_update(&ctx, inner, 32) != 0
		|| mbedtls_sha256_finish(&ctx, out) != 0)
	{
		mbedtls_sha256_free(&ctx);
		return false;
	}
	mbedtls_sha256_free(&ctx);
	return true;
}

inline bool ConnectPw_WireU64(const unsigned char key[32], const uint32_t nChallenge, uint64_t* const out)
{
	if (!key || !out)
		return false;

	unsigned char msg[4];
	memcpy(msg, &nChallenge, 4);

	unsigned char hmac[32];
	if (!ConnectPw_HmacSha256(key, 32, msg, 4, hmac))
		return false;

	uint64_t v = 0;
	memcpy(&v, hmac, 8);
	*out = v;
	return true;
}
