# r5sdk -- agent notes

Human overview: `README.md`. This file is how to write code in this tree.

Match the surrounding file. Do not modernize. Do not invent a second registration
table, a second dirty-mark helper, or a second PCH.

Machine-local deploy roots, IDA, and session memory stay out of this file.
Operator overlay (this machine): `CLAUDE.local.md`.

---

## What this is

A Source-engine detour layer injected into Apex. Not a standalone game.

This tree ships two products from one source:

| product | injects | macros / PCH | output |
|---------|---------|--------------|--------|
| `client` | S21 `r5apex.exe` | `CLIENT_DLL`, `vpc_cl` PCH | `game/client.dll` |
| `server` | S3 `r5apex_ds.exe` | `DEDICATED`, `vpc` PCH | `game/server.dll` |

`loader.dll` loads the matching product. Dual-body sources use
`#if defined(CLIENT_DLL)` / `#else`. **Never infer client vs server from the
file path** -- walk the preprocessor half or the CMake gate.

Client + every `*_cl` lib **must** reuse `vpc_cl`. Shared `vpc` freezes S3
layouts; dual ABI headers then AV the client at ConVar register.

Dual ABI headers (sizes differ by product):

- `public/tier1/convar.h` (0x98 / 0xA0)
- `gametrace.h`
- `game/shared/basehandle.h`
- `iconvar.h`

