#if defined(CLIENT_DLL)
#ifndef C_BASEENTITY_H
#define C_BASEENTITY_H

#include "mathlib/vector.h"
#include "vscript/ivscript.h"

#include "game/shared/collisionproperty.h"
#include "game/shared/particleproperty.h"
#include "game/shared/predictioncopy.h"
#include "game/client/icliententity.h"

// How many data slots to use when in multiplayer.
#define MULTIPLAYER_BACKUP 750

// Opaque by design: the former member layout did not match the live client and
// nothing dereferenced it (entity fields are read by explicit offset). Leaving it
// incomplete turns any future member access into a compile error.
class C_BaseEntity;

inline HSCRIPT(*v_C_BaseEntity__GetScriptInstance)(C_BaseEntity* thisp);


#endif // C_BASEENTITY_H
#else // !CLIENT_DLL
#ifndef C_BASEENTITY_H
#define C_BASEENTITY_H

#include "mathlib/vector.h"
#include "vscript/ivscript.h"

#include "game/shared/collisionproperty.h"
#include "game/shared/particleproperty.h"
#include "game/shared/predictioncopy.h"
#include "game/client/icliententity.h"

// How many data slots to use when in multiplayer.
#define MULTIPLAYER_BACKUP 750

// Opaque by design: the former member layout did not match the live client and
// nothing dereferenced it (entity fields are read by explicit offset). Leaving it
// incomplete turns any future member access into a compile error.
class C_BaseEntity;

inline HSCRIPT(*v_C_BaseEntity__GetScriptInstance)(C_BaseEntity* thisp);


#endif // C_BASEENTITY_H
#endif // CLIENT_DLL
