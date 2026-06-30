#include "log.h"
#include "PrismaUI_API.h"
#include <nlohmann/json.hpp>
#include <SimpleIni.h>
#include <unordered_set>

using JSON = nlohmann::json;

// ====================== GLOBALS ======================

PRISMA_UI_API::IVPrismaUI1* PrismaUI = nullptr;
static PrismaView view;

// ---- Settings ----

std::uint32_t                    g_rollThreshold = 50;
std::uint32_t                    g_openCardsScanCode1 = 0x2D;
std::uint32_t                    g_openCardsScanCode2 = 0x1000;
std::unordered_set<std::string>  g_excludedMods;
std::unordered_set<std::string>  g_includedMods;

// ---- Spell Data ----

struct SpellData
{
    std::string    name;
    std::uint32_t  school;
    std::uint32_t  minSkill;
    std::string    sourceMod;
    std::string    description;
    RE::SpellItem* form;
};

std::vector<SpellData> g_spells;

// ---- Spell Cast Tracking ----

std::unordered_map<std::uint32_t, std::uint32_t> g_spellCastCountByTier;
std::mutex g_spellCountMutex;

// ---- Pending Card State ----

std::uint32_t                 g_pendingTier = UINT32_MAX;
std::vector<const SpellData*> g_pendingSpells;

// ---- UI State ----

bool g_isMenuOpen = false;

// ====================== SERIALIZATION ======================

constexpr std::uint32_t kPraxisSerializationType = 'PRAX';
constexpr std::uint32_t kPraxisSerializationVersion = 1;

// ====================== UTILITIES ======================

std::string GetTranslation(const std::string& key)
{
    std::string result;
    SKSE::Translation::Translate(key, result);
    return result;
}

void PlaySound(const char* soundName)
{
    auto* audioManager = RE::BSAudioManager::GetSingleton();
    if (!audioManager) return;

    RE::BSSoundHandle handle;
    audioManager->BuildSoundDataFromEditorID(handle, soundName, 0);
    if (handle.IsValid())
        handle.Play();
}

bool IsVanillaMenuOpen()
{
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return true;

    return ui->GameIsPaused()
        || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)
        || ui->IsMenuOpen(RE::Console::MENU_NAME)
        || ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)
        || ui->IsMenuOpen(RE::MagicMenu::MENU_NAME)
        || ui->IsMenuOpen(RE::MapMenu::MENU_NAME)
        || ui->IsMenuOpen(RE::JournalMenu::MENU_NAME)
        || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)
        || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
}

// ====================== UI ======================

void HideCards()
{
    if (PrismaUI->IsValid(view))
        PrismaUI->Hide(view);
    g_isMenuOpen = false;
}

JSON BuildSpellsArray(const std::vector<const SpellData*>& spells)
{
    JSON arr = JSON::array();
    for (const auto* spell : spells) {
        arr.push_back({
            {"name",        spell->name},
            {"school",      spell->school},
            {"minSkill",    spell->minSkill},
            {"sourceMod",   spell->sourceMod},
            {"description", spell->description}
            });
    }
    return arr;
}

void ShowCards(const std::vector<const SpellData*>& spells)
{
    PrismaUI->Show(view);
    PrismaUI->Focus(view, true);

    std::string script = "updateCards(" + BuildSpellsArray(spells).dump() + ")";
    PrismaUI->Invoke(view, script.c_str());

    g_isMenuOpen = true;
}

JSON BuildTranslationsJSON()
{
    return {
        {"title", GetTranslation("$PraxisTitle")},
        {"school_info", {
            {"18", {{"name", GetTranslation("$PraxisAlteration")},  {"icon", "alteration.png"}}},
            {"19", {{"name", GetTranslation("$PraxisConjuration")}, {"icon", "conjuration.png"}}},
            {"20", {{"name", GetTranslation("$PraxisDestruction")}, {"icon", "destruction.png"}}},
            {"21", {{"name", GetTranslation("$PraxisIllusion")},    {"icon", "illusion.png"}}},
            {"22", {{"name", GetTranslation("$PraxisRestoration")}, {"icon", "restoration.png"}}}
        }},
        {"tier_names", {
            {"0",   GetTranslation("$PraxisNovice")},
            {"25",  GetTranslation("$PraxisApprentice")},
            {"50",  GetTranslation("$PraxisAdept")},
            {"75",  GetTranslation("$PraxisExpert")},
            {"100", GetTranslation("$PraxisMaster")}
        }}
    };
}

