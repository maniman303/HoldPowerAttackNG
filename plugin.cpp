#include <SimpleIni.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;
using namespace RE;
using namespace RE::BSScript;
using namespace SKSE;
using namespace SKSE::stl;

const bool IS_DEBUG = false;

const int ACTION_MAX_RETRY = 4;
const uint64_t DUAL_ATTACK_TIME_DIFF = 110;
const int POWER_ATTACK_MIN_HOLD_TIME = 440;
const int VIBRATION_STRENGTH = 25;
const int DEFAULT_LEFT_BUTTON = 280;
const int DEFAULT_RIGHT_BUTTON = 281;

bool isEnabled = true;
bool isSoundEnabled = true;
bool isVibrationEnabled = true;
float minPowerAttackHoldMs = 0.44f;
float vibrationStrength = 0.25f;
uint64_t leftButton = DEFAULT_LEFT_BUTTON;
uint64_t rightButton = DEFAULT_RIGHT_BUTTON;
bool isMouseReversed = false;

const TaskInterface* tasks = NULL;
BSAudioManager* audioManager = NULL;

BGSSoundDescriptorForm* powerAttackSound;

BGSAction* actionRightAttack;
BGSAction* actionLeftAttack;
BGSAction* actionDualAttack;
BGSAction* actionRightPowerAttack;
BGSAction* actionLeftPowerAttack;
BGSAction* actionDualPowerAttack;
BGSAction* actionRightRelease;

float leftHoldTime = 0.0f;
float rightHoldTime = 0.0f;

uint64_t leftLastTime = 0;
uint64_t rightLastTime = 0;

bool isLeftDualHeld = false;
bool isRightDualHeld = false;

bool leftAltBehavior = false;
bool rightAltBehavior = false;

bool isLeftAttackIndicated = false;
bool isRightAttackIndicated = false;

void SetIsAttackIndicated(bool isLeft, bool value) {
    if (isLeft) {
        isLeftAttackIndicated = value;
    } else {
        isRightAttackIndicated = value;
    }
}

void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    
    auto log = std::make_shared<spdlog::logger>(
        "Global", std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true));
    log->set_level(spdlog::level::trace);
    log->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(std::move(log));
}

long Limit(long min, long value, long max) {
    if (value < min) {
        return min;
    }

    if (value > max) {
        return max;
    }

    return value;
}

int LimitGamepadButton(int value, int defaultValue) {
    if (value < 266 || value > 281) {
        return defaultValue;
    }

    return value;
}

void LoadSettings() {
    constexpr auto path = L"Data/SKSE/Plugins/HoldPowerAttackNG.ini";

    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(path);

    isEnabled = ini.GetBoolValue("Settings", "Enabled", true);
    isSoundEnabled = ini.GetBoolValue("Settings", "Sound", true);
    isVibrationEnabled = ini.GetBoolValue("Settings", "Vibration", true);
    minPowerAttackHoldMs = ini.GetLongValue("Settings", "MinPowerAttackHoldMs", POWER_ATTACK_MIN_HOLD_TIME) / 1000.0f;
    vibrationStrength = Limit(0, ini.GetLongValue("Settings", "VibrationStrength", VIBRATION_STRENGTH), 200) / 100.0f;
    leftButton =
        LimitGamepadButton(ini.GetLongValue("Buttons", "OverrideLeftButton", DEFAULT_LEFT_BUTTON), DEFAULT_LEFT_BUTTON);
    rightButton =
        LimitGamepadButton(ini.GetLongValue("Buttons", "OverrideRightButton", DEFAULT_RIGHT_BUTTON), DEFAULT_RIGHT_BUTTON);
    isMouseReversed = ini.GetBoolValue("Buttons", "ReverseMouseButtons", false);

    ini.SetBoolValue("Settings", "Enabled", isEnabled);
    ini.SetBoolValue("Settings", "Sound", isSoundEnabled);
    ini.SetBoolValue("Settings", "Vibration", isVibrationEnabled);
    ini.SetLongValue("Settings", "MinPowerAttackHoldMs", (long)(minPowerAttackHoldMs * 1000.0f));
    ini.SetLongValue("Settings", "VibrationStrength", (long)(vibrationStrength * 100.0f));
    ini.SetLongValue("Buttons", "OverrideLeftButton", (long)leftButton);
    ini.SetLongValue("Buttons", "OverrideRightButton", (long)rightButton);
    ini.SetBoolValue("Buttons", "ReverseMouseButtons", isMouseReversed);

    (void)ini.SaveFile(path);
}

