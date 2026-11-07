#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace autoplay {

enum class Mode : uint8_t { Cube = 0, Ship, Ball, UFO, Wave, Robot, Spider, Swing, Unknown };
constexpr size_t kModeCount = 8;
constexpr size_t kMaxEvents = 100000;

struct InputEvent {
    int tick = 0;
    int button = 1;
    bool down = false;
    bool isPlayer1 = true;
    Mode mode = Mode::Cube;
};

struct SavedRoute {
    fs::path path;
    int levelID = 0;
    std::string levelName;
    std::string name;
    size_t eventCount = 0;
};

struct ModeStats {
    size_t presses = 0;
    size_t holds = 0;
    int64_t totalHoldTicks = 0;
    int maxHoldTicks = 0;
};

inline char const* modeName(Mode mode) {
    switch (mode) {
        case Mode::Cube: return "Cube";
        case Mode::Ship: return "Ship";
        case Mode::Ball: return "Ball";
        case Mode::UFO: return "UFO";
        case Mode::Wave: return "Wave";
        case Mode::Robot: return "Robot";
        case Mode::Spider: return "Spider";
        case Mode::Swing: return "Swing";
        default: return "Unknown";
    }
}

inline Mode currentMode(PlayerObject* player) {
    if (!player) return Mode::Unknown;
    if (player->m_isDart) return Mode::Wave;
    if (player->m_isSwing) return Mode::Swing;
    if (player->m_isShip) return Mode::Ship;
    if (player->m_isBird) return Mode::UFO;
    if (player->m_isBall) return Mode::Ball;
    if (player->m_isRobot) return Mode::Robot;
    if (player->m_isSpider) return Mode::Spider;
    return Mode::Cube;
}

inline std::string cleanName(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (value.size() > 48) value.resize(48);
    return value;
}

inline std::string fileSlug(std::string const& value) {
    std::string result;
    for (unsigned char c : value) {
        if (std::isalnum(c)) result.push_back(static_cast<char>(c));
        else if ((c == ' ' || c == '-' || c == '_') && !result.empty() && result.back() != '-') result.push_back('-');
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.empty()) result = "Save";
    return result;
}

class Controller {
public:
    static Controller& get() {
        static Controller instance;
        return instance;
    }

    bool isRecording() const { return m_recording; }
    bool isPlaying() const { return m_playing; }
    bool isInjecting() const { return m_injecting; }

    fs::path routesDir() const { return Mod::get()->getSaveDir() / "routes"; }
    fs::path scansDir() const { return Mod::get()->getSaveDir() / "level-scans"; }

    void requestRecording(std::string name) {
        m_pendingName = cleanName(std::move(name));
        if (m_pendingName.empty()) m_pendingName = "My Route";
        m_events.clear();
        m_pendingRecord = true;
        m_pendingPlayback = false;
        m_playing = false;
        m_recording = false;
        m_restartRequested = true;
    }

    bool requestPlayback(PlayLayer* layer, fs::path const& path) {
        if (!layer || !layer->m_level) return false;

        SavedRoute info;
        std::vector<InputEvent> loaded;
        if (!readRoute(path, info, loaded)) {
            Notification::create("This save is damaged or cannot be read.", NotificationIcon::Error)->show();
            return false;
        }

        auto levelID = layer->m_level->m_levelID;
        auto levelName = std::string(layer->m_level->m_levelName);
        bool sameLevel = info.levelID == levelID;
        if (levelID == 0) sameLevel = info.levelName == levelName;
        if (!sameLevel || info.levelName != levelName) {
            Notification::create(
                fmt::format("That save belongs to '{}'. Open the same level first.", info.levelName),
                NotificationIcon::Warning
            )->show();
            return false;
        }

        m_events = std::move(loaded);
        m_levelID = info.levelID;
        m_levelName = info.levelName;
        m_pendingName = info.name;
        m_pendingPlayback = true;
        m_pendingRecord = false;
        m_recording = false;
        m_playing = false;
        m_restartRequested = true;
        return true;
    }

