#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/cocos/support/zip_support/ZipUtils.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;

class StyleScannerPopup;

namespace {
    constexpr std::string_view kCodeMagic = "STY3";
    constexpr std::string_view kPreviousCodeMagic = "STY2";
    constexpr std::string_view kLegacyCodeMagic = "STY1";
    constexpr size_t kMaxCodeBytes = 12 * 1024 * 1024;
    constexpr size_t kMaxPayloadBytes = 64 * 1024 * 1024;
    constexpr size_t kMaxSettingsBytes = 1024 * 1024;
    constexpr size_t kMaxObjectBytes = 64 * 1024;
    constexpr size_t kMaxObjects = 50000;
    constexpr size_t kMaxNameBytes = 4096;

    struct DecoItem {
        std::string record;
        float offsetX = 0.f;
        float offsetY = 0.f;
    };

    struct DecoTemplate {
        uint32_t anchorType = 0;
        float anchorWidth = 30.f;
        float anchorHeight = 30.f;
        std::vector<DecoItem> items;
    };

    struct StyleData {
        std::string levelName;
        std::string creatorName;
        std::string settings;
        // Used only when decoding old STY1/STY2 codes. New exports store
        // decoration templates relative to source blocks instead of whole maps.
        std::vector<std::string> objects;
        std::vector<DecoTemplate> templates;
    };

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

    // Only decorative pieces are copied. Regular level geometry is used
    // as an anchor during matching, but is never exported or imported.
    bool isDecorationPart(GameObject* object) {
        if (!object || object->m_objectID == 749) return false;
        if (object->m_classType == GameObjectClassType::Effect) return false;

        if (object->m_isDecoration || object->m_isDecoration2) {
            switch (object->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Decoration:
                case GameObjectType::Slope:
                case GameObjectType::Breakable:
                    return true;
                default:
                    return false;
            }
        }
        return object->m_objectType == GameObjectType::Decoration;
    }

    bool isAnchorBlock(GameObject* object) {
        if (!object || object->m_objectID == 749) return false;
        if (object->m_classType == GameObjectClassType::Effect ||
            isDecorationPart(object)) return false;

        switch (object->m_objectType) {
            case GameObjectType::Solid:
            case GameObjectType::Slope:
            case GameObjectType::Breakable:
                return true;
            default:
                return false;
        }
    }

    float safeObjectWidth(GameObject* object) {
        return object && std::isfinite(object->m_width) && object->m_width > 1.f
            ? object->m_width : 30.f;
    }

    float safeObjectHeight(GameObject* object) {
        return object && std::isfinite(object->m_height) && object->m_height > 1.f
            ? object->m_height : 30.f;
    }

    void appendU32(std::string& output, uint32_t value) {
        output.push_back(static_cast<char>(value & 0xff));
        output.push_back(static_cast<char>((value >> 8) & 0xff));
        output.push_back(static_cast<char>((value >> 16) & 0xff));
        output.push_back(static_cast<char>((value >> 24) & 0xff));
    }

