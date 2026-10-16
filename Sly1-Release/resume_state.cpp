// "Resume where you left off" (Android).
//
// The retail game keeps its in-level progress in plain value data: the current game state (GS, with every level's LS: coins,
// clue bits, dialogs played, per-level checkpoint bits) and the checkpoint manager (the 4096-bit table of triggered
// checkpoint-bound objects plus the warp the player respawns at). That is exactly what "return to checkpoint" uses after a
// death, so it is enough to bring a run back after the app was left, killed or the phone restarted:
//   * cane_resume_save() writes it to Saves/resume.dat (app goes to the background, Exit Game, checkpoint reached),
//   * cane_resume_try_start() (title screen, Start) restores it and starts the same transition "return to checkpoint" does,
//   * cane_resume_clear() drops it when the run is deliberately ended (back to the title, new game).
// It is NOT an emulator-style snapshot: you come back at the last checkpoint with the progress you had, not on the exact frame.
#include "game.h"
#include "chkpnt.h"
#include "save.h"
#include "ui.h"
#include "wipe.h"
#include "transition.h"
#include "po.h"
#include "cm.h"
#include "xform.h"
#include "math.h"
#include "Input.h"
#include "../platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef __ANDROID__
#include <android/log.h>
#define RESUME_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cane", __VA_ARGS__)
#else
#define RESUME_LOG(...) std::printf(__VA_ARGS__)
#endif

namespace
{
constexpr char kMagic[8] = { 'P', 'C', 'A', 'N', 'E', 'R', 'S', 'M' };
constexpr uint32_t kVersion = 2;

struct ResumeHeader
{
    char magic[8];
    uint32_t version;
    uint32_t sizeofGs;
    int32_t slot;       // save slot this run belongs to
    int32_t levelId;    // (world << 8) | level
    int32_t fCheckpoint;
    int32_t oidWarp;
    int32_t oidWarpContext;
    uint32_t sizeofBits;
    // Save State only: where Sly stood. The live (not just checkpointed) trigger bits follow the GS and the saved bits in the file.
    int32_t fPlacement;
    float pos[3];
    float mat[9];
    float vel[3];
};

// Placement waiting for the level to finish loading (applied right after the player is teleported to the warp).
struct PendingState
{
    bool valid = false;
    float pos[3]{};
    float mat[9]{};
    float vel[3]{};
    CBitArray4096 bitsSaved{};   // the real checkpoint bits, put back once the live bits were restored
} s_pending;

std::filesystem::path StatePath()
{
    return cane::UserDirectory() / "Saves" / "state.dat";
}

std::filesystem::path ResumePath()
{
    return cane::UserDirectory() / "Saves" / "resume.dat";
}

int CurrentSlot()
{
    if (g_saveData.pgsCurrentSave == nullptr)
        return -1;
    const ptrdiff_t slot = g_saveData.pgsCurrentSave - g_saveData.saveData;
    return (slot >= 0 && slot < SAVE_SLOT_COUNT) ? static_cast<int>(slot) : -1;
}

// Only while a level is really being played (or the Hideout hub): not on the splash/title, not during a wipe.
// g_ui.uisPlaying is only an override (Splash / Attract / Hub) and stays Nil in an ordinary level; g_ui.uis is the screen state.
bool FInRun()
{
    if (g_pgsCur == nullptr || g_psw == nullptr || g_wipe.wipes != WIPES_Idle)
        return false;

    if (g_ui.uisPlaying == UIS_Splash || g_ui.uisPlaying == UIS_Attract)
        return false;

    switch (g_ui.uis)
    {
    case UIS_Hub:
    case UIS_Playing:
    case UIS_Pausing:
    case UIS_Paused:
    case UIS_Unpausing:
        return true;
    default:
        return false;
    }
}
}   // namespace

extern "C" void cane_resume_clear()
{
    // The game boots by "wiping to the title screen" with no world loaded yet: that must not throw the resume file away.
    if (g_psw == nullptr)
        return;

    std::error_code ec;
    if (std::filesystem::remove(ResumePath(), ec))
        RESUME_LOG("[Resume] cleared");
}

static void WriteFile(const std::filesystem::path& path, const char* why, bool fState);

extern "C" void cane_resume_save(const char* why)
{
    WriteFile(ResumePath(), why, false);
}

