#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"

#include "FileMgr.h"
#include "Font.h"
#ifdef MORE_LANGUAGES
#include "Game.h"
#endif
#include "GenericGameStorage.h"
#include "Messages.h"
#include "PCSave.h"
#include "Text.h"
#include "Frontend.h"
#include "Lists.h"
#include "PlayerInfo.h"
#include "World.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// docs/SAVES_WASM.md: notifies the browser-side persistence layer (web/saves.js)
// that the save directory (userfiles/, mounted as its own dedicated IDBFS --
// separate from the game-asset IDBFS mount -- in web/launcher.js) has changed
// on the in-memory virtual filesystem and needs to be flushed to IndexedDB.
// Called directly from C++ (SaveSlot()/DeleteSlot() below), so -- unlike the
// EM_JS exports elsewhere in this codebase that are only ever reached via
// ccall from JS -- this one doesn't need an EMSCRIPTEN_KEEPALIVE C wrapper to
// be retained by the linker.
EM_JS(void, re3_NotifySaveDirChangedJS, (const char *reason, const char *filename), {
	if (window.Re3Saves && window.Re3Saves.onSaveDirChanged)
		window.Re3Saves.onSaveDirChanged(UTF8ToString(reason), UTF8ToString(filename));
});

// docs/SAVES_WASM.md validation: calls the exact same C_PcSave::SaveSlot()
// used by the pause-menu "Save Game" screen (src/core/Frontend.cpp's
// MENUPAGE_SAVING_IN_PROGRESS handler), so browser-side testing exercises the
// real save path without needing to drive that menu's UI. Off the native
// save/load code path otherwise; only reachable via an explicit ccall from
// browser JS/devtools, never called automatically.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_DebugTriggerSave(int slot)
{
	if (gGameState != GS_PLAYING_GAME)
		return -1;
	bool ok = PcSaveHelper.SaveSlot(slot);
	PcSaveHelper.PopulateSlotInfo();
	return ok ? 1 : 0;
}

// docs/SAVES_WASM.md validation: runs the exact same slot scan the frontend
// menu runs when opening the Load/Save/Delete pages (src/core/Frontend.cpp,
// e.g. the MENUPAGE_LOADING_IN_PROGRESS / *_SAVING_IN_PROGRESS handlers all
// call this before reading Slots[]) -- confirms a restored save is actually
// *found*, not just present as bytes on the virtual filesystem. Returns
// Slots[slot+1] (see the SLOT_OK/SLOT_EMPTY/SLOT_CORRUPTED enum in PCSave.h).
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_DebugScanSlot(int slot)
{
	PcSaveHelper.PopulateSlotInfo(); // declared in GenericGameStorage.h, already #included above
	return Slots[slot + 1];
}

// docs/SAVES_WASM.md validation: read/write CWorld::Players[0].m_nMoney -- a
// simple, easy-to-verify-by-eye field -- so a test can set a distinctive value,
// save, reload the page, load the save back, and confirm the *exact* value
// round-tripped through GenericSave()/GenericLoad() rather than just checking
// "a save file exists" or "the slot scan found something".
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_DebugGetPlayerMoney(void)
{
	return CWorld::Players[0].m_nMoney;
}

extern "C" EMSCRIPTEN_KEEPALIVE void
re3_DebugSetPlayerMoney(int amount)
{
	CWorld::Players[0].m_nMoney = amount;
}

// docs/SAVES_WASM.md validation: sets m_nCurrSaveSlot and m_nCurrScreen exactly
// as the frontend's own slot-confirm handler does right before the player would
// see the "Loading..." transition (see MENUPAGE_LOAD_SLOT_CONFIRM's confirm
// button in src/core/Frontend.cpp), then lets the engine's own, completely
// unmodified CMenuManager::Process() -- called every tick via
// RsEventHandler(rsFRONTENDIDLE, ...) while gGameState==GS_FRONTEND -- notice
// m_nCurrScreen==MENUPAGE_LOADING_IN_PROGRESS on its own next call and do
// everything else itself: CheckSlotDataValid(), setting m_bWantToRestart/
// m_bWantToLoad/b_FoundRecentSavedGameWantToLoad, RequestFrontEndShutDown().
// Deliberately does NOT set those flags directly here: CMenuManager::Process()
// unconditionally resets m_bWantToRestart=false at its own top on every call
// (Frontend.cpp), so setting it externally between ticks just gets clobbered
// before the Emscripten-specific restart handler in tick() (glfw.cpp) ever
// sees it -- driving the same m_nCurrScreen state a real menu confirm reaches
// is what actually reproduces the real "Load Game" flow.
extern "C" EMSCRIPTEN_KEEPALIVE int
re3_DebugTriggerLoad(int slot)
{
	if (gGameState != GS_FRONTEND)
		return -1;
	if (Slots[slot + 1] != SLOT_OK)
		return -2;
	FrontEndMenuManager.m_nCurrSaveSlot = slot;
	FrontEndMenuManager.m_nCurrScreen = MENUPAGE_LOADING_IN_PROGRESS;
	return 1;
}
#endif

const char* _psGetUserFilesFolder();

C_PcSave PcSaveHelper;

void
C_PcSave::SetSaveDirectory(const char *path)
{
	sprintf(DefaultPCSaveFileName, "%s\\%s", path, "GTA3sf");
}

