// Checks src/burnout_overrides.cpp without the generated guest code: every
// SDK binding resolves to the intended runtime handler, and rom0:ROMVER has the
// layout the game's parser expects. Also the text replacement of src/burnout_texts.cpp and the
// frame rate wrappers of src/burnout_frame_rate.cpp.
#include "../../src/burnout_frame_rate.h"
#include "../../src/burnout_pages.h"
#include "../../src/burnout_texts.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_memory.h"
#include "runtime/vblank_rate.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr const char* kElfName = "SLES_546.27";
constexpr uint32_t kEntry = 0x00100008;
constexpr uint32_t kElfCrc32 = 0xC804E2C1;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

struct Expected {
    uint32_t address;
    PS2Runtime::RecompiledFunction handler;
    const char* name;
};

// A second, independent record of src/burnout_overrides.cpp: a renamed or
// mistyped handler, or a name that resolves to another handler, fails here.
const Expected kExpected[] = {
    {0x003AF8D8, &ps2_stubs::sceSifInitCmd, "sceSifInitCmd"},
    {0x003B0020, &ps2_stubs::sceSifInitRpc, "sceSifInitRpc"},
    {0x003B0668, &ps2_stubs::sceSifBindRpc, "sceSifBindRpc"},
    {0x003B0848, &ps2_syscalls::sceSifCallRpc, "sceSifCallRpc"},
    {0x003B2320, &ps2_stubs::sceSifSyncIop, "sceSifSyncIop"},
    {0x003B2370, &ps2_stubs::sceSifRebootIop, "sceSifRebootIop"},
    {0x003AFB58, &ps2_stubs::sceSifExitCmd, "sceSifExitCmd"},
    {0x003AFB90, &ps2_stubs::sceSifSetCmdBuffer, "sceSifSetCmdBuffer"},
    {0x003AFBA8, &ps2_stubs::sceSifAddCmdHandler, "sceSifAddCmdHandler"},
    {0x003AFC20, &ps2_stubs::sceSifRemoveCmdHandler, "sceSifRemoveCmdHandler"},
    {0x003AFDA8, &ps2_syscalls::sceSifSendCmd, "sceSifSendCmd"},
    {0x003AFDE8, &ps2_syscalls::sceSifSendCmd, "isceSifSendCmd"},
    {0x003AF8B0, &ps2_stubs::sceSifGetSreg, "sceSifGetSreg"},
    {0x003B2180, &ps2_syscalls::sceSifLoadModule, "sceSifLoadModule"},
    {0x003B1EA8, &ps2_stubs::sceSifSearchModuleByName, "sceSifSearchModuleByName"},
    {0x003B4E28, &ps2_stubs::sceMcInit, "sceMcInit"},
    {0x003B5050, &ps2_stubs::sceMcEnd, "sceMcEnd"},
    {0x003B5700, &ps2_stubs::sceMcSync, "sceMcSync"},
    {0x003B5820, &ps2_stubs::sceMcGetInfo, "sceMcGetInfo"},
    {0x003B50C0, &ps2_stubs::sceMcOpen, "sceMcOpen"},
    {0x003B51E8, &ps2_stubs::sceMcMkdir, "sceMcMkdir"},
    {0x003B5220, &ps2_stubs::sceMcClose, "sceMcClose"},
    {0x003B52E0, &ps2_stubs::sceMcSeek, "sceMcSeek"},
    {0x003B5470, &ps2_stubs::sceMcRead, "sceMcRead"},
    {0x003B5588, &ps2_stubs::sceMcWrite, "sceMcWrite"},
    {0x003B5EB0, &ps2_stubs::sceMcFlush, "sceMcFlush"},
    {0x003B5B80, &ps2_stubs::sceMcChdir, "sceMcChdir"},
    {0x003B59A0, &ps2_stubs::sceMcGetDir, "sceMcGetDir"},
    {0x003B5F70, &ps2_stubs::sceMcSetFileInfo, "sceMcSetFileInfo"},
    {0x003B5D90, &ps2_stubs::sceMcDelete, "sceMcDelete"},
    {0x003B5CC0, &ps2_stubs::sceMcFormat, "sceMcFormat"},
    {0x003B1B48, &ps2_stubs::sceSifInitIopHeap, "sceSifInitIopHeap"},
    {0x003B1BD0, &ps2_stubs::sceSifAllocSysMemory, "sceSifAllocSysMemory"},
    {0x003B1C50, &ps2_stubs::sceSifFreeIopHeap, "sceSifFreeIopHeap"},
    {0x003B1468, &ps2_stubs::sceOpen, "sceOpen"},
    {0x003B16F8, &ps2_stubs::sceClose, "sceClose"},
    {0x003B1878, &ps2_stubs::sceRead, "sceRead"},
    {0x00377CA0, &ps2_stubs::sceCdInit, "sceCdInit"},
    {0x00378458, &ps2_stubs::sceCdGetDiskType, "sceCdGetDiskType"},
    {0x00378480, &ps2_stubs::sceCdMmode, "sceCdMmode"},
    {0x003781A0, &ps2_stubs::sceCdDiskReady, "sceCdDiskReady"},
    {0x00378558, &ps2_stubs::sceCdReadClock, "sceCdReadClock"},
    {0x00377F88, &ps2_stubs::sceCdDiskReady, "sceCdDiskReady (old protocol)"},
    {0x003783C0, &ps2_stubs::sceCdGetDiskType, "sceCdGetDiskType (core)"},
    {0x00377AD8, &ps2_stubs::sceCdSyncS, "sceCdSyncS"},
};

