//=============================================================================//
//
// Purpose: Process memory and rpak slot table census at map pak changes.
//
//=============================================================================//
#ifndef PAK_CENSUS_S21_H
#define PAK_CENSUS_S21_H

void Pak_CensusLog(const char* pszReason, bool bForce);
void Pak_CensusTick(void);

///////////////////////////////////////////////////////////////////////////////
class VPakCensus : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // PAK_CENSUS_S21_H