void OnViewReady(PrismaView view)
{
    SKSE::log::info("View DOM is ready {}", view);

    std::string script = "updateTranslations(" + BuildTranslationsJSON().dump() + ")";
    PrismaUI->Invoke(view, script.c_str());

    PrismaUI->Hide(view);
}

// ====================== SPELL SCANNING ======================

std::unordered_set<RE::SpellItem*> CollectLearnableSpells(RE::TESDataHandler* dataHandler)
{
    std::unordered_set<RE::SpellItem*> learnableSpells;
    for (auto* book : dataHandler->GetFormArray<RE::TESObjectBOOK>()) {
        if (book && book->TeachesSpell()) {
            if (auto* spell = book->GetSpell())
                learnableSpells.insert(spell);
        }
    }
    SKSE::log::info("Found {} spell tomes", learnableSpells.size());
    return learnableSpells;
}

std::string BuildSpellDescription(RE::SpellItem* spell)
{
    RE::BSString buf;
    spell->GetDescription(buf, spell);
    if (buf.c_str() && buf.size() > 0)
        return buf.c_str();

    std::string description;
    for (auto* effect : spell->effects) {
        if (!effect || !effect->baseEffect) continue;
        std::string effectName = effect->baseEffect->GetName();
        if (effectName.empty()) continue;
        if (!description.empty()) description += ", ";
        description += effectName;
    }
    return description;
}

void ScanAndRegisterSpells(RE::TESDataHandler* dataHandler,
    const std::unordered_set<RE::SpellItem*>& learnableSpells)
{
    constexpr std::array<std::uint32_t, 5> validSchools = { 18, 19, 20, 21, 22 };
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t>> seen;
    std::uint32_t count = 0;

    for (auto* spell : dataHandler->GetFormArray<RE::SpellItem>()) {
        if (!spell) continue;
        if (spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell &&
        spell->GetSpellType() != RE::MagicSystem::SpellType::kLesserPower &&
        spell->GetSpellType() != RE::MagicSystem::SpellType::kVoicePower)
        {
			continue;
        }
        if (spell->data.costOverride == 0) continue;

        const char* sourceMod = "unknown";
        if (auto* file = spell->GetFile(0))
            sourceMod = file->fileName;

        bool forceInclude = false;
        if (sourceMod && g_includedMods.size() > 0 && g_includedMods.count(std::string(sourceMod)))
        {
            forceInclude = true;
        }

        if (!learnableSpells.count(spell) && !forceInclude) continue;

        auto school = static_cast<std::uint32_t>(spell->GetAssociatedSkill());
        if (!std::count(validSchools.begin(), validSchools.end(), school) && !forceInclude)
        {
            continue;
        }

        std::uint32_t minSkill = 75;
		auto* effect = spell->GetCostliestEffectItem();
        if (effect && effect->baseEffect)
            minSkill = static_cast<std::uint32_t>(effect->baseEffect->data.minimumSkill);

        std::string name = spell->GetName();
        if (name.empty()) continue;

        if (!seen.insert({ name, school, minSkill }).second) continue;

        if (sourceMod && g_excludedMods.count(std::string(sourceMod))) continue;

        std::string description = BuildSpellDescription(spell);

        SKSE::log::info("  [{}] '{}' | school={} | minSkill={} | mod={}", count, name, school, minSkill, sourceMod);
        g_spells.push_back({ name, school, minSkill, sourceMod, description, spell });
        ++count;
    }

    SKSE::log::info("Scan complete. {} spells found.", count);
}

// ====================== SPELL ROLL ======================

