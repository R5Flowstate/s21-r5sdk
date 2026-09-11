#pragma once

__declspec(dllexport) void DummyExport()
{
    // Required for detours.
}

// Shared client + dedi boot banner (plain ASCII 0x20-0x7E only).
static const char* const R5F_EMBLEM[] =
{
	R"(        888888888   .d888 888                                 888             888            )",
	R"(        888        d88P"  888                                 888             888            )",
	R"(        888        888    888                                 888             888            )",
	R"(888d888 8888888b.  888888 888  .d88b.  888  888  888 .d8888b  888888  8888b.  888888 .d88b.  )",
	R"(888P"        "Y88b 888    888 d88""88b 888  888  888 88K      888        "88b 888   d8P  Y8b )",
	R"(888            888 888    888 888  888 888  888  888 "Y8888b. 888    .d888888 888   88888888 )",
	R"(888     Y88b  d88P 888    888 Y88..88P Y88b 888 d88P      X88 Y88b.  888  888 Y88b. Y8b.     )",
	R"(888      "Y8888P"  888    888  "Y88P"   "Y8888888P"   88888P'  "Y888 "Y888888  "Y888 "Y8888  )",
};