    std::vector<SavedRoute> listRoutes(int levelID, std::string const& levelName) const {
        std::vector<SavedRoute> result;
        std::error_code ec;
        fs::create_directories(routesDir(), ec);
        ec.clear();
        for (fs::directory_iterator it(routesDir(), ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".apl") continue;
            SavedRoute info;
            std::vector<InputEvent> ignored;
            if (!readRoute(it->path(), info, ignored)) continue;
            bool sameLevel = info.levelID == levelID;
            if (levelID == 0) sameLevel = info.levelName == levelName;
            if (sameLevel && info.levelName == levelName) {
                info.path = it->path();
                result.push_back(std::move(info));
            }
        }
        std::sort(result.begin(), result.end(), [](SavedRoute const& a, SavedRoute const& b) {
            return a.name < b.name;
        });
        return result;
    }

    bool analyzeEditor(std::string name, std::string& report) {
        auto* editor = LevelEditorLayer::get();
        if (!editor || !editor->m_level) return false;

        name = cleanName(std::move(name));
        if (name.empty()) name = "Level Scan";

        auto* objects = editor->getAllObjects();
        if (!objects) return false;

        size_t objectCount = static_cast<size_t>(objects->count());
        size_t effectCount = 0;
        float minX = 0.f;
        float maxX = 0.f;
        bool first = true;
        std::vector<std::array<int64_t, 4>> signature;
        signature.reserve(objectCount);

        for (auto object : geode::cocos::CCArrayExt<GameObject, false>(objects)) {
            if (!object) continue;
            auto pos = object->getPosition();
            if (first) {
                minX = maxX = pos.x;
                first = false;
            } else {
                minX = std::min(minX, pos.x);
                maxX = std::max(maxX, pos.x);
            }
            if (object->m_classType == GameObjectClassType::Effect) ++effectCount;
            signature.push_back({
                static_cast<int64_t>(object->m_objectID),
                static_cast<int64_t>(std::llround(pos.x * 10.f)),
                static_cast<int64_t>(std::llround(pos.y * 10.f)),
                static_cast<int64_t>(std::llround(object->getRotation() * 10.f))
            });
        }

        std::sort(signature.begin(), signature.end());
        uint64_t hash = 1469598103934665603ull;
        for (auto const& item : signature) {
            for (auto value : item) {
                hash ^= static_cast<uint64_t>(value);
                hash *= 1099511628211ull;
            }
        }

        int levelID = editor->m_level->m_levelID;
        std::string levelName = editor->m_level->m_levelName;
        std::error_code ec;
        fs::create_directories(scansDir(), ec);
        if (ec) return false;

        auto base = fileSlug(name) + "-" + std::to_string(levelID);
        fs::path path = scansDir() / (base + ".scan");
        for (int suffix = 2; fs::exists(path, ec); ++suffix) {
            path = scansDir() / (base + "-" + std::to_string(suffix) + ".scan");
            ec.clear();
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "AUTOPLAY_LEVEL_SCAN_V1\n"
            << levelID << '\n'
            << std::quoted(levelName) << '\n'
            << std::quoted(name) << '\n'
            << objectCount << ' ' << effectCount << ' '
            << std::fixed << std::setprecision(1) << minX << ' ' << maxX << '\n'
            << std::hex << hash << '\n';
        out.flush();
        if (!out) return false;

        report = fmt::format(
            "Scan saved as '{}'.\nObjects: {}\nEffect/trigger objects: {}\nHorizontal object span: {:.1f}\nLevel fingerprint: {:016x}",
            name, objectCount, effectCount, std::max(0.f, maxX - minX), hash
        );
        return true;
    }

    void beginPendingAction(PlayLayer* layer) {
        if (!layer || layer->m_isPaused) return;

        if (m_pendingRecord) {
            m_pendingRecord = false;
            m_restartRequested = false;
            beginRecording(layer);
        } else if (m_pendingPlayback) {
            m_pendingPlayback = false;
            m_restartRequested = false;
            beginPlayback(layer);
        }
    }

    void beginRecording(PlayLayer* layer) {
        if (!layer || !layer->m_level) return;
        m_events.clear();
        m_cursor = 0;
        m_levelID = layer->m_level->m_levelID;
        m_levelName = layer->m_level->m_levelName;
        m_recordOrigin = static_cast<int>(layer->m_gameState.m_commandIndex);
        m_playOrigin = 0;
        m_recording = true;
        m_playing = false;
        Notification::create(
            fmt::format("Recording '{}'. Play the level normally.", m_pendingName),
            NotificationIcon::Info
        )->show();
    }

    void capture(PlayLayer* layer, bool down, int button, bool isPlayer1) {
        if (!m_recording || !layer || button < 1 || button > 3 || m_events.size() >= kMaxEvents) return;
        int tick = static_cast<int>(layer->m_gameState.m_commandIndex) - m_recordOrigin;
        if (tick < 0) tick = 0;
        PlayerObject* player = isPlayer1 ? layer->m_player1 : layer->m_player2;
        m_events.push_back(InputEvent{tick, button, down, isPlayer1, currentMode(player)});
    }

    void finishRecording(PlayLayer* layer, char const* reason) {
        if (!m_recording) return;
        m_recording = false;
        if (m_events.empty()) {
            Notification::create("AutoPlay: no input was captured. Try recording again.", NotificationIcon::Warning)->show();
            return;
        }

        if (saveRoute(layer)) {
            FLAlertLayer::create(
                "AutoPlay Learner",
                fmt::format(
                    "Saved route: <cy>{}</c>\nEnd reason: {}\n\n{}",
                    m_pendingName, reason ? reason : "stopped", buildProfile()
                ),
                "OK"
            )->show();
        } else {
            Notification::create("AutoPlay: could not save this named route.", NotificationIcon::Error)->show();
        }
    }

    void beginPlayback(PlayLayer* layer) {
        if (!layer || m_events.empty()) return;
        m_cursor = 0;
        m_playOrigin = static_cast<int>(layer->m_gameState.m_commandIndex);
        m_playing = true;
        m_recording = false;
        Notification::create(
            fmt::format("Playing saved route '{}'.", m_pendingName),
            NotificationIcon::Info
        )->show();
    }

    void dispatch(GJBaseGameLayer* base) {
        if (!m_playing || !base) return;
        auto* layer = PlayLayer::get();
        if (!layer || static_cast<GJBaseGameLayer*>(layer) != base) return;

        int now = static_cast<int>(base->m_gameState.m_commandIndex) - m_playOrigin;
        while (m_cursor < m_events.size() && m_events[m_cursor].tick <= now) {
            auto const event = m_events[m_cursor++];
            m_injecting = true;
            base->handleButton(event.down, event.button, event.isPlayer1);
            m_injecting = false;
        }

        if (m_cursor >= m_events.size()) {
            m_playing = false;
            Notification::create("AutoPlay: saved route finished.", NotificationIcon::Success)->show();
        }
    }

    void stop(PlayLayer* layer) {
        m_pendingRecord = false;
        m_pendingPlayback = false;
        m_restartRequested = false;
        if (m_recording) finishRecording(layer, "manual stop");
        m_playing = false;
        m_injecting = false;
    }

    void onDeath(PlayLayer* layer) {
        if (m_recording) finishRecording(layer, "player died");
        m_playing = false;
    }

    void onComplete(PlayLayer* layer) {
        if (m_recording) finishRecording(layer, "level complete");
        m_playing = false;
    }

    void onQuit(PlayLayer* layer) {
        // resumeAndRestart tears down the old PlayLayer. Keep the queued action
        // so it can begin on the newly-created PlayLayer.
        if (m_restartRequested) return;
        if (m_recording) finishRecording(layer, "level exited");
        m_playing = false;
        m_pendingPlayback = false;
        m_pendingRecord = false;
    }

private:
    Controller() = default;

    bool readRoute(fs::path const& path, SavedRoute& info, std::vector<InputEvent>& events) const {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string magic;
        std::getline(in, magic);
        if (magic != "AUTOPLAY_ROUTE_V2") return false;

        size_t count = 0;
        if (!(in >> info.levelID >> std::quoted(info.levelName) >> std::quoted(info.name) >> count)) return false;
        if (count == 0 || count > kMaxEvents) return false;

        events.clear();
        events.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            InputEvent event;
            int down = 0, player1 = 1, mode = 0;
            if (!(in >> event.tick >> event.button >> down >> player1 >> mode)) return false;
            if (event.tick < 0 || event.button < 1 || event.button > 3 ||
                mode < 0 || mode >= static_cast<int>(kModeCount)) return false;
            event.down = down != 0;
            event.isPlayer1 = player1 != 0;
            event.mode = static_cast<Mode>(mode);
            events.push_back(event);
        }
        info.eventCount = count;
        info.path = path;
        return true;
    }

