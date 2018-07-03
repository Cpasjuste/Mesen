#include "stdafx.h"
#include <thread>
#include "Console.h"
#include "CPU.h"
#include "PPU.h"
#include "APU.h"
#include "MemoryManager.h"
#include "AutoSaveManager.h"
#include "BaseMapper.h"
#include "ControlManager.h"
#include "VsControlManager.h"
#include "MapperFactory.h"
#include "Debugger.h"
#include "MessageManager.h"
#include "RomLoader.h"
#include "EmulationSettings.h"
#include "../Utilities/sha1.h"
#include "../Utilities/Timer.h"
#include "../Utilities/FolderUtilities.h"
#include "../Utilities/PlatformUtilities.h"
#include "VirtualFile.h"
#include "HdBuilderPpu.h"
#include "HdPpu.h"
#include "NsfPpu.h"
#include "SoundMixer.h"
#include "NsfMapper.h"
#include "MovieManager.h"
#include "RewindManager.h"
#include "SaveStateManager.h"
#include "HdPackBuilder.h"
#include "HdAudioDevice.h"
#include "FDS.h"
#include "SystemActionManager.h"
#include "FdsSystemActionManager.h"
#include "VsSystemActionManager.h"
#include "FamilyBasicDataRecorder.h"
#include "IBarcodeReader.h"
#include "IBattery.h"
#include "KeyManager.h"
#include "BatteryManager.h"
#include "DebugHud.h"

shared_ptr<Console> Console::Instance(new Console());

Console::Console()
{
	_model = NesModel::NTSC;
}

Console::~Console()
{
	MovieManager::Stop();
	SoundMixer::StopRecording();
}

shared_ptr<Console> Console::GetInstance()
{
	return Console::Instance;
}

void Console::Release()
{
	Console::Instance.reset();
}

void Console::SaveBatteries()
{
	shared_ptr<BaseMapper> mapper = _mapper;
	shared_ptr<ControlManager> controlManager = _controlManager;
	
	if(mapper) {
		mapper->SaveBattery();
	}

	if(controlManager) {
		shared_ptr<IBattery> device = std::dynamic_pointer_cast<IBattery>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort));
		if(device) {
			device->SaveBattery();
		}
	}
}