uint64_t AbsDiff(uint64_t left, uint64_t right) {
    if (right > left) {
        return right - left;
    }

    return left - right;
}

float Max(float left, float right) {
    if (left > right) {
        return left;
    }

    return right;
}

float Min(float left, float right) {
    if (left < right) {
        return left;
    }

    return right;
}

uint64_t TimeMillisec() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void PerformAction(BGSAction* action, Actor* actor, int index) {
    if (tasks == NULL) {
        logger::info("Tasks not initialized.");

        return;
    }
    
    tasks->AddTask([action, actor, index]() {
        std::unique_ptr<TESActionData> data(TESActionData::Create());
        data->source = NiPointer<TESObjectREFR>(actor);
        data->action = action;
        typedef bool func_t(TESActionData*);
        REL::Relocation<func_t> func{RELOCATION_ID(40551, 41557)};
        bool succ = func(data.get());

        if (!succ && index >= ACTION_MAX_RETRY && IS_DEBUG) {
            logger::info("Failed to perform action.");
        }

        if (!succ && index < ACTION_MAX_RETRY) {
            std::thread thread([action, actor, index]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                
                if (IS_DEBUG) {
                    logger::info("What.");
                }

                PerformAction(action, actor, index + 1);
            });
            thread.detach();
        }
    });
}

void PerformAction(BGSAction* action, Actor* actor, bool isPowerAttack) {
    int index = isPowerAttack ? 0 : ACTION_MAX_RETRY;
    PerformAction(action, actor, index);
}

void PlayDebugSound(BGSSoundDescriptorForm* sound, PlayerCharacter* player) {
    if (!isSoundEnabled) {
        return;
    }

    if (!audioManager || !sound) {
        logger::info("Audio manager: {0}, sound {1}", audioManager != NULL, sound != NULL);
        return;
    }

    BSSoundHandle handle;
    audioManager->BuildSoundDataFromDescriptor(handle, sound->soundDescriptor);
    handle.SetVolume(1.0f);
    handle.SetObjectToFollow(player->Get3D());
    handle.Play();
}

static void VibrateImp(std::int32_t type, float power, float duration) {
    using func_t = decltype(&VibrateImp);
    REL::Relocation<func_t> func{REL::RelocationID(67220, 68528)};
    func(type, power, duration);
}

static void Vibrate(float power, float duration) {
    VibrateImp(0, power, duration);
    VibrateImp(1, power, duration);
}

uint32_t GamepadKeycode(uint32_t dxScanCode) {
    int dxGamepadKeycode = -1;
    RE::BSWin32GamepadDevice::Key gamepadKey = static_cast<RE::BSWin32GamepadDevice::Key>(dxScanCode);
    switch (gamepadKey) {
        case RE::BSWin32GamepadDevice::Key::kUp:
            dxGamepadKeycode = 266;
            break;
        case RE::BSWin32GamepadDevice::Key::kDown:
            dxGamepadKeycode = 267;
            break;
        case RE::BSWin32GamepadDevice::Key::kLeft:
            dxGamepadKeycode = 268;
            break;
        case RE::BSWin32GamepadDevice::Key::kRight:
            dxGamepadKeycode = 269;
            break;
        case RE::BSWin32GamepadDevice::Key::kStart:
            dxGamepadKeycode = 270;
            break;
        case RE::BSWin32GamepadDevice::Key::kBack:
            dxGamepadKeycode = 271;
            break;
        case RE::BSWin32GamepadDevice::Key::kLeftThumb:
            dxGamepadKeycode = 272;
            break;
        case RE::BSWin32GamepadDevice::Key::kRightThumb:
            dxGamepadKeycode = 273;
            break;
        case RE::BSWin32GamepadDevice::Key::kLeftShoulder:
            dxGamepadKeycode = 274;
            break;
        case RE::BSWin32GamepadDevice::Key::kRightShoulder:
            dxGamepadKeycode = 275;
            break;
        case RE::BSWin32GamepadDevice::Key::kA:
            dxGamepadKeycode = 276;
            break;
        case RE::BSWin32GamepadDevice::Key::kB:
            dxGamepadKeycode = 277;
            break;
        case RE::BSWin32GamepadDevice::Key::kX:
            dxGamepadKeycode = 278;
            break;
        case RE::BSWin32GamepadDevice::Key::kY:
            dxGamepadKeycode = 279;
            break;
        case RE::BSWin32GamepadDevice::Key::kLeftTrigger:
            dxGamepadKeycode = 280;
            break;
        case RE::BSWin32GamepadDevice::Key::kRightTrigger:
            dxGamepadKeycode = 281;
            break;
        default:
            dxGamepadKeycode = static_cast<uint32_t>(-1);
            break;
    }
    return dxGamepadKeycode;
}

