#include "VideoCompositor.h"

#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/RenderStates.hpp>
#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/Graphics/Shader.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/System/Angle.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr int MaximumCubeLutSize = 128;
    constexpr std::uintmax_t MaximumCubeLutFileBytes = 64U * 1024U * 1024U;
    constexpr std::size_t MaximumCachedCubeLuts = 12;

    struct CubeLutFileStamp
    {
        bool                            available = false;
        std::filesystem::file_time_type modificationTime{};
        std::uintmax_t                  byteCount = 0;

        bool operator==(const CubeLutFileStamp& other) const = default;
    };

    struct CubeLutFileStatus
    {
        CubeLutFileStamp stamp;
        std::string      error;
    };

    struct CachedCubeLut
    {
        CubeLutFileStamp                        stamp;
        bool                                    hasStamp = false;
        std::shared_ptr<const weasel::CubeLut>  lut;
        std::string                             error;
        std::uint64_t                           revision = 0;
        std::uint64_t                           lastUse = 0;
    };

    struct CubeLutCache
    {
        std::unordered_map<std::string, CachedCubeLut> entries;
        std::mutex                                     mutex;
        std::uint64_t                                  useCounter = 0;
        std::uint64_t                                  revisionCounter = 0;
    };

    CubeLutCache& CachedCubeLuts()
    {
        static CubeLutCache cache;
        return cache;
    }

    std::string_view Trim(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        {
            value.remove_suffix(1);
        }
        return value;
    }

    std::vector<std::string_view> SplitWhitespace(std::string_view value)
    {
        std::vector<std::string_view> tokens;
        while (!value.empty())
        {
            value = Trim(value);
            if (value.empty())
            {
                break;
            }

            std::size_t end = 0;
            while (end < value.size() && !std::isspace(static_cast<unsigned char>(value[end])))
            {
                ++end;
            }
            tokens.push_back(value.substr(0, end));
            value.remove_prefix(end);
        }
        return tokens;
    }

    bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>(left[index]))
                != std::tolower(static_cast<unsigned char>(right[index])))
            {
                return false;
            }
        }
        return true;
    }

    bool ParseFiniteFloat(std::string_view token, float& value)
    {
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value,
                                                   std::chars_format::general);
        return error == std::errc{} && end == token.data() + token.size() && std::isfinite(value);
    }

    bool ParseLutSize(std::string_view token, int& value)
    {
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        return error == std::errc{} && end == token.data() + token.size()
            && value >= 2 && value <= MaximumCubeLutSize;
    }

    bool ParseRgbTriplet(const std::vector<std::string_view>& tokens, std::array<float, 3>& value)
    {
        return tokens.size() == 3
            && ParseFiniteFloat(tokens[0], value[0])
            && ParseFiniteFloat(tokens[1], value[1])
            && ParseFiniteFloat(tokens[2], value[2]);
    }

    std::filesystem::path NormalizeLutPath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return error ? path.lexically_normal() : absolute.lexically_normal();
    }

    CubeLutFileStatus GetCubeLutFileStatus(const std::filesystem::path& path)
    {
        CubeLutFileStatus result;
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::status(path, error);
        if (error)
        {
            result.error = "Could not inspect the LUT file: " + error.message();
            return result;
        }
        if (!std::filesystem::exists(status))
        {
            result.error = "The LUT file was not found.";
            return result;
        }
        if (!std::filesystem::is_regular_file(status))
        {
            result.error = "The LUT path is not a regular file.";
            return result;
        }

        result.stamp.modificationTime = std::filesystem::last_write_time(path, error);
        if (error)
        {
            result.error = "Could not read the LUT modification time: " + error.message();
            return result;
        }
        result.stamp.byteCount = std::filesystem::file_size(path, error);
        if (error)
        {
            result.error = "Could not read the LUT file size: " + error.message();
            return result;
        }
        result.stamp.available = true;
        return result;
    }

    bool LoadCubeLut(const std::filesystem::path& path, weasel::CubeLut& lut, std::string& error)
    {
        std::error_code fileSizeError;
        const std::uintmax_t fileSize = std::filesystem::file_size(path, fileSizeError);
        if (!fileSizeError && fileSize > MaximumCubeLutFileBytes)
        {
            error = "The LUT file is larger than 64 MiB.";
            return false;
        }

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            error = "Could not open the LUT file.";
            return false;
        }

        int size = 0;
        bool hasSize = false;
        bool hasDomainMinimum = false;
        bool hasDomainMaximum = false;
        bool sawData = false;
        std::array<float, 3> domainMinimum = { 0.0f, 0.0f, 0.0f };
        std::array<float, 3> domainMaximum = { 1.0f, 1.0f, 1.0f };
        std::vector<std::array<float, 3>> values;
        std::string line;
        std::size_t lineNumber = 0;
        std::size_t expectedValueCount = 0;

        const auto lineError = [&error, &lineNumber](const std::string& message)
        {
            error = "LUT line " + std::to_string(lineNumber) + ": " + message;
        };

        while (std::getline(stream, line))
        {
            ++lineNumber;
            std::string_view content(line);
            if (lineNumber == 1 && content.size() >= 3
                && static_cast<unsigned char>(content[0]) == 0xEF
                && static_cast<unsigned char>(content[1]) == 0xBB
                && static_cast<unsigned char>(content[2]) == 0xBF)
            {
                content.remove_prefix(3);
            }
            const std::size_t commentStart = content.find('#');
            if (commentStart != std::string_view::npos)
            {
                content = content.substr(0, commentStart);
            }
            content = Trim(content);
            if (content.empty())
            {
                continue;
            }

            const std::vector<std::string_view> tokens = SplitWhitespace(content);
            if (tokens.empty())
            {
                continue;
            }

            const std::string_view keyword = tokens.front();
            if (EqualsIgnoringAsciiCase(keyword, "TITLE"))
            {
                if (sawData)
                {
                    lineError("TITLE appears after table data.");
                    return false;
                }
                continue;
            }
            if (EqualsIgnoringAsciiCase(keyword, "LUT_1D_SIZE"))
            {
                lineError("LUT_1D_SIZE is not supported; choose a 3D .cube LUT.");
                return false;
            }
            if (EqualsIgnoringAsciiCase(keyword, "LUT_3D_SIZE"))
            {
                if (sawData || hasSize || tokens.size() != 2 || !ParseLutSize(tokens[1], size))
                {
                    lineError("LUT_3D_SIZE must appear once before data and be an integer from 2 to 128.");
                    return false;
                }
                hasSize = true;
                expectedValueCount = static_cast<std::size_t>(size) * static_cast<std::size_t>(size)
                    * static_cast<std::size_t>(size);
                values.reserve(expectedValueCount);
                continue;
            }
            if (EqualsIgnoringAsciiCase(keyword, "DOMAIN_MIN") || EqualsIgnoringAsciiCase(keyword, "DOMAIN_MAX"))
            {
                if (sawData || tokens.size() != 4)
                {
                    lineError("DOMAIN_MIN and DOMAIN_MAX require three values before table data.");
                    return false;
                }
                const std::vector<std::string_view> rgbTokens(tokens.begin() + 1, tokens.end());
                std::array<float, 3> domain{};
                if (!ParseRgbTriplet(rgbTokens, domain))
                {
                    lineError("DOMAIN_MIN and DOMAIN_MAX must contain finite numeric values.");
                    return false;
                }
                if (EqualsIgnoringAsciiCase(keyword, "DOMAIN_MIN"))
                {
                    if (hasDomainMinimum)
                    {
                        lineError("DOMAIN_MIN is specified more than once.");
                        return false;
                    }
                    domainMinimum = domain;
                    hasDomainMinimum = true;
                }
                else
                {
                    if (hasDomainMaximum)
                    {
                        lineError("DOMAIN_MAX is specified more than once.");
                        return false;
                    }
                    domainMaximum = domain;
                    hasDomainMaximum = true;
                }
                continue;
            }

            if (std::isalpha(static_cast<unsigned char>(keyword.front())))
            {
                lineError("Unsupported .cube directive '" + std::string(keyword) + "'.");
                return false;
            }
            if (!hasSize)
            {
                lineError("Table data appears before LUT_3D_SIZE.");
                return false;
            }
            sawData = true;
            std::array<float, 3> value{};
            if (!ParseRgbTriplet(tokens, value))
            {
                lineError("Each 3D LUT entry must contain three finite numeric values.");
                return false;
            }
            if (values.size() >= expectedValueCount)
            {
                lineError("The LUT contains more entries than LUT_3D_SIZE declares.");
                return false;
            }
            values.push_back(value);
        }

        if (!stream.eof())
        {
            error = "Could not finish reading the LUT file.";
            return false;
        }
        if (!hasSize)
        {
            error = "The LUT file does not declare LUT_3D_SIZE.";
            return false;
        }
        if (values.size() != expectedValueCount)
        {
            error = "LUT_3D_SIZE declares " + std::to_string(expectedValueCount)
                + " entries, but the file contains " + std::to_string(values.size()) + ".";
            return false;
        }
        for (int channel = 0; channel < 3; ++channel)
        {
            if (!(domainMaximum[static_cast<std::size_t>(channel)]
                > domainMinimum[static_cast<std::size_t>(channel)]))
            {
                error = "DOMAIN_MAX must be greater than DOMAIN_MIN for every channel.";
                return false;
            }
        }

        lut.size = size;
        lut.domainMinimum = domainMinimum;
        lut.domainMaximum = domainMaximum;
        lut.values = std::move(values);
        error.clear();
        return true;
    }

    void TrimCubeLutCache(CubeLutCache& cache)
    {
        while (cache.entries.size() > MaximumCachedCubeLuts)
        {
            const auto oldest = std::min_element(cache.entries.begin(), cache.entries.end(),
                [](const auto& left, const auto& right)
                {
                    return left.second.lastUse < right.second.lastUse;
                });
            if (oldest == cache.entries.end())
            {
                return;
            }
            cache.entries.erase(oldest);
        }
    }

    std::size_t CubeLutIndex(const weasel::CubeLut& lut, int red, int green, int blue)
    {
        return (static_cast<std::size_t>(red) * static_cast<std::size_t>(lut.size)
            + static_cast<std::size_t>(green)) * static_cast<std::size_t>(lut.size)
            + static_cast<std::size_t>(blue);
    }

    double PchipEndpointSlope(double firstDelta, double secondDelta)
    {
        double slope = (3.0 * firstDelta - secondDelta) * 0.5;
        if (slope * firstDelta <= 0.0)
        {
            return 0.0;
        }
        if (firstDelta * secondDelta < 0.0 && std::abs(slope) > std::abs(3.0 * firstDelta))
        {
            return 3.0 * firstDelta;
        }
        return slope;
    }

    std::array<std::uint8_t, 256 * 4> ToneCurvePixels(double shadows, double highlights)
    {
        shadows = std::clamp(shadows, -1.0, 1.0);
        highlights = std::clamp(highlights, -1.0, 1.0);
        const std::array<double, 5> values = {
            0.0,
            0.25 + 0.22 * shadows,
            0.50 + 0.08 * shadows + 0.08 * highlights,
            0.75 + 0.22 * highlights,
            1.0
        };
        constexpr double SegmentWidth = 0.25;
        std::array<double, 4> deltas{};
        for (std::size_t index = 0; index < deltas.size(); ++index)
        {
            deltas[index] = (values[index + 1] - values[index]) / SegmentWidth;
        }
        std::array<double, 5> slopes{};
        slopes.front() = PchipEndpointSlope(deltas[0], deltas[1]);
        for (std::size_t index = 1; index + 1 < slopes.size(); ++index)
        {
            const double previous = deltas[index - 1];
            const double next = deltas[index];
            slopes[index] = previous * next <= 0.0 ? 0.0 : 2.0 * previous * next / (previous + next);
        }
        slopes.back() = PchipEndpointSlope(deltas.back(), deltas[deltas.size() - 2]);

        std::array<std::uint8_t, 256 * 4> pixels{};
        for (int index = 0; index < 256; ++index)
        {
            const double input = static_cast<double>(index) / 255.0;
            const std::size_t segment = std::min<std::size_t>(3, static_cast<std::size_t>(input * 4.0));
            const double t = std::clamp((input - static_cast<double>(segment) * SegmentWidth)
                                         / SegmentWidth, 0.0, 1.0);
            const double t2 = t * t;
            const double t3 = t2 * t;
            const double output = (2.0 * t3 - 3.0 * t2 + 1.0) * values[segment]
                + (t3 - 2.0 * t2 + t) * SegmentWidth * slopes[segment]
                + (-2.0 * t3 + 3.0 * t2) * values[segment + 1]
                + (t3 - t2) * SegmentWidth * slopes[segment + 1];
            const std::uint8_t value = static_cast<std::uint8_t>(
                std::lround(std::clamp(output, 0.0, 1.0) * 255.0));
            const std::size_t offset = static_cast<std::size_t>(index) * 4;
            pixels[offset] = value;
            pixels[offset + 1] = value;
            pixels[offset + 2] = value;
            pixels[offset + 3] = 255;
        }
        return pixels;
    }

    std::array<double, 3> TemperatureRgb(double kelvin)
    {
        const double temperature = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
        if (temperature <= 66.0)
        {
            red = 255.0;
            green = 99.4708025861 * std::log(std::max(temperature, 1.0)) - 161.1195681661;
            blue = temperature <= 19.0
                ? 0.0
                : 138.5177312231 * std::log(temperature - 10.0) - 305.0447927307;
        }
        else
        {
            red = 329.698727446 * std::pow(temperature - 60.0, -0.1332047592);
            green = 288.1221695283 * std::pow(temperature - 60.0, -0.0755148492);
            blue = 255.0;
        }
        return {
            std::clamp(red, 0.0, 255.0) / 255.0,
            std::clamp(green, 0.0, 255.0) / 255.0,
            std::clamp(blue, 0.0, 255.0) / 255.0
        };
    }
}

