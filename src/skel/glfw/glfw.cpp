#if defined RW_GL3 && !defined LIBRW_SDL2

#ifdef _WIN32
#include <shlobj.h>
#include <basetsd.h>
#include <mmsystem.h>
#include <regstr.h>
#include <shellapi.h>
#include <windowsx.h>

DWORD _dwOperatingSystemVersion;
#include "resource.h"
#else
long _dwOperatingSystemVersion;
#ifndef __APPLE__
#include <sys/sysinfo.h>
#else
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stddef.h>
#endif

#include "common.h"
#if (defined(_MSC_VER))
#include <tchar.h>
#endif /* (defined(_MSC_VER)) */
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif
#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "crossplatform.h"

#include "main.h"
#include "FileMgr.h"
#include "Text.h"
#include "Pad.h"
#include "Timer.h"
#include "DMAudio.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Game.h"
#include "PCSave.h"
#include "MemoryCard.h"
#include "Sprite2d.h"
#include "AnimViewer.h"
#include "Font.h"
#include "MemoryMgr.h"

// We found out that GLFW's keyboard input handling is still pretty delayed/not stable, so now we fetch input from X11 directly on Linux.
// Emscripten's GLFW port has no native X11 (or any native platform) backend, so it must always use GLFW's own key callback.
#if !defined _WIN32 && !defined __APPLE__ && !defined __SWITCH__ && !defined __EMSCRIPTEN__ // && !defined WAYLAND
#define GET_KEYBOARD_INPUT_FROM_X11
#endif

#ifdef GET_KEYBOARD_INPUT_FROM_X11
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#endif

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#define MAX_SUBSYSTEMS		(16)

rw::EngineOpenParams openParams;

static RwBool		  ForegroundApp = TRUE;
static RwBool		  WindowIconified = FALSE;
static RwBool		  WindowFocused = TRUE;

static RwBool		  RwInitialised = FALSE;

static RwSubSystemInfo GsubSysInfo[MAX_SUBSYSTEMS];
static RwInt32		GnumSubSystems = 0;
static RwInt32		GcurSel = 0, GcurSelVM = 0;

static RwBool useDefault;

// What is that for anyway?
#ifndef IMPROVED_VIDEOMODE
static RwBool defaultFullscreenRes = TRUE;
#else
static RwBool defaultFullscreenRes = FALSE;
static RwInt32 bestWndMode = -1;
#endif

static psGlobalType PsGlobal;


#define PSGLOBAL(var) (((psGlobalType *)(RsGlobal.ps))->var)

size_t _dwMemAvailPhys;
RwUInt32 gGameState;

#ifdef __EMSCRIPTEN__
// Phase 9 (docs/FIRST_BOOT.md): a pointer to the capture-less `tick` lambda
// registered with emscripten_set_main_loop() further down in main() -- stashed
// here so re3_PumpTick() (below) can invoke it directly. See that export's
// comment for why this exists.
static void (*g_re3TickFn)() = nullptr;

// Debug-only: last-observed inputs/result of the GS_PLAYING_GAME frame-limiter
// check below (main.cpp's Idle() is unreachable at all unless this passes) --
// exposed so we can tell directly whether it's the frame limiter permanently
// blocking Idle() from running, rather than inferring it from frozen FPS.
static float g_re3LastFrameLimiterMs = -1.0f;
static int g_re3LastFrameLimiterMaxFPS = -1;
static bool g_re3LastFrameLimiterEnabled = false;
static bool g_re3LastFrameLimiterWillCall = false;

// Explicit browser-side FPS cap (docs/WASM_GAMEPLAY_STABILITY.md). A real
// browser tab's rAF-driven tick() was observed calling Idle() 700-800
// times/second: CMenuManager::m_PrefsFrameLimiter (a native, user-facing menu
// setting) defaults off, and even when on, RsGlobal.maxFPS is a native
// display-refresh-rate preference this port has no business depending on --
// "browser vsync if available" was explicitly not what was wanted here. This
// fixed budget replaces that native prefs-driven check, at the two call sites
// below (GS_PLAYING_GAME's Idle() and GS_FRONTEND's FrontendIdle()).
static const double RE3_EMSCRIPTEN_FRAME_BUDGET_MS = 1000.0 / 50.0; // 20.0ms

// A first version of this gate compared CTimer's own "time since the last
// CTimer::Update()" against the budget and, when it passed, let CTimer::Update()
// (called at the top of Idle()/FrontendIdle()) reset that reference to
// whatever "now" happened to be -- including however much the check had
// already overshot the budget by by the time it got noticed. Measured live at
// ~48fps instead of ~59fps as a result: this environment's tick() itself
// fires roughly every ~4ms (not real vsync -- see re3_SetDebugTiming's
// comment on non-composited tabs), so each check could overshoot by up to
// that much, and resetting to "now" every time let that overshoot compound
// into a steady-state bias rather than being paid back.
// re3EmscriptenFrameDue() fixes this by scheduling the *next* frame at a
// fixed `+= budget` from the last one instead of `= now`, so any one frame's
// overshoot shortens the wait before the next one -- the average rate
// converges on the true target regardless of how granular the underlying
// tick() polling is. It shares its own clock (RsTimer(), the same real-time
// source CTimer itself is built on) rather than reusing CTimer's
// Update()-driven reference, since that reference is only meaningful right
// after Idle()/FrontendIdle() actually ran.
static double g_re3NextFrameTimeMs = 0.0;

static bool
re3EmscriptenFrameDue()
{
	double nowMs = (double)RsTimer();
	if (g_re3NextFrameTimeMs <= 0.0)
		g_re3NextFrameTimeMs = nowMs;
	if (nowMs < g_re3NextFrameTimeMs)
		return false;
	g_re3NextFrameTimeMs += RE3_EMSCRIPTEN_FRAME_BUDGET_MS;
	// Long stall (backgrounded tab, suspended rAF): don't let the schedule
	// fall so far behind that it tries to burst-fire makeup frames once
	// ticking resumes -- just resume a fresh one-budget cadence from now.
	if (g_re3NextFrameTimeMs < nowMs - RE3_EMSCRIPTEN_FRAME_BUDGET_MS)
		g_re3NextFrameTimeMs = nowMs;
	return true;
}

// Raw tick() invocation counter -- unconditional, counts every single call
// regardless of game state or the frame cap above, to answer directly "how
// often is the browser actually calling our registered main-loop callback"
// (rAF is supposed to be display-refresh-paced and therefore never anywhere
// near 300-800/sec, but this environment's tab compositing has been unusual
// before -- see re3_SetDebugTiming's comment -- so this is measured, not
// assumed).
static unsigned int g_re3TickCallCount = 0;
#endif

#ifdef DETECT_JOYSTICK_MENU
char gSelectedJoystickName[128] = "";
#endif

/*
 *****************************************************************************
 */
void _psCreateFolder(const char *path)
{
#ifdef _WIN32
	HANDLE hfle = CreateFile(path, GENERIC_READ, 
									FILE_SHARE_READ,
									nil,
									OPEN_EXISTING,
									FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_NORMAL,
									nil);

	if ( hfle == INVALID_HANDLE_VALUE )
		CreateDirectory(path, nil);
	else
		CloseHandle(hfle);
#else
	struct stat info;
	char fullpath[PATH_MAX];
	realpath(path, fullpath);

	if (lstat(fullpath, &info) != 0) {
		if (errno == ENOENT || (errno != EACCES && !S_ISDIR(info.st_mode))) {
			mkdir(fullpath, 0755);
		}
	}
#endif
}

/*
 *****************************************************************************
 */
const char *_psGetUserFilesFolder()
{
#if defined USE_MY_DOCUMENTS && defined _WIN32
	HKEY hKey = NULL;

	static CHAR szUserFiles[256];

	if ( RegOpenKeyEx(HKEY_CURRENT_USER,
						REGSTR_PATH_SPECIAL_FOLDERS,
						REG_OPTION_RESERVED,
						KEY_READ,
						&hKey) == ERROR_SUCCESS )
	{
		DWORD KeyType;
		DWORD KeycbData = sizeof(szUserFiles);
		if ( RegQueryValueEx(hKey,
							"Personal",
							NULL,
							&KeyType,
							(LPBYTE)szUserFiles,
							&KeycbData) == ERROR_SUCCESS )
		{
			RegCloseKey(hKey);
			strcat(szUserFiles, "\\GTA3 User Files");
			_psCreateFolder(szUserFiles);
			return szUserFiles;
		}	

		RegCloseKey(hKey);		
	}
	
	strcpy(szUserFiles, "data");
	return szUserFiles;
#else
	static char szUserFiles[256];
	strcpy(szUserFiles, "userfiles");
	_psCreateFolder(szUserFiles);
	return szUserFiles;
#endif
}

/*
 *****************************************************************************
 */
RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	if ( !RwCameraBeginUpdate(Scene.camera) )
	{
		ForegroundApp = FALSE;
		RsEventHandler(rsACTIVATE, (void *)FALSE);
		return FALSE;
	}
	
	return TRUE;
}

/*
 *****************************************************************************
 */
void
psCameraShowRaster(RwCamera *camera)
{
	if (CMenuManager::m_PrefsVsync)
		RwCameraShowRaster(camera, PSGLOBAL(window), rwRASTERFLIPWAITVSYNC);
	else
		RwCameraShowRaster(camera, PSGLOBAL(window), rwRASTERFLIPDONTWAIT);

	return;
}

/*
 *****************************************************************************
 */
RwImage *
psGrabScreen(RwCamera *pCamera)
{
#ifndef LIBRW
	RwRaster *pRaster = RwCameraGetRaster(pCamera);
	if (RwImage *pImage = RwImageCreate(pRaster->width, pRaster->height, 32)) {
		RwImageAllocatePixels(pImage);
		RwImageSetFromRaster(pImage, pRaster);
		return pImage;
	}
#else
	rw::Image *image = RwCameraGetRaster(pCamera)->toImage();
	image->removeMask();
	if(image)
		return image;
#endif
	return nil;
}

/*
 *****************************************************************************
 */
#ifdef _WIN32
#pragma comment( lib, "Winmm.lib" ) // Needed for time
RwUInt32
psTimer(void)
{
	RwUInt32 time;

	TIMECAPS TimeCaps;
	
	timeGetDevCaps(&TimeCaps, sizeof(TIMECAPS));
	
	timeBeginPeriod(TimeCaps.wPeriodMin);
	
	time = (RwUInt32) timeGetTime();

	timeEndPeriod(TimeCaps.wPeriodMin);
	
	return time;
}
#else
double
psTimer(void)
{
#ifdef __EMSCRIPTEN__
	// clock_gettime(CLOCK_MONOTONIC_RAW, ...) does not track real elapsed time
	// under this Emscripten build -- confirmed by direct measurement: two calls
	// spaced ~32 real seconds apart (per JS performance.now()) returned the
	// bit-identical value (0.000098ms) both times, i.e. it never advances at
	// all. Since CTimer::GetCurrentTimeInCycles() (Timer.cpp) is built entirely
	// on this function's return value, a frozen psTimer() means CTimer never
	// sees any elapsed time: the GS_PLAYING_GAME frame-limiter gate in this
	// file's tick() (1000/maxFPS < ms, where ms comes from here) is then always
	// false and Idle() -- and with it CGame::Process(), script processing,
	// DoFade()'s fade-in timer, and rendering -- almost never runs once gameplay
	// starts, even though the state machine itself reaches GS_PLAYING_GAME and
	// the screen stays permanently faded to black. emscripten_get_now() wraps
	// performance.now(), already proven correct elsewhere in this file (the
	// GAME STATE tracer above), and returns the same "milliseconds as a double"
	// unit psTimer() promises, so it's a direct drop-in replacement.
	return emscripten_get_now();
#else
	struct timespec start;
#if defined(CLOCK_MONOTONIC_RAW)
	clock_gettime(CLOCK_MONOTONIC_RAW, &start);
#elif defined(CLOCK_MONOTONIC_FAST)
	clock_gettime(CLOCK_MONOTONIC_FAST, &start);
#else
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	return start.tv_sec * 1000.0 + start.tv_nsec/1000000.0;
#endif
}
#endif


/*
 *****************************************************************************
 */
void
psMouseSetPos(RwV2d *pos)
{
	glfwSetCursorPos(PSGLOBAL(window), pos->x, pos->y);
	
	PSGLOBAL(lastMousePos.x) = (RwInt32)pos->x;

	PSGLOBAL(lastMousePos.y) = (RwInt32)pos->y;

	return;
}

/*
 *****************************************************************************
 */
RwMemoryFunctions*
psGetMemoryFunctions(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return &memFuncs;
#else
	return nil;
#endif
}

/*
 *****************************************************************************
 */
RwBool
psInstallFileSystem(void)
{
	return (TRUE);
}


/*
 *****************************************************************************
 */
RwBool
psNativeTextureSupport(void)
{
	return true;
}

/*
 *****************************************************************************
 */
#ifdef UNDER_CE
#define CMDSTR	LPWSTR
#else
#define CMDSTR	LPSTR
#endif

/*
 *****************************************************************************
 */
RwBool
psInitialize(void)
{
	PsGlobal.lastMousePos.x = PsGlobal.lastMousePos.y = 0.0f;

	RsGlobal.ps = &PsGlobal;
	
	PsGlobal.fullScreen = FALSE;
	PsGlobal.cursorIsInWindow = FALSE;
	WindowFocused = TRUE;
	WindowIconified = FALSE;
	
	PsGlobal.joy1id	= -1;
	PsGlobal.joy2id	= -1;

	CFileMgr::Initialise();
	
#ifdef PS2_MENU
	CPad::Initialise();
	CPad::GetPad(0)->Mode = 0;

	CGame::frenchGame = false;
	CGame::germanGame = false;
	CGame::nastyGame = true;
	CMenuManager::m_PrefsAllowNastyGame = true;

#ifndef _WIN32
	// Mandatory for Linux(Unix? Posix?) to set lang. to environment lang.
	setlocale(LC_ALL, "");	

	char *systemLang, *keyboardLang;

	systemLang = setlocale (LC_ALL, NULL);
	keyboardLang = setlocale (LC_CTYPE, NULL);
	
	short lang;
	lang = !strncmp(systemLang, "fr_",3) ? LANG_FRENCH :
					!strncmp(systemLang, "de_",3) ? LANG_GERMAN :
					!strncmp(systemLang, "en_",3) ? LANG_ENGLISH :
					!strncmp(systemLang, "it_",3) ? LANG_ITALIAN :
					!strncmp(systemLang, "es_",3) ? LANG_SPANISH :
					LANG_OTHER;
#else
	WORD lang	= PRIMARYLANGID(GetSystemDefaultLCID());
#endif

	if ( lang  == LANG_ITALIAN )
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_ITALIAN;
	else if ( lang  == LANG_SPANISH )
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_SPANISH;
	else if ( lang  == LANG_GERMAN )
	{
		CGame::germanGame = true;
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_GERMAN;
	}
	else if ( lang  == LANG_FRENCH )
	{
		CGame::frenchGame = true;
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_FRENCH;
	}
	else
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;

	FrontEndMenuManager.InitialiseMenuContentsAfterLoadingGame();

	TheMemoryCard.Init();
#else
	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());
	
	InitialiseLanguage();