    bool saveRoute(PlayLayer* layer) {
        if (!layer || !layer->m_level || m_events.empty()) return false;
        std::error_code ec;
        fs::create_directories(routesDir(), ec);
        if (ec) return false;

        auto base = fileSlug(m_pendingName) + "-" + std::to_string(m_levelID);
        fs::path path = routesDir() / (base + ".apl");
        for (int suffix = 2; fs::exists(path, ec); ++suffix) {
            path = routesDir() / (base + "-" + std::to_string(suffix) + ".apl");
            ec.clear();
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "AUTOPLAY_ROUTE_V2\n"
            << m_levelID << '\n'
            << std::quoted(m_levelName) << '\n'
            << std::quoted(m_pendingName) << '\n'
            << m_events.size() << '\n';
        for (auto const& event : m_events) {
            out << event.tick << ' ' << event.button << ' '
                << (event.down ? 1 : 0) << ' '
                << (event.isPlayer1 ? 1 : 0) << ' '
                << static_cast<int>(event.mode) << '\n';
        }
        out.flush();
        return static_cast<bool>(out);
    }

    std::string buildProfile() const {
        std::array<ModeStats, kModeCount> stats{};
        std::array<std::array<int, 4>, 2> pressTick{};
        std::array<std::array<int, 4>, 2> pressMode{};
        for (auto& row : pressTick) row.fill(-1);
        for (auto& row : pressMode) row.fill(0);

        for (auto const& event : m_events) {
            if (event.button < 1 || event.button > 3) continue;
            size_t playerSlot = event.isPlayer1 ? 0 : 1;
            size_t buttonSlot = static_cast<size_t>(event.button);
            size_t modeSlot = std::min(static_cast<size_t>(event.mode), kModeCount - 1);
            if (event.down) {
                ++stats[modeSlot].presses;
                pressTick[playerSlot][buttonSlot] = event.tick;
                pressMode[playerSlot][buttonSlot] = static_cast<int>(modeSlot);
            } else if (pressTick[playerSlot][buttonSlot] >= 0) {
                int duration = std::max(0, event.tick - pressTick[playerSlot][buttonSlot]);
                auto& stat = stats[static_cast<size_t>(pressMode[playerSlot][buttonSlot])];
                ++stat.holds;
                stat.totalHoldTicks += duration;
                stat.maxHoldTicks = std::max(stat.maxHoldTicks, duration);
                pressTick[playerSlot][buttonSlot] = -1;
            }
        }

        std::string result = fmt::format("{} recorded input events.", m_events.size());
        for (size_t i = 0; i < stats.size(); ++i) {
            if (stats[i].presses == 0) continue;
            double average = stats[i].holds
                ? static_cast<double>(stats[i].totalHoldTicks) / stats[i].holds : 0.0;
            result += fmt::format("\n{}: {} presses, average hold {:.1f} ticks, longest {} ticks",
                modeName(static_cast<Mode>(i)), stats[i].presses, average, stats[i].maxHoldTicks);
        }
        return result;
    }

    std::vector<InputEvent> m_events;
    size_t m_cursor = 0;
    int m_levelID = 0;
    std::string m_levelName;
    std::string m_pendingName = "My Route";
    int m_recordOrigin = 0;
    int m_playOrigin = 0;
    bool m_recording = false;
    bool m_playing = false;
    bool m_injecting = false;
    bool m_pendingRecord = false;
    bool m_pendingPlayback = false;
    bool m_restartRequested = false;
};

} // namespace autoplay

