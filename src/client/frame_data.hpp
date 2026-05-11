#pragma once

#include "detail/frame_data.hpp"

namespace ashiato::sync::client_detail {

using detail::apply_archetype_tags;
using detail::frame_component_data;
using detail::frame_has_component;
using detail::has_tag_slot;
using detail::init_frame_data;
using detail::mutable_frame_component_data;
using detail::remove_archetype_tags;
using detail::sync_slot_bit;
using detail::sync_slot_count;
using detail::tag_bit_set;
using detail::unchecked_frame_component_data;
using detail::unchecked_mutable_frame_component_data;

}  // namespace ashiato::sync::client_detail