#if GTA_VERSION < GTA3_PC_11
	FrontEndMenuManager.LoadSettings();
#endif

#endif
	
	gGameState = GS_START_UP;
	TRACE("gGameState = GS_START_UP");
#ifdef _WIN32
	OSVERSIONINFO verInfo;
	verInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	
	GetVersionEx(&verInfo);
	
	_dwOperatingSystemVersion = OS_WIN95;
	
	if ( verInfo.dwPlatformId == VER_PLATFORM_WIN32_NT )
	{
		if ( verInfo.dwMajorVersion == 4 )
		{
			debug("Operating System is WinNT\n");
			_dwOperatingSystemVersion = OS_WINNT;
		}
		else if ( verInfo.dwMajorVersion == 5 )
		{
			debug("Operating System is Win2000\n");
			_dwOperatingSystemVersion = OS_WIN2000;
		}
		else if ( verInfo.dwMajorVersion > 5 )
		{
			debug("Operating System is WinXP or greater\n");
			_dwOperatingSystemVersion = OS_WINXP;
		}
	}
	else if ( verInfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS )
	{
		if ( verInfo.dwMajorVersion > 4 || verInfo.dwMajorVersion == 4 && verInfo.dwMinorVersion != 0 )
		{
			debug("Operating System is Win98\n");
			_dwOperatingSystemVersion = OS_WIN98;
		}
		else
		{
			debug("Operating System is Win95\n");
			_dwOperatingSystemVersion = OS_WIN95;
		}
	}
#else
	_dwOperatingSystemVersion = OS_WINXP; // To fool other classes
#endif

	
#ifndef PS2_MENU

#if GTA_VERSION >= GTA3_PC_11
	FrontEndMenuManager.LoadSettings();
#endif

#endif


#ifdef _WIN32
	MEMORYSTATUS memstats;
	GlobalMemoryStatus(&memstats);

	_dwMemAvailPhys = memstats.dwAvailPhys;

	debug("Physical memory size %u\n", memstats.dwTotalPhys);
	debug("Available physical memory %u\n", memstats.dwAvailPhys);
#elif defined (__APPLE__)
	uint64_t size = 0;
	uint64_t page_size = 0;
	size_t uint64_len = sizeof(uint64_t);
	size_t ull_len = sizeof(unsigned long long);
	sysctl((int[]){CTL_HW, HW_PAGESIZE}, 2, &page_size, &ull_len, NULL, 0);
	sysctl((int[]){CTL_HW, HW_MEMSIZE}, 2, &size, &uint64_len, NULL, 0);
	vm_statistics_data_t vm_stat;
	mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
	host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&vm_stat, &count);
	_dwMemAvailPhys = (uint64_t)(vm_stat.free_count * page_size);
	debug("Physical memory size %llu\n", _dwMemAvailPhys);
	debug("Available physical memory %llu\n", size);
#elif defined(__EMSCRIPTEN__)
	// Emscripten's libc doesn't implement sysinfo(); the browser doesn't expose real
	// physical/available memory either, so report a conservative fixed budget for the
	// streaming system's memory calculations (see CStreaming::ms_memoryAvailable).
	_dwMemAvailPhys = 256 * 1024 * 1024;
	debug("Available physical memory %u (assumed, Emscripten)\n", _dwMemAvailPhys);
#else
 	struct sysinfo systemInfo;
	sysinfo(&systemInfo);
	_dwMemAvailPhys = systemInfo.freeram;
	debug("Physical memory size %u\n", systemInfo.totalram);
	debug("Available physical memory %u\n", systemInfo.freeram);
#endif
  
  TheText.Unload();

	return TRUE;
}


/*
 *****************************************************************************
 */
void
psTerminate(void)
{
	return;
}

/*
 *****************************************************************************
 */
static RwChar **_VMList;

RwInt32 _psGetNumVideModes()
{
	return RwEngineGetNumVideoModes();
}

/*
 *****************************************************************************
 */
RwBool _psFreeVideoModeList()
{
	RwInt32 numModes;
	RwInt32 i;
	
	numModes = _psGetNumVideModes();
	
	if ( _VMList == nil )
		return TRUE;
	
	for ( i = 0; i < numModes; i++ )
	{
		RwFree(_VMList[i]);
	}
	
	RwFree(_VMList);
	
	_VMList = nil;
	
	return TRUE;
}
							
/*
 *****************************************************************************
 */							
RwChar **_psGetVideoModeList()
{
	RwInt32 numModes;
	RwInt32 i;
	
	if ( _VMList != nil )
	{
		return _VMList;
	}
	
	numModes = RwEngineGetNumVideoModes();
	
	_VMList = (RwChar **)RwCalloc(numModes, sizeof(RwChar*));
	
	for ( i = 0; i < numModes; i++	)
	{
		RwVideoMode			vm;
		
		RwEngineGetVideoModeInfo(&vm, i);
		
		if ( vm.flags & rwVIDEOMODEEXCLUSIVE )
		{
			_VMList[i] = (RwChar*)RwCalloc(100, sizeof(RwChar));
			rwsprintf(_VMList[i],"%d X %d X %d", vm.width, vm.height, vm.depth);
		}
		else
			_VMList[i] = nil;
	}
	
	return _VMList;
}

/*
 *****************************************************************************
 */
void _psSelectScreenVM(RwInt32 videoMode)
{
	RwTexDictionarySetCurrent( nil );
	
	FrontEndMenuManager.UnloadTextures();
	
	if (!_psSetVideoMode(RwEngineGetCurrentSubSystem(), videoMode))
	{
		RsGlobal.quit = TRUE;

		printf("ERROR: Failed to select new screen resolution\n");
	}
	else
		FrontEndMenuManager.LoadAllTextures();
}

/*
 *****************************************************************************
 */

RwBool IsForegroundApp()
{
	return !!ForegroundApp;
}
/*
UINT GetBestRefreshRate(UINT width, UINT height, UINT depth)
{
	LPDIRECT3D8 d3d = Direct3DCreate8(D3D_SDK_VERSION);
	
	ASSERT(d3d != nil);
	
	UINT refreshRate = INT_MAX;
	D3DFORMAT format;

	if ( depth == 32 )
		format = D3DFMT_X8R8G8B8;
	else if ( depth == 24 )
		format = D3DFMT_R8G8B8;
	else
		format = D3DFMT_R5G6B5;
	
	UINT modeCount = d3d->GetAdapterModeCount(GcurSel);
	
	for ( UINT i = 0; i < modeCount; i++ )
	{
		D3DDISPLAYMODE mode;
		
		d3d->EnumAdapterModes(GcurSel, i, &mode);
		
		if ( mode.Width == width && mode.Height == height && mode.Format == format )
		{
			if ( mode.RefreshRate == 0 )
				return 0;

			if ( mode.RefreshRate < refreshRate && mode.RefreshRate >= 60 )
				refreshRate = mode.RefreshRate;
		}
	}
	
#ifdef FIX_BUGS
	d3d->Release();
#endif
	
	if ( refreshRate == -1 )
		return -1;

	return refreshRate;
}
*/
/*
 *****************************************************************************
 */
RwBool
psSelectDevice()
{
	RwVideoMode			vm;
	RwInt32				subSysNum;
	RwInt32				AutoRenderer = 0;
	

	RwBool modeFound = FALSE;
	
	if ( !useDefault )
	{
		GnumSubSystems = RwEngineGetNumSubSystems();
		if ( !GnumSubSystems )
		{
			 return FALSE;
		}
		
		/* Just to be sure ... */
		GnumSubSystems = (GnumSubSystems > MAX_SUBSYSTEMS) ? MAX_SUBSYSTEMS : GnumSubSystems;
		
		/* Get the names of all the sub systems */
		for (subSysNum = 0; subSysNum < GnumSubSystems; subSysNum++)
		{
			RwEngineGetSubSystemInfo(&GsubSysInfo[subSysNum], subSysNum);
		}
		
		/* Get the default selection */
		GcurSel = RwEngineGetCurrentSubSystem();
#ifdef IMPROVED_VIDEOMODE
		if(FrontEndMenuManager.m_nPrefsSubsystem < GnumSubSystems)
			GcurSel = FrontEndMenuManager.m_nPrefsSubsystem;
#endif
	}
	
	/* Set the driver to use the correct sub system */
	if (!RwEngineSetSubSystem(GcurSel))
	{
		return FALSE;
	}

#ifdef IMPROVED_VIDEOMODE
	FrontEndMenuManager.m_nPrefsSubsystem = GcurSel;
#endif

#ifndef IMPROVED_VIDEOMODE
	if ( !useDefault )
	{
		if ( _psGetVideoModeList()[FrontEndMenuManager.m_nDisplayVideoMode] && FrontEndMenuManager.m_nDisplayVideoMode )
		{
			FrontEndMenuManager.m_nPrefsVideoMode = FrontEndMenuManager.m_nDisplayVideoMode;
			GcurSelVM = FrontEndMenuManager.m_nDisplayVideoMode;
		}
		else
		{
#ifdef DEFAULT_NATIVE_RESOLUTION
			// get the native video mode
			HDC hDevice = GetDC(NULL);
			int w = GetDeviceCaps(hDevice, HORZRES);
			int h = GetDeviceCaps(hDevice, VERTRES);
			int d = GetDeviceCaps(hDevice, BITSPIXEL);
#else
			const int w = 640;
			const int h = 480;
			const int d = 16;
#endif
			while ( !modeFound && GcurSelVM < RwEngineGetNumVideoModes() )
			{
				RwEngineGetVideoModeInfo(&vm, GcurSelVM);
				if ( defaultFullscreenRes	&& vm.width	 != w 
											|| vm.height != h
											|| vm.depth	 != d
											|| !(vm.flags & rwVIDEOMODEEXCLUSIVE) )
					++GcurSelVM;
				else
					modeFound = TRUE;
			}
			
			if ( !modeFound )
			{
#ifdef DEFAULT_NATIVE_RESOLUTION
				GcurSelVM = 1;
#else
				printf("WARNING: Cannot find 640x480 video mode, selecting device cancelled\n");
				return FALSE;
#endif
			}
		}
	}
#else
	if ( !useDefault )
	{
		if(FrontEndMenuManager.m_nPrefsWidth == 0 ||
		   FrontEndMenuManager.m_nPrefsHeight == 0 ||
		   FrontEndMenuManager.m_nPrefsDepth == 0){
			// Defaults if nothing specified
			const GLFWvidmode *mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
			FrontEndMenuManager.m_nPrefsWidth = mode->width;
			FrontEndMenuManager.m_nPrefsHeight = mode->height;
			FrontEndMenuManager.m_nPrefsDepth = 32;
			FrontEndMenuManager.m_nPrefsWindowed = 0;
		}

		// Find the videomode that best fits what we got from the settings file
		RwInt32 bestFsMode = -1;
		RwInt32 bestWidth = -1;
		RwInt32 bestHeight = -1;
		RwInt32 bestDepth = -1;
		for(GcurSelVM = 0; GcurSelVM < RwEngineGetNumVideoModes(); GcurSelVM++){
			RwEngineGetVideoModeInfo(&vm, GcurSelVM);

			if (!(vm.flags & rwVIDEOMODEEXCLUSIVE)){
				bestWndMode = GcurSelVM;
			} else {
				// try the largest one that isn't larger than what we wanted
				if(vm.width >= bestWidth && vm.width <= FrontEndMenuManager.m_nPrefsWidth &&
				   vm.height >= bestHeight && vm.height <= FrontEndMenuManager.m_nPrefsHeight &&
				   vm.depth >= bestDepth && vm.depth <= FrontEndMenuManager.m_nPrefsDepth){
					bestWidth = vm.width;
					bestHeight = vm.height;
					bestDepth = vm.depth;
					bestFsMode = GcurSelVM;
				}
			}
		}

		if(bestFsMode < 0){
#ifdef __EMSCRIPTEN__
			// A browser <canvas> has no OS-level "exclusive fullscreen video mode" to
			// select -- GLFW's Emscripten port never reports one as rwVIDEOMODEEXCLUSIVE,
			// so this search can never succeed here, and the engine would otherwise
			// never get past device selection in any browser. Fall back to the windowed
			// mode found above instead of aborting; real fullscreen is handled entirely
			// through the browser's own Fullscreen API (web/input.js), a separate,
			// browser-layer concern from RenderWare's video-mode model, same as Pointer
			// Lock (see docs/BROWSER_INPUT.md).
			if (bestWndMode >= 0) {
				bestFsMode = bestWndMode;
				FrontEndMenuManager.m_nPrefsWindowed = 1;
			} else
#endif
			{
				printf("WARNING: Cannot find desired video mode, selecting device cancelled\n");
				return FALSE;
			}
		}
		GcurSelVM = bestFsMode;

		FrontEndMenuManager.m_nDisplayVideoMode = GcurSelVM;
		FrontEndMenuManager.m_nPrefsVideoMode = FrontEndMenuManager.m_nDisplayVideoMode;

		FrontEndMenuManager.m_nSelectedScreenMode = FrontEndMenuManager.m_nPrefsWindowed;
	}
#endif

	RwEngineGetVideoModeInfo(&vm, GcurSelVM);

#ifdef IMPROVED_VIDEOMODE
	if (FrontEndMenuManager.m_nPrefsWindowed)
		GcurSelVM = bestWndMode;

	// Now GcurSelVM is 0 but vm has sizes(and fullscreen flag) of the video mode we want, that's why we changed the rwVIDEOMODEEXCLUSIVE conditions below
#ifdef __EMSCRIPTEN__
	// A browser <canvas> has no real "windowed video mode" list the way a
	// native desktop does (see the __EMSCRIPTEN__ comment further up in this
	// function) -- Emscripten's GLFW port reports its one synthetic windowed
	// entry as 0x0, which used to unconditionally overwrite the canvas-size
	// values main() pre-seeds into FrontEndMenuManager.m_nPrefsWidth/Height
	// right before RsEventHandler(rsRWINITIALIZE, ...) runs. That 0x0 then
	// propagates into RsGlobal.maximumWidth/Height a few lines below, and from
	// there into the camera's raster size for the rest of the session -- the
	// confirmed root cause of the camera raster staying permanently 0x0 (traced
	// via CameraSize()'s call log: every call after this point reported
	// origSize=(0,0)), which both keeps the screen black (SCREEN_WIDTH/HEIGHT
	// resolve through RsGlobal.width/height) and crashes the first thing that
	// divides by them (ScreenDroplets::FillScreenMoving's `% (int)SCREEN_WIDTH`,
	// "RuntimeError: remainder by zero"). Only apply vm's size if it's actually
	// valid; otherwise keep whatever was already there.
	if (vm.width > 0 && vm.height > 0) {
		FrontEndMenuManager.m_nPrefsWidth = vm.width;
		FrontEndMenuManager.m_nPrefsHeight = vm.height;
	}
#else
	FrontEndMenuManager.m_nPrefsWidth = vm.width;
	FrontEndMenuManager.m_nPrefsHeight = vm.height;
#endif
	FrontEndMenuManager.m_nPrefsDepth = vm.depth;
#endif

#ifndef PS2_MENU
	FrontEndMenuManager.m_nCurrOption = 0;
#endif
	
	/* Set up the video mode and set the apps window
	* dimensions to match */
	if (!RwEngineSetVideoMode(GcurSelVM))
	{
		return FALSE;
	}
	/*
	TODO
	if (vm.flags & rwVIDEOMODEEXCLUSIVE)
	{
		debug("%dx%dx%d", vm.width, vm.height, vm.depth);
		
		UINT refresh = GetBestRefreshRate(vm.width, vm.height, vm.depth);
		
		if ( refresh != (UINT)-1 )
		{
			debug("refresh %d", refresh);
			RwD3D8EngineSetRefreshRate((RwUInt32)refresh);
		}
	}
	*/
#ifndef IMPROVED_VIDEOMODE
	if (vm.flags & rwVIDEOMODEEXCLUSIVE)
	{
		RsGlobal.maximumWidth = vm.width;
		RsGlobal.maximumHeight = vm.height;
		RsGlobal.width = vm.width;
		RsGlobal.height = vm.height;
		
		PSGLOBAL(fullScreen) = TRUE;
	}
#else
		RsGlobal.maximumWidth = FrontEndMenuManager.m_nPrefsWidth;
		RsGlobal.maximumHeight = FrontEndMenuManager.m_nPrefsHeight;
		RsGlobal.width = FrontEndMenuManager.m_nPrefsWidth;
		RsGlobal.height = FrontEndMenuManager.m_nPrefsHeight;
		
		PSGLOBAL(fullScreen) = !FrontEndMenuManager.m_nPrefsWindowed;
#endif

#ifdef MULTISAMPLING
	RwD3D8EngineSetMultiSamplingLevels(1 << FrontEndMenuManager.m_nPrefsMSAALevel);
#endif
	return TRUE;
}

