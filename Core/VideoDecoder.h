#pragma once
#include "stdafx.h"
#include <thread>
#ifndef __SWITCH__
using std::thread;
#endif

#include "../Utilities/SimpleLock.h"
#include "../Utilities/AutoResetEvent.h"
#include "EmulationSettings.h"
#include "FrameInfo.h"

class BaseVideoFilter;
class ScaleFilter;
class RotateFilter;
class IRenderingDevice;
class VideoHud;
struct HdScreenInfo;

struct ScreenSize
{
	int32_t Width;
	int32_t Height;
	double Scale;
};

class VideoDecoder
{
private:
	static unique_ptr<VideoDecoder> Instance;

	uint16_t *_ppuOutputBuffer = nullptr;
	HdScreenInfo *_hdScreenInfo = nullptr;
	bool _hdFilterEnabled = false;
	uint32_t _frameNumber = 0;

#ifndef __SWITCH__
	unique_ptr<thread> _decodeThread;
#endif
	unique_ptr<VideoHud> _hud;

	AutoResetEvent _waitForFrame;
	
	atomic<bool> _frameChanged;
	atomic<bool> _stopFlag;
	uint32_t _frameCount = 0;

	ScreenSize _previousScreenSize = {};
	double _previousScale = 0;
	FrameInfo _lastFrameInfo;

	VideoFilterType _videoFilterType = VideoFilterType::None;
	unique_ptr<BaseVideoFilter> _videoFilter;
	shared_ptr<ScaleFilter> _scaleFilter;
	shared_ptr<RotateFilter> _rotateFilter;

	void UpdateVideoFilter();

	void DecodeThread();

public:
	static VideoDecoder* GetInstance();
	VideoDecoder();
	~VideoDecoder();

	static void Release();

	void DecodeFrame(bool synchronous = false);
	void TakeScreenshot();
	void TakeScreenshot(std::stringstream &stream);

	uint32_t GetFrameCount();

	FrameInfo GetFrameInfo();
	void GetScreenSize(ScreenSize &size, bool ignoreScale);

	void UpdateFrameSync(void* frameBuffer, HdScreenInfo *hdScreenInfo = nullptr);
	void UpdateFrame(void* frameBuffer, HdScreenInfo *hdScreenInfo = nullptr);

	bool IsRunning();
	void StartThread();
	void StopThread();
};