bool
C_PcSave::DeleteSlot(int32 slot)
{
	char FileName[200];

	PcSaveHelper.nErrorCode = SAVESTATUS_SUCCESSFUL;
	sprintf(FileName, "%s%i.b", DefaultPCSaveFileName, slot + 1);
	DeleteFile(FileName);
	SlotSaveDate[slot][0] = '\0';
#ifdef __EMSCRIPTEN__
	re3_NotifySaveDirChangedJS("delete", FileName);
#endif
	return true;
}

bool
C_PcSave::SaveSlot(int32 slot)
{
	MakeValidSaveName(slot);
	PcSaveHelper.nErrorCode = SAVESTATUS_SUCCESSFUL;
	_psGetUserFilesFolder();
	int file = CFileMgr::OpenFile(ValidSaveName, "wb");
	if (file != 0) {
#ifdef MISSION_REPLAY
		if (!IsQuickSave)
#endif
			DoGameSpecificStuffBeforeSave();
		if (GenericSave(file)) {
			if (!!CFileMgr::CloseFile(file))
				nErrorCode = SAVESTATUS_ERR_SAVE_CLOSE;
#ifdef __EMSCRIPTEN__
			re3_NotifySaveDirChangedJS("save", ValidSaveName);
#endif
			return true;
		}

		return false;
	}
	PcSaveHelper.nErrorCode = SAVESTATUS_ERR_SAVE_CREATE;
	return false;
}

bool
C_PcSave::PcClassSaveRoutine(int32 file, uint8 *data, uint32 size)
{
	CFileMgr::Write(file, (const char*)&size, sizeof(size));
	if (CFileMgr::GetErrorReadWrite(file)) {
		nErrorCode = SAVESTATUS_ERR_SAVE_WRITE;
		strncpy(SaveFileNameJustSaved, ValidSaveName, sizeof(ValidSaveName) - 1);
		return false;
	}

	CFileMgr::Write(file, (const char*)data, align4bytes(size));
	CheckSum += (uint8) size;
	CheckSum += (uint8) (size >> 8);
	CheckSum += (uint8) (size >> 16);
	CheckSum += (uint8) (size >> 24);
	for (int i = 0; i < align4bytes(size); i++) {
		CheckSum += *data++;
	}
	if (CFileMgr::GetErrorReadWrite(file)) {
		nErrorCode = SAVESTATUS_ERR_SAVE_WRITE;
		strncpy(SaveFileNameJustSaved, ValidSaveName, sizeof(ValidSaveName) - 1);
		return false;
	}

	return true;
}

void
C_PcSave::PopulateSlotInfo()
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		Slots[i + 1] = SLOT_EMPTY;
		SlotFileName[i][0] = '\0';
		SlotSaveDate[i][0] = '\0';
	}
	for (int i = 0; i < SLOT_COUNT; i++) {
#ifdef FIX_BUGS
		char savename[MAX_PATH];
#else
		char savename[52];
#endif
		struct {
			int size;
			wchar FileName[24];
			SYSTEMTIME SaveDateTime;
		} header;
		sprintf(savename, "%s%i%s", DefaultPCSaveFileName, i + 1, ".b");
		int file = CFileMgr::OpenFile(savename, "rb");
		if (file != 0) {
			CFileMgr::Read(file, (char*)&header, sizeof(header));
			if (strncmp((char*)&header, TopLineEmptyFile, sizeof(TopLineEmptyFile)-1) != 0) {
				Slots[i + 1] = SLOT_OK;
				memcpy(SlotFileName[i], &header.FileName, sizeof(header.FileName));
				
				SlotFileName[i][24] = '\0';
			}
			CFileMgr::CloseFile(file);
		}
		if (Slots[i + 1] == SLOT_OK) {
			if (CheckDataNotCorrupt(i, savename)) {
				SYSTEMTIME st;
				memcpy(&st, &header.SaveDateTime, sizeof(SYSTEMTIME));
				const char *month;
				switch (st.wMonth)
				{
				case 1: month = "JAN"; break;
				case 2: month = "FEB"; break;
				case 3: month = "MAR"; break;
				case 4: month = "APR"; break;
				case 5: month = "MAY"; break;
				case 6: month = "JUN"; break;
				case 7: month = "JUL"; break;
				case 8: month = "AUG"; break;
				case 9: month = "SEP"; break;
				case 10: month = "OCT"; break;
				case 11: month = "NOV"; break;
				case 12: month = "DEC"; break;
				default: assert(0);
				}
				char date[70];
#ifdef MORE_LANGUAGES
				if (CGame::japaneseGame)
					sprintf(date, "%02d %02d %04d %02d:%02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
				else
#endif // MORE_LANGUAGES
					sprintf(date, "%02d %s %04d %02d:%02d:%02d", st.wDay, UnicodeToAsciiForSaveLoad(TheText.Get(month)), st.wYear, st.wHour, st.wMinute, st.wSecond);
				AsciiToUnicode(date, SlotSaveDate[i]);

			} else {
				CMessages::InsertNumberInString(TheText.Get("FEC_SLC"), i + 1, -1, -1, -1, -1, -1, SlotFileName[i]);
				Slots[i + 1] = SLOT_CORRUPTED;
			}
		}
	}
}
