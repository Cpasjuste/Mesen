#pragma once 
#include "stdafx.h"

#include <condition_variable>
#include <mutex>

#ifdef __SWITCH__
typedef _LOCK_T Mutex;
typedef struct {
	uint32_t tag;
	Mutex* mutex;
} CondVar;
#endif

class AutoResetEvent
{
private:
#ifdef __SWITCH__
	CondVar condVar;
	Mutex condMutex;
	Mutex mutex;
#else
	std::condition_variable _signal;
	std::mutex _mutex;
#endif
	bool _signaled;

public:
	AutoResetEvent();
	~AutoResetEvent();

	void Reset();
	void Wait(int timeoutDelay = 0);
	void Signal();
};
