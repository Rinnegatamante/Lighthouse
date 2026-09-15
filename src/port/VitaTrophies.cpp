#include "VitaTrophies.h"

#ifdef __vita__

#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <iterator>
#include <tuple>
#include <vector>

#include "functions.h"
#include "enums.h"
#include "core1/sns.h"
#include "src/port/Enhancements/Events/Hooks/Events.h"
#include "src/port/Patches/Patches.h"

extern "C" {
void VitaTrophies_RequestRegistration(void);
int sceNpTrophyInit(void* options);
int sceNpTrophyCreateContext(int* context, const void* communicationId,
                             const void* communicationSignature, uint64_t options);
int sceNpTrophyCreateHandle(int* handle);
int sceNpTrophySetupDialogInit(void* parameter);
SceCommonDialogStatus sceNpTrophySetupDialogGetStatus(void);
int sceNpTrophySetupDialogTerm(void);
int sceNpTrophyUnlockTrophy(int context, int handle, int trophyId, int* platinumId);
int sceNpTrophyGetTrophyUnlockState(int context, int handle, void* state, uint32_t* count);
}

namespace {

struct TrophySetupDialogParam {
    int sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    int options;
    uint8_t reserved[128];
};

constexpr unsigned kTrophyCount = 72;
int sContext = -1;
int sHandle = -1;
bool sUnavailable = false;
bool sSetupComplete = false;
volatile bool sRegistrationRequested = false;
volatile bool sUnlockStateLoaded = false;
SceUID sRegistrationThread = -1;
SceUID sTrophyQueueSema = -1;
uint8_t sPendingTrophies[kTrophyCount]{};
volatile unsigned sPendingRead = 0;
volatile unsigned sPendingWrite = 0;
uint32_t sSubmitted[(kTrophyCount + 31) / 32]{};
uint32_t sUnlocked[(kTrophyCount + 31) / 32]{};
bool sGruntyFightActive = false;
bool sGruntyDamaged = false;
int sLastHealth = 0;
bool sVileUsedTurbo = false;
bool sClankerChallengeActive = false;
bool sClankerAirRefilled = false;
int sLastAir = 0;
int sLastGoldFeathers = -1;
bool sTtcBDiveUsed = false;
bool sMmmWellBDiveUsed = false;
uint8_t sBottlesPuzzleMask = 0;
std::vector<std::tuple<int, int, int, int>> sLifePickups;

void TrophyLog(const char* format, ...) {
    // Keep release builds quiet unless the user explicitly opts in by placing
    // an empty file named "debug" alongside Lighthouse's data files.
    FILE* debugFlag = std::fopen("ux0:data/lighthouse/debug", "rb");
    if (debugFlag == nullptr) return;
    std::fclose(debugFlag);

    FILE* file = std::fopen("ux0:data/lighthouse/trophy_debug.log", "a");
    if (file == nullptr) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
    std::fclose(file);
}

bool Ready() {
    TrophyLog("Ready: enter unavailable=%d context=%d handle=%d", sUnavailable, sContext, sHandle);
    if (sUnavailable) return false;
    if (sContext >= 0 && sHandle >= 0) return true;
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    TrophyLog("sceSysmoduleLoadModule=0x%08X", result);
    if (result < 0) {
        sUnavailable = true;
        return false;
    }
    result = sceNpTrophyInit(nullptr);
    TrophyLog("sceNpTrophyInit=0x%08X", result);
    if (result < 0) {
        sUnavailable = true;
        return false;
    }

    // The context ABI consumes a fixed 12-byte title-ID buffer, without _00.
    static constexpr char kCommunicationId[12] = "BANJ00064";
    static constexpr uint8_t kSignature[160] = { 0xb9, 0xdd, 0xe1, 0x3b, 0x01, 0x00 };
    result = sceNpTrophyCreateContext(&sContext, kCommunicationId, kSignature, 0);
    TrophyLog("sceNpTrophyCreateContext=0x%08X context=%d", result, sContext);
    if (result < 0) {
        sUnavailable = true;
        return false;
    }

    if (!sSetupComplete) {
        TrophySetupDialogParam parameter{};
        _sceCommonDialogSetMagicNumber(&parameter.commonParam);
        parameter.sdkVersion = PSP2_SDK_VERSION;
        parameter.context = sContext;
        result = sceNpTrophySetupDialogInit(&parameter);
        TrophyLog("sceNpTrophySetupDialogInit=0x%08X", result);
        if (result < 0) {
            sUnavailable = true;
            return false;
        }
        SceCommonDialogStatus status;
        do {
            status = sceNpTrophySetupDialogGetStatus();
            if (status == SCE_COMMON_DIALOG_STATUS_RUNNING) sceKernelDelayThread(16667);
        } while (status == SCE_COMMON_DIALOG_STATUS_RUNNING);
        result = sceNpTrophySetupDialogTerm();
        TrophyLog("sceNpTrophySetupDialogTerm=0x%08X status=%d", result, status);
        if (result < 0 || status != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            sUnavailable = true;
            return false;
        }
        sSetupComplete = true;
    }

    result = sceNpTrophyCreateHandle(&sHandle);
    TrophyLog("sceNpTrophyCreateHandle=0x%08X handle=%d", result, sHandle);
    if (result < 0) {
        sUnavailable = true;
        return false;
    }
    uint32_t count = 0;
    result = sceNpTrophyGetTrophyUnlockState(sContext, sHandle, sUnlocked, &count);
    TrophyLog("sceNpTrophyGetTrophyUnlockState=0x%08X count=%u", result, count);
    if (result < 0) {
        std::memset(sUnlocked, 0, sizeof(sUnlocked));
    }
    // Do not let a loaded/100% save enqueue trophies until the console's
    // existing unlock bitmap has been read.  The next game tick will
    // reconcile progress against this bitmap and queue only locked entries.
    __sync_synchronize();
    sUnlockStateLoaded = true;
    return true;
}

void Unlock(unsigned trophyId) {
    if (trophyId == 0 || trophyId >= kTrophyCount || !sRegistrationRequested || !sUnlockStateLoaded) return;
    const unsigned word = trophyId / 32;
    const uint32_t bit = UINT32_C(1) << (trophyId % 32);
    if ((sSubmitted[word] & bit) || (sUnlocked[word] & bit)) return;
    sSubmitted[word] |= bit;
    sPendingTrophies[sPendingWrite % kTrophyCount] = static_cast<uint8_t>(trophyId);
    __sync_synchronize();
    ++sPendingWrite;
    TrophyLog("queued unlock id=%u", trophyId);
    if (sTrophyQueueSema >= 0) sceKernelSignalSema(sTrophyQueueSema, 1);
}

bool LevelComplete(level_e level) {
    return jiggyscore_leveltotal(level) == 10 && itemscore_noteScores_get(level) == 100 &&
           honeycombscore_get_level_total(level) == 2;
}

int MumboTokenTotal() {
    int total = 0;
    for (int id = 1; id <= MUMBOTOKEN_73_CCW_WINTER_SIR_SLUSH_BETWEEN_BIG_FLOWER_AND_MUMBOS_SKULL; ++id) {
        total += mumboscore_get(static_cast<mumbotoken_e>(id)) ? 1 : 0;
    }
    return total;
}

int JiggyTotal(int first = JIGGY_01_MM_JINJO, int last = JIGGY_64_MMM_LOGGO) {
    int total = 0;
    for (int id = first; id <= last; ++id) {
        total += jiggyscore_isCollected(static_cast<jiggy_e>(id)) ? 1 : 0;
    }
    return total;
}

void ReconcilePersistentState() {
    static constexpr file_progress_e noteDoors[] = {
        FILEPROG_3A_NOTE_DOOR_50_OPEN, FILEPROG_3B_NOTE_DOOR_180_OPEN,
        FILEPROG_3C_NOTE_DOOR_260_OPEN, FILEPROG_3D_NOTE_DOOR_350_OPEN,
        FILEPROG_3E_NOTE_DOOR_450_OPEN, FILEPROG_3F_NOTE_DOOR_640_OPEN,
        FILEPROG_40_NOTE_DOOR_765_OPEN,
    };
    for (unsigned i = 0; i < std::size(noteDoors); ++i) {
        if (fileProgressFlag_get(noteDoors[i])) Unlock(1 + i);
    }

    if (honeycombscore_get_level_total(LEVEL_B_SPIRAL_MOUNTAIN) == 6) Unlock(8);
    static constexpr level_e worlds[] = {
        LEVEL_1_MUMBOS_MOUNTAIN, LEVEL_2_TREASURE_TROVE_COVE, LEVEL_3_CLANKERS_CAVERN,
        LEVEL_4_BUBBLEGLOOP_SWAMP, LEVEL_5_FREEZEEZY_PEAK, LEVEL_7_GOBIS_VALLEY,
        LEVEL_A_MAD_MONSTER_MANSION, LEVEL_9_RUSTY_BUCKET_BAY, LEVEL_8_CLICK_CLOCK_WOOD,
    };
    static constexpr unsigned worldTrophies[] = { 10, 12, 14, 17, 19, 20, 22, 23, 25 };
    for (unsigned i = 0; i < std::size(worlds); ++i) {
        if (LevelComplete(worlds[i])) Unlock(worldTrophies[i]);
    }

    if (honeycombscore_get_total() == 24 && fileProgressFlag_get(FILEPROG_B9_DOUBLE_HEALTH)) Unlock(26);
    const bool allCauldrons = fileProgressFlag_get(FILEPROG_49_PINK_CAULDRON_1_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_4A_PINK_CAULDRON_2_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_4B_GREEN_CAULDRON_1_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_4C_GREEN_CAULDRON_2_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_4D_RED_CAULDRON_1_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_4E_RED_CAULDRON_2_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_51_YELLOW_CAULDRON_1_ACTIVE) &&
        fileProgressFlag_get(FILEPROG_52_YELLOW_CAULDRON_2_ACTIVE);
    if (allCauldrons) Unlock(27);
    if (JiggyTotal(JIGGY_33_LAIR_1ST_JIGGY, JIGGY_3C_LAIR_CCW_WITCH_SWITCH) == 10) Unlock(28);
    if (JiggyTotal() == 100) Unlock(29);
    if (fileProgressFlag_get(FILEPROG_FC_DEFEAT_GRUNTY)) {
        Unlock(31);
        if (honeycombscore_get_total() == 0) Unlock(32);
        if (JiggyTotal() == 100) Unlock(35);
    }
    if (fileProgressFlag_get(FILEPROG_E1_UNKNOWN)) Unlock(33);
    if (fileProgressFlag_get(FILEPROG_A6_FURNACE_FUN_COMPLETE)) Unlock(34);
    if ((ability_getAllLearned() & ((1 << 19) - 1)) == ((1 << 19) - 1)) Unlock(37);
    if (fileProgressFlag_get(FILEPROG_AD_CHEATO_BLUEEGGS_UNLOCKED)) Unlock(38);
    if (fileProgressFlag_get(FILEPROG_AE_CHEATO_REDFEATHERS_UNLOCKED)) Unlock(39);
    if (fileProgressFlag_get(FILEPROG_AF_CHEATO_GOLDFEATHERS_UNLOCKED)) Unlock(40);
    if (MumboTokenTotal() == 115) Unlock(41);

    bool allStopAndSwop = true;
    for (int item = SNS_ITEM_EGG_YELLOW; item <= SNS_ITEM_ICE_KEY; ++item) {
        allStopAndSwop &= sns_get_item_state(static_cast<StopNSwop_Item>(item), SNS_COLLECTED);
    }
    if (allStopAndSwop) Unlock(42);
    if (item_getCount(ITEM_16_LIFE) >= 9) Unlock(43);

    const map_e map = gsworld_getMap();
    if (map == MAP_7_TTC_TREASURE_TROVE_COVE && bs_getState() == BS_2C_DIVE_B) sTtcBDiveUsed = true;
    if (map == MAP_25_MMM_WELL && bs_getState() == BS_2C_DIVE_B) sMmmWellBDiveUsed = true;

    // Furnace Fun Skip leaves the normal completion flag clear while allowing
    // Banjo to stand beside the Tooty prize actor.
    if (map == MAP_8E_GL_FURNACE_FUN && !fileProgressFlag_get(FILEPROG_A6_FURNACE_FUN_COMPLETE)) {
        float position[3];
        float distance = 0.0f;
        player_getPosition(position);
        if (actorArray_findClosestActorFromActorId(position, ACTOR_3C8_FF_PRIZE_TOOTY, 0, &distance) != nullptr &&
            distance < 500.0f) {
            Unlock(71);
        }
    }
}

void RegisterHooks() {
    REGISTER_LISTENER(OnPlayerTransformChange, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* transform = reinterpret_cast<OnPlayerTransformChange*>(event);
        if (map_getLevel(gsworld_getMap()) == LEVEL_6_LAIR && transform->tf_id != TRANSFORM_1_BANJO) {
            sLifePickups.erase(std::remove_if(sLifePickups.begin(), sLifePickups.end(), [](const auto& key) {
                return map_getLevel(static_cast<map_e>(std::get<0>(key))) == LEVEL_6_LAIR;
            }), sLifePickups.end());
        }
        switch (transform->tf_id) {
            case TRANSFORM_2_TERMITE: Unlock(9); break;
            case TRANSFORM_3_PUMPKIN: Unlock(21); break;
            case TRANSFORM_4_WALRUS: Unlock(18); break;
            case TRANSFORM_5_CROC: Unlock(15); break;
            case TRANSFORM_6_BEE: Unlock(24); break;
            case TRANSFORM_7_WISHWASHY: Unlock(55); break;
            default: break;
        }
    });
    REGISTER_LISTENER(OnBottlesBonusComplete, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* bonus = reinterpret_cast<OnBottlesBonusComplete*>(event);
        if (bonus->index >= 0 && bonus->index < 7) {
            sBottlesPuzzleMask |= static_cast<uint8_t>(1u << bonus->index);
            if (sBottlesPuzzleMask == 0x7f) Unlock(36);
        }
    });
    REGISTER_LISTENER(OnSandcastleCheatEntered, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* cheat = reinterpret_cast<OnSandcastleCheatEntered*>(event);
        if (cheat->index >= 4 && cheat->index <= 8) Unlock(56 + cheat->index - 4);
        if (cheat->index == 10) Unlock(55);
    });
    REGISTER_LISTENER(OnLighthouseTopExit, EVENT_PRIORITY_NORMAL, [](IEvent*) { Unlock(11); });
    REGISTER_LISTENER(OnMapLoad, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* load = reinterpret_cast<OnMapLoad*>(event);
        if (load->nextMap == MAP_7_TTC_TREASURE_TROVE_COVE) sTtcBDiveUsed = false;
        if (load->nextMap == MAP_25_MMM_WELL) sMmmWellBDiveUsed = false;
        if (map_getLevel(load->nextMap) == LEVEL_C_BOSS) {
            sGruntyFightActive = true;
            sGruntyDamaged = false;
            sLastHealth = item_getCount(ITEM_14_HEALTH);
        } else if (map_getLevel(load->prevMap) == LEVEL_C_BOSS) {
            sGruntyFightActive = false;
        }
    });
    REGISTER_LISTENER(OnItemCountChanged, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* item = reinterpret_cast<OnItemCountChanged*>(event);
        if (item->item == ITEM_14_HEALTH && sGruntyFightActive) {
            if (item->count < sLastHealth) sGruntyDamaged = true;
            sLastHealth = item->count;
        }
        if (item->item == ITEM_17_AIR && sClankerChallengeActive) {
            if (item->count > sLastAir) sClankerAirRefilled = true;
            sLastAir = item->count;
        }
        if (item->item == ITEM_10_GOLD_FEATHER) {
            if (sLastGoldFeathers >= 0 && item->count > sLastGoldFeathers && item->count <= 10 &&
                gsworld_getMap() == MAP_72_GL_BGS_LOBBY &&
                player_getWaterState() == BSWATERGROUP_2_UNDERWATER) {
                Unlock(68);
            }
            sLastGoldFeathers = item->count;
        }
    });
    REGISTER_LISTENER(OnPlayerDeath, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        sGruntyFightActive = false;
        sClankerChallengeActive = false;
        sTtcBDiveUsed = false;
        sMmmWellBDiveUsed = false;
        sLifePickups.clear();
    });
    REGISTER_LISTENER(OnCollectibleCollected, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* collectible = reinterpret_cast<OnCollectibleCollected*>(event);
        if (collectible->kind == ANCHOR_COLLECTIBLE_HONEYCOMB &&
            collectible->id == HONEYCOMB_3_TTC_UNDERWATER && !sTtcBDiveUsed) {
            Unlock(63);
            return;
        }
        if (collectible->kind != ANCHOR_COLLECTIBLE_JIGGY) return;
        switch (collectible->id) {
            case JIGGY_02_MM_TICKERS_TOWER:
                if (player_getTransformation() != TRANSFORM_2_TERMITE) Unlock(61);
                break;
            case JIGGY_06_MM_RUINS:
                if (player_getTransformation() != TRANSFORM_2_TERMITE) Unlock(62);
                break;
            case JIGGY_1B_CC_TOOTH:
                if (!jiggyscore_isCollected(JIGGY_17_CC_CLANKER_RAISED)) Unlock(64);
                break;
            case JIGGY_3F_GV_SHYINX:
                if (!(port_puzzleStep_get(ANCHOR_PUZZLE_GV_JINXY_DOOR) & 1)) Unlock(65);
                break;
            case JIGGY_33_LAIR_1ST_JIGGY:
                if (!fileProgressFlag_get(FILEPROG_9F_BGS_WITCH_SWITCH_JIGGY_PRESSED)) Unlock(66);
                break;
            case JIGGY_3A_LAIR_GV_WITCH_SWITCH:
                if (!fileProgressFlag_get(FILEPROG_A0_GV_WITCH_SWITCH_JIGGY_PRESSED)) Unlock(67);
                break;
            case JIGGY_5C_MMM_WELL:
                if (!sMmmWellBDiveUsed) Unlock(69);
                break;
            case JIGGY_4B_CCW_GNAWTY:
                if (fileProgressFlag_get(FILEPROG_8B_CCW_SPRING_OPEN) &&
                    !fileProgressFlag_get(FILEPROG_8C_CCW_SUMMER_OPEN) &&
                    !fileProgressFlag_get(FILEPROG_8D_CCW_AUTUMN_OPEN) &&
                    !fileProgressFlag_get(FILEPROG_8E_CCW_WINTER_OPEN)) Unlock(70);
                break;
            default: break;
        }
    });
    REGISTER_LISTENER(OnExtraLifeCollected, EVENT_PRIORITY_NORMAL, ([](IEvent* event) {
        const auto* life = reinterpret_cast<OnExtraLifeCollected*>(event);
        if (life->map == MAP_8E_GL_FURNACE_FUN ||
            (map_getLevel(static_cast<map_e>(life->map)) == LEVEL_B_SPIRAL_MOUNTAIN &&
             volatileFlag_get(VOLATILE_FLAG_2_FF_IN_MINIGAME))) return;
        const auto key = std::make_tuple(life->map, static_cast<int>(life->x / 10.0f),
                                         static_cast<int>(life->y / 10.0f), static_cast<int>(life->z / 10.0f));
        if (std::find(sLifePickups.begin(), sLifePickups.end(), key) == sLifePickups.end()) {
            sLifePickups.push_back(key);
        }
        const level_e level = map_getLevel(static_cast<map_e>(life->map));
        static constexpr level_e levels[] = {
            LEVEL_B_SPIRAL_MOUNTAIN, LEVEL_1_MUMBOS_MOUNTAIN, LEVEL_2_TREASURE_TROVE_COVE,
            LEVEL_3_CLANKERS_CAVERN, LEVEL_4_BUBBLEGLOOP_SWAMP, LEVEL_5_FREEZEEZY_PEAK,
            LEVEL_7_GOBIS_VALLEY, LEVEL_A_MAD_MONSTER_MANSION, LEVEL_9_RUSTY_BUCKET_BAY,
            LEVEL_8_CLICK_CLOCK_WOOD, LEVEL_6_LAIR,
        };
        static constexpr unsigned required[] = { 2, 2, 3, 2, 2, 3, 3, 3, 3, 12, 6 };
        for (unsigned i = 0; i < std::size(levels); ++i) {
            if (level != levels[i]) continue;
            unsigned count = 0;
            for (const auto& collected : sLifePickups) {
                if (map_getLevel(static_cast<map_e>(std::get<0>(collected))) == level) ++count;
            }
            if (count >= required[i]) Unlock(44 + i);
            break;
        }
    }));
    REGISTER_LISTENER(OnGruntyJinjonatorComplete, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        if (sGruntyFightActive && !sGruntyDamaged) Unlock(30);
        Unlock(31);
        sGruntyFightActive = false;
    });
    REGISTER_LISTENER(OnVileGameStateChange, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* state = reinterpret_cast<OnVileGameStateChange*>(event);
        if (state->state == 4 || state->state == 5) sVileUsedTurbo = false;
    });
    REGISTER_LISTENER(OnTurboTrainerUsed, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        if (map_getLevel(gsworld_getMap()) == LEVEL_4_BUBBLEGLOOP_SWAMP) sVileUsedTurbo = true;
    });
    REGISTER_LISTENER(OnVileVictory, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        const auto* victory = reinterpret_cast<OnVileVictory*>(event);
        if (victory->mode == 4 && !sVileUsedTurbo) Unlock(16);
    });
    REGISTER_LISTENER(OnClankerChallengeStart, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        sClankerChallengeActive = true;
        sClankerAirRefilled = false;
        sLastAir = item_getCount(ITEM_17_AIR);
    });
    REGISTER_LISTENER(OnClankerReleased, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        if (sClankerChallengeActive && !sClankerAirRefilled) Unlock(13);
        sClankerChallengeActive = false;
    });
}

} // namespace