#ifndef GET_KEYBOARD_INPUT_FROM_X11
void keypressCB(GLFWwindow* window, int key, int scancode, int action, int mods);
#endif
void resizeCB(GLFWwindow* window, int width, int height);
void scrollCB(GLFWwindow* window, double xoffset, double yoffset);
void cursorCB(GLFWwindow* window, double xpos, double ypos);
void cursorEnterCB(GLFWwindow* window, int entered);
void windowFocusCB(GLFWwindow* window, int focused);
void windowIconifyCB(GLFWwindow* window, int iconified);
void joysChangeCB(int jid, int event);

bool IsThisJoystickBlacklisted(int i)
{
#ifndef DETECT_JOYSTICK_MENU
	return false;
#else
	if (glfwJoystickIsGamepad(i))
		return false;

	const char* joyname = glfwGetJoystickName(i);

	if (gSelectedJoystickName[0] != '\0' &&
		strncmp(joyname, gSelectedJoystickName, strlen(gSelectedJoystickName)) == 0)
		return false;

	return true;
#endif
}

void _InputInitialiseJoys()
{
	PSGLOBAL(joy1id) = -1;
	PSGLOBAL(joy2id) = -1;

	// Load our gamepad mappings.
	// Emscripten's GLFW3 port doesn't implement glfwUpdateGamepadMappings/glfwGetGamepadState,
	// so gamepad mapping DB support is skipped under Emscripten; the browser's own Gamepad API
	// mapping is used instead.
#ifndef __EMSCRIPTEN__
#define SDL_GAMEPAD_DB_PATH "gamecontrollerdb.txt"
	FILE *f = fopen(SDL_GAMEPAD_DB_PATH, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		size_t fsize = ftell(f);
		fseek(f, 0, SEEK_SET);

		char *db = (char*)malloc(fsize + 1);
		if (fread(db, 1, fsize, f) == fsize) {
			db[fsize] = '\0';

			if (glfwUpdateGamepadMappings(db) == GLFW_FALSE)
				Error("glfwUpdateGamepadMappings didn't succeed, check " SDL_GAMEPAD_DB_PATH ".\n");
		} else
			Error("fread on " SDL_GAMEPAD_DB_PATH " wasn't successful.\n");

		free(db);
		fclose(f);
	} else
		printf("You don't seem to have copied " SDL_GAMEPAD_DB_PATH " file from re3/gamefiles to GTA3 directory. Some gamepads may not be recognized.\n");

#undef SDL_GAMEPAD_DB_PATH

	// But always overwrite it with the one in SDL_GAMECONTROLLERCONFIG.
	char const* EnvControlConfig = getenv("SDL_GAMECONTROLLERCONFIG");
	if (EnvControlConfig != nil) {
		glfwUpdateGamepadMappings(EnvControlConfig);
	}
#endif

	for (int i = 0; i <= GLFW_JOYSTICK_LAST; i++) {
		if (glfwJoystickPresent(i) && !IsThisJoystickBlacklisted(i)) {
			if (PSGLOBAL(joy1id) == -1)
				PSGLOBAL(joy1id) = i;
			else if (PSGLOBAL(joy2id) == -1)
				PSGLOBAL(joy2id) = i;
			else
				break;
		}
	}

	if (PSGLOBAL(joy1id) != -1) {
		int count;
		glfwGetJoystickButtons(PSGLOBAL(joy1id), &count);
#ifdef DETECT_JOYSTICK_MENU
		strcpy(gSelectedJoystickName, glfwGetJoystickName(PSGLOBAL(joy1id)));
#endif
		ControlsManager.InitDefaultControlConfigJoyPad(count);
	}
}

long _InputInitialiseMouse()
{
	glfwSetInputMode(PSGLOBAL(window), GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
	return 0;
}

void psPostRWinit(void)
{
	RwVideoMode vm;
	RwEngineGetVideoModeInfo(&vm, GcurSelVM);

#ifndef GET_KEYBOARD_INPUT_FROM_X11
	glfwSetKeyCallback(PSGLOBAL(window), keypressCB);
#endif
	glfwSetFramebufferSizeCallback(PSGLOBAL(window), resizeCB);
	glfwSetScrollCallback(PSGLOBAL(window), scrollCB);
	glfwSetCursorPosCallback(PSGLOBAL(window), cursorCB);
	glfwSetCursorEnterCallback(PSGLOBAL(window), cursorEnterCB);
	glfwSetWindowIconifyCallback(PSGLOBAL(window), windowIconifyCB);
	glfwSetWindowFocusCallback(PSGLOBAL(window), windowFocusCB);
	glfwSetJoystickCallback(joysChangeCB);

	_InputInitialiseJoys();
	_InputInitialiseMouse();

	if(!(vm.flags & rwVIDEOMODEEXCLUSIVE))
		glfwSetWindowSize(PSGLOBAL(window), RsGlobal.maximumWidth, RsGlobal.maximumHeight);

	// Make sure all keys are released
	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);
}

/*
 *****************************************************************************
 */
RwBool _psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	RwInitialised = FALSE;
	
	RsEventHandler(rsRWTERMINATE, nil);
	
	GcurSel = subSystem;
	GcurSelVM = videoMode;
	
	useDefault = TRUE;
	
	if ( RsEventHandler(rsRWINITIALIZE, &openParams) == rsEVENTERROR )
		return FALSE;

	RwInitialised = TRUE;
	useDefault = FALSE;
	
	RwRect r;

	r.x = 0;
	r.y = 0;
	r.w = RsGlobal.maximumWidth;
	r.h = RsGlobal.maximumHeight;

	RsEventHandler(rsCAMERASIZE, &r);

	psPostRWinit();
	
	return TRUE;
}
 
 
/*
 *****************************************************************************
 */
static RwChar **
CommandLineToArgv(RwChar *cmdLine, RwInt32 *argCount)
{
	RwInt32 numArgs = 0;
	RwBool inArg, inString;
	RwInt32 i, len;
	RwChar *res, *str, **aptr;

	len = strlen(cmdLine);

	/* 
	 * Count the number of arguments...
	 */
	inString = FALSE;
	inArg = FALSE;

	for(i=0; i<=len; i++)
	{
		if( cmdLine[i] == '"' )
		{
			inString = !inString;
		}

		if( (cmdLine[i] <= ' ' && !inString) || i == len )
		{
			if( inArg ) 
			{
				inArg = FALSE;
				
				numArgs++;
			}
		} 
		else if( !inArg )
		{
			inArg = TRUE;
		}
	}

	/* 
	 * Allocate memory for result...
	 */
	res = (RwChar *)malloc(sizeof(RwChar *) * numArgs + len + 1);
	str = res + sizeof(RwChar *) * numArgs;
	aptr = (RwChar **)res;

	strcpy(str, cmdLine);

	/*
	 * Walk through cmdLine again this time setting pointer to each arg...
	 */
	inArg = FALSE;
	inString = FALSE;

	for(i=0; i<=len; i++)
	{
		if( cmdLine[i] == '"' )
		{
			inString = !inString;
		}

		if( (cmdLine[i] <= ' ' && !inString) || i == len )
		{
			if( inArg ) 
			{
				if( str[i-1] == '"' )
				{
					str[i-1] = '\0';
				}
				else
				{
					str[i] = '\0';
				}
				
				inArg = FALSE;
			}
		} 
		else if( !inArg && cmdLine[i] != '"' )
		{
			inArg = TRUE; 
			
			*aptr++ = &str[i];
		}
	}

	*argCount = numArgs;

	return (RwChar **)res;
}

/*
 *****************************************************************************
 */
void InitialiseLanguage()
{
#ifndef _WIN32
	// Mandatory for Linux(Unix? Posix?) to set lang. to environment lang.
	setlocale(LC_ALL, "");	

	char *systemLang, *keyboardLang;

	systemLang = setlocale (LC_ALL, NULL);
	keyboardLang = setlocale (LC_CTYPE, NULL);
	
	short primUserLCID, primSystemLCID;
	primUserLCID = primSystemLCID = !strncmp(systemLang, "fr_",3) ? LANG_FRENCH :
					!strncmp(systemLang, "de_",3) ? LANG_GERMAN :
					!strncmp(systemLang, "en_",3) ? LANG_ENGLISH :
					!strncmp(systemLang, "it_",3) ? LANG_ITALIAN :
					!strncmp(systemLang, "es_",3) ? LANG_SPANISH :
					LANG_OTHER;

	short primLayout = !strncmp(keyboardLang, "fr_",3) ? LANG_FRENCH : (!strncmp(keyboardLang, "de_",3) ? LANG_GERMAN : LANG_ENGLISH);

	short subUserLCID, subSystemLCID;
	subUserLCID = subSystemLCID = !strncmp(systemLang, "en_AU",5) ? SUBLANG_ENGLISH_AUS : SUBLANG_OTHER;
	short subLayout = !strncmp(keyboardLang, "en_AU",5) ? SUBLANG_ENGLISH_AUS : SUBLANG_OTHER;

#else
	WORD primUserLCID	= PRIMARYLANGID(GetSystemDefaultLCID());
	WORD primSystemLCID = PRIMARYLANGID(GetUserDefaultLCID());
	WORD primLayout		= PRIMARYLANGID((DWORD)GetKeyboardLayout(0));
	
	WORD subUserLCID	= SUBLANGID(GetSystemDefaultLCID());
	WORD subSystemLCID	= SUBLANGID(GetUserDefaultLCID());
	WORD subLayout		= SUBLANGID((DWORD)GetKeyboardLayout(0));
#endif
	if (   primUserLCID	  == LANG_GERMAN
		|| primSystemLCID == LANG_GERMAN
		|| primLayout	  == LANG_GERMAN )
	{
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CGame::germanGame = true;
	}
	
	if (   primUserLCID	  == LANG_FRENCH
		|| primSystemLCID == LANG_FRENCH
		|| primLayout	  == LANG_FRENCH )
	{
		CGame::nastyGame = false;
		CMenuManager::m_PrefsAllowNastyGame = false;
		CGame::frenchGame = true;
	}
	
	if (   subUserLCID	 == SUBLANG_ENGLISH_AUS
		|| subSystemLCID == SUBLANG_ENGLISH_AUS
		|| subLayout	 == SUBLANG_ENGLISH_AUS )
		CGame::noProstitutes = true;

#ifdef NASTY_GAME
	CGame::nastyGame = true;
	CMenuManager::m_PrefsAllowNastyGame = true;
	CGame::noProstitutes = false;
#endif
	
	int32 lang;
	
	switch ( primSystemLCID )
	{
		case LANG_GERMAN:
		{
			lang = LANG_GERMAN;
			break;
		}
		case LANG_FRENCH:
		{
			lang = LANG_FRENCH;
			break;
		}
		case LANG_SPANISH:
		{
			lang = LANG_SPANISH;
			break;
		}
		case LANG_ITALIAN:
		{
			lang = LANG_ITALIAN;
			break;
		}
		default:
		{
			lang = ( subSystemLCID == SUBLANG_ENGLISH_AUS ) ? -99 : LANG_ENGLISH;
			break;
		}
	}
	
	CMenuManager::OS_Language = primUserLCID;

	switch ( lang )
	{
		case LANG_GERMAN:
		{
			CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_GERMAN;
			break;
		}
		case LANG_SPANISH:
		{
			CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_SPANISH;
			break;
		}
		case LANG_FRENCH:
		{
			CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_FRENCH;
			break;
		}
		case LANG_ITALIAN:
		{
			CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_ITALIAN;
			break;
		}
		default:
		{
			CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;
			break;
		}
	}

#ifndef _WIN32
	// TODO this is needed for strcasecmp to work correctly across all languages, but can these cause other problems??
	setlocale(LC_CTYPE, "C");
	setlocale(LC_COLLATE, "C");
	setlocale(LC_NUMERIC, "C");
#endif

	TheText.Unload();
	TheText.Load();
}

