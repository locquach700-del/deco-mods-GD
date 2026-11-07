#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace autoplay {

enum class Mode : uint8_t {
    Cube = 0,
    Ship,
    Ball,
    UFO,
    Wave,
    Robot,
    Spider,
    Swing,
    Unknown
};

constexpr size_t kModeCount = 8;
constexpr size_t kMaxEvents = 100000;

struct InputEvent {
    int tick = 0;
    int button = 1;
    bool down = false;
    bool isPlayer1 = true;
    Mode mode = Mode::Cube;
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

class Controller {
public:
    static Controller& get() {
        static Controller instance;
        return instance;
    }

    bool isRecording() const { return m_recording; }
    bool isPlaying() const { return m_playing; }
    bool isInjecting() const { return m_injecting; }
    bool hasMacro() const { return !m_events.empty(); }
    size_t eventCount() const { return m_events.size(); }

    void requestRecording() {
        m_pendingRecord = true;
        m_pendingPlayback = false;
        m_playing = false;
        m_recording = false;
    }

    bool requestPlayback(PlayLayer* layer) {
        if (!layer || !layer->m_level) return false;
        if (!loadLast()) return false;

        std::string levelName = layer->m_level->m_levelName;
        if (m_levelID != layer->m_level->m_levelID || m_levelName != levelName) {
            Notification::create(
                fmt::format("Saved trial belongs to '{}'. Open that same level before replaying.", m_levelName),
                NotificationIcon::Warning
            )->show();
            return false;
        }

        m_pendingPlayback = true;
        m_pendingRecord = false;
        m_recording = false;
        m_playing = false;
        return true;
    }

    void beginPendingAction(PlayLayer* layer) {
        if (!layer || layer->m_isPaused) return;

        if (m_pendingRecord) {
            m_pendingRecord = false;
            beginRecording(layer);
        } else if (m_pendingPlayback) {
            m_pendingPlayback = false;
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
        m_pendingRecord = false;
        m_pendingPlayback = false;
        Notification::create("AutoPlay: recording this trial. Play normally.", NotificationIcon::Info)->show();
    }

    void capture(PlayLayer* layer, bool down, int button, bool isPlayer1) {
        if (!m_recording || !layer || button < 1 || button > 3 || m_events.size() >= kMaxEvents) return;

        int tick = static_cast<int>(layer->m_gameState.m_commandIndex) - m_recordOrigin;
        if (tick < 0) tick = 0;

        PlayerObject* player = isPlayer1 ? layer->m_player1 : layer->m_player2;
        m_events.push_back(InputEvent{
            tick,
            button,
            down,
            isPlayer1,
            currentMode(player)
        });
    }

    void finishRecording(PlayLayer* layer, char const* reason) {
        if (!m_recording) return;
        m_recording = false;
        m_pendingRecord = false;

        if (m_events.empty()) {
            Notification::create("AutoPlay: no input was recorded.", NotificationIcon::Warning)->show();
            return;
        }

        if (saveLast(layer)) {
            FLAlertLayer::create(
                "AutoPlay Learner",
                fmt::format("Trial saved ({})\n{}\n\nThe same input route can now be replayed on this level.",
                    reason ? reason : "stopped", buildProfile()),
                "OK"
            )->show();
        } else {
            Notification::create("AutoPlay: could not save the trial.", NotificationIcon::Error)->show();
        }
    }

    void beginPlayback(PlayLayer* layer) {
        if (!layer || m_events.empty()) return;
        m_cursor = 0;
        m_playOrigin = static_cast<int>(layer->m_gameState.m_commandIndex);
        m_playing = true;
        m_recording = false;
        m_pendingPlayback = false;
        Notification::create(
            fmt::format("AutoPlay: replaying {} recorded inputs.", m_events.size()),
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
            Notification::create("AutoPlay: saved input sequence finished.", NotificationIcon::Success)->show();
        }
    }

    void stop(PlayLayer* layer) {
        m_pendingRecord = false;
        m_pendingPlayback = false;
        if (m_recording) {
            finishRecording(layer, "manual stop");
        }
        m_playing = false;
        m_injecting = false;
    }

    void onDeath(PlayLayer* layer) {
        if (m_recording) finishRecording(layer, "player died");
        m_playing = false;
        m_pendingPlayback = false;
    }

    void onComplete(PlayLayer* layer) {
        if (m_recording) finishRecording(layer, "level complete");
        m_playing = false;
        m_pendingPlayback = false;
    }

    void onQuit(PlayLayer* layer) {
        if (m_recording) finishRecording(layer, "level exited");
        m_playing = false;
        m_pendingPlayback = false;
        m_pendingRecord = false;
    }

private:
    Controller() = default;

    ghc::filesystem::path macroPath() const {
        return Mod::get()->getSaveDir() / "last-trial.apl";
    }

    bool saveLast(PlayLayer* layer) {
        if (!layer || !layer->m_level) return false;
        std::ofstream out(macroPath().string(), std::ios::binary | std::ios::trunc);
        if (!out) return false;

        out << "AUTOPLAY_LEARNER_V1\n";
        out << m_levelID << '\n';
        out << std::quoted(m_levelName) << '\n';
        out << m_events.size() << '\n';
        for (auto const& event : m_events) {
            out << event.tick << ' '
                << event.button << ' '
                << (event.down ? 1 : 0) << ' '
                << (event.isPlayer1 ? 1 : 0) << ' '
                << static_cast<int>(event.mode) << '\n';
        }
        out.flush();
        return static_cast<bool>(out);
    }

    bool loadLast() {
        std::ifstream in(macroPath().string(), std::ios::binary);
        if (!in) return false;

        std::string magic;
        std::getline(in, magic);
        if (magic != "AUTOPLAY_LEARNER_V1") return false;

        int levelID = 0;
        std::string levelName;
        size_t count = 0;
        if (!(in >> levelID >> std::quoted(levelName) >> count)) return false;
        if (count == 0 || count > kMaxEvents) return false;

        std::vector<InputEvent> loaded;
        loaded.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            InputEvent event;
            int down = 0, player1 = 1, mode = 0;
            if (!(in >> event.tick >> event.button >> down >> player1 >> mode)) return false;
            if (event.tick < 0 || event.button < 1 || event.button > 3 || mode < 0 || mode >= static_cast<int>(kModeCount)) {
                return false;
            }
            event.down = down != 0;
            event.isPlayer1 = player1 != 0;
            event.mode = static_cast<Mode>(mode);
            loaded.push_back(event);
        }

        m_levelID = levelID;
        m_levelName = std::move(levelName);
        m_events = std::move(loaded);
        return true;
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

        std::string result = fmt::format("Recorded {} input events.", m_events.size());
        for (size_t i = 0; i < stats.size(); ++i) {
            if (stats[i].presses == 0) continue;
            double average = stats[i].holds
                ? static_cast<double>(stats[i].totalHoldTicks) / stats[i].holds
                : 0.0;
            result += fmt::format("\n{}: {} presses, average hold {:.1f} ticks, longest {} ticks",
                modeName(static_cast<Mode>(i)), stats[i].presses, average, stats[i].maxHoldTicks);
        }
        return result;
    }