static void WriteFile(const std::filesystem::path& path, const char* why, bool fState)
{
    const int slot = CurrentSlot();
    if (!FInRun() || slot < 0)
    {
        // slot < 0: a run that was never saved to a slot has nowhere to resume into
        RESUME_LOG("[Resume] not saving (%s): uis=%d playing=%d wipes=%d slot=%d psw=%d", why != nullptr ? why : "?",
            static_cast<int>(g_ui.uis), static_cast<int>(g_ui.uisPlaying), static_cast<int>(g_wipe.wipes), slot, g_psw != nullptr ? 1 : 0);
        return;
    }

    ResumeHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(header.magic));
    header.version = kVersion;
    header.sizeofGs = static_cast<uint32_t>(sizeof(GS));
    header.slot = slot;
    header.levelId = (static_cast<int>(g_pgsCur->gameWorldCur) << 8) | static_cast<int>(g_pgsCur->worldLevelCur);
    header.fCheckpoint = g_chkmgr.fCheckpoint ? 1 : 0;
    header.oidWarp = static_cast<int32_t>(g_chkmgr.csSaved.oidWarp);
    header.oidWarpContext = static_cast<int32_t>(g_chkmgr.csSaved.oidWarpContext);
    header.sizeofBits = static_cast<uint32_t>(sizeof(CBitArray4096));

    if (fState)
    {
        PO* const ppo = PpoCur();
        if (ppo == nullptr)
        {
            RESUME_LOG("[State] not saving: no player object");
            return;
        }
        glm::vec3 pos{};
        glm::mat3 mat{ 1.0f };
        pos = ppo->xf.posWorld;
        mat = ppo->xf.matWorld;
        header.fPlacement = 1;
        std::memcpy(header.pos, &pos, sizeof(header.pos));
        std::memcpy(header.mat, &mat, sizeof(header.mat));
        std::memcpy(header.vel, &ppo->xf.v, sizeof(header.vel));
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + ".part";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(g_pgsCur), sizeof(GS));
        out.write(reinterpret_cast<const char*>(&g_chkmgr.csSaved.abitChk), sizeof(CBitArray4096));
        if (fState)
            out.write(reinterpret_cast<const char*>(&g_chkmgr.abitChk), sizeof(CBitArray4096));
        if (!out)
            return;
    }

    std::filesystem::rename(temp, path, ec);   // never leave a half-written file behind
    if (!ec)
        RESUME_LOG("[Resume] saved%s (%s): slot %d level 0x%x checkpoint %d", fState ? " STATE" : "", why != nullptr ? why : "?", slot, header.levelId, header.fCheckpoint);
}