bool IsPlayerAttacking(PlayerCharacter* player) {
    if (player->AsActorState()->GetSitSleepState() == SIT_SLEEP_STATE::kNormal && !player->IsInKillMove()) {
        ATTACK_STATE_ENUM currentState = (player->AsActorState()->actorState1.meleeAttackState);
        if (currentState >= ATTACK_STATE_ENUM::kDraw && currentState <= ATTACK_STATE_ENUM::kBash) {
            return true;
        } else {
            return false;
        }
    }

    return false;
}

float GetPlayerStamina(PlayerCharacter* player) {
    return player->AsActorValueOwner()->GetActorValue(ActorValue::kStamina);
}

bool IsPowerAttack(PlayerCharacter* player, float maxDuration, bool isOtherHandBusy) {
    if (GetPlayerStamina(player) <= 1.0f) {
        return false;
    }

    auto isPowerAttack = maxDuration > minPowerAttackHoldMs;

    if (isOtherHandBusy) {
        isPowerAttack = false;
    }

    return isPowerAttack;
}

BGSAction* GetAttackAction(bool isLeft, uint64_t timeDiff, bool isDualWielding, bool isDualHeld, bool isPowerAttack) {
    if (isDualWielding && isDualHeld && timeDiff < DUAL_ATTACK_TIME_DIFF) {
        return isPowerAttack ? actionDualPowerAttack : actionDualAttack;
    }

    if (isLeft) {
        return isPowerAttack ? actionLeftPowerAttack : actionLeftAttack;
    }

    return isPowerAttack ? actionRightPowerAttack : actionRightAttack;
}

bool IsEventLeft(ButtonEvent* a_event) {
    auto device = a_event->device.get();
    auto keyMask = a_event->GetIDCode();

    if (device == INPUT_DEVICE::kMouse && keyMask == (uint32_t)(isMouseReversed ? 0 : 1)) return true;
    if (device == INPUT_DEVICE::kGamepad && GamepadKeycode(keyMask) == leftButton) return true;

    return false;
}

bool IsWeaponValid(TESObjectWEAP* weapon, bool isLeft) {
    if (weapon == NULL) {
        return false;
    }

    if (!weapon->IsWeapon() || weapon->IsBow() || weapon->IsCrossbow() || weapon->IsStaff()) {
        return false;
    }

    if (isLeft && (weapon->IsTwoHandedAxe() || weapon->IsTwoHandedSword())) {
        return false;
    }

    return true;
}

bool IsDualWielding() {
    const auto player = PlayerCharacter::GetSingleton();

    auto weaponLeft = reinterpret_cast<TESObjectWEAP*>(player->GetEquippedObject(true));
    auto weaponRight = reinterpret_cast<TESObjectWEAP*>(player->GetEquippedObject(false));

    return IsWeaponValid(weaponLeft, true) && IsWeaponValid(weaponRight, false);
}

bool IsButtonEventValid(ButtonEvent* a_event) {
    if (!isEnabled) {
        return false;
    }

    auto device = a_event->device.get();
    auto keyMask = a_event->GetIDCode();

    if ((device != INPUT_DEVICE::kMouse && device != INPUT_DEVICE::kGamepad) ||
        (device == INPUT_DEVICE::kGamepad && GamepadKeycode(keyMask) != leftButton && GamepadKeycode(keyMask) != rightButton) ||
        (device == INPUT_DEVICE::kMouse && keyMask != 0 && keyMask != 1)) {
        return false;
    }

    return true;
}

