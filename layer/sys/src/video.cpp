#include "any360/sys/video.h"

#include <cstring>

namespace any360::sys {

namespace {

constexpr uint32_t kDisplayGammaType = 2;  // TV (BT.709)
constexpr float kDisplayGammaPower = 2.22222233f;

void StoreFloat(Guest g, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  g.Store32(address, bits);
}

}  // namespace

VideoSettings& Video() {
  static VideoSettings settings;
  return settings;
}

void VdQueryVideoMode(Guest g, uint32_t m) {
  const VideoSettings& v = Video();
  std::memset(g.host(m), 0, 48);
  g.Store32(m + 0, v.width);
  g.Store32(m + 4, v.height);
  g.Store32(m + 8, 0);
  g.Store32(m + 12, v.width * 3 >= v.height * 4 ? 1 : 0);
  g.Store32(m + 16, v.width >= 1280 || v.height >= 720 ? 1 : 0);
  StoreFloat(g, m + 20, v.refresh_rate);
  g.Store32(m + 24, 1);  // NTSC
  g.Store32(m + 28, 0x4A);
  g.Store32(m + 32, 0x01);
}

uint32_t VdQueryVideoFlags() {
  const VideoSettings& v = Video();
  uint32_t flags = v.width * 3 >= v.height * 4 ? 1 : 0;
  flags |= v.width >= 1024 ? 2 : 0;
  flags |= v.width >= 1920 ? 4 : 0;
  return flags;
}

void VdGetCurrentDisplayInformation(Guest g, uint32_t d) {
  const VideoSettings& v = Video();
  std::memset(g.host(d), 0, 0x58);
  g.Store16(d + 0x00, uint16_t(v.width));
  g.Store16(d + 0x02, uint16_t(v.height));
  g.Store32(d + 0x08 + 0x08, v.width);   // scaler source rect x2
  g.Store32(d + 0x08 + 0x0C, v.height);  // scaler source rect y2
  g.Store32(d + 0x08 + 0x10, v.width);   // scaled output width
  g.Store32(d + 0x08 + 0x14, v.height);  // scaled output height
  g.Store32(d + 0x08 + 0x18, 1);         // vertical filter type
  g.Store32(d + 0x08 + 0x28, 1);         // horizontal filter type
  const uint16_t overscan_x = uint16_t(v.width / 4), overscan_y = uint16_t(v.height / 4);
  g.Store16(d + 0x40, overscan_x);
  g.Store16(d + 0x42, overscan_y);
  g.Store16(d + 0x44, overscan_x);
  g.Store16(d + 0x46, overscan_y);
  g.Store16(d + 0x48, uint16_t(v.width));
  g.Store16(d + 0x4A, uint16_t(v.height));
  StoreFloat(g, d + 0x4C, v.refresh_rate);
  g.Store16(d + 0x56, uint16_t(v.width));
}

void VdGetCurrentDisplayGamma(Guest g, uint32_t type, uint32_t power) {
  g.Store32(type, kDisplayGammaType);
  StoreFloat(g, power, kDisplayGammaPower);
}

uint32_t VdSetDisplayMode(uint32_t) { return 0; }
uint32_t VdEnableDisableClockGating(uint32_t) { return 0; }
uint32_t VdIsHSIOTrainingSucceeded() { return 1; }
uint32_t VdRetrainEDRAM() { return 0; }
uint32_t VdRetrainEDRAMWorker() { return 0; }
uint32_t VdGetGraphicsAsicID() { return 0x11; }

void VdGetSystemCommandBuffer(Guest g, uint32_t p0, uint32_t p1) {
  std::memset(g.host(p0), 0, 0x94);
  g.Store32(p0, 0xBEEF0000);
  g.Store32(p1, 0xBEEF0001);
}

uint32_t VdCallGraphicsNotificationRoutines() { return 0; }
void VdShutdownEngines() {}
void VdSetSystemCommandBufferGpuIdentifierAddress() {}

void VdQueryRealVideoMode(Guest g, uint32_t video_mode) { VdQueryVideoMode(g, video_mode); }
uint32_t XGetVideoFlags() { return VdQueryVideoFlags(); }
uint32_t XGetVideoStandard() { return 1; }  // NTSC, the standard VdQueryVideoMode reports
uint32_t XGetVideoCapabilities() { return 0; }
uint32_t VdDisplayHardwareRequest() { return 0; }

uint32_t VdInitializeScalerCommandBuffer(Guest g, uint32_t destination, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) g.Store32(destination + 4 * i, 0x80000000);
  return count;
}

}  // namespace any360::sys