bool Console::Initialize(VirtualFile &romFile, VirtualFile &patchFile)
{
	if(!_romFilepath.empty() && _mapper) {
		//Ensure we save any battery file before loading a new game
		SaveBatteries();
	}

	if(romFile.IsValid()) {
		VideoDecoder::GetInstance()->StopThread();

		LoadHdPack(romFile, patchFile);
		if(patchFile.IsValid()) {
			if(romFile.ApplyPatch(patchFile)) {
				MessageManager::DisplayMessage("Patch", "ApplyingPatch", patchFile.GetFileName());
			} else {
				//Patch failed
			}
		}
		vector<uint8_t> fileData;
		romFile.ReadFile(fileData);

		BatteryManager::Initialize(FolderUtilities::GetFilename(romFile.GetFileName(), false));
		shared_ptr<BaseMapper> mapper = MapperFactory::InitializeFromFile(romFile.GetFileName(), fileData);
		if(mapper) {
			SoundMixer::StopAudio(true);

			bool isDifferentGame = _romFilepath != (string)romFile || _patchFilename != (string)patchFile;
			if(_mapper) {
				if(isDifferentGame) {
					//Save current game state before loading another one
					SaveStateManager::SaveRecentGame(GetMapperInfo().RomName, _romFilepath, _patchFilename);
				}

				//Send notification only if a game was already running and we successfully loaded the new one
				MessageManager::SendNotification(ConsoleNotificationType::GameStopped, (void*)1);
			}

			if(isDifferentGame) {
				_romFilepath = romFile;
				_patchFilename = patchFile;
				
				//Changed game, stop all recordings
				MovieManager::Stop();
				SoundMixer::StopRecording();
				StopRecordingHdPack();
			}

#ifndef LIBRETRO
			//Don't use auto-save manager for libretro
			_autoSaveManager.reset(new AutoSaveManager());
#endif

			_mapper = mapper;
			_memoryManager.reset(new MemoryManager(_mapper));
			_cpu.reset(new CPU(_memoryManager.get()));
			_apu.reset(new APU(_memoryManager.get()));

			GameSystem system = _mapper->GetMapperInfo().System;

			switch(system) {
				case GameSystem::FDS:
					EmulationSettings::SetPpuModel(PpuModel::Ppu2C02);
					_systemActionManager.reset(new FdsSystemActionManager(Console::GetInstance(), _mapper));
					break;
				
				case GameSystem::VsUniSystem:
					_systemActionManager.reset(new VsSystemActionManager(Console::GetInstance()));
					break;
				
				default: 
					EmulationSettings::SetPpuModel(PpuModel::Ppu2C02);
					_systemActionManager.reset(new SystemActionManager(Console::GetInstance())); break;
			}

			//Temporarely disable battery saves to prevent battery files from being created for the wrong game (for Battle Box & Turbo File)
			BatteryManager::SetSaveEnabled(false);
			if(system == GameSystem::VsUniSystem) {
				_controlManager.reset(new VsControlManager(_systemActionManager, _mapper->GetMapperControlDevice()));
			} else {
				_controlManager.reset(new ControlManager(_systemActionManager, _mapper->GetMapperControlDevice()));
			}
			_controlManager->UpdateControlDevices();
			//Re-enable battery saves
			BatteryManager::SetSaveEnabled(true);
			
			if(_hdData && (!_hdData->Tiles.empty() || !_hdData->Backgrounds.empty())) {
				_ppu.reset(new HdPpu(_mapper.get(), _controlManager.get(), _hdData.get()));
			} else if(std::dynamic_pointer_cast<NsfMapper>(_mapper)) {
				//Disable most of the PPU for NSFs
				_ppu.reset(new NsfPpu(_mapper.get(), _controlManager.get()));
			} else {
				_ppu.reset(new PPU(_mapper.get(), _controlManager.get()));
			}

			_memoryManager->RegisterIODevice(_ppu.get());
			_memoryManager->RegisterIODevice(_apu.get());
			_memoryManager->RegisterIODevice(_controlManager.get());
			_memoryManager->RegisterIODevice(_mapper.get());
			if(_hdData && (!_hdData->BgmFilesById.empty() || !_hdData->SfxFilesById.empty())) {
				_hdAudioDevice.reset(new HdAudioDevice(_hdData.get()));
				_memoryManager->RegisterIODevice(_hdAudioDevice.get());
			}

			_model = NesModel::Auto;
			UpdateNesModel(false);

			_initialized = true;

			if(_debugger) {
				//Reset debugger if it was running before
				auto lock = _debuggerLock.AcquireSafe();
				StopDebugger();
				GetDebugger();
			}

			ResetComponents(false);

			_rewindManager.reset(new RewindManager());
			MessageManager::RegisterNotificationListener(_rewindManager);

			VideoDecoder::GetInstance()->StartThread();

			FolderUtilities::AddKnownGameFolder(romFile.GetFolderPath());

			string modelName = _model == NesModel::PAL ? "PAL" : (_model == NesModel::Dendy ? "Dendy" : "NTSC");
			string messageTitle = MessageManager::Localize("GameLoaded") + " (" + modelName + ")";
			MessageManager::DisplayMessage(messageTitle, FolderUtilities::GetFilename(GetMapperInfo().RomName, false));
			if(EmulationSettings::GetOverclockRate() != 100) {
				MessageManager::DisplayMessage("ClockRate", std::to_string(EmulationSettings::GetOverclockRate()) + "%");
			}
			EmulationSettings::ClearFlags(EmulationFlags::ForceMaxSpeed);
			return true;
		}
	}

	//Reset battery source to current game if new game failed to load
	BatteryManager::Initialize(FolderUtilities::GetFilename(GetMapperInfo().RomName, false));
	if(_mapper) {
		VideoDecoder::GetInstance()->StartThread();
	}

	MessageManager::DisplayMessage("Error", "CouldNotLoadFile", romFile.GetFileName());
	return false;
}

bool Console::LoadROM(VirtualFile romFile, VirtualFile patchFile)
{
	Console::Pause();
	bool result = Instance->Initialize(romFile, patchFile);
	Console::Resume();
	return result;
}

bool Console::LoadROM(string romName, HashInfo hashInfo)
{
	string currentRomFilepath = Console::GetRomPath().GetFilePath();
	if(!currentRomFilepath.empty()) {
		HashInfo gameHashInfo = Console::GetMapperInfo().Hash;
		if(gameHashInfo.Crc32Hash == hashInfo.Crc32Hash || gameHashInfo.Sha1Hash.compare(hashInfo.Sha1Hash) == 0 || gameHashInfo.PrgChrMd5Hash.compare(hashInfo.PrgChrMd5Hash) == 0) {
			//Current game matches, power cycle game and return
			Instance->PowerCycle();
			return true;
		}
	}

	string match = FindMatchingRom(romName, hashInfo);
	if(!match.empty()) {
		return Console::LoadROM(match);
	}
	return false;
}

