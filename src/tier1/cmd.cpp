//=============================================================================//
//
// Purpose: Console Commands
//
//=============================================================================//

#include "tier0/tslist.h"
#include "tier0/memstd.h"
#include "tier0/commandline.h"
#include "tier1/cmd.h"
#include "tier1/cvar.h"
#include "tier1/characterset.h"
#include "tier1/utlstring.h"
#include "tier1/utlbuffer.h"

//-----------------------------------------------------------------------------
// Global methods
//-----------------------------------------------------------------------------
static characterset_t s_BreakSet;
static bool s_bBuiltBreakSet = false;


//-----------------------------------------------------------------------------
// Tokenizer class
//-----------------------------------------------------------------------------
CCommand::CCommand()
{
	if (!s_bBuiltBreakSet)
	{
		s_bBuiltBreakSet = true;
		CharacterSetBuild(&s_BreakSet, "{}()':");
	}

	Reset();
}

//-----------------------------------------------------------------------------
// Purpose: constructor
// Input: nArgC - 
// **ppArgV - 
// source - 
//-----------------------------------------------------------------------------
CCommand::CCommand(int nArgC, const char** ppArgV, cmd_source_t source)
{
	if (!s_bBuiltBreakSet)
	{
		s_bBuiltBreakSet = true;
		CharacterSetBuild(&s_BreakSet, "{}()':");
	}

	Reset();
	m_nQueuedVal = source;

	if (nArgC <= 0 || !ppArgV)
		return;

	if (nArgC > COMMAND_MAX_ARGC)
	{
		Warning(eDLL_T::COMMON, "%s: Encountered command which overflows the argument buffer... Clamped!\n", __FUNCTION__);
		nArgC = COMMAND_MAX_ARGC;
	}

	char* pBuf = m_pArgvBuffer;
	char* pSBuf = m_pArgSBuffer;
	const char* const pBufEnd = m_pArgvBuffer + COMMAND_MAX_LENGTH;
	const char* const pSBufEnd = m_pArgSBuffer + COMMAND_MAX_LENGTH;

	for (int i = 0; i < nArgC; ++i)
	{
		const char* const pszArg = ppArgV[i] ? ppArgV[i] : "";
		const size_t nLen = strlen(pszArg);
		const bool bContainsSpace = strchr(pszArg, ' ') != NULL;
		const size_t nQuote = bContainsSpace ? 2 : 0;
		const size_t nSpace = (i != nArgC - 1) ? 1 : 0;
		const size_t nArgvNeed = nLen + 1;
		const size_t nArgSNeed = nLen + nQuote + nSpace + 1;

		if (nArgvNeed > (size_t)(pBufEnd - pBuf) ||
			nArgSNeed > (size_t)(pSBufEnd - pSBuf))
		{
			Warning(eDLL_T::COMMON, "%s: Encountered command which overflows the tokenizer buffer... Skipped!\n", __FUNCTION__);
			Reset();
			return;
		}

		m_ppArgv[i] = pBuf;
		memcpy(pBuf, pszArg, nLen + 1);
		if (i == 0)
			m_nArgv0Size = static_cast<ssize_t>(nLen);
		pBuf += nLen + 1;

		if (bContainsSpace)
			*pSBuf++ = '\"';
		memcpy(pSBuf, pszArg, nLen);
		pSBuf += nLen;
		if (bContainsSpace)
			*pSBuf++ = '\"';
		if (i != nArgC - 1)
			*pSBuf++ = ' ';
	}

	*pSBuf = '\0';
	m_nArgc = nArgC;
}

characterset_t* CCommand::DefaultBreakSet()
{
	return &s_BreakSet;
}

//-----------------------------------------------------------------------------
// Purpose: tokenizer
// Input: *pCommand - 
// source - 
// *pBreakSet - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CCommand::Tokenize(const char* pCommand, cmd_source_t source, characterset_t* pBreakSet)
{
	Reset();
	m_nQueuedVal = source;

	if (!pCommand)
		return false;

	// Use default break set
	if (!pBreakSet)
	{
		pBreakSet = &s_BreakSet;
	}

	// Copy the current command into a temp buffer
	// NOTE: This is here to avoid the pointers returned by DequeueNextCommand
	// to become invalid by calling AddText. Is there a way we can avoid the memcpy?
	size_t nLen = Q_strlen(pCommand);
	if (nLen >= COMMAND_MAX_LENGTH - 1)
	{
		Warning(eDLL_T::COMMON, "%s: Encountered command which overflows the tokenizer buffer... Skipped!\n", __FUNCTION__);
		return false;
	}

	memcpy(m_pArgSBuffer, pCommand, nLen + 1);

	// Parse the current command into the current command buffer
	CUtlBuffer bufParse(m_pArgSBuffer, nLen, CUtlBuffer::TEXT_BUFFER | CUtlBuffer::READ_ONLY);
	ssize_t nArgvBufferSize = 0;

	while (bufParse.IsValid() && (m_nArgc < COMMAND_MAX_ARGC))
	{
		char* pArgvBuf = &m_pArgvBuffer[nArgvBufferSize];
		ssize_t nMaxLen = COMMAND_MAX_LENGTH - nArgvBufferSize;
		// Breakset tokens write byte+NUL; nMaxLen==0 also OOB-writes pTokenBuf[0].
		if (nMaxLen < 2)
		{
			Reset();
			return false;
		}
		ssize_t nStartGet = bufParse.TellGet();
		ssize_t nSize = bufParse.ParseToken(pBreakSet, pArgvBuf, nMaxLen);

		if (nSize < 0)
			break;

		// Check for overflow condition
		if (nMaxLen == nSize)
		{
			Reset();
			return false;
		}

		if (m_nArgc == 1)
		{
			// Deal with the case where the arguments were quoted
			m_nArgv0Size = bufParse.TellGet();
			bool bFoundEndQuote = m_pArgSBuffer[m_nArgv0Size - 1] == '\"';

			if (bFoundEndQuote)
			{
				--m_nArgv0Size;
			}

			m_nArgv0Size -= nSize;
			Assert(m_nArgv0Size != 0);

			// The StartGet check is to handle this case: "foo"bar
			// which will parse into 2 different args. ArgS should point to bar.
			bool bFoundStartQuote = (m_nArgv0Size > nStartGet) && (m_pArgSBuffer[m_nArgv0Size - 1] == '\"');
			Assert(bFoundEndQuote == bFoundStartQuote);

			if (bFoundStartQuote)
			{
				--m_nArgv0Size;
			}
		}

		m_ppArgv[m_nArgc++] = pArgvBuf;

		if (m_nArgc >= COMMAND_MAX_ARGC)
		{
			Warning(eDLL_T::COMMON, "%s: Encountered command which overflows the argument buffer... Clamped!\n", __FUNCTION__);
		}

		nArgvBufferSize += nSize + 1;
		Assert(nArgvBufferSize <= COMMAND_MAX_LENGTH);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: return boolean depending on if the string only has digits in it
// Input: svString - 
//-----------------------------------------------------------------------------
bool CCommand::HasOnlyDigits(int nIndex) const
{
	const char* source = nIndex == -1 ? ArgS() : Arg(nIndex);

	for (size_t i = 0; source[i] != '\0'; i++)
	{
		if (!V_isdigit(source[i]))
		{
			return false;
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: reset
//-----------------------------------------------------------------------------
void CCommand::Reset()
{
	m_nArgc = 0;
	m_nArgv0Size = 0;
	m_pArgSBuffer[0] = 0;
	m_nQueuedVal = cmd_source_t::kCommandSrcInvalid;
}
