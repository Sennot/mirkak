#include "smart_contrast.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/NineSlice.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/base64.hpp>
#include <Geode/cocos/support/zip_support/ZipUtils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace cleanfeed::smart_contrast {
    namespace {
        constexpr std::array<std::string_view, 8> kColorSettingKeys = {
            "solid-color",
            "hazard-color",
            "interactable-color",
            "player-color",
            "player-inner-color",
            "player-rotated-color",
            "trajectory-hold-color",
            "trajectory-release-color",
        };

        struct Rgb {
            uint8_t r = 255;
            uint8_t g = 255;
            uint8_t b = 255;
        };

        struct Lab {
            double l = 0.0;
            double a = 0.0;
            double b = 0.0;
        };

        struct Palette {
            std::array<Rgb, kColorSettingKeys.size()> colors;
        };

        struct AnalysisResult {
            std::array<Palette, 3> palettes;
            size_t objectCount = 0;
            size_t sampledColors = 0;
            std::string error;
        };

        struct LevelEntry {
            Ref<GJGameLevel> level;
            std::string name;
            int id = 0;
            bool editorLevel = false;
        };

        struct AnalysisJob {
            std::atomic<float> progress = 0.f;
            std::atomic<int> stage = 0;
            std::atomic<bool> cancelled = false;
            std::atomic<bool> complete = false;
            std::mutex resultMutex;
            std::optional<AnalysisResult> result;
        };

        struct WeightedColor {
            Rgb rgb;
            double weight = 0.0;
        };

        struct Histogram {
            std::array<double, 32 * 32 * 32> bins{};
            size_t samples = 0;

            void add(Rgb color, double weight) {
                if (weight <= 0.0) return;
                auto const index =
                    (static_cast<size_t>(color.r >> 3) << 10) |
                    (static_cast<size_t>(color.g >> 3) << 5) |
                    static_cast<size_t>(color.b >> 3);
                bins[index] += weight;
                ++samples;
            }

            std::vector<WeightedColor> dominant(size_t limit) const {
                std::vector<WeightedColor> colors;
                colors.reserve(bins.size());

                for (size_t index = 0; index < bins.size(); ++index) {
                    auto const weight = bins[index];
                    if (weight <= 0.0) continue;

                    colors.push_back({
                        {
                            static_cast<uint8_t>((((index >> 10) & 31) << 3) + 4),
                            static_cast<uint8_t>((((index >> 5) & 31) << 3) + 4),
                            static_cast<uint8_t>(((index & 31) << 3) + 4),
                        },
                        weight,
                    });
                }

                auto const count = std::min(limit, colors.size());
                std::partial_sort(
                    colors.begin(), colors.begin() + count, colors.end(),
                    [](auto const& lhs, auto const& rhs) {
                        return lhs.weight > rhs.weight;
                    }
                );
                colors.resize(count);
                return colors;
            }
        };

        double srgbToLinear(double value) {
            value /= 255.0;
            return value <= 0.04045
                ? value / 12.92
                : std::pow((value + 0.055) / 1.055, 2.4);
        }

        double linearToSrgb(double value) {
            value = std::clamp(value, 0.0, 1.0);
            auto const srgb = value <= 0.0031308
                ? value * 12.92
                : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
            return srgb * 255.0;
        }

        double luminance(Rgb color) {
            return
                0.2126 * srgbToLinear(color.r) +
                0.7152 * srgbToLinear(color.g) +
                0.0722 * srgbToLinear(color.b);
        }

        double contrastRatio(Rgb lhs, Rgb rhs) {
            auto const first = luminance(lhs);
            auto const second = luminance(rhs);
            auto const bright = std::max(first, second);
            auto const dark = std::min(first, second);
            return (bright + 0.05) / (dark + 0.05);
        }

        Lab toLab(Rgb color) {
            auto const r = srgbToLinear(color.r);
            auto const g = srgbToLinear(color.g);
            auto const b = srgbToLinear(color.b);

            auto const l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
            auto const m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
            auto const s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);

            return {
                0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
            };
        }

        Rgb fromLch(double lightness, double chroma, double hueDegrees) {
            constexpr double pi = 3.14159265358979323846;
            auto const hue = hueDegrees * pi / 180.0;
            auto const a = std::cos(hue) * chroma;
            auto const b = std::sin(hue) * chroma;

            auto const lRoot = lightness + 0.3963377774 * a + 0.2158037573 * b;
            auto const mRoot = lightness - 0.1055613458 * a - 0.0638541728 * b;
            auto const sRoot = lightness - 0.0894841775 * a - 1.2914855480 * b;
            auto const l = lRoot * lRoot * lRoot;
            auto const m = mRoot * mRoot * mRoot;
            auto const s = sRoot * sRoot * sRoot;

            auto const r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
            auto const g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
            auto const blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

            return {
                static_cast<uint8_t>(std::lround(linearToSrgb(r))),
                static_cast<uint8_t>(std::lround(linearToSrgb(g))),
                static_cast<uint8_t>(std::lround(linearToSrgb(blue))),
            };
        }

        Rgb gamutMappedLch(double lightness, double chroma, double hueDegrees) {
            // Clamping highly saturated OKLCH colors can collapse different hues
            // into the same RGB corner. Reducing chroma first keeps the patterns distinct.
            for (auto current = chroma; current >= 0.04; current -= 0.01) {
                constexpr double pi = 3.14159265358979323846;
                auto const hue = hueDegrees * pi / 180.0;
                auto const a = std::cos(hue) * current;
                auto const b = std::sin(hue) * current;
                auto const lRoot = lightness + 0.3963377774 * a + 0.2158037573 * b;
                auto const mRoot = lightness - 0.1055613458 * a - 0.0638541728 * b;
                auto const sRoot = lightness - 0.0894841775 * a - 1.2914855480 * b;
                auto const l = lRoot * lRoot * lRoot;
                auto const m = mRoot * mRoot * mRoot;
                auto const s = sRoot * sRoot * sRoot;
                auto const r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
                auto const g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
                auto const blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

                if (r >= 0.0 && r <= 1.0 && g >= 0.0 && g <= 1.0 && blue >= 0.0 && blue <= 1.0) {
                    return fromLch(lightness, current, hueDegrees);
                }
            }
            return fromLch(lightness, 0.04, hueDegrees);
        }

        double hueDistance(double first, double second) {
            auto distance = std::fmod(std::abs(first - second), 360.0);
            return std::min(distance, 360.0 - distance);
        }

        bool parseInt(std::string_view value, int& output) {
            if (value.empty()) return false;
            auto const begin = value.data();
            auto const end = value.data() + value.size();
            auto const result = std::from_chars(begin, end, output);
            return result.ec == std::errc{} && result.ptr == end;
        }

        bool parseDouble(std::string_view value, double& output) {
            if (value.empty()) return false;
            auto const begin = value.data();
            auto const end = value.data() + value.size();
            auto const result = std::from_chars(begin, end, output);
            return result.ec == std::errc{} && result.ptr == end && std::isfinite(output);
        }

        struct HsvShift {
            double hue = 0.0;
            double saturation = 1.0;
            double value = 1.0;
            bool addSaturation = false;
            bool addValue = false;
            bool valid = false;
        };

        HsvShift parseHsv(std::string_view encoded) {
            HsvShift shift;
            std::array<std::string_view, 5> fields{};
            size_t count = 0;
            size_t position = 0;
            while (position <= encoded.size() && count < fields.size()) {
                auto end = encoded.find('a', position);
                if (end == std::string_view::npos) end = encoded.size();
                fields[count++] = encoded.substr(position, end - position);
                if (end == encoded.size()) break;
                position = end + 1;
            }

            int addSaturation = 0;
            int addValue = 0;
            if (
                count != fields.size() ||
                !parseDouble(fields[0], shift.hue) ||
                !parseDouble(fields[1], shift.saturation) ||
                !parseDouble(fields[2], shift.value) ||
                !parseInt(fields[3], addSaturation) ||
                !parseInt(fields[4], addValue)
            ) {
                return {};
            }

            shift.addSaturation = addSaturation != 0;
            shift.addValue = addValue != 0;
            shift.valid = true;
            return shift;
        }

        std::array<double, 3> rgbToHsv(Rgb color) {
            auto const r = static_cast<double>(color.r) / 255.0;
            auto const g = static_cast<double>(color.g) / 255.0;
            auto const b = static_cast<double>(color.b) / 255.0;
            auto const maximum = std::max({r, g, b});
            auto const minimum = std::min({r, g, b});
            auto const chroma = maximum - minimum;

            double hue = 0.0;
            if (chroma > 1e-9) {
                if (maximum == r) hue = 60.0 * std::fmod((g - b) / chroma, 6.0);
                else if (maximum == g) hue = 60.0 * ((b - r) / chroma + 2.0);
                else hue = 60.0 * ((r - g) / chroma + 4.0);
                if (hue < 0.0) hue += 360.0;
            }

            auto const saturation = maximum <= 1e-9 ? 0.0 : chroma / maximum;
            return {hue, saturation, maximum};
        }

        Rgb hsvToRgb(double hue, double saturation, double value) {
            hue = std::fmod(hue, 360.0);
            if (hue < 0.0) hue += 360.0;
            saturation = std::clamp(saturation, 0.0, 1.0);
            value = std::clamp(value, 0.0, 1.0);

            auto const chroma = value * saturation;
            auto const section = hue / 60.0;
            auto const x = chroma * (1.0 - std::abs(std::fmod(section, 2.0) - 1.0));
            auto const offset = value - chroma;
            double r = 0.0;
            double g = 0.0;
            double b = 0.0;

            if (section < 1.0) { r = chroma; g = x; }
            else if (section < 2.0) { r = x; g = chroma; }
            else if (section < 3.0) { g = chroma; b = x; }
            else if (section < 4.0) { g = x; b = chroma; }
            else if (section < 5.0) { r = x; b = chroma; }
            else { r = chroma; b = x; }

            return {
                static_cast<uint8_t>(std::lround((r + offset) * 255.0)),
                static_cast<uint8_t>(std::lround((g + offset) * 255.0)),
                static_cast<uint8_t>(std::lround((b + offset) * 255.0)),
            };
        }

        Rgb applyHsv(Rgb color, HsvShift const& shift) {
            if (!shift.valid) return color;
            auto hsv = rgbToHsv(color);
            hsv[0] += shift.hue;
            hsv[1] = shift.addSaturation
                ? hsv[1] + shift.saturation
                : hsv[1] * shift.saturation;
            hsv[2] = shift.addValue
                ? hsv[2] + shift.value
                : hsv[2] * shift.value;
            return hsvToRgb(hsv[0], hsv[1], hsv[2]);
        }

        template <class Callback>
        void visitPairs(std::string_view input, char separator, Callback&& callback) {
            size_t position = 0;
            while (position < input.size()) {
                auto keyEnd = input.find(separator, position);
                if (keyEnd == std::string_view::npos) break;
                auto valueEnd = input.find(separator, keyEnd + 1);
                if (valueEnd == std::string_view::npos) valueEnd = input.size();

                callback(
                    input.substr(position, keyEnd - position),
                    input.substr(keyEnd + 1, valueEnd - keyEnd - 1)
                );
                position = valueEnd + 1;
            }
        }

        struct ChannelColor {
            Rgb from{255, 255, 255};
            Rgb to{255, 255, 255};
            int id = -1;
            int copyId = 0;
            int playerColor = -1;
            double fromOpacity = 1.0;
            double toOpacity = 1.0;
            bool hasTo = false;
            bool copyOpacity = false;
            bool blending = false;
            HsvShift copyHsv;
        };

        struct ResolvedColor {
            Rgb from{255, 255, 255};
            Rgb to{255, 255, 255};
            double fromOpacity = 1.0;
            double toOpacity = 1.0;
            bool hasTo = false;
            bool blending = false;
        };

        using ChannelMap = std::unordered_map<int, ChannelColor>;

        ChannelMap parseStartColors(std::string_view start) {
            std::string_view encodedColors;
            visitPairs(start, ',', [&](std::string_view key, std::string_view value) {
                if (key == "kS38") encodedColors = value;
            });

            ChannelMap channels;
            channels.reserve(128);
            size_t position = 0;
            while (position < encodedColors.size()) {
                auto end = encodedColors.find('|', position);
                if (end == std::string_view::npos) end = encodedColors.size();
                auto const encoded = encodedColors.substr(position, end - position);

                ChannelColor channel;
                std::string_view hsvText;
                bool hasTargetRed = false;
                bool hasTargetGreen = false;
                bool hasTargetBlue = false;

                visitPairs(encoded, '_', [&](std::string_view keyText, std::string_view valueText) {
                    int key = 0;
                    int value = 0;
                    if (!parseInt(keyText, key)) return;

                    double decimal = 0.0;
                    if (key == 7 && parseDouble(valueText, decimal)) {
                        channel.fromOpacity = std::clamp(decimal, 0.0, 1.0);
                        return;
                    }
                    if (key == 15 && parseDouble(valueText, decimal)) {
                        channel.toOpacity = std::clamp(decimal, 0.0, 1.0);
                        return;
                    }
                    if (key == 10) {
                        hsvText = valueText;
                        return;
                    }
                    if (!parseInt(valueText, value)) return;

                    switch (key) {
                        case 1: channel.from.r = static_cast<uint8_t>(std::clamp(value, 0, 255)); break;
                        case 2: channel.from.g = static_cast<uint8_t>(std::clamp(value, 0, 255)); break;
                        case 3: channel.from.b = static_cast<uint8_t>(std::clamp(value, 0, 255)); break;
                        case 4: channel.playerColor = value; break;
                        case 5: channel.blending = value != 0; break;
                        case 6: channel.id = value; break;
                        case 9: channel.copyId = value; break;
                        case 11:
                            channel.to.r = static_cast<uint8_t>(std::clamp(value, 0, 255));
                            hasTargetRed = true;
                            break;
                        case 12:
                            channel.to.g = static_cast<uint8_t>(std::clamp(value, 0, 255));
                            hasTargetGreen = true;
                            break;
                        case 13:
                            channel.to.b = static_cast<uint8_t>(std::clamp(value, 0, 255));
                            hasTargetBlue = true;
                            break;
                        case 17: channel.copyOpacity = value != 0; break;
                        default: break;
                    }
                });

                channel.hasTo = hasTargetRed && hasTargetGreen && hasTargetBlue;
                if (!channel.hasTo) {
                    channel.to = channel.from;
                    channel.toOpacity = channel.fromOpacity;
                }
                channel.copyHsv = parseHsv(hsvText);
                if (channel.id >= 0) channels[channel.id] = channel;

                position = end + 1;
            }
            return channels;
        }

        std::optional<ResolvedColor> resolveChannel(
            int id,
            ChannelMap const& channels,
            std::unordered_set<int>& resolving,
            int depth = 0
        ) {
            if (id <= 0 || depth >= 8 || resolving.contains(id)) return std::nullopt;
            if (id == 1010) return ResolvedColor{{0, 0, 0}, {0, 0, 0}};
            if (id == 1011) return ResolvedColor{{255, 255, 255}, {255, 255, 255}};

            auto found = channels.find(id);
            if (found == channels.end()) {
                // Undefined channels render white in the game. Player colors
                // cannot be known from the level string, so they are omitted.
                if (id == 1005 || id == 1006) return std::nullopt;
                return ResolvedColor{};
            }

            auto const& channel = found->second;
            if (channel.playerColor > 0) return std::nullopt;
            if (channel.copyId == 0) {
                return ResolvedColor{
                    channel.from,
                    channel.to,
                    channel.fromOpacity,
                    channel.toOpacity,
                    channel.hasTo,
                    channel.blending,
                };
            }

            resolving.insert(id);
            auto parent = resolveChannel(channel.copyId, channels, resolving, depth + 1);
            resolving.erase(id);
            if (!parent) return std::nullopt;

            parent->from = applyHsv(parent->from, channel.copyHsv);
            parent->to = applyHsv(parent->to, channel.copyHsv);
            if (!channel.copyOpacity) {
                parent->fromOpacity = channel.fromOpacity;
                parent->toOpacity = channel.hasTo ? channel.toOpacity : channel.fromOpacity;
            }
            parent->blending = channel.blending;
            return parent;
        }

        double startChannelWeight(int id) {
            switch (id) {
                case 1000: return 260.0; // BG fills most of the frame.
                case 1001:
                case 1009: return 110.0; // Ground layers.
                case 1013:
                case 1014: return 75.0;  // Middleground layers.
                case 1002: return 45.0;  // Ground line.
                case 1003:
                case 1004: return 30.0;  // Common object colors.
                default: return 0.3;     // Keep unused custom channels negligible.
            }
        }

        void addResolvedColor(
            Histogram& histogram,
            ResolvedColor const& color,
            double weight,
            HsvShift const& hsv = {}
        ) {
            auto blendBoost = color.blending ? 1.12 : 1.0;
            histogram.add(
                applyHsv(color.from, hsv),
                weight * std::clamp(color.fromOpacity, 0.0, 1.0) * blendBoost
            );
            if (color.hasTo) {
                histogram.add(
                    applyHsv(color.to, hsv),
                    weight * 0.45 * std::clamp(color.toOpacity, 0.0, 1.0) * blendBoost
                );
            }
        }

        bool isColorTrigger(int id) {
            switch (id) {
                case 29:
                case 30:
                case 104:
                case 105:
                case 221:
                case 717:
                case 718:
                case 743:
                case 744:
                case 899:
                case 900:
                case 915: return true;
                default: return false;
            }
        }

        int defaultColorTriggerTarget(int id) {
            switch (id) {
                case 29: return 1000;
                case 30: return 1001;
                case 104: return 1002;
                case 105: return 1004;
                case 221: return 1;
                case 717: return 2;
                case 718: return 3;
                case 743: return 4;
                case 744: return 1003;
                default: return 1;
            }
        }

        int modernChannelFromLegacy(int id) {
            switch (id) {
                case 1: return 1005;
                case 2: return 1006;
                case 3: return 1;
                case 4: return 2;
                case 5: return 1007;
                case 6: return 3;
                case 7: return 4;
                case 8: return 1003;
                default: return -1;
            }
        }

        bool looksLikePlainLevel(std::string_view data) {
            auto const headerEnd = data.find(';');
            if (headerEnd == std::string_view::npos) return false;
            auto const header = data.substr(0, headerEnd);
            return header.find("kS") != std::string_view::npos && header.find(',') != std::string_view::npos;
        }

        std::optional<std::string> decompressLevel(
            std::string_view compressed,
            std::shared_ptr<AnalysisJob> const& job
        ) {
            if (compressed.empty() || compressed.size() > std::numeric_limits<unsigned int>::max()) {
                return std::nullopt;
            }

            job->progress.store(0.025f, std::memory_order_relaxed);
            auto decodedResult = geode::utils::base64::decode(
                compressed,
                geode::utils::base64::Base64Variant::Url
            );
            if (decodedResult.isErr() || job->cancelled.load(std::memory_order_relaxed)) {
                return std::nullopt;
            }

            auto decoded = std::move(decodedResult).unwrap();
            if (decoded.empty() || decoded.size() > std::numeric_limits<unsigned int>::max()) {
                return std::nullopt;
            }

            job->progress.store(0.055f, std::memory_order_relaxed);
            unsigned char* inflated = nullptr;
            auto const inflatedSize = cocos2d::ZipUtils::ccInflateMemory(
                decoded.data(),
                static_cast<unsigned int>(decoded.size()),
                &inflated
            );
            if (inflatedSize <= 0 || !inflated) {
                delete[] inflated;
                return std::nullopt;
            }

            std::string output(reinterpret_cast<char const*>(inflated), static_cast<size_t>(inflatedSize));
            delete[] inflated;
            if (!looksLikePlainLevel(output)) return std::nullopt;

            job->progress.store(0.1f, std::memory_order_relaxed);
            return output;
        }

        AnalysisResult analyzeLevel(std::string const& data, std::shared_ptr<AnalysisJob> const& job) {
            AnalysisResult result;
            Histogram histogram;

            job->stage.store(0, std::memory_order_relaxed);
            job->progress.store(0.01f, std::memory_order_relaxed);

            std::string unpacked;
            std::string_view levelData = data;
            if (!looksLikePlainLevel(levelData)) {
                auto decompressed = decompressLevel(data, job);
                if (!decompressed) {
                    result.error = "Could not decompress this level's saved data.";
                    job->progress.store(1.f, std::memory_order_relaxed);
                    job->stage.store(4, std::memory_order_relaxed);
                    return result;
                }
                unpacked = std::move(*decompressed);
                levelData = unpacked;
            }

            if (job->cancelled.load(std::memory_order_relaxed)) return result;

            auto const headerEnd = levelData.find(';');
            auto const header = levelData.substr(0, headerEnd);
            auto const channels = parseStartColors(header);

            std::unordered_map<int, ResolvedColor> resolvedChannels;
            std::unordered_set<int> unavailableChannels;
            resolvedChannels.reserve(channels.size() + 16);
            unavailableChannels.reserve(16);
            for (auto const& [id, channel] : channels) {
                (void)channel;
                std::unordered_set<int> resolving;
                if (auto resolved = resolveChannel(id, channels, resolving)) {
                    resolvedChannels.emplace(id, *resolved);
                } else {
                    unavailableChannels.insert(id);
                }
            }

            auto getResolved = [&](int id) -> std::optional<ResolvedColor> {
                if (id <= 0) return std::nullopt;
                if (unavailableChannels.contains(id)) return std::nullopt;
                if (auto found = resolvedChannels.find(id); found != resolvedChannels.end()) {
                    return found->second;
                }
                std::unordered_set<int> resolving;
                auto resolved = resolveChannel(id, channels, resolving);
                if (resolved) resolvedChannels.emplace(id, *resolved);
                else unavailableChannels.insert(id);
                return resolved;
            };

            // Initial scene colors are always visible even when no ordinary
            // object explicitly serializes their channel IDs.
            for (auto const& [id, color] : resolvedChannels) {
                addResolvedColor(histogram, color, startChannelWeight(id));
            }

            job->stage.store(1, std::memory_order_relaxed);
            job->progress.store(0.12f, std::memory_order_relaxed);

            size_t objectCount = 0;
            size_t position = headerEnd == std::string_view::npos ? levelData.size() : headerEnd + 1;
            size_t nextProgressAt = position;

            while (position < levelData.size() && !job->cancelled.load(std::memory_order_relaxed)) {
                auto end = levelData.find(';', position);
                if (end == std::string_view::npos) end = levelData.size();
                auto const object = levelData.substr(position, end - position);

                int red = -1;
                int green = -1;
                int blue = -1;
                int objectId = 0;
                int targetChannel = -1;
                int mainChannel = -1;
                int secondaryChannel = -1;
                int legacyChannel = -1;
                int copiedChannel = 0;
                int pulseTarget = 0;
                double scale = 1.0;
                double duration = 0.0;
                double pulseFadeIn = 0.0;
                double pulseHold = 0.0;
                double pulseFadeOut = 0.0;
                bool mainHsvEnabled = false;
                bool secondaryHsvEnabled = false;
                bool pulseHsvMode = false;
                bool pulseTargetsGroup = false;
                std::string_view mainHsvText;
                std::string_view secondaryHsvText;
                std::string_view copiedHsvText;

                visitPairs(object, ',', [&](std::string_view keyText, std::string_view valueText) {
                    int key = 0;
                    int value = 0;
                    if (!parseInt(keyText, key)) return;

                    double decimal = 0.0;
                    switch (key) {
                        case 10:
                            if (parseDouble(valueText, decimal)) duration = std::max(0.0, decimal);
                            return;
                        case 32:
                            if (parseDouble(valueText, decimal)) scale = std::abs(decimal);
                            return;
                        case 43: mainHsvText = valueText; return;
                        case 44: secondaryHsvText = valueText; return;
                        case 45:
                            if (parseDouble(valueText, decimal)) pulseFadeIn = std::max(0.0, decimal);
                            return;
                        case 46:
                            if (parseDouble(valueText, decimal)) pulseHold = std::max(0.0, decimal);
                            return;
                        case 47:
                            if (parseDouble(valueText, decimal)) pulseFadeOut = std::max(0.0, decimal);
                            return;
                        case 49: copiedHsvText = valueText; return;
                        default: break;
                    }

                    if (!parseInt(valueText, value)) return;

                    switch (key) {
                        case 1: objectId = value; break;
                        case 7: red = value; break;
                        case 8: green = value; break;
                        case 9: blue = value; break;
                        case 19: legacyChannel = value; break;
                        case 21: mainChannel = value; break;
                        case 22: secondaryChannel = value; break;
                        case 23: targetChannel = value; break;
                        case 41: mainHsvEnabled = value != 0; break;
                        case 42: secondaryHsvEnabled = value != 0; break;
                        case 48: pulseHsvMode = value != 0; break;
                        case 50: copiedChannel = value; break;
                        case 51: pulseTarget = value; break;
                        case 52: pulseTargetsGroup = value != 0; break;
                        default: break;
                    }
                });

                auto const colorTrigger = isColorTrigger(objectId);
                auto const pulseTrigger = objectId == 1006;
                auto const areaWeight = std::clamp(scale * scale, 0.2, 20.0);

                if (auto modernLegacy = modernChannelFromLegacy(legacyChannel); modernLegacy > 0) {
                    mainChannel = modernLegacy;
                    secondaryChannel = -1;
                }

                auto sampleChannel = [&](int id, double weight, HsvShift const& hsv = HsvShift{}) {
                    if (auto resolved = getResolved(id)) {
                        addResolvedColor(histogram, *resolved, weight, hsv);
                    }
                };

                // Actual object colors, including per-object HSV shifts. Scale
                // approximates how much screen area a decoration can occupy.
                if (!colorTrigger && !pulseTrigger) {
                    if (mainChannel > 0) {
                        sampleChannel(
                            mainChannel,
                            areaWeight,
                            mainHsvEnabled ? parseHsv(mainHsvText) : HsvShift{}
                        );
                    } else {
                        // Most omitted base colors inherit OBJ. Keep this at a
                        // lower weight because object metadata is not serialized.
                        sampleChannel(1004, areaWeight * 0.35);
                    }
                    if (secondaryChannel > 0) {
                        sampleChannel(
                            secondaryChannel,
                            areaWeight * 0.58,
                            secondaryHsvEnabled ? parseHsv(secondaryHsvText) : HsvShift{}
                        );
                    }
                }

                // Color triggers permanently change a channel until another
                // trigger replaces it, so include both direct and copied colors.
                if (colorTrigger) {
                    if (targetChannel <= 0) targetChannel = defaultColorTriggerTarget(objectId);
                    auto const triggerWeight = (targetChannel == 1000 ? 95.0 : 12.0) *
                        (1.0 + std::min(duration, 5.0) * 0.08);
                    if (copiedChannel > 0) {
                        sampleChannel(copiedChannel, triggerWeight, parseHsv(copiedHsvText));
                    } else if (red >= 0 && green >= 0 && blue >= 0) {
                        histogram.add({
                            static_cast<uint8_t>(std::clamp(red, 0, 255)),
                            static_cast<uint8_t>(std::clamp(green, 0, 255)),
                            static_cast<uint8_t>(std::clamp(blue, 0, 255)),
                        }, triggerWeight);
                    }
                }

                // Pulse colors are temporary, so their weight follows their
                // fade/hold duration. Group pulses still contribute their RGB.
                if (pulseTrigger) {
                    auto const pulseDuration = pulseFadeIn + pulseHold + pulseFadeOut;
                    auto const pulseWeight = 5.0 + std::min(pulseDuration, 8.0) * 2.0;
                    if (pulseHsvMode && !pulseTargetsGroup && pulseTarget > 0) {
                        sampleChannel(pulseTarget, pulseWeight, parseHsv(copiedHsvText));
                    } else if (red >= 0 && green >= 0 && blue >= 0) {
                        histogram.add({
                            static_cast<uint8_t>(std::clamp(red, 0, 255)),
                            static_cast<uint8_t>(std::clamp(green, 0, 255)),
                            static_cast<uint8_t>(std::clamp(blue, 0, 255)),
                        }, pulseWeight);
                    }
                }

                ++objectCount;
                position = end + 1;

                if (position >= nextProgressAt) {
                    auto const fraction = levelData.empty()
                        ? 1.0
                        : static_cast<double>(position) / static_cast<double>(levelData.size());
                    job->progress.store(static_cast<float>(0.12 + 0.58 * fraction), std::memory_order_relaxed);
                    nextProgressAt = position + 65536;
                }
            }

            if (job->cancelled.load(std::memory_order_relaxed)) return result;

            if (objectCount == 0) {
                result.error = "No level objects were found after decompression.";
                job->progress.store(1.f, std::memory_order_relaxed);
                job->stage.store(4, std::memory_order_relaxed);
                return result;
            }

            if (histogram.samples == 0) {
                histogram.add({24, 24, 32}, 1.0);
                histogram.add({96, 96, 112}, 0.35);
            }

            job->stage.store(2, std::memory_order_relaxed);
            job->progress.store(0.76f, std::memory_order_relaxed);

            auto const dominant = histogram.dominant(96);
            double totalWeight = 0.0;
            double averageLuminance = 0.0;
            double chromaticA = 0.0;
            double chromaticB = 0.0;
            double chromaticWeight = 0.0;

            for (auto const& sample : dominant) {
                auto const lab = toLab(sample.rgb);
                auto const chroma = std::hypot(lab.a, lab.b);
                auto const chromaFactor = std::clamp(chroma / 0.12, 0.0, 1.0);
                totalWeight += sample.weight;
                averageLuminance += luminance(sample.rgb) * sample.weight;
                chromaticA += lab.a * sample.weight * chromaFactor;
                chromaticB += lab.b * sample.weight * chromaFactor;
                chromaticWeight += sample.weight * chromaFactor;
            }
            if (totalWeight > 0.0) {
                averageLuminance /= totalWeight;
            }

            constexpr double pi = 3.14159265358979323846;
            // Neutral levels have no meaningful complementary hue. A stable
            // warm reference produces a comfortable cyan family for them.
            auto dominantHue = chromaticWeight < totalWeight * 0.045
                ? 15.0
                : std::atan2(chromaticB, chromaticA) * 180.0 / pi;
            if (dominantHue < 0.0) dominantHue += 360.0;
            auto const levelIsDark = averageLuminance < 0.38;

            auto visibilityScore = [&](Rgb candidate) {
                double weighted = 0.0;
                double weight = 0.0;
                double worstImportant = 14.0;
                auto const importantThreshold = totalWeight * 0.008;
                auto const candidateLab = toLab(candidate);

                for (auto const& sample : dominant) {
                    auto const contrast = contrastRatio(candidate, sample.rgb);
                    auto const sampleLab = toLab(sample.rgb);
                    auto const deltaL = (candidateLab.l - sampleLab.l) * 0.85;
                    auto const deltaA = candidateLab.a - sampleLab.a;
                    auto const deltaB = candidateLab.b - sampleLab.b;
                    auto const distance = std::sqrt(deltaL * deltaL + deltaA * deltaA + deltaB * deltaB);
                    auto const colorSeparation = 2.2 * std::clamp(distance / 0.28, 0.0, 1.0);
                    auto const clarity = std::min(contrast, 10.0) + colorSeparation;
                    weighted += clarity * sample.weight;
                    weight += sample.weight;
                    if (sample.weight >= importantThreshold) {
                        worstImportant = std::min(worstImportant, clarity);
                    }
                }
                auto const average = weight > 0.0 ? weighted / weight : 1.0;
                return 0.72 * average + 0.28 * worstImportant;
            };

            auto buildPalette = [&](double lightness, double chroma, double preferredHue, bool maximum) {
                struct Candidate {
                    Rgb color;
                    double hue = 0.0;
                    double score = 0.0;
                };

                std::vector<Candidate> candidates;
                candidates.reserve(72);
                for (int hue = 0; hue < 360; hue += 5) {
                    auto const color = gamutMappedLch(lightness, chroma, static_cast<double>(hue));
                    // Contrast stays the main criterion. This complementary-hue
                    // preference is strong enough for different levels to stop
                    // collapsing to the same RGB choices.
                    auto const preference = (std::cos((hue - preferredHue) * pi / 180.0) + 1.0) * 0.75;
                    candidates.push_back({color, static_cast<double>(hue), visibilityScore(color) + preference});
                }
                std::sort(candidates.begin(), candidates.end(), [](auto const& lhs, auto const& rhs) {
                    return lhs.score > rhs.score;
                });

                std::array<Candidate, 3> selected{};
                size_t selectedCount = 0;
                for (auto const& candidate : candidates) {
                    bool separated = true;
                    for (size_t index = 0; index < selectedCount; ++index) {
                        if (hueDistance(candidate.hue, selected[index].hue) < 62.0) {
                            separated = false;
                            break;
                        }
                    }
                    if (!separated) continue;
                    selected[selectedCount++] = candidate;
                    if (selectedCount == selected.size()) break;
                }

                if (selectedCount < selected.size()) {
                    for (; selectedCount < selected.size(); ++selectedCount) {
                        selected[selectedCount] = candidates[selectedCount % candidates.size()];
                    }
                }

                auto neutral = levelIsDark ? Rgb{255, 255, 255} : Rgb{8, 8, 12};
                if (maximum) {
                    auto const opposite = levelIsDark ? Rgb{8, 8, 12} : Rgb{255, 255, 255};
                    if (visibilityScore(opposite) > visibilityScore(neutral)) neutral = opposite;
                }

                Palette palette;
                palette.colors = {
                    selected[0].color,
                    selected[1].color,
                    selected[2].color,
                    selected[1].color,
                    selected[2].color,
                    neutral,
                    selected[2].color,
                    selected[1].color,
                };
                return palette;
            };

            job->stage.store(3, std::memory_order_relaxed);
            job->progress.store(0.9f, std::memory_order_relaxed);

            auto const oppositeHue = std::fmod(dominantHue + 180.0, 360.0);
            result.palettes[0] = buildPalette(levelIsDark ? 0.82 : 0.34, 0.20, oppositeHue, false);
            result.palettes[1] = buildPalette(levelIsDark ? 0.75 : 0.40, 0.13, std::fmod(oppositeHue + 70.0, 360.0), false);
            result.palettes[2] = buildPalette(levelIsDark ? 0.94 : 0.20, 0.25, std::fmod(oppositeHue + 290.0, 360.0), true);
            result.objectCount = objectCount;
            result.sampledColors = histogram.samples;

            job->progress.store(1.f, std::memory_order_relaxed);
            job->stage.store(4, std::memory_order_relaxed);
            return result;
        }

        uint64_t hashLevel(std::string_view data) {
            uint64_t hash = 14695981039346656037ull;
            for (auto const value : data) {
                hash ^= static_cast<uint8_t>(value);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string cacheKey(uint64_t hash) {
            // v3 adds copied channels, HSV, trigger colors and object-area weighting.
            return fmt::format("smart-contrast-cache-v3-{:016x}", hash);
        }

        int64_t packColor(Rgb color) {
            return
                (static_cast<int64_t>(color.r) << 16) |
                (static_cast<int64_t>(color.g) << 8) |
                static_cast<int64_t>(color.b);
        }

        Rgb unpackColor(int64_t color) {
            return {
                static_cast<uint8_t>((color >> 16) & 0xff),
                static_cast<uint8_t>((color >> 8) & 0xff),
                static_cast<uint8_t>(color & 0xff),
            };
        }

        void saveCache(uint64_t hash, AnalysisResult const& result) {
            std::vector<int64_t> values;
            values.reserve(28);
            values.push_back(3);
            values.push_back(static_cast<int64_t>(result.objectCount));
            values.push_back(static_cast<int64_t>(result.sampledColors));
            for (auto const& palette : result.palettes) {
                for (auto const color : palette.colors) values.push_back(packColor(color));
            }
            Mod::get()->setSavedValue(cacheKey(hash), values);
        }

        std::optional<AnalysisResult> loadCache(uint64_t hash) {
            auto const key = cacheKey(hash);
            if (!Mod::get()->hasSavedValue(key)) return std::nullopt;
            auto const values = Mod::get()->getSavedValue<std::vector<int64_t>>(key);
            if (values.size() != 27 || values.front() != 3) return std::nullopt;

            AnalysisResult result;
            result.objectCount = static_cast<size_t>(std::max<int64_t>(values[1], 0));
            result.sampledColors = static_cast<size_t>(std::max<int64_t>(values[2], 0));
            size_t position = 3;
            for (auto& palette : result.palettes) {
                for (auto& color : palette.colors) color = unpackColor(values[position++]);
            }
            return result;
        }

        void applyPalette(Palette const& palette) {
            for (size_t index = 0; index < kColorSettingKeys.size(); ++index) {
                auto const color = palette.colors[index];
                Mod::get()->setSettingValue<ccColor4B>(
                    kColorSettingKeys[index],
                    {color.r, color.g, color.b, 255}
                );
            }
        }

        void addLevel(
            std::vector<LevelEntry>& output,
            std::unordered_set<std::string>& identities,
            GJGameLevel* level,
            bool editorLevel
        ) {
            if (!level || level->m_levelString.empty()) return;

            auto const id = static_cast<int>(level->m_levelID);
            auto name = std::string(level->m_levelName.c_str());
            if (name.empty()) name = "Unnamed level";

            auto identity = editorLevel
                ? fmt::format("editor:{}:{}", name, level->m_levelString.size())
                : id > 0
                    ? fmt::format("online:{}", id)
                    : fmt::format("saved:{}:{}", name, level->m_levelString.size());
            if (!identities.insert(identity).second) return;

            output.push_back({level, std::move(name), id, editorLevel});
        }

        std::vector<LevelEntry> collectLevels() {
            std::vector<LevelEntry> levels;
            std::unordered_set<std::string> identities;

            if (auto* local = LocalLevelManager::sharedState(); local && local->m_localLevels) {
                for (auto* level : geode::cocos::CCArrayExt<GJGameLevel*, false>(local->m_localLevels)) {
                    addLevel(levels, identities, level, true);
                }
            }

            if (auto* manager = GameLevelManager::sharedState(); manager && manager->m_storedLevels) {
                for (auto const& [key, level] : geode::cocos::CCDictionaryExt<std::string_view, GJGameLevel*, false>(manager->m_storedLevels)) {
                    (void)key;
                    addLevel(levels, identities, level, false);
                }
            }

            std::sort(levels.begin(), levels.end(), [](auto const& lhs, auto const& rhs) {
                auto first = lhs.name;
                auto second = rhs.name;
                std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(second.begin(), second.end(), second.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return first < second;
            });
            return levels;
        }

        class PalettePopup;
        class AnalysisPopup;

        class PalettePopup final : public Popup {
        protected:
            AnalysisResult m_result;
            std::string m_levelName;
            std::string m_levelData;
            uint64_t m_hash = 0;

            static NineSlice* createSwatch(Rgb color, CCSize size) {
                auto* swatch = NineSlice::create("square02b_001.png");
                swatch->setColor({color.r, color.g, color.b});
                swatch->setOpacity(255);
                swatch->setContentSize(size);
                swatch->setAnchorPoint({.5f, .5f});
                return swatch;
            }

            bool init(std::string levelName, std::string levelData, uint64_t hash, AnalysisResult result, bool cached) {
                if (!Popup::init(450.f, 285.f)) return false;
                m_noElasticity = true;
                m_levelName = std::move(levelName);
                m_levelData = std::move(levelData);
                m_hash = hash;
                m_result = std::move(result);
                this->setTitle("Smart Contrast");

                auto* subtitle = CCLabelBMFont::create(
                    fmt::format(
                        "{}  -  {} objects, {} color samples{}",
                        m_levelName,
                        m_result.objectCount,
                        m_result.sampledColors,
                        cached ? "  (cached)" : ""
                    ).c_str(),
                    "bigFont.fnt"
                );
                subtitle->setScale(.38f);
                subtitle->limitLabelWidth(390.f, .38f, .2f);
                m_mainLayer->addChildAtPosition(subtitle, Anchor::Top, ccp(0, -48));

                constexpr std::array<char const*, 3> names = {"Balanced", "Comfort", "Maximum"};
                constexpr std::array<char const*, 3> buttonTextures = {
                    "GJ_button_01.png", "GJ_button_05.png", "GJ_button_06.png"
                };

                for (size_t paletteIndex = 0; paletteIndex < m_result.palettes.size(); ++paletteIndex) {
                    auto* card = CCNode::create();
                    card->setContentSize({126.f, 155.f});
                    card->setAnchorPoint({.5f, .5f});
                    card->setPosition({96.f + 129.f * static_cast<float>(paletteIndex), 147.f});

                    auto* cardBackground = NineSlice::create("square02b_001.png");
                    cardBackground->setColor({0, 0, 0});
                    cardBackground->setOpacity(90);
                    cardBackground->setContentSize(card->getContentSize());
                    card->addChildAtPosition(cardBackground, Anchor::Center);

                    auto* name = CCLabelBMFont::create(names[paletteIndex], "goldFont.fnt");
                    name->setScale(.46f);
                    card->addChildAtPosition(name, Anchor::Top, ccp(0, -15));

                    for (size_t colorIndex = 0; colorIndex < 8; ++colorIndex) {
                        auto const column = static_cast<float>(colorIndex % 4);
                        auto const row = static_cast<float>(colorIndex / 4);
                        auto* swatch = createSwatch(m_result.palettes[paletteIndex].colors[colorIndex], {24.f, 26.f});
                        swatch->setPosition({22.f + column * 27.f, 96.f - row * 29.f});
                        card->addChild(swatch);
                    }

                    auto* applySprite = ButtonSprite::create(
                        "Apply", "bigFont.fnt", buttonTextures[paletteIndex], .62f
                    );
                    applySprite->setScale(.74f);
                    auto* applyButton = CCMenuItemExt::createSpriteExtra(
                        applySprite,
                        [this, paletteIndex](CCMenuItemSpriteExtra*) {
                            applyPalette(m_result.palettes[paletteIndex]);
                            Notification::create(
                                fmt::format("{} palette applied", paletteIndex == 0 ? "Balanced" : paletteIndex == 1 ? "Comfort" : "Maximum"),
                                NotificationIcon::Success
                            )->show();
                            this->onClose(nullptr);
                        }
                    );
                    auto* cardMenu = CCMenu::create();
                    cardMenu->setContentSize(card->getContentSize());
                    cardMenu->setAnchorPoint({0.f, 0.f});
                    cardMenu->setPosition({0.f, 0.f});
                    cardMenu->addChildAtPosition(applyButton, Anchor::Bottom, ccp(0, 14));
                    card->addChild(cardMenu, 2);
                    m_mainLayer->addChild(card);
                }

                auto* hint = CCLabelBMFont::create("8 swatches: hitboxes, player and trajectory", "bigFont.fnt");
                hint->setScale(.3f);
                hint->setOpacity(180);
                m_mainLayer->addChildAtPosition(hint, Anchor::Bottom, ccp(0, 26));

                auto* reanalyzeSprite = ButtonSprite::create("Reanalyze", "bigFont.fnt", "GJ_button_04.png", .55f);
                reanalyzeSprite->setScale(.65f);
                auto* reanalyze = CCMenuItemExt::createSpriteExtra(
                    reanalyzeSprite,
                    [this](CCMenuItemSpriteExtra*) {
                        auto name = m_levelName;
                        auto data = m_levelData;
                        auto hash = m_hash;
                        this->onClose(nullptr);
                        startAnalysis(std::move(name), std::move(data), hash);
                    }
                );
                m_buttonMenu->addChildAtPosition(reanalyze, Anchor::Bottom, ccp(0, 7));
                return true;
            }

        public:
            static PalettePopup* create(
                std::string levelName,
                std::string levelData,
                uint64_t hash,
                AnalysisResult result,
                bool cached
            ) {
                auto* popup = new PalettePopup();
                if (popup->init(std::move(levelName), std::move(levelData), hash, std::move(result), cached)) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }

            static void startAnalysis(std::string levelName, std::string levelData, uint64_t hash);
        };

        class AnalysisPopup final : public Popup {
        protected:
            std::shared_ptr<AnalysisJob> m_job;
            std::string m_levelName;
            std::string m_levelData;
            uint64_t m_hash = 0;
            NineSlice* m_progressFill = nullptr;
            CCLabelBMFont* m_progressLabel = nullptr;
            CCLabelBMFont* m_stageLabel = nullptr;
            bool m_delivered = false;
            std::jthread m_worker;

            ~AnalysisPopup() override {
                if (m_job && !m_job->complete.load(std::memory_order_acquire)) {
                    m_job->cancelled.store(true, std::memory_order_release);
                }
                if (m_worker.joinable()) m_worker.join();
            }

            bool init(std::string levelName, std::string levelData, uint64_t hash) {
                if (!Popup::init(380.f, 175.f)) return false;
                m_noElasticity = true;
                m_levelName = std::move(levelName);
                m_levelData = std::move(levelData);
                m_hash = hash;
                m_job = std::make_shared<AnalysisJob>();
                this->setTitle("Analyzing level");

                auto* levelLabel = CCLabelBMFont::create(m_levelName.c_str(), "goldFont.fnt");
                levelLabel->setScale(.5f);
                levelLabel->limitLabelWidth(320.f, .5f, .25f);
                m_mainLayer->addChildAtPosition(levelLabel, Anchor::Center, ccp(0, 37));

                m_stageLabel = CCLabelBMFont::create("Preparing saved level...", "bigFont.fnt");
                m_stageLabel->setScale(.38f);
                m_mainLayer->addChildAtPosition(m_stageLabel, Anchor::Center, ccp(0, 7));

                auto* progressBackground = NineSlice::create("square02b_001.png");
                progressBackground->setColor({0, 0, 0});
                progressBackground->setOpacity(120);
                progressBackground->setContentSize({300.f, 18.f});
                progressBackground->setAnchorPoint({.5f, .5f});
                m_mainLayer->addChildAtPosition(progressBackground, Anchor::Center, ccp(0, -25));

                m_progressFill = NineSlice::create("square02b_001.png");
                m_progressFill->setColor({60, 220, 120});
                m_progressFill->setOpacity(255);
                m_progressFill->setAnchorPoint({0.f, .5f});
                m_progressFill->setContentSize({1.f, 12.f});
                m_progressFill->setPosition({3.f, 9.f});
                progressBackground->addChild(m_progressFill);

                m_progressLabel = CCLabelBMFont::create("0%", "bigFont.fnt");
                m_progressLabel->setScale(.32f);
                m_mainLayer->addChildAtPosition(m_progressLabel, Anchor::Center, ccp(0, -49));

                auto job = m_job;
                auto data = m_levelData;
                m_worker = std::jthread([job, data = std::move(data)]() mutable {
                    AnalysisResult result;
                    try {
                        result = analyzeLevel(data, job);
                    } catch (std::exception const&) {
                        result.error = "Unexpected error while reading this level.";
                    } catch (...) {
                        result.error = "Unexpected error while reading this level.";
                    }
                    if (job->cancelled.load(std::memory_order_acquire)) return;
                    {
                        std::lock_guard lock(job->resultMutex);
                        job->result = std::move(result);
                    }
                    job->complete.store(true, std::memory_order_release);
                });

                this->schedule(schedule_selector(AnalysisPopup::updateProgress), 0.04f);
                return true;
            }

            void updateProgress(float) {
                auto const progress = std::clamp(m_job->progress.load(std::memory_order_relaxed), 0.f, 1.f);
                m_progressFill->setContentSize({std::max(1.f, 294.f * progress), 12.f});
                m_progressLabel->setString(fmt::format("{}%", static_cast<int>(std::lround(progress * 100.f))).c_str());

                constexpr std::array<char const*, 5> stages = {
                    "Decompressing saved level...",
                    "Mapping colors and objects...",
                    "Scoring contrast...",
                    "Building three palettes...",
                    "Done",
                };
                auto const stage = std::clamp(m_job->stage.load(std::memory_order_relaxed), 0, 4);
                m_stageLabel->setString(stages[stage]);

                if (!m_delivered && m_job->complete.load(std::memory_order_acquire)) {
                    m_delivered = true;
                    std::optional<AnalysisResult> result;
                    {
                        std::lock_guard lock(m_job->resultMutex);
                        result = m_job->result;
                    }
                    if (!result) return;

                    auto error = result->error;
                    auto levelName = m_levelName;
                    auto levelData = std::move(m_levelData);
                    auto hash = m_hash;
                    this->unschedule(schedule_selector(AnalysisPopup::updateProgress));
                    this->onClose(nullptr);
                    if (!error.empty()) {
                        FLAlertLayer::create("Smart Contrast", error.c_str(), "OK")->show();
                        return;
                    }

                    saveCache(hash, *result);
                    if (auto* popup = PalettePopup::create(
                        std::move(levelName), std::move(levelData), hash, std::move(*result), false
                    )) {
                        popup->show();
                    }
                }
            }

        public:
            static AnalysisPopup* create(std::string levelName, std::string levelData, uint64_t hash) {
                auto* popup = new AnalysisPopup();
                if (popup->init(std::move(levelName), std::move(levelData), hash)) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }
        };

        void PalettePopup::startAnalysis(std::string levelName, std::string levelData, uint64_t hash) {
            AnalysisPopup::create(std::move(levelName), std::move(levelData), hash)->show();
        }

        class LevelPickerPopup final : public Popup {
        protected:
            std::vector<LevelEntry> m_levels;
            ScrollLayer* m_list = nullptr;
            TextInput* m_search = nullptr;
            CCLabelBMFont* m_emptyLabel = nullptr;

            bool init() override {
                if (!Popup::init(440.f, 285.f)) return false;
                m_noElasticity = true;
                m_levels = collectLevels();
                this->setTitle("Smart Contrast - Select Level");

                m_search = TextInput::create(346.f, "Search saved levels...");
                m_search->setTextAlign(TextInputAlign::Left);
                m_search->setCommonFilter(CommonFilter::Any);
                m_search->setMaxCharCount(80);
                m_search->setScale(.8f);
                m_search->setCallback([this](std::string const&) { this->rebuildList(); });
                m_mainLayer->addChildAtPosition(m_search, Anchor::Top, ccp(0, -48));

                auto* listBackground = CCLayerColor::create({0, 0, 0, 80});
                listBackground->setContentSize({370.f, 185.f});
                listBackground->ignoreAnchorPointForPosition(false);
                m_mainLayer->addChildAtPosition(listBackground, Anchor::Center, ccp(0, -22));

                m_list = ScrollLayer::create({360.f, 177.f});
                m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
                listBackground->addChildAtPosition(m_list, Anchor::BottomLeft, ccp(5.f, 4.f));

                m_emptyLabel = CCLabelBMFont::create("No downloaded level data found", "bigFont.fnt");
                m_emptyLabel->setScale(.4f);
                m_emptyLabel->setOpacity(190);
                listBackground->addChildAtPosition(m_emptyLabel, Anchor::Center);

                this->rebuildList();
                return true;
            }

            void rebuildList() {
                if (!m_list) return;
                m_list->m_contentLayer->removeAllChildren();
                auto query = std::string(m_search ? m_search->getString().c_str() : "");
                std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                size_t visible = 0;
                for (size_t index = 0; index < m_levels.size(); ++index) {
                    auto searchName = m_levels[index].name;
                    std::transform(searchName.begin(), searchName.end(), searchName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!query.empty() && searchName.find(query) == std::string::npos) continue;

                    auto const& level = m_levels[index];
                    auto caption = level.editorLevel
                        ? fmt::format("{}  [Editor]", level.name)
                        : level.id > 0
                            ? fmt::format("{}  [ID {}]", level.name, level.id)
                            : fmt::format("{}  [Saved]", level.name);
                    auto* sprite = ButtonSprite::create(caption.c_str(), 340, 0, .48f, true);
                    auto* button = CCMenuItemExt::createSpriteExtra(
                        sprite,
                        [this, index](CCMenuItemSpriteExtra*) { this->selectLevel(index); }
                    );
                    auto* row = CCMenu::create();
                    row->setContentSize({355.f, 34.f});
                    row->setAnchorPoint({.5f, .5f});
                    row->addChildAtPosition(button, Anchor::Center);
                    m_list->m_contentLayer->addChild(row);
                    ++visible;
                }

                m_emptyLabel->setVisible(visible == 0);
                m_list->m_contentLayer->updateLayout();
                m_list->scrollToTop();
            }

            void selectLevel(size_t index) {
                if (index >= m_levels.size()) return;
                auto const& entry = m_levels[index];
                if (!entry.level || entry.level->m_levelString.empty()) {
                    Notification::create("This level has no local data", NotificationIcon::Error)->show();
                    return;
                }

                // This is the only engine-owned read used by the analyzer. From
                // this point on, the worker sees a stable std::string copy only.
                auto data = std::string(entry.level->m_levelString.c_str());
                auto const hash = hashLevel(data);
                auto name = entry.name;

                if (auto cached = loadCache(hash)) {
                    PalettePopup::create(std::move(name), std::move(data), hash, std::move(*cached), true)->show();
                } else {
                    AnalysisPopup::create(std::move(name), std::move(data), hash)->show();
                }
            }

        public:
            static LevelPickerPopup* create() {
                auto* popup = new LevelPickerPopup();
                if (popup->init()) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }
        };
    }

    void openLevelPicker() {
        auto* popup = LevelPickerPopup::create();
        if (popup) popup->show();
    }
}

$on_mod(Loaded) {
    ButtonSettingPressedEventV3(Mod::get(), "smart-contrast").listen([](std::string_view buttonKey) {
        if (buttonKey == "analyze") cleanfeed::smart_contrast::openLevelPicker();
    }).leak();
}
