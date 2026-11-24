#pragma once

namespace SceneRotation {

constexpr bool isValid(int degrees)
{
    return degrees >= 0 && degrees < 360 && degrees % 90 == 0;
}

constexpr bool isLandscape(int degrees)
{
    return degrees % 180 == 90;
}

constexpr int toggle(int degrees)
{
    return isLandscape(degrees) ? degrees - 90 : degrees + 90;
}

} // namespace SceneRotation
