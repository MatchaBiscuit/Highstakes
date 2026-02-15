/*
		THIS FILE IS A PART OF RDR 2 SCRIPT HOOK SDK
					http://dev-c.com
				(C) Alexander Blade 2019
*/

#pragma once

#include <windows.h>

void OnKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow);

bool KeyPressed(DWORD key, bool consume = true);
bool KeyReleased(DWORD key, bool consume = true);
bool KeyHeld(DWORD key);
DWORD KeyHeldMs(DWORD key);
void ClearKey(DWORD key);
void ClearAllKeys();
