#pragma once

// Boot ledger can report how many chain steps hit an invalid base.
int CMemory_GetInvalidChainCount(void);
void CMemory_ReportInvalidChain(void);

class CMemory
{
public:
	enum class Direction : int
	{
		DOWN = 0,
		UP,
	};

	CMemory(void) = default;
	CMemory(const uintptr_t ptr) : ptr(ptr) {}
	CMemory(const void* ptr) : ptr(uintptr_t(ptr)) {}

	inline operator uintptr_t(void) const
	{
		return ptr;
	}

	inline operator void*(void) const
	{
		return reinterpret_cast<void*>(ptr);
	}

	inline operator bool(void) const
	{
		return ptr != NULL;
	}

	inline bool operator!= (const CMemory& addr) const
	{
		return ptr != addr.ptr;
	}

	inline bool operator== (const CMemory& addr) const
	{
		return ptr == addr.ptr;
	}

	inline bool operator== (const uintptr_t& addr) const
	{
		return ptr == addr;
	}

	// Non-null and above the Windows lowest-user-page boundary (failed match / bad step).
	inline bool IsValid(void) const
	{
		return ptr >= 0x10000;
	}

	inline uintptr_t GetPtr(void) const
	{
		return ptr;
	}

	template<typename T>
	inline void GetPtr(T*& outPtr) const
	{
		outPtr = IsValid() ? reinterpret_cast<T*>(ptr) : nullptr;
	}

	template<class T> inline T GetValue(void) const
	{
		if (!IsValid())
		{
			CMemory_ReportInvalidChain();
			return T{};
		}
		return *reinterpret_cast<T*>(ptr);
	}

	template<class T> inline T GetVirtualFunctionIndex(void) const
	{
		if (!IsValid())
		{
			CMemory_ReportInvalidChain();
			return T{};
		}
		return *reinterpret_cast<T*>(ptr) / 8; // Its divided by 8 in x64.
	}

	template<typename T> inline T CCast(void) const
	{
		return (T)ptr;
	}

	template<typename T> inline T RCast(void) const
	{
		return reinterpret_cast<T>(ptr);
	}

	inline CMemory Offset(ptrdiff_t offset) const
	{
		if (!IsValid())
			return CMemory();
		return CMemory(ptr + offset);
	}

	inline CMemory OffsetSelf(ptrdiff_t offset)
	{
		if (!IsValid())
		{
			ptr = 0;
			return *this;
		}
		ptr += offset;
		return *this;
	}

	inline CMemory Deref(int deref = 1) const
	{
		uintptr_t reference = ptr;

		while (deref--)
		{
			if (reference < 0x10000)
			{
				CMemory_ReportInvalidChain();
				return CMemory();
			}
			reference = *reinterpret_cast<uintptr_t*>(reference);
		}

		return CMemory(reference);
	}

	inline CMemory DerefSelf(int deref = 1)
	{
		while (deref--)
		{
			if (!IsValid())
			{
				CMemory_ReportInvalidChain();
				ptr = 0;
				return *this;
			}
			ptr = *reinterpret_cast<uintptr_t*>(ptr);
		}

		return *this;
	}

	inline CMemory WalkVTable(ptrdiff_t vfuncIndex)
	{
		if (!IsValid())
			return CMemory();
		uintptr_t reference = ptr + (8 * vfuncIndex);
		return CMemory(reference);
	}

	inline CMemory WalkVTableSelf(ptrdiff_t vfuncIndex)
	{
		if (!IsValid())
		{
			ptr = 0;
			return *this;
		}
		ptr += (8 * vfuncIndex);
		return *this;
	}

	bool CheckOpCodes(const vector<uint8_t>& vOpcodeArray) const;
	void Patch(const vector<uint8_t>& vOpcodeArray) const;
	void PatchString(const char* szString) const;

	CMemory FindPattern(const char* szPattern, const Direction searchDirect = Direction::DOWN, const int opCodesToScan = 512, const ptrdiff_t occurrence = 1) const;
	CMemory FindPatternSelf(const char* szPattern, const Direction searchDirect = Direction::DOWN, const int opCodesToScan = 512, const ptrdiff_t occurrence = 1);
	vector<CMemory> FindAllCallReferences(const uintptr_t sectionBase, const size_t sectionSize);

	// E8/E9 5-byte rel32; target must land in g_GameDll or the result is null.
	CMemory FollowNearCall(const ptrdiff_t opcodeOffset = 0x1, const ptrdiff_t nextInstructionOffset = 0x5) const;
	CMemory FollowNearCallSelf(const ptrdiff_t opcodeOffset = 0x1, const ptrdiff_t nextInstructionOffset = 0x5);
	CMemory ResolveRelativeAddress(const ptrdiff_t registerOffset = 0x0, const ptrdiff_t nextInstructionOffset = 0x4) const;
	CMemory ResolveRelativeAddressSelf(const ptrdiff_t registerOffset = 0x0, const ptrdiff_t nextInstructionOffset = 0x4);

	static void HookVirtualMethod(const uintptr_t virtualTable, const void* pHookMethod, const ptrdiff_t methodIndex, void** ppOriginalMethod);
	static void HookImportedFunction(const uintptr_t pImportedMethod, const void* pHookMethod, void** ppOriginalMethod);

private:
	uintptr_t ptr = 0;
};
