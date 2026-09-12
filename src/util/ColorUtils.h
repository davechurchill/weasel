#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace weasel
{
    inline std::array<double, 3> TemperatureRgb(double kelvin)
    {
        const double temperature = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
        if (temperature <= 66.0)
        {
            red = 255.0;
            green = 99.4708025861 * std::log(std::max(temperature, 1.0)) - 161.1195681661;
            blue = temperature <= 19.0 ? 0.0
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
