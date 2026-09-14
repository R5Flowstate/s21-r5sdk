# Source SDK
* This repository houses the source code for the development package targeting the game **Apex Legends**.

This tree builds two products from one source:

| product | injects into | output |
|---------|--------------|--------|
| `client` | the Season 21 client executable | `game/client.dll` |
| `server` | the Season 3 dedicated server executable | `game/server.dll` |

`loader.dll` selects the matching product from the host executable name.
Agent and contributor notes live in `CLAUDE.md`.

The shipping branch of the public tree is `s21-unify`. `main` and
`S16-S21-MERGE` are upstream credit.

Valve Source SDK terms stay in `license/`. See `NOTICE`. This project is
unaffiliated with, and not endorsed by, Respawn Entertainment or
Electronic Arts.

## Building
R5sdk uses the CMake project generation and build tools. For more information, visit [CMake](https://cmake.org/).<br />
In order to compile the SDK, you will need to install Visual Studio 2017, 2019, 2022 or 2026 with:
* Desktop Development with C++ Package.
* Windows SDK 10.0.10240.0 or higher.
* C++ MFC build tools for x86 and x64.
* [Optional] C++ Clang/LLVM compiler.

Steps:
1. Download or clone the project to anywhere on your disk.
    1. Run `CreateSolution.bat` in the root folder, this will generate the files in `build_intermediate`.
       The batch file is the recipe (`OPTION_RETAIL=ON`, `OPTION_CERTAIN=OFF`,
       `OPTION_LTCG_MODE=ALL`, `OPTION_WARNINGS_AS_ERRORS=OFF`,
       `BOOST_REGEX_STANDALONE=OFF`). Re-run it after adding or removing source
       files; CMake lists them at configure time.
2. Open `r5sdk.slnx` (Visual Studio 2026) or `r5sdk.sln` (older generators) and compile the `Release` configuration.
    1. All binaries and symbols are compiled to the `game` folder.
    2. The launcher is a separate project and is not built from this tree.

## Note [IMPORTANT]
This is not a cheat or hack; attempting to use the SDK on the live version of the game could result in a permanent account ban. The supported game versions are:

 * S3 `R5pc_r5launch_N1094_CL456479_2019_10_30_05_20_PM` (dedicated server).
 * S21 `R5pc_r5-211_J12_CL6933961_2024_06_14_13_58` (client).

## Spire [DISCLAIMER]
When you host game servers on the Server Browser (Spire) you will stream your IP address to the database,
which will be stored there until you stop hosting the server; this is needed so other people can connect to your server.

There is a checkbox in the Server Browser called `Server Visibility` that defaults to `Offline`.
- `Offline`: No data is broadcasted to the master server; you are playing offline.
- `Hidden`: Your server will be broadcasted to the master server, but could only be joined using a private token.
- `Online`: Your server will be broadcasted to the master server, and could be joined from the public list.

Alternatively, you can host game servers without the use of our master server. You can grant people access to your game server
by sharing the IP address and port manually. The client can connect using the `connect` command. The usage of the `connect`
command is as follows: IPv4 `connect 127.0.0.1:37015`, IPv6 `connect [::1]:37015`. NOTE: the IP address and port were examples.