void FillPendingSpells(std::unordered_map<std::uint32_t,
    std::vector<const SpellData*>>&unlearnedBySchool,
    std::mt19937& rng)
{
    constexpr std::array<std::uint32_t, 5> schoolOrder = { 20, 18, 22, 19, 21 };

    for (auto schoolId : schoolOrder) {
        auto& pool = unlearnedBySchool[schoolId];
        if (pool.empty()) continue;

        std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
        std::size_t idx = dist(rng);
        g_pendingSpells.push_back(pool[idx]);
        pool.erase(pool.begin() + idx);
    }

    if (g_pendingSpells.size() < 5) {
        std::vector<const SpellData*> backup;
        for (auto schoolId : schoolOrder) {
            auto& pool = unlearnedBySchool[schoolId];
            backup.insert(backup.end(), pool.begin(), pool.end());
        }

        while (g_pendingSpells.size() < 5 && !backup.empty()) {
            std::uniform_int_distribution<std::size_t> dist(0, backup.size() - 1);
            std::size_t idx = dist(rng);
            g_pendingSpells.push_back(backup[idx]);
            backup.erase(backup.begin() + idx);
        }
    }
}

bool TryRollTier(std::uint32_t tier, RE::PlayerCharacter* player, std::mt19937& rng)
{
    std::unordered_map<std::uint32_t, std::vector<const SpellData*>> unlearnedBySchool;
    std::size_t totalUnlearned = 0;

    for (const auto& entry : g_spells) {
        if (entry.minSkill != tier) continue;
        if (player->HasSpell(entry.form)) continue;
        unlearnedBySchool[entry.school].push_back(&entry);
        totalUnlearned++;
    }

    if (totalUnlearned == 0) {
        SKSE::log::info("Tier {} fully learned. Escalating...", tier);
        return false;
    }

    g_pendingTier = tier;
    FillPendingSpells(unlearnedBySchool, rng);
    return true;
}

void OnSpellCastThresholdReached(std::uint32_t minSkill, RE::PlayerCharacter* player)
{
    constexpr std::array<std::uint32_t, 5> tierOrder = { 0, 25, 50, 75, 100 };
    static std::mt19937 rng(std::random_device{}());

    g_pendingSpells.clear();

    auto tierIt = std::find(tierOrder.begin(), tierOrder.end(), minSkill);
    if (tierIt == tierOrder.end())
        tierIt = tierOrder.begin();

    bool foundValidRoll = false;
    for (; tierIt != tierOrder.end(); ++tierIt) {
        if (TryRollTier(*tierIt, player, rng)) {
            foundValidRoll = true;
            break;
        }
    }

    if (foundValidRoll) {
        RE::DebugNotification(GetTranslation("$PraxisPressChosenKey").c_str());
    }
    else {
        g_pendingTier = UINT32_MAX;
        SKSE::log::info("Player has learned every available spell.");
    }
}

// ====================== CARD SELECTION ======================

void OnCardChosen(const char* spellName)
{
    if (!spellName) return;

    std::string targetName(spellName);
    auto it = std::find_if(g_spells.begin(), g_spells.end(), [&](const SpellData& s) {
        return s.name == targetName;
        });

    if (it == g_spells.end()) {
        SKSE::log::warn("Spell not found in g_spells: {}", targetName);
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player && it->form) {
        player->AddSpell(it->form);
        PlaySound("UISkillIncreaseSD");
        HideCards();
        RE::DebugNotification(GetTranslation("$PraxisSpellLearned").c_str());
        g_pendingSpells.clear();
    }
}

// ====================== INPUT HANDLER ======================