    bool readU32(std::string_view input, size_t& cursor, uint32_t& value) {
        if (cursor > input.size() || input.size() - cursor < 4) return false;
        value =
            static_cast<uint32_t>(static_cast<uint8_t>(input[cursor])) |
            (static_cast<uint32_t>(static_cast<uint8_t>(input[cursor + 1])) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(input[cursor + 2])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(input[cursor + 3])) << 24);
        cursor += 4;
        return true;
    }

    void appendFloat(std::string& output, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(output, bits);
    }

    bool readFloat(std::string_view input, size_t& cursor, float& value) {
        uint32_t bits = 0;
        if (!readU32(input, cursor, bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    }

    bool appendField(std::string& output, std::string_view field) {
        if (field.size() > UINT32_MAX) return false;
        if (output.size() > kMaxPayloadBytes - 4 ||
            field.size() > kMaxPayloadBytes - output.size() - 4) return false;
        appendU32(output, static_cast<uint32_t>(field.size()));
        output.append(field.data(), field.size());
        return true;
    }

    bool readField(
        std::string_view input,
        size_t& cursor,
        std::string& output,
        size_t maxFieldBytes
    ) {
        uint32_t size = 0;
        if (!readU32(input, cursor, size) || size > maxFieldBytes) return false;
        if (cursor > input.size() || size > input.size() - cursor) return false;
        output.assign(input.data() + cursor, size);
        cursor += size;
        return true;
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
            data.templates.size() > kMaxObjects) {
            error = "The style archive exceeds its safe export limits.";
            return false;
        }

        size_t totalItems = 0;
        for (auto const& styleTemplate : data.templates) {
            totalItems += styleTemplate.items.size();
            if (totalItems > kMaxObjects ||
                styleTemplate.items.size() > kMaxObjects ||
                !std::isfinite(styleTemplate.anchorWidth) ||
                !std::isfinite(styleTemplate.anchorHeight) ||
                styleTemplate.anchorWidth <= 0.f ||
                styleTemplate.anchorHeight <= 0.f ||
                styleTemplate.anchorWidth > 10000.f ||
                styleTemplate.anchorHeight > 10000.f) {
                error = "A block decoration template is outside the safe limits.";
                return false;
            }
        }

        std::string payload;
        payload.reserve(std::min<size_t>(
            kMaxPayloadBytes,
            256 + data.settings.size() + totalItems * 128
        ));
        if (!appendField(payload, data.levelName) ||
            !appendField(payload, data.creatorName) ||
            !appendField(payload, data.settings)) {
            error = "The style header is too large.";
            return false;
        }

        appendU32(payload, static_cast<uint32_t>(data.templates.size()));
        for (auto const& styleTemplate : data.templates) {
            appendU32(payload, styleTemplate.anchorType);
            appendFloat(payload, styleTemplate.anchorWidth);
            appendFloat(payload, styleTemplate.anchorHeight);
            appendU32(payload, static_cast<uint32_t>(styleTemplate.items.size()));

            for (auto const& item : styleTemplate.items) {
                if (item.record.empty() || item.record.size() > kMaxObjectBytes ||
                    !validObjectRecord(item.record) ||
                    !std::isfinite(item.offsetX) || !std::isfinite(item.offsetY) ||
                    std::abs(item.offsetX) > 100000.f ||
                    std::abs(item.offsetY) > 100000.f ||
                    !appendField(payload, item.record)) {
                    error = "A decoration piece in the style archive is invalid.";
                    return false;
                }
                appendFloat(payload, item.offsetX);
                appendFloat(payload, item.offsetY);
                if (payload.size() > kMaxPayloadBytes) {
                    error = "The style exceeds the 64 MiB uncompressed export limit.";
                    return false;
                }
            }
        }

        auto compressed = cocos2d::ZipUtils::compressString(payload, false, 0);
        if (compressed.empty()) {
            error = "The style data could not be compressed.";
            return false;
        }

        code = std::string(kCodeMagic);
        code.push_back('|');
        code += cocos2d::ZipUtils::base64URLEncode(compressed);

        if (code.size() > kMaxCodeBytes) {
            error = "The compressed style exceeds the 12 MiB code limit.";
            return false;
        }
        return true;
    }

    bool decodeLegacyStyle(
        std::vector<std::string_view> const& fields,
        StyleData& data,
        std::string& error
    ) {
        if (fields.size() < 4 || fields.size() - 4 > kMaxObjects) {
            error = "The legacy style code has an invalid object count.";
            return false;
        }

        StyleData parsed;
        if (!base64Decode(fields[1], parsed.levelName, kMaxNameBytes) ||
            !base64Decode(fields[2], parsed.creatorName, kMaxNameBytes) ||
            !base64Decode(fields[3], parsed.settings, kMaxSettingsBytes)) {
            error = "The legacy style header or palette data is invalid.";
            return false;
        }

        parsed.objects.reserve(fields.size() - 4);
        size_t decodedObjectBytes = 0;
        for (size_t i = 4; i < fields.size(); ++i) {
            std::string object;
            if (!base64Decode(fields[i], object, kMaxObjectBytes) || !validObjectRecord(object)) {
                error = "A decoration record in the legacy code is invalid.";
                return false;
            }
            decodedObjectBytes += object.size();
            if (decodedObjectBytes > kMaxPayloadBytes) {
                error = "Decoded legacy decoration data is too large.";
                return false;
            }
            parsed.objects.emplace_back(std::move(object));
        }

        data = std::move(parsed);
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
        if (fields.empty()) {
            error = "The pasted style code is empty.";
            return false;
        }

        if (fields[0] == kLegacyCodeMagic) {
            return decodeLegacyStyle(fields, data, error);
        }
        const bool previousFormat = fields[0] == kPreviousCodeMagic;
        if ((!previousFormat && fields[0] != kCodeMagic) || fields.size() != 2) {
            error = "Unsupported style code. Scan again with Style Scanner 1.1.0.";
            return false;
        }

        const std::string compressed = cocos2d::ZipUtils::base64URLDecode(std::string(fields[1]));
        if (compressed.empty()) {
            error = "The compressed style code is invalid.";
            return false;
        }

        const std::string payload = cocos2d::ZipUtils::decompressString(compressed, false, 0);
        if (payload.empty() || payload.size() > kMaxPayloadBytes) {
            error = "The style archive is damaged or exceeds the 64 MiB import limit.";
            return false;
        }

        std::string_view view(payload);
        size_t cursor = 0;
        StyleData parsed;
        if (!readField(view, cursor, parsed.levelName, kMaxNameBytes) ||
            !readField(view, cursor, parsed.creatorName, kMaxNameBytes) ||
            !readField(view, cursor, parsed.settings, kMaxSettingsBytes)) {
            error = "The style header or visual settings are invalid.";
            return false;
        }

        uint32_t count = 0;
        if (!readU32(view, cursor, count) || count > kMaxObjects) {
            error = "The style archive contains too many decoration templates.";
            return false;
        }

        size_t totalItems = 0;
        if (previousFormat) {
            parsed.objects.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                std::string object;
                if (!readField(view, cursor, object, kMaxObjectBytes) ||
                    !validObjectRecord(object)) {
                    error = "An old-format object record is invalid.";
                    return false;
                }
                parsed.objects.emplace_back(std::move(object));
            }
        } else {
            parsed.templates.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                DecoTemplate styleTemplate;
                uint32_t itemCount = 0;
                if (!readU32(view, cursor, styleTemplate.anchorType) ||
                    !readFloat(view, cursor, styleTemplate.anchorWidth) ||
                    !readFloat(view, cursor, styleTemplate.anchorHeight) ||
                    !readU32(view, cursor, itemCount) ||
                    itemCount > kMaxObjects ||
                    styleTemplate.anchorWidth <= 0.f ||
                    styleTemplate.anchorHeight <= 0.f ||
                    styleTemplate.anchorWidth > 10000.f ||
                    styleTemplate.anchorHeight > 10000.f) {
                    error = "A block decoration template is invalid.";
                    return false;
                }

                const bool validAnchor =
                    styleTemplate.anchorType == static_cast<uint32_t>(GameObjectType::Solid) ||
                    styleTemplate.anchorType == static_cast<uint32_t>(GameObjectType::Slope) ||
                    styleTemplate.anchorType == static_cast<uint32_t>(GameObjectType::Breakable);
                if (!validAnchor) {
                    error = "The archive contains an unsupported block type.";
                    return false;
                }

                if (totalItems + itemCount > kMaxObjects) {
                    error = "The style archive contains too many decoration pieces.";
                    return false;
                }
                totalItems += itemCount;
                styleTemplate.items.reserve(itemCount);

                for (uint32_t j = 0; j < itemCount; ++j) {
                    DecoItem item;
                    if (!readField(view, cursor, item.record, kMaxObjectBytes) ||
                        !validObjectRecord(item.record) ||
                        !readFloat(view, cursor, item.offsetX) ||
                        !readFloat(view, cursor, item.offsetY) ||
                        std::abs(item.offsetX) > 100000.f ||
                        std::abs(item.offsetY) > 100000.f) {
                        error = "A decoration piece in the style archive is invalid.";
                        return false;
                    }
                    styleTemplate.items.emplace_back(std::move(item));
                }
                parsed.templates.emplace_back(std::move(styleTemplate));
            }
        }

        if (cursor != view.size()) {
            error = "The style archive contains unexpected trailing data.";
            return false;
        }

        data = std::move(parsed);
        return true;
    }

    bool readStyleHeader(
        std::string_view rawCode,
        std::string& levelName,
        std::string& creatorName
    ) {
        StyleData parsed;
        std::string error;
        if (!decodeStyle(rawCode, parsed, error)) return false;
        levelName = std::move(parsed.levelName);
        creatorName = std::move(parsed.creatorName);
        return true;
    }

    bool scanLayer(GJBaseGameLayer* source, StyleData& output, std::string& error) {
        if (!source || !source->m_level) {
            error = "No active Geometry Dash editor level was found.";
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
            error = "The background / palette settings exceed the export size limit.";
            return false;
        }
        if (!source->m_objects) return true;

        struct SourceAnchor {
            GameObject* object = nullptr;
            cocos2d::CCPoint position;
            float width = 30.f;
            float height = 30.f;
            std::vector<DecoItem> items;
        };

        std::vector<SourceAnchor> anchors;
        std::vector<GameObject*> decorations;
        const unsigned int objectCount = source->m_objects->count();
        anchors.reserve(std::min<size_t>(objectCount, 20000));
        decorations.reserve(std::min<size_t>(objectCount, 20000));

        for (unsigned int i = 0; i < objectCount; ++i) {
            auto* object = static_cast<GameObject*>(source->m_objects->objectAtIndex(i));
            if (!object) continue;

            if (isAnchorBlock(object)) {
                SourceAnchor anchor;
                anchor.object = object;
                anchor.position = object->getPosition();
                anchor.width = safeObjectWidth(object);
                anchor.height = safeObjectHeight(object);
                anchors.emplace_back(std::move(anchor));
            } else if (isDecorationPart(object)) {
                decorations.push_back(object);
            }
        }

        if (anchors.empty() || decorations.empty()) {
            return true; // Background / palette can still be transferred on its own.
        }

        size_t totalItems = 0;
        size_t serializedBytes = 0;
        for (auto* decoration : decorations) {
            if (!decoration) continue;

            const auto position = decoration->getPosition();
            size_t bestIndex = anchors.size();
            float bestDistance = std::numeric_limits<float>::max();

            for (size_t i = 0; i < anchors.size(); ++i) {
                auto const& anchor = anchors[i];
                const float halfW = anchor.width * 0.5f;
                const float halfH = anchor.height * 0.5f;
                const float dx = std::max(0.f, std::abs(position.x - anchor.position.x) - halfW);
                const float dy = std::max(0.f, std::abs(position.y - anchor.position.y) - halfH);
                const float distance = std::hypot(dx, dy);

                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                }
            }

            if (bestIndex >= anchors.size()) continue;
            auto const& bestAnchor = anchors[bestIndex];
            const float attachRange = std::max(
                88.f,
                std::max(bestAnchor.width, bestAnchor.height) * 0.9f + 36.f
            );
            if (bestDistance > attachRange) continue;

            // Serialize only the decorative piece, not its source block.
            // getSaveString is called exclusively on objects belonging to the
            // open LevelEditorLayer, never on live PlayLayer objects.
            const auto saved = decoration->getSaveString(source);
            std::string record = saved.c_str();
            if (record.empty() || !validObjectRecord(record)) continue;
            if (record.size() > kMaxObjectBytes ||
                totalItems >= kMaxObjects ||
                serializedBytes > kMaxPayloadBytes - record.size() - 4) {
                error = "The level exceeds the 50,000 decoration-piece / 64 MiB export limits.";
                return false;
            }

            DecoItem item;
            item.record = std::move(record);
            item.offsetX = position.x - bestAnchor.position.x;
            item.offsetY = position.y - bestAnchor.position.y;
            anchors[bestIndex].items.emplace_back(std::move(item));
            serializedBytes += anchors[bestIndex].items.back().record.size() + 12;
            ++totalItems;
        }

        output.templates.reserve(std::min<size_t>(anchors.size(), kMaxObjects));
        for (auto& anchor : anchors) {
            if (anchor.items.empty()) continue;

            DecoTemplate styleTemplate;
            styleTemplate.anchorType = static_cast<uint32_t>(anchor.object->m_objectType);
            styleTemplate.anchorWidth = anchor.width;
            styleTemplate.anchorHeight = anchor.height;
            styleTemplate.items = std::move(anchor.items);
            output.templates.emplace_back(std::move(styleTemplate));
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
        return !text.empty() && geode::utils::clipboard::write(text);
    }

    std::string readFromClipboard() {
        return geode::utils::clipboard::read();
    }
}

