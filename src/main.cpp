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
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

class StyleScannerPopup;
class AutoLevelPopup;

namespace {
    constexpr std::string_view kCodeMagic = "STY5";
    constexpr std::string_view kPreviousCodeMagicV4 = "STY4";
    constexpr std::string_view kPreviousTemplateMagic = "STY3";
    constexpr std::string_view kPreviousCodeMagic = "STY2";
    constexpr std::string_view kLegacyCodeMagic = "STY1";
    // Larger imports are bounded and parsed in small batches to avoid
    // building one enormous Geometry Dash object string in memory.
    constexpr size_t kMaxCodeBytes = 32 * 1024 * 1024;
    constexpr size_t kMaxPayloadBytes = 128 * 1024 * 1024;
    constexpr size_t kMaxSettingsBytes = 1024 * 1024;
    constexpr size_t kMaxObjectBytes = 64 * 1024;
    constexpr size_t kMaxObjects = 250000;
    constexpr size_t kMaxTemplates = 512;
    constexpr size_t kImportBatchObjects = 2000;
    constexpr size_t kImportBatchBytes = 2 * 1024 * 1024;
    constexpr size_t kMaxNameBytes = 4096;

    struct DecoItem {
        std::string record;
        float offsetX = 0.f;
        float offsetY = 0.f;
    };

    struct DecoTemplate {
        uint32_t anchorType = 0;
        uint32_t anchorObjectID = 0;
        float anchorWidth = 30.f;
        float anchorHeight = 30.f;
        std::vector<DecoItem> items;
    };

    // A static snapshot of a custom color channel used by copied decoration.
    // Color Trigger objects themselves are never exported or imported.
    struct PaletteColor {
        uint32_t channelID = 0;
        uint8_t r = 255;
        uint8_t g = 255;
        uint8_t b = 255;
        float opacity = 1.f;
        bool blending = false;
    };

    struct StyleData {
        std::string levelName;
        std::string creatorName;
        std::string settings;
        // Used only when decoding old STY1/STY2 codes. New exports store
        // decoration templates relative to source blocks instead of whole maps.
        std::vector<std::string> objects;
        std::vector<DecoTemplate> templates;
        std::vector<PaletteColor> colors;
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

    bool isSupportedStyleAnchorType(uint32_t type) {
        switch (static_cast<GameObjectType>(type)) {
            case GameObjectType::Solid:
            case GameObjectType::Slope:
            case GameObjectType::Breakable:
            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard:
                return true;
            default:
                return false;
        }
    }

    bool isAnchorBlock(GameObject* object) {
        if (!object || object->m_objectID == 749) return false;
        if (object->m_classType == GameObjectClassType::Effect ||
            isDecorationPart(object)) return false;
        return isSupportedStyleAnchorType(static_cast<uint32_t>(object->m_objectType));
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

    uint64_t spatialKey(int32_t cellX, int32_t cellY) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32) |
               static_cast<uint32_t>(cellY);
    }

    uint64_t spatialKey(cocos2d::CCPoint position, float cellSize = 64.f) {
        const auto cellX = static_cast<int32_t>(std::floor(position.x / cellSize));
        const auto cellY = static_cast<int32_t>(std::floor(position.y / cellSize));
        return spatialKey(cellX, cellY);
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
            data.templates.size() > kMaxTemplates ||
            data.colors.size() > 999) {
            error = "The style archive exceeds its safe export limits.";
            return false;
        }

        size_t totalItems = 0;
        for (auto const& styleTemplate : data.templates) {
            totalItems += styleTemplate.items.size();
            if (totalItems > kMaxObjects ||
                styleTemplate.items.size() > kMaxObjects ||
                styleTemplate.anchorObjectID > 100000 ||
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
            appendU32(payload, styleTemplate.anchorObjectID);
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

        // Store only static RGB snapshots for custom channels used by the deco.
        appendU32(payload, static_cast<uint32_t>(data.colors.size()));
        for (auto const& color : data.colors) {
            if (color.channelID < 1 || color.channelID > 999 ||
                !std::isfinite(color.opacity) || color.opacity < 0.f || color.opacity > 1.f) {
                error = "A custom color palette entry is invalid.";
                return false;
            }
            appendU32(payload, color.channelID);
            payload.push_back(static_cast<char>(color.r));
            payload.push_back(static_cast<char>(color.g));
            payload.push_back(static_cast<char>(color.b));
            appendFloat(payload, color.opacity);
            appendU32(payload, color.blending ? 1u : 0u);
            if (payload.size() > kMaxPayloadBytes) {
                error = "The style exceeds the 64 MiB uncompressed export limit.";
                return false;
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
        const bool previousTemplateFormat = fields[0] == kPreviousTemplateMagic;
        const bool previousV4Format = fields[0] == kPreviousCodeMagicV4;
        const bool paletteFormat = fields[0] == kCodeMagic;
        if ((!previousFormat && !previousTemplateFormat && !previousV4Format && !paletteFormat) ||
            fields.size() != 2) {
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
        const uint32_t maxEntryCount = previousFormat
            ? static_cast<uint32_t>(kMaxObjects)
            : static_cast<uint32_t>(kMaxTemplates);
        if (!readU32(view, cursor, count) || count > maxEntryCount) {
            error = "The style archive contains too many decoration templates or objects.";
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
                    (!previousTemplateFormat && !readU32(view, cursor, styleTemplate.anchorObjectID)) ||
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

                const bool validAnchor = isSupportedStyleAnchorType(styleTemplate.anchorType);
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

        if (paletteFormat) {
            uint32_t colorCount = 0;
            if (!readU32(view, cursor, colorCount) || colorCount > 999) {
                error = "The style archive contains an invalid custom color count.";
                return false;
            }
            parsed.colors.reserve(colorCount);
            for (uint32_t i = 0; i < colorCount; ++i) {
                PaletteColor color;
                uint32_t blending = 0;
                if (!readU32(view, cursor, color.channelID) ||
                    cursor > view.size() || view.size() - cursor < 3) {
                    error = "A custom color palette entry is truncated.";
                    return false;
                }
                color.r = static_cast<uint8_t>(view[cursor++]);
                color.g = static_cast<uint8_t>(view[cursor++]);
                color.b = static_cast<uint8_t>(view[cursor++]);
                if (!readFloat(view, cursor, color.opacity) ||
                    !readU32(view, cursor, blending) ||
                    color.channelID < 1 || color.channelID > 999 ||
                    color.opacity < 0.f || color.opacity > 1.f || blending > 1) {
                    error = "A custom color palette entry is invalid.";
                    return false;
                }
                color.blending = blending != 0;
                parsed.colors.emplace_back(color);
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
        std::unordered_set<uint32_t> neededColorIDs;
        const unsigned int objectCount = source->m_objects->count();
        anchors.reserve(std::min<size_t>(objectCount, 20000));
        decorations.reserve(std::min<size_t>(objectCount, 20000));
        std::unordered_map<uint64_t, std::vector<size_t>> anchorGrid;
        float maxAnchorExtent = 30.f;

        for (unsigned int i = 0; i < objectCount; ++i) {
            auto* object = static_cast<GameObject*>(source->m_objects->objectAtIndex(i));
            if (!object) continue;

            if (isAnchorBlock(object)) {
                SourceAnchor anchor;
                anchor.object = object;
                anchor.position = object->getPosition();
                anchor.width = safeObjectWidth(object);
                anchor.height = safeObjectHeight(object);
                maxAnchorExtent = std::max(maxAnchorExtent, std::max(anchor.width, anchor.height));
                const size_t index = anchors.size();
                anchorGrid[spatialKey(anchor.position)].push_back(index);
                anchors.emplace_back(std::move(anchor));
            } else if (isDecorationPart(object)) {
                decorations.push_back(object);
            }
        }

        if (anchors.empty()) return true;

        // A 64-unit spatial grid keeps large levels from doing an O(blocks × deco)
        // full scan. The radius expands for unusually large source blocks.
        const int cellRadius = std::clamp(
            static_cast<int>(std::ceil((maxAnchorExtent * 0.5f + 160.f) / 64.f)),
            1, 8
        );

        size_t totalItems = 0;
        size_t serializedBytes = 0;
        for (auto* decoration : decorations) {
            if (!decoration) continue;
            const auto position = decoration->getPosition();
            const auto cellX = static_cast<int32_t>(std::floor(position.x / 64.f));
            const auto cellY = static_cast<int32_t>(std::floor(position.y / 64.f));

            size_t bestIndex = anchors.size();
            float bestDistance = std::numeric_limits<float>::max();

            for (int x = -cellRadius; x <= cellRadius; ++x) {
                for (int y = -cellRadius; y <= cellRadius; ++y) {
                    const auto found = anchorGrid.find(spatialKey(cellX + x, cellY + y));
                    if (found == anchorGrid.end()) continue;

                    for (const size_t index : found->second) {
                        auto const& anchor = anchors[index];
                        const float halfW = anchor.width * 0.5f;
                        const float halfH = anchor.height * 0.5f;
                        const float dx = std::max(0.f, std::abs(position.x - anchor.position.x) - halfW);
                        const float dy = std::max(0.f, std::abs(position.y - anchor.position.y) - halfH);
                        const float distance = std::hypot(dx, dy);
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            bestIndex = index;
                        }
                    }
                }
            }

            if (bestIndex >= anchors.size()) continue;
            auto const& bestAnchor = anchors[bestIndex];
            const float attachRange = std::clamp(
                std::max(bestAnchor.width, bestAnchor.height) * 0.9f + 36.f,
                88.f, 160.f
            );
            if (bestDistance > attachRange) continue;

            // Serialize only the decorative piece, never the source block.
            const auto saved = decoration->getSaveString(source);
            std::string record = saved.c_str();
            if (record.empty() || !validObjectRecord(record)) continue;
            if (record.size() > kMaxObjectBytes ||
                totalItems >= kMaxObjects ||
                serializedBytes > kMaxPayloadBytes - record.size() - 4) {
                error = "The level exceeds the 50,000 decoration-piece / 64 MiB export limits.";
                return false;
            }

            // Remember the custom channels referenced by this exact deco piece.
            // Built-in channels (background, ground, player colors) stay in level settings.
            if (decoration->m_baseColor &&
                decoration->m_baseColor->m_colorID >= 1 &&
                decoration->m_baseColor->m_colorID <= 999) {
                neededColorIDs.insert(static_cast<uint32_t>(decoration->m_baseColor->m_colorID));
            }
            if (decoration->m_detailColor &&
                decoration->m_detailColor->m_colorID >= 1 &&
                decoration->m_detailColor->m_colorID <= 999) {
                neededColorIDs.insert(static_cast<uint32_t>(decoration->m_detailColor->m_colorID));
            }

            DecoItem item;
            item.record = std::move(record);
            item.offsetX = position.x - bestAnchor.position.x;
            item.offsetY = position.y - bestAnchor.position.y;
            anchors[bestIndex].items.emplace_back(std::move(item));
            serializedBytes += anchors[bestIndex].items.back().record.size() + 12;
            ++totalItems;
        }

        // Keep one representative deco pattern for each block ID + shape.
        // This avoids storing thousands of near-identical templates and makes
        // target matching fast while retaining the richest nearby pattern.
        std::unordered_map<std::string, size_t> templateByShape;
        output.templates.reserve(std::min<size_t>(anchors.size(), kMaxTemplates));
        for (auto& anchor : anchors) {
            if (anchor.items.empty()) continue;

            DecoTemplate styleTemplate;
            styleTemplate.anchorType = static_cast<uint32_t>(anchor.object->m_objectType);
            styleTemplate.anchorObjectID = static_cast<uint32_t>(anchor.object->m_objectID);
            styleTemplate.anchorWidth = anchor.width;
            styleTemplate.anchorHeight = anchor.height;
            styleTemplate.items = std::move(anchor.items);

            // Source and target numeric IDs may differ while representing
            // the same visual family (e.g. alternate large-spike variants).
            // Group templates by object type and base dimensions instead.
            const std::string key =
                std::to_string(styleTemplate.anchorType) + ":" +
                std::to_string(static_cast<int>(std::lround(styleTemplate.anchorWidth / 4.f))) + ":" +
                std::to_string(static_cast<int>(std::lround(styleTemplate.anchorHeight / 4.f)));

            const auto found = templateByShape.find(key);
            if (found != templateByShape.end()) {
                auto& existing = output.templates[found->second];
                if (styleTemplate.items.size() > existing.items.size()) {
                    existing = std::move(styleTemplate);
                }
                continue;
            }

            if (output.templates.size() >= kMaxTemplates) continue;
            templateByShape.emplace(key, output.templates.size());
            output.templates.emplace_back(std::move(styleTemplate));
        }

        // Read only the first (leftmost) static color definition for each custom
        // channel referenced by copied decoration. Trigger objects themselves
        // are deliberately not stored, moved, or imported into the target level.
        if (!neededColorIDs.empty()) {
            std::unordered_map<uint32_t, float> firstColorX;
            std::unordered_map<uint32_t, PaletteColor> colorsByChannel;
            for (unsigned int i = 0; i < objectCount; ++i) {
                auto* object = static_cast<GameObject*>(source->m_objects->objectAtIndex(i));
                if (!object || !object->isColorTrigger()) continue;

                const int channel = object->getColorIndex();
                if (channel < 1 || channel > 999 ||
                    !neededColorIDs.count(static_cast<uint32_t>(channel))) continue;

                const float x = object->getPosition().x;
                auto found = firstColorX.find(static_cast<uint32_t>(channel));
                if (found != firstColorX.end() && x >= found->second) continue;

                auto* trigger = static_cast<EffectGameObject*>(object);
                PaletteColor color;
                color.channelID = static_cast<uint32_t>(channel);
                color.r = trigger->m_triggerTargetColor.r;
                color.g = trigger->m_triggerTargetColor.g;
                color.b = trigger->m_triggerTargetColor.b;
                color.opacity = std::isfinite(trigger->m_opacity)
                    ? std::clamp(trigger->m_opacity, 0.f, 1.f) : 1.f;
                color.blending = trigger->m_usesBlending;

                firstColorX[color.channelID] = x;
                colorsByChannel[color.channelID] = color;
            }

            output.colors.reserve(colorsByChannel.size());
            for (uint32_t channel : neededColorIDs) {
                const auto found = colorsByChannel.find(channel);
                if (found != colorsByChannel.end()) {
                    output.colors.emplace_back(found->second);
                }
            }
        }

        return true;
    }

    size_t applyCustomPalette(LevelEditorLayer* editor, std::vector<PaletteColor> const& colors) {
        if (!editor || !editor->m_levelSettings ||
            !editor->m_levelSettings->m_effectManager || colors.empty()) return 0;

        auto* manager = editor->m_levelSettings->m_effectManager;
        size_t copied = 0;
        for (auto const& entry : colors) {
            if (entry.channelID < 1 || entry.channelID > 999) continue;
            const cocos2d::ccColor3B rgb{entry.r, entry.g, entry.b};
            const float opacity = std::clamp(entry.opacity, 0.f, 1.f);

            auto* action = manager->getColorAction(static_cast<int>(entry.channelID));
            if (!action) continue;

            action->m_colorID = static_cast<int>(entry.channelID);
            action->m_fromColor = rgb;
            action->m_toColor = rgb;
            action->m_color = rgb;
            action->m_currentOpacity = opacity;
            action->m_fromOpacity = opacity;
            action->m_toOpacity = opacity;
            action->m_duration = 0.f;
            action->m_playerColor = 0;
            action->m_blending = entry.blending;
            action->m_copyID = 0;
            action->m_copyOpacity = false;
            action->m_paused = false;
            manager->colorActionChanged(action);

            auto* sprite = manager->getColorSprite(static_cast<int>(entry.channelID));
            if (sprite) {
                sprite->m_color = rgb;
                sprite->m_copyColor = rgb;
                sprite->m_opacity = opacity * 255.f;
            }
            ++copied;
        }

        return copied;
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
        editor->updateLevelColors();
        return true;
    }


    // ---------------------------------------------------------------------
    // Procedural level generator + themed automatic decoration.
    // It only generates into an empty editor level and never deletes user data.
    // ---------------------------------------------------------------------
    struct GeneratedObject {
        int id = 1;
        float x = 0.f;
        float y = 105.f;
        float rotation = 0.f;
        float scaleX = 1.f;
        float scaleY = 1.f;
        bool anchor = false;
        bool decoration = false;
        int primaryColor = 0;
        int secondaryColor = 0;
        uint8_t opacity = 255;
    };

    struct GeneratorPalette {
        cocos2d::ccColor3B primary;
        cocos2d::ccColor3B accent;
        cocos2d::ccColor3B background;
        cocos2d::ccColor3B ground;
        cocos2d::ccColor3B line;
        cocos2d::ccColor3B middle;
    };

    LevelEditorLayer* g_generatedEditor = nullptr;
    std::vector<int> g_generatedAnchorIDs;
    bool g_generatedDecorated = false;
    uint32_t g_generatorSeed = 0x51A7E123u;

    GeneratorPalette paletteForTheme(int theme) {
        switch (theme) {
            case 1: // Inferno
                return {
                    cocos2d::ccc3(255, 103, 45), cocos2d::ccc3(255, 210, 70),
                    cocos2d::ccc3(28, 7, 13), cocos2d::ccc3(73, 20, 20),
                    cocos2d::ccc3(255, 123, 49), cocos2d::ccc3(104, 28, 35)
                };
            case 2: // Ice Crystal
                return {
                    cocos2d::ccc3(75, 210, 255), cocos2d::ccc3(205, 245, 255),
                    cocos2d::ccc3(5, 20, 48), cocos2d::ccc3(18, 54, 93),
                    cocos2d::ccc3(112, 224, 255), cocos2d::ccc3(27, 91, 133)
                };
            case 3: // Emerald Forest
                return {
                    cocos2d::ccc3(75, 220, 130), cocos2d::ccc3(197, 255, 116),
                    cocos2d::ccc3(5, 28, 21), cocos2d::ccc3(15, 67, 44),
                    cocos2d::ccc3(89, 242, 150), cocos2d::ccc3(23, 91, 55)
                };
            case 4: // Galaxy
                return {
                    cocos2d::ccc3(177, 92, 255), cocos2d::ccc3(255, 105, 205),
                    cocos2d::ccc3(17, 8, 43), cocos2d::ccc3(48, 23, 83),
                    cocos2d::ccc3(214, 133, 255), cocos2d::ccc3(73, 36, 111)
                };
            case 5: // Cyber
                return {
                    cocos2d::ccc3(36, 245, 218), cocos2d::ccc3(190, 255, 47),
                    cocos2d::ccc3(3, 24, 34), cocos2d::ccc3(10, 65, 70),
                    cocos2d::ccc3(42, 255, 237), cocos2d::ccc3(17, 93, 94)
                };
            default: // Neon City
                return {
                    cocos2d::ccc3(47, 211, 255), cocos2d::ccc3(202, 77, 255),
                    cocos2d::ccc3(7, 10, 36), cocos2d::ccc3(20, 28, 69),
                    cocos2d::ccc3(57, 235, 255), cocos2d::ccc3(39, 27, 91)
                };
        }
    }

    void setStaticColorChannel(
        GJEffectManager* manager,
        int channelID,
        cocos2d::ccColor3B color,
        float opacity = 1.f
    ) {
        if (!manager) return;
        auto* action = manager->getColorAction(channelID);
        if (!action) return;

        action->m_colorID = channelID;
        action->m_fromColor = color;
        action->m_toColor = color;
        action->m_color = color;
        action->m_currentOpacity = opacity;
        action->m_fromOpacity = opacity;
        action->m_toOpacity = opacity;
        action->m_duration = 0.f;
        action->m_playerColor = 0;
        action->m_blending = true;
        action->m_copyID = 0;
        action->m_copyOpacity = false;
        action->m_paused = false;
        manager->colorActionChanged(action);

        if (auto* sprite = manager->getColorSprite(channelID)) {
            sprite->m_color = color;
            sprite->m_copyColor = color;
            sprite->m_opacity = opacity * 255.f;
        }
    }

    void applyGeneratorPalette(LevelEditorLayer* editor, int theme) {
        if (!editor || !editor->m_levelSettings) return;
        auto* manager = editor->m_levelSettings->m_effectManager;
        const auto palette = paletteForTheme(theme);

        // Two custom channels are used by every generated block and deco piece.
        setStaticColorChannel(manager, 1, palette.primary);
        setStaticColorChannel(manager, 2, palette.accent);

        // Geometry Dash's reserved channels tint the background and ground layers.
        setStaticColorChannel(manager, 1000, palette.background);
        setStaticColorChannel(manager, 1001, palette.ground);
        setStaticColorChannel(manager, 1002, palette.line);
        setStaticColorChannel(manager, 1003, palette.middle);
        setStaticColorChannel(manager, 1004, palette.middle);

        editor->updateLevelColors();
        editor->levelSettingsUpdated();
        editor->updateLevelColors();
    }

    void applyObjectChannels(GameObject* object, int primary, int secondary) {
        if (!object) return;
        if (object->getMainColor()) {
            object->setMainColorMode(primary);
            object->updateMainColor();
        }
        if (object->getSecondaryColor()) {
            object->setSecondaryColorMode(secondary);
            object->updateSecondaryColor();
        }
    }

    int randomInt(std::mt19937& rng, int minimum, int maximum) {
        return std::uniform_int_distribution<int>(minimum, maximum)(rng);
    }

    void pushGenerated(
        std::vector<GeneratedObject>& output,
        int id,
        float x,
        float y,
        bool anchor = false,
        float rotation = 0.f
    ) {
        GeneratedObject object;
        object.id = id;
        object.x = x;
        object.y = y;
        object.anchor = anchor;
        object.rotation = rotation;
        if (anchor) {
            object.primaryColor = 1;
            object.secondaryColor = 2;
        }
        output.emplace_back(object);
    }

    void addSpikeGroup(
        std::vector<GeneratedObject>& output,
        float x,
        int count,
        float y = 105.f,
        float spacing = 30.f,
        float rotation = 0.f
    ) {
        for (int i = 0; i < count; ++i) {
            pushGenerated(output, 8, x + spacing * i, y, true, rotation);
        }
    }

    void addModePattern(
        std::vector<GeneratedObject>& output,
        int mode,
        float startX,
        float endX,
        float step,
        int difficulty,
        int layoutStyle,
        std::mt19937& rng
    ) {
        const int maxSpikeGroup = std::clamp(1 + difficulty / 3, 1, 4);
        const int orbChance = std::clamp(18 + difficulty * 4 + (layoutStyle == 1 ? 18 : 0), 18, 72);
        int beat = 0;

        for (float x = startX + 135.f; x < endX - 65.f; x += step) {
            ++beat;
            const int group = randomInt(rng, 1, maxSpikeGroup);
            const bool addSaw = randomInt(rng, 0, 99) < (8 + difficulty * 3 + (layoutStyle == 2 ? 12 : 0));
            const bool addOrb = randomInt(rng, 0, 99) < orbChance;
            const bool addPlatform = randomInt(rng, 0, 99) < (14 + difficulty * 3 + (layoutStyle == 2 ? 18 : 0));

            if (mode == 1) { // Ship: keep a visible flight lane between floor and ceiling.
                if (beat % 2 == 0) addSpikeGroup(output, x, group, 105.f, 30.f);
                if (beat % 3 == 0) addSpikeGroup(output, x + 35.f, 1, 285.f, 30.f, 180.f);
                if (beat % 2 == 0) {
                    pushGenerated(output, 1, x + 100.f, 315.f, true);
                }
                if (addSaw) pushGenerated(output, 89, x + 45.f, 205.f, true);
                continue;
            }

            if (mode == 4) { // Wave: alternate upper/lower blockers to create a readable zig-zag lane.
                const bool upper = (beat % 2) == 0;
                pushGenerated(output, 1, x, upper ? 235.f : 165.f, true);
                if (difficulty >= 5 && beat % 3 == 0) {
                    pushGenerated(output, 1, x + 45.f, upper ? 165.f : 235.f, true);
                }
                if (beat % 2 == 0) addSpikeGroup(output, x + 90.f, 1, 105.f);
                continue;
            }

            if (mode == 6 || mode == 7) { // Spider / Swing: staggered platforms with open gaps.
                if (addPlatform || beat % 2 == 0) {
                    const float platformY = (beat % 2 == 0) ? 165.f : 225.f;
                    pushGenerated(output, 1, x, platformY, true);
                    pushGenerated(output, 1, x + 30.f, platformY, true);
                    if (difficulty >= 6 && beat % 3 == 0) {
                        addSpikeGroup(output, x + 30.f, 1, platformY + 30.f);
                    }
                }
                if (addSaw) pushGenerated(output, 98, x + 95.f, 180.f, true);
                if (addOrb) pushGenerated(output, (beat % 2) ? 36 : 141, x + 55.f, 255.f);
                continue;
            }

            // Cube, Ball, UFO and Robot sections.
            if (mode == 0 || mode == 2 || mode == 3 || mode == 5) {
                if (mode == 3 && beat % 2 == 0) {
                    pushGenerated(output, 35, x, 105.f);
                } else {
                    addSpikeGroup(output, x, group);
                }

                if (addPlatform) {
                    const float py = (beat % 2 == 0) ? 165.f : 195.f;
                    pushGenerated(output, 1, x + 105.f, py, true);
                    if (difficulty >= 4) {
                        pushGenerated(output, 1, x + 135.f, py, true);
                    }
                }

                if (addSaw) {
                    pushGenerated(output, (beat % 2 == 0) ? 88 : 89, x + 115.f, 165.f, true);
                }

                if (addOrb) {
                    const int orbIDs[] = {36, 84, 141, 1022, 1330, 1333};
                    pushGenerated(output, orbIDs[randomInt(rng, 0, 5)], x + 45.f, 190.f + randomInt(rng, 0, 40));
                }

                if (layoutStyle == 1 && beat % 3 == 0) {
                    pushGenerated(output, 140, x + 145.f, 105.f);
                    pushGenerated(output, 36, x + 205.f, 205.f);
                } else if (layoutStyle == 4 && beat % 4 == 0) {
                    pushGenerated(output, 67, x + 120.f, 105.f);
                    pushGenerated(output, 84, x + 185.f, 215.f);
                }

                if (mode == 2 && beat % 3 == 0) {
                    pushGenerated(output, 10, x + 170.f, 105.f);
                    pushGenerated(output, 11, x + 250.f, 105.f);
                }

                if (mode == 5 && beat % 2 == 0) {
                    pushGenerated(output, 143, x + 70.f, 165.f, true);
                }
            }
        }
    }

    bool insertGeneratedBatch(
        LevelEditorLayer* editor,
        std::vector<GeneratedObject> const& objects,
        std::string& error,
        bool recordAnchors
    ) {
        if (!editor) {
            error = "Open an empty level in the Geometry Dash editor first.";
            return false;
        }

        constexpr size_t batchSize = 700;
        for (size_t start = 0; start < objects.size(); start += batchSize) {
            const size_t end = std::min(objects.size(), start + batchSize);
            std::string batch;
            batch.reserve((end - start) * 48);

            for (size_t i = start; i < end; ++i) {
                auto const& spec = objects[i];
                batch += "1,";
                batch += std::to_string(spec.id);
                batch += ",2,";
                batch += std::to_string(spec.x);
                batch += ",3,";
                batch += std::to_string(spec.y);
                batch.push_back(';');
            }

            auto* created = editor->createObjectsFromString(batch, false, false);
            if (!created || created->count() != end - start) {
                error = "Geometry Dash could not create every object in the generated batch. The level may be near its object limit.";
                return false;
            }

            for (size_t i = 0; i < end - start; ++i) {
                auto* object = static_cast<GameObject*>(created->objectAtIndex(static_cast<unsigned int>(i)));
                if (!object) continue;
                auto const& spec = objects[start + i];
                object->setRotation(spec.rotation);
                object->setScaleX(spec.scaleX);
                object->setScaleY(spec.scaleY);
                if (spec.decoration) {
                    object->m_isNoTouch = true;
                    object->m_isDecoration = true;
                    object->setOpacity(spec.opacity);
                }
                if (spec.primaryColor > 0) {
                    applyObjectChannels(object, spec.primaryColor, spec.secondaryColor);
                }
                if (recordAnchors && spec.anchor && object->m_uniqueID > 0) {
                    g_generatedAnchorIDs.push_back(object->m_uniqueID);
                }
            }
        }

        return true;
    }

    bool generatePlayableLayout(
        LevelEditorLayer* editor,
        int difficulty,
        int layoutStyle,
        std::string& error
    ) {
        if (!editor || !editor->m_objects) {
            error = "Open an empty Geometry Dash level in the editor first.";
            return false;
        }
        if (editor->m_objects->count() != 0) {
            error = "To protect your work, create a NEW EMPTY level before generating a layout. Existing objects will not be deleted.";
            return false;
        }
        if (g_generatedEditor == editor && !g_generatedAnchorIDs.empty()) {
            error = "A layout has already been generated in this editor session. Use a new empty level to create another layout.";
            return false;
        }

        g_generatedAnchorIDs.clear();
        g_generatedEditor = nullptr;
        g_generatedDecorated = false;

        std::mt19937 rng(++g_generatorSeed);
        std::vector<GeneratedObject> objects;
        objects.reserve(1800);

        const float step = std::max(125.f, 300.f - static_cast<float>(difficulty) * 17.f);
        struct ModeSpan { int portal; float start; float end; int mode; };
        const ModeSpan spans[] = {
            {0,    120.f, 1000.f, 0},   // Cube
            {13,  1100.f, 2000.f, 1},   // Ship
            {47,  2100.f, 3100.f, 2},   // Ball
            {111, 3200.f, 4100.f, 3},   // UFO
            {660, 4200.f, 5100.f, 4},   // Wave
            {745, 5200.f, 6100.f, 5},   // Robot
            {1331,6200.f, 7100.f, 6},   // Spider
            {1933,7200.f, 8100.f, 7}    // Swing
        };

        // Add the eight common game modes in separate, readable sections.
        for (size_t i = 0; i < std::size(spans); ++i) {
            auto const& span = spans[i];
            if (span.portal != 0) pushGenerated(objects, span.portal, span.start, 105.f);
            addModePattern(
                objects, span.mode, span.start, span.end, step,
                difficulty, layoutStyle, rng
            );
            if (i + 1 < std::size(spans)) {
                pushGenerated(objects, 12, span.end + 20.f, 105.f); // return to Cube
            }
        }
        pushGenerated(objects, 12, 8140.f, 105.f);

        // Vary speed, size and gravity as normal gameplay ingredients.
        pushGenerated(objects, 200, 500.f, 105.f);
        pushGenerated(objects, 201, 760.f, 105.f);
        if (difficulty >= 3) pushGenerated(objects, 202, 3750.f, 105.f);
        if (difficulty >= 7) pushGenerated(objects, 203, 6800.f, 105.f);
        if (difficulty >= 4) {
            pushGenerated(objects, 99, 2750.f, 105.f);
            pushGenerated(objects, 101, 3000.f, 105.f);
        }

        if (objects.size() > 12000) {
            error = "The generated layout exceeded the safe object limit. Choose a shorter or easier generation profile.";
            return false;
        }

        if (!insertGeneratedBatch(editor, objects, error, true)) {
            g_generatedAnchorIDs.clear();
            return false;
        }

        g_generatedEditor = editor;
        editor->updateLevelColors();
        return true;
    }

    bool autoDecorateGeneratedLayout(
        LevelEditorLayer* editor,
        int theme,
        std::string& error,
        size_t& decorationCount
    ) {
        decorationCount = 0;
        if (!editor || editor != g_generatedEditor || g_generatedAnchorIDs.empty()) {
            error = "Generate a layout FIRST in this same empty level. Auto Deco only decorates a layout created by this tool.";
            return false;
        }
        if (g_generatedDecorated) {
            error = "This generated layout is already decorated. Generate a fresh layout to try another theme.";
            return false;
        }

        applyGeneratorPalette(editor, theme);

        std::vector<GeneratedObject> decorations;
        decorations.reserve(g_generatedAnchorIDs.size() * 2 + 120);
        std::mt19937 rng(++g_generatorSeed);

        const std::vector<int> themeAssets = [&]() {
            switch (theme) {
                case 1: return std::vector<int>{123, 124, 126, 85, 86, 18, 20};
                case 2: return std::vector<int>{48, 49, 53, 54, 85, 18, 21};
                case 3: return std::vector<int>{129, 130, 131, 123, 126, 52, 53};
                case 4: return std::vector<int>{50, 53, 54, 85, 86, 48, 190};
                case 5: return std::vector<int>{15, 16, 17, 85, 86, 87, 157, 159};
                default: return std::vector<int>{50, 51, 53, 85, 86, 18, 20};
            }
        }();

        size_t anchorIndex = 0;
        for (int uniqueID : g_generatedAnchorIDs) {
            auto* anchor = editor->findGameObject(uniqueID);
            if (!anchor) continue;

            applyObjectChannels(anchor, 1, 2);
            const auto pos = anchor->getPosition();
            const int asset = themeAssets[anchorIndex % themeAssets.size()];
            const float smallScale = (asset >= 123 && asset <= 131) ? 0.55f : 0.38f;

            if (anchor->m_objectID == 8) {
                pushGenerated(decorations, asset, pos.x, pos.y + 27.f, false, 0.f);
                decorations.back().decoration = true;
                decorations.back().scaleX = smallScale;
                decorations.back().scaleY = smallScale;
                decorations.back().primaryColor = 2;
                decorations.back().secondaryColor = 1;
                decorations.back().opacity = 220;
            } else if (anchor->m_objectID == 88 || anchor->m_objectID == 89 || anchor->m_objectID == 98) {
                pushGenerated(decorations, 85, pos.x, pos.y + 34.f, false, 0.f);
                decorations.back().decoration = true;
                decorations.back().scaleX = 0.42f;
                decorations.back().scaleY = 0.42f;
                decorations.back().primaryColor = 2;
                decorations.back().secondaryColor = 1;
                decorations.back().opacity = 210;
            } else {
                pushGenerated(decorations, asset, pos.x - 18.f, pos.y + 24.f, false, 0.f);
                decorations.back().decoration = true;
                decorations.back().scaleX = smallScale;
                decorations.back().scaleY = smallScale;
                decorations.back().primaryColor = 2;
                decorations.back().secondaryColor = 1;
                decorations.back().opacity = 220;

                if (anchorIndex % 2 == 0) {
                    const int accentAsset = themeAssets[(anchorIndex + 3) % themeAssets.size()];
                    pushGenerated(decorations, accentAsset, pos.x + 18.f, pos.y + 25.f, false, 45.f);
                    decorations.back().decoration = true;
                    decorations.back().scaleX = 0.30f;
                    decorations.back().scaleY = 0.30f;
                    decorations.back().primaryColor = 1;
                    decorations.back().secondaryColor = 2;
                    decorations.back().opacity = 205;
                }
            }
            ++anchorIndex;
        }

        // Backdrop ornaments form a visual rhythm behind the playable route.
        for (float x = 300.f; x < 8200.f; x += 430.f) {
            const int cloudID = (theme == 3 || theme == 2) ? 49 : 48;
            pushGenerated(decorations, cloudID, x, 355.f + randomInt(rng, -25, 35));
            decorations.back().decoration = true;
            decorations.back().scaleX = 0.75f;
            decorations.back().scaleY = 0.75f;
            decorations.back().primaryColor = 1;
            decorations.back().secondaryColor = 2;
            decorations.back().opacity = 95;

            const int sparkleIDs[] = {50, 53, 54};
            pushGenerated(
                decorations, sparkleIDs[randomInt(rng, 0, 2)],
                x + 145.f, 295.f + randomInt(rng, -25, 30)
            );
            decorations.back().decoration = true;
            decorations.back().scaleX = 0.32f;
            decorations.back().scaleY = 0.32f;
            decorations.back().primaryColor = 2;
            decorations.back().secondaryColor = 1;
            decorations.back().opacity = 150;
        }

        if (decorations.empty()) {
            error = "No generated blocks or spikes were found to decorate.";
            return false;
        }

        if (!insertGeneratedBatch(editor, decorations, error, false)) return false;

        decorationCount = decorations.size();
        g_generatedDecorated = true;
        editor->updateLevelColors();
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
            " object styles / " + std::to_string(pieceCount) +
            " deco pieces / " + std::to_string(data.colors.size()) +
            " custom colors + background. Tap Copy Code."
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
            " object styles / " + std::to_string(pieceCount) +
            " deco pieces / " + std::to_string(m_importData.colors.size()) +
            " custom colors + background. Open your target level and Apply."
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
        size_t totalTargetBlocks = 0;
        size_t unmatchedBlocks = 0;

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
            if (editor->m_objects) {
                const unsigned int count = editor->m_objects->count();
                targetBlocks.reserve(std::min<size_t>(count, 20000));

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
                    }
                }
            }

            if (targetBlocks.empty()) {
                this->setStatus("No regular blocks were found in your target level to attach the deco to.", true);
                return;
            }
            totalTargetBlocks = targetBlocks.size();

            struct PendingPiece {
                std::string const* record = nullptr;
                Placement placement;
            };
            std::vector<PendingPiece> pendingPieces;
            pendingPieces.reserve(std::min<size_t>(targetBlocks.size() * 2, kMaxObjects));
            size_t totalBytes = 0;

            for (auto const& target : targetBlocks) {
                const uint32_t targetType = static_cast<uint32_t>(target.object->m_objectType);
                const DecoTemplate* bestTemplate = nullptr;
                float bestScore = std::numeric_limits<float>::max();

                for (auto const& candidate : data.templates) {
                    // Only apply real attached deco patterns. The target block
                    // or spike itself is never replaced, so gameplay stays intact.
                    if (candidate.items.empty() || candidate.anchorType != targetType) continue;

                    const float ratioX = target.width / candidate.anchorWidth;
                    const float ratioY = target.height / candidate.anchorHeight;
                    if (!std::isfinite(ratioX) || !std::isfinite(ratioY) ||
                        ratioX <= 0.f || ratioY <= 0.f) continue;

                    // Keep small/large object families separate.
                    if (ratioX < 0.85f || ratioX > 1.18f ||
                        ratioY < 0.85f || ratioY > 1.18f) continue;

                    float score = std::abs(std::log(ratioX)) +
                                  std::abs(std::log(ratioY));
                    if (candidate.anchorObjectID != static_cast<uint32_t>(target.object->m_objectID)) {
                        score += 0.12f;
                    }
                    // Prefer a source object with the richest attached deco
                    // when multiple samples have nearly identical dimensions.
                    score -= static_cast<float>(candidate.items.size()) * 0.0001f;

                    if (score < bestScore) {
                        bestScore = score;
                        bestTemplate = &candidate;
                    }
                }

                if (!bestTemplate) {
                    ++unmatchedBlocks;
                    continue;
                }

                const float ratioX = target.width / bestTemplate->anchorWidth;
                const float ratioY = target.height / bestTemplate->anchorHeight;
                const float fitScale = std::clamp(
                    std::min(ratioX, ratioY), 0.85f, 1.18f
                );

                for (auto const& item : bestTemplate->items) {
                    const size_t itemBytes = item.record.size() + 1;
                    if (pendingPieces.size() >= kMaxObjects ||
                        itemBytes > kMaxPayloadBytes - totalBytes) {
                        this->setStatus(
                            "This style needs more than 250,000 deco pieces or 128 MiB. Split the target level into smaller sections.",
                            true
                        );
                        return;
                    }

                    totalBytes += itemBytes;
                    pendingPieces.push_back({
                        &item.record,
                        {
                            target.position,
                            item.offsetX,
                            item.offsetY,
                            fitScale
                        }
                    });
                }
                ++matchedBlocks;
            }

            // Parse and insert a limited number of objects per call. This keeps
            // individual Geometry Dash parser calls bounded on very large levels.
            for (size_t start = 0; start < pendingPieces.size();) {
                std::string batchText;
                std::vector<Placement> batchPlacements;
                batchText.reserve(std::min(kImportBatchBytes, totalBytes));
                batchPlacements.reserve(std::min(kImportBatchObjects, pendingPieces.size() - start));

                size_t batchBytes = 0;
                size_t end = start;
                while (end < pendingPieces.size() &&
                       batchPlacements.size() < kImportBatchObjects) {
                    auto const& pending = pendingPieces[end];
                    if (!pending.record) {
                        this->setStatus("A decoration record is missing from the import plan.", true);
                        return;
                    }
                    const size_t itemBytes = pending.record->size() + 1;
                    if (!batchPlacements.empty() &&
                        itemBytes > kImportBatchBytes - batchBytes) {
                        break;
                    }

                    batchText += *pending.record;
                    batchText.push_back(';');
                    batchBytes += itemBytes;
                    batchPlacements.push_back(pending.placement);
                    ++end;
                }

                if (batchPlacements.empty()) {
                    this->setStatus("Could not prepare a safe decoration import batch.", true);
                    return;
                }

                auto* created = editor->createObjectsFromString(batchText, false, false);
                if (!created) {
                    this->setStatus(
                        "Import stopped after " + std::to_string(addedPieces) +
                        " deco pieces. Geometry Dash could not create the next batch.",
                        true
                    );
                    return;
                }

                const unsigned int createdCount = created->count();
                const size_t applyCount = std::min<size_t>(createdCount, batchPlacements.size());
                for (size_t i = 0; i < applyCount; ++i) {
                    auto* object = static_cast<GameObject*>(created->objectAtIndex(static_cast<unsigned int>(i)));
                    if (!object) continue;

                    auto const& placement = batchPlacements[i];
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

                if (createdCount < batchPlacements.size()) {
                    this->setStatus(
                        "Import stopped: Geometry Dash created only " +
                        std::to_string(createdCount) + " of " +
                        std::to_string(batchPlacements.size()) + " objects in a batch.",
                        true
                    );
                    return;
                }

                start = end;
            }
        }

        const bool settingsApplied = applyVisualSettings(editor, data.settings);
        const size_t copiedColors = applyCustomPalette(editor, data.colors);
        if (copiedColors > 0) {
            editor->updateLevelColors();
        }
        if (addedPieces == 0 && !settingsApplied && copiedColors == 0) {
            this->setStatus(
                "No attached deco patterns matched your blocks or spikes. Try a sample level with visible decoration around them.",
                true
            );
            return;
        }

        std::string result =
            "Applied " + std::to_string(addedPieces) + " deco pieces to " +
            std::to_string(matchedBlocks) + " of " +
            std::to_string(totalTargetBlocks) + " matching blocks / spikes.";
        if (unmatchedBlocks > 0) {
            result += " " + std::to_string(unmatchedBlocks) +
                      " objects had no matching decoration pattern.";
        }
        if (settingsApplied) result += " Background and ground copied.";
        if (copiedColors > 0) {
            result += " " + std::to_string(copiedColors) + " custom colors copied.";
        } else if (!data.colors.empty()) {
            result += " Custom color channels could not be applied.";
        }
        result += " Your original blocks, spikes, gameplay and music were not replaced.";
        this->setStatus(result);
    }

    void onClose(cocos2d::CCObject* sender) override {
        if (g_popup == this) g_popup = nullptr;
        geode::Popup::onClose(sender);
    }
};


namespace {
    AutoLevelPopup* g_generatorPopup = nullptr;
}

class AutoLevelPopup final : public geode::Popup {
protected:
    int m_difficulty = 0;
    int m_layoutStyle = 0;
    int m_theme = 0;
    cocos2d::CCLabelBMFont* m_difficultyLabel = nullptr;
    cocos2d::CCLabelBMFont* m_layoutLabel = nullptr;
    cocos2d::CCLabelBMFont* m_themeLabel = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;

    static constexpr const char* difficultyNames[] = {
        "Easy", "Normal", "Hard", "Harder", "Insane",
        "Easy Demon", "Medium Demon", "Hard Demon", "Insane Demon", "Extreme Demon"
    };
    static constexpr const char* layoutNames[] = {
        "Classic Flow", "Orb Flow", "Tech Steps", "Wave Corridors", "Sync Pattern", "Mixed Adventure"
    };
    static constexpr const char* themeNames[] = {
        "Neon City", "Inferno", "Ice Crystal", "Emerald Forest", "Galaxy", "Cyber"
    };

    ~AutoLevelPopup() override {
        if (g_generatorPopup == this) g_generatorPopup = nullptr;
    }

    CCMenuItemSpriteExtra* makeArrow(
        const char* label,
        SEL_MenuHandler selector,
        float x,
        float y
    ) {
        auto* item = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(label, "goldFont.fnt", "GJ_button_01.png", 0.48f),
            this,
            selector
        );
        if (!item) return nullptr;
        item->setPosition({x, y});
        m_buttonMenu->addChild(item);
        return item;
    }

    void refreshOptions() {
        if (m_difficultyLabel) {
            const std::string text = std::string("DIFFICULTY: ") + difficultyNames[m_difficulty];
            m_difficultyLabel->setString(text.c_str());
        }
        if (m_layoutLabel) {
            const std::string text = std::string("LAYOUT: ") + layoutNames[m_layoutStyle];
            m_layoutLabel->setString(text.c_str());
        }
        if (m_themeLabel) {
            const std::string text = std::string("DECO THEME: ") + themeNames[m_theme];
            m_themeLabel->setString(text.c_str());
        }
    }

    bool init() override {
        constexpr float popupWidth = 430.f;
        constexpr float popupHeight = 390.f;
        if (!geode::Popup::init(popupWidth, popupHeight)) return false;

        const auto winSize = CCDirector::get()->getWinSize();
        m_mainLayer->setPosition(winSize / 2.f);
        const float fitScale = std::min(
            (winSize.width - 20.f) / popupWidth,
            (winSize.height - 20.f) / popupHeight
        );
        m_mainLayer->setScale(std::clamp(fitScale, 0.46f, 0.78f));

        this->setTitle("LEVEL GENERATOR + AUTO DECO", "goldFont.fnt", 0.48f, 22.f);
        if (m_closeBtn) {
            const auto size = m_mainLayer->getContentSize();
            m_closeBtn->setPosition({size.width - 17.f, size.height - 17.f});
            m_closeBtn->setScale(0.50f);
            m_closeBtn->setZOrder(10000);
        }

        auto makeOptionLabel = [this](const char* text, float y) {
            auto* label = cocos2d::CCLabelBMFont::create(text, "goldFont.fnt");
            if (!label) return static_cast<cocos2d::CCLabelBMFont*>(nullptr);
            label->setScale(0.48f);
            m_mainLayer->addChildAtPosition(label, geode::Anchor::Center, {0.f, y});
            return label;
        };

        m_difficultyLabel = makeOptionLabel("DIFFICULTY: Easy", 103.f);
        m_layoutLabel = makeOptionLabel("LAYOUT: Classic Flow", 53.f);
        m_themeLabel = makeOptionLabel("DECO THEME: Neon City", 3.f);
        if (!m_difficultyLabel || !m_layoutLabel || !m_themeLabel) return false;

        const float leftX = popupWidth / 2.f - 145.f;
        const float rightX = popupWidth / 2.f + 145.f;

        if (!makeArrow("<", menu_selector(AutoLevelPopup::onDifficultyPrev), leftX, popupHeight / 2.f + 103.f)) return false;
        if (!makeArrow(">", menu_selector(AutoLevelPopup::onDifficultyNext), rightX, popupHeight / 2.f + 103.f)) return false;
        if (!makeArrow("<", menu_selector(AutoLevelPopup::onLayoutPrev), leftX, popupHeight / 2.f + 53.f)) return false;
        if (!makeArrow(">", menu_selector(AutoLevelPopup::onLayoutNext), rightX, popupHeight / 2.f + 53.f)) return false;
        if (!makeArrow("<", menu_selector(AutoLevelPopup::onThemePrev), leftX, popupHeight / 2.f + 3.f)) return false;
        if (!makeArrow(">", menu_selector(AutoLevelPopup::onThemeNext), rightX, popupHeight / 2.f + 3.f)) return false;

        auto* generateButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("1. GENERATE LAYOUT", "goldFont.fnt", "GJ_button_01.png", 0.62f),
            this,
            menu_selector(AutoLevelPopup::onGenerateLayout)
        );
        if (!generateButton) return false;
        generateButton->setPosition({popupWidth / 2.f - 104.f, popupHeight / 2.f - 65.f});
        m_buttonMenu->addChild(generateButton);

        auto* decoButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("2. AUTO DECO", "goldFont.fnt", "GJ_button_02.png", 0.62f),
            this,
            menu_selector(AutoLevelPopup::onAutoDeco)
        );
        if (!decoButton) return false;
        decoButton->setPosition({popupWidth / 2.f + 104.f, popupHeight / 2.f - 65.f});
        m_buttonMenu->addChild(decoButton);

        auto* hint = cocos2d::CCLabelBMFont::create(
            "Create layout first. Auto Deco only works after a successful layout.",
            "chatFont.fnt"
        );
        if (!hint) return false;
        hint->setScale(0.36f);
        hint->limitLabelWidth(380.f, 0.36f, 0.18f);
        m_mainLayer->addChildAtPosition(hint, geode::Anchor::Center, {0.f, -103.f});

        m_statusLabel = cocos2d::CCLabelBMFont::create(
            "Open a NEW EMPTY level, choose options, then Generate Layout.",
            "chatFont.fnt"
        );
        if (!m_statusLabel) return false;
        m_statusLabel->setScale(0.37f);
        m_statusLabel->limitLabelWidth(380.f, 0.37f, 0.18f);
        m_mainLayer->addChildAtPosition(m_statusLabel, geode::Anchor::Center, {0.f, -143.f});

        refreshOptions();
        return true;
    }

    void setStatus(std::string const& message, bool isError = false) {
        if (!m_statusLabel) return;
        m_statusLabel->setString(message.c_str());
        m_statusLabel->setScale(0.37f);
        m_statusLabel->setColor(
            isError ? cocos2d::ccc3(255, 105, 105) : cocos2d::ccc3(150, 255, 150)
        );
        m_statusLabel->limitLabelWidth(380.f, 0.37f, 0.18f);
    }

public:
    static AutoLevelPopup* create() {
        auto* popup = new AutoLevelPopup();
        if (popup && popup->init()) {
            popup->autorelease();
            return popup;
        }
        CC_SAFE_DELETE(popup);
        return nullptr;
    }

    void onDifficultyPrev(cocos2d::CCObject*) {
        m_difficulty = (m_difficulty + 9) % 10;
        refreshOptions();
    }

    void onDifficultyNext(cocos2d::CCObject*) {
        m_difficulty = (m_difficulty + 1) % 10;
        refreshOptions();
    }

    void onLayoutPrev(cocos2d::CCObject*) {
        m_layoutStyle = (m_layoutStyle + 5) % 6;
        refreshOptions();
    }

    void onLayoutNext(cocos2d::CCObject*) {
        m_layoutStyle = (m_layoutStyle + 1) % 6;
        refreshOptions();
    }

    void onThemePrev(cocos2d::CCObject*) {
        m_theme = (m_theme + 5) % 6;
        refreshOptions();
    }

    void onThemeNext(cocos2d::CCObject*) {
        m_theme = (m_theme + 1) % 6;
        refreshOptions();
    }

    void onGenerateLayout(cocos2d::CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (!editor) {
            this->setStatus("Open a new empty level in the Geometry Dash editor first.", true);
            return;
        }

        std::string error;
        if (!generatePlayableLayout(editor, m_difficulty, m_layoutStyle, error)) {
            this->setStatus(error, true);
            return;
        }

        this->setStatus(
            std::string("LAYOUT CREATED | ") + difficultyNames[m_difficulty] +
            " / " + layoutNames[m_layoutStyle] +
            " | " + std::to_string(g_generatedAnchorIDs.size()) +
            " blocks, spikes and saw anchors. Now press AUTO DECO."
        );
    }

    void onAutoDeco(cocos2d::CCObject*) {
        auto* editor = LevelEditorLayer::get();
        std::string error;
        size_t decorationCount = 0;
        if (!autoDecorateGeneratedLayout(editor, m_theme, error, decorationCount)) {
            this->setStatus(error, true);
            return;
        }

        this->setStatus(
            std::string("AUTO DECO COMPLETE | ") + themeNames[m_theme] +
            " | " + std::to_string(decorationCount) +
            " visual deco pieces, custom colors and background palette applied. Gameplay objects were kept."
        );
    }

    void onClose(cocos2d::CCObject* sender) override {
        if (g_generatorPopup == this) g_generatorPopup = nullptr;
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

    void toggleLevelGenerator() {
        if (!LevelEditorLayer::get()) return;

        if (g_generatorPopup) {
            g_generatorPopup->onClose(nullptr);
            return;
        }

        if (auto* popup = AutoLevelPopup::create()) {
            g_generatorPopup = popup;
            popup->show();
        }
    }

    geode::ListenerHandle g_keyboardListener;
    geode::ListenerHandle g_styleKeyboardListener;

}

$on_game(Loaded) {
    g_keyboardListener = geode::KeyboardInputEvent(cocos2d::KEY_F3).listen(
        [](geode::KeyboardInputData& data) {
            if (data.action == geode::KeyboardInputData::Action::Press) {
                toggleLevelGenerator();
            }
            return false;
        }
    );

    // Keep the old style scanner available as an optional legacy tool on F4.
    g_styleKeyboardListener = geode::KeyboardInputEvent(cocos2d::KEY_F4).listen(
        [](geode::KeyboardInputData& data) {
            if (data.action == geode::KeyboardInputData::Action::Press) {
                toggleStyleScanner();
            }
            return false;
        }
    );
}
