#include <SimpleIni.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;
using namespace RE;
using namespace RE::BSScript;
using namespace SKSE;
using namespace SKSE::stl;

const bool isEnabled = true;

const uint64_t DUAL_ATTACK_TIME_DIFF = 120;
const float POWER_ATTACK_MIN_HOLD_TIME = 0.33f;

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

float leftHoldTime = 0.0f;
float rightHoldTime = 0.0f;

uint64_t leftLastTime = 0;
uint64_t rightLastTime = 0;

bool isLeftDualHeld = false;
bool isRightDualHeld = false;

bool leftAltBehavior = false;
bool rightAltBehavior = false;

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

std::string GetAttackAction(bool isLeft, uint64_t timeDiff, bool isDualWielding, bool isDualHeld, bool isPowerAttack) {
    if (isDualWielding && isDualHeld && timeDiff < DUAL_ATTACK_TIME_DIFF) {
        return isPowerAttack ? bothPowerHands : bothHands;
    }

    if (isLeft) {
        return isPowerAttack ? leftPowerHand : leftHand;
    }

    return isPowerAttack ? rightPowerHand : rightHand;
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

        logger::info("Hooked Attack Blocker OK...");
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

            auto isDualWielding = IsDualWielding();
            auto isDualHeld = isLeft ? tempIsRightDualHeld : tempIsLeftDualHeld;

            auto isPowerAttack =
                IsPowerAttack(Max(tempLeftHoldTime, tempRightHoldTime), leftAltBehavior || rightAltBehavior);
            auto attackDirection = GetAttackAction(isLeft, timeDiff, isDualWielding, isDualHeld, false);

            //logger::info("Left last time: {0}", leftLastTime);
            //logger::info("Right last time: {0}", rightLastTime);

            RunConsoleCommand(attackDirection);

            if (isPowerAttack) {
                attackDirection = GetAttackAction(isLeft, timeDiff, isDualWielding, isDualHeld, true);

                RunConsoleCommand(attackDirection);
            }
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

        HookAttackBlockHandler::Hook();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);

    SetupLog();
    logger::info("Setup log...");

    // LoadSettings();
    // logger::info("Settings loaded...");

    if (!isEnabled) {
        logger::info("Mod is disabled...");
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    return true;
}