string Console::FindMatchingRom(string romName, HashInfo hashInfo)
{
	VirtualFile currentRom = Console::GetRomPath();
	if(currentRom.IsValid() && !Console::GetPatchFile().IsValid()) {
		HashInfo gameHashInfo = Console::GetMapperInfo().Hash;
		if(gameHashInfo.Crc32Hash == hashInfo.Crc32Hash || gameHashInfo.Sha1Hash.compare(hashInfo.Sha1Hash) == 0 || gameHashInfo.PrgChrMd5Hash.compare(hashInfo.PrgChrMd5Hash) == 0) {
			//Current game matches
			return currentRom;
		}
	}

	string lcRomname = romName;
	std::transform(lcRomname.begin(), lcRomname.end(), lcRomname.begin(), ::tolower);
	std::unordered_set<string> validExtensions = { { ".nes", ".fds", "*.unif", "*.unif", "*.nsf", "*.nsfe", "*.7z", "*.zip" } };
	vector<string> romFiles;
	for(string folder : FolderUtilities::GetKnownGameFolders()) {
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, validExtensions, true);
		romFiles.insert(romFiles.end(), files.begin(), files.end());
	}

	string match = RomLoader::FindMatchingRom(romFiles, romName, hashInfo, true);
	if(!match.empty()) {
		return match;
	}

	//Perform slow CRC32 search for ROM
	match = RomLoader::FindMatchingRom(romFiles, romName, hashInfo, false);
	if(!match.empty()) {
		return match;
	}

	return "";
}

VirtualFile Console::GetRomPath()
{
	return static_cast<VirtualFile>(Instance->_romFilepath);
}

VirtualFile Console::GetPatchFile()
{
	return Instance ? (VirtualFile)Instance->_patchFilename : VirtualFile();
}

MapperInfo Console::GetMapperInfo()
{
	if(Instance->_mapper) {
		return Instance->_mapper->GetMapperInfo();
	} else {
		return { };
	}
}

NesModel Console::GetModel()
{
	return Instance->_model;
}

shared_ptr<SystemActionManager> Console::GetSystemActionManager()
{
	return _systemActionManager;
}

void Console::PowerCycle()
{
	if(_initialized && !_romFilepath.empty()) {
		LoadROM(_romFilepath, _patchFilename);
	}
}

void Console::Reset(bool softReset)
{
	if(Instance->_initialized) {
		if(softReset) {
			Instance->_systemActionManager->Reset();
		} else {
			Instance->_systemActionManager->PowerCycle();
		}

		//Resume from code break if needed (otherwise reset doesn't happen right away)
		shared_ptr<Debugger> debugger = Instance->_debugger;
		if(debugger) {
			debugger->Suspend();
			debugger->Run();
		}
	}
}

void Console::ResetComponents(bool softReset)
{
	SoundMixer::StopAudio(true);

	_memoryManager->Reset(softReset);
	if(!EmulationSettings::CheckFlag(EmulationFlags::DisablePpuReset) || !softReset) {
		_ppu->Reset();
	}
	_apu->Reset(softReset);
	_cpu->Reset(softReset, _model);
	_controlManager->Reset(softReset);

	KeyManager::UpdateDevices();

	MessageManager::SendNotification(softReset ? ConsoleNotificationType::GameReset : ConsoleNotificationType::GameLoaded);

	if(softReset) {
		shared_ptr<Debugger> debugger = _debugger;
		if(debugger) {
			debugger->ResetCounters();
			debugger->ProcessEvent(EventType::Reset);
			debugger->Resume();
		}
	}

	_controlManager->UpdateInputState();
}

void Console::Stop(int stopCode)
{
	_stop = true;
	_stopCode = stopCode;

	shared_ptr<Debugger> debugger = _debugger;
	if(debugger) {
		debugger->Suspend();
	}
	_stopLock.Acquire();
	_stopLock.Release();
}

void Console::Pause()
{
	shared_ptr<Debugger> debugger = Console::Instance->_debugger;
	if(debugger) {
		//Make sure debugger resumes if we try to pause the emu, otherwise we will get deadlocked.
		debugger->Suspend();
	}
	Console::Instance->_pauseLock.Acquire();
	//Spin wait until emu pauses
	Console::Instance->_runLock.Acquire();
}

void Console::Resume()
{
	Console::Instance->_runLock.Release();
	Console::Instance->_pauseLock.Release();
	
	shared_ptr<Debugger> debugger = Console::Instance->_debugger;
	if(debugger) {
		//Make sure debugger resumes if we try to pause the emu, otherwise we will get deadlocked.
		debugger->Resume();
	}
}

