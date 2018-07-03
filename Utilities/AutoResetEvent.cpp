#include "stdafx.h"
#include "AutoResetEvent.h"

#ifdef __SWITCH__
extern "C" {
	void mutexLock(Mutex* m);
	void condvarInit(CondVar* c, Mutex* m);
	uint32_t condvarWaitTimeout(CondVar* c, uint64_t timeout);
	uint32_t condvarWake(CondVar* c, int num);
}

static inline void mutexInit(Mutex* m) {
	*m = 0;
}

static inline uint32_t condvarWait(CondVar* c) {
	return condvarWaitTimeout(c, -1ull);
}

static inline uint32_t condvarWakeAll(CondVar* c) {
	return condvarWake(c, -1);
}
#endif

AutoResetEvent::AutoResetEvent()
{
#ifdef __SWITCH__
	mutexInit(&mutex);
	mutexInit(&condMutex);
	condvarInit(&condVar, &condMutex);
#endif
	_signaled = false;
}

AutoResetEvent::~AutoResetEvent()
{
	//Can't signal here, seems to cause process crashes when this occurs while the
	//application is exiting.
}

void AutoResetEvent::Wait(int timeoutDelay)
{
#ifdef __SWITCH__
	mutexLock(&mutex);
	if(timeoutDelay == 0) {
		condvarWait(&condVar);
	} else {
		condvarWaitTimeout(&condVar, (uint64_t) timeoutDelay * 1000000);
	}
#else
	std::unique_lock<std::mutex> lock(_mutex);
	if(timeoutDelay == 0) {
		//Wait until signaled
		_signal.wait(lock, [this] { return _signaled; });
	} else {
		//Wait until signaled or timeout
		auto timeoutTime = std::chrono::system_clock::now() + std::chrono::duration<int, std::milli>(timeoutDelay);
		_signal.wait_until(lock, timeoutTime, [this] { return _signaled; });
	}
#endif
	_signaled = false;
}

void AutoResetEvent::Reset()
{
#ifdef __SWITCH__
	mutexLock(&mutex);
	_signaled = false;
#else
	std::unique_lock<std::mutex> lock(_mutex);
#endif
	_signaled = false;
}

void AutoResetEvent::Signal()
{
#ifdef __SWITCH__
	mutexLock(&mutex);
	_signaled = true;
	condvarWakeAll(&condVar);
#else
	std::unique_lock<std::mutex> lock(_mutex);
	_signaled = true;
	_signal.notify_all();
#endif
}
