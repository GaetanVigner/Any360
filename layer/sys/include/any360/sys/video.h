// xboxkrnl.exe video driver exports (Vd*) and xam's XGetVideoMode: the display mode the layer presents and the
// structures the kernel fills from it. The GPU-facing ones (ring buffer, interrupts, swap) belong to any360::gfx.
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

// The display the layer reports; defaults to 1280x720 at 60 Hz (the reference runtime's).
struct VideoSettings {
  uint32_t width = 1280, height = 720;
  float refresh_rate = 60.0f;
};
VideoSettings& Video();

// X_VIDEO_MODE (48 bytes): width, height, interlaced, widescreen, hi-def, refresh rate, standard, 0x4A, 1, reserved.
void VdQueryVideoMode(Guest g, uint32_t video_mode);
uint32_t VdQueryVideoFlags();
// X_DISPLAY_INFO (0x58 bytes).
void VdGetCurrentDisplayInformation(Guest g, uint32_t display_info);
void VdGetCurrentDisplayGamma(Guest g, uint32_t type, uint32_t power);
uint32_t VdSetDisplayMode(uint32_t flags);
uint32_t VdEnableDisableClockGating(uint32_t enabled);
uint32_t VdIsHSIOTrainingSucceeded();
uint32_t VdRetrainEDRAM();
uint32_t VdRetrainEDRAMWorker();
uint32_t VdGetGraphicsAsicID();
void VdGetSystemCommandBuffer(Guest g, uint32_t p0, uint32_t p1);
uint32_t VdCallGraphicsNotificationRoutines();
// Fills the destination with `count` NOP words; returns the count.
uint32_t VdInitializeScalerCommandBuffer(Guest g, uint32_t destination, uint32_t count);

}  // namespace any360::sys