class AutoPlayNamePrompt : public CCLayerColor {
public:
    static AutoPlayNamePrompt* create(std::function<void(std::string)> callback) {
        auto* ret = new AutoPlayNamePrompt();
        if (ret->init(std::move(callback))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init(std::function<void(std::string)> callback) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        if (!CCLayerColor::initWithColor({0, 0, 0, 175}, win.width, win.height)) return false;
        m_callback = std::move(callback);

        auto* panel = CCLayerColor::create({36, 39, 56, 255}, 390.f, 170.f);
        panel->setPosition(ccp((win.width - 390.f) * .5f, (win.height - 170.f) * .5f));
        this->addChild(panel);

        auto* title = CCLabelBMFont::create("Name this save", "bigFont.fnt");
        title->setPosition(ccp(195.f, 143.f));
        title->setScale(.72f);
        panel->addChild(title);

        m_input = CCTextInputNode::create(330, 30, "Enter a name...", "bigFont.fnt");
        m_input->setPosition(ccp(195.f, 92.f));
        m_input->setLabelPlaceholderColor({180, 180, 190});
        panel->addChild(m_input);

        auto* menu = CCMenu::create();
        menu->setPosition(CCPointZero);
        panel->addChild(menu);

        auto* save = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save", 90, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(AutoPlayNamePrompt::onSave));
        save->setPosition(ccp(140.f, 35.f));
        menu->addChild(save);

        auto* cancel = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Cancel", 90, true, "bigFont.fnt", "GJ_button_06.png", 30.f, .65f),
            this, menu_selector(AutoPlayNamePrompt::onCancel));
        cancel->setPosition(ccp(250.f, 35.f));
        menu->addChild(cancel);
        return true;
    }

