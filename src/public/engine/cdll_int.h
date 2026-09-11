#ifndef CDLL_INT_H
#define CDLL_INT_H

// change this when the new version is incompatable with the old
#define VENGINE_CLIENT_INTERFACE_VERSION		"VEngineClient013"

//-----------------------------------------------------------------------------
// Purpose: Interface exposed from the engine to the client.dll
//-----------------------------------------------------------------------------
abstract_class IVEngineClient
{
public:
	// Find the model's surfaces that intersect the given sphere.
	// Returns the number of surfaces filled in.
	virtual int					GetIntersectingSurfaces(/*params undocumented*/) = 0;

	virtual void				GetLightForPoint(/*params undocumented*/) = 0;

	// Returns whether all optional streaming files have finished downloading.
	virtual bool				StreamingDownloadFinished() const = 0;

	virtual void				ReturnsTrue_Unknown() const = 0; // always returns true.

	// Returns the current optional streaming files download progress.
	virtual float				GetStreamingDownloadProgress() const = 0;

	virtual void				UnknownEngineClientFn0() const = 0;
	virtual void				UnknownEngineClientFn1() const = 0;
	virtual void				UnknownEngineClientFn2() const = 0;
	virtual void				UnknownEngineClientFn3() const = 0;
	virtual void				UnknownEngineClientFn4() const = 0;

	virtual void				UnknownEngineClientFn5() const = 0;
	virtual void				UnknownEngineClientFn6() const = 0;
	virtual void				UnknownEngineClientFn7() const = 0;
	virtual void				UnknownEngineClientFn8() const = 0;
	virtual void				UnknownEngineClientFn9() const = 0;
	virtual void				UnknownEngineClientFn10() const = 0;
	virtual void				UnknownEngineClientFn11() const = 0;
	virtual void				UnknownEngineClientFn12() const = 0;

	virtual void				Disconnect(const char* const reason) const = 0;
	virtual void				GetDefaultScreenSize(int& width, int& height) const = 0;

	virtual void				UnknownEngineClientFn13() const = 0;

	// Gets the dimensions of the game window
	virtual void				GetScreenSize(int& width, int& height) const = 0;

	// Forwards szCmdString to the server.
	virtual void				ServerCmd(const char* szCmdString) = 0;
	// Inserts szCmdString into the command buffer as if it was typed by the client to his/her console.
	// Note: Calls to this are checked against FCVAR_CLIENTCMD_CAN_EXECUTE (if that bit is not set, then this function can't change it).
	// Call ClientCmd_Unrestricted to have access to FCVAR_CLIENTCMD_CAN_EXECUTE vars.
	virtual void				ClientCmd(const char* szCmdString) = 0;
};

#endif // CDLL_INT_H