void Console::RunSingleFrame()
{
	//Used by Libretro
	uint32_t lastFrameNumber = PPU::GetFrameCount();
	_emulationThreadId = std::this_thread::get_id();
	UpdateNesModel(true);

	while(PPU::GetFrameCount() == lastFrameNumber) {
		_cpu->Exec();
	}

	EmulationSettings::DisableOverclocking(_disableOcNextFrame || NsfMapper::GetInstance());
	_disableOcNextFrame = false;

	_systemActionManager->ProcessSystemActions();
	_apu->EndFrame();
}

void Console::Run()
{
	Timer clockTimer;
	Timer lastFrameTimer;
	double targetTime;
	double timeLagData[16] = {};
	int timeLagDataIndex = 0;
	double lastFrameMin = 9999;
	double lastFrameMax = 0;

	uint32_t lastFrameNumber = -1;

#ifndef LIBRETRO
	_autoSaveManager.reset(new AutoSaveManager());
#endif

	_runLock.Acquire();
	_stopLock.Acquire();

	_emulationThreadId = std::this_thread::get_id();

	targetTime = GetFrameDelay();

	VideoDecoder::GetInstance()->StartThread();

	PlatformUtilities::DisableScreensaver();

	UpdateNesModel(true);

	_running = true;

	bool crashed = false;
	try {
		while(true) {
			_cpu->Exec();

			uint32_t currentFrameNumber = PPU::GetFrameCount();
			if(currentFrameNumber != lastFrameNumber) {
				SoundMixer::ProcessEndOfFrame();

				bool displayDebugInfo = EmulationSettings::CheckFlag(EmulationFlags::DisplayDebugInfo);
				if(displayDebugInfo) {
					DisplayDebugInformation(clockTimer, lastFrameTimer, lastFrameMin, lastFrameMax, timeLagData);
				}
				lastFrameTimer.Reset();

				_rewindManager->ProcessEndOfFrame();
				EmulationSettings::DisableOverclocking(_disableOcNextFrame || NsfMapper::GetInstance());
				_disableOcNextFrame = false;

				//Sleep until we're ready to start the next frame
				clockTimer.WaitUntil(targetTime);

				if(!_pauseLock.IsFree()) {
					//Need to temporarely pause the emu (to save/load a state, etc.)
					_runLock.Release();

					//Spin wait until we are allowed to start again
					_pauseLock.WaitForRelease();

					_runLock.Acquire();
				}

				bool paused = EmulationSettings::IsPaused();
				if(paused && !_stop) {
					MessageManager::SendNotification(ConsoleNotificationType::GamePaused);

					//Prevent audio from looping endlessly while game is paused
					SoundMixer::StopAudio();

					_runLock.Release();

					PlatformUtilities::EnableScreensaver();
					PlatformUtilities::RestoreTimerResolution();
					while(paused && !_stop) {
						//Sleep until emulation is resumed
						std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(30));
						paused = EmulationSettings::IsPaused();
					}

					if(EmulationSettings::CheckFlag(EmulationFlags::DebuggerWindowEnabled)) {
						//Prevent pausing when debugger is active
						EmulationSettings::ClearFlags(EmulationFlags::Paused);
					}

					PlatformUtilities::DisableScreensaver();
					_runLock.Acquire();
					MessageManager::SendNotification(ConsoleNotificationType::GameResumed);
					lastFrameTimer.Reset();
				}

				if(EmulationSettings::CheckFlag(EmulationFlags::UseHighResolutionTimer)) {
					PlatformUtilities::EnableHighResolutionTimer();
				} else {
					PlatformUtilities::RestoreTimerResolution();
				}

				_systemActionManager->ProcessSystemActions();

				//Get next target time, and adjust based on whether we are ahead or behind
				double timeLag = EmulationSettings::GetEmulationSpeed() == 0 ? 0 : clockTimer.GetElapsedMS() - targetTime;
				if(displayDebugInfo) {
					timeLagData[timeLagDataIndex] = timeLag;
					timeLagDataIndex = (timeLagDataIndex + 1) & 0x0F;
				}
				UpdateNesModel(true);
				targetTime = GetFrameDelay();

				clockTimer.Reset();
				targetTime -= timeLag;
				if(targetTime < 0) {
					targetTime = 0;
				}

				lastFrameNumber = PPU::GetFrameCount();

				if(_stop) {
					_stop = false;
					break;
				}
			}
		}
	} catch(const std::runtime_error &ex) {
		crashed = true;
		_stopCode = -1;
		MessageManager::DisplayMessage("Error", "GameCrash", ex.what());
	}

	_running = false;

	MessageManager::SendNotification(ConsoleNotificationType::BeforeEmulationStop);

	if(!crashed) {
		SaveStateManager::SaveRecentGame(GetMapperInfo().RomName, _romFilepath, _patchFilename);
	}

	_rewindManager.reset();
	StopRecordingHdPack();
	SoundMixer::StopAudio();
	MovieManager::Stop();
	SoundMixer::StopRecording();
	PlatformUtilities::EnableScreensaver();
	PlatformUtilities::RestoreTimerResolution();