/*
 *****************************************************************************
 */

void HandleExit()
{
#ifdef _WIN32
	MSG message;
	while ( PeekMessage(&message, nil, 0U, 0U, PM_REMOVE|PM_NOYIELD) )
	{
		if( message.message == WM_QUIT )
		{
			RsGlobal.quit = TRUE;
		}
		else
		{
			TranslateMessage(&message);
			DispatchMessage(&message);
		}
	}
#else
	// We now handle terminate message always, why handle on some cases?
	return;
#endif
}

#ifndef _WIN32
void terminateHandler(int sig, siginfo_t *info, void *ucontext) {
	RsGlobal.quit = TRUE;
}

#ifdef FLUSHABLE_STREAMING
void dummyHandler(int sig){
	// Don't kill the app pls
}
#endif
#endif

void resizeCB(GLFWwindow* window, int width, int height) {
	/*
	* Handle event to ensure window contents are displayed during re-size
	* as this can be disabled by the user, then if there is not enough
	* memory things don't work.
	*/
	/* redraw window */

	if (RwInitialised && gGameState == GS_PLAYING_GAME)
	{
		RsEventHandler(rsIDLE, (void *)TRUE);
	}

	if (RwInitialised && height > 0 && width > 0) {
		RwRect r;

		// TODO fix artifacts of resizing with mouse
		RsGlobal.maximumHeight = height;
		RsGlobal.maximumWidth = width;

		r.x = 0;
		r.y = 0;
		r.w = width;
		r.h = height;

		RsEventHandler(rsCAMERASIZE, &r);
	}
//	glfwSetWindowPos(window, 0, 0);
}

void scrollCB(GLFWwindow* window, double xoffset, double yoffset) {
	PSGLOBAL(mouseWheel) = yoffset;
}

bool lshiftStatus = false;
bool rshiftStatus = false;

#ifndef GET_KEYBOARD_INPUT_FROM_X11
int keymap[GLFW_KEY_LAST + 1];

static void
initkeymap(void)
{
	int i;
	for (i = 0; i < GLFW_KEY_LAST + 1; i++)
		keymap[i] = rsNULL;

	keymap[GLFW_KEY_SPACE] = ' ';
	keymap[GLFW_KEY_APOSTROPHE] = '\'';
	keymap[GLFW_KEY_COMMA] = ',';
	keymap[GLFW_KEY_MINUS] = '-';
	keymap[GLFW_KEY_PERIOD] = '.';
	keymap[GLFW_KEY_SLASH] = '/';
	keymap[GLFW_KEY_0] = '0';
	keymap[GLFW_KEY_1] = '1';
	keymap[GLFW_KEY_2] = '2';
	keymap[GLFW_KEY_3] = '3';
	keymap[GLFW_KEY_4] = '4';
	keymap[GLFW_KEY_5] = '5';
	keymap[GLFW_KEY_6] = '6';
	keymap[GLFW_KEY_7] = '7';
	keymap[GLFW_KEY_8] = '8';
	keymap[GLFW_KEY_9] = '9';
	keymap[GLFW_KEY_SEMICOLON] = ';';
	keymap[GLFW_KEY_EQUAL] = '=';
	keymap[GLFW_KEY_A] = 'A';
	keymap[GLFW_KEY_B] = 'B';
	keymap[GLFW_KEY_C] = 'C';
	keymap[GLFW_KEY_D] = 'D';
	keymap[GLFW_KEY_E] = 'E';
	keymap[GLFW_KEY_F] = 'F';
	keymap[GLFW_KEY_G] = 'G';
	keymap[GLFW_KEY_H] = 'H';
	keymap[GLFW_KEY_I] = 'I';
	keymap[GLFW_KEY_J] = 'J';
	keymap[GLFW_KEY_K] = 'K';
	keymap[GLFW_KEY_L] = 'L';
	keymap[GLFW_KEY_M] = 'M';
	keymap[GLFW_KEY_N] = 'N';
	keymap[GLFW_KEY_O] = 'O';
	keymap[GLFW_KEY_P] = 'P';
	keymap[GLFW_KEY_Q] = 'Q';
	keymap[GLFW_KEY_R] = 'R';
	keymap[GLFW_KEY_S] = 'S';
	keymap[GLFW_KEY_T] = 'T';
	keymap[GLFW_KEY_U] = 'U';
	keymap[GLFW_KEY_V] = 'V';
	keymap[GLFW_KEY_W] = 'W';
	keymap[GLFW_KEY_X] = 'X';
	keymap[GLFW_KEY_Y] = 'Y';
	keymap[GLFW_KEY_Z] = 'Z';
	keymap[GLFW_KEY_LEFT_BRACKET] = '[';
	keymap[GLFW_KEY_BACKSLASH] = '\\';
	keymap[GLFW_KEY_RIGHT_BRACKET] = ']';
	keymap[GLFW_KEY_GRAVE_ACCENT] = '`';
	keymap[GLFW_KEY_ESCAPE] = rsESC;
	keymap[GLFW_KEY_ENTER] = rsENTER;
	keymap[GLFW_KEY_TAB] = rsTAB;
	keymap[GLFW_KEY_BACKSPACE] = rsBACKSP;
	keymap[GLFW_KEY_INSERT] = rsINS;
	keymap[GLFW_KEY_DELETE] = rsDEL;
	keymap[GLFW_KEY_RIGHT] = rsRIGHT;
	keymap[GLFW_KEY_LEFT] = rsLEFT;
	keymap[GLFW_KEY_DOWN] = rsDOWN;
	keymap[GLFW_KEY_UP] = rsUP;
	keymap[GLFW_KEY_PAGE_UP] = rsPGUP;
	keymap[GLFW_KEY_PAGE_DOWN] = rsPGDN;
	keymap[GLFW_KEY_HOME] = rsHOME;
	keymap[GLFW_KEY_END] = rsEND;
	keymap[GLFW_KEY_CAPS_LOCK] = rsCAPSLK;
	keymap[GLFW_KEY_SCROLL_LOCK] = rsSCROLL;
	keymap[GLFW_KEY_NUM_LOCK] = rsNUMLOCK;
	keymap[GLFW_KEY_PRINT_SCREEN] = rsNULL;
	keymap[GLFW_KEY_PAUSE] = rsPAUSE;

	keymap[GLFW_KEY_F1] = rsF1;
	keymap[GLFW_KEY_F2] = rsF2;
	keymap[GLFW_KEY_F3] = rsF3;
	keymap[GLFW_KEY_F4] = rsF4;
	keymap[GLFW_KEY_F5] = rsF5;
	keymap[GLFW_KEY_F6] = rsF6;
	keymap[GLFW_KEY_F7] = rsF7;
	keymap[GLFW_KEY_F8] = rsF8;
	keymap[GLFW_KEY_F9] = rsF9;
	keymap[GLFW_KEY_F10] = rsF10;
	keymap[GLFW_KEY_F11] = rsF11;
	keymap[GLFW_KEY_F12] = rsF12;
	keymap[GLFW_KEY_F13] = rsNULL;
	keymap[GLFW_KEY_F14] = rsNULL;
	keymap[GLFW_KEY_F15] = rsNULL;
	keymap[GLFW_KEY_F16] = rsNULL;
	keymap[GLFW_KEY_F17] = rsNULL;
	keymap[GLFW_KEY_F18] = rsNULL;
	keymap[GLFW_KEY_F19] = rsNULL;
	keymap[GLFW_KEY_F20] = rsNULL;
	keymap[GLFW_KEY_F21] = rsNULL;
	keymap[GLFW_KEY_F22] = rsNULL;
	keymap[GLFW_KEY_F23] = rsNULL;
	keymap[GLFW_KEY_F24] = rsNULL;
	keymap[GLFW_KEY_F25] = rsNULL;
	keymap[GLFW_KEY_KP_0] = rsPADINS;
	keymap[GLFW_KEY_KP_1] = rsPADEND;
	keymap[GLFW_KEY_KP_2] = rsPADDOWN;
	keymap[GLFW_KEY_KP_3] = rsPADPGDN;
	keymap[GLFW_KEY_KP_4] = rsPADLEFT;
	keymap[GLFW_KEY_KP_5] = rsPAD5;
	keymap[GLFW_KEY_KP_6] = rsPADRIGHT;
	keymap[GLFW_KEY_KP_7] = rsPADHOME;
	keymap[GLFW_KEY_KP_8] = rsPADUP;
	keymap[GLFW_KEY_KP_9] = rsPADPGUP;
	keymap[GLFW_KEY_KP_DECIMAL] = rsPADDEL;
	keymap[GLFW_KEY_KP_DIVIDE] = rsDIVIDE;
	keymap[GLFW_KEY_KP_MULTIPLY] = rsTIMES;
	keymap[GLFW_KEY_KP_SUBTRACT] = rsMINUS;
	keymap[GLFW_KEY_KP_ADD] = rsPLUS;
	keymap[GLFW_KEY_KP_ENTER] = rsPADENTER;
	keymap[GLFW_KEY_KP_EQUAL] = rsNULL;
	keymap[GLFW_KEY_LEFT_SHIFT] = rsLSHIFT;
	keymap[GLFW_KEY_LEFT_CONTROL] = rsLCTRL;
	keymap[GLFW_KEY_LEFT_ALT] = rsLALT;
	keymap[GLFW_KEY_LEFT_SUPER] = rsLWIN;
	keymap[GLFW_KEY_RIGHT_SHIFT] = rsRSHIFT;
	keymap[GLFW_KEY_RIGHT_CONTROL] = rsRCTRL;
	keymap[GLFW_KEY_RIGHT_ALT] = rsRALT;
	keymap[GLFW_KEY_RIGHT_SUPER] = rsRWIN;
	keymap[GLFW_KEY_MENU] = rsNULL;
}

void
keypressCB(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	if (key >= 0 && key <= GLFW_KEY_LAST && action != GLFW_REPEAT) {
		RsKeyCodes ks = (RsKeyCodes)keymap[key];

		if (key == GLFW_KEY_LEFT_SHIFT)
			lshiftStatus = action != GLFW_RELEASE;

		if (key == GLFW_KEY_RIGHT_SHIFT)
			rshiftStatus = action != GLFW_RELEASE;

		if (action == GLFW_RELEASE) RsKeyboardEventHandler(rsKEYUP, &ks);
		else if (action == GLFW_PRESS) RsKeyboardEventHandler(rsKEYDOWN, &ks);
	}
}

#else

uint32 keymap[512]; // 256 ascii + 256 KeySyms between 0xff00 - 0xffff
bool keyStates[512];
uint32 keyCodeToKeymapIndex[256]; // cache for physical keys

