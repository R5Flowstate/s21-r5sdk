#ifndef LOADER_H
#define LOADER_H

//-----------------------------------------------------------------------------
// PE import glue: undecorated extern "C" DummyExport so the exe maps loader.dll first.
//-----------------------------------------------------------------------------
extern "C" __declspec(dllexport) void DummyExport()
{
}

#endif // LOADER_H
