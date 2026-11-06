#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/cocos/robtop/keyboard_dispatcher/CCKeyboardDispatcher.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(GEODE_IS_WINDOWS)
#  define NOMINMAX
#  include <windows.h>
#elif defined(GEODE_IS_ANDROID)
#  include <jni.h>
#  include <Geode/cocos/platform/android/jni/JniHelper.h>
#endif

using namespace geode::prelude;

namespace {
    constexpr std::string_view kCodeMagic = "STY1";
    constexpr size_t kMaxCodeBytes = 4 * 1024 * 1024;
    constexpr size_t kMaxSettingsBytes = 512 * 1024;
    constexpr size_t kMaxObjectBytes = 12 * 1024;
    constexpr size_t kMaxObjects = 2500;
    constexpr size_t kMaxNameBytes = 1024;

    struct StyleData {
        std::string levelName;
        std::string creatorName;
        std::string settings;
        std::vector<std::string> objects;
    };

    class StyleScannerPopup;
    StyleScannerPopup* g_popup = nullptr;

    int base64Value(unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    }

    std::string base64Encode(std::string_view input) {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        output.reserve(((input.size() + 2) / 3) * 4);

        for (size_t i = 0; i < input.size(); i += 3) {
            const auto a = static_cast<uint8_t>(input[i]);
            const bool hasB = i + 1 < input.size();
            const bool hasC = i + 2 < input.size();
            const auto b = hasB ? static_cast<uint8_t>(input[i + 1]) : 0;
            const auto c = hasC ? static_cast<uint8_t>(input[i + 2]) : 0;
            const uint32_t value =
                (static_cast<uint32_t>(a) << 16) |
                (static_cast<uint32_t>(b) << 8) |
                static_cast<uint32_t>(c);

            output.push_back(alphabet[(value >> 18) & 0x3f]);
            output.push_back(alphabet[(value >> 12) & 0x3f]);
            output.push_back(hasB ? alphabet[(value >> 6) & 0x3f] : '=');
            output.push_back(hasC ? alphabet[value & 0x3f] : '=');
        }
        return output;
    }

    bool base64Decode(std::string_view input, std::string& output, size_t maxOutput) {
        output.clear();
        if (input.empty()) return true;
        if ((input.size() % 4) != 0) return false;
        if (input.size() > ((maxOutput + 2) / 3) * 4 + 4) return false;

        output.reserve(std::min(maxOutput, (input.size() / 4) * 3));

        for (size_t i = 0; i < input.size(); i += 4) {
            const int a = base64Value(static_cast<unsigned char>(input[i]));
            const int b = base64Value(static_cast<unsigned char>(input[i + 1]));
            if (a < 0 || b < 0) return false;

            const bool padC = input[i + 2] == '=';
            const bool padD = input[i + 3] == '=';
            if (padC && !padD) return false;
            if ((padC || padD) && i + 4 != input.size()) return false;

            const int c = padC ? 0 : base64Value(static_cast<unsigned char>(input[i + 2]));
            const int d = padD ? 0 : base64Value(static_cast<unsigned char>(input[i + 3]));
            if (c < 0 || d < 0) return false;

            const uint32_t value =
                (static_cast<uint32_t>(a) << 18) |
                (static_cast<uint32_t>(b) << 12) |
                (static_cast<uint32_t>(c) << 6) |
                static_cast<uint32_t>(d);

            if (output.size() >= maxOutput) return false;
            output.push_back(static_cast<char>((value >> 16) & 0xff));

            if (!padC) {
                if (output.size() >= maxOutput) return false;
                output.push_back(static_cast<char>((value >> 8) & 0xff));
            }
            if (!padD) {
                if (output.size() >= maxOutput) return false;
                output.push_back(static_cast<char>(value & 0xff));
            }
        }
        return true;
    }

