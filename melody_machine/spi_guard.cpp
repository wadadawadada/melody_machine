#include "spi_guard.h"
#include <LilyGoLib.h>

// Use the display library's own SPI mutex so SD and display
// share a single lock. pushColors(x1,y1,w,h) in LilyGoDispInterface
// now holds this lock atomically across setAddrWindow+pixel transfer,
// so SD reads cannot interleave and corrupt the SPI transaction.

void spiGuardInit() {}

bool spiGuardLock(TickType_t timeoutTicks) {
    return instance.lockSPI(timeoutTicks);
}

void spiGuardUnlock() {
    instance.unlockSPI();
}