void setReg(R5900Context& ctx, int reg, uint32_t value) {
    ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
}

int32_t returnValue(const R5900Context& ctx) {
    return static_cast<int32_t>(getRegU32(&ctx, 2));
}

void testBindings() {
    // PS2Runtime is ~0.5 MB: keep it off the stack (1 MB by default on Windows).
    const auto runtime = std::make_unique<PS2Runtime>();
    ps2_game_overrides::applyMatching(*runtime, std::string("disc/") + kElfName, kEntry, kElfCrc32, true);
    std::size_t bound = 0;
    for (std::size_t slot = 0x00100000 >> 2; slot < (0x003BEB28 >> 2); ++slot)
        bound += g_ps2RecompiledFunctionTable[slot] != nullptr;
    check(bound == std::size(kExpected),
          "bound " + std::to_string(bound) + " .text slots, expected " + std::to_string(std::size(kExpected)));
    for (const Expected& expected : kExpected) {
        check(g_ps2RecompiledFunctionTable[expected.address >> 2] == expected.handler,
              std::string(expected.name) + " is not bound to its runtime handler");
    }
}

void testRomVersion() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bdr-overrides-test";
    std::filesystem::create_directories(root);
    PS2Runtime::IoPaths paths;
    paths.elfDirectory = paths.hostRoot = paths.cdRoot = paths.mcRoot = root;
    PS2Runtime::setIoPaths(paths);

    const auto runtime = std::make_unique<PS2Runtime>();
    std::string error;
    check(runtime->romDevice().configure({kElfName, kEntry, kElfCrc32}, &error), "ROM0 configure: " + error);
    check(runtime->romDevice().activeProfile() == "burnout-dominator-pal", "Burnout ROM0 profile not selected");

    std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0xA5);
    constexpr uint32_t kPath = 0x1000, kBuffer = 0x2000;
    std::memcpy(rdram.data() + kPath, "rom0:ROMVER", 12);
    R5900Context ctx{};
    setReg(ctx, 4, kPath);
    setReg(ctx, 5, PS2_FIO_O_RDONLY);
    ps2_stubs::sceOpen(rdram.data(), &ctx, runtime.get());
    const int32_t fd = returnValue(ctx);
    check(fd >= 0, "sceOpen(rom0:ROMVER) failed");

    // Same access pattern as 0x379908: one byte per sceRead, up to the NUL.
    uint32_t length = 0;
    for (; length < 256; ++length) {
        setReg(ctx, 4, static_cast<uint32_t>(fd));
        setReg(ctx, 5, kBuffer + length);
        setReg(ctx, 6, 1);
        ps2_stubs::sceRead(rdram.data(), &ctx, runtime.get());
        if (returnValue(ctx) != 1) break;
        if (rdram[kBuffer + length] == 0) break;
    }
    check(length == 15, "ROMVER NUL at offset " + std::to_string(length) + ", expected 15");
    check(std::memcmp(rdram.data() + kBuffer, "0200EC20040614\n", 16) == 0, "ROMVER bytes differ");
    // 0x379908 converts the 9 bytes before the NUL and compares with 20010608.
    const long date = std::strtol(reinterpret_cast<const char*>(rdram.data() + kBuffer + length - 9), nullptr, 10);
    check(date == 20040614, "ROMVER date parsed as " + std::to_string(date));

    setReg(ctx, 4, static_cast<uint32_t>(fd));
    ps2_stubs::sceClose(rdram.data(), &ctx, runtime.get());
    check(returnValue(ctx) == 0, "sceClose(rom0:ROMVER) failed");

    const auto other = std::make_unique<PS2Runtime>();
    check(other->romDevice().configure({"SLUS_000.00", kEntry, 0}, &error) && other->romDevice().activeProfile().empty(),
          "the Burnout ROM0 profile must not apply to other ELFs");
    std::filesystem::remove_all(root);
}
void testPcTexts() {
    // Two records as in MAINUK.BIN: a two-unit hash (here with a no-break space in it), then
    // NUL-terminated UTF-16 text.
    const std::u16string records[] = {u"\x9A0D\x00A0No memory\x00A0" u"card (PS2) here.", u"\x1234\x5678Short."};
    std::vector<uint8_t> file;
    for (const std::u16string& record : records)
        for (const char16_t c : record + u'\0') {
            file.push_back(uint8_t(c & 0xFF));
            file.push_back(uint8_t(c >> 8));
        }
    const size_t size = file.size();
    const size_t changed = burnout::replaceUtf16Text(
        file, {{u"memory card (PS2)", u"save"}, {u"Short.", u"Far too long to fit."}});
    std::u16string text;
    for (size_t i = 0; i < file.size(); i += 2)
        text.push_back(char16_t(file[i] | (file[i + 1] << 8)));
    check(changed == 1 && file.size() == size, "one string changes and the file keeps its size");
    check(text.compare(0, 16, std::u16string(u"\x9A0D\x00A0No save here.") + u'\0') == 0,
          "the hash stays and a no-break space matches a space");
    check(text.find(u"Short.") != std::u16string::npos, "a replacement that does not fit is not made");
    check(text[28] == 0 && text[29] == 0x1234, "the shortened text is padded with NULs up to the next record");
}
// Stand-in for the game functions wrapped by src/burnout_frame_rate.cpp: records what the
// wrapper hands on, and returns 1200 as the VBlank counter read by 0x1B9F80.
struct Seen {
    int32_t a0 = 0;
    float f12 = 0.0f, f13 = 0.0f, duration = 0.0f;
    uint8_t palFlag = 0;
};
Seen g_seen;

