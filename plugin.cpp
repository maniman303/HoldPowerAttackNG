#include <SimpleIni.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;
using namespace RE;
using namespace RE::BSScript;
using namespace SKSE;
using namespace SKSE::stl;

const bool isEnabled = true;

const bool IS_DEBUG = false;

const int ACTION_MAX_RETRY = 4;
const uint64_t DUAL_ATTACK_TIME_DIFF = 110;
const float POWER_ATTACK_MIN_HOLD_TIME = 0.44f;
const float POWER_ATTACK_SOUND_OFFSET = 0.25f;

enum VibrationType { kSmooth, kDiscrete, kBump };

const TaskInterface* tasks = NULL;
BSAudioManager* audioManager = NULL;

BGSSoundDescriptorForm* powerAttackSound;

std::string rightHand = "player.pa ActionRightAttack";
std::string leftHand = "player.pa ActionLeftAttack";
std::string bothHands = "player.pa ActionDualAttack";
std::string rightPowerHand = "player.pa ActionRightPowerAttack";
std::string leftPowerHand = "player.pa ActionLeftPowerAttack";
std::string bothPowerHands = "player.pa ActionDualPowerAttack";

BGSAction* actionRightAttack;
BGSAction* actionLeftAttack;
BGSAction* actionDualAttack;
BGSAction* actionRightPowerAttack;
BGSAction* actionLeftPowerAttack;
BGSAction* actionDualPowerAttack;

bool isAttacking;

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

std::string isEnabledTest = "true";

void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
}

