#pragma once 
#include "stdafx.h"
#include <thread>

class SimpleLock;

class LockHandler
{
private:
	SimpleLock *_lock;
public:
	LockHandler(SimpleLock *lock);
	~LockHandler();
};

class SimpleLock
{
private:
#ifdef __SWITCH__
	int _holderThreadID;
#else
	thread_local static std::thread::id _threadID;

	std::thread::id _holderThreadID;
#endif
	uint32_t _lockCount;
	atomic_flag _lock;

public:
	SimpleLock();
	~SimpleLock();

	LockHandler AcquireSafe();

	void Acquire();
	bool IsFree();
	void WaitForRelease();
	void Release();
#ifdef __SWITCH__
	static void setMainThreadId(int tid);
#endif

};