#define KEY_MAP_OFFSET (0xff00 - 256)
static void
initkeymap(void)
{
	Display *display = glfwGetX11Display();
	int i;

	for (i = 0; i < ARRAY_SIZE(keymap); i++)
		keymap[i] = rsNULL;

	// You can add new ASCII mappings to here freely (but beware that if right hand side of assignment isn't supported on CFont, it'll be blank/won't work on binding screen)
	// Right hand side of assigments should always be uppercase counterpart of character
	keymap[XK_space] = ' ';
	keymap[XK_apostrophe] = '\'';
	keymap[XK_ampersand] = '&';
	keymap[XK_percent] = '%';
	keymap[XK_dollar] = '$';
	keymap[XK_comma] = ',';
	keymap[XK_minus] = '-';
	keymap[XK_period] = '.';
	keymap[XK_slash] = '/';
	keymap[XK_question] = '?';
	keymap[XK_exclam] = '!';
	keymap[XK_quotedbl] = '"';
	keymap[XK_colon] = ':';
	keymap[XK_semicolon] = ';';
	keymap[XK_equal] = '=';
	keymap[XK_bracketleft] = '[';
	keymap[XK_backslash] = '\\';
	keymap[XK_bracketright] = ']';
	keymap[XK_grave] = '`';
	keymap[XK_0] = '0';
	keymap[XK_1] = '1';
	keymap[XK_2] = '2';
	keymap[XK_3] = '3';
	keymap[XK_4] = '4';
	keymap[XK_5] = '5';
	keymap[XK_6] = '6';
	keymap[XK_7] = '7';
	keymap[XK_8] = '8';
	keymap[XK_9] = '9';
	keymap[XK_a] = 'A';
	keymap[XK_b] = 'B';
	keymap[XK_c] = 'C';
	keymap[XK_d] = 'D';
	keymap[XK_e] = 'E';
	keymap[XK_f] = 'F';
	keymap[XK_g] = 'G';
	keymap[XK_h] = 'H';
	keymap[XK_i] = 'I';
	keymap[XK_I] = 'I'; // Turkish I problem
	keymap[XK_j] = 'J';
	keymap[XK_k] = 'K';
	keymap[XK_l] = 'L';
	keymap[XK_m] = 'M';
	keymap[XK_n] = 'N';
	keymap[XK_o] = 'O';
	keymap[XK_p] = 'P';
	keymap[XK_q] = 'Q';
	keymap[XK_r] = 'R';
	keymap[XK_s] = 'S';
	keymap[XK_t] = 'T';
	keymap[XK_u] = 'U';
	keymap[XK_v] = 'V';
	keymap[XK_w] = 'W';
	keymap[XK_x] = 'X';
	keymap[XK_y] = 'Y';
	keymap[XK_z] = 'Z';

	// Some of regional but ASCII characters that GTA supports
	keymap[XK_agrave] = 0x00c0;
	keymap[XK_aacute] = 0x00c1;
	keymap[XK_acircumflex] = 0x00c2;
	keymap[XK_adiaeresis] = 0x00c4;

	keymap[XK_ae] = 0x00c6;

	keymap[XK_egrave] = 0x00c8;
	keymap[XK_eacute] = 0x00c9;
	keymap[XK_ecircumflex] = 0x00ca;
	keymap[XK_ediaeresis] = 0x00cb;

	keymap[XK_igrave] = 0x00cc;
	keymap[XK_iacute] = 0x00cd;
	keymap[XK_icircumflex] = 0x00ce;
	keymap[XK_idiaeresis] = 0x00cf;

	keymap[XK_ccedilla] = 0x00c7;
	keymap[XK_odiaeresis] = 0x00d6;
	keymap[XK_udiaeresis] = 0x00dc;

	// These are 0xff00 - 0xffff range of KeySym's, and subtracting KEY_MAP_OFFSET is needed
	keymap[XK_Escape - KEY_MAP_OFFSET] = rsESC;
	keymap[XK_Return - KEY_MAP_OFFSET] = rsENTER;
	keymap[XK_Tab - KEY_MAP_OFFSET] = rsTAB;
	keymap[XK_BackSpace - KEY_MAP_OFFSET] = rsBACKSP;
	keymap[XK_Insert - KEY_MAP_OFFSET] = rsINS;
	keymap[XK_Delete - KEY_MAP_OFFSET] = rsDEL;
	keymap[XK_Right - KEY_MAP_OFFSET] = rsRIGHT;
	keymap[XK_Left - KEY_MAP_OFFSET] = rsLEFT;
	keymap[XK_Down - KEY_MAP_OFFSET] = rsDOWN;
	keymap[XK_Up - KEY_MAP_OFFSET] = rsUP;
	keymap[XK_Page_Up - KEY_MAP_OFFSET] = rsPGUP;
	keymap[XK_Page_Down - KEY_MAP_OFFSET] = rsPGDN;
	keymap[XK_Home - KEY_MAP_OFFSET] = rsHOME;
	keymap[XK_End - KEY_MAP_OFFSET] = rsEND;
	keymap[XK_Caps_Lock - KEY_MAP_OFFSET] = rsCAPSLK;
	keymap[XK_Scroll_Lock - KEY_MAP_OFFSET] = rsSCROLL;
	keymap[XK_Num_Lock - KEY_MAP_OFFSET] = rsNUMLOCK;
	keymap[XK_Pause - KEY_MAP_OFFSET] = rsPAUSE;

	keymap[XK_F1 - KEY_MAP_OFFSET] = rsF1;
	keymap[XK_F2 - KEY_MAP_OFFSET] = rsF2;
	keymap[XK_F3 - KEY_MAP_OFFSET] = rsF3;
	keymap[XK_F4 - KEY_MAP_OFFSET] = rsF4;
	keymap[XK_F5 - KEY_MAP_OFFSET] = rsF5;
	keymap[XK_F6 - KEY_MAP_OFFSET] = rsF6;
	keymap[XK_F7 - KEY_MAP_OFFSET] = rsF7;
	keymap[XK_F8 - KEY_MAP_OFFSET] = rsF8;
	keymap[XK_F9 - KEY_MAP_OFFSET] = rsF9;
	keymap[XK_F10 - KEY_MAP_OFFSET] = rsF10;
	keymap[XK_F11 - KEY_MAP_OFFSET] = rsF11;
	keymap[XK_F12 - KEY_MAP_OFFSET] = rsF12;
	keymap[XK_F13 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F14 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F15 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F16 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F17 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F18 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F19 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F20 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F21 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F22 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F23 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F24 - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_F25 - KEY_MAP_OFFSET] = rsNULL;

	keymap[XK_KP_0 - KEY_MAP_OFFSET] = rsPADINS;
	keymap[XK_KP_1 - KEY_MAP_OFFSET] = rsPADEND;
	keymap[XK_KP_2 - KEY_MAP_OFFSET] = rsPADDOWN;
	keymap[XK_KP_3 - KEY_MAP_OFFSET] = rsPADPGDN;
	keymap[XK_KP_4 - KEY_MAP_OFFSET] = rsPADLEFT;
	keymap[XK_KP_5 - KEY_MAP_OFFSET] = rsPAD5;
	keymap[XK_KP_6 - KEY_MAP_OFFSET] = rsPADRIGHT;
	keymap[XK_KP_7 - KEY_MAP_OFFSET] = rsPADHOME;
	keymap[XK_KP_8 - KEY_MAP_OFFSET] = rsPADUP;
	keymap[XK_KP_9 - KEY_MAP_OFFSET] = rsPADPGUP;
	keymap[XK_KP_Insert - KEY_MAP_OFFSET] = rsPADINS;
	keymap[XK_KP_End - KEY_MAP_OFFSET] = rsPADEND;
	keymap[XK_KP_Down - KEY_MAP_OFFSET] = rsPADDOWN;
	keymap[XK_KP_Page_Down - KEY_MAP_OFFSET] = rsPADPGDN;
	keymap[XK_KP_Left - KEY_MAP_OFFSET] = rsPADLEFT;
	keymap[XK_KP_Begin - KEY_MAP_OFFSET] = rsPAD5;
	keymap[XK_KP_Right - KEY_MAP_OFFSET] = rsPADRIGHT;
	keymap[XK_KP_Home - KEY_MAP_OFFSET] = rsPADHOME;
	keymap[XK_KP_Up - KEY_MAP_OFFSET] = rsPADUP;
	keymap[XK_KP_Page_Up - KEY_MAP_OFFSET] = rsPADPGUP;

	keymap[XK_KP_Decimal - KEY_MAP_OFFSET] = rsPADDEL;
	keymap[XK_KP_Divide - KEY_MAP_OFFSET] = rsDIVIDE;
	keymap[XK_KP_Multiply - KEY_MAP_OFFSET] = rsTIMES;
	keymap[XK_KP_Subtract - KEY_MAP_OFFSET] = rsMINUS;
	keymap[XK_KP_Add - KEY_MAP_OFFSET] = rsPLUS;
	keymap[XK_KP_Enter - KEY_MAP_OFFSET] = rsPADENTER;
	keymap[XK_KP_Equal - KEY_MAP_OFFSET] = rsNULL;
	keymap[XK_Shift_L - KEY_MAP_OFFSET] = rsLSHIFT;
	keymap[XK_Control_L - KEY_MAP_OFFSET] = rsLCTRL;
	keymap[XK_Alt_L - KEY_MAP_OFFSET] = rsLALT;
	keymap[XK_Super_L - KEY_MAP_OFFSET] = rsLWIN;
	keymap[XK_Shift_R - KEY_MAP_OFFSET] = rsRSHIFT;
	keymap[XK_Control_R - KEY_MAP_OFFSET] = rsRCTRL;
	keymap[XK_Alt_R - KEY_MAP_OFFSET] = rsRALT;
	keymap[XK_Super_R - KEY_MAP_OFFSET] = rsRWIN;
	keymap[XK_Menu - KEY_MAP_OFFSET] = rsNULL;

	// Cache the key codes' key symbol equivelants, otherwise we will have to do it on each frame
	// KeyCode is always in [0,255], and represents a physical key

	int min_keycode, max_keycode, keysyms_per_keycode;
	KeySym *keymap, *origkeymap;

	char *keyboardLang = setlocale (LC_CTYPE, NULL);
	setlocale(LC_CTYPE, "");

	XDisplayKeycodes(display, &min_keycode, &max_keycode);
	origkeymap = XGetKeyboardMapping(display, min_keycode, (max_keycode - min_keycode + 1), &keysyms_per_keycode);
	keymap = origkeymap;
	for (int i = min_keycode; i <= max_keycode; i++) {
		int  j, lastKeysym;

		lastKeysym = keysyms_per_keycode - 1;
		while ((lastKeysym >= 0) && (keymap[lastKeysym] == NoSymbol))
			lastKeysym--;

		for (j = 0; j <= lastKeysym; j++) {
			KeySym ks = keymap[j];

			if (ks == NoSymbol)
				continue;

			if (ks < 256) {
				keyCodeToKeymapIndex[i] = ks;
				break;
			} else if (ks >= 0xff00 && ks < 0xffff) {
				keyCodeToKeymapIndex[i] = ks - KEY_MAP_OFFSET;
				break;
			}
		}
		keymap += keysyms_per_keycode;
	}
	XFree(origkeymap);

	setlocale(LC_CTYPE, keyboardLang);
}
#undef KEY_MAP_OFFSET

void checkKeyPresses()
{
	Display *display = glfwGetX11Display();
	char keys[32];
	XQueryKeymap(display, keys);
	for (int i = 0; i < sizeof(keys); i++) {
		for (int j = 0; j < 8; j++) {
			KeyCode keycode = 8 * i + j;
			uint32 keymapIndex = keyCodeToKeymapIndex[keycode];
			if (keymapIndex != 0) {
				int rsCode = keymap[keymapIndex];
				if (rsCode == rsNULL)
					continue;

				bool pressed = WindowFocused && !!(keys[i] & (1 << j));

				// idk why R* does that
				if (rsCode == rsLSHIFT)
					lshiftStatus = pressed;
				else if (rsCode == rsRSHIFT)
					rshiftStatus = pressed;

				if (keyStates[keymapIndex] != pressed) {
					if (pressed) {
						RsKeyboardEventHandler(rsKEYDOWN, &rsCode);
					} else {
						RsKeyboardEventHandler(rsKEYUP, &rsCode);
					}
				}

				keyStates[keymapIndex] = pressed;
			}
		}
	}

}
#endif

// R* calls that in ControllerConfig, idk why
void
_InputTranslateShiftKeyUpDown(RsKeyCodes *rs) {
	RsKeyboardEventHandler(lshiftStatus ? rsKEYDOWN : rsKEYUP, &(*rs = rsLSHIFT));
	RsKeyboardEventHandler(rshiftStatus ? rsKEYDOWN : rsKEYUP, &(*rs = rsRSHIFT));
}

// TODO this only works in frontend(and luckily only frontend use this). Fun fact: if I get pos manually in game, glfw reports that it's > 32000
void
cursorCB(GLFWwindow* window, double xpos, double ypos) {
	if (!FrontEndMenuManager.m_bMenuActive)
		return;
	
	int winw, winh;
	glfwGetWindowSize(PSGLOBAL(window), &winw, &winh);
	FrontEndMenuManager.m_nMouseTempPosX = xpos * (RsGlobal.maximumWidth / winw);
	FrontEndMenuManager.m_nMouseTempPosY = ypos * (RsGlobal.maximumHeight / winh);
}

void
cursorEnterCB(GLFWwindow* window, int entered) {
	PSGLOBAL(cursorIsInWindow) = !!entered;
}

void
windowFocusCB(GLFWwindow* window, int focused) {
	WindowFocused = !!focused;
}

void
windowIconifyCB(GLFWwindow* window, int iconified) {
	WindowIconified = !!iconified;
}

#ifdef __EMSCRIPTEN__
// Browser tab focus loss (switching tabs, alt-tabbing, opening devtools, clicking
// outside the page) doesn't reliably reach glfwSetWindowFocusCallback under
// Emscripten -- libglfw.js stores the callback but its own 'blur' handler never
// actually invokes it (it only synthesizes key-release events, which is why
// keyboard keys alone don't get stuck; see docs/BROWSER_INPUT.md). Mouse buttons
// are read via polling (glfwGetMouseButton in CPad::UpdateMouse(), src/core/Pad.cpp)
// with no such safety net, so a button held down when focus is lost would
// otherwise stay "pressed" forever. web/launcher.js calls this directly (via
// ccall) on window 'blur' and on the page becoming hidden, covering keyboard,
// mouse, and gamepad/joystick state uniformly through the same CPad::Clear() the
// engine already uses at startup -- no new state-clearing logic, just triggering
// the existing one from a browser-specific signal. Releasing pointer lock itself
// is handled directly in JS (document.exitPointerLock()), not here -- see
// docs/BROWSER_INPUT.md for why glfwSetInputMode/glfwGetInputMode aren't used for
// this under this Emscripten version.
extern "C" EMSCRIPTEN_KEEPALIVE void
re3_OnBrowserFocusLost(void)
{
	CPad::GetPad(0)->Clear(false);
	CPad::GetPad(1)->Clear(false);
}

// Tiny read-only getter so web/input.js can release pointer lock itself when a
// menu opens (menu mouse navigation needs absolute position; pointer lock only
// reports relative deltas) without re3's C++ code needing to touch browser-only
// pointer-lock APIs at all. See docs/BROWSER_INPUT.md.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_IsMenuActive(void)
{
	return FrontEndMenuManager.m_bMenuActive ? 1 : 0;
}

// Phase 7 (docs/BROWSER_MAIN_LOOP.md), browser tab throttling: requestAnimationFrame
// (what emscripten_set_main_loop(tick, 0, 1) drives off of, see the state machine
// below) stops firing entirely while a tab is hidden/backgrounded -- so the whole
// tick() loop, and with it CTimer::Update(), simply stops running. Nothing races or
// spins; the risk is purely in the *next* CTimer::Update() call once the tab comes
// back, whose measured delta would otherwise include the entire hidden duration
// (seconds to hours). CTimer already clamps a single frame's timestep/delta for
// exactly this kind of stall (see ms_fTimeStep's Min(3.0f, ...) and the 60ms delta
// clamp in Timer.cpp) so nothing would actually break without this -- but
// CTimer::Suspend()/Resume() (already used natively for e.g. long collision
// precomputation and script pauses that must not count as elapsed game time, see
// Collision.cpp/Script.cpp) is the more precise, purpose-built tool for "this real
// time gap should not count at all", so web/perf.js calls these directly on the
// page's visibilitychange event rather than relying solely on the clamp.
extern "C" EMSCRIPTEN_KEEPALIVE void
re3_OnBrowserTabHidden(void)
{
	CTimer::Suspend();
}