    std::string stripWhitespace(std::string_view input) {
        std::string result;
        result.reserve(input.size());
        for (unsigned char ch : input) {
            if (!std::isspace(ch)) result.push_back(static_cast<char>(ch));
        }
        return result;
    }

    std::vector<std::string_view> splitCode(std::string const& code) {
        std::vector<std::string_view> fields;
        const std::string_view view(code);
        size_t start = 0;
        for (;;) {
            const size_t end = view.find('|', start);
            if (end == std::string_view::npos) {
                fields.emplace_back(view.substr(start));
                break;
            }
            fields.emplace_back(view.substr(start, end - start));
            start = end + 1;
        }
        return fields;
    }

    bool isStyleObject(GameObject* object) {
        if (!object) return false;
        return object->m_isDecoration ||
            object->m_isDecoration2 ||
            object->m_isColorTrigger ||
            object->isColorTrigger();
    }

    bool validObjectRecord(std::string_view record) {
        // GameObject save strings start with "1,<object-id>,...".
        if (record.size() < 5 || record[0] != '1' || record[1] != ',') return false;
        const size_t end = record.find(',', 2);
        if (end == std::string_view::npos || end == 2 || end > 9) return false;

        int objectID = 0;
        for (size_t i = 2; i < end; ++i) {
            const char ch = record[i];
            if (ch < '0' || ch > '9') return false;
            objectID = objectID * 10 + (ch - '0');
        }
        return objectID > 0 && objectID <= 100000;
    }

    bool encodeStyle(StyleData const& data, std::string& code, std::string& error) {
        if (data.levelName.size() > kMaxNameBytes ||
            data.creatorName.size() > kMaxNameBytes ||
            data.settings.size() > kMaxSettingsBytes ||
            data.objects.size() > kMaxObjects) {
            error = "This style is larger than the supported export limits.";
            return false;
        }

        code.clear();
        code.reserve(256 + data.settings.size() * 2);
        code.append(kCodeMagic);
        code.push_back('|');
        code += base64Encode(data.levelName);
        code.push_back('|');
        code += base64Encode(data.creatorName);
        code.push_back('|');
        code += base64Encode(data.settings);

        for (auto const& object : data.objects) {
            if (object.empty() || object.size() > kMaxObjectBytes ||
                !validObjectRecord(object)) {
                error = "The level contains a decoration record that cannot be exported safely.";
                return false;
            }
            code.push_back('|');
            code += base64Encode(object);
            if (code.size() > kMaxCodeBytes) {
                error = "The exported style exceeds the 4 MiB code limit.";
                return false;
            }
        }

        if (code.size() > kMaxCodeBytes) {
            error = "The exported style exceeds the 4 MiB code limit.";
            return false;
        }
        return true;
    }

    bool decodeStyle(std::string_view rawCode, StyleData& data, std::string& error) {
        if (rawCode.size() > kMaxCodeBytes + 4096) {
            error = "The pasted code is too large.";
            return false;
        }

        const std::string normalized = stripWhitespace(rawCode);
        if (normalized.empty() || normalized.size() > kMaxCodeBytes) {
            error = "Paste a valid StyleScanner code first.";
            return false;
        }

        const auto fields = splitCode(normalized);
        if (fields.size() < 4 || fields[0] != kCodeMagic) {
            error = "This is not a supported StyleScanner code (expected STY1).";
            return false;
        }
        if (fields.size() - 4 > kMaxObjects) {
            error = "The code contains too many decoration records.";
            return false;
        }

        StyleData parsed;
        if (!base64Decode(fields[1], parsed.levelName, kMaxNameBytes) ||
            !base64Decode(fields[2], parsed.creatorName, kMaxNameBytes) ||
            !base64Decode(fields[3], parsed.settings, kMaxSettingsBytes)) {
            error = "The code header or palette data is invalid.";
            return false;
        }

        parsed.objects.reserve(fields.size() - 4);
        size_t decodedObjectBytes = 0;
        for (size_t i = 4; i < fields.size(); ++i) {
            std::string object;
            if (!base64Decode(fields[i], object, kMaxObjectBytes) ||
                !validObjectRecord(object)) {
                error = "A decoration record in the code is invalid.";
                return false;
            }
            decodedObjectBytes += object.size();
            if (decodedObjectBytes > kMaxCodeBytes) {
                error = "Decoded decoration data is too large.";
                return false;
            }
            parsed.objects.emplace_back(std::move(object));
        }

        data = std::move(parsed);
        return true;
    }

