#ifndef ENGINE_SHARED_HOST_PROOF_H
#define ENGINE_SHARED_HOST_PROOF_H

// Proof that a connecting client runs on the server's own machine. The dedi
// publishes a random key in a session-local named mapping keyed by its port;
// only processes of the same user on that machine can open it, so a relay or
// tunnel that forwards remote players from 127.0.0.1 cannot present it.
#define HOST_PROOF_MAPPING_FMT "Local\\R5F_HostProof_%d"
#define HOST_PROOF_CONVAR      "cl_hostProof"
#define HOST_PROOF_HEX_LEN     64

#endif // ENGINE_SHARED_HOST_PROOF_H
