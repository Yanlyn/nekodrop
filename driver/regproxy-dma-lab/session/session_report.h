// SPDX-License-Identifier: MIT
#pragma once

// Windows DEVPROP_TYPE_BINARY. All fields are little-endian 32-bit words.
// Include the Windows or WDK headers before this file.
static const DEVPROPKEY sessionReportKey = {
    {0xb81473c0, 0x37ab, 0x48a1, {0xb1, 0x9d, 0xe5, 0xf6, 0x74, 0x70, 0x13, 0xff}}, 3
};
enum { SessionReportVersion = 1, SessionBufferBytes = 4096 };

typedef struct {
    ULONG version, ready;
    ULONG opens, cleanups, pnpReclaims;
    ULONG active, heldBytes;
    ULONG prepareTimeLow, prepareTimeHigh;
} SESSION_REPORT;
C_ASSERT(sizeof(SESSION_REPORT) == 9 * sizeof(ULONG));