    bool readStyleHeader(std::string_view rawCode, std::string& levelName, std::string& creatorName) {
        if (rawCode.size() > kMaxCodeBytes + 4096) return false;
        const std::string normalized = stripWhitespace(rawCode);
        const auto fields = splitCode(normalized);
        if (fields.size() < 4 || fields[0] != kCodeMagic) return false;
        return base64Decode(fields[1], levelName, kMaxNameBytes) &&
            base64Decode(fields[2], creatorName, kMaxNameBytes);
    }

    bool scanLayer(GJBaseGameLayer* source, StyleData& output, std::string& error) {
        if (!source || !source->m_level) {
            error = "No active Geometry Dash level was found.";
            return false;
        }

        output = {};
        output.levelName = source->m_level->m_levelName.c_str();
        output.creatorName = source->m_level->m_creatorName.c_str();

        if (source->m_levelSettings) {
            output.settings = source->m_levelSettings->getSaveString().c_str();
        } else {
            output.settings = source->m_level->generateSettingsString().c_str();
        }

        if (output.settings.size() > kMaxSettingsBytes) {
            error = "The level settings/palette data exceeds the export size limit.";
            return false;
        }

        if (!source->m_objects) return true;

        const unsigned int objectCount = source->m_objects->count();
        output.objects.reserve(std::min<size_t>(objectCount, kMaxObjects));

        for (unsigned int i = 0; i < objectCount; ++i) {
            auto* object = static_cast<GameObject*>(source->m_objects->objectAtIndex(i));
            if (!isStyleObject(object)) continue;

            const auto saved = object->getSaveString(source);
            std::string record = saved.c_str();
            if (record.empty()) continue;

            if (record.size() > kMaxObjectBytes || output.objects.size() >= kMaxObjects) {
                error = "The level has more decoration data than StyleScanner can safely export.";
                return false;
            }
            if (!validObjectRecord(record)) continue;
            output.objects.emplace_back(std::move(record));
        }
        return true;
    }

    bool applyVisualSettings(LevelEditorLayer* editor, std::string const& settingsString) {
        if (!editor || !editor->m_levelSettings || settingsString.empty()) return false;

        auto* imported = LevelSettingsObject::objectFromString(settingsString);
        if (!imported) return false;

        auto* current = editor->m_levelSettings;

        // Copy visual settings only. Gameplay start mode, speed, dual, and platformer mode are untouched.
        current->m_backgroundIndex = imported->m_backgroundIndex;
        current->m_groundIndex = imported->m_groundIndex;
        current->m_middleGroundIndex = imported->m_middleGroundIndex;
        current->m_groundLineIndex = imported->m_groundLineIndex;
        current->m_fontIndex = imported->m_fontIndex;
        current->m_colorPage = imported->m_colorPage;

        if (imported->m_effectManager && current->m_effectManager) {
            editor->transferDefaultColors(imported->m_effectManager, current->m_effectManager);
        }

        editor->levelSettingsUpdated();
        return true;
    }

    bool copyToClipboard(std::string const& text) {
#if defined(GEODE_IS_WINDOWS)
        if (text.empty()) return false;

        const int wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0
        );
        if (wideLength <= 0) return false;

        const SIZE_T bytes = (static_cast<SIZE_T>(wideLength) + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) return false;

        auto* wide = static_cast<wchar_t*>(GlobalLock(memory));
        if (!wide) {
            GlobalFree(memory);
            return false;
        }