static bool LoadFile(const std::filesystem::path& path, bool fState)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    ResumeHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
        header.sizeofGs != sizeof(GS) || header.sizeofBits != sizeof(CBitArray4096))
    {
        RESUME_LOG("[Resume] ignoring save file (wrong format)");
        return false;
    }

    if (header.slot != CurrentSlot())
        return false;   // another slot's run: keep the file for its own slot

    static GS s_gs;
    static CBitArray4096 s_bits;
    static CBitArray4096 s_bitsLive;
    in.read(reinterpret_cast<char*>(&s_gs), sizeof(GS));
    in.read(reinterpret_cast<char*>(&s_bits), sizeof(CBitArray4096));
    if (fState)
        in.read(reinterpret_cast<char*>(&s_bitsLive), sizeof(CBitArray4096));
    if (!in)
    {
        RESUME_LOG("[Resume] ignoring save file (truncated)");
        return false;
    }

    LEVELINFO* const plevel = GetLevelInfo(header.levelId);
    const GS& gs = s_gs;
    if (plevel == nullptr || gs.gameWorldCur < GAMEWORLD_Intro || gs.gameWorldCur >= GAMEWORLD_Max || gs.worldLevelCur < 0 ||
        gs.worldLevelCur >= 9 || ((static_cast<int>(gs.gameWorldCur) << 8) | static_cast<int>(gs.worldLevelCur)) != header.levelId)
    {
        RESUME_LOG("[Resume] ignoring save file (bad level)");
        return false;
    }

    // Same preparation as a normal Load Game, then the in-level state on top of it.
    *g_pgsCur = s_gs;
    ApplyVibrationSetting(g_pgsCur);

    g_chkmgr.csSaved.abitChk = fState ? s_bitsLive : s_bits;   // the level load restores the live bits from here...
    g_chkmgr.csSaved.oidWarp = static_cast<OID>(header.oidWarp);
    g_chkmgr.csSaved.oidWarpContext = static_cast<OID>(header.oidWarpContext);
    g_chkmgr.fCheckpoint = header.fCheckpoint != 0;

    s_pending.valid = false;
    if (fState && header.fPlacement != 0)
    {
        s_pending.valid = true;
        std::memcpy(s_pending.pos, header.pos, sizeof(s_pending.pos));
        std::memcpy(s_pending.mat, header.mat, sizeof(s_pending.mat));
        std::memcpy(s_pending.vel, header.vel, sizeof(s_pending.vel));
        s_pending.bitsSaved = s_bits;   // ...and the real checkpoint bits go back afterwards
    }

    OnGameLoad(&g_game);

    // Exactly what ReturnChkmgrToCheckpoint does after a death: the checkpoint flag (1) restores the bits, 0x10 shows the lives.
    TRANS trans{};
    trans.fSet = 1;
    trans.plevel = plevel;
    if (g_chkmgr.fCheckpoint || fState)
    {
        trans.oidWarp = g_chkmgr.csSaved.oidWarp;
        trans.oidWarpContext = g_chkmgr.csSaved.oidWarpContext;
        trans.grftrans = static_cast<GRFTRANS>(0x11);
    }
    else
    {
        trans.oidWarp = OID_Nil;
        trans.oidWarpContext = OID_Nil;
        trans.grftrans = static_cast<GRFTRANS>(0x10);
    }

    ActivateWipe(&g_wipe, &trans, WIPEK_Keyhole);
    RESUME_LOG("[Resume] %s slot %d level 0x%x checkpoint %d", fState ? "loading STATE" : "resuming", header.slot, header.levelId, header.fCheckpoint);
    return true;
}

// Title screen, Start pressed, the slot's game state already loaded into *g_pgsCur. Returns true when it started the resume.
extern "C" bool cane_resume_try_start()
{
    if (g_ui.uisPlaying != UIS_Attract)
        return false;
    return LoadFile(ResumePath(), false);
}

// ---- pause menu: Save State / Load State ----------------------------------------------------------------------------------

extern "C" bool cane_state_can_save()
{
    return FInRun() && CurrentSlot() >= 0 && PpoCur() != nullptr && g_pgsCur->gameWorldCur != GAMEWORLD_Intro;
}

extern "C" bool cane_state_exists()
{
    std::ifstream in(StatePath(), std::ios::binary);
    ResumeHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    return in && std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 && header.version == kVersion && header.slot == CurrentSlot();
}

extern "C" bool cane_state_save()
{
    if (!cane_state_can_save())
        return false;
    WriteFile(StatePath(), "state", true);
    return cane_state_exists();
}

extern "C" bool cane_state_load()
{
    return LoadFile(StatePath(), true);
}

extern "C" bool cane_state_pending()
{
    return s_pending.valid;
}

// Called by the level load right after the player was teleported to the warp: puts Sly back where the state was taken.
extern "C" void cane_state_apply_pending()
{
    if (!s_pending.valid)
        return;
    s_pending.valid = false;

    g_chkmgr.csSaved.abitChk = s_pending.bitsSaved;

    PO* const ppo = PpoCur();
    if (ppo == nullptr)
        return;

    glm::vec3 pos, vel;
    glm::mat3 mat;
    std::memcpy(&pos, s_pending.pos, sizeof(pos));
    std::memcpy(&mat, s_pending.mat, sizeof(mat));
    std::memcpy(&vel, s_pending.vel, sizeof(vel));

    ppo->pvtalo->pfnRotateAloToMat(ppo, &mat);
    TranslateSoToPosSafe(ppo, &pos, &g_normalZ);
    ppo->pvtalo->pfnSetAloVelocityVec(ppo, &g_vecZero);   // start at rest: the saved velocity flung Sly upwards
    ppo->pvtalo->pfnSetAloAngularVelocityVec(ppo, &g_vecZero);
    SetCmCut(g_pcm, nullptr);
    RESUME_LOG("[State] placed player at %.1f %.1f %.1f", pos.x, pos.y, pos.z);
}