bool IsEventValid(ButtonEvent* a_event) {
    if (!isEnabled) {
        return false;
    }

    if (!IsButtonEventValid(a_event)) {
        return false;
    }

    auto isLeft = IsEventLeft(a_event);

    const auto gameUI = UI::GetSingleton();
    const auto controlMap = ControlMap::GetSingleton();

    if (gameUI == NULL || controlMap == NULL || (gameUI && gameUI->GameIsPaused()) || controlMap == NULL) {
        return false;
    }

    const auto player = PlayerCharacter::GetSingleton();

    if (player == NULL || player->IsInKillMove()) {
        return false;
    }

    auto playerState = player->AsActorState();

    if (playerState == NULL || (playerState && (playerState->GetWeaponState() != WEAPON_STATE::kDrawn ||
                                                playerState->GetSitSleepState() != SIT_SLEEP_STATE::kNormal ||
                                                playerState->GetKnockState() != KNOCK_STATE_ENUM::kNormal ||
                                                playerState->GetFlyState() != FLY_STATE::kNone))) {
        return false;
    }

    auto weapon = reinterpret_cast<TESObjectWEAP*>(player->GetEquippedObject(isLeft));

    return IsWeaponValid(weapon, isLeft);
}

// Fired when the user presses the attack or block key
class HookAttackBlockHandler {
public:
    typedef void (HookAttackBlockHandler::*FnProcessButton)(ButtonEvent*, void*);
    void ProcessButton(ButtonEvent* a_event, void* a_data) {
        FnProcessButton fn = fnHash.at(*(uintptr_t*)this);

        if (IsEventValid(a_event)) {
            ProcessEvent(a_event);

            return;
        }

        if (IsButtonEventValid(a_event)) {
            auto isLeft = IsEventLeft(a_event);

            if (isLeft) {
                leftAltBehavior = a_event->IsHeld();
            } else {
                rightAltBehavior = a_event->IsHeld();
            }

            auto isAltBehavior = isLeft ? leftAltBehavior : rightAltBehavior;

            if (isAltBehavior) {
                SetIsAttackIndicated(isLeft, false);
            }
        }

        if (fn) (this->*fn)(a_event, a_data);
    }

    static void Hook() {
        REL::Relocation<uintptr_t> vtable{VTABLE_AttackBlockHandler[0]};
        FnProcessButton fn =
            stl::unrestricted_cast<FnProcessButton>(vtable.write_vfunc(4, &HookAttackBlockHandler::ProcessButton));
        fnHash.insert(std::pair<uintptr_t, FnProcessButton>(vtable.address(), fn));

        logger::info("Hooked Attack System OK...");
    }

private:
    static std::unordered_map<uintptr_t, FnProcessButton> fnHash;

    void ProcessEventUp(PlayerCharacter* playerCharacter, bool isLeft) {
        auto tempLeftHoldTime = leftHoldTime;
        auto tempRightHoldTime = rightHoldTime;

        auto tempIsLeftDualHeld = isLeftDualHeld;
        auto tempIsRightDualHeld = isRightDualHeld;

        auto shouldAttack = false;
        uint64_t timeDiff = 0;

        if (isLeft) {
            leftHoldTime = 0.0f;
            leftLastTime = TimeMillisec();
            isRightDualHeld = false;
            shouldAttack = tempRightHoldTime == 0.0f;
        } else {
            rightHoldTime = 0.0f;
            rightLastTime = TimeMillisec();
            isLeftDualHeld = false;
            shouldAttack = tempLeftHoldTime == 0.0f;
        }

        timeDiff = AbsDiff(leftLastTime, rightLastTime);

        bool isBlocking = false;
        playerCharacter->GetGraphVariableBool("IsBlocking", isBlocking);

        if (shouldAttack || (timeDiff == 0 && isLeft)) {
            if (timeDiff == 0) {
                logger::info("Nice reflex!");
            }

            SetIsAttackIndicated(isLeft, false);

            auto isDualWielding = IsDualWielding();
            auto isDualHeld = isLeft ? tempIsRightDualHeld : tempIsLeftDualHeld;

            auto isAttacking = IsPlayerAttacking(playerCharacter);
            auto isPowerAttack =
                IsPowerAttack(playerCharacter, Max(tempLeftHoldTime, tempRightHoldTime), (leftAltBehavior || rightAltBehavior) && !isBlocking);
            auto attackAction = GetAttackAction(isLeft, timeDiff, isDualWielding, isDualHeld, false);

            if (!isPowerAttack || (isPowerAttack && !isAttacking)) {
                logger::info("Normal attack");

                PerformAction(attackAction, playerCharacter, false);

                if (!isLeft && !isPowerAttack && isBlocking) {
                    PerformAction(actionRightRelease, playerCharacter, false);
                }
            }

            if (isPowerAttack && !isAttacking && !isBlocking) {
                attackAction = GetAttackAction(isLeft, timeDiff, isDualWielding, isDualHeld, true);

                logger::info("Power attack");

                PerformAction(attackAction, playerCharacter, true);
            }
        }
    }