class InputEventHandler : public RE::BSTEventSink<RE::InputEvent*>
{
public:
    static InputEventHandler* GetSingleton()
    {
        static InputEventHandler instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(
        RE::InputEvent* const* a_event,
        RE::BSTEventSource<RE::InputEvent*>*) override
    {
        if (!a_event)
            return RE::BSEventNotifyControl::kContinue;

        for (auto* event = *a_event; event; event = event->next) {
            auto* buttonEvent = event->AsButtonEvent();
            if (!buttonEvent || !buttonEvent->IsDown()) continue;

            std::uint32_t idCode = buttonEvent->GetIDCode();
            RE::INPUT_DEVICE device = buttonEvent->GetDevice();

            if (HandlePendingSpellInput(idCode)) break;
            HandleGamepadMenuInput(device, idCode);
        }

        return RE::BSEventNotifyControl::kContinue;
    }

private:
    InputEventHandler() = default;

    bool HandlePendingSpellInput(std::uint32_t idCode)
    {
        if (g_pendingTier == UINT32_MAX) return false;
        if (IsVanillaMenuOpen()) return false;

        if (idCode == g_openCardsScanCode1 || idCode == g_openCardsScanCode2) {
            ShowCards(g_pendingSpells);
            PlaySound("UILevelUpSD");
            g_pendingTier = UINT32_MAX;
            return true;
        }
        return false;
    }

    void HandleGamepadMenuInput(RE::INPUT_DEVICE device, std::uint32_t idCode)
    {
        if (device != RE::INPUT_DEVICE::kGamepad || !g_isMenuOpen) return;

        if (idCode == 0x0004) PrismaUI->Invoke(view, "navigateCards(-1)");
        else if (idCode == 0x0008) PrismaUI->Invoke(view, "navigateCards(1)");
        else if (idCode == 0x1000) PrismaUI->Invoke(view, "selectFocusedCard()");
    }
};

// ====================== SPELL CAST HANDLER ======================

class SpellCastEventHandler : public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
    static SpellCastEventHandler* GetSingleton()
    {
        static SpellCastEventHandler instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESSpellCastEvent* a_event,
        RE::BSTEventSource<RE::TESSpellCastEvent>*) override
    {
        if (!a_event || !a_event->object)
            return RE::BSEventNotifyControl::kContinue;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (a_event->object.get() != player)
            return RE::BSEventNotifyControl::kContinue;

        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell);
        if (!spell) return RE::BSEventNotifyControl::kContinue;

        if (!IsValidMagicSchool(spell)) return RE::BSEventNotifyControl::kContinue;
        if (g_pendingTier != UINT32_MAX) return RE::BSEventNotifyControl::kContinue;

        std::uint32_t minSkill = GetMinSkill(spell);
        std::lock_guard lock(g_spellCountMutex);
        auto& count = g_spellCastCountByTier[minSkill];
        if (++count >= g_rollThreshold) {
            count = 0;
            OnSpellCastThresholdReached(minSkill, player);
        }

        if (spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
            std::thread([spell, minSkill, player]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));

                    // Check if still casting on either hand
                    auto* casterL = player->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
                    auto* casterR = player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
                    bool stillCasting =
                        (casterL && casterL->currentSpell == spell && casterL->state == RE::MagicCaster::State::kCasting) ||
                        (casterR && casterR->currentSpell == spell && casterR->state == RE::MagicCaster::State::kCasting);

                    if (!stillCasting || g_pendingTier != UINT32_MAX) break;

                    std::lock_guard lock(g_spellCountMutex);
                    auto& count = g_spellCastCountByTier[minSkill];
                    if (++count >= g_rollThreshold) {
                        count = 0;
                        SKSE::GetTaskInterface()->AddTask([minSkill, player]() {
                            OnSpellCastThresholdReached(minSkill, player);
                            });
                        break;
                    }
                }
            }).detach();
        }

        return RE::BSEventNotifyControl::kContinue;
    }

private:
    SpellCastEventHandler() = default;

    static bool IsValidMagicSchool(RE::SpellItem* spell)
    {
        constexpr std::array<std::uint32_t, 5> validSchools = { 18, 19, 20, 21, 22 };
        auto school = static_cast<std::uint32_t>(spell->GetAssociatedSkill());
        return std::count(validSchools.begin(), validSchools.end(), school) > 0;
    }

    static std::uint32_t GetMinSkill(RE::SpellItem* spell)
    {
        if (auto* effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
            return static_cast<std::uint32_t>(effect->baseEffect->data.minimumSkill);
        return 0;
    }
};

// ====================== SERIALIZATION ======================

void PraxisSaveCallback(SKSE::SerializationInterface* a_intfc)
{
    if (!a_intfc->OpenRecord(kPraxisSerializationType, kPraxisSerializationVersion)) {
        SKSE::log::error("Failed to open Praxis save record");
        return;
    }

    std::uint32_t size = static_cast<std::uint32_t>(g_spellCastCountByTier.size());
    if (!a_intfc->WriteRecordData(&size, sizeof(size))) {
        SKSE::log::error("Failed to write map size to save game.");
        return;
    }

    for (const auto& [tier, count] : g_spellCastCountByTier) {
        a_intfc->WriteRecordData(&tier, sizeof(tier));
        a_intfc->WriteRecordData(&count, sizeof(count));
    }
}

