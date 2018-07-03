#pragma once
#include "stdafx.h"
#include <thread>
#include "../Utilities/Timer.h"
#include "../Utilities/AutoResetEvent.h"

class AutoSaveManager
{
private:
	const uint32_t _autoSaveSlot = 11;
#ifndef __SWITCH__
	std::thread _autoSaveThread;
#endif

	atomic<bool> _stopThread;
	AutoResetEvent _signal;
	Timer _timer;

public:
	AutoSaveManager();
	~AutoSaveManager();
};