namespace VitaTrophies {
void Register() {
    VitaTrophies_RequestRegistration();
}
void Pump() { ReconcilePersistentState(); }
} // namespace VitaTrophies

static int TrophyRegistrationWorker(SceSize, void*) {
    TrophyLog("registration worker entered");
    const bool ready = Ready();
    TrophyLog("registration worker finished ready=%d", ready ? 1 : 0);
    while (ready) {
        if (sceKernelWaitSema(sTrophyQueueSema, 1, nullptr) < 0) continue;
        __sync_synchronize();
        const unsigned trophyId = sPendingTrophies[sPendingRead % kTrophyCount];
        ++sPendingRead;
        int platinumId = -1;
        const int result = sceNpTrophyUnlockTrophy(sContext, sHandle, static_cast<int>(trophyId), &platinumId);
        TrophyLog("unlock id=%u result=0x%08X platinum=%d", trophyId, result, platinumId);
        if (result >= 0) {
            __sync_fetch_and_or(&sUnlocked[trophyId / 32], UINT32_C(1) << (trophyId % 32));
        } else {
            // Allow the game-thread reconciliation pass to retry a transient
            // NP failure instead of permanently suppressing this trophy.
            __sync_fetch_and_and(&sSubmitted[trophyId / 32],
                                 ~(UINT32_C(1) << (trophyId % 32)));
        }
    }
    return 0;
}

extern "C" void VitaTrophies_RequestRegistration(void) {
    if (sRegistrationRequested) return;
    sRegistrationRequested = true;
    RegisterHooks();
    TrophyLog("registration requested after game selection");
    sTrophyQueueSema = sceKernelCreateSema("Lighthouse trophy queue", 0, 0, kTrophyCount, nullptr);
    TrophyLog("sceKernelCreateSema=0x%08X", sTrophyQueueSema);
    if (sTrophyQueueSema < 0) return;
    sRegistrationThread = sceKernelCreateThread("Lighthouse trophy setup", TrophyRegistrationWorker,
                                                0x10000100, 0x40000, 0, 0, nullptr);
    TrophyLog("sceKernelCreateThread=0x%08X", sRegistrationThread);
    if (sRegistrationThread < 0) return;
    const int result = sceKernelStartThread(sRegistrationThread, 0, nullptr);
    TrophyLog("sceKernelStartThread=0x%08X", result);
}

#else

namespace VitaTrophies {
void Register() {}
void Pump() {}
} // namespace VitaTrophies

#endif