        const int converted = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), wide, wideLength
        );
        if (converted != wideLength) {
            GlobalUnlock(memory);
            GlobalFree(memory);
            return false;
        }
        wide[wideLength] = L'\0';
        GlobalUnlock(memory);

        if (!OpenClipboard(nullptr)) {
            GlobalFree(memory);
            return false;
        }
        EmptyClipboard();
        HANDLE result = SetClipboardData(CF_UNICODETEXT, memory);
        CloseClipboard();

        if (!result) {
            GlobalFree(memory);
            return false;
        }
        return true;
#elif defined(GEODE_IS_ANDROID)
        if (text.empty()) return false;

        cocos2d::JniMethodInfo method;
        if (!cocos2d::JniHelper::getStaticMethodInfo(
                method,
                "com/geode/launcher/utils/GeodeUtils",
                "writeClipboard",
                "(Ljava/lang/String;)V")) {
            return false;
        }

        jstring value = method.env->NewStringUTF(text.c_str());
        if (!value) {
            method.env->DeleteLocalRef(method.classID);
            return false;
        }

        method.env->CallStaticVoidMethod(method.classID, method.methodID, value);
        const bool failed = method.env->ExceptionCheck();
        if (failed) method.env->ExceptionClear();
        method.env->DeleteLocalRef(value);
        method.env->DeleteLocalRef(method.classID);
        return !failed;
#else
        (void)text;
        return false;
#endif
    }
}

class StyleScannerPopup final : public geode::Popup<> {
protected:
    geode::TextInput* m_exportInput = nullptr;
    geode::TextInput* m_importInput = nullptr;
    cocos2d::CCLabelBMFont* m_metadataLabel = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;

    ~StyleScannerPopup() override {
        if (g_popup == this) g_popup = nullptr;
    }

    bool setup() override {
        this->setTitle("Style Scanner & Deco Copier", "goldFont.fnt", 0.58f, 20.f);

        auto* exportTitle = cocos2d::CCLabelBMFont::create(
            "SCAN / EXPORT STYLE", "goldFont.fnt"
        );
        exportTitle->setScale(0.48f);
        exportTitle->setPosition({0.f, 145.f});
        m_mainLayer->addChild(exportTitle);

        m_exportInput = geode::TextInput::create(365.f, "Exported style code", "bigFont.fnt");
        if (!m_exportInput) return false;
        m_exportInput->setCommonFilter(geode::CommonFilter::Any);
        m_exportInput->setMaxCharCount(kMaxCodeBytes);
        m_exportInput->setTextAlign(geode::TextInputAlign::Left);
        m_exportInput->setPosition({0.f, 111.f});
        m_exportInput->setScale(0.78f);
        m_mainLayer->addChild(m_exportInput);

        auto* scanButton = cocos2d::CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Scan Current Level", "goldFont.fnt", "GJ_button_01.png", 0.68f),
            this,
            menu_selector(StyleScannerPopup::onScan)
        );
        scanButton->setPosition({-86.f, 71.f});
        m_buttonMenu->addChild(scanButton);