    void TryIndicatePowerAttack(bool isLeft, PlayerCharacter* player) {
        bool altHandBehavior = isLeft ? rightAltBehavior : leftAltBehavior;
        float holdTime = isLeft ? leftHoldTime : rightHoldTime;

        bool isBlocking = false;
        player->GetGraphVariableBool("IsBlocking", isBlocking);

        bool isPlayerAttacking = IsPlayerAttacking(player);
        bool isPowerAttack = IsPowerAttack(player, holdTime, altHandBehavior && !isBlocking);
        
        if (!isPlayerAttacking && isPowerAttack) {
            if (isLeftAttackIndicated || isRightAttackIndicated) {
                return;
            }

            SetIsAttackIndicated(isLeft, true);

            PlayDebugSound(powerAttackSound, player);
            
            Vibrate(vibrationStrength, 0.24f);
        } else {
            SetIsAttackIndicated(isLeft, false);
        }
    }

    void ProcessEvent(ButtonEvent* buttonEvent) {
        const auto playerCharacter = PlayerCharacter::GetSingleton();

        auto isLeft = IsEventLeft(buttonEvent);

        if (buttonEvent->IsDown() || buttonEvent->IsHeld()) {
            if (isLeft) {
                leftHoldTime = buttonEvent->HeldDuration();
                leftAltBehavior = false;
                isRightDualHeld = isRightDualHeld || rightHoldTime > 0.0f;
            } else {
                rightHoldTime = buttonEvent->HeldDuration();
                rightAltBehavior = false;
                isLeftDualHeld = isLeftDualHeld || leftHoldTime > 0.0f;
            }

            TryIndicatePowerAttack(isLeft, playerCharacter);
        }

        if (buttonEvent->IsUp()) {
            ProcessEventUp(playerCharacter, isLeft);
        }
    }
};
std::unordered_map<uintptr_t, HookAttackBlockHandler::FnProcessButton> HookAttackBlockHandler::fnHash;

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        actionRightAttack = (BGSAction*)TESForm::LookupByID(0x13005);
        actionLeftAttack = (BGSAction*)TESForm::LookupByID(0x13004);
        actionDualAttack = (BGSAction*)TESForm::LookupByID(0x50c96);
        actionRightPowerAttack = (BGSAction*)TESForm::LookupByID(0x13383);
        actionLeftPowerAttack = (BGSAction*)TESForm::LookupByID(0x2e2f6);
        actionDualPowerAttack = (BGSAction*)TESForm::LookupByID(0x2e2f7);
        actionRightRelease = (BGSAction*)TESForm::LookupByID(0x13454);
        powerAttackSound = (BGSSoundDescriptorForm*)TESForm::LookupByID(0x10eb7a);

        audioManager = BSAudioManager::GetSingleton();

        HookAttackBlockHandler::Hook();
    }
}

SKSEPluginLoad(const LoadInterface* skse) {
    Init(skse);

    SetupLog();
    logger::info("Setup log...");

    LoadSettings();
    logger::info("Settings loaded...");

    if (!isEnabled) {
        logger::info("Mod is disabled...");
    }

    tasks = GetTaskInterface();
    GetMessagingInterface()->RegisterListener(OnMessage);

    return true;
}