// Xenos GPU register indices the command processor itself acts on (the whole file is kRegisterCount dwords; the
// renderer reads the rest). Indices are those of the console's MMIO window, 0x7FC80000 + 4 * index.
#pragma once

#include <cstdint>

namespace any360::gfx::reg {

constexpr uint32_t kRegisterCount = 0x5003;
// The display gamma ramp (written by the D3D runtime through MMIO).
constexpr uint32_t DC_LUT_RW_MODE = 0x1921, DC_LUT_RW_INDEX = 0x1922, DC_LUT_SEQ_COLOR = 0x1923, DC_LUT_PWL_DATA = 0x1924,
                   DC_LUT_30_COLOR = 0x1925, DC_LUT_WRITE_EN_MASK = 0x1927;

constexpr uint32_t CP_RB_BASE = 0x01C0, CP_RB_CNTL = 0x01C1, CP_RB_RPTR_ADDR = 0x01C3, CP_RB_RPTR = 0x01C4, CP_RB_WPTR = 0x01C5;
constexpr uint32_t SCRATCH_UMSK = 0x01DC, SCRATCH_ADDR = 0x01DD, SCRATCH_REG0 = 0x0578;
constexpr uint32_t COHER_SIZE_HOST = 0x0A2F, COHER_BASE_HOST = 0x0A30, COHER_STATUS_HOST = 0x0A31;
constexpr uint32_t RB_EDRAM_TIMING = 0x0F00, RB_BC_CONTROL = 0x0F01;
constexpr uint32_t D1MODE_V_COUNTER = 0x194C, D1MODE_INTERRUPT_STATUS = 0x1951, D1MODE_VIEWPORT_SIZE = 0x1961;
constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9, VGT_DMA_BASE = 0x21FA, VGT_DMA_SIZE = 0x21FB, VGT_DRAW_INITIATOR = 0x21FC;
constexpr uint32_t RB_MODECONTROL = 0x2208, PA_SC_VIZ_QUERY = 0x2293, RB_SAMPLE_COUNT_ADDR = 0x2325;
// The constant spaces SET_CONSTANT and LOAD_ALU_CONSTANT address by type 0-4.
constexpr uint32_t kAluConstants = 0x4000, kFetchConstants = 0x4800, kBoolConstants = 0x4900, kLoopConstants = 0x4908,
                   kContextRegisters = 0x2000;

}  // namespace any360::gfx::reg

namespace any360::gfx::pm4 {

// Type-3 opcodes.
constexpr uint32_t NOP = 0x10, REG_RMW = 0x21, DRAW_INDX = 0x22, VIZ_QUERY = 0x23, WAIT_FOR_IDLE = 0x26, IM_LOAD = 0x27,
                   IM_LOAD_IMMEDIATE = 0x2B, SET_CONSTANT = 0x2D, LOAD_ALU_CONSTANT = 0x2F, DRAW_INDX_2 = 0x36,
                   INDIRECT_BUFFER_PFD = 0x37, INVALIDATE_STATE = 0x3B, WAIT_REG_MEM = 0x3C, MEM_WRITE = 0x3D,
                   REG_TO_MEM = 0x3E, INDIRECT_BUFFER = 0x3F, COND_WRITE = 0x45, EVENT_WRITE = 0x46, ME_INIT = 0x48,
                   SET_BIN_MASK = 0x50, SET_BIN_SELECT = 0x51, INTERRUPT = 0x54, CONTEXT_UPDATE = 0x5E, SET_CONSTANT2 = 0x55,
                   SET_SHADER_CONSTANTS = 0x56, EVENT_WRITE_SHD = 0x58, EVENT_WRITE_EXT = 0x5A, EVENT_WRITE_ZPD = 0x5B,
                   SET_BIN_MASK_LO = 0x60, SET_BIN_MASK_HI = 0x61, SET_BIN_SELECT_LO = 0x62, SET_BIN_SELECT_HI = 0x63,
                   WAIT_UNTIL_READ = 0x5C, WAIT_IB_PFD_COMPLETE = 0x5D, XE_SWAP = 0x64;

constexpr uint32_t Type0(uint32_t first, uint32_t count, bool one_register = false) {
  return (count - 1) << 16 | (one_register ? 0x8000u : 0u) | first;
}
constexpr uint32_t Type1(uint32_t first, uint32_t second) { return 1u << 30 | second << 11 | first; }
constexpr uint32_t Type2() { return 2u << 30; }
constexpr uint32_t Type3(uint32_t opcode, uint32_t count, bool predicated = false) {
  return 3u << 30 | (count - 1) << 16 | opcode << 8 | (predicated ? 1u : 0u);
}

}  // namespace any360::gfx::pm4