weasel::CubeLutLoad weasel::FindCubeLut(const std::filesystem::path& requestedPath)
{
    const std::filesystem::path normalizedPath = NormalizeLutPath(requestedPath);
    const std::string cacheKey = normalizedPath.generic_string();
    CubeLutCache& cache = CachedCubeLuts();
    std::scoped_lock lock(cache.mutex);
    CachedCubeLut& entry = cache.entries[cacheKey];
    const CubeLutFileStatus status = GetCubeLutFileStatus(normalizedPath);
    if (!entry.hasStamp || entry.stamp != status.stamp)
    {
        entry.hasStamp = true;
        entry.stamp = status.stamp;
        entry.revision = ++cache.revisionCounter;
        entry.lut.reset();
        entry.error.clear();
        if (!status.error.empty())
        {
            entry.error = status.error;
        }
        else
        {
            auto loaded = std::make_shared<CubeLut>();
            if (LoadCubeLut(normalizedPath, *loaded, entry.error))
            {
                entry.lut = std::move(loaded);
            }
        }
    }
    entry.lastUse = ++cache.useCounter;

    CubeLutLoad result;
    result.lut = entry.lut;
    result.error = entry.error;
    result.cacheKey = cacheKey;
    result.revision = entry.revision;
    TrimCubeLutCache(cache);
    return result;
}