#ifndef LIBRETRO
	_autoSaveManager.reset();
#endif

	VideoDecoder::GetInstance()->StopThread();

	EmulationSettings::ClearFlags(EmulationFlags::Paused);
	EmulationSettings::ClearFlags(EmulationFlags::ForceMaxSpeed);

	_initialized = false;

	if(!_romFilepath.empty() && _mapper) {
		//Ensure we save any battery file before unloading anything
		SaveBatteries();
	}

	_romFilepath = "";
	_mapper.reset();
	_ppu.reset();
	_cpu.reset();
	_memoryManager.reset();
	_controlManager.reset();

	_hdPackBuilder.reset();
	_hdData.reset();

	_debugger.reset();

	_stopLock.Release();
	_runLock.Release();

	_emulationThreadId = std::thread::id();

	MessageManager::SendNotification(ConsoleNotificationType::GameStopped);
	MessageManager::SendNotification(ConsoleNotificationType::EmulationStopped);
}

bool Console::IsRunning()
{
	return !Instance->_stopLock.IsFree() && Instance->_running;
}

bool Console::IsPaused()
{
	return _runLock.IsFree() || !_pauseLock.IsFree() || !_running;
}

void Console::UpdateNesModel(bool sendNotification)
{
	bool configChanged = false;
	if(EmulationSettings::NeedControllerUpdate()) {
		_controlManager->UpdateControlDevices();
		configChanged = true;
	}

	NesModel model = EmulationSettings::GetNesModel();
	if(model == NesModel::Auto) {
		switch(_mapper->GetMapperInfo().System) {
			case GameSystem::NesPal: model = NesModel::PAL; break;
			case GameSystem::Dendy: model = NesModel::Dendy; break;
			default: model = NesModel::NTSC; break;
		}
	}
	if(_model != model) {
		_model = model;
		configChanged = true;

		if(sendNotification) {
			MessageManager::DisplayMessage("Region", model == NesModel::PAL ? "PAL" : (model == NesModel::Dendy ? "Dendy" : "NTSC"));
		}
	}

	_mapper->SetNesModel(model);
	_ppu->SetNesModel(model);
	_apu->SetNesModel(model);

	if(configChanged && sendNotification) {
		MessageManager::SendNotification(ConsoleNotificationType::ConfigChanged);
	}
}

double Console::GetFrameDelay()
{
	uint32_t emulationSpeed = EmulationSettings::GetEmulationSpeed();
	double frameDelay;
	if(emulationSpeed == 0) {
		frameDelay = 0;
	} else {
		//60.1fps (NTSC), 50.01fps (PAL/Dendy)
		switch(_model) {
			default:
			case NesModel::NTSC: frameDelay = EmulationSettings::CheckFlag(EmulationFlags::IntegerFpsMode) ? 16.6666666666666666667 : 16.63926405550947; break;
			case NesModel::PAL:
			case NesModel::Dendy: frameDelay = EmulationSettings::CheckFlag(EmulationFlags::IntegerFpsMode) ? 20 : 19.99720920217466; break;
		}
		frameDelay /= (double)emulationSpeed / 100.0;
	}

	return frameDelay;
}

void Console::SaveState(ostream &saveStream)
{
	if(Instance->_initialized) {
		Instance->_cpu->SaveSnapshot(&saveStream);
		Instance->_ppu->SaveSnapshot(&saveStream);
		Instance->_memoryManager->SaveSnapshot(&saveStream);
		Instance->_apu->SaveSnapshot(&saveStream);
		Instance->_controlManager->SaveSnapshot(&saveStream);
		Instance->_mapper->SaveSnapshot(&saveStream);
		if(Instance->_hdAudioDevice) {
			Instance->_hdAudioDevice->SaveSnapshot(&saveStream);
		} else {
			Snapshotable::WriteEmptyBlock(&saveStream);
		}
	}
}

void Console::LoadState(istream &loadStream, uint32_t stateVersion)
{
	if(Instance->_initialized) {
		Instance->_cpu->LoadSnapshot(&loadStream, stateVersion);
		Instance->_ppu->LoadSnapshot(&loadStream, stateVersion);
		Instance->_memoryManager->LoadSnapshot(&loadStream, stateVersion);
		Instance->_apu->LoadSnapshot(&loadStream, stateVersion);
		Instance->_controlManager->LoadSnapshot(&loadStream, stateVersion);
		Instance->_mapper->LoadSnapshot(&loadStream, stateVersion);
		if(Instance->_hdAudioDevice) {
			Instance->_hdAudioDevice->LoadSnapshot(&loadStream, stateVersion);
		} else {
			Snapshotable::SkipBlock(&loadStream);
		}
		
		shared_ptr<Debugger> debugger = Instance->_debugger;
		if(debugger) {
			debugger->ResetCounters();
		}

		MessageManager::SendNotification(ConsoleNotificationType::StateLoaded);
	}
}

