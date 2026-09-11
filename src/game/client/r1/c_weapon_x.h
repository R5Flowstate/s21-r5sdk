#ifndef C_WEAPON_X
#define C_WEAPON_X

#include <cstddef>
#include <cstdint>

//-----------------------------------------------------------------------------
// C_WeaponX -- live S21 client layout (standalone pad; not sizeof(C_BaseAnimating)).
// Proven anchors through m_customActivity @ 0x15CE; m_fireMode verified at 0x2BC8.
//-----------------------------------------------------------------------------
class C_WeaponX
{
public:
	inline bool IsWeaponOffhandMelee() const
	{
		return static_cast<unsigned int>(m_fireMode - 5) <= 1;
	}

	// Exposed for RUI track system (ruitracks.cpp)
	inline float GetLastPrimaryAttack() const { return m_lastPrimaryAttackTime; }

public:
	// All data members public: standard-layout for offsetof.
	char _pad_base[0x1560]; // live S21 base-class region; NOT sizeof(C_BaseAnimating)

	uint32_t m_weaponOwner;              // CHandle<C_BaseCombatCharacter>
	float    m_lastPrimaryAttackTime;
	float    m_nextReadyTime;
	float    m_nextPrimaryAttackTime;
	float    m_attackTimeThisFrame;
	int32_t  m_worldModelIndexOverride;
	int32_t  m_iWorldModelIndex;
	int32_t  m_holsterModelIndex;
	int32_t  m_droppedModelIndex;
	int16_t  m_idealSequence;            // Sequence_t
	int16_t  m_idealActivity;            // Activity_t
	int16_t  m_weaponActivity;           // Activity_t
	char     _pad_158A[2];
	int32_t  m_ActiveState;              // WeaponActiveState_e
	uint32_t m_ammoInClip;
	uint32_t m_ammoInStockpile;
	int32_t  m_infiniteAmmoState;        // InfiniteAmmoState_e
	int32_t  m_lifetimeShots;
	float    m_flTimeWeaponIdle;
	int32_t  m_weapState;                // WeaponState_e
	bool     m_allowedToUse;
	bool     m_discarded;
	bool     m_bInReload;
	char     _pad_15AB[1];
	int32_t  m_forcedADS;
	uint8_t  m_tossRelease;              // TossWeaponRelease_e
	char     _pad_15B1[3];
	int32_t  m_offhandSwitchSlot;        // eActiveInventorySlot
	uint8_t  m_energizeState;            // EnergizeState_e
	char     _pad_15B9[1];               // reserved (S21 use of next byte unconfirmed)
	char     _pad_15BA[2];
	float    m_startEnergizingTime;
	float    m_energizedEndTime;
	float    m_heatValue;
	float    m_heatValueOnLastFire;
	bool     m_fullyHeated;
	char     _pad_15CD[1];
	int16_t  m_customActivity;           // Activity_t
	int16_t  m_customActivitySequence;   // Sequence_t
	char     _pad_15D2[2];
	uint32_t m_customActivityOwner;      // CHandle<C_BaseEntity>
	float    m_customActivityEndTime;
	int16_t  m_customActivityFlags;
	char     _pad_15DE[2];

	char _pad_to_fireMode[0x2BC8 - 0x15E0];

	// Weapon fire/class mode; only this field of the mod-var block is exposed.
	int32_t m_fireMode;
};

static_assert(offsetof(C_WeaponX, m_idealSequence)  == 0x1584);
static_assert(offsetof(C_WeaponX, m_weaponActivity) == 0x1588);
static_assert(offsetof(C_WeaponX, m_weapState)      == 0x15A4);
static_assert(offsetof(C_WeaponX, m_energizeState)  == 0x15B8);
static_assert(offsetof(C_WeaponX, m_customActivity) == 0x15CE);
static_assert(offsetof(C_WeaponX, m_fireMode)       == 0x2BC8);

#endif // C_WEAPON_X