namespace
{
    constexpr std::string_view VideoFragmentShader = R"glsl(
#version 120

uniform sampler2D sourceTexture;
uniform sampler2D toneTexture;
uniform sampler2D lutTexture;
uniform int effectMode;
uniform vec2 textureSize;
uniform float amount;
uniform float secondary;
uniform float angleRadians;
uniform float timeSeed;
uniform float brightness;
uniform float contrast;
uniform float hueRadians;
uniform float saturation;
uniform vec3 temperatureScale;
uniform bool blackAndWhite;
uniform bool invertColor;
uniform bool useToneCurve;
uniform bool useLut;
uniform float lutSize;
uniform vec3 lutDomainMinimum;
uniform vec3 lutDomainMaximum;

vec4 ReadSource(vec2 uv)
{
    return texture2D(sourceTexture, clamp(uv, vec2(0.0), vec2(1.0)));
}

vec4 ReadTransparentSource(vec2 uv)
{
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
    {
        return vec4(0.0);
    }
    return texture2D(sourceTexture, uv);
}

float Luma(vec3 colour)
{
    return dot(colour, vec3(0.299, 0.587, 0.114));
}

vec3 RgbToHsv(vec3 colour)
{
    vec4 k = vec4(0.0, -0.3333333333, 0.6666666667, -1.0);
    vec4 p = mix(vec4(colour.bg, k.wz), vec4(colour.gb, k.xy), step(colour.b, colour.g));
    vec4 q = mix(vec4(p.xyw, colour.r), vec4(colour.r, p.yzx), step(p.x, colour.r));
    float delta = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * delta + 0.0000001)),
                delta / (q.x + 0.0000001), q.x);
}

vec3 HsvToRgb(vec3 colour)
{
    vec3 p = abs(fract(colour.xxx + vec3(0.0, 0.6666666667, 0.3333333333)) * 6.0 - 3.0);
    return colour.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), colour.y);
}

vec3 FetchLut(vec3 coordinate)
{
    float x = (coordinate.g * lutSize + coordinate.b + 0.5) / (lutSize * lutSize);
    float y = (coordinate.r + 0.5) / lutSize;
    return texture2D(lutTexture, vec2(x, y)).rgb;
}

vec3 ApplyLut(vec3 colour)
{
    vec3 normalized = clamp((colour - lutDomainMinimum)
        / max(lutDomainMaximum - lutDomainMinimum, vec3(0.000001)), 0.0, 1.0);
    vec3 coordinate = normalized * (lutSize - 1.0);
    vec3 lower = min(floor(coordinate), vec3(lutSize - 2.0));
    vec3 fraction = coordinate - lower;
    vec3 c000 = FetchLut(lower);
    vec3 c001 = FetchLut(lower + vec3(0.0, 0.0, 1.0));
    vec3 c010 = FetchLut(lower + vec3(0.0, 1.0, 0.0));
    vec3 c011 = FetchLut(lower + vec3(0.0, 1.0, 1.0));
    vec3 c100 = FetchLut(lower + vec3(1.0, 0.0, 0.0));
    vec3 c101 = FetchLut(lower + vec3(1.0, 0.0, 1.0));
    vec3 c110 = FetchLut(lower + vec3(1.0, 1.0, 0.0));
    vec3 c111 = FetchLut(lower + vec3(1.0, 1.0, 1.0));
    vec3 result = c000;
    if (fraction.r >= fraction.g)
    {
        if (fraction.g >= fraction.b)
        {
            result += fraction.r * (c100 - c000) + fraction.g * (c110 - c100)
                + fraction.b * (c111 - c110);
        }
        else if (fraction.r >= fraction.b)
        {
            result += fraction.r * (c100 - c000) + fraction.b * (c101 - c100)
                + fraction.g * (c111 - c101);
        }
        else
        {
            result += fraction.b * (c001 - c000) + fraction.r * (c101 - c001)
                + fraction.g * (c111 - c101);
        }
    }
    else if (fraction.b >= fraction.g)
    {
        result += fraction.b * (c001 - c000) + fraction.g * (c011 - c001)
            + fraction.r * (c111 - c011);
    }
    else if (fraction.b >= fraction.r)
    {
        result += fraction.g * (c010 - c000) + fraction.b * (c011 - c010)
            + fraction.r * (c111 - c011);
    }
    else
    {
        result += fraction.g * (c010 - c000) + fraction.r * (c110 - c010)
            + fraction.b * (c111 - c110);
    }
    return clamp(result, 0.0, 1.0);
}

