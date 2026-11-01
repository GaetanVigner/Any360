// gfx_shader_tests: smoke test of the vendored Xenos microcode -> SPIR-V translator (any360_xenos_shader).
//
// Hand-assembles tiny Xenos shaders (no game data): the control flow and ALU words are packed from the bit layouts of
// ucode.h's ControlFlowExecInstruction / ControlFlowAllocInstruction / AluInstruction, then decoded back through those
// structures' accessors to prove the encoding matches. Each shader is analyzed and translated the way the SDK's Vulkan
// pipeline cache requests it (Shader::AnalyzeUcode, then SpirvShaderTranslator::TranslateAnalyzedShader on the
// translation of GetDefault{Vertex,Pixel}ShaderModification), and the result is checked to be a plausible SPIR-V
// module. The words are written next to the executable (smoke_*.spv) for feeding to a Vulkan driver.
//
//   gfx_shader_tests [output directory]
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include "ucode_encode.h"

namespace {

using namespace rex::graphics;
namespace uc = rex::graphics::ucode;

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// The encoders (control flow, vector ALU, vertex fetch) are shared with gfx_render_tests.
using namespace any360::gfx::test;

// Decodes the words back through ucode.h's structures and checks they say what the encoders meant.
void CheckDecode(const std::vector<uint32_t>& words, uc::AllocType alloc, uint32_t export_reg, AluOperand src,
                 const std::string& name) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(words.data(), cf);
    Check(cf[0].opcode() == uc::ControlFlowOpcode::kAlloc, name + ": cf0 decodes as alloc");
    Check(cf[0].alloc.alloc_type() == alloc, name + ": cf0 alloc type");
    Check(cf[1].opcode() == uc::ControlFlowOpcode::kExecEnd, name + ": cf1 decodes as exece");
    Check(cf[1].exec.address() == 1 && cf[1].exec.count() == 1 && cf[1].exec.sequence() == 0,
          name + ": cf1 exec address/count/sequence");
    Check(cf[1].exec.is_predicate_clean(), name + ": cf1 predicate clean");
    uc::AluInstruction alu;
    static_assert(sizeof(alu) == 3 * sizeof(uint32_t));
    std::memcpy(&alu, words.data() + 3, sizeof(alu));
    Check(alu.is_export() && alu.vector_dest() == export_reg, name + ": alu exports to the expected register");
    Check(alu.vector_opcode() == uc::AluVectorOpcode::kMax, name + ": alu vector opcode is max");
    Check(alu.scalar_opcode() == uc::AluScalarOpcode::kRetainPrev && alu.scalar_write_mask() == 0,
          name + ": alu scalar unit idle");
    Check(alu.vector_write_mask() == 0b1111, name + ": alu writes xyzw");
    Check(alu.src_is_temp(1) == src.is_temp && alu.src_is_temp(2) == src.is_temp, name + ": alu operand kinds");
    Check(alu.src_reg(1) == src.reg && alu.src_reg(2) == src.reg, name + ": alu operand registers");
    Check(alu.src_swizzle(1) == 0 && alu.src_swizzle(2) == 0, name + ": alu identity swizzles");
}

// ---- Translation ---------------------------------------------------------------------------------------------------

struct Result {
    bool translated = false;
    std::vector<uint32_t> spirv;
};

Result Translate(SpirvShaderTranslator& translator, xenos::ShaderType type, const std::vector<uint32_t>& words,
                 uint64_t hash, const std::string& name) {
    Result result;
    // The SDK's pipeline cache hands the guest (big-endian) ucode to the Shader; ours is host-endian.
    SpirvShader shader(type, hash, words.data(), words.size(), std::endian::native);
    rex::string::StringBuffer disasm;
    shader.AnalyzeUcode(disasm);
    Check(shader.is_ucode_analyzed(), name + ": ucode analyzed");
    Check(!shader.ucode_disassembly().empty(), name + ": ucode disassembly produced");

    // Default modification bits, as VulkanPipelineCache::GetCurrent{Vertex,Pixel}ShaderModification start from
    // (SQ_PROGRAM_CNTL num_reg 0: no dynamic register addressing in these shaders anyway).
    uint64_t modification =
        type == xenos::ShaderType::kVertex
            ? translator.GetDefaultVertexShaderModification(shader.GetDynamicAddressableRegisterCount(0),
                                                            Shader::HostVertexShaderType::kVertex)
            : translator.GetDefaultPixelShaderModification(shader.GetDynamicAddressableRegisterCount(0));
    Shader::Translation& translation = *shader.GetOrCreateTranslation(modification);
    result.translated = translator.TranslateAnalyzedShader(translation);
    Check(result.translated, name + ": TranslateAnalyzedShader reports success");
    Check(translation.is_translated() && translation.is_valid(), name + ": translation marked translated and valid");

    const std::vector<uint8_t>& binary = translation.translated_binary();
    Check(!binary.empty() && binary.size() % 4 == 0, name + ": SPIR-V is a non-empty whole number of words");
    result.spirv.resize(binary.size() / 4);
    if (!binary.empty()) std::memcpy(result.spirv.data(), binary.data(), result.spirv.size() * 4);
    if (result.spirv.size() >= 5) {
        const uint32_t* w = result.spirv.data();
        Check(w[0] == 0x07230203u, name + ": SPIR-V magic");
        uint32_t major = (w[1] >> 16) & 0xFF, minor = (w[1] >> 8) & 0xFF;
        Check(major == 1 && minor <= 6 && (w[1] & 0xFF0000FFu) == 0, name + ": SPIR-V version word");
        Check(w[2] >> 16 == SpirvShaderTranslator::kSpirvMagicToolId, name + ": generator is the Xenos translator");
        Check(w[3] > 1 && w[3] < 0x100000, name + ": SPIR-V id bound is sane");
        Check(w[4] == 0, name + ": SPIR-V schema word is 0");
    } else {
        Check(false, name + ": SPIR-V has a 5-word header");
    }
    return result;
}

