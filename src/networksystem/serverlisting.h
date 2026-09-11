#pragma once

struct NetGameServer_t
{
	// the name and description of this listing, which will be display to the
	// client's server browser
	string name;
	string description;

	// whether or not this is a visible 'public' gameserver; this is only used
	// on the masterserver to determine whether or not to broadcast your
	// listing from there
	bool hidden = true;
	bool hasPassword = false;

	// the level and playlist of the server, which will be display to the
	// client's server browser
	string map;
	string playlist;

	// the address and port of the server, validated and set from the
	// masterserver
	string address;
	int port = NULL;

	// the base64 net key used to decrypt game packets, the client has to
	// install this before issuing a connectionless packet
	string netKey;

	string netPassword;

	// version identifiers used to check if the gameserver and gameclient are
	// compatible with each other
	unsigned int checksum = NULL;
	string versionId;

	// current amount of players, and the maximum allowed for this gameserver
	int numPlayers = NULL;
	int maxPlayers = NULL;

	// the issue time of this listing
	int64_t timeStamp = -1;

	// required mod ids for clients to join this server
	vector<string> requiredMods;

	// additional mod ids a client may have when sv_modPolicy is 2
	vector<string> allowedMods;

	// the mods profile identifier for this server
	string modsProfile;

	// Region the master server reports for this listing, shown in the browser.
	// A host never sets it, so it stays last: the struct is built by positional
	// aggregate initialization in several places, and a field inserted higher up
	// silently shifts every value after it.
	string region;
};
