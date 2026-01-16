#pragma once

#include <Geode/Geode.hpp>
#include "linuxeventcodes.hpp"

enum DeviceType : int8_t {
    MOUSE,
    TOUCHPAD,
    KEYBOARD,
    TOUCHSCREEN,
    CONTROLLER,
    UNKNOWN
};

// MSVC compatibility fix: Use #pragma pack instead of __attribute__
#pragma pack(push, 1)
struct LinuxInputEvent {
    LARGE_INTEGER time;
    USHORT type;
    USHORT code;
    int value;
    DeviceType deviceType;
};
#pragma pack(pop)

extern HANDLE hSharedMem;
extern HANDLE hMutex;
extern LPVOID pBuf;

extern bool linuxNative;

inline LARGE_INTEGER largeFromTimestamp(TimestampType t) {
    LARGE_INTEGER res;
    res.QuadPart = t;
    return res;
}

inline TimestampType timestampFromLarge(LARGE_INTEGER l) {
    return l.QuadPart;
}

constexpr size_t BUFFER_SIZE = 20;

void windowsSetup();
void linuxCheckInputs();
void rawInputThread();