class StyleScannerPopup final : public geode::Popup {
protected:
    geode::TextInput* m_exportInput = nullptr;
    std::string m_exportCode;
    std::string m_importCode;
    StyleData m_importData;
    cocos2d::CCLabelBMFont* m_importPreview = nullptr;
    cocos2d::CCLabelBMFont* m_metadataLabel = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;

    ~StyleScannerPopup() override {
        if (g_popup == this) g_popup = nullptr;
    }

    bool init() override {
        constexpr float popupWidth = 400.f;
        constexpr float popupHeight = 340.f;

        if (!geode::Popup::init(popupWidth, popupHeight)) return false;

        auto const winSize = CCDirector::get()->getWinSize();
        m_mainLayer->setPosition(winSize / 2.f);

        const float fitScale = std::min(
            (winSize.width - 20.f) / popupWidth,
            (winSize.height - 20.f) / popupHeight
        );
        const float uiScale = std::clamp(fitScale, 0.48f, 0.78f);
        m_mainLayer->setScale(uiScale);

        this->setTitle("Style Scanner", "goldFont.fnt", 0.52f, 22.f);

        // Pin the close button to the actual popup bounds and draw it above
        // every other control so the X stays visible on small windows.
        if (m_closeBtn) {
            auto const size = m_mainLayer->getContentSize();
            m_closeBtn->setPosition({size.width - 17.f, size.height - 17.f});
            m_closeBtn->setScale(0.50f);
            m_closeBtn->setZOrder(10000);
        }

        auto* exportTitle = cocos2d::CCLabelBMFont::create(
            "1. SCAN / EXPORT VISUAL STYLE", "goldFont.fnt"
        );
        if (!exportTitle) return false;
        exportTitle->setScale(0.43f);
        m_mainLayer->addChildAtPosition(exportTitle, geode::Anchor::Center, {0.f, 127.f});

        m_exportInput = geode::TextInput::create(340.f, "Scan a level to prepare its code", "bigFont.fnt");
        if (!m_exportInput) return false;
        m_exportInput->setCommonFilter(geode::CommonFilter::Any);
        m_exportInput->setMaxCharCount(120);
        m_exportInput->setTextAlign(geode::TextInputAlign::Left);
        m_mainLayer->addChildAtPosition(m_exportInput, geode::Anchor::Center, {0.f, 91.f});
        m_exportInput->setEnabled(true);

        auto* scanButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Scan Level", "goldFont.fnt", "GJ_button_01.png", 0.62f),
            this,
            menu_selector(StyleScannerPopup::onScan)
        );
        if (!scanButton) return false;
        scanButton->setPosition({popupWidth / 2.f - 78.f, popupHeight / 2.f + 43.f});
        m_buttonMenu->addChild(scanButton);