extern "C" EMSCRIPTEN_KEEPALIVE void
re3_OnBrowserTabVisible(void)
{
	CTimer::Resume();
}

// Phase 9 (docs/FIRST_BOOT.md), boot-progress tracking: a single monotonically
// increasing stage counter, set from a handful of call sites across the engine
// as each subsystem actually finishes initializing (Initialise3D() in
// main.cpp, CGame::InitialiseOnceAfterRW() in Game.cpp, the state machine
// below), read by web/boot.js to drive the on-page [1..7] boot-progress
// indicator. re3_SetBootStage() is deliberately not exported to JS -- only the
// engine itself should ever advance it; only the read side is browser-facing.
static int g_re3BootStage = 1; // 1 = "WASM loaded", the floor -- this code
                                // couldn't be running otherwise.

extern "C" void
re3_SetBootStage(int stage)
{
	if (stage > g_re3BootStage)
		g_re3BootStage = stage;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetBootStage(void)
{
	return g_re3BootStage;
}

// Phase 9 (docs/FIRST_BOOT.md), browser debug mode: emscripten_set_main_loop(tick,
// 0, 1) (below) drives tick() off requestAnimationFrame, which several browser
// states suspend entirely (a hidden/backgrounded tab, or -- as found validating
// this very port -- a tab that isn't actually being composited by whatever's
// hosting the browser). emscripten_set_main_loop_timing() lets the *same*
// already-registered tick() callback be rescheduled onto setTimeout instead,
// which several of those states don't suspend. This trades vsync alignment
// (and the power/battery benefits of rAF) for a loop that keeps advancing
// somewhere rAF wouldn't fire -- which is exactly what a *debug* mode should
// do; it is not the default. web/boot.js exposes this as the "Debug mode"
// toggle, off by default.
extern "C" EMSCRIPTEN_KEEPALIVE void
re3_SetDebugTiming(int enable)
{
	if (enable)
		emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 16);
	else
		emscripten_set_main_loop_timing(EM_TIMING_RAF, 1);
}

// Phase 9 (docs/FIRST_BOOT.md): runs exactly one iteration of the same tick()
// callback emscripten_set_main_loop() drives -- synchronously, on whatever
// thread calls this (via ccall, i.e. the browser's own JS/CDP execution, not a
// browser timer). This exists because even re3_SetDebugTiming(1) reschedules
// tick() onto a *browser* timer (setTimeout), which some hosting contexts
// throttle or suspend just as aggressively as requestAnimationFrame for a
// backgrounded/non-composited page -- discovered validating this exact port.
// Calling this repeatedly from JS (a tight `for` loop of ccall("re3_PumpTick")
// in one script, not one call per rAF/setTimeout) drives the engine forward
// regardless of any browser timer throttling. Not part of normal operation --
// normal play always uses emscripten_set_main_loop's own scheduling.
extern "C" EMSCRIPTEN_KEEPALIVE void
re3_PumpTick(void)
{
	if (g_re3TickFn)
		g_re3TickFn();
}

// DevTools-dependency debugging: raw gGameState value (see the GS_* enum in
// src/skel/crossplatform.h -- 0=START_UP .. 7=FRONTEND, 8=INIT_PLAYING_GAME,
// 9=PLAYING_GAME) for the browser diagnostics panel, so canvas/viewport
// dimensions can be directly correlated against actual engine state rather
// than inferred from log timing.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetGameState(void)
{
	return (int)gGameState;
}

// docs/SAVES_WASM.md validation: clears the same CMenuManager::m_bMenuActive
// flag that "New Game"/"Load Game" clear when selected from the frontend menu
// (see the GS_FRONTEND case in tick() below -- !m_bMenuActive is exactly what
// advances gGameState to GS_INIT_PLAYING_GAME, which calls the real,
// unmodified InitialiseGame()). Lets browser-side testing reach GS_PLAYING_GAME
// deterministically without driving the menu's mouse/keyboard UI. Debug-only;
// never called by the engine itself.
extern "C" EMSCRIPTEN_KEEPALIVE void
re3_DebugForceExitFrontend(void)
{
	FrontEndMenuManager.m_bMenuActive = false;
}

// Audio debugging: real (not fabricated) read of the underlying Web Audio
// AudioContext's state, reaching directly into Emscripten's own OpenAL
// implementation object model (emsdk/upstream/emscripten/src/lib/libopenal.js
// -- AL.currentCtx.audioCtx) since OpenAL itself has no ALC query for this.
// Returns: 0 = no context yet, 1 = running, 2 = suspended, 3 = closed.
// Plain EM_JS-defined functions aren't retained by the linker the way an
// EMSCRIPTEN_KEEPALIVE C function is (nothing on the C++ side ever calls this
// one -- it's only meant to be reached via ccall from JS) -- so it's given an
// internal name and wrapped in a normal EMSCRIPTEN_KEEPALIVE export, matching
// every other function in this file.
EM_JS(int, re3_GetAudioContextStateJS, (), {
	try {
		if (typeof AL !== "undefined" && AL.currentCtx && AL.currentCtx.audioCtx) {
			var state = AL.currentCtx.audioCtx.state;
			if (state === "running") return 1;
			if (state === "suspended") return 2;
			if (state === "closed") return 3;
		}
	} catch (e) {}
	return 0;
});

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetAudioContextState(void)
{
	return re3_GetAudioContextStateJS();
}

// Audio debugging: a running AudioContext doesn't by itself prove anything is
// audible -- these inspect the actual OpenAL source table Emscripten's
// libopenal.js maintains (AL.currentCtx.sources), the same object model
// re3_GetAudioContextState reaches into, to answer "are sources even being
// created" and "is anything actually in AL_PLAYING state with nonzero gain"
// directly instead of inferring it.
EM_JS(int, re3_GetAudioSourceCountJS, (), {
	try {
		if (typeof AL !== "undefined" && AL.currentCtx && AL.currentCtx.sources) {
			return Object.keys(AL.currentCtx.sources).length;
		}
	} catch (e) {}
	return 0;
});

EM_JS(int, re3_GetPlayingAudioSourceCountJS, (), {
	try {
		if (typeof AL !== "undefined" && AL.currentCtx && AL.currentCtx.sources) {
			var n = 0;
			for (var id in AL.currentCtx.sources) {
				if (AL.currentCtx.sources[id].state === 0x1012 /* AL_PLAYING */) n++;
			}
			return n;
		}
	} catch (e) {}
	return 0;
});