void WriteSpv(const std::filesystem::path& dir, const char* file, const std::vector<uint32_t>& words) {
    std::filesystem::path path = dir / file;
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) {
        Check(false, "open " + path.string());
        return;
    }
    std::fwrite(words.data(), sizeof(uint32_t), words.size(), f);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path out_dir =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::absolute(argv[0]).parent_path();

    const uint32_t kPosition = uint32_t(uc::ExportRegister::kVSPosition);
    const uint32_t kColor0 = uint32_t(uc::ExportRegister::kPSColor0);

    // (a) Vertex shaders: oPos = c0 (float constant), and oPos = r0 (temp register; r0.x holds the vertex index).
    AluOperand c0{0, false}, r0{0, true};
    std::vector<uint32_t> vs_const = BuildSingleExportShader(uc::AllocType::kVsPosition, kPosition, c0);
    std::vector<uint32_t> vs_reg = BuildSingleExportShader(uc::AllocType::kVsPosition, kPosition, r0);
    // (b) Pixel shader: oC0 = c0.
    std::vector<uint32_t> ps_const = BuildSingleExportShader(uc::AllocType::kPsColors, kColor0, c0);
    CheckDecode(vs_const, uc::AllocType::kVsPosition, kPosition, c0, "vs_const");
    CheckDecode(vs_reg, uc::AllocType::kVsPosition, kPosition, r0, "vs_reg");
    CheckDecode(ps_const, uc::AllocType::kPsColors, kColor0, c0, "ps_const");

    // Host render target path (no fragment shader interlock), every feature on (SPIR-V 1.5) - what a desktop
    // Vulkan 1.2+ device yields through the SDK's Features(VulkanDevice*).
    SpirvShaderTranslator full(SpirvShaderTranslator::Features(true), true, true, false);
    Result vs_full = Translate(full, xenos::ShaderType::kVertex, vs_const, 0x5A0001, "vs_const/full");
    Result vs_reg_full = Translate(full, xenos::ShaderType::kVertex, vs_reg, 0x5A0002, "vs_reg/full");
    Result ps_full = Translate(full, xenos::ShaderType::kPixel, ps_const, 0x5A0003, "ps_const/full");

    // Minimum features (SPIR-V 1.0, no optional capabilities).
    SpirvShaderTranslator minimal(SpirvShaderTranslator::Features(false), false, false, false);
    Result vs_min = Translate(minimal, xenos::ShaderType::kVertex, vs_const, 0x5A0001, "vs_const/min");
    Result ps_min = Translate(minimal, xenos::ShaderType::kPixel, ps_const, 0x5A0003, "ps_const/min");
    if (vs_min.spirv.size() >= 2) Check(vs_min.spirv[1] == 0x00010000u, "vs_const/min: SPIR-V 1.0");
    if (vs_full.spirv.size() >= 2) Check(vs_full.spirv[1] == 0x00010500u, "vs_const/full: SPIR-V 1.5");

    // EDRAM through fragment shader interlock (the render-backend emulation in spirv_translator_rb.cpp).
    SpirvShaderTranslator fsi(SpirvShaderTranslator::Features(true), true, true, true);
    Result ps_fsi = Translate(fsi, xenos::ShaderType::kPixel, ps_const, 0x5A0003, "ps_const/fsi");
    std::vector<uint8_t> depth_only = fsi.CreateDepthOnlyFragmentShader();
    Check(depth_only.size() >= 20 && depth_only.size() % 4 == 0, "fsi: depth-only fragment shader produced");

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    WriteSpv(out_dir, "smoke_vs.spv", vs_full.spirv);
    WriteSpv(out_dir, "smoke_vs_reg.spv", vs_reg_full.spirv);
    WriteSpv(out_dir, "smoke_ps.spv", ps_full.spirv);
    WriteSpv(out_dir, "smoke_ps_fsi.spv", ps_fsi.spirv);

    std::printf("gfx_shader_tests: SPIR-V words vs=%zu vs_reg=%zu ps=%zu (min: vs=%zu ps=%zu) ps_fsi=%zu, written to %s\n",
                vs_full.spirv.size(), vs_reg_full.spirv.size(), ps_full.spirv.size(), vs_min.spirv.size(),
                ps_min.spirv.size(), ps_fsi.spirv.size(), out_dir.string().c_str());
    std::printf("gfx_shader_tests: %d failing checks\n", g_failures);
    return g_failures ? 1 : 0;
}