vec4 ApplyBaseGrade(vec2 uv)
{
    vec4 source = ReadSource(uv);
    float oldLuma = Luma(source.rgb);
    float adjustedLuma = clamp(oldLuma * contrast + brightness + 0.5 * (1.0 - contrast), 0.0, 1.0);
    vec3 colour = clamp(source.rgb + vec3(adjustedLuma - oldLuma), 0.0, 1.0);
    if (blackAndWhite)
    {
        colour = vec3(adjustedLuma);
    }
    else
    {
        vec3 hsv = RgbToHsv(colour);
        hsv.x = fract(hsv.x + hueRadians / 6.28318530718);
        hsv.y = clamp(hsv.y * saturation, 0.0, 1.0);
        colour = HsvToRgb(hsv) * temperatureScale;
    }
    colour = clamp(colour, 0.0, 1.0);
    if (useToneCurve)
    {
        colour = vec3(texture2D(toneTexture, vec2(colour.r, 0.5)).r,
                      texture2D(toneTexture, vec2(colour.g, 0.5)).r,
                      texture2D(toneTexture, vec2(colour.b, 0.5)).r);
    }
    if (useLut)
    {
        colour = ApplyLut(colour);
    }
    if (invertColor)
    {
        colour = clamp(1.0 - colour, 0.0, 1.0);
    }
    return vec4(colour, source.a);
}

float Hash(vec2 value)
{
    return fract(sin(dot(value, vec2(12.9898, 78.233)) + timeSeed) * 43758.5453);
}

vec4 ApplyEdgeDetection(vec2 uv)
{
    vec2 pixel = 1.0 / textureSize;
    float tl = Luma(ReadSource(uv + pixel * vec2(-1.0, -1.0)).rgb);
    float tc = Luma(ReadSource(uv + pixel * vec2( 0.0, -1.0)).rgb);
    float tr = Luma(ReadSource(uv + pixel * vec2( 1.0, -1.0)).rgb);
    float ml = Luma(ReadSource(uv + pixel * vec2(-1.0,  0.0)).rgb);
    float mr = Luma(ReadSource(uv + pixel * vec2( 1.0,  0.0)).rgb);
    float bl = Luma(ReadSource(uv + pixel * vec2(-1.0,  1.0)).rgb);
    float bc = Luma(ReadSource(uv + pixel * vec2( 0.0,  1.0)).rgb);
    float br = Luma(ReadSource(uv + pixel * vec2( 1.0,  1.0)).rgb);
    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
    float edge = smoothstep(0.21, 0.61, length(vec2(gx, gy)));
    vec4 source = ReadSource(uv);
    return vec4(source.rgb * (1.0 - 0.35 * amount) + vec3(edge * amount), source.a);
}

vec4 NeighbourBlur(vec2 uv, float radius)
{
    vec2 stepSize = (1.0 / textureSize) * max(1.0, radius * 0.5);
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float sampleWeight = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));
            sum += ReadSource(uv + vec2(float(x), float(y)) * stepSize) * sampleWeight;
            weight += sampleWeight;
        }
    }
    return sum / weight;
}

vec4 ApplyEffect(vec2 uv)
{
    vec2 pixel = 1.0 / textureSize;
    if (effectMode == 1)
    {
        vec2 centre = (uv - 0.5) * textureSize / max(textureSize.x, textureSize.y);
        vec2 distorted = 0.5 + centre * (1.0 + amount * dot(centre, centre))
            * max(textureSize.x, textureSize.y) / textureSize;
        return ReadTransparentSource(distorted);
    }
    if (effectMode == 2)
    {
        vec2 block = vec2(max(2.0, amount));
        vec2 sampleUv = (floor(uv * textureSize / block) * block + block * 0.5) / textureSize;
        return ReadSource(sampleUv);
    }
    if (effectMode == 3)
    {
        vec4 source = ReadSource(uv);
        float levels = max(2.0, floor(amount + 0.5));
        source.rgb = floor(source.rgb * (levels - 1.0) + 0.5) / (levels - 1.0);
        return source;
    }
    if (effectMode == 4)
    {
        return ApplyEdgeDetection(uv);
    }
    if (effectMode == 5)
    {
        vec4 source = ReadSource(uv);
        vec3 blurred = NeighbourBlur(uv, 1.1).rgb;
        return vec4(clamp(source.rgb * (1.0 + amount) - blurred * amount, 0.0, 1.0), source.a);
    }
    if (effectMode == 6)
    {
        vec4 source = ReadSource(uv);
        vec2 stepSize = pixel * (1.5 + 7.0 * amount);
        vec3 glow = vec3(0.0);
        float weight = 0.0;
        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                vec3 sampleColour = ReadSource(uv + vec2(float(x), float(y)) * stepSize).rgb;
                float sampleWeight = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));
                glow += sampleColour * step(0.647, Luma(sampleColour)) * sampleWeight;
                weight += sampleWeight;
            }
        }
        return vec4(clamp(source.rgb + glow * amount / weight, 0.0, 1.0), source.a);
    }
    if (effectMode == 7)
    {
        vec2 shift = vec2(cos(angleRadians), sin(angleRadians)) * amount * pixel;
        vec4 source = ReadSource(uv);
        source.r = ReadSource(uv - shift).r;
        source.b = ReadSource(uv + shift).b;
        return source;
    }
    if (effectMode == 8)
    {
        vec4 source = ReadSource(uv);
        vec2 centre = uv - 0.5;
        float distanceFromCentre = length(centre) / 0.70710678118;
        float edge = smoothstep(secondary, 1.0, distanceFromCentre);
        source.rgb *= 1.0 - amount * edge;
        return source;
    }
    if (effectMode == 9)
    {
        vec4 source = ReadSource(uv);
        vec2 block = floor(uv * textureSize / max(1.0, secondary));
        float noise = (Hash(block) - 0.5) * 0.6 * amount;
        source.rgb = clamp(source.rgb + vec3(noise), 0.0, 1.0);
        return source;
    }
    if (effectMode == 10)
    {
        float row = uv.y * textureSize.y;
        float shift = sin(row * 0.075 + secondary * 19.0) * 6.0 * amount;
        vec4 source = ReadSource(uv - vec2(shift / textureSize.x, 0.0));
        float scanline = mod(floor(row), 4.0) < 2.0 ? 1.0 - 0.22 * amount : 1.0;
        float noise = (Hash(floor(uv * textureSize)) - 0.5) * 0.24 * amount;
        source.rgb = clamp(source.rgb * scanline + vec3(noise), 0.0, 1.0);
        source.b *= 1.0 - 0.08 * amount;
        return source;
    }
    if (effectMode == 11)
    {
        return NeighbourBlur(uv, amount);
    }
    return ReadSource(uv);
}