`src/core/` is **client-only** except `logdef` / `logger` /
`termutil` / `stdafx`. Safe-mode exists nowhere; the scan/validate/hook
detour registry is client-only (linked into core but never driven
on the dedi -- `init_server.cpp` walks `GetCon` / `GetFun` / `GetVar` in a
plain loop. `game/shared/dt_extend.cpp` is server-only; the client links
`dt_extend_client_stub.cpp`.

`gpGlobals` is **server-only**. Client code must not dirty-mark edicts;
replication is the dedi's job.

If a field is **networked**, the fix is on the dedi. The S21 client is a
retail build -- writing client code so something renders, moves, collides,
culls, sounds, or displays is the signal that the server is not publishing
the field. A promoted class inherits the S3 parent's value for every prop
nobody set; that inherited value is usually the bug.

The dedi binary carries **both** halves (`CWeaponX` and `C_WeaponX`). A byte
signature that hits twice is expected. Identify the **server** half (script
class banner `"CWeaponX server class"` vs `"CWeaponX client class"`) and
re-derive offsets on that half. Never carry an offset across the twin
boundary.

In-tree `note(amos)` / `TODO[ AMOS ]` / `!TODO[ AMOS ]` are the original
author's unfinished markers. Leave them. Do not call the async filesystem
surface (purecall).

---

## Style

Hard tabs. Allman braces. `(void)` empty params. Trailing
`#endif // !DEDICATED`. No formatter.

| kind | form |
|------|------|
| class | `C` (`CMemory`) |
| interface | `I` (`IDetour`) |
| detour holder | `V` : `IDetour` (`VHost`) |
| member | `m_` + type (`m_pPtr`, `m_bFlag`, `m_nCount`, `m_flScale`, `m_pszName`) |
| global | `g_` + type |
| file static | `s_` |
| resolved engine fn | `v_Name` or `Class__Method` |
| free function | `Subsystem_Name` |
| ConVar | lowercase, subsystem prefix |
| ConCommand cb | `CC_Name_Action` / `Name_f` / `Class::Method_f` |
| new enum | `enum class` + `_t` |

Guard-clause early returns. C++ casts in new code (`reinterpret_cast`,
`CMemory::RCast` / `CCast`). Match `NULL` vs `nullptr` to the file.
C++17 `inline` header globals. Magic offsets get a one-line meaning.
Engine-structure walks carry a safety counter.

Banners: Valve files keep the Valve box; r5sdk-original files use the
Respawn-style `====` box. Detour classes sit between
`///////////////////////////////////////////////////////////////////////////////`
lines, inline in the subsystem header.

---

## Detours

`IDetour` (`src/thirdparty/detours/include/idetour.h`): `GetAdr` / `GetFun` /
`GetVar` / `GetCon` / `Detour`. `REGISTER(VName)` self-registers into
`g_DetourVec`. Do not invent a second table.

```cpp
///////////////////////////////////////////////////////////////////////////////
class VExample : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Example", v_Example);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
```

Unresolved pattern = loud `Warning`, feature off -- never a silent no-op.

---

## Patterns

Canonical navigation (`src/public/tier0/memaddr.h`):

```cpp
Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 57 48 83 EC 20 ...")
	.GetPtr(v_Example);

g_pSomeSize = fn.Offset(0x1B)
	.ResolveRelativeAddress(2, 10)
	.RCast<int*>();

Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 8B ?? 0C")
	.FollowNearCallSelf()
	.GetPtr(v_Cmd_Dispatch);
```

Hex + `??` wildcards. Distinctive signatures; confirm uniqueness in the
target binary before shipping. RVAs only when a pattern is impractical, and
commented as such. Direct `.exe` patch is the exception
(`VirtualProtect` + rewrite).

x64: everything is `__fastcall`; `this` is RCX. The dedi VEH catches AVs
before frame SEH -- do not deref wild pointers under `__try` expecting SEH
to save you. `abort()` bypasses the SDK process-exit hooks (silent close,
no dump). An AV in the heap walker is **never** the named subsystem; hunt
the earlier write through a wrong-half or wrong-offset pointer.

---

## Log / ConVar / Error

Logs take `eDLL_T` first and a bracketed `[Tag]`:

```cpp
DevMsg(eDLL_T::ENGINE, "[DT_Inject] Injected %d\n", n);
```

`Error(eDLL_T, code, ...)` is **not** stock Source. Nonzero code
`TerminateProcess`s. `Error(..., NO_ERROR, ...)` logs and **returns**.
After `Error(NO_ERROR)`: hard-return. Never `SetState(HS_RUN)` on that
branch. Do not make `Error()` stock-fatal -- RCON pre-auth depends on
`NO_ERROR` being non-fatal (`base_rcon.cpp`).

Observe-only ConVars (`*_diag` / `*_probe` / `*_trace` / `*_dump` / `*_tap`
/ `*_census` / `*_log` / `*_debug`) are `FCVAR_DEVELOPMENTONLY`, written
out at the declaration. `FCVAR_RELEASE` is for levers that change
behaviour. The ctor auto-adds `DEVELOPMENTONLY` when no release-mask flag
is set; stating it makes the intent reviewable. `-devsdk` strips the flag
at run time. Hiding a lever does not turn it off -- a non-zero default
still runs every boot. Before demoting an existing ConVar, grep
`platform/cfg/*.cfg` for its name.

Cross-thread ConVars need `FCVAR_ACCESSIBLE_FROM_THREADS`. Hot-path diags
are rate-limited and gated.

Console recipes (value differs from ship default only): one pasteable line
per engine, `name value;name value` with no space after `;`. Separate
**DEDI** and **CLIENT** lines. Boot-time levers also as `+name value`.
Include the lever under test, anything a cfg overrides, and any competing
lever that would confound the measurement.

---

## Squirrel

Three VMs: SERVER (dedi), CLIENT, UI. Name `ServerScript_` /
`ClientScript_` / `SharedScript_`. Stack index 2 is the first argument.
Return through `SCRIPT_CHECK_AND_RETURN`. Treat script input as
attacker-controlled.

An uncaught server-VM error schedules `HS_GAME_SHUTDOWN` (~80 ms;
`Shutdown host game`). The player reads a random kick. Every
player-reachable `ExecuteFunction` (C2S ScriptRemote first) must check
`SCRIPT_ERROR`, log + drop, and restore `HS_RUN` if shutdown was
scheduled. Contain at the dispatch (`scriptremotefunctions_server.cpp`).
`Remote_RegisterServerFunction(..., "int", INT_MIN, INT_MAX)` plus
`default: Assert(false)` is a one-packet match kill -- register the real
range; default is `Warning` + return.

---

## Containment

Contain at the **boundary**, not at each handler.

| symptom | cause |
|---------|--------|
| `Shutdown host game` / random kick, no AV | uncaught `SCRIPT_ERROR` (above) |
| `Error(...)` then empty `HS_RUN` | `Error(NO_ERROR)` returned; do not `SetState(HS_RUN)` |
| silent close / `abort()` / huge `new` | wire `u32` size with no ceiling |
| dedi crash on compressed C2S | LZSS checked output only; pass `unInputSize` |
| AV after changelevel / toss | `void*` entity cache; key with `SDKEntityHandle` |

Wire sizes, counts, and names are attacker-controlled. Ceiling before
`new` / `VirtualAlloc` / bit-cursor move (`new (std::nothrow)` + drop).
Validate `dataLen` against remaining bits **before** moving `bitPos`.
Map names go through `Bridge_IsBareMapName` (`[a-zA-Z0-9_]{3,63}`) --
`..`, `/`, UNC are not a name. Signon `m_nDataBytes` is 22-bit
(`0x3FFFFF`); a 4 MiB ceiling wraps to 0.

Dedi entity caches: `SDKEntityHandle` / `SDKEntityMap`, erase on destroy,
`SDKEntityState_FlushAll` at LevelShutdown, apply to **every** map in the
file.

Snapshot desyncs are bit-exact. First-divergence wins; everything after
is cascade. Watch encode and decode. A hypothesis ruled out on one side
is not ruled out.

No allocations on per-snapshot / per-frame paths. Pre-size file-scope
buffers with a hard bounds check.

---

## Build

Release only. CMake lists sources at configure time -- re-run configure
after adding or removing a `.cpp` / `.h`, after CMake flag changes, or on
a fresh checkout. Incremental MSBuild ignores new TUs until then.

A header layout change needs a **clean** rebuild. Two layouts of one class
in one binary is a real failure mode. Trust a crash repro only after the
log shows `All N functions were compiled because no usable IPDB/IOBJ`.

```
cmake -B build_intermediate -G "Visual Studio 18 2026" -A x64
  -DBOOST_REGEX_STANDALONE=OFF -DOPTION_CERTAIN=OFF -DOPTION_RETAIL=ON
  -DOPTION_LTCG_MODE=ALL -DOPTION_WARNINGS_AS_ERRORS=OFF

msbuild src/core/client.vcxproj -t:Build -p:Configuration=Release -p:Platform=x64
msbuild src/core/server.vcxproj -t:Build -p:Configuration=Release -p:Platform=x64
msbuild src/loader/loader.vcxproj -t:Build -p:Configuration=Release -p:Platform=x64
```

Loader is that vcxproj, not `-t:loader` through the slnx (MSB4057).
Never pipe MSBuild through `tail` / `head` -- the pipe hides the exit code.

`GAMEDLL_S21` gates nothing in source. `/EHa` is client-only;
`__try/__except` works under both `/EHa` and `/EHsc`.

Dead `_client.inl` files under `game/shared/` are merge artifacts -- not
in CMake. Ignore them when grepping callers.

Before writing a helper, search for the existing one. If it returns too
little, widen it -- do not copy it.

---

## Logs / deploy

File logs are OFF by default (release minimum-disk): stdout is the log, and only
crash-time files hit disk (`apex_crash.txt` + `*.dmp` under
`platform/logs/<role>/<GUID>/`, `veh_crash.log` in `platform\`). File sinks are
created only with `-devsdk` / `-dev` / `-developer` / `-logfiles` (`-nologfiles`
forces them off even then); each such run writes its own
`platform/logs/<client|server>/<GUID>/` channel-split `.log` set -- list the
newest GUID folder, then grep **that folder only**. Do not recurse the deploy root.

`apex_crash.txt` RVAs are already module-relative. DLL and PDB must match
the crashing build (size / mtime). Codebook and string-table changes need
a full client restart, not a reconnect.

---

## Git

Stage **explicit paths**. Never `git add -A` / `git add .`.
`git status --short` before every commit. Log style
`unify: <subsystem> -- <summary>` (`client:` / `server:` when
product-specific). Smaller cohesive commits.

Revert means that change only. `git checkout HEAD -- <file>` only when
the whole file's recent history is meant to go.

---

## Checklist

- Hard tabs, Allman, `(void)`, matching banner.
- `C` / `I` / `V`; `m_` / `g_` / `s_` / `v_` + type letter.
- `eDLL_T` + `[Tag]`; stubs are loud.
- Observe-only ConVar is `FCVAR_DEVELOPMENTONLY`.
- Engine code via `Module_FindPattern` chains, not raw RVAs.
- New detour: inline in the subsystem header, `REGISTER(VName)`.
- Dual-body: `#if CLIENT_DLL` is the product, not the folder.
- Client / `*_cl` reuse `vpc_cl`.
- After `Error(NO_ERROR)`, hard-return.
- Player-reachable `ExecuteFunction` traps `SCRIPT_ERROR` and cancels
  `HS_GAME_SHUTDOWN`.
- Wire sizes / counts / names have a ceiling; LZSS takes input length;
  dedi caches are handle-keyed.
- `#endif // !X` on every conditional region.
- Pattern unique in the target binary.
- Do not modernize the file you are in.