        auto* copyButton = cocos2d::CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Copy Code", "goldFont.fnt", "GJ_button_02.png", 0.68f),
            this,
            menu_selector(StyleScannerPopup::onCopy)
        );
        copyButton->setPosition({86.f, 71.f});
        m_buttonMenu->addChild(copyButton);

        m_metadataLabel = cocos2d::CCLabelBMFont::create(
            "Source Style Name: [None]  |  Original Creator: [None]",
            "chatFont.fnt"
        );
        m_metadataLabel->setScale(0.43f);
        m_metadataLabel->setPosition({0.f, 26.f});
        m_metadataLabel->limitLabelWidth(385.f, 0.43f, 0.2f);
        m_mainLayer->addChild(m_metadataLabel);

        auto* importTitle = cocos2d::CCLabelBMFont::create(
            "PASTE / APPLY STYLE CODE", "goldFont.fnt"
        );
        importTitle->setScale(0.48f);
        importTitle->setPosition({0.f, -22.f});
        m_mainLayer->addChild(importTitle);

        m_importInput = geode::TextInput::create(365.f, "Paste a StyleScanner code", "bigFont.fnt");
        if (!m_importInput) return false;
        m_importInput->setCommonFilter(geode::CommonFilter::Any);
        m_importInput->setMaxCharCount(kMaxCodeBytes);
        m_importInput->setTextAlign(geode::TextInputAlign::Left);
        m_importInput->setPosition({0.f, -58.f});
        m_importInput->setScale(0.78f);
        m_importInput->setCallback([this](std::string const& value) {
            this->updateMetadata(value);
        });
        m_mainLayer->addChild(m_importInput);

        auto* applyButton = cocos2d::CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Apply Style to Editor Level", "goldFont.fnt", "GJ_button_03.png", 0.62f),
            this,
            menu_selector(StyleScannerPopup::onApply)
        );
        applyButton->setPosition({0.f, -105.f});
        m_buttonMenu->addChild(applyButton);

        m_statusLabel = cocos2d::CCLabelBMFont::create(
            "F3: close StyleScanner", "chatFont.fnt"
        );
        m_statusLabel->setScale(0.43f);
        m_statusLabel->setPosition({0.f, -151.f});
        m_statusLabel->limitLabelWidth(385.f, 0.43f, 0.2f);
        m_mainLayer->addChild(m_statusLabel);

        return true;
    }

    void updateMetadata(std::string const& code) {
        std::string levelName;
        std::string creatorName;

        if (!readStyleHeader(code, levelName, creatorName)) {
            m_metadataLabel->setString(
                "Source Style Name: [Invalid / incomplete]  |  Original Creator: [None]"
            );
            return;
        }

        if (levelName.empty()) levelName = "[Unnamed]";
        if (creatorName.empty()) creatorName = "[Unavailable]";

        const std::string label =
            "Source Style Name: " + levelName + "  |  Original Creator: " + creatorName;
        m_metadataLabel->setString(label.c_str());
        m_metadataLabel->limitLabelWidth(385.f, 0.43f, 0.2f);
    }

    void setStatus(std::string const& message, bool isError = false) {
        if (!m_statusLabel) return;
        m_statusLabel->setString(message.c_str());
        m_statusLabel->setColor(isError ? cocos2d::ccc3(255, 105, 105) : cocos2d::ccc3(150, 255, 150));
        m_statusLabel->limitLabelWidth(385.f, 0.43f, 0.2f);
    }