void PraxisLoadCallback(SKSE::SerializationInterface* a_intfc)
{
    g_spellCastCountByTier.clear();
    g_pendingSpells.clear();
    g_pendingTier = UINT32_MAX;

    std::uint32_t type, version, length;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kPraxisSerializationType) continue;

        if (version != kPraxisSerializationVersion) {
            SKSE::log::warn("Outdated save version for Praxis. Skipping.");
            continue;
        }

        std::uint32_t size = 0;
        if (!a_intfc->ReadRecordData(&size, sizeof(size))) {
            SKSE::log::error("Failed to read map size block.");
            return;
        }

        for (std::uint32_t i = 0; i < size; ++i) {
            std::uint32_t tier = 0, count = 0;
            a_intfc->ReadRecordData(&tier, sizeof(tier));
            a_intfc->ReadRecordData(&count, sizeof(count));
            g_spellCastCountByTier[tier] = count;
        }
    }
}

void PraxisRevertCallback(SKSE::SerializationInterface*)
{
    g_spellCastCountByTier.clear();
    g_pendingSpells.clear();
    g_pendingTier = UINT32_MAX;
    HideCards();
}

// ====================== INIT ======================

void OnDataLoaded()
{
    SKSE::Translation::ParseTranslation("Praxis");

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        SKSE::log::error("TESDataHandler not available!");
        return;
    }

    PrismaUI = static_cast<PRISMA_UI_API::IVPrismaUI1*>(
        PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));

    if (!PrismaUI) {
        SKSE::log::error("Failed to initialize PrismaUI API");
        return;
    }

    SKSE::log::info("PrismaUI API initialized successfully");

    view = PrismaUI->CreateView("Praxis/index.html", OnViewReady);

    auto learnableSpells = CollectLearnableSpells(dataHandler);
    ScanAndRegisterSpells(dataHandler, learnableSpells);

    PrismaUI->RegisterJSListener(view, "choseCard", OnCardChosen);

    RE::BSInputDeviceManager::GetSingleton()->AddEventSink(InputEventHandler::GetSingleton());
    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(SpellCastEventHandler::GetSingleton());
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    if (a_msg->type == SKSE::MessagingInterface::kDataLoaded)
        OnDataLoaded();
}

void LoadSettings()
{
    CSimpleIniA ini;
    ini.SetUnicode();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const std::string path = "Data/SKSE/Plugins/" + std::string(plugin->GetName()) + ".ini";
    ini.LoadFile(path.c_str());

    g_rollThreshold = static_cast<std::uint32_t>(ini.GetDoubleValue("General", "iRollThreshold", 50));
    g_openCardsScanCode1 = static_cast<std::uint32_t>(ini.GetDoubleValue("General", "iOpenCardsScanCode1", 0x2D));
    g_openCardsScanCode2 = static_cast<std::uint32_t>(ini.GetDoubleValue("General", "iOpenCardsScanCode2", 0x1000));

    const std::string excludedModsStr = ini.GetValue("General", "sExcludeMods", "");
    if (!excludedModsStr.empty()) {
        std::stringstream ss(excludedModsStr);
        std::string modName;
        while (std::getline(ss, modName, ',')) {
            auto start = std::find_if(modName.begin(), modName.end(), [](unsigned char c) { return !std::isspace(c); });
            auto end = std::find_if(modName.rbegin(), modName.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
            if (start < end)
                g_excludedMods.insert(std::string(start, end));
        }
    }

    const std::string includedModsStr = ini.GetValue("General", "sIncludeMods", "");
    if (!includedModsStr.empty()) {
        std::stringstream ss(includedModsStr);
        std::string modName;
        while (std::getline(ss, modName, ',')) {
            auto start = std::find_if(modName.begin(), modName.end(), [](unsigned char c) { return !std::isspace(c); });
            auto end = std::find_if(modName.rbegin(), modName.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
            if (start < end)
                g_includedMods.insert(std::string(start, end));
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    LoadSettings();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", MessageHandler))
        return false;

    auto* serialization = SKSE::GetSerializationInterface();
    if (serialization) {
        serialization->SetUniqueID(kPraxisSerializationType);
        serialization->SetSaveCallback(PraxisSaveCallback);
        serialization->SetLoadCallback(PraxisLoadCallback);
        serialization->SetRevertCallback(PraxisRevertCallback);
        SKSE::log::info("Praxis serialization registered");
    }

    return true;
}