    void onSave(CCObject*) {
        auto name = autoplay::cleanName(m_input ? m_input->getString() : "");
        if (name.empty()) {
            Notification::create("Please enter a name first.", NotificationIcon::Warning)->show();
            return;
        }
        auto callback = std::move(m_callback);
        this->removeFromParentAndCleanup(true);
        if (callback) callback(std::move(name));
    }

    void onCancel(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

private:
    CCTextInputNode* m_input = nullptr;
    std::function<void(std::string)> m_callback;
};

class SavedRoutesPopup : public CCLayerColor {
public:
    static SavedRoutesPopup* create(
        std::vector<autoplay::SavedRoute> routes,
        std::function<void(fs::path)> callback
    ) {
        auto* ret = new SavedRoutesPopup();
        if (ret->init(std::move(routes), std::move(callback))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init(std::vector<autoplay::SavedRoute> routes, std::function<void(fs::path)> callback) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        if (!CCLayerColor::initWithColor({0, 0, 0, 185}, win.width, win.height)) return false;
        m_routes = std::move(routes);
        m_callback = std::move(callback);

        auto* panel = CCLayerColor::create({35, 39, 56, 255}, 430.f, 390.f);
        panel->setPosition(ccp((win.width - 430.f) * .5f, (win.height - 390.f) * .5f));
        this->addChild(panel);

        auto* title = CCLabelBMFont::create("Saved Routes", "bigFont.fnt");
        title->setPosition(ccp(215.f, 360.f));
        title->setScale(.78f);
        panel->addChild(title);

        auto* menu = CCMenu::create();
        menu->setPosition(CCPointZero);
        panel->addChild(menu);

        if (m_routes.empty()) {
            auto* empty = CCLabelBMFont::create("No routes saved for this level yet.", "bigFont.fnt");
            empty->setPosition(ccp(215.f, 205.f));
            empty->setScale(.48f);
            panel->addChild(empty);
        } else {
            size_t shown = std::min<size_t>(m_routes.size(), 7);
            for (size_t i = 0; i < shown; ++i) {
                auto label = fmt::format("{}  ({} inputs)", m_routes[i].name, m_routes[i].eventCount);
                if (label.size() > 34) label.resize(34);
                auto* button = CCMenuItemSpriteExtra::create(
                    ButtonSprite::create(label.c_str(), 285, true, "bigFont.fnt", "GJ_button_02.png", 28.f, .55f),
                    this, menu_selector(SavedRoutesPopup::onSelect));
                button->setTag(static_cast<int>(i));
                button->setPosition(ccp(215.f, 310.f - static_cast<float>(i) * 39.f));
                menu->addChild(button);
            }
            if (m_routes.size() > shown) {
                auto* more = CCLabelBMFont::create("Showing first 7 saves. Rename older saves to sort them.", "bigFont.fnt");
                more->setScale(.38f);
                more->setPosition(ccp(215.f, 47.f));
                panel->addChild(more);
            }
        }

        auto* close = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Close", 90, true, "bigFont.fnt", "GJ_button_06.png", 28.f, .55f),
            this, menu_selector(SavedRoutesPopup::onClose));
        close->setPosition(ccp(215.f, 22.f));
        menu->addChild(close);
        return true;
    }

    void onSelect(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || static_cast<size_t>(index) >= m_routes.size()) return;
        auto path = m_routes[static_cast<size_t>(index)].path;
        auto callback = std::move(m_callback);
        this->removeFromParentAndCleanup(true);
        if (callback) callback(std::move(path));
    }

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

private:
    std::vector<autoplay::SavedRoute> m_routes;
    std::function<void(fs::path)> m_callback;
};

class AutoPlayBaseLayer : public GJBaseGameLayer {};