public:
    static StyleScannerPopup* create() {
        auto* popup = new StyleScannerPopup();
        if (popup && popup->initAnchored(420.f, 360.f)) {
            popup->autorelease();
            return popup;
        }
        CC_SAFE_DELETE(popup);
        return nullptr;
    }

    void onScan(cocos2d::CCObject*) {
        auto* editor = LevelEditorLayer::get();
        auto* play = PlayLayer::get();
        auto* active = editor
            ? static_cast<GJBaseGameLayer*>(editor)
            : static_cast<GJBaseGameLayer*>(play);

        StyleData data;
        std::string error;
        if (!scanLayer(active, data, error)) {
            this->setStatus(error, true);
            return;
        }

        std::string code;
        if (!encodeStyle(data, code, error)) {
            this->setStatus(error, true);
            return;
        }

        m_exportInput->setString(code);
        const std::string label =
            "Source Style Name: " + (data.levelName.empty() ? "[Unnamed]" : data.levelName) +
            "  |  Original Creator: " +
            (data.creatorName.empty() ? "[Unavailable]" : data.creatorName);
        m_metadataLabel->setString(label.c_str());
        m_metadataLabel->limitLabelWidth(385.f, 0.43f, 0.2f);

        this->setStatus(
            "Scanned " + std::to_string(data.objects.size()) +
            " decoration / color-trigger objects. Code is ready to copy."
        );
    }

    void onCopy(cocos2d::CCObject*) {
        const std::string code = m_exportInput ? m_exportInput->getString() : "";
        if (code.empty()) {
            this->setStatus("Scan a level before copying its code.", true);
            return;
        }
        if (!copyToClipboard(code)) {
            this->setStatus("Clipboard access failed on this device.", true);
            return;
        }
        this->setStatus("Style code copied to the clipboard.");
    }

    void onApply(cocos2d::CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (!editor) {
            this->setStatus("Open a level in the editor before applying a style.", true);
            return;
        }

        StyleData data;
        std::string error;
        if (!decodeStyle(m_importInput->getString(), data, error)) {
            this->setStatus(error, true);
            return;
        }

        size_t keptObjects = 0;
        size_t removedObjects = 0;

        if (!data.objects.empty()) {
            std::string serializedObjects;
            size_t totalBytes = 0;
            for (auto const& record : data.objects) {
                totalBytes += record.size() + 1;
                if (totalBytes > kMaxCodeBytes) {
                    this->setStatus("Decoration data exceeds the safe import limit.", true);
                    return;
                }
                serializedObjects += record;
                serializedObjects.push_back(';');
            }

            auto* created = editor->createObjectsFromString(serializedObjects, false, false);
            if (!created) {
                this->setStatus("Geometry Dash could not create the decoration objects.", true);
                return;
            }

            // Keep only visual decorations and color triggers. This protects the
            // target level's structural blocks even if a pasted code is modified.
            const unsigned int createdCount = created->count();
            for (unsigned int i = 0; i < createdCount; ++i) {
                auto* object = static_cast<GameObject*>(created->objectAtIndex(i));
                if (isStyleObject(object)) {
                    ++keptObjects;
                } else if (object) {
                    editor->removeObject(object, true);
                    ++removedObjects;
                }
            }

            if (keptObjects == 0) {
                this->setStatus("No supported decoration objects were created.", true);
                return;
            }
        }

        const bool settingsApplied = applyVisualSettings(editor, data.settings);
        if (data.objects.empty() && !settingsApplied) {
            this->setStatus("No visual settings or decoration objects could be applied.", true);
            return;
        }

        std::string result =
            "Added " + std::to_string(keptObjects) + " decoration / color-trigger objects";
        if (settingsApplied) result += " and applied background / palette settings";
        if (removedObjects > 0) {
            result += " (" + std::to_string(removedObjects) + " non-style objects rejected)";
        }
        result += ". Existing layout blocks were not intentionally replaced.";
        this->setStatus(result);
    }

    void onClose(cocos2d::CCObject* sender) override {
        if (g_popup == this) g_popup = nullptr;
        geode::Popup<>::onClose(sender);
    }
};

namespace {
    void toggleStyleScanner() {
        if (!LevelEditorLayer::get() && !PlayLayer::get()) return;

        if (g_popup) {
            g_popup->onClose(nullptr);
            return;
        }

        if (auto* popup = StyleScannerPopup::create()) {
            g_popup = popup;
            popup->show();
        }
    }

    class StyleScannerKeyboardListener final : public cocos2d::CCKeyboardDelegate {
        bool m_f3IsDown = false;

    public:
        void keyDown(cocos2d::enumKeyCodes key) override {
            if (key != cocos2d::KEY_F3 || m_f3IsDown) return;
            m_f3IsDown = true;
            toggleStyleScanner();
        }

        void keyUp(cocos2d::enumKeyCodes key) override {
            if (key == cocos2d::KEY_F3) m_f3IsDown = false;
        }
    };

    StyleScannerKeyboardListener g_keyboardListener;
}

$on_game(Loaded) {
    if (auto* dispatcher = cocos2d::CCKeyboardDispatcher::get()) {
        dispatcher->addDelegate(&g_keyboardListener);
    }
}