    std::vector<InputEvent> m_events;
    size_t m_cursor = 0;
    int m_levelID = 0;
    std::string m_levelName;
    int m_recordOrigin = 0;
    int m_playOrigin = 0;
    bool m_recording = false;
    bool m_playing = false;
    bool m_injecting = false;
    bool m_pendingRecord = false;
    bool m_pendingPlayback = false;
};

} // namespace autoplay

class $modify(AutoPlayBaseLayer, GJBaseGameLayer) {
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

class $modify(AutoPlayPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        if (!PlayLayer::get()) return;
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto* menu = CCMenu::create();
        menu->setPosition(ccp(win.width - 88.f, win.height - 52.f));
        menu->setID("autoplay-learner-menu");

        auto& controller = autoplay::Controller::get();
        char const* recordText = controller.isRecording() ? "Stop Record" : "Record Trial";
        char const* recordFrame = controller.isRecording() ? "GJ_button_06.png" : "GJ_button_01.png";

        auto* recordSprite = ButtonSprite::create(recordText, 104, true, "bigFont.fnt", recordFrame, 28.f, 0.5f);
        auto* recordButton = CCMenuItemSpriteExtra::create(
            recordSprite, this, menu_selector(AutoPlayPauseLayer::onRecord));
        recordButton->setPosition(ccp(0.f, 0.f));
        menu->addChild(recordButton);

        auto* replaySprite = ButtonSprite::create("Replay Last", 104, true, "bigFont.fnt", "GJ_button_02.png", 28.f, 0.5f);
        auto* replayButton = CCMenuItemSpriteExtra::create(
            replaySprite, this, menu_selector(AutoPlayPauseLayer::onReplay));
        replayButton->setPosition(ccp(0.f, -34.f));
        menu->addChild(replayButton);

        auto* stopSprite = ButtonSprite::create("Stop Bot", 104, true, "bigFont.fnt", "GJ_button_06.png", 28.f, 0.5f);
        auto* stopButton = CCMenuItemSpriteExtra::create(
            stopSprite, this, menu_selector(AutoPlayPauseLayer::onStop));
        stopButton->setPosition(ccp(0.f, -68.f));
        menu->addChild(stopButton);

        this->addChild(menu, 100);
    }

    void onRecord(CCObject*) {
        auto* layer = PlayLayer::get();
        if (!layer) return;
        auto& controller = autoplay::Controller::get();
        if (controller.isRecording()) {
            controller.finishRecording(layer, "manual stop");
            return;
        }

        controller.requestRecording();
        layer->resumeAndRestart(true);
    }

    void onReplay(CCObject*) {
        auto* layer = PlayLayer::get();
        if (!layer) return;
        auto& controller = autoplay::Controller::get();
        if (!controller.requestPlayback(layer)) {
            Notification::create(
                "No saved trial for this level. Record one first.",
                NotificationIcon::Warning
            )->show();
            return;
        }
        layer->resumeAndRestart(true);
    }

    void onStop(CCObject*) {
        autoplay::Controller::get().stop(PlayLayer::get());
        Notification::create("AutoPlay stopped.", NotificationIcon::Info)->show();
    }
};