class $modify(AutoPlayGameBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto* layer = PlayLayer::get();
        auto& controller = autoplay::Controller::get();
        if (layer && static_cast<GJBaseGameLayer*>(layer) == this && !controller.isInjecting()) {
            controller.capture(layer, down, button, isPlayer1);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        autoplay::Controller::get().dispatch(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(AutoPlayLayer, PlayLayer) {
    void update(float dt) {
        autoplay::Controller::get().beginPendingAction(this);
        PlayLayer::update(dt);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        autoplay::Controller::get().onDeath(this);
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        autoplay::Controller::get().onComplete(this);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        autoplay::Controller::get().onQuit(this);
        PlayLayer::onQuit();
    }
};

class AutoPlayMenuHandler : public CCNode {
public:
    void onRecord(CCObject*) {
        auto* layer = PlayLayer::get();
        if (!layer) return;
        auto& controller = autoplay::Controller::get();
        if (controller.isRecording()) {
            controller.finishRecording(layer, "manual stop");
            return;
        }

        auto* prompt = AutoPlayNamePrompt::create([layer](std::string name) {
            auto* current = PlayLayer::get();
            if (!current) return;
            autoplay::Controller::get().requestRecording(std::move(name));
            current->resumeAndRestart(true);
        });
        if (prompt) layer->getParent()->addChild(prompt, 5000);
    }

    void onSavedRoutes(CCObject*) {
        auto* layer = PlayLayer::get();
        if (!layer || !layer->m_level) return;
        auto routes = autoplay::Controller::get().listRoutes(
            layer->m_level->m_levelID, std::string(layer->m_level->m_levelName)
        );
        auto* popup = SavedRoutesPopup::create(std::move(routes), [](fs::path path) {
            auto* current = PlayLayer::get();
            if (!current) return;
            if (autoplay::Controller::get().requestPlayback(current, path)) {
                current->resumeAndRestart(true);
            }
        });
        if (popup) layer->getParent()->addChild(popup, 5000);
    }

    void onStop(CCObject*) {
        autoplay::Controller::get().stop(PlayLayer::get());
        Notification::create("AutoPlay stopped.", NotificationIcon::Info)->show();
    }
};

class $modify(AutoPlayPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!PlayLayer::get()) return;

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto* handler = new AutoPlayMenuHandler();
        handler->autorelease();
        this->addChild(handler, 100);

        auto* menu = CCMenu::create();
        menu->setPosition(ccp(win.width - 92.f, win.height - 56.f));
        menu->setID("autoplay-learner-menu");

        auto* recordButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Record Named", 112, true, "bigFont.fnt", "GJ_button_01.png", 27.f, .5f),
            handler, menu_selector(AutoPlayMenuHandler::onRecord));
        recordButton->setPosition(ccp(0.f, 0.f));
        menu->addChild(recordButton);

        auto* savedButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Saved Routes", 112, true, "bigFont.fnt", "GJ_button_02.png", 27.f, .5f),
            handler, menu_selector(AutoPlayMenuHandler::onSavedRoutes));
        savedButton->setPosition(ccp(0.f, -36.f));
        menu->addChild(savedButton);

        auto* stopButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Stop Bot", 112, true, "bigFont.fnt", "GJ_button_06.png", 27.f, .5f),
            handler, menu_selector(AutoPlayMenuHandler::onStop));
        stopButton->setPosition(ccp(0.f, -72.f));
        menu->addChild(stopButton);

        this->addChild(menu, 100);
    }
};

class AutoPlayEditorHandler : public CCNode {
public:
    void onAnalyze(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (!editor || !editor->m_level) return;

        auto* prompt = AutoPlayNamePrompt::create([](std::string name) {
            std::string report;
            if (autoplay::Controller::get().analyzeEditor(std::move(name), report)) {
                FLAlertLayer::create("Level Analysis", report, "OK")->show();
            } else {
                Notification::create("Could not analyze or save this level.", NotificationIcon::Error)->show();
            }
        });
        if (prompt) this->getParent()->addChild(prompt, 5000);
    }
};

class $modify(AutoPlayEditorPauseLayer, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();

        if (!LevelEditorLayer::get()) return;
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto* handler = new AutoPlayEditorHandler();
        handler->autorelease();
        this->addChild(handler, 100);

        auto* menu = CCMenu::create();
        menu->setPosition(ccp(win.width - 105.f, 45.f));

        auto* analyzeButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Analyze Level", 125, true, "bigFont.fnt", "GJ_button_01.png", 28.f, .5f),
            handler, menu_selector(AutoPlayEditorHandler::onAnalyze));
        menu->addChild(analyzeButton);
        this->addChild(menu, 100);
    }
};