void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec4 colour = effectMode == 0 ? ApplyBaseGrade(uv) : ApplyEffect(uv);
    gl_FragColor = colour * gl_Color;
}
)glsl";
}

namespace weasel
{
    class VideoCompositor::Impl
    {
    private:
        struct LayerResources
        {
            sf::Texture                              sourceTexture;
            std::array<sf::RenderTexture, 2>         effectTargets;
            sf::Texture                              lutTexture;
            std::weak_ptr<const void>                uploadedOwner;
            std::weak_ptr<const void>                processedOwner;
            ClipVideoSettings                        processedVideo;
            ClipEffectsSettings                      processedEffects;
            sf::Vector2u                             size{};
            std::string                              lutCacheKey;
            std::string                              processedLutCacheKey;
            std::uint64_t                            uploadedRevision = 0;
            std::uint64_t                            processedRevision = 0;
            std::uint64_t                            lutRevision = 0;
            std::uint64_t                            processedLutRevision = 0;
            double                                   processedEffectTime = 0.0;
            int                                      processedNativeWidth = 0;
            int                                      processedNativeHeight = 0;
            int                                      processedTarget = -1;
            bool                                     uploadedUsesOwner = false;
            bool                                     processedUsesOwner = false;
            bool                                     hasUploadedFrame = false;
            bool                                     hasProcessedFrame = false;
        };

        sf::RenderTexture                                       m_composite;
        sf::Texture                                             m_outputTexture;
        sf::Shader                                              m_shader;
        sf::Texture                                             m_toneCurveTexture;
        std::unordered_map<int, std::unique_ptr<LayerResources>> m_layers;
        double                                                  m_toneCurveShadows = 0.0;
        double                                                  m_toneCurveHighlights = 0.0;
        bool                                                    m_hasToneCurve = false;
        bool                                                    m_shaderReady = false;
        bool                                                    m_hasTexture = false;

        static bool resizeRenderTexture(sf::RenderTexture& texture, sf::Vector2u size, std::string& error)
        {
            if (texture.getSize() == size)
            {
                return true;
            }
            if (!texture.resize(size))
            {
                error = "Could not allocate a GPU compositor render target.";
                return false;
            }
            texture.setSmooth(true);
            return true;
        }

        static bool sameFrame(const std::weak_ptr<const void>& cachedOwner,
                              std::uint64_t cachedRevision,
                              bool cachedUsesOwner,
                              bool hasCachedFrame,
                              const VideoCompositorFrame& frame)
        {
            const bool usesOwner = static_cast<bool>(frame.owner);
            if (!hasCachedFrame || cachedRevision != frame.revision || cachedUsesOwner != usesOwner)
            {
                return false;
            }
            return !usesOwner || cachedOwner.lock().get() == frame.owner.get();
        }

        static float effectSeed(int clipId, double effectTime)
        {
            const std::uint64_t microseconds = static_cast<std::uint64_t>(
                std::llround(std::max(0.0, effectTime) * 1000000.0));
            const std::uint64_t seed = static_cast<std::uint64_t>(std::max(clipId, 0)) * 2654435761ULL
                ^ microseconds * 2246822519ULL;
            return static_cast<float>(seed % 1000003ULL);
        }

        bool uploadFrame(LayerResources& resources,
                         const VideoCompositorFrame& frame,
                         std::string& error)
        {
            const sf::Vector2u size(static_cast<unsigned int>(frame.width),
                                    static_cast<unsigned int>(frame.height));
            if (resources.size == size
                && sameFrame(resources.uploadedOwner, resources.uploadedRevision,
                             resources.uploadedUsesOwner, resources.hasUploadedFrame, frame))
            {
                return true;
            }
            if (resources.sourceTexture.getSize() != size && !resources.sourceTexture.resize(size))
            {
                error = "Could not allocate a GPU compositor source texture.";
                return false;
            }
            resources.sourceTexture.setSmooth(true);
            resources.sourceTexture.update(frame.pixels);
            resources.uploadedOwner = frame.owner;
            resources.uploadedRevision = frame.revision;
            resources.uploadedUsesOwner = static_cast<bool>(frame.owner);
            resources.hasUploadedFrame = true;
            resources.hasProcessedFrame = false;
            resources.size = size;
            return true;
        }

        bool uploadToneCurve(double shadows, double highlights, std::string& error)
        {
            if (m_hasToneCurve && shadows == m_toneCurveShadows && highlights == m_toneCurveHighlights)
            {
                return true;
            }

            constexpr sf::Vector2u Size(256, 1);
            if (m_toneCurveTexture.getSize() != Size && !m_toneCurveTexture.resize(Size))
            {
                error = "Could not allocate the GPU compositor tone-curve texture.";
                return false;
            }
            const auto pixels = ToneCurvePixels(shadows, highlights);
            m_toneCurveTexture.setSmooth(false);
            m_toneCurveTexture.update(pixels.data());
            m_toneCurveShadows = shadows;
            m_toneCurveHighlights = highlights;
            m_hasToneCurve = true;
            return true;
        }

