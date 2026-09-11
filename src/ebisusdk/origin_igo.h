#pragma once

// Neutralize the EA in-game overlay (IGO64 Present/raw-input hook) without
// touching LSX identity. Opt out with -sdk_allow_igo.
void Origin_InstallIgoGuard(void);