        auto* copyButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Copy Code", "goldFont.fnt", "GJ_button_02.png", 0.62f),
            this,
            menu_selector(StyleScannerPopup::onCopy)
        );
        if (!copyButton) return false;
        copyButton->setPosition({popupWidth / 2.f + 78.f, popupHeight / 2.f + 43.f});
        m_buttonMenu->addChild(copyButton);

        m_metadataLabel = cocos2d::CCLabelBMFont::create(
            "Source: [None]  |  Creator: [None]", "chatFont.fnt"
        );
        if (!m_metadataLabel) return false;
        m_metadataLabel->setScale(0.38f);
        m_metadataLabel->limitLabelWidth(350.f, 0.38f, 0.2f);
        m_mainLayer->addChildAtPosition(m_metadataLabel, geode::Anchor::Center, {0.f, 5.f});

        auto* importTitle = cocos2d::CCLabelBMFont::create(
            "2. IMPORT STYLE", "goldFont.fnt"
        );
        if (!importTitle) return false;
        importTitle->setScale(0.43f);
        m_mainLayer->addChildAtPosition(importTitle, geode::Anchor::Center, {0.f, -42.f});

        m_importPreview = cocos2d::CCLabelBMFont::create(
            "No style code loaded. Copy code, then paste from clipboard.", "chatFont.fnt"
        );
        if (!m_importPreview) return false;
        m_importPreview->setScale(0.38f);
        m_importPreview->limitLabelWidth(350.f, 0.38f, 0.2f);
        m_mainLayer->addChildAtPosition(m_importPreview, geode::Anchor::Center, {0.f, -76.f});

        auto* pasteButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste Code", "goldFont.fnt", "GJ_button_01.png", 0.60f),
            this,
            menu_selector(StyleScannerPopup::onPasteFromClipboard)
        );
        if (!pasteButton) return false;
        pasteButton->setPosition({popupWidth / 2.f - 82.f, popupHeight / 2.f - 105.f});
        m_buttonMenu->addChild(pasteButton);

        auto* applyButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Apply Style", "goldFont.fnt", "GJ_button_03.png", 0.60f),
            this,
            menu_selector(StyleScannerPopup::onApply)
        );
        if (!applyButton) return false;
        applyButton->setPosition({popupWidth / 2.f + 82.f, popupHeight / 2.f - 105.f});
        m_buttonMenu->addChild(applyButton);

        m_statusLabel = cocos2d::CCLabelBMFont::create(
            "Open a level in the editor, then press Scan.", "chatFont.fnt"
        );
        if (!m_statusLabel) return false;
        m_statusLabel->setScale(0.38f);
        m_statusLabel->limitLabelWidth(350.f, 0.38f, 0.2f);
        m_mainLayer->addChildAtPosition(m_statusLabel, geode::Anchor::Center, {0.f, -140.f});

        return true;
    }

    void setStatus(std::string const& message, bool isError = false) {
        if (!m_statusLabel) return;
        m_statusLabel->setString(message.c_str());
        m_statusLabel->setScale(0.38f);
        m_statusLabel->setColor(isError ? cocos2d::ccc3(255, 105, 105) : cocos2d::ccc3(150, 255, 150));
        m_statusLabel->limitLabelWidth(350.f, 0.38f, 0.2f);
    }

public:
    static StyleScannerPopup* create() {
        auto* popup = new StyleScannerPopup();
        if (popup && popup->init()) {
            popup->autorelease();
            return popup;
        }
        CC_SAFE_DELETE(popup);
        return nullptr;
    }

    void onScan(cocos2d::CCObject*) {
        // Geometry Dash's own object-copy path serializes objects from
        // LevelEditorLayer. Never run getSaveString on live PlayLayer objects.
        auto* editor = LevelEditorLayer::get();
        if (!editor) {
            this->setStatus(
                "Open this level in the Level Editor before scanning. Play Mode scanning is disabled for safety.",
                true
            );
            return;
        }

        StyleData data;
        std::string error;
        if (!scanLayer(static_cast<GJBaseGameLayer*>(editor), data, error)) {
            this->setStatus(error, true);
            return;
        }

        std::string code;
        if (!encodeStyle(data, code, error)) {
            this->setStatus(error, true);
            return;
        }

        m_exportCode = code;
        // The export input is a compact preview; the full multi-kilobyte code
        // remains stored separately and is copied by the Copy Code button.
        constexpr size_t previewLength = 90;
        std::string preview = code.substr(0, previewLength);
        if (code.size() > previewLength) preview += "...";
        m_exportInput->setEnabled(true);
        m_exportInput->setString(preview);
        const std::string label =
            "Source Style Name: " + (data.levelName.empty() ? "[Unnamed]" : data.levelName) +
            "  |  Original Creator: " +
            (data.creatorName.empty() ? "[Unavailable]" : data.creatorName);
        m_metadataLabel->setString(label.c_str());
        m_metadataLabel->limitLabelWidth(385.f, 0.43f, 0.2f);

        size_t pieceCount = 0;
        for (auto const& styleTemplate : data.templates) {
            pieceCount += styleTemplate.items.size();
        }
        this->setStatus(
            "SCAN OK | " + std::to_string(data.templates.size()) +
            " block patterns / " + std::to_string(pieceCount) +
            " deco pieces + background settings. Tap Copy Code."
        );
    }

    void onCopy(cocos2d::CCObject*) {
        const std::string code = m_exportCode;
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

    void onPasteFromClipboard(cocos2d::CCObject*) {
        const std::string code = readFromClipboard();
        if (code.empty()) {
            this->setStatus("Clipboard is empty. Copy a StyleScanner code first.", true);
            return;
        }

        StyleData data;
        std::string error;
        if (!decodeStyle(code, data, error)) {
            this->setStatus(error, true);
            return;
        }
        if (!data.objects.empty()) {
            this->setStatus(
                "This is an old whole-object code. Rescan the source level with Style Scanner 1.1.0 so only deco patterns are exported.",
                true
            );
            return;
        }

        m_importCode = code;
        m_importData = std::move(data);

        std::string preview = code.substr(0, 72);
        if (code.size() > 72) preview += "...";
        m_importPreview->setString(preview.c_str());
        m_importPreview->limitLabelWidth(350.f, 0.38f, 0.2f);

        std::string levelName = m_importData.levelName.empty() ? "[Unnamed]" : m_importData.levelName;
        std::string creatorName = m_importData.creatorName.empty() ? "[Unavailable]" : m_importData.creatorName;
        const std::string label =
            "Source: " + levelName + " | Creator: " + creatorName;
        m_metadataLabel->setString(label.c_str());
        m_metadataLabel->limitLabelWidth(350.f, 0.38f, 0.2f);

        size_t pieceCount = 0;
        for (auto const& styleTemplate : m_importData.templates) {
            pieceCount += styleTemplate.items.size();
        }
        this->setStatus(
            "STYLE READY | " + std::to_string(m_importData.templates.size()) +
            " block patterns / " + std::to_string(pieceCount) +
            " deco pieces + background. Open your target level and Apply."
        );
    }

    void onApply(cocos2d::CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (!editor) {
            this->setStatus("Open your target level in the editor before applying a style.", true);
            return;
        }

        if (m_importCode.empty()) {
            this->setStatus("Press Paste Code first to load a style from the clipboard.", true);
            return;
        }

        auto const& data = m_importData;
        size_t addedPieces = 0;
        size_t matchedBlocks = 0;
        size_t skippedDecoratedBlocks = 0;

        if (!data.templates.empty()) {
            struct TargetBlock {
                GameObject* object = nullptr;
                cocos2d::CCPoint position;
                float width = 30.f;
                float height = 30.f;
            };
            struct Placement {
                cocos2d::CCPoint anchorPosition;
                float offsetX = 0.f;
                float offsetY = 0.f;
                float scale = 1.f;
            };

            std::vector<TargetBlock> targetBlocks;
            std::vector<GameObject*> existingDecorations;
            if (editor->m_objects) {
                const unsigned int count = editor->m_objects->count();
                targetBlocks.reserve(std::min<size_t>(count, 20000));
                existingDecorations.reserve(std::min<size_t>(count, 20000));

                for (unsigned int i = 0; i < count; ++i) {
                    auto* object = static_cast<GameObject*>(editor->m_objects->objectAtIndex(i));
                    if (!object) continue;

                    if (isAnchorBlock(object)) {
                        targetBlocks.push_back({
                            object,
                            object->getPosition(),
                            safeObjectWidth(object),
                            safeObjectHeight(object)
                        });
                    } else if (isDecorationPart(object)) {
                        existingDecorations.push_back(object);
                    }
                }
            }

            if (targetBlocks.empty()) {
                this->setStatus("No regular blocks were found in your target level to attach the deco to.", true);
                return;
            }

            std::string serializedPieces;
            std::vector<Placement> placements;
            size_t totalBytes = 0;

            for (auto const& target : targetBlocks) {
                // Do not stack a second source pattern on top of blocks that
                // already have decoration pieces around them.
                bool alreadyDecorated = false;
                for (auto* decoration : existingDecorations) {
                    if (!decoration) continue;
                    const auto pos = decoration->getPosition();
                    if (std::abs(pos.x - target.position.x) <= target.width * 0.5f + 18.f &&
                        std::abs(pos.y - target.position.y) <= target.height * 0.5f + 18.f) {
                        alreadyDecorated = true;
                        break;
                    }
                }
                if (alreadyDecorated) {
                    ++skippedDecoratedBlocks;
                    continue;
                }

                const uint32_t targetType = static_cast<uint32_t>(target.object->m_objectType);
                const DecoTemplate* bestTemplate = nullptr;
                float bestScore = std::numeric_limits<float>::max();

                for (auto const& candidate : data.templates) {
                    if (candidate.items.empty()) continue;
                    const float ratioX = target.width / candidate.anchorWidth;
                    const float ratioY = target.height / candidate.anchorHeight;
                    if (!std::isfinite(ratioX) || !std::isfinite(ratioY) ||
                        ratioX <= 0.f || ratioY <= 0.f) continue;

                    float score = std::abs(std::log(ratioX)) +
                                  std::abs(std::log(ratioY));
                    if (candidate.anchorType != targetType) score += 0.9f;
                    // Prefer a reusable compact pattern when two patterns fit
                    // the target block equally well.
                    score += static_cast<float>(candidate.items.size()) * 0.0001f;

                    if (score < bestScore) {
                        bestScore = score;
                        bestTemplate = &candidate;
                    }
                }

                if (!bestTemplate) continue;

                const float ratioX = target.width / bestTemplate->anchorWidth;
                const float ratioY = target.height / bestTemplate->anchorHeight;
                const float fitScale = std::clamp(
                    std::min(ratioX, ratioY), 0.55f, 1.6f
                );

                for (auto const& item : bestTemplate->items) {
                    totalBytes += item.record.size() + 1;
                    if (totalBytes > kMaxPayloadBytes || placements.size() >= kMaxObjects) {
                        this->setStatus("The matching decoration pieces exceed the safe import limit.", true);
                        return;
                    }

                    serializedPieces += item.record;
                    serializedPieces.push_back(';');
                    placements.push_back({
                        target.position,
                        item.offsetX,
                        item.offsetY,
                        fitScale
                    });
                }
                ++matchedBlocks;
            }

            if (!serializedPieces.empty()) {
                auto* created = editor->createObjectsFromString(serializedPieces, false, false);
                if (!created) {
                    this->setStatus("Geometry Dash could not create the matched decoration pieces.", true);
                    return;
                }

                const unsigned int createdCount = created->count();
                const size_t applyCount = std::min<size_t>(createdCount, placements.size());
                for (size_t i = 0; i < applyCount; ++i) {
                    auto* object = static_cast<GameObject*>(created->objectAtIndex(static_cast<unsigned int>(i)));
                    if (!object) continue;

                    auto const& placement = placements[i];
                    object->setPosition({
                        placement.anchorPosition.x + placement.offsetX * placement.scale,
                        placement.anchorPosition.y + placement.offsetY * placement.scale
                    });
                    object->setScaleX(object->getScaleX() * placement.scale);
                    object->setScaleY(object->getScaleY() * placement.scale);

                    // Some decorative pieces use solid/slope sprites. Keep
                    // those visual-only so no collision gameplay is imported.
                    if (object->m_objectType == GameObjectType::Solid ||
                        object->m_objectType == GameObjectType::Slope ||
                        object->m_objectType == GameObjectType::Breakable) {
                        object->m_isNoTouch = true;
                    }
                    ++addedPieces;
                }
            }
        }

        const bool settingsApplied = applyVisualSettings(editor, data.settings);
        if (addedPieces == 0 && !settingsApplied) {
            this->setStatus(
                "No matching undecorated blocks were found. Background settings were not changed.",
                true
            );
            return;
        }

        std::string result =
            "Added " + std::to_string(addedPieces) + " deco pieces to " +
            std::to_string(matchedBlocks) + " matching blocks";
        if (skippedDecoratedBlocks > 0) {
            result += " | skipped " + std::to_string(skippedDecoratedBlocks) +
                      " blocks that already had deco";
        }
        if (settingsApplied) result += " | background / ground / palette copied";
        result += ". Your original map geometry, gameplay and music stay unchanged.";
        this->setStatus(result);
    }

    void onClose(cocos2d::CCObject* sender) override {
        if (g_popup == this) g_popup = nullptr;
        geode::Popup::onClose(sender);
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

    geode::ListenerHandle g_keyboardListener;

}

$on_game(Loaded) {
    g_keyboardListener = geode::KeyboardInputEvent(cocos2d::KEY_F3).listen(
        [](geode::KeyboardInputData& data) {
            // Handle only the initial press. Repeated key-down events must not
            // toggle the popup more than once while F3 is held.
            if (data.action == geode::KeyboardInputData::Action::Press) {
                toggleStyleScanner();
            }
            return false; // Let Geometry Dash and other mods receive the key.
        }
    );
}