        bool uploadLut(LayerResources& resources,
                       const VideoCompositorLayer& layer,
                       std::string& error)
        {
            if (!layer.lut)
            {
                return true;
            }
            if (resources.lutCacheKey == layer.lutCacheKey
                && resources.lutRevision == layer.lutRevision
                && resources.lutTexture.getSize().x != 0)
            {
                return true;
            }
            if (layer.lut->size < 2 || layer.lut->size > MaximumCubeLutSize)
            {
                error = "The selected LUT has an invalid 3D table size.";
                return false;
            }
            const std::size_t lutSize = static_cast<std::size_t>(layer.lut->size);
            if (layer.lut->values.size() != lutSize * lutSize * lutSize)
            {
                error = "The selected LUT has an incomplete 3D table.";
                return false;
            }

            const unsigned int size = static_cast<unsigned int>(layer.lut->size);
            const sf::Vector2u textureSize(size * size, size);
            if (textureSize.x > sf::Texture::getMaximumSize()
                || textureSize.y > sf::Texture::getMaximumSize())
            {
                error = "The selected LUT is larger than the GPU supports.";
                return false;
            }

            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(textureSize.x)
                * static_cast<std::size_t>(textureSize.y) * 4);
            for (int red = 0; red < layer.lut->size; ++red)
            {
                for (int green = 0; green < layer.lut->size; ++green)
                {
                    for (int blue = 0; blue < layer.lut->size; ++blue)
                    {
                        const std::array<float, 3>& value = layer.lut->values[
                            CubeLutIndex(*layer.lut, red, green, blue)];
                        const std::size_t pixelIndex = (static_cast<std::size_t>(red) * textureSize.x
                            + static_cast<std::size_t>(green * layer.lut->size + blue)) * 4;
                        for (int channel = 0; channel < 3; ++channel)
                        {
                            pixels[pixelIndex + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
                                std::lround(std::clamp(value[static_cast<std::size_t>(channel)], 0.0f, 1.0f)
                                    * 255.0f));
                        }
                        pixels[pixelIndex + 3] = 255;
                    }
                }
            }

            if (resources.lutTexture.getSize() != textureSize && !resources.lutTexture.resize(textureSize))
            {
                error = "Could not allocate the GPU compositor LUT texture.";
                return false;
            }
            resources.lutTexture.setSmooth(false);
            resources.lutTexture.update(pixels.data());
            resources.lutCacheKey = layer.lutCacheKey;
            resources.lutRevision = layer.lutRevision;
            return true;
        }

        const sf::Texture* runPass(LayerResources& resources,
                                   const sf::Texture& source,
                                   int effectMode,
                                   float amount,
                                   float secondary,
                                   float angleRadians,
                                   float timeSeed,
                                   int& targetIndex,
                                   std::string& error)
        {
            sf::RenderTexture& target = resources.effectTargets[static_cast<std::size_t>(targetIndex)];
            if (!resizeRenderTexture(target, resources.size, error))
            {
                return nullptr;
            }

            m_shader.setUniform("sourceTexture", sf::Shader::CurrentTexture);
            m_shader.setUniform("effectMode", effectMode);
            m_shader.setUniform("textureSize", sf::Glsl::Vec2(resources.size));
            m_shader.setUniform("amount", amount);
            m_shader.setUniform("secondary", secondary);
            m_shader.setUniform("angleRadians", angleRadians);
            m_shader.setUniform("timeSeed", timeSeed);

            target.clear(sf::Color::Transparent);
            const sf::Sprite sprite(source);
            target.draw(sprite, sf::RenderStates(&m_shader));
            target.display();
            targetIndex = 1 - targetIndex;
            return &target.getTexture();
        }

