#pragma once

// Per-tick frame-budget census for the dedicated server. A 5s average
// hides the jitter that is actually felt as lag: a run holding 19.8 fps
// can still be missing its 50ms deadline on a third of its ticks.

bool TickBudget_Armed(void);

// Last tick's SendClientMessages cost. recipients is encode-path clients
// only (fake players do not take that path).
void TickBudget_ReportScm(long long scmUs, long long waitUs, int recipients);