void LoadSettings() {
    constexpr auto path = L"Data/SKSE/Plugins/HoldPowerAttack.ini";

    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(path);

    // isEnabledTest = std::stoi(ini.GetValue("Settings", "IsEnabled", "true"));

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

void RunConsoleCommand(std::string a_command) {
    const auto scriptFactory = IFormFactory::GetConcreteFormFactoryByType<Script>();
    const auto script = scriptFactory ? scriptFactory->Create() : nullptr;
    if (script) {
        const auto selectedRef = Console::GetSelectedRef();
        script->SetCommand(a_command);
        script->CompileAndRun(selectedRef.get());
        delete script;
    }
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
    if (!audioManager || !sound) {
        logger::info("Audio manager: {0}, sound {1}", audioManager != NULL, sound != NULL);
        return;
    }

    BSSoundHandle handle;
    audioManager->BuildSoundDataFromDescriptor(handle, sound->soundDescriptor);
    handle.SetVolume(0.65f);
    handle.SetObjectToFollow(player->Get3D());
    handle.Play();
}

static void Vibrate(std::int32_t type, float power, float duration) {
    using func_t = decltype(&Vibrate);
    REL::Relocation<func_t> func{REL::RelocationID(67220, 68528)};
    func(type, power, duration);
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

bool IsPowerAttack(float maxDuration, bool isOtherHandBusy) {
    auto isPowerAttack = maxDuration > POWER_ATTACK_MIN_HOLD_TIME;

    if (isOtherHandBusy) {
        isPowerAttack = false;
    }

    return isPowerAttack;
}

std::string GetAttackCommand(bool isLeft, uint64_t timeDiff, bool isDualWielding, bool isDualHeld, bool isPowerAttack) {
    if (isDualWielding && isDualHeld && timeDiff < DUAL_ATTACK_TIME_DIFF) {
        return isPowerAttack ? bothPowerHands : bothHands;
    }

    if (isLeft) {
        return isPowerAttack ? leftPowerHand : leftHand;
    }

    return isPowerAttack ? rightPowerHand : rightHand;
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

    if (device == INPUT_DEVICE::kMouse && keyMask == 1) return true;
    if (device == INPUT_DEVICE::kGamepad && GamepadKeycode(keyMask) == 280) return true;

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

    /*if (weaponLeft && weaponLeft == weaponRight)
    {
        return false;
    }*/

    return IsWeaponValid(weaponLeft, true) && IsWeaponValid(weaponRight, false);
}

bool IsButtonEventValid(ButtonEvent* a_event) {
    if (!isEnabled) {
        return false;
    }

    auto device = a_event->device.get();
    auto keyMask = a_event->GetIDCode();
    if ((device != INPUT_DEVICE::kMouse && device != INPUT_DEVICE::kGamepad) ||
        (device == INPUT_DEVICE::kGamepad && GamepadKeycode(keyMask) != 280 && GamepadKeycode(keyMask) != 281) ||
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

    if (gameUI == NULL || controlMap == NULL || (gameUI && gameUI->GameIsPaused()) ||
        (controlMap && !controlMap->IsFightingControlsEnabled())) {
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

        if (isBlocking) {
            return;
        }

        if (shouldAttack || (timeDiff == 0 && isLeft)) {
            if (timeDiff == 0) {
                logger::info("Nice reflex!");
            }

            SetIsAttackIndicated(isLeft, false);

            auto isDualWielding = IsDualWielding();
            auto isDualHeld = isLeft ? tempIsRightDualHeld : tempIsLeftDualHeld;

            auto isPowerAttack =
                IsPowerAttack(Max(tempLeftHoldTime, tempRightHoldTime), leftAltBehavior || rightAltBehavior);
            auto attackAction = GetAttackAction(isLeft, timeDiff, isDualWielding, isDualHeld, false);

            PerformAction(attackAction, playerCharacter, false);

            if (isPowerAttack) {
                attackAction = GetAttackAction(isLeft, timeDiff, isDualWielding, isDualHeld, true);

                PerformAction(attackAction, playerCharacter, true);
            }
        }
    }

    void TryIndicatePowerAttack(bool isLeft, PlayerCharacter* player) {
        bool altHandBehavior = isLeft ? rightAltBehavior : leftAltBehavior;
        float holdTime = isLeft ? leftHoldTime : rightHoldTime;

        
        if (!isAttacking && !altHandBehavior && holdTime > POWER_ATTACK_MIN_HOLD_TIME - POWER_ATTACK_SOUND_OFFSET) {
            if (isLeftAttackIndicated || isRightAttackIndicated) {
                return;
            }

            SetIsAttackIndicated(isLeft, true);

            PlayDebugSound(powerAttackSound, player);
            
            std::thread thread([]() {
                long timeSpan = POWER_ATTACK_SOUND_OFFSET * 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(timeSpan));
                Vibrate(VibrationType::kSmooth, 0.22f, 0.24f);
            });
            thread.detach();
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

class HookAnimGraphEvent {
public:
    typedef BSEventNotifyControl (HookAnimGraphEvent::*FnReceiveEvent)(
        BSAnimationGraphEvent* evn, BSTEventSource<BSAnimationGraphEvent>* dispatcher);

    BSEventNotifyControl ReceiveEventHook(BSAnimationGraphEvent* evn, BSTEventSource<BSAnimationGraphEvent>* src) {
        Actor* a = stl::unrestricted_cast<Actor*>(evn->holder);
        if (a) {
            if (a->AsActorState()->GetSitSleepState() == SIT_SLEEP_STATE::kNormal && !a->IsInKillMove()) {
                ATTACK_STATE_ENUM currentState = (a->AsActorState()->actorState1.meleeAttackState);
                if (currentState >= ATTACK_STATE_ENUM::kDraw && currentState <= ATTACK_STATE_ENUM::kBash) {
                    isAttacking = true;
                } else {
                    isAttacking = false;
                }
            } else {
                isAttacking = false;
            }
        }

        FnReceiveEvent fn = fnHash.at(*(uintptr_t*)this);
        return fn ? (this->*fn)(evn, src) : BSEventNotifyControl::kContinue;
    }

    static void Hook() {
        REL::Relocation<uintptr_t> vtable{VTABLE_PlayerCharacter[2]};
        FnReceiveEvent fn =
            stl::unrestricted_cast<FnReceiveEvent>(vtable.write_vfunc(1, &HookAnimGraphEvent::ReceiveEventHook));
        fnHash.insert(std::pair<uintptr_t, FnReceiveEvent>(vtable.address(), fn));
    }

private:
    static std::unordered_map<uintptr_t, FnReceiveEvent> fnHash;
};
std::unordered_map<uintptr_t, HookAnimGraphEvent::FnReceiveEvent> HookAnimGraphEvent::fnHash;

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        actionRightAttack = (BGSAction*)TESForm::LookupByID(0x13005);
        actionLeftAttack = (BGSAction*)TESForm::LookupByID(0x13004);
        actionDualAttack = (BGSAction*)TESForm::LookupByID(0x50c96);
        actionRightPowerAttack = (BGSAction*)TESForm::LookupByID(0x13383);
        actionLeftPowerAttack = (BGSAction*)TESForm::LookupByID(0x2e2f6);
        actionDualPowerAttack = (BGSAction*)TESForm::LookupByID(0x2e2f7);
        powerAttackSound = (BGSSoundDescriptorForm*)TESForm::LookupByID(0x7c71f);  // 3c72e

        audioManager = BSAudioManager::GetSingleton();

        HookAttackBlockHandler::Hook();
        HookAnimGraphEvent::Hook();
    }
}

SKSEPluginLoad(const LoadInterface* skse) {
    Init(skse);

    SetupLog();
    logger::info("Setup log...");

    // LoadSettings();
    // logger::info("Settings loaded...");

    if (!isEnabled) {
        logger::info("Mod is disabled...");
    }

    tasks = GetTaskInterface();
    GetMessagingInterface()->RegisterListener(OnMessage);

    return true;
}