void Console::LoadState(uint8_t *buffer, uint32_t bufferSize)
{
	//Send any unprocessed sound to the SoundMixer - needed for rewind
	Instance->_apu->EndFrame();

	stringstream stream;
	stream.write((char*)buffer, bufferSize);
	stream.seekg(0, ios::beg);
	LoadState(stream);
}

std::shared_ptr<Debugger> Console::GetDebugger(bool autoStart)
{
	shared_ptr<Debugger> debugger = _debugger;
	if(!debugger && autoStart) {
		//Lock to make sure we don't try to start debuggers in 2 separate threads at once
		auto lock = _debuggerLock.AcquireSafe();
		debugger = _debugger;
		if(!debugger) {
			debugger.reset(new Debugger(Console::Instance, _cpu, _ppu, _apu, _memoryManager, _mapper));
			_debugger = debugger;
		}
	}
	return debugger;
}

void Console::StopDebugger()
{
	_debugger.reset();
}

std::thread::id Console::GetEmulationThreadId()
{
	return Instance->_emulationThreadId;
}

uint32_t Console::GetLagCounter()
{
	return Instance->_controlManager->GetLagCounter();
}

void Console::ResetLagCounter()
{
	Console::Pause();
	Instance->_controlManager->ResetLagCounter();
	Console::Reset();
}

bool Console::IsDebuggerAttached()
{
	return (bool)Instance->_debugger;
}

void Console::SetNextFrameOverclockStatus(bool disabled)
{
	Instance->_disableOcNextFrame = disabled;
}

int32_t Console::GetStopCode()
{
	return _stopCode;
}

HdPackData* Console::GetHdData()
{
	return Instance->_hdData.get();
}

bool Console::IsHdPpu()
{
	return Instance->_hdData && std::dynamic_pointer_cast<HdPpu>(Instance->_ppu) != nullptr;
}

void Console::LoadHdPack(VirtualFile &romFile, VirtualFile &patchFile)
{
	_hdData.reset();
	_hdAudioDevice.reset();
	if(EmulationSettings::CheckFlag(EmulationFlags::UseHdPacks)) {
		_hdData.reset(new HdPackData());
		if(!HdPackLoader::LoadHdNesPack(romFile, *_hdData.get())) {
			_hdData.reset();
		} else {
			auto result = _hdData->PatchesByHash.find(romFile.GetSha1Hash());
			if(result != _hdData->PatchesByHash.end()) {
				patchFile = result->second;
			}
		}
	}
}

void Console::StartRecordingHdPack(string saveFolder, ScaleFilterType filterType, uint32_t scale, uint32_t flags, uint32_t chrRamBankSize)
{
	Console::Pause();
	std::stringstream saveState;
	Instance->SaveState(saveState);
	
	Instance->_hdPackBuilder.reset();
	Instance->_hdPackBuilder.reset(new HdPackBuilder(saveFolder, filterType, scale, flags, chrRamBankSize, !Instance->_mapper->HasChrRom()));

	Instance->_memoryManager->UnregisterIODevice(Instance->_ppu.get());
	Instance->_ppu.reset();
	Instance->_ppu.reset(new HdBuilderPpu(Instance->_mapper.get(), Instance->_controlManager.get(), Instance->_hdPackBuilder.get(), chrRamBankSize));
	Instance->_memoryManager->RegisterIODevice(Instance->_ppu.get());

	Instance->LoadState(saveState);
	Console::Resume();
}

void Console::StopRecordingHdPack()
{
	if(Instance->_hdPackBuilder) {
		Console::Pause();
		std::stringstream saveState;
		Instance->SaveState(saveState);

		Instance->_memoryManager->UnregisterIODevice(Instance->_ppu.get());
		Instance->_ppu.reset();
		Instance->_ppu.reset(new PPU(Instance->_mapper.get(), Instance->_controlManager.get()));
		Instance->_memoryManager->RegisterIODevice(Instance->_ppu.get());

		Instance->_hdPackBuilder.reset();

		Instance->LoadState(saveState);
		Console::Resume();
	}
}

bool Console::UpdateHdPackMode()
{
	//Switch back and forth between HD PPU and regular PPU as needed
	Console::Pause();

	VirtualFile romFile = _romFilepath;
	VirtualFile patchFile = _patchFilename;
	LoadHdPack(romFile, patchFile);

	bool isHdPackLoaded = std::dynamic_pointer_cast<HdPpu>(_ppu) != nullptr;
	bool hdPackFound = _hdData && (!_hdData->Tiles.empty() || !_hdData->Backgrounds.empty());

	bool modeChanged = false;
	if(isHdPackLoaded != hdPackFound) {
		std::stringstream saveState;
		Instance->SaveState(saveState);

		Instance->_memoryManager->UnregisterIODevice(Instance->_ppu.get());
		Instance->_ppu.reset();
		if(_hdData && (!_hdData->Tiles.empty() || !_hdData->Backgrounds.empty())) {
			_ppu.reset(new HdPpu(_mapper.get(), _controlManager.get(), _hdData.get()));
		} else if(std::dynamic_pointer_cast<NsfMapper>(_mapper)) {
			//Disable most of the PPU for NSFs
			_ppu.reset(new NsfPpu(_mapper.get(), _controlManager.get()));
		} else {
			_ppu.reset(new PPU(_mapper.get(), _controlManager.get()));
		}
		Instance->_memoryManager->RegisterIODevice(Instance->_ppu.get());

		Instance->LoadState(saveState);
		modeChanged = true;
	}

	Console::Resume();
	
	return modeChanged;
}

ConsoleFeatures Console::GetAvailableFeatures()
{
	ConsoleFeatures features = ConsoleFeatures::None;
	shared_ptr<BaseMapper> mapper = _mapper;
	shared_ptr<ControlManager> controlManager = _controlManager;
	if(mapper && controlManager) {
		features = (ConsoleFeatures)((int)features | (int)mapper->GetAvailableFeatures());

		if(dynamic_cast<VsControlManager*>(controlManager.get())) {
			features = (ConsoleFeatures)((int)features | (int)ConsoleFeatures::VsSystem);
		}

		if(std::dynamic_pointer_cast<IBarcodeReader>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort))) {
			features = (ConsoleFeatures)((int)features | (int)ConsoleFeatures::BarcodeReader);
		}

		if(std::dynamic_pointer_cast<FamilyBasicDataRecorder>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort2))) {
			features = (ConsoleFeatures)((int)features | (int)ConsoleFeatures::TapeRecorder);
		}
	}
	return features;
}

void Console::InputBarcode(uint64_t barcode, uint32_t digitCount)
{
	shared_ptr<BaseMapper> mapper = _mapper;
	shared_ptr<ControlManager> controlManager = _controlManager;

	if(mapper) {
		shared_ptr<IBarcodeReader> barcodeReader = std::dynamic_pointer_cast<IBarcodeReader>(mapper->GetMapperControlDevice());
		if(barcodeReader) {
			barcodeReader->InputBarcode(barcode, digitCount);
		}
	}

	if(controlManager) {
		shared_ptr<IBarcodeReader> barcodeReader = std::dynamic_pointer_cast<IBarcodeReader>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort));
		if(barcodeReader) {
			barcodeReader->InputBarcode(barcode, digitCount);
		}
	}
}

void Console::LoadTapeFile(string filepath)
{
	shared_ptr<ControlManager> controlManager = _controlManager;
	if(controlManager) {
		shared_ptr<FamilyBasicDataRecorder> dataRecorder = std::dynamic_pointer_cast<FamilyBasicDataRecorder>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort2));
		if(dataRecorder) {
			Console::Pause();
			dataRecorder->LoadFromFile(filepath);
			Console::Resume();
		}
	}
}

void Console::StartRecordingTapeFile(string filepath)
{
	shared_ptr<ControlManager> controlManager = _controlManager;
	if(controlManager) {
		shared_ptr<FamilyBasicDataRecorder> dataRecorder = std::dynamic_pointer_cast<FamilyBasicDataRecorder>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort2));
		if(dataRecorder) {
			Console::Pause();
			dataRecorder->StartRecording(filepath);
			Console::Resume();
		}
	}
}

void Console::StopRecordingTapeFile()
{
	shared_ptr<ControlManager> controlManager = _controlManager;
	if(controlManager) {
		shared_ptr<FamilyBasicDataRecorder> dataRecorder = std::dynamic_pointer_cast<FamilyBasicDataRecorder>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort2));
		if(dataRecorder) {
			Console::Pause();
			dataRecorder->StopRecording();
			Console::Resume();
		}
	}
}

bool Console::IsRecordingTapeFile()
{
	shared_ptr<ControlManager> controlManager = _controlManager;
	if(controlManager) {
		shared_ptr<FamilyBasicDataRecorder> dataRecorder = std::dynamic_pointer_cast<FamilyBasicDataRecorder>(controlManager->GetControlDevice(BaseControlDevice::ExpDevicePort2));
		if(dataRecorder) {
			return dataRecorder->IsRecording();
		}
	}

	return false;
}

uint8_t* Console::GetRamBuffer(DebugMemoryType memoryType, uint32_t &size, int32_t &startAddr)
{
	//Only used by libretro port for achievements - should not be used by anything else.
	switch(memoryType) {
		default: break;

		case DebugMemoryType::InternalRam:
			size = MemoryManager::InternalRAMSize;
			startAddr = 0;
			return _memoryManager->GetInternalRAM();

		case DebugMemoryType::SaveRam:
			size = _mapper->GetMemorySize(DebugMemoryType::SaveRam);
			startAddr = _mapper->FromAbsoluteAddress(0, AddressType::SaveRam);
			return _mapper->GetSaveRam();

		case DebugMemoryType::WorkRam:
			size = _mapper->GetMemorySize(DebugMemoryType::WorkRam);
			startAddr = _mapper->FromAbsoluteAddress(0, AddressType::WorkRam);
			return _mapper->GetWorkRam();
	}

	throw std::runtime_error("unsupported memory type");
}

void Console::DisplayDebugInformation(Timer &clockTimer, Timer &lastFrameTimer, double &lastFrameMin, double &lastFrameMax, double *timeLagData)
{
	AudioStatistics stats = SoundMixer::GetStatistics();
	DebugHud* hud = DebugHud::GetInstance();
	hud->DrawRectangle(8, 8, 115, 40, 0x40000000, true, 1);
	hud->DrawRectangle(8, 8, 115, 40, 0xFFFFFF, false, 1);

	hud->DrawString(10, 10, "Audio Stats", 0xFFFFFF, 0xFF000000, 1);
	hud->DrawString(10, 21, "Latency: ", 0xFFFFFF, 0xFF000000, 1);
	
	int color = (stats.AverageLatency > 0 && std::abs(stats.AverageLatency - EmulationSettings::GetAudioLatency()) > 3) ? 0xFF0000 : 0xFFFFFF;
	std::stringstream ss;
	ss << std::fixed << std::setprecision(2) << stats.AverageLatency << " ms";
	hud->DrawString(54, 21, ss.str(), color, 0xFF000000, 1);

	hud->DrawString(10, 30, "Underruns: " + std::to_string(stats.BufferUnderrunEventCount), 0xFFFFFF, 0xFF000000, 1);
	hud->DrawString(10, 39, "Buffer Size: " + std::to_string(stats.BufferSize / 1024) + "kb", 0xFFFFFF, 0xFF000000, 1);

	hud->DrawRectangle(136, 8, 115, 58, 0x40000000, true, 1);
	hud->DrawRectangle(136, 8, 115, 58, 0xFFFFFF, false, 1);
	hud->DrawString(138, 10, "Video Stats", 0xFFFFFF, 0xFF000000, 1);

	ss = std::stringstream();
	ss << "Exec Time: " << std::fixed << std::setprecision(2) << clockTimer.GetElapsedMS() << " ms";
	hud->DrawString(138, 21, ss.str(), 0xFFFFFF, 0xFF000000, 1);

	double avgTimeLag = 0;
	for(int i = 0; i < 16; i++) {
		avgTimeLag += timeLagData[i];
	}
	avgTimeLag /= 16;

	ss = std::stringstream();
	ss << "Time Gap: " << std::fixed << std::setprecision(2) << avgTimeLag << " ms";
	hud->DrawString(138, 30, ss.str(), 0xFFFFFF, 0xFF000000, 1);

	double lastFrame = lastFrameTimer.GetElapsedMS();

	ss = std::stringstream();
	ss << "Last Frame: " << std::fixed << std::setprecision(2) << lastFrame << " ms";
	hud->DrawString(138, 39, ss.str(), 0xFFFFFF, 0xFF000000, 1);

	if(PPU::GetFrameCount() > 60) {
		lastFrameMin = (std::min)(lastFrame, lastFrameMin);
		lastFrameMax = (std::max)(lastFrame, lastFrameMax);
	} else {
		lastFrameMin = 9999;
		lastFrameMax = 0;
	}

	ss = std::stringstream();
	ss << "Min Delay: " << std::fixed << std::setprecision(2) << ((lastFrameMin < 9999) ? lastFrameMin : 0.0) << " ms";
	hud->DrawString(138, 48, ss.str(), 0xFFFFFF, 0xFF000000, 1);

	ss = std::stringstream();
	ss << "Max Delay: " << std::fixed << std::setprecision(2) << lastFrameMax << " ms";
	hud->DrawString(138, 57, ss.str(), 0xFFFFFF, 0xFF000000, 1);
}