        const sf::Texture* processLayer(LayerResources& resources,
                                        const VideoCompositorLayer& layer,
                                        std::string& error)
        {
            const double cacheEffectTime = layer.effects.filmGrainEnabled || layer.effects.vhsEnabled
                ? layer.effectTime
                : 0.0;
            if (resources.hasProcessedFrame
                && sameFrame(resources.processedOwner, resources.processedRevision,
                             resources.processedUsesOwner, true, layer.frame)
                && resources.processedVideo == layer.video
                && resources.processedEffects == layer.effects
                && resources.processedEffectTime == cacheEffectTime
                && resources.processedNativeWidth == layer.nativeWidth
                && resources.processedNativeHeight == layer.nativeHeight
                && resources.processedLutCacheKey == layer.lutCacheKey
                && resources.processedLutRevision == layer.lutRevision)
            {
                return resources.processedTarget < 0
                    ? &resources.sourceTexture
                    : &resources.effectTargets[static_cast<std::size_t>(resources.processedTarget)].getTexture();
            }

            const bool hasToneCurve = layer.video.shadows != 0.0 || layer.video.highlights != 0.0;
            const bool hasBaseGrade = layer.video.brightness != 0.0 || layer.video.contrast != 1.0
                || hasToneCurve || layer.video.blackAndWhite || layer.video.invertColor
                || layer.video.hue != 0.0 || layer.video.saturation != 1.0
                || layer.video.temperature != 6500.0 || static_cast<bool>(layer.lut);
            if (hasToneCurve && !uploadToneCurve(layer.video.shadows, layer.video.highlights, error))
            {
                return nullptr;
            }
            if (!uploadLut(resources, layer, error))
            {
                return nullptr;
            }

            const sf::Texture* current = &resources.sourceTexture;
            int targetIndex = 0;
            if (hasBaseGrade)
            {
                const std::array<double, 3> reference = TemperatureRgb(6500.0);
                const std::array<double, 3> temperature = TemperatureRgb(layer.video.temperature);
                m_shader.setUniform("brightness", static_cast<float>(layer.video.brightness));
                m_shader.setUniform("contrast", static_cast<float>(layer.video.contrast));
                m_shader.setUniform("hueRadians", static_cast<float>(
                    layer.video.hue * 3.14159265358979323846 / 180.0));
                m_shader.setUniform("saturation", static_cast<float>(layer.video.saturation));
                m_shader.setUniform("temperatureScale", sf::Glsl::Vec3(
                    static_cast<float>(temperature[0] / reference[0]),
                    static_cast<float>(temperature[1] / reference[1]),
                    static_cast<float>(temperature[2] / reference[2])));
                m_shader.setUniform("blackAndWhite", layer.video.blackAndWhite);
                m_shader.setUniform("invertColor", layer.video.invertColor);
                m_shader.setUniform("useToneCurve", hasToneCurve);
                m_shader.setUniform("toneTexture", hasToneCurve ? m_toneCurveTexture : resources.sourceTexture);
                m_shader.setUniform("useLut", static_cast<bool>(layer.lut));
                m_shader.setUniform("lutTexture", layer.lut ? resources.lutTexture : resources.sourceTexture);
                m_shader.setUniform("lutSize", layer.lut ? static_cast<float>(layer.lut->size) : 2.0f);
                m_shader.setUniform("lutDomainMinimum", layer.lut
                    ? sf::Glsl::Vec3(layer.lut->domainMinimum[0], layer.lut->domainMinimum[1],
                                     layer.lut->domainMinimum[2])
                    : sf::Glsl::Vec3(0.0f, 0.0f, 0.0f));
                m_shader.setUniform("lutDomainMaximum", layer.lut
                    ? sf::Glsl::Vec3(layer.lut->domainMaximum[0], layer.lut->domainMaximum[1],
                                     layer.lut->domainMaximum[2])
                    : sf::Glsl::Vec3(1.0f, 1.0f, 1.0f));
                current = runPass(resources, *current, 0, 0.0f, 0.0f, 0.0f, 0.0f,
                                  targetIndex, error);
                if (!current)
                {
                    return nullptr;
                }
            }

            constexpr float Pi = 3.14159265358979323846f;
            const float seed = effectSeed(layer.clipId, layer.effectTime);
            const auto apply = [&](bool enabled, int mode, double first,
                                   double second = 0.0, double angle = 0.0)
            {
                if (enabled && current)
                {
                    current = runPass(resources, *current, mode,
                                      static_cast<float>(first), static_cast<float>(second),
                                      static_cast<float>(angle) * Pi / 180.0f, seed,
                                      targetIndex, error);
                }
            };
            apply(layer.effects.lensDistortionEnabled, 1, layer.effects.lensDistortionStrength);
            apply(layer.effects.pixelateEnabled, 2, layer.effects.pixelateBlockSize);
            apply(layer.effects.posterizeEnabled, 3, layer.effects.posterizeLevels);
            apply(layer.effects.edgeDetectionEnabled, 4, layer.effects.edgeDetectionAmount);
            apply(layer.effects.sharpenEnabled, 5, layer.effects.sharpenAmount);
            apply(layer.effects.glowEnabled, 6, layer.effects.glowIntensity);
            apply(layer.effects.chromaticAberrationEnabled, 7,
                  layer.effects.chromaticAberrationAmount, 0.0, layer.effects.chromaticAberrationAngle);
            apply(layer.effects.vignetteEnabled, 8,
                  layer.effects.vignetteStrength, layer.effects.vignetteRadius);
            apply(layer.effects.filmGrainEnabled, 9,
                  layer.effects.filmGrainIntensity, layer.effects.filmGrainSize);
            apply(layer.effects.vhsEnabled, 10, layer.effects.vhsIntensity, layer.effectTime);

            if (layer.video.blur > 0.001 && current)
            {
                const int nativeWidth = layer.nativeWidth > 0 ? layer.nativeWidth : layer.frame.width;
                const int nativeHeight = layer.nativeHeight > 0 ? layer.nativeHeight : layer.frame.height;
                const double decodedScale = std::min(
                    static_cast<double>(layer.frame.width) / std::max(1, nativeWidth),
                    static_cast<double>(layer.frame.height) / std::max(1, nativeHeight));
                apply(true, 11, std::max(0.01, layer.video.blur * decodedScale));
            }
            if (current)
            {
                resources.processedOwner = layer.frame.owner;
                resources.processedRevision = layer.frame.revision;
                resources.processedUsesOwner = static_cast<bool>(layer.frame.owner);
                resources.processedVideo = layer.video;
                resources.processedEffects = layer.effects;
                resources.processedEffectTime = cacheEffectTime;
                resources.processedNativeWidth = layer.nativeWidth;
                resources.processedNativeHeight = layer.nativeHeight;
                resources.processedLutCacheKey = layer.lutCacheKey;
                resources.processedLutRevision = layer.lutRevision;
                resources.processedTarget = current == &resources.sourceTexture ? -1 : 1 - targetIndex;
                resources.hasProcessedFrame = true;
            }
            return current;
        }

        void drawLayer(const VideoCompositorLayer& layer,
                       const sf::Texture& texture,
                       int sequenceWidth,
                       int sequenceHeight,
                       double outputScale)
        {
            const int sourceWidth = layer.frame.width;
            const int sourceHeight = layer.frame.height;
            const int nativeWidth = layer.nativeWidth > 0 ? layer.nativeWidth : sourceWidth;
            const int nativeHeight = layer.nativeHeight > 0 ? layer.nativeHeight : sourceHeight;
            const int transformedWidth = std::max(1, static_cast<int>(nativeWidth * layer.video.scale));
            const int transformedHeight = std::max(1, static_cast<int>(nativeHeight * layer.video.scale));

            const double cropLeft = std::clamp(layer.video.cropLeft, 0.0, 0.999);
            const double cropTop = std::clamp(layer.video.cropTop, 0.0, 0.999);
            const double cropRight = std::clamp(layer.video.cropRight, 0.0, 0.999);
            const double cropBottom = std::clamp(layer.video.cropBottom, 0.0, 0.999);
            int cropX = 0;
            int cropY = 0;
            int cropWidth = transformedWidth;
            int cropHeight = transformedHeight;
            if (cropLeft != 0.0 || cropTop != 0.0 || cropRight != 0.0 || cropBottom != 0.0)
            {
                const int minimumWidth = transformedWidth >= 2 ? 2 : 1;
                const int minimumHeight = transformedHeight >= 2 ? 2 : 1;
                cropWidth = std::clamp(static_cast<int>(std::floor(
                    transformedWidth * std::max(0.001, 1.0 - cropLeft - cropRight) / 2.0)) * 2,
                    minimumWidth, transformedWidth);
                cropHeight = std::clamp(static_cast<int>(std::floor(
                    transformedHeight * std::max(0.001, 1.0 - cropTop - cropBottom) / 2.0)) * 2,
                    minimumHeight, transformedHeight);
                cropX = std::clamp(static_cast<int>(std::floor(transformedWidth * cropLeft / 2.0)) * 2,
                                   0, transformedWidth - cropWidth);
                cropY = std::clamp(static_cast<int>(std::floor(transformedHeight * cropTop / 2.0)) * 2,
                                   0, transformedHeight - cropHeight);
            }

            const int sourceLeft = std::clamp(static_cast<int>(std::floor(
                static_cast<double>(cropX) * sourceWidth / transformedWidth)), 0, sourceWidth - 1);
            const int sourceTop = std::clamp(static_cast<int>(std::floor(
                static_cast<double>(cropY) * sourceHeight / transformedHeight)), 0, sourceHeight - 1);
            const int sourceRight = std::clamp(static_cast<int>(std::ceil(
                static_cast<double>(cropX + cropWidth) * sourceWidth / transformedWidth)),
                sourceLeft + 1, sourceWidth);
            const int sourceBottom = std::clamp(static_cast<int>(std::ceil(
                static_cast<double>(cropY + cropHeight) * sourceHeight / transformedHeight)),
                sourceTop + 1, sourceHeight);
            const int targetWidth = std::max(1, static_cast<int>(std::lround(transformedWidth * outputScale)));
            const int targetHeight = std::max(1, static_cast<int>(std::lround(transformedHeight * outputScale)));
            const int left = static_cast<int>(std::lround(
                ((sequenceWidth - transformedWidth) * 0.5 + layer.video.positionX) * outputScale));
            const int top = static_cast<int>(std::lround(
                ((sequenceHeight - transformedHeight) * 0.5 + layer.video.positionY) * outputScale));

            sf::Sprite sprite(texture, sf::IntRect(
                { sourceLeft, sourceTop }, { sourceRight - sourceLeft, sourceBottom - sourceTop }));
            sprite.setOrigin({ sourceWidth * 0.5f - sourceLeft, sourceHeight * 0.5f - sourceTop });
            sprite.setPosition({ left + targetWidth * 0.5f, top + targetHeight * 0.5f });
            sprite.setScale({ static_cast<float>(targetWidth) / sourceWidth,
                              static_cast<float>(targetHeight) / sourceHeight });
            sprite.setRotation(sf::degrees(static_cast<float>(layer.video.rotation)));
            sprite.setColor(sf::Color(255, 255, 255, static_cast<std::uint8_t>(std::lround(
                std::clamp(layer.video.opacity, 0.0, 1.0) * 255.0))));
            m_composite.draw(sprite);
        }

    public:
        Impl()
        {
            m_shaderReady = sf::Shader::isAvailable()
                && m_shader.loadFromMemory(VideoFragmentShader, sf::Shader::Type::Fragment);
        }

        bool render(const std::vector<VideoCompositorLayer>& layers,
                    int sequenceWidth,
                    int sequenceHeight,
                    double outputScale,
                    int canvasWidth,
                    int canvasHeight,
                    std::string& error)
        {
            if (!m_shaderReady)
            {
                error = "GPU compositor shaders are not available.";
                return false;
            }
            if (sequenceWidth <= 0 || sequenceHeight <= 0 || outputScale <= 0.0
                || canvasWidth <= 0 || canvasHeight <= 0)
            {
                error = "The GPU compositor received invalid output dimensions.";
                return false;
            }

            const sf::Vector2u canvasSize(static_cast<unsigned int>(canvasWidth),
                                          static_cast<unsigned int>(canvasHeight));
            if (!resizeRenderTexture(m_composite, canvasSize, error))
            {
                return false;
            }
            m_composite.clear(sf::Color::Black);

            std::unordered_set<int> activeClipIds;
            for (const VideoCompositorLayer& layer : layers)
            {
                if (!layer.frame.pixels || layer.frame.width <= 0 || layer.frame.height <= 0)
                {
                    continue;
                }

                activeClipIds.insert(layer.clipId);
                std::unique_ptr<LayerResources>& resources = m_layers[layer.clipId];
                if (!resources)
                {
                    resources = std::make_unique<LayerResources>();
                }
                if (!uploadFrame(*resources, layer.frame, error))
                {
                    return false;
                }
                const sf::Texture* processed = processLayer(*resources, layer, error);
                if (!processed)
                {
                    return false;
                }
                drawLayer(layer, *processed, sequenceWidth, sequenceHeight, outputScale);
            }
            std::erase_if(m_layers, [&activeClipIds](const auto& item)
            {
                return !activeClipIds.contains(item.first);
            });

            m_composite.display();
            if (m_outputTexture.getSize() != canvasSize && !m_outputTexture.resize(canvasSize))
            {
                error = "Could not allocate the GPU compositor output texture.";
                return false;
            }
            m_outputTexture.setSmooth(true);
            m_outputTexture.update(m_composite.getTexture());
            m_hasTexture = true;
            error.clear();
            return true;
        }

        bool copyToImage(sf::Image& output, std::string& error) const
        {
            if (!m_hasTexture)
            {
                error = "The GPU compositor has no rendered frame to read back.";
                return false;
            }

            output = m_composite.getTexture().copyToImage();
            if (output.getSize() != m_composite.getSize() || !output.getPixelsPtr())
            {
                error = "Could not read the rendered frame back from the GPU.";
                return false;
            }
            error.clear();
            return true;
        }

        const sf::Texture* texture() const
        {
            return m_hasTexture ? &m_outputTexture : nullptr;
        }

        bool hasTexture() const
        {
            return m_hasTexture;
        }

        void reset()
        {
            m_layers.clear();
            m_hasTexture = false;
        }
    };

    VideoCompositor::VideoCompositor()
        : m_impl(std::make_unique<Impl>())
    {
    }

    VideoCompositor::~VideoCompositor() = default;

    bool VideoCompositor::render(const std::vector<VideoCompositorLayer>& layers,
                                 int sequenceWidth,
                                 int sequenceHeight,
                                 double outputScale,
                                 int canvasWidth,
                                 int canvasHeight,
                                 std::string& error)
    {
        return m_impl->render(layers, sequenceWidth, sequenceHeight, outputScale,
                              canvasWidth, canvasHeight, error);
    }

    bool VideoCompositor::copyToImage(sf::Image& output, std::string& error) const
    {
        return m_impl->copyToImage(output, error);
    }

    const sf::Texture* VideoCompositor::texture() const
    {
        return m_impl->texture();
    }

    bool VideoCompositor::hasTexture() const
    {
        return m_impl->hasTexture();
    }

    void VideoCompositor::reset()
    {
        m_impl->reset();
    }
}