void recordCall(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    g_seen.a0 = static_cast<int32_t>(getRegU32(ctx, 4));
    g_seen.f12 = ctx->f[12];
    g_seen.f13 = ctx->f[13];
    g_seen.palFlag = rdram[0x432E98];
    std::memcpy(&g_seen.duration, rdram + (getRegU32(ctx, 7) & ~3u), sizeof(float));
    setReg(*ctx, 2, 1200u);
}

bool closeTo(float value, float expected) {
    return std::fabs(value - expected) < 1e-5f;
}

void testFrameRate() {
    constexpr uint32_t kSetRate = 0x1E68A0, kSetStep = 0x1E6668, kCamera = 0x169998, kPowf = 0x37C5A8,
                       kMenuBox = 0x24A260, kVBlankCount = 0x1B9F80;
    const auto runtime = std::make_unique<PS2Runtime>();
    for (const uint32_t address : {kSetRate, kSetStep, kCamera, kPowf, kMenuBox, kVBlankCount})
        runtime->registerFunction(address, &recordCall);
    burnout::installFrameRate(*runtime);
    std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
    R5900Context ctx{};
    const auto call = [&](uint32_t address) {
        g_ps2RecompiledFunctionTable[address >> 2](rdram.data(), &ctx, runtime.get());
    };
    const auto floatAt = [&](uint32_t address) {
        float value = 0.0f;
        std::memcpy(&value, rdram.data() + address, sizeof(value));
        return value;
    };
    const auto setFloat = [&](uint32_t address, float value) {
        std::memcpy(rdram.data() + address, &value, sizeof(value));
    };

    // At the console's rate the wrappers hand everything on unchanged.
    ps2x::setVBlankRate(0);
    setReg(ctx, 4, 60);
    call(kSetRate);
    check(g_seen.a0 == 60 && floatAt(0x3C18C0) == 0.0f, "without a VBlank rate the step rate is the game's");
    call(kCamera);
    check(g_seen.palFlag == 0, "without a VBlank rate the camera keeps the 60 Hz branch");
    call(kVBlankCount);
    check(getRegU32(&ctx, 2) == 1200u, "without a VBlank rate the VBlank count is the game's");

    ps2x::setVBlankRate(120);
    for (const int pal : {0, 1}) {
        rdram[0x432E98] = static_cast<uint8_t>(pal);
        const std::string mode = pal ? " at 50 Hz" : " at 60 Hz";
        setReg(ctx, 4, pal ? 50 : 60);
        call(kSetRate);
        check(g_seen.a0 == 120 && floatAt(0x3C18C0) == 120.0f, "a race steps 120 times a second" + mode);
        setReg(ctx, 4, pal ? 25 : 30);
        call(kSetRate);
        check(g_seen.a0 == 60, "the front end steps 60 times a second" + mode);
        ctx.f[12] = pal ? 20.0f : 100.0f / 6.0f;
        ctx.f[13] = 0.0f;
        call(kSetStep);
        check(closeTo(g_seen.f12, 1000.0f / 120.0f) && g_seen.f13 == 0.0f, "the governor's step is 8.33 ms" + mode);
        call(kCamera);
        check(g_seen.palFlag == 1 && rdram[0x432E98] == pal, "the camera runs its 50 Hz branch" + mode);
        setReg(ctx, 2, 0);
        call(kVBlankCount);
        check(getRegU32(&ctx, 2) == (pal ? 500u : 600u), "1200 VBlanks count as console VBlanks" + mode);
        setFloat(0x1000, 0.5f);
        setReg(ctx, 7, 0x1000);
        call(kMenuBox);
        check(closeTo(g_seen.duration, pal ? 1.2f : 1.0f) && floatAt(0x1000) == 0.5f,
              "menu boxes slide for as many frames as the front end draws in their duration" + mode);
    }
    rdram[0x432E98] = 0;
    setFloat(0x4915BC, 1.0f / 120.0f);
    ctx.f[13] = 1.2f;
    setReg(ctx, 31, 0x169B6C);
    call(kPowf);
    check(closeTo(g_seen.f13, 0.5f), "the camera's damping power is 60 * dt");
    ctx.f[13] = 1.2f;
    setReg(ctx, 31, 0x100000);
    call(kPowf);
    check(g_seen.f13 == 1.2f, "other powf calls are unchanged");

    ps2x::setVBlankRate(0);
    for (const uint32_t address : {kSetRate, kSetStep, kCamera, kPowf, kMenuBox, kVBlankCount})
        g_ps2RecompiledFunctionTable[address >> 2] = nullptr;
}
void testPages() {
    const auto page = [](const char* name) {
        const std::string text(name);
        return burnout::classifyPage(reinterpret_cast<const uint8_t*>(name), text.size() + 1);
    };
    using burnout::Page;
    check(page("SU_Frequency") == Page::Frequency, "SU_Frequency is the frequency page");
    check(page("su_movie") == Page::Movie && page("SU_MOVIE2") == Page::Movie2 && page("SU_Intro") == Page::Intro,
          "the start-up pages are found by name, ignoring case");
    check(page("SU_MainMenu") == Page::Other && page("SU_Movie3") == Page::Other && page("SU_Mov") == Page::Other &&
              page("SU_PressStart") == Page::Other && page("") == Page::Other,
          "other names, longer or shorter, are no special page");
    check(page("DD_Profile_LoadSave") == Page::ProfileLoadSave, "DD_Profile_LoadSave is the boot's first page");
    check(burnout::isSkippedPage(Page::Movie) && burnout::isSkippedPage(Page::Movie2) &&
              !burnout::isSkippedPage(Page::Intro) && !burnout::isSkippedPage(Page::Frequency) &&
              !burnout::isSkippedPage(Page::ProfileLoadSave) && !burnout::isSkippedPage(Page::Other),
          "skip_intro leaves out SU_Movie and SU_Movie2 only");
    // A name that ends where the readable bytes end is not read past them.
    const uint8_t bare[] = {'S', 'U', '_', 'I', 'n', 't', 'r', 'o'};
    check(burnout::classifyPage(bare, sizeof(bare)) == Page::Other && burnout::classifyPage(bare, 0) == Page::Other,
          "the name is read only within the room given");

    // skip_intro: the game's own name buffer becomes "SU_Intro", and nothing after the old name's
    // NUL is written (the shortest buffer, for SU_Movie, holds 9 bytes).
    for (const char* from : {"SU_Frequency", "SU_Movie", "SU_Movie2"}) {
        char buffer[16] = {};
        std::strcpy(buffer, from);
        const size_t end = std::strlen(from) + 1;
        buffer[end] = 'x';
        burnout::IntroSkip skip(true);
        const bool redirected = page(from) == Page::Frequency
                                    ? skip.onFrequencySkip(reinterpret_cast<uint8_t*>(buffer))
                                    : skip.onGoToPage(page(from), reinterpret_cast<uint8_t*>(buffer));
        check(redirected && std::strcmp(buffer, "SU_Intro") == 0 && buffer[end] == 'x' && page(buffer) == Page::Intro,
              std::string("skip_intro redirects ") + from + " to SU_Intro");
    }

    // Over a run: one redirect at the boot, and none once the game is past the boot's first pages.
    // "+" marks the SU_Frequency the 50/60 Hz skip leaves.
    const auto run = [&](bool on, std::initializer_list<const char*> pages) {
        burnout::IntroSkip skip(on);
        int redirects = 0;
        for (const char* name : pages) {
            const bool frequencySkip = name[0] == '+';
            char buffer[32] = {};
            std::strcpy(buffer, name + frequencySkip);
            auto* bytes = reinterpret_cast<uint8_t*>(buffer);
            const bool redirected = frequencySkip ? skip.onFrequencySkip(bytes) : skip.onGoToPage(page(buffer), bytes);
            check(redirected == (std::strcmp(buffer, "SU_Intro") == 0 && std::strcmp(name + frequencySkip, "SU_Intro") != 0),
                  std::string("the name is rewritten exactly when redirected: ") + name);
            redirects += redirected;
        }
        return redirects;
    };
    check(run(true, {"DD_Profile_LoadSave", "+SU_Frequency", "SU_Intro", "SU_PressStart"}) == 1,
          "the 50/60 Hz skip's SU_Frequency is redirected once");
    check(run(true, {"DD_Profile_LoadSave", "SU_Frequency", "SU_Movie", "SU_Movie2", "SU_Intro"}) == 1,
          "a SU_Frequency the game shows stays, then SU_Movie is redirected and SU_Movie2 after it is not");
    check(run(true, {"SU_PressStart", "SU_Attract", "SU_Movie", "SU_Movie2"}) == 0,
          "after a page past the boot (a restored state, attract mode) nothing is redirected");
    check(run(true, {"SU_MainMenu", "+SU_Frequency"}) == 0, "nor is the 50/60 Hz skip of a later page change");
    check(run(false, {"DD_Profile_LoadSave", "+SU_Frequency", "SU_Movie"}) == 0, "without skip_intro nothing is");
}
} // namespace

int main() {
    testBindings();
    testRomVersion();
    testPcTexts();
    testFrameRate();
    testPages();
    if (g_failures) {
        std::cerr << g_failures << " Burnout override check(s) failed\n";
        return 1;
    }
    std::cout << "Burnout override tests passed (" << std::size(kExpected) << " bindings, ROM0 profile, frame rate, pages)\n";
    return 0;
}