EM_JS(double, re3_GetMaxPlayingAudioGainJS, (), {
	try {
		if (typeof AL !== "undefined" && AL.currentCtx && AL.currentCtx.sources) {
			var maxGain = -1.0;
			for (var id in AL.currentCtx.sources) {
				var src = AL.currentCtx.sources[id];
				if (src.state === 0x1012 /* AL_PLAYING */) {
					var g = src.gain && src.gain.gain ? src.gain.gain.value : -1.0;
					if (g > maxGain) maxGain = g;
				}
			}
			return maxGain;
		}
	} catch (e) {}
	return -1.0;
});

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetAudioSourceCount(void)
{
	return re3_GetAudioSourceCountJS();
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetPlayingAudioSourceCount(void)
{
	return re3_GetPlayingAudioSourceCountJS();
}

extern "C" EMSCRIPTEN_KEEPALIVE double
re3_GetMaxPlayingAudioGain(void)
{
	return re3_GetMaxPlayingAudioGainJS();
}

// Debug-only: calls the exact same function CMenuManager::ProcessButtonPresses()
// calls when the player confirms "Start New Game" from the menu
// (CMenuManager::DoSettingsBeforeStartingAGame(), Frontend.cpp). Exists purely
// to verify the m_bWantToRestart handling fix above end-to-end without needing
// to drive real keyboard/mouse input through the rendered menu UI from a
// script -- it does not skip, fake, or shortcut any engine state transition;
// it triggers precisely the same one a real click does.
extern "C" EMSCRIPTEN_KEEPALIVE void
re3_DebugTriggerStartNewGame(void)
{
	FrontEndMenuManager.DoSettingsBeforeStartingAGame();
}

// Debug-only: the two flags that gate whether tick()'s GS_PLAYING_GAME case
// ever calls RsEventHandler(rsIDLE, ...) at all (see the switch statement
// below) -- exists to check directly whether "gGameState reached
// GS_PLAYING_GAME but no frame ever renders" is caused by one of these being
// unexpectedly false, rather than guessing from outside.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetRwInitialised(void)
{
	return RwInitialised ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetForegroundApp(void)
{
	return ForegroundApp ? 1 : 0;
}

// Debug-only: main.cpp's Idle() returns immediately, before ever reaching its
// render block or Phase 7's frame-timing instrumentation, if either of these
// is true -- checking directly whether one of them is unexpectedly stuck true
// once gGameState == GS_PLAYING_GAME, rather than guessing.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetWantToRestart(void)
{
	return FrontEndMenuManager.m_bWantToRestart ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE float
re3_GetFrameLimiterMs(void)
{
	return g_re3LastFrameLimiterMs;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetFrameLimiterMaxFPS(void)
{
	return g_re3LastFrameLimiterMaxFPS;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetFrameLimiterEnabled(void)
{
	return g_re3LastFrameLimiterEnabled ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetFrameLimiterWillCall(void)
{
	return g_re3LastFrameLimiterWillCall ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE unsigned int
re3_GetTickCallCount(void)
{
	return g_re3TickCallCount;
}

// Debug-only: raw psTimer()/RsTimer() reading (the clock_gettime(CLOCK_MONOTONIC_RAW)
// -based clock CTimer::GetCurrentTimeInCycles() is built on) sampled directly,
// so it can be compared against two calls spaced by real wall-clock time from
// JS (performance.now()) to prove or disprove whether this specific clock
// source is advancing correctly under Emscripten -- CTimer's own delta-based
// getters can't distinguish "clock is stuck" from "the reference point keeps
// getting reset every frame" on their own.
extern "C" EMSCRIPTEN_KEEPALIVE double
re3_GetRawPsTimer(void)
{
	return RsTimer();
}

// Debug-only: RsGlobal.width/height as SCREEN_WIDTH/SCREEN_HEIGHT (common.h)
// see them at the moment of a crash -- ScreenDroplets::FillScreenMoving()
// (src/extras/screendroplets.cpp) does `% (int)SCREEN_WIDTH`, which traps
// ("remainder by zero") if either is 0 at that point, to find out whether
// that's really happening rather than guessing from the two competing
// RsGlobal.width/height write sites in this file.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetRsGlobalWidth(void)
{
	return RsGlobal.width;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetRsGlobalHeight(void)
{
	return RsGlobal.height;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetCameraRasterWidth(void)
{
	return Scene.camera ? RwRasterGetWidth(RwCameraGetRaster(Scene.camera)) : -1;
}

extern "C" EMSCRIPTEN_KEEPALIVE int
re3_GetCameraRasterHeight(void)
{
	return Scene.camera ? RwRasterGetHeight(RwCameraGetRaster(Scene.camera)) : -1;
}
#endif

/*
 *****************************************************************************
 */
#ifdef _WIN32
int PASCAL
WinMain(HINSTANCE instance,
	HINSTANCE prevInstance	__RWUNUSED__,
	CMDSTR cmdLine,
	int cmdShow)
{

	RwInt32 argc;
	RwChar** argv;
	SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nil, SPIF_SENDCHANGE);

#ifndef MASTER
	if (strstr(cmdLine, "-console"))
	{
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
	}
#endif

#else
int
main(int argc, char *argv[])
{
#endif
	RwV2d pos;
	RwInt32 i;

#ifdef USE_CUSTOM_ALLOCATOR
	InitMemoryMgr();
#endif

#ifndef _WIN32
	struct sigaction act;
	act.sa_sigaction = terminateHandler;
	act.sa_flags = SA_SIGINFO;
	sigaction(SIGTERM, &act, NULL);
#ifdef FLUSHABLE_STREAMING
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = dummyHandler;
	sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, NULL);
#endif
#endif

	/* 
	 * Initialize the platform independent data.
	 * This will in turn initialize the platform specific data...
	 */
	if( RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR )
	{
		return FALSE;
	}

#ifdef _WIN32
	/*
	 * Get proper command line params, cmdLine passed to us does not
	 * work properly under all circumstances...
	 */
	cmdLine = GetCommandLine();

	/*
	 * Parse command line into standard (argv, argc) parameters...
	 */
	argv = CommandLineToArgv(cmdLine, &argc);


	/* 
	 * Parse command line parameters (except program name) one at 
	 * a time BEFORE RenderWare initialization...
	 */
#endif
	for(i=1; i<argc; i++)
	{
		RsEventHandler(rsPREINITCOMMANDLINE, argv[i]);
	}

	/*
	 * Parameters to be used in RwEngineOpen / rsRWINITIALISE event
	 */

#ifdef __EMSCRIPTEN__
	// Canvas/resize debugging pass: RsGlobal.maximumWidth/Height are still at
	// their RsInitialize() skeleton default (DEFAULT_SCREEN_WIDTH/HEIGHT, 640x480,
	// src/skel/skeleton.cpp) at this point -- the *real* desired resolution isn't
	// computed until psSelectDevice() runs, which happens AFTER RwEngineOpen()
	// (rsRWINITIALIZE, just below) has already created the GLFW window/canvas
	// from these exact values. Nothing in the normal boot path ever resizes that
	// already-created window to match psSelectDevice()'s later computation --
	// that correction (psPostRWinit()'s glfwSetWindowSize() call) only runs from
	// _psSetVideoMode(), which is exclusively used for *later*, explicit
	// resolution changes (the Display Settings menu), never during initial boot.
	// This is shared/native code, not something any WASM phase changed, and native
	// windowing/DPI behavior apparently masks the same gap -- but in a browser it
	// means the canvas is permanently created at 640x480 regardless of its actual
	// on-page size, which is exactly the "tiny/blank until something happens to
	// trigger a resize" symptom. Fixed at the source: query the canvas's actual
	// current backing-store size (already correctly computed by web/launcher.js's
	// resizeCanvasToDisplaySize(), which always runs before callMain()) and use
	// that as the real starting resolution instead of the hardcoded default.
	{
		int cw = 0, ch = 0;
		if (emscripten_get_canvas_element_size("#canvas", &cw, &ch) == EMSCRIPTEN_RESULT_SUCCESS && cw > 0 && ch > 0) {
			RsGlobal.maximumWidth = cw;
			RsGlobal.maximumHeight = ch;
			RsGlobal.width = cw;
			RsGlobal.height = ch;
			// Also pre-seed FrontEndMenuManager's prefs so psSelectDevice()'s own
			// "compute a default resolution" block (glfwGetVideoMode() -- reports
			// the browser's screen resolution, not this canvas's actual size) is
			// skipped entirely (it only runs when m_nPrefsWidth == 0) rather than
			// overwriting RsGlobal.maximumWidth/Height with a *different*, mismatched
			// value a few lines later -- that mismatch (camera/viewport sized for one
			// resolution, the actual GL framebuffer created at another) is what
			// produces the "rendered into a tiny sub-region" symptom. A saved
			// gta3.set resolution preference doesn't carry meaningful intent across
			// browser sessions the way it does on a fixed native desktop (canvas
			// layout size can differ every load), so the live canvas size always
			// wins here, native settings-persistence semantics aside.
			FrontEndMenuManager.m_nPrefsWidth = cw;
			FrontEndMenuManager.m_nPrefsHeight = ch;
			FrontEndMenuManager.m_nPrefsDepth = 32;
			FrontEndMenuManager.m_nPrefsWindowed = 1;
		}
	}
#endif
	openParams.width = RsGlobal.maximumWidth;
	openParams.height = RsGlobal.maximumHeight;
	openParams.windowtitle = RsGlobal.appName;
	openParams.window = &PSGLOBAL(window);
	
	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	/* 
	 * Initialize the 3D (RenderWare) components of the app...
	 */
	if( rsEVENTERROR == RsEventHandler(rsRWINITIALIZE, &openParams) )
	{
		RsEventHandler(rsTERMINATE, nil);

		return 0;
	}

#ifdef _WIN32
	HWND wnd = glfwGetWin32Window(PSGLOBAL(window));

	HICON icon = LoadIcon(instance, MAKEINTRESOURCE(IDI_MAIN_ICON));

	SendMessage(wnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
	SendMessage(wnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
#endif

	psPostRWinit();

	ControlsManager.InitDefaultControlConfigMouse(MousePointerStateHelper.GetMouseSetUp());

//	glfwSetWindowPos(PSGLOBAL(window), 0, 0);

	/* 
	 * Parse command line parameters (except program name) one at 
	 * a time AFTER RenderWare initialization...
	 */
	for(i=1; i<argc; i++)
	{
		RsEventHandler(rsCOMMANDLINE, argv[i]);
	}

	/* 
	 * Force a camera resize event...
	 */
	{
		RwRect r;

		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;

		RsEventHandler(rsCAMERASIZE, &r);
	}
#ifdef _WIN32
	SystemParametersInfo(SPI_SETPOWEROFFACTIVE, FALSE, nil, SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETLOWPOWERACTIVE, FALSE, nil, SPIF_SENDCHANGE);
	

	STICKYKEYS SavedStickyKeys;
	SavedStickyKeys.cbSize = sizeof(STICKYKEYS);
	
	SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(STICKYKEYS), &SavedStickyKeys, SPIF_SENDCHANGE);
	
	STICKYKEYS NewStickyKeys;
	NewStickyKeys.cbSize = sizeof(STICKYKEYS);
	NewStickyKeys.dwFlags = SKF_TWOKEYSOFF;
	
	SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &NewStickyKeys, SPIF_SENDCHANGE);
#endif

	{
		CFileMgr::SetDirMyDocuments();
		
#ifdef LOAD_INI_SETTINGS
		// At this point InitDefaultControlConfigJoyPad must have set all bindings to default and ms_padButtonsInited to number of detected buttons.
		// We will load stored bindings below, but let's cache ms_padButtonsInited before LoadINIControllerSettings and LoadSettings clears it,
		// so we can add new joy bindings **on top of** stored bindings.
		int connectedPadButtons = ControlsManager.ms_padButtonsInited;
#endif

		int32 gta3set = CFileMgr::OpenFile("gta3.set", "r");
		
		if ( gta3set )
		{
			ControlsManager.LoadSettings(gta3set);
			CFileMgr::CloseFile(gta3set);
		}
		
		CFileMgr::SetDir("");

#ifdef LOAD_INI_SETTINGS
		LoadINIControllerSettings();
		if (connectedPadButtons != 0) {
			ControlsManager.InitDefaultControlConfigJoyPad(connectedPadButtons);
			SaveINIControllerSettings();
		}
#endif
	}
	
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS);
#endif

#ifdef PS2_MENU
	int32 r = TheMemoryCard.CheckCardStateAtGameStartUp(CARD_ONE);
	if (   r == CMemoryCard::ERR_DIRNOENTRY  || r == CMemoryCard::ERR_NOFORMAT
		&& r != CMemoryCard::ERR_OPENNOENTRY && r != CMemoryCard::ERR_NONE )
	{
		LoadingScreen(nil, nil, "loadsc0");
		
		TheText.Unload();
		TheText.Load();
		
		CFont::Initialise();
		
		FrontEndMenuManager.DrawMemoryCardStartUpMenus();
	}
#endif
	
	initkeymap();

#ifndef __EMSCRIPTEN__
	while ( TRUE )
	{
#endif
		RwInitialised = TRUE;
		
		/* 
		* Set the initial mouse position...
		*/
		pos.x = RsGlobal.maximumWidth * 0.5f;
		pos.y = RsGlobal.maximumHeight * 0.5f;

		RsMouseSetPos(&pos);
		
		/*
		* Enter the message processing loop...
		*/

#ifndef MASTER
		if (gbModelViewer) {
			// This is TheModelViewer in LCS, but not compiled on III Mobile.
			LoadingScreen("Loading the ModelViewer", NULL, GetRandomSplashScreen());
			CAnimViewer::Initialise();
			CTimer::Update();
#ifndef PS2_MENU
			FrontEndMenuManager.m_bGameNotLoaded = false;
#endif
		}
#endif

#ifdef PS2_MENU
		if (TheMemoryCard.m_bWantToLoad)
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
		
		TheMemoryCard.m_bWantToLoad = false;
		
		CTimer::Update();
#endif

#ifdef __EMSCRIPTEN__
	auto tick = []() {
		g_re3TickCallCount++;
#else
#ifdef PS2_MENU
		while( !RsGlobal.quit && !(FrontEndMenuManager.m_bWantToRestart || TheMemoryCard.b_FoundRecentSavedGameWantToLoad) && !glfwWindowShouldClose(PSGLOBAL(window)) )
#else
		while( !RsGlobal.quit && !FrontEndMenuManager.m_bWantToRestart && !glfwWindowShouldClose(PSGLOBAL(window)))
#endif
		{
#endif
			glfwPollEvents();
#ifdef GET_KEYBOARD_INPUT_FROM_X11
			checkKeyPresses();
#endif
#ifndef MASTER
			if (gbModelViewer) {
				// This is TheModelViewerCore in LCS, but TheModelViewer on other state-machine III-VCs.
				TheModelViewer();
			} else
#endif
#ifdef __EMSCRIPTEN__
			// Real transition tracer (task 1 of the post-loading-screen debugging
			// pass): checked every tick, but only ever *prints* when gGameState
			// actually changes -- not a periodic poll. GS_* names mirror
			// src/skel/crossplatform.h. Timestamped with emscripten_get_now() (ms
			// since page load) so gaps between transitions are directly visible in
			// the log, not just their order.
			{
				static RwUInt32 __lastGameState = (RwUInt32)-1;
				if (gGameState != __lastGameState) {
					const char *__gsNames[] = {
						"GS_START_UP", "GS_INIT_LOGO_MPEG", "GS_LOGO_MPEG", "GS_INIT_INTRO_MPEG",
						"GS_INTRO_MPEG", "GS_INIT_ONCE", "GS_INIT_FRONTEND", "GS_FRONTEND",
						"GS_INIT_PLAYING_GAME", "GS_PLAYING_GAME",
					};
					const char *__prevName = (__lastGameState < 10) ? __gsNames[__lastGameState] : "(none)";
					const char *__curName = (gGameState < 10) ? __gsNames[gGameState] : "(unknown)";
					debug("GAME STATE: %s -> %s  [t=%.1fms]\n", __prevName, __curName, emscripten_get_now());
					__lastGameState = gGameState;
				}
			}
#endif
			if ( ForegroundApp )
			{
				switch ( gGameState )
				{
					case GS_START_UP:
					{
#ifdef NO_MOVIES
						gGameState = GS_INIT_ONCE;
#else
						gGameState = GS_INIT_LOGO_MPEG;
#endif
						TRACE("gGameState = GS_INIT_ONCE");
						break;
					}

				    case GS_INIT_LOGO_MPEG:
					{
					    //if (!startupDeactivate)
						//    PlayMovieInWindow(cmdShow, "movies\\Logo.mpg");
					    gGameState = GS_LOGO_MPEG;
					    TRACE("gGameState = GS_LOGO_MPEG;");
					    break;
				    }

				    case GS_LOGO_MPEG:
					{
//					    CPad::UpdatePads();

//					    if (startupDeactivate || ControlsManager.GetJoyButtonJustDown() != 0)
						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetLeftMouseJustDown())
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetEnterJustDown())
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetCharJustDown(' '))
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetAltJustDown())
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetTabJustDown())
//						    ++gGameState;

					    break;
				    }

				    case GS_INIT_INTRO_MPEG:
					{
//#ifndef NO_MOVIES
//					    CloseClip();
//					    CoUninitialize();
//#endif
//
//					    if (CMenuManager::OS_Language == LANG_FRENCH || CMenuManager::OS_Language == LANG_GERMAN)
//						    PlayMovieInWindow(cmdShow, "movies\\GTAtitlesGER.mpg");
//					    else
//						    PlayMovieInWindow(cmdShow, "movies\\GTAtitles.mpg");

					    gGameState = GS_INTRO_MPEG;
					    TRACE("gGameState = GS_INTRO_MPEG;");
					    break;
				    }

				    case GS_INTRO_MPEG:
					{
//					    CPad::UpdatePads();
//
//					    if (startupDeactivate || ControlsManager.GetJoyButtonJustDown() != 0)
						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetLeftMouseJustDown())
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetEnterJustDown())
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetCharJustDown(' '))
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetAltJustDown())
//						    ++gGameState;
//					    else if (CPad::GetPad(0)->GetTabJustDown())
//						    ++gGameState;

					    break;
				    }

					case GS_INIT_ONCE:
					{
						//CoUninitialize();
						
#ifdef PS2_MENU
						extern char version_name[64];
						if ( CGame::frenchGame || CGame::germanGame )
							LoadingScreen(NULL, version_name, "loadsc24");
						else
							LoadingScreen(NULL, version_name, "loadsc0");
						
						printf("Into TheGame!!!\n");
#else				
						LoadingScreen(nil, nil, "loadsc0");
#endif
						if ( !CGame::InitialiseOnceAfterRW() )
							RsGlobal.quit = TRUE;
						
#ifdef PS2_MENU
						gGameState = GS_INIT_PLAYING_GAME;
#else
						gGameState = GS_INIT_FRONTEND;
						TRACE("gGameState = GS_INIT_FRONTEND;");
#ifdef __EMSCRIPTEN__
						re3_SetBootStage(6); // [6] Engine initialized
#endif
#endif
						break;
					}

#ifndef PS2_MENU
					case GS_INIT_FRONTEND:
					{
						LoadingScreen(nil, nil, "loadsc0");

						FrontEndMenuManager.m_bGameNotLoaded = true;

						CMenuManager::m_bStartUpFrontEndRequested = true;

						if ( defaultFullscreenRes )
						{
							defaultFullscreenRes = FALSE;
							FrontEndMenuManager.m_nPrefsVideoMode = GcurSelVM;
							FrontEndMenuManager.m_nDisplayVideoMode = GcurSelVM;
						}

						gGameState = GS_FRONTEND;
						TRACE("gGameState = GS_FRONTEND;");
#ifdef __EMSCRIPTEN__
						re3_SetBootStage(7); // [7] Main menu
#endif
						break;
					}
					
					case GS_FRONTEND:
					{
#ifdef __EMSCRIPTEN__
						// Same fixed frame budget (RE3_EMSCRIPTEN_FRAME_BUDGET_MS) and scheduler
						// as GS_PLAYING_GAME below --
						// shared between them (only one of the two states is ever active at
						// once), so the cadence stays consistent across the menu<->gameplay
						// transition instead of resetting.
						bool __willCallFrontendIdle = re3EmscriptenFrameDue();
#else
						bool __willCallFrontendIdle = true;
#endif
						if(!WindowIconified && __willCallFrontendIdle)
							RsEventHandler(rsFRONTENDIDLE, nil);

#ifdef PS2_MENU
						if ( !FrontEndMenuManager.m_bMenuActive || TheMemoryCard.m_bWantToLoad )
#else
						if ( !FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad )
#endif
						{
							gGameState = GS_INIT_PLAYING_GAME;
							TRACE("gGameState = GS_INIT_PLAYING_GAME;");
						}

#ifdef PS2_MENU
						if (TheMemoryCard.m_bWantToLoad )
#else
						if ( FrontEndMenuManager.m_bWantToLoad )
#endif
						{
							InitialiseGame();
							FrontEndMenuManager.m_bGameNotLoaded = false;
							gGameState = GS_PLAYING_GAME;
							TRACE("gGameState = GS_PLAYING_GAME;");
						}
						break;
					}
#endif
					
					case GS_INIT_PLAYING_GAME:
					{
#ifdef __EMSCRIPTEN__
						debug("GAMESTATE: FRONTEND -> INITIALISING (GS_INIT_PLAYING_GAME)\n");
#endif
#ifdef PS2_MENU
						CGame::Initialise("DATA\\GTA3.DAT");
						
						//LoadingScreen("Starting Game", NULL, GetRandomSplashScreen());
					
						if (   TheMemoryCard.CheckCardInserted(CARD_ONE) == CMemoryCard::NO_ERR_SUCCESS
							&& TheMemoryCard.ChangeDirectory(CARD_ONE, TheMemoryCard.Cards[CARD_ONE].dir)
							&& TheMemoryCard.FindMostRecentFileName(CARD_ONE, TheMemoryCard.MostRecentFile) == true
							&& TheMemoryCard.CheckDataNotCorrupt(TheMemoryCard.MostRecentFile))
						{
							strcpy(TheMemoryCard.LoadFileName, TheMemoryCard.MostRecentFile);
							TheMemoryCard.b_FoundRecentSavedGameWantToLoad = true;
					
							if (CMenuManager::m_PrefsLanguage != TheMemoryCard.GetLanguageToLoad())
							{
								CMenuManager::m_PrefsLanguage = TheMemoryCard.GetLanguageToLoad();
								TheText.Unload();
								TheText.Load();
							}
					
							CGame::currLevel = (eLevelName)TheMemoryCard.GetLevelToLoad();
						}
#else
						InitialiseGame();

						FrontEndMenuManager.m_bGameNotLoaded = false;
#endif
						gGameState = GS_PLAYING_GAME;
						TRACE("gGameState = GS_PLAYING_GAME;");
#ifdef __EMSCRIPTEN__
						debug("GAMESTATE: -> PLAYING_GAME\n");
#endif
						break;
					}

					case GS_PLAYING_GAME:
					{
						float ms = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerMillisecond();
						if ( RwInitialised )
						{
#ifdef __EMSCRIPTEN__
							bool willCallIdle = re3EmscriptenFrameDue();
#else
							bool willCallIdle = !CMenuManager::m_PrefsFrameLimiter || (1000.0f / (float)RsGlobal.maxFPS) < ms;
#endif
#ifdef __EMSCRIPTEN__
							g_re3LastFrameLimiterMs = ms;
							g_re3LastFrameLimiterMaxFPS = RsGlobal.maxFPS;
							g_re3LastFrameLimiterEnabled = CMenuManager::m_PrefsFrameLimiter;
							g_re3LastFrameLimiterWillCall = willCallIdle;
#endif
							if (willCallIdle)
								RsEventHandler(rsIDLE, (void *)TRUE);
						}
						break;
					}
				}
			}
			else
			{
				if ( RwCameraBeginUpdate(Scene.camera) )
				{
					RwCameraEndUpdate(Scene.camera);
					ForegroundApp = TRUE;
					RsEventHandler(rsACTIVATE, (void *)TRUE);
				}

			}
#ifdef __EMSCRIPTEN__
			if ( RsGlobal.quit || glfwWindowShouldClose(PSGLOBAL(window)) ) {
				emscripten_cancel_main_loop();
			} else if ( FrontEndMenuManager.m_bWantToRestart ) {
				// m_bWantToRestart is NOT a quit signal -- it's how the menu (e.g.
				// "Start New Game", DoSettingsBeforeStartingAGame() in Frontend.cpp)
				// tells the platform loop "break out and re-initialise", exactly the
				// same as it does natively. On native (the #else branch below, and
				// src/skel/win/win.cpp's equivalent), the outer while(TRUE) loop is
				// what receives that break: it falls out of the inner tick loop, runs
				// this exact restart block, resets the flag, and loops back in to a
				// fresh inner loop. There is no such outer loop here -- emscripten_
				// set_main_loop() is only ever registered once -- so treating this
				// flag the same as a real quit (as this code used to) called
				// emscripten_cancel_main_loop() and never came back: the entire engine
				// stopped ticking the moment "Start New Game" was confirmed, which is
				// why gGameState could reach GS_INIT_PLAYING_GAME (set earlier this
				// same tick) but never actually get processed. Running the restart
				// block inline, then continuing (not cancelling), is the fix.
				debug("GAMESTATE: restart handling entered (m_bWantToRestart, m_bWantToLoad=%d)\n", FrontEndMenuManager.m_bWantToLoad);
				RwInitialised = FALSE;
				FrontEndMenuManager.UnloadTextures();

				CPad::ResetCheats();
				CPad::StopPadsShaking();

				DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);

				CTimer::Stop();

				if ( FrontEndMenuManager.m_bWantToLoad )
				{
					CGame::ShutDownForRestart();
					CGame::InitialiseWhenRestarting();
					DMAudio.ChangeMusicMode(MUSICMODE_GAME);
					LoadSplash(GetLevelSplashScreen(CGame::currLevel));
					FrontEndMenuManager.m_bWantToLoad = false;
				}
				else
				{
					if ( gGameState == GS_PLAYING_GAME )
						CGame::ShutDown();
					CTimer::Stop();
					if ( FrontEndMenuManager.m_bFirstTime == true )
						gGameState = GS_INIT_FRONTEND;
					else
						gGameState = GS_INIT_PLAYING_GAME;
				}

				FrontEndMenuManager.m_bFirstTime = false;
				FrontEndMenuManager.m_bWantToRestart = false;

				RwInitialised = TRUE;
			}
#endif
		}
