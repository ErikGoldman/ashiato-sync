#include "game/constants.hpp"

namespace fps {

const std::array<CoverBox, 7> g_cover_boxes{{
    {Vector3{-3.8f, 0.55f, -2.8f}, Vector3{1.4f, 1.1f, 0.8f}},
    {Vector3{-1.4f, 0.45f, 2.6f}, Vector3{2.0f, 0.9f, 0.7f}},
    {Vector3{2.8f, 0.75f, -3.0f}, Vector3{0.8f, 1.5f, 1.2f}},
    {Vector3{4.5f, 0.5f, 1.8f}, Vector3{1.6f, 1.0f, 0.7f}},
    {Vector3{0.2f, 0.65f, -0.4f}, Vector3{0.9f, 1.3f, 0.9f}},
    {Vector3{-5.4f, 0.45f, 4.4f}, Vector3{1.3f, 0.9f, 1.3f}},
    {Vector3{5.5f, 0.45f, -5.0f}, Vector3{1.2f, 0.9f, 1.6f}},
}};

}  // namespace fps
