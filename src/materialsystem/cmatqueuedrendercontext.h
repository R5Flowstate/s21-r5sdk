#ifndef MATQUEUEDRENDERCONTEXT_H
#define MATQUEUEDRENDERCONTEXT_H

struct CallQueue_s
{
	int queueEndIndex;
	char* queueStart;
	int currentAllocIndex;
	int currentCallIndex;

	inline void* GetCurrentAllocatedItem()
	{
		return queueStart + currentAllocIndex;
	}

	inline void* GetCurrentCallItem()
	{
		return queueStart + currentCallIndex;
	}
};

inline int(**g_fnHasRenderCallQueue)(void);
inline CallQueue_s* (**g_fnAddRenderCallQueueItem)(void* method, const size_t structSize, const size_t unknown);
inline int(**g_fnAdvanceRenderCallQueue)(const size_t structSize);

#endif // MATQUEUEDRENDERCONTEXT_H