#ifdef __EMSCRIPTEN__
		;
		g_re3TickFn = tick;
		emscripten_set_main_loop(tick, 0, 1);
#else


		/*
		* About to shut down - block resize events again...
		*/
		RwInitialised = FALSE;
		
		FrontEndMenuManager.UnloadTextures();
#ifdef PS2_MENU	
		if ( !(FrontEndMenuManager.m_bWantToRestart || TheMemoryCard.b_FoundRecentSavedGameWantToLoad))
			break;
#else
		if ( !FrontEndMenuManager.m_bWantToRestart )
			break;
#endif
		
		CPad::ResetCheats();
		CPad::StopPadsShaking();
		
		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
		
#ifdef PS2_MENU
		CGame::ShutDownForRestart();
#endif
		
		CTimer::Stop();
		
#ifdef PS2_MENU
		if (FrontEndMenuManager.m_bWantToRestart || TheMemoryCard.b_FoundRecentSavedGameWantToLoad)
		{
			if (TheMemoryCard.b_FoundRecentSavedGameWantToLoad)
			{
				FrontEndMenuManager.m_bWantToRestart = true;
				TheMemoryCard.m_bWantToLoad = true;
			}

			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			FrontEndMenuManager.m_bWantToRestart = false;
			
			continue;
		}
		
		CGame::ShutDown();	
		CTimer::Stop();
		
		break;
#else
		if ( FrontEndMenuManager.m_bWantToLoad )
		{
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
		}
		else
		{
#ifndef MASTER
			if ( gbModelViewer )
				CAnimViewer::Shutdown();
			else
#endif
			if ( gGameState == GS_PLAYING_GAME )
				CGame::ShutDown();
			
			CTimer::Stop();
			
			if ( FrontEndMenuManager.m_bFirstTime == true )
			{
				gGameState = GS_INIT_FRONTEND;
				TRACE("gGameState = GS_INIT_FRONTEND;");
			}
			else
			{
				gGameState = GS_INIT_PLAYING_GAME;
				TRACE("gGameState = GS_INIT_PLAYING_GAME;");
			}
		}
		
		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
#endif
	}
#endif



#ifndef MASTER
	if ( gbModelViewer )
		CAnimViewer::Shutdown();
	else
#endif
	if ( gGameState == GS_PLAYING_GAME )
		CGame::ShutDown();

	DMAudio.Terminate();
	
	_psFreeVideoModeList();


	/*
	 * Tidy up the 3D (RenderWare) components of the application...
	 */
	RsEventHandler(rsRWTERMINATE, nil);

	/*
	 * Free the platform dependent data...
	 */
	RsEventHandler(rsTERMINATE, nil);

#ifdef _WIN32
	/* 
	 * Free the argv strings...
	 */
	free(argv);
	
	SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &SavedStickyKeys, SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETPOWEROFFACTIVE, TRUE, nil, SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETLOWPOWERACTIVE, TRUE, nil, SPIF_SENDCHANGE);
	SetErrorMode(0);
#endif

	return 0;
}

/*
 *****************************************************************************
 */

RwV2d leftStickPos;
RwV2d rightStickPos;

void CapturePad(RwInt32 padID)
{
	int8 glfwPad = -1;

	if( padID == 0 )
		glfwPad = PSGLOBAL(joy1id);
	else if( padID == 1)
		glfwPad = PSGLOBAL(joy2id);
	else
		assert("invalid padID");
	
	if ( glfwPad == -1 )
		return;
	
	int numButtons, numAxes;
	const uint8 *buttons = glfwGetJoystickButtons(glfwPad, &numButtons);
	const float *axes = glfwGetJoystickAxes(glfwPad, &numAxes);
	GLFWgamepadstate gamepadState;

	if (ControlsManager.m_bFirstCapture == false) {
		memcpy(&ControlsManager.m_OldState, &ControlsManager.m_NewState, sizeof(ControlsManager.m_NewState));
	} else {
		// In case connected gamepad doesn't have L-R trigger axes.
		ControlsManager.m_NewState.mappedButtons[15] = ControlsManager.m_NewState.mappedButtons[16] = 0;
	}

	ControlsManager.m_NewState.buttons = (uint8*)buttons;
	ControlsManager.m_NewState.numButtons = numButtons;
	ControlsManager.m_NewState.id = glfwPad;
#ifndef __EMSCRIPTEN__
	// Emscripten's GLFW3 port doesn't implement glfwGetGamepadState (SDL-style mapped
	// gamepad input); raw joystick buttons/axes above are still read via the browser's
	// Gamepad API.
	ControlsManager.m_NewState.isGamepad = glfwGetGamepadState(glfwPad, &gamepadState);
	if (ControlsManager.m_NewState.isGamepad) {
		memcpy(&ControlsManager.m_NewState.mappedButtons, gamepadState.buttons, sizeof(gamepadState.buttons));
		float lt = gamepadState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER], rt = gamepadState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER];

		// glfw returns 0.0 for non-existent axises(which is bullocks) so we treat it as deadzone, and keep value of previous frame.
		// otherwise if this axis is present, -1 = released, 1 = pressed
		if (lt != 0.0f)
			ControlsManager.m_NewState.mappedButtons[15] = lt > -0.8f;

		if (rt != 0.0f)
			ControlsManager.m_NewState.mappedButtons[16] = rt > -0.8f;
	}
#else
	ControlsManager.m_NewState.isGamepad = false;
#endif
	// TODO? L2-R2 axes(not buttons-that's fine) on joysticks that don't have SDL gamepad mapping AREN'T handled, and I think it's impossible to do without mapping.

	if (ControlsManager.m_bFirstCapture == true) {
		memcpy(&ControlsManager.m_OldState, &ControlsManager.m_NewState, sizeof(ControlsManager.m_NewState));
		
		ControlsManager.m_bFirstCapture = false;
	}

	RsPadButtonStatus bs;
	bs.padID = padID;

	RsPadEventHandler(rsPADBUTTONUP, (void *)&bs);
	
	// Gamepad axes are guaranteed to return 0.0f if that particular gamepad doesn't have that axis.
	// And that's really good for sticks, because gamepads return 0.0 for them when sticks are in released state.
	if ( glfwPad != -1 ) {
		leftStickPos.x = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] : numAxes >= 1 ? axes[0] : 0.0f;
		leftStickPos.y = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] : numAxes >= 2 ? axes[1] : 0.0f;

		rightStickPos.x = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] : numAxes >= 3 ? axes[2] : 0.0f;
		rightStickPos.y = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] : numAxes >= 4 ? axes[3] : 0.0f;
	}
	
	{
		if (CPad::m_bMapPadOneToPadTwo)
			bs.padID = 1;
		
		RsPadEventHandler(rsPADBUTTONUP,   (void *)&bs);
		RsPadEventHandler(rsPADBUTTONDOWN, (void *)&bs);
	}
	
	{
		if (CPad::m_bMapPadOneToPadTwo)
			bs.padID = 1;
		
		CPad *pad = CPad::GetPad(bs.padID);

		if ( Abs(leftStickPos.x)  > 0.3f )
			pad->PCTempJoyState.LeftStickX	= (int32)(leftStickPos.x  * 128.0f);
		
		if ( Abs(leftStickPos.y)  > 0.3f )
			pad->PCTempJoyState.LeftStickY	= (int32)(leftStickPos.y  * 128.0f);
		
		if ( Abs(rightStickPos.x) > 0.3f )
			pad->PCTempJoyState.RightStickX = (int32)(rightStickPos.x * 128.0f);

		if ( Abs(rightStickPos.y) > 0.3f )
			pad->PCTempJoyState.RightStickY = (int32)(rightStickPos.y * 128.0f);
	}
	
	return;
}

void joysChangeCB(int jid, int event)
{
	if (event == GLFW_CONNECTED && !IsThisJoystickBlacklisted(jid)) {
		if (PSGLOBAL(joy1id) == -1) {
			PSGLOBAL(joy1id) = jid;
#ifdef DETECT_JOYSTICK_MENU
			strcpy(gSelectedJoystickName, glfwGetJoystickName(jid));
#endif
			// This is behind LOAD_INI_SETTINGS, because otherwise the Init call below will destroy/overwrite your bindings.
#ifdef LOAD_INI_SETTINGS
			int count;
			glfwGetJoystickButtons(PSGLOBAL(joy1id), &count);
			ControlsManager.InitDefaultControlConfigJoyPad(count);
#endif
		} else if (PSGLOBAL(joy2id) == -1)
			PSGLOBAL(joy2id) = jid;

	} else if (event == GLFW_DISCONNECTED) {
		if (PSGLOBAL(joy1id) == jid) {
			PSGLOBAL(joy1id) = -1;
		} else if (PSGLOBAL(joy2id) == jid)
			PSGLOBAL(joy2id) = -1;
	}
}

#if (defined(_MSC_VER))
int strcasecmp(const char* str1, const char* str2)
{
	return _strcmpi(str1, str2);
}
#endif
#endif
