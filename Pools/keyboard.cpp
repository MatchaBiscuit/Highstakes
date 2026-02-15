/*
	THIS FILE IS A PART OF RDR 2 SCRIPT HOOK SDK
				http://dev-c.com
			(C) Alexander Blade 2019
*/

#include "keyboard.h"

const int KEYS_SIZE = 256;

struct KeyState
{
	bool isDown;
	ULONGLONG downSinceMs;
	ULONGLONG lastChangeMs;
	ULONGLONG pressSeq;
	ULONGLONG releaseSeq;
	ULONGLONG consumedPressSeq;
	ULONGLONG consumedReleaseSeq;
};

KeyState keyStates[KEYS_SIZE]{};
ULONGLONG gEventSeq = 0;

void OnKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow)
{
	if (key < KEYS_SIZE)
	{
		KeyState& state = keyStates[key];
		ULONGLONG now = GetTickCount64();

		if (!isUpNow && !wasDownBefore)
		{
			state.isDown = true;
			state.downSinceMs = now;
			state.lastChangeMs = now;
			state.pressSeq = ++gEventSeq;
		}
		else if (isUpNow && state.isDown)
		{
			state.isDown = false;
			state.lastChangeMs = now;
			state.releaseSeq = ++gEventSeq;
		}
	}
}

bool KeyPressed(DWORD key, bool consume)
{
	if (key >= KEYS_SIZE)
		return false;
	KeyState& state = keyStates[key];
	bool pressed = (state.pressSeq != 0) && (state.pressSeq != state.consumedPressSeq);
	if (pressed && consume)
		state.consumedPressSeq = state.pressSeq;
	return pressed;
}

bool KeyReleased(DWORD key, bool consume)
{
	if (key >= KEYS_SIZE)
		return false;
	KeyState& state = keyStates[key];
	bool released = (state.releaseSeq != 0) && (state.releaseSeq != state.consumedReleaseSeq);
	if (released && consume)
		state.consumedReleaseSeq = state.releaseSeq;
	return released;
}

bool KeyHeld(DWORD key)
{
	return (key < KEYS_SIZE) ? keyStates[key].isDown : false;
}

DWORD KeyHeldMs(DWORD key)
{
	if (key >= KEYS_SIZE || !keyStates[key].isDown)
		return 0;
	ULONGLONG elapsed = GetTickCount64() - keyStates[key].downSinceMs;
	if (elapsed > 0xFFFFFFFFull)
		return 0xFFFFFFFFu;
	return (DWORD)elapsed;
}

void ClearKey(DWORD key)
{
	if (key < KEYS_SIZE)
		memset(&keyStates[key], 0, sizeof(keyStates[key]));
}

void ClearAllKeys()
{
	memset(keyStates, 0, sizeof(keyStates));
}
