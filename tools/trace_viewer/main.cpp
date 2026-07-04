#include "ashiato/sync/tracing.hpp"
#include "ashiato/sync/types.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <atomic>
#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#endif

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION) && !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace {

using namespace ashiato::sync;

constexpr float label_width = 340.0f;
constexpr float row_height = 24.0f;
constexpr float frame_pitch = 18.0f;
constexpr float packet_time_us_pitch = 0.0035f;
constexpr float packet_column_width = 300.0f;
constexpr float packet_column_gap = 140.0f;
constexpr float packet_marker_size = 13.0f;
constexpr float pill_width = 14.0f;
constexpr float pill_height = 14.0f;
constexpr float timeline_header_height = 24.0f;
constexpr ImU32 timeline_link_color = IM_COL32(194, 200, 210, 125);

struct SelectedCell {
    int source_index = -1;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
    ashiato::Entity component{};
    SyncFrame frame = 0;
    KTraceRunId run = 0;
    std::vector<std::uint32_t> event_indices;
};

bool operator==(const SelectedCell& lhs, const SelectedCell& rhs) {
    return lhs.source_index == rhs.source_index &&
        lhs.network_id == rhs.network_id &&
        lhs.component == rhs.component &&
        lhs.frame == rhs.frame &&
        lhs.run == rhs.run &&
        lhs.event_indices == rhs.event_indices;
}

bool operator!=(const SelectedCell& lhs, const SelectedCell& rhs) {
    return !(lhs == rhs);
}

struct SelectedRecordDetail {
    const KTraceRecord* record = nullptr;
    const KTraceSourceHistory* source_history = nullptr;
    std::string source;
};

struct SelectedPacketChip {
    int source_index = -1;
    std::uint32_t event_index = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::pair<int, std::uint32_t>> event_indices;
};

bool operator==(const SelectedPacketChip& lhs, const SelectedPacketChip& rhs) {
    return lhs.source_index == rhs.source_index &&
        lhs.event_index == rhs.event_index &&
        lhs.event_indices == rhs.event_indices;
}

bool operator!=(const SelectedPacketChip& lhs, const SelectedPacketChip& rhs) {
    return !(lhs == rhs);
}

struct EventVisualKey {
    int source_index = -1;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
    ashiato::Entity component{};
    SyncFrame frame = 0;
    KTraceRunId run = 0;
};

bool operator==(const EventVisualKey& lhs, const EventVisualKey& rhs) {
    return lhs.source_index == rhs.source_index &&
        lhs.network_id == rhs.network_id &&
        lhs.component == rhs.component &&
        lhs.frame == rhs.frame &&
        lhs.run == rhs.run;
}

struct EventVisualKeyHash {
    std::size_t operator()(const EventVisualKey& key) const noexcept {
        std::uint64_t mixed = key.network_id ^ (std::uint64_t{static_cast<std::uint32_t>(key.source_index)} << 32U);
        mixed ^= key.component.value + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        mixed ^= std::uint64_t{key.frame} + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        mixed ^= std::uint64_t{key.run} + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        return std::hash<std::uint64_t>{}(mixed);
    }
};

struct PacketVisualKey {
    int source_index = -1;
    std::uint32_t record_index = 0;
};

bool operator==(const PacketVisualKey& lhs, const PacketVisualKey& rhs) {
    return lhs.source_index == rhs.source_index && lhs.record_index == rhs.record_index;
}

struct PacketVisualKeyHash {
    std::size_t operator()(const PacketVisualKey& key) const noexcept {
        const std::uint64_t mixed =
            std::uint64_t{static_cast<std::uint32_t>(key.source_index)} << 32U |
            std::uint64_t{key.record_index};
        return std::hash<std::uint64_t>{}(mixed);
    }
};

struct SourceMetrics {
    SyncFrame min_frame = 0;
    SyncFrame max_frame = 0;
    int rows = 0;
    int cells = 0;
};

struct TimelineNavCell {
    SelectedCell selected;
    int row = 0;
};

struct PacketKeyValues {
    std::unordered_map<std::string, std::string> values;
};

struct PacketEventInfo {
    int source_index = -1;
    std::uint32_t record_index = 0;
    SyncFrame frame = 0;
    std::uint64_t absolute_us = 0;
    std::uint64_t relative_us = 0;
    ClientId client = invalid_client_id;
    std::string direction;
    std::string message;
    std::string sequence;
    std::string acks;
    std::string baseline;
    std::string server_frame;
    std::string input_frames;
    std::string input_ack;
    std::string applied;
    std::string apply_failure;
    bool server_side = false;
    bool send = false;
    int lane = 0;
    float lane_offset = 0.0f;
    int flow_index = -1;
};

struct PacketEndpointKey {
    ClientId client = invalid_client_id;
    bool server_side = false;
    std::string match_key;
};

bool operator==(const PacketEndpointKey& lhs, const PacketEndpointKey& rhs) {
    return lhs.client == rhs.client &&
        lhs.server_side == rhs.server_side &&
        lhs.match_key == rhs.match_key;
}

struct PacketEndpointKeyHash {
    std::size_t operator()(const PacketEndpointKey& key) const noexcept {
        std::size_t mixed = std::hash<ClientId>{}(key.client);
        mixed ^= std::hash<bool>{}(key.server_side) + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        mixed ^= std::hash<std::string>{}(key.match_key) + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        return mixed;
    }
};

struct PacketFlow {
    int send_event = -1;
    int receive_event = -1;
    ClientId client = invalid_client_id;
    bool server_to_client = true;
};

struct PacketFrameMarker {
    SyncFrame frame = 0;
    std::uint64_t absolute_us = 0;
    std::uint64_t relative_us = 0;
};

struct PacketClientTimeline {
    ClientId client = invalid_client_id;
    std::vector<int> event_indices;
    std::vector<int> flow_indices;
    std::vector<PacketFrameMarker> server_frames;
    std::vector<PacketFrameMarker> client_frames;
};

enum class PayloadSortMode {
    Time,
    SizeDescending
};

enum class PayloadAggregateSortColumn {
    Name,
    Count,
    TotalBits,
    BandwidthBps,
    AvgBits,
    MaxBits,
    TotalExclusiveBits,
    AvgExclusiveBits,
};

enum class PayloadSourceFilter {
    All,
    Network,
    Replay
};

enum class PayloadDirectionFilter {
    All,
    Outgoing,
    Incoming
};

struct PayloadScopeInfo {
    std::uint32_t id = UINT32_MAX;
    std::uint32_t parent = UINT32_MAX;
    int depth = 0;
    std::string name;
    std::string path;
    std::uint64_t begin_bits = 0;
    std::uint64_t end_bits = 0;
    std::uint64_t payload_bits = 0;
};

struct PayloadPacketInfo {
    int source_index = -1;
    std::uint32_t record_index = 0;
    SyncFrame frame = 0;
    ClientId client = invalid_client_id;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
    ashiato::Entity component{};
    ashiato::Entity server_entity{};
    SyncTracePayloadSource payload_source = SyncTracePayloadSource::Network;
    std::uint8_t payload_tag_bits = 0;
    std::string source;
    std::string root_name;
    std::string data;
    std::uint64_t wire_bits = 0;
    std::uint64_t absolute_us = 0;
    std::vector<PayloadScopeInfo> scopes;
};

struct PayloadAggregateRow {
    std::string path;
    std::string name;
    int depth = 0;
    std::uint64_t count = 0;
    std::uint64_t total_bits = 0;
    std::uint64_t max_bits = 0;
    std::uint64_t total_exclusive_bits = 0;
    std::uint64_t max_exclusive_bits = 0;
    int max_packet_index = -1;
};

struct PayloadBandwidthBucket {
    ClientId client = invalid_client_id;
    SyncTracePayloadSource payload_source = SyncTracePayloadSource::Network;
    std::uint8_t payload_tag_bits = 0;
    std::uint64_t begin_us = 0;
    std::uint64_t end_us = 0;
    std::uint64_t total_bits = 0;
    std::uint64_t packets = 0;
};

struct PayloadSummary {
    std::uint64_t total_bits = 0;
    std::uint64_t duration_us = 0;
    std::uint64_t packets = 0;
};

struct PayloadClientSummary {
    SyncTracePayloadSource payload_source = SyncTracePayloadSource::Network;
    ClientId client = invalid_client_id;
    std::uint64_t total_bits = 0;
    std::uint64_t packets = 0;
};

struct PayloadClientKey {
    SyncTracePayloadSource payload_source = SyncTracePayloadSource::Network;
    ClientId client = invalid_client_id;

    bool operator==(const PayloadClientKey& other) const noexcept {
        return payload_source == other.payload_source && client == other.client;
    }
};

struct PayloadClientKeyHash {
    std::size_t operator()(const PayloadClientKey& key) const noexcept {
        std::size_t mixed = std::hash<unsigned>{}(static_cast<unsigned>(key.payload_source));
        mixed ^= std::hash<ClientId>{}(key.client) + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        return mixed;
    }
};

struct DirectoryPickerEntry {
    std::string name;
    std::string path;
    bool has_ktrace_files = false;
};

struct BenchmarkOptions {
    bool enabled = false;
    int frames = 1200;
    std::string report_path;
};

struct FrameTiming {
    double total_ms = 0.0;
    double timeline_ms = 0.0;
    double details_ms = 0.0;
    double selection_ms = 0.0;
    int visible_rows = 0;
    int visible_cells = 0;
};

struct BenchmarkState {
    BenchmarkOptions options;
    std::vector<FrameTiming> frames;
    double load_ms = 0.0;
    int frame_index = 0;
    bool click_requested = false;
    bool report_written = false;
};

struct TraceLoadMessage {
    enum class Type {
        SourceBegin,
        Progress,
        SourceReady,
        Finished,
        Failed
    };

    Type type = Type::Progress;
    int source_index = -1;
    KTraceFileHeader source;
    KTraceSourceHistory history;
    SourceMetrics metrics;
    std::vector<SelectedCell> benchmark_candidates;
    std::uint64_t bytes_read = 0;
    std::uint64_t total_bytes = 0;
    double load_ms = 0.0;
    std::string error;
};

struct TraceLoadState {
    std::thread worker;
    std::shared_ptr<std::atomic_bool> cancel;
    std::mutex mutex;
    std::deque<TraceLoadMessage> messages;
    std::uint64_t bytes_read = 0;
    std::uint64_t total_bytes = 0;
    double load_ms = 0.0;
    bool active = false;
    bool finished = false;
};

enum class ViewerMode {
    Frames,
    EventLog,
    Payloads
};

struct EntityExpansionKey {
    int source_index = -1;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
};

bool operator==(const EntityExpansionKey& lhs, const EntityExpansionKey& rhs) {
    return lhs.source_index == rhs.source_index && lhs.network_id == rhs.network_id;
}

struct EntityExpansionKeyHash {
    std::size_t operator()(const EntityExpansionKey& key) const noexcept {
        const std::uint64_t mixed = key.network_id ^ (std::uint64_t{static_cast<std::uint32_t>(key.source_index)} << 32U);
        return std::hash<std::uint64_t>{}(mixed);
    }
};

struct ComponentExpansionKey {
    int source_index = -1;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
    ashiato::Entity component{};
};

bool operator==(const ComponentExpansionKey& lhs, const ComponentExpansionKey& rhs) {
    return lhs.source_index == rhs.source_index &&
        lhs.network_id == rhs.network_id &&
        lhs.component == rhs.component;
}

struct ComponentExpansionKeyHash {
    std::size_t operator()(const ComponentExpansionKey& key) const noexcept {
        std::uint64_t mixed = key.network_id ^ (std::uint64_t{static_cast<std::uint32_t>(key.source_index)} << 32U);
        mixed ^= key.component.value + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
        return std::hash<std::uint64_t>{}(mixed);
    }
};

constexpr int component_lane = -1;
constexpr int unassigned_lane = -2;

struct RunRenderItem {
    KTraceRunId run_id = invalid_trace_run;
    int lane = 0;
    int parent_lane = component_lane;
    SyncFrame start_frame = 0;
    SyncFrame end_frame = 0;
    KTraceRunId prev = invalid_trace_run;
    std::vector<std::pair<SyncFrame, int>> frame_lanes;
};

struct RunLaneLayout {
    std::vector<RunRenderItem> items;
    int lane_count = 0;
};

struct ViewerState {
    SyncTraceHistory history;
    std::array<char, 1024> directory{};
    std::string status;
    bool status_is_error = false;
    SelectedCell selected;
    SelectedPacketChip selected_packet;
    int selected_source = 0;
    ViewerMode mode = ViewerMode::Frames;
    BenchmarkState benchmark;
    FrameTiming current_timing;
    SelectedCell cached_detail_selection;
    std::vector<SelectedRecordDetail> cached_details;
    std::vector<SourceMetrics> source_metrics;
    std::vector<SelectedCell> benchmark_candidates;
    std::vector<PacketEventInfo> packet_events;
    std::vector<PacketFlow> packet_flows;
    std::vector<PacketClientTimeline> packet_clients;
    std::uint64_t packet_log_min_us = 0;
    std::uint64_t packet_log_max_us = 0;
    std::vector<PayloadPacketInfo> payload_packets;
    std::vector<PayloadAggregateRow> payload_rows;
    std::vector<PayloadBandwidthBucket> payload_buckets;
    std::vector<int> payload_filtered_packets;
    PayloadSortMode payload_sort = PayloadSortMode::Time;
    PayloadAggregateSortColumn payload_row_sort_column = PayloadAggregateSortColumn::TotalBits;
    bool payload_row_sort_ascending = false;
    PayloadSourceFilter payload_source_filter = PayloadSourceFilter::All;
    PayloadDirectionFilter payload_direction_filter = PayloadDirectionFilter::All;
    std::unordered_set<PayloadClientKey, PayloadClientKeyHash> payload_excluded_clients;
    std::array<char, 256> payload_filter{};
    bool payload_filter_enabled = false;
    bool payload_hierarchy_view = false;
    bool payload_return_to_flat_on_back_to_top = false;
    int selected_payload_row = -1;
    int selected_payload_packet = -1;
    std::string selected_payload_scope_path;
    int selected_payload_scope_packet = -1;
    std::uint32_t selected_payload_scope_id = UINT32_MAX;
    bool payload_time_filter_enabled = false;
    std::uint64_t payload_filter_begin_us = 0;
    std::uint64_t payload_filter_end_us = 0;
    bool payload_drag_selecting = false;
    ImVec2 payload_drag_start{};
    ImVec2 payload_drag_current{};
    std::uint64_t payload_bucket_us = 100000;
    std::uint64_t payload_min_us = 0;
    std::uint64_t payload_max_us = 0;
    bool payload_dirty = true;
    bool payload_view_dirty = true;
    std::unordered_map<ClientEntityNetworkId, ashiato::Entity> server_entities_by_network_id;
    int selected_packet_client = 0;
    std::array<char, 1024> picker_path{};
    std::vector<DirectoryPickerEntry> picker_entries;
    std::string picker_error;
    int picker_selected = -1;
    bool directory_has_ktrace_files = false;
    bool picker_path_has_ktrace_files = false;
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    std::string screenshot_path;
    std::string last_screenshot_path;
    std::string control_socket_path;
#endif
    std::unordered_set<EntityExpansionKey, EntityExpansionKeyHash> expanded_entities;
    std::unordered_set<ComponentExpansionKey, ComponentExpansionKeyHash> expanded_components;
    std::unordered_map<ComponentExpansionKey, RunLaneLayout, ComponentExpansionKeyHash> run_lane_layouts;
    std::unordered_map<EventVisualKey, float, EventVisualKeyHash> event_hover_alpha;
    std::unordered_map<PacketVisualKey, float, PacketVisualKeyHash> packet_hover_alpha;
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    std::uint32_t screenshot_counter = 0;
    bool screenshot_requested = false;
    bool screenshot_failed = false;
#endif
    bool selected_source_dirty = true;
    bool details_dirty = true;
    bool packet_details_dirty = true;
    bool packet_log_dirty = false;
    bool scroll_selected_frame_into_view = false;
    bool scroll_selected_packet_into_view = false;
    TraceLoadState loader;
};

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
struct PendingMouseClick {
    float x = 0.0f;
    float y = 0.0f;
    int button = 0;
    int phase = 0;
};

struct PendingMouseButton {
    float x = 0.0f;
    float y = 0.0f;
    int button = 0;
    bool down = false;
};

struct AutomationInput {
    std::vector<ImVec2> moves;
    std::vector<PendingMouseClick> clicks;
    std::vector<PendingMouseButton> buttons;
    std::vector<std::array<float, 2>> scrolls;
};
#endif

bool directory_contains_ktrace_files(const std::filesystem::path& path);
void load_directory(ViewerState& state);
void update_source_metrics(ViewerState& state, int source_index);
void move_selection_to(ViewerState& state, const SelectedCell& selected);
void select_packet_event(ViewerState& state, const PacketEventInfo& event);
void jump_selected_frame_to_event_log(ViewerState& state);
void jump_selected_frame_to_payload(ViewerState& state);
void jump_selected_packet_to_frame(ViewerState& state);
void jump_selected_packet_to_payload(ViewerState& state);
void jump_selected_payload_to_frame(ViewerState& state);
void jump_selected_payload_to_event_log(ViewerState& state);

struct RunVerticalSpan {
    SyncFrame frame = 0;
    int from_lane = 0;
    int to_lane = 0;
};

enum class CellVisualKind {
    Unknown,
    Applied,
    ServerGolden,
    Interpolated,
    Predicted,
    InputReceived,
    CorrectPrediction,
    Mispredicted,
    Starved,
    Removed,
    Resimulated,
    Cue
};

struct CellVisual {
    CellVisualKind kind = CellVisualKind::Unknown;
    ImU32 fill = IM_COL32(120, 120, 120, 255);
    ImU32 accent = IM_COL32(255, 255, 255, 180);
    const char* label = "unknown";
};

bool has_state(const KTraceFrameCell& cell, KTraceCellState state) {
    return (cell.state_mask & static_cast<std::uint32_t>(state)) != 0U;
}

bool is_cue_row_component(ashiato::Entity component) noexcept {
    return (component.value & (std::uint64_t{1} << 63U)) != 0U;
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end = Clock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
std::string default_screenshot_path(ViewerState& state) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "ashiato_sync_trace_viewer_" + std::to_string(ms) + "_" + std::to_string(state.screenshot_counter++) + ".png";
    return path.string();
}

std::string screenshot_output_path(ViewerState& state) {
    if (state.screenshot_path.empty()) {
        return default_screenshot_path(state);
    }
    std::filesystem::path path = state.screenshot_path;
    if (path.extension() != ".png") {
        path.replace_extension(".png");
    }
    return path.string();
}

void request_screenshot(ViewerState& state) {
    state.screenshot_requested = true;
}

void apply_automation_input(AutomationInput& input) {
    ImGuiIO& io = ImGui::GetIO();
    for (const ImVec2& move : input.moves) {
        io.AddMousePosEvent(move.x, move.y);
    }
    input.moves.clear();
    for (const std::array<float, 2>& scroll : input.scrolls) {
        io.AddMouseWheelEvent(scroll[0], scroll[1]);
    }
    input.scrolls.clear();
    for (const PendingMouseButton& button : input.buttons) {
        io.AddMousePosEvent(button.x, button.y);
        io.AddMouseButtonEvent(button.button, button.down);
    }
    input.buttons.clear();

    std::vector<PendingMouseClick> continuing;
    continuing.reserve(input.clicks.size());
    for (PendingMouseClick& click : input.clicks) {
        io.AddMousePosEvent(click.x, click.y);
        if (click.phase == 0) {
            io.AddMouseButtonEvent(click.button, true);
            click.phase = 1;
            continuing.push_back(click);
        } else {
            io.AddMouseButtonEvent(click.button, false);
        }
    }
    input.clicks = std::move(continuing);
}

bool write_png_rgb(const std::string& path, int width, int height, const std::vector<unsigned char>& bottom_up_rgb, std::string& error) {
    const std::filesystem::path output_path(path);
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "failed to create screenshot directory: " + ec.message();
            return false;
        }
    }

    if (bottom_up_rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U) {
        error = "invalid screenshot buffer size";
        return false;
    }

    stbi_flip_vertically_on_write(1);
    if (stbi_write_png(path.c_str(), width, height, 3, bottom_up_rgb.data(), width * 3) == 0) {
        error = "failed to write screenshot file";
        return false;
    }
    return true;
}

bool write_framebuffer_png(const std::string& path, int width, int height, std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "invalid framebuffer size";
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    return write_png_rgb(path, width, height, pixels, error);
}
#endif

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
int mouse_button_from_token(const std::string& token) {
    if (token.empty() || token == "left" || token == "0") {
        return 0;
    }
    if (token == "right" || token == "1") {
        return 1;
    }
    if (token == "middle" || token == "2") {
        return 2;
    }
    return -1;
}

std::string handle_control_command(
    const std::string& line,
    ViewerState& state,
    AutomationInput& input,
    GLFWwindow* window) {
    std::istringstream stream(line);
    std::string command;
    stream >> command;
    if (command.empty()) {
        return "OK\n";
    }
    if (command == "help") {
        return "OK commands: load path, click x y [left|right|middle], move x y, mouse_down x y [button], mouse_up x y [button], scroll x y, screenshot [path], close, status\n";
    }
    if (command == "status") {
        std::ostringstream out;
        out << "OK sources=" << state.history.sources.size()
            << " selected_source=" << state.selected_source
            << " loading=" << (state.loader.active ? 1 : 0)
            << " bytes_read=" << state.loader.bytes_read
            << " total_bytes=" << state.loader.total_bytes
            << " screenshot_path=" << (state.screenshot_path.empty() ? std::string("<default>") : state.screenshot_path)
            << " last_screenshot=" << state.last_screenshot_path
            << "\n";
        return out.str();
    }
    if (command == "move") {
        float x = 0.0f;
        float y = 0.0f;
        if (!(stream >> x >> y)) {
            return "ERR usage: move x y\n";
        }
        input.moves.push_back(ImVec2(x, y));
        return "OK\n";
    }
    if (command == "load") {
        std::string path;
        if (!(stream >> path)) {
            return "ERR usage: load path\n";
        }
        std::snprintf(state.directory.data(), state.directory.size(), "%s", path.c_str());
        load_directory(state);
        std::ostringstream out;
        out << "OK sources=" << state.history.sources.size() << "\n";
        return out.str();
    }
    if (command == "click" || command == "mouse_down" || command == "mouse_up") {
        float x = 0.0f;
        float y = 0.0f;
        std::string button_token;
        if (!(stream >> x >> y)) {
            return "ERR usage: click x y [button]\n";
        }
        stream >> button_token;
        const int button = mouse_button_from_token(button_token);
        if (button < 0) {
            return "ERR unknown mouse button\n";
        }
        if (command == "click") {
            input.clicks.push_back(PendingMouseClick{x, y, button, 0});
        } else {
            input.buttons.push_back(PendingMouseButton{x, y, button, command == "mouse_down"});
        }
        return "OK\n";
    }
    if (command == "scroll") {
        float x = 0.0f;
        float y = 0.0f;
        if (!(stream >> x >> y)) {
            return "ERR usage: scroll x y\n";
        }
        input.scrolls.push_back({x, y});
        return "OK\n";
    }
    if (command == "screenshot") {
        std::string path;
        if (stream >> path) {
            state.screenshot_path = path;
        }
        request_screenshot(state);
        return "OK\n";
    }
    if (command == "close" || command == "quit") {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return "OK\n";
    }
    return "ERR unknown command\n";
}

#if !defined(_WIN32)
struct ControlClient {
    int fd = -1;
    std::string input;
};

struct ControlServer {
    int listen_fd = -1;
    std::string path;
    std::vector<ControlClient> clients;
};

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool set_nonblocking(int fd, std::string& error) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool send_response(int fd, const std::string& response) {
    const char* data = response.data();
    std::size_t remaining = response.size();
    while (remaining > 0) {
        const ssize_t sent = send(fd, data, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool start_control_server(ControlServer& server, const std::string& path, std::string& error) {
    if (path.empty()) {
        return true;
    }
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "control socket path is too long";
        return false;
    }

    server.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server.listen_fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (!set_nonblocking(server.listen_fd, error)) {
        close_fd(server.listen_fd);
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
    unlink(path.c_str());
    if (bind(server.listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        error = std::strerror(errno);
        close_fd(server.listen_fd);
        return false;
    }
    if (listen(server.listen_fd, 8) < 0) {
        error = std::strerror(errno);
        close_fd(server.listen_fd);
        unlink(path.c_str());
        return false;
    }
    server.path = path;
    return true;
}

void stop_control_server(ControlServer& server) {
    for (ControlClient& client : server.clients) {
        close_fd(client.fd);
    }
    server.clients.clear();
    close_fd(server.listen_fd);
    if (!server.path.empty()) {
        unlink(server.path.c_str());
        server.path.clear();
    }
}

void poll_control_server(
    ControlServer& server,
    ViewerState& state,
    AutomationInput& input,
    GLFWwindow* window) {
    if (server.listen_fd < 0) {
        return;
    }

    while (true) {
        const int fd = accept(server.listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                state.status = std::string("control socket accept failed: ") + std::strerror(errno);
                state.status_is_error = true;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        std::string error;
        if (!set_nonblocking(fd, error)) {
            int rejected = fd;
            close_fd(rejected);
            continue;
        }
        server.clients.push_back(ControlClient{fd, {}});
    }

    char buffer[1024];
    for (std::size_t index = 0; index < server.clients.size();) {
        ControlClient& client = server.clients[index];
        bool close_client = false;
        while (!close_client) {
            const ssize_t received = recv(client.fd, buffer, sizeof(buffer), 0);
            if (received > 0) {
                client.input.append(buffer, buffer + received);
                std::size_t newline = std::string::npos;
                while ((newline = client.input.find('\n')) != std::string::npos) {
                    std::string line = client.input.substr(0, newline);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    client.input.erase(0, newline + 1U);
                    if (!send_response(client.fd, handle_control_command(line, state, input, window))) {
                        close_client = true;
                        break;
                    }
                }
            } else if (received == 0) {
                close_client = true;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno != EINTR) {
                close_client = true;
            }
        }

        if (close_client) {
            close_fd(client.fd);
            server.clients[index] = server.clients.back();
            server.clients.pop_back();
        } else {
            ++index;
        }
    }
}
#else
struct ControlServer {};

bool start_control_server(ControlServer&, const std::string& path, std::string& error) {
    if (!path.empty()) {
        error = "control sockets are not supported on this platform";
        return false;
    }
    return true;
}

void stop_control_server(ControlServer&) {}

void poll_control_server(ControlServer&, ViewerState&, AutomationInput&, GLFWwindow*) {}
#endif
#endif

SyncFrame component_end_frame(const KTraceComponentRow& component, SyncFrame fallback) {
    SyncFrame end = fallback;
    for (const KTraceFrameRun& run : component.runs) {
        for (const KTraceFrameCell& cell : run.frames) {
            end = std::max(end, cell.frame);
        }
    }
    return end;
}

SyncFrame run_end_frame(const KTraceFrameRun& run) {
    SyncFrame end = run.start_frame;
    for (const KTraceFrameCell& cell : run.frames) {
        end = std::max(end, cell.frame);
    }
    return end;
}

std::vector<SyncFrame> run_frames_for_layout(const KTraceFrameRun& run) {
    std::vector<SyncFrame> frames;
    frames.push_back(run.start_frame);
    for (const KTraceFrameCell& cell : run.frames) {
        frames.push_back(cell.frame);
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

bool lane_has_gap_before(SyncFrame lane_end, SyncFrame frame) {
    return lane_end < frame;
}

bool lane_has_open_frame_before(SyncFrame lane_end, SyncFrame frame) {
    return lane_end < frame && frame - lane_end > 1U;
}

SyncFrame lane_end_at(const std::vector<SyncFrame>& lane_ends, SyncFrame component_end, int lane) {
    return lane == component_lane ? component_end : lane_ends[static_cast<std::size_t>(lane)];
}

void set_lane_end(std::vector<SyncFrame>& lane_ends, SyncFrame& component_end, int lane, SyncFrame frame) {
    if (lane == component_lane) {
        component_end = frame;
        return;
    }
    while (static_cast<int>(lane_ends.size()) <= lane) {
        lane_ends.push_back(0);
    }
    lane_ends[static_cast<std::size_t>(lane)] = frame;
}

bool vertical_spans_overlap(int from_a, int to_a, int from_b, int to_b) {
    const int min_a = std::min(from_a, to_a);
    const int max_a = std::max(from_a, to_a);
    const int min_b = std::min(from_b, to_b);
    const int max_b = std::max(from_b, to_b);
    return min_a < max_b && min_b < max_a;
}

bool vertical_span_available(
    const std::unordered_map<SyncFrame, std::vector<RunVerticalSpan>>& spans_by_frame,
    SyncFrame frame,
    int from_lane,
    int to_lane) {
    if (from_lane == to_lane) {
        return true;
    }
    const auto found = spans_by_frame.find(frame);
    if (found == spans_by_frame.end()) {
        return true;
    }
    const std::vector<RunVerticalSpan>& spans = found->second;
    for (const RunVerticalSpan& span : spans) {
        if (vertical_spans_overlap(from_lane, to_lane, span.from_lane, span.to_lane)) {
            return false;
        }
    }
    return true;
}

void reserve_vertical_span(
    std::unordered_map<SyncFrame, std::vector<RunVerticalSpan>>& spans_by_frame,
    SyncFrame frame,
    int from_lane,
    int to_lane) {
    if (from_lane == to_lane) {
        return;
    }
    spans_by_frame[frame].push_back(RunVerticalSpan{frame, from_lane, to_lane});
}

int first_available_run_lane(
    const std::vector<SyncFrame>& lane_ends,
    const std::unordered_map<SyncFrame, std::vector<RunVerticalSpan>>& spans_by_frame,
    int first_lane,
    SyncFrame frame,
    int parent_lane) {
    int lane = std::max(0, first_lane);
    for (; lane < static_cast<int>(lane_ends.size()); ++lane) {
        if (lane_has_gap_before(lane_ends[static_cast<std::size_t>(lane)], frame) &&
            vertical_span_available(spans_by_frame, frame, parent_lane, lane)) {
            return lane;
        }
    }
    return lane;
}

int run_lane_at_frame(const RunRenderItem& item, SyncFrame frame);
int run_parent_lane(const std::vector<RunRenderItem>& runs, std::size_t run_index);

RunLaneLayout pack_run_lanes(const KTraceComponentRow& component) {
    RunLaneLayout layout;
    for (std::size_t run_index = 1; run_index < component.runs.size(); ++run_index) {
        const KTraceFrameRun& run = component.runs[run_index];
        if (run.frames.empty()) {
            continue;
        }
        layout.items.push_back(RunRenderItem{
            static_cast<KTraceRunId>(run_index),
            0,
            component_lane,
            run.start_frame,
            run_end_frame(run),
            run.prev,
            {}});
    }
    std::sort(layout.items.begin(), layout.items.end(), [](const RunRenderItem& lhs, const RunRenderItem& rhs) {
        if (lhs.start_frame != rhs.start_frame) {
            return lhs.start_frame < rhs.start_frame;
        }
        return lhs.end_frame < rhs.end_frame;
    });

    std::vector<SyncFrame> lane_ends;
    std::unordered_map<SyncFrame, std::vector<RunVerticalSpan>> vertical_spans_by_frame;
    SyncFrame component_end = component.runs.empty() ? 0 : run_end_frame(component.runs[0]);
    for (RunRenderItem& item : layout.items) {
        const KTraceFrameRun& run = component.runs[item.run_id];
        const std::vector<SyncFrame> frames = run_frames_for_layout(run);
        int current_lane = unassigned_lane;
        item.frame_lanes.reserve(frames.size());
        const std::size_t item_index = static_cast<std::size_t>(&item - layout.items.data());
        const int parent_lane = run_parent_lane(layout.items, item_index);
        item.parent_lane = parent_lane;
        for (SyncFrame frame : frames) {
            int lane = unassigned_lane;
            if (current_lane == unassigned_lane) {
                lane = first_available_run_lane(lane_ends, vertical_spans_by_frame, parent_lane + 1, frame, parent_lane);
                reserve_vertical_span(vertical_spans_by_frame, frame, parent_lane, lane);
            } else {
                for (int candidate = component_lane; candidate < current_lane; ++candidate) {
                    if (candidate >= static_cast<int>(lane_ends.size())) {
                        break;
                    }
                    if (lane_has_open_frame_before(lane_end_at(lane_ends, component_end, candidate), frame) &&
                        vertical_span_available(vertical_spans_by_frame, frame, current_lane, candidate)) {
                        lane = candidate;
                        break;
                    }
                }
                if (lane == unassigned_lane) {
                    lane = current_lane;
                } else {
                    reserve_vertical_span(vertical_spans_by_frame, frame, current_lane, lane);
                }
            }
            if (lane != component_lane && lane == static_cast<int>(lane_ends.size())) {
                lane_ends.push_back(frame);
            } else {
                set_lane_end(lane_ends, component_end, lane, frame);
            }
            current_lane = lane;
            item.frame_lanes.push_back({frame, lane});
        }
        item.lane = item.frame_lanes.empty() ? 0 : item.frame_lanes.front().second;
    }
    layout.lane_count = static_cast<int>(lane_ends.size());
    return layout;
}

int run_lane_at_frame(const RunRenderItem& item, SyncFrame frame) {
    const auto found = std::lower_bound(
        item.frame_lanes.begin(),
        item.frame_lanes.end(),
        frame,
        [](const std::pair<SyncFrame, int>& frame_lane, SyncFrame target) {
            return frame_lane.first < target;
        });
    if (found != item.frame_lanes.end() && found->first == frame) {
        return found->second;
    }
    return unassigned_lane;
}

int run_parent_lane(const std::vector<RunRenderItem>& runs, std::size_t run_index) {
    const RunRenderItem& run = runs[run_index];
    if (run.prev == invalid_trace_run || run.prev == 0) {
        return component_lane;
    }
    int parent_lane = component_lane;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index == run_index) {
            continue;
        }
        const RunRenderItem& candidate = runs[index];
        if (candidate.run_id == run.prev) {
            parent_lane = run_lane_at_frame(candidate, run.start_frame);
            break;
        }
    }
    return parent_lane;
}

RunLaneLayout& get_run_lane_layout(
    ViewerState& state,
    int source_index,
    const KTraceEntityRow& entity,
    const KTraceComponentRow& component) {
    const ComponentExpansionKey key{source_index, entity.client_network_id, component.component};
    auto found = state.run_lane_layouts.find(key);
    if (found == state.run_lane_layouts.end()) {
        found = state.run_lane_layouts.emplace(key, pack_run_lanes(component)).first;
    }
    return found->second;
}

void clear_run_lane_cache_for_source(ViewerState& state, int source_index) {
    for (auto it = state.run_lane_layouts.begin(); it != state.run_lane_layouts.end();) {
        if (it->first.source_index == source_index) {
            it = state.run_lane_layouts.erase(it);
        } else {
            ++it;
        }
    }
}

std::string event_name(SyncTraceEventType type) {
    switch (type) {
    case SyncTraceEventType::ClientConnected: return "client connected";
    case SyncTraceEventType::ClientDisconnected: return "client disconnected";
    case SyncTraceEventType::EntityStartedSyncing: return "entity started syncing";
    case SyncTraceEventType::EntityReceived: return "entity received";
    case SyncTraceEventType::ComponentSent: return "component sent";
    case SyncTraceEventType::ComponentReceived: return "component received";
    case SyncTraceEventType::ComponentApplied: return "component applied";
    case SyncTraceEventType::ComponentRemoved: return "component removed";
    case SyncTraceEventType::TagSent: return "tag sent";
    case SyncTraceEventType::TagApplied: return "tag applied";
    case SyncTraceEventType::ModeChanged: return "mode changed";
    case SyncTraceEventType::BufferedStarved: return "buffered starved";
    case SyncTraceEventType::PredictionRollbackConflict: return "rollback conflict";
    case SyncTraceEventType::FrameComponent: return "frame component";
    case SyncTraceEventType::CueEmitted: return "cue emitted";
    case SyncTraceEventType::CueSent: return "cue sent";
    case SyncTraceEventType::CueReceived: return "cue received";
    case SyncTraceEventType::CuePlayed: return "cue played";
    case SyncTraceEventType::CueRolledBack: return "cue rolled back";
    case SyncTraceEventType::CueConfirmed: return "cue confirmed";
    case SyncTraceEventType::TagReceived: return "tag received";
    case SyncTraceEventType::EntityDestroyed: return "entity destroyed";
    case SyncTraceEventType::ResimulatedFrameComponent: return "resimulated frame component";
    case SyncTraceEventType::ComponentName: return "component name";
    case SyncTraceEventType::CueName: return "cue name";
    case SyncTraceEventType::RollbackReason: return "rollback reason";
    case SyncTraceEventType::InputStarved: return "input starved";
    case SyncTraceEventType::PacketLog: return "packet log";
    case SyncTraceEventType::ClockSkew: return "clock skew";
    case SyncTraceEventType::SerializationPayload: return "serialization payload";
    case SyncTraceEventType::SerializationPayloadTagName: return "serialization payload tag name";
    }
    return "unknown";
}

bool is_cue_event(SyncTraceEventType type) {
    return type == SyncTraceEventType::CueEmitted ||
        type == SyncTraceEventType::CueSent ||
        type == SyncTraceEventType::CueReceived ||
        type == SyncTraceEventType::CuePlayed ||
        type == SyncTraceEventType::CueRolledBack ||
        type == SyncTraceEventType::CueConfirmed;
}

std::string normalized_cue_data_key(const std::string& data) {
    std::string out;
    std::size_t begin = 0;
    while (begin <= data.size()) {
        const std::size_t end = data.find(',', begin);
        const std::size_t token_end = end == std::string::npos ? data.size() : end;
        const std::string token = data.substr(begin, token_end - begin);
        if (!token.empty() &&
            token.rfind("source=", 0) != 0 &&
            token.rfind("rollback_reason=", 0) != 0) {
            if (!out.empty()) {
                out += ",";
            }
            out += token;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return out;
}

std::string cue_instance_key(const SyncTraceEvent& event) {
    std::string key = std::to_string(event.frame);
    key += "|";
    key += std::to_string(event.cue_type);
    key += "|";
    key += normalized_cue_data_key(event.data);
    return key;
}

std::size_t cue_lifecycle_slot(SyncTraceEventType type) {
    switch (type) {
    case SyncTraceEventType::CueEmitted: return 0;
    case SyncTraceEventType::CueSent: return 1;
    case SyncTraceEventType::CueReceived: return 2;
    case SyncTraceEventType::CuePlayed: return 3;
    case SyncTraceEventType::CueRolledBack: return 4;
    case SyncTraceEventType::CueConfirmed: return 5;
    default: return 0;
    }
}

std::string mode_name(ReplicationClientMode mode) {
    switch (mode) {
    case ReplicationClientMode::Snap: return "snap";
    case ReplicationClientMode::BufferedInterpolation: return "buffered interpolation";
    case ReplicationClientMode::Predict: return "predict";
    }
    return "unknown";
}

std::string source_label(const KTraceSourceHistory& source) {
    if (source.role == SyncTraceRole::Server) {
        return "server";
    }
    return "client " + std::to_string(source.client);
}

bool packet_logs_enabled_in_trace(const ViewerState& state) {
    return std::any_of(state.history.sources.begin(), state.history.sources.end(), [](const KTraceSourceHistory& source) {
        return (source.flags & ktrace_flag_packet_logs) != 0U;
    });
}

std::uint64_t source_recorded_us(const KTraceSourceHistory& source) {
    return source.recorded_unix_ns / 1000U;
}

std::vector<std::string> split_packet_fields(const std::string& data) {
    std::vector<std::string> fields;
    std::string field;
    int bracket_depth = 0;
    for (char c : data) {
        if (c == '[') {
            ++bracket_depth;
        } else if (c == ']' && bracket_depth > 0) {
            --bracket_depth;
        }
        if (c == ',' && bracket_depth == 0) {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(c);
        }
    }
    if (!field.empty()) {
        fields.push_back(field);
    }
    return fields;
}

PacketKeyValues parse_packet_data(const std::string& data) {
    PacketKeyValues parsed;
    for (const std::string& field : split_packet_fields(data)) {
        const std::size_t equals = field.find('=');
        if (equals == std::string::npos || equals == 0U) {
            continue;
        }
        parsed.values[field.substr(0, equals)] = field.substr(equals + 1U);
    }
    return parsed;
}

std::string packet_value(const PacketKeyValues& values, const char* key) {
    const auto found = values.values.find(key);
    return found != values.values.end() ? found->second : std::string{};
}

std::uint64_t packet_u64(const PacketKeyValues& values, const char* key) {
    const std::string value = packet_value(values, key);
    if (value.empty()) {
        return 0;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (...) {
        return 0;
    }
}

ClientId parse_packet_client(const PacketKeyValues& values, const KTraceSourceHistory& source, const SyncTraceEvent& event) {
    const std::string client = packet_value(values, "client");
    if (!client.empty()) {
        try {
            return static_cast<ClientId>(std::stoull(client));
        } catch (...) {
        }
    }
    if (event.client != invalid_client_id) {
        return event.client;
    }
    return source.role == SyncTraceRole::Client ? source.client : invalid_client_id;
}

ClientEntityNetworkId trace_event_network_key(const SyncTraceEvent& event) {
    if (event.role == SyncTraceRole::Server && event.server_entity) {
        return event.server_entity.value;
    }
    if (event.client_network_id != invalid_client_entity_network_id) {
        return event.client_network_id;
    }
    if (event.client != invalid_client_id && event.wire_network_id != 0U) {
        return make_client_entity_network_id(event.client, event.wire_network_id, event.network_version);
    }
    if (event.server_entity) {
        return event.server_entity.value;
    }
    return invalid_client_entity_network_id;
}

std::string packet_match_key(const PacketEventInfo& event) {
    std::ostringstream out;
    out << event.client << "|" << event.message << "|";
    if (event.message == "server_update") {
        out << event.sequence;
    } else if (event.message == "client_ping" || event.message == "server_pong") {
        out << event.sequence;
    } else if (event.message == "client_input") {
        out << event.acks << "|" << event.baseline << "|" << event.input_frames;
    } else {
        out << event.acks;
    }
    return out.str();
}

PacketEndpointKey packet_receive_endpoint_key(const PacketEventInfo& event) {
    return PacketEndpointKey{event.client, event.server_side, packet_match_key(event)};
}

using PacketReceiveOrder = std::set<std::pair<std::uint64_t, int>>;

int choose_packet_receive_candidate(
    const PacketEventInfo& send,
    const PacketReceiveOrder& receive_order,
    const std::vector<PacketEventInfo>& events) {
    if (receive_order.empty()) {
        return -1;
    }
    const auto after = receive_order.lower_bound({send.absolute_us, std::numeric_limits<int>::min()});
    int best_receive = after != receive_order.end() ? after->second : -1;
    std::uint64_t best_delta = best_receive >= 0
        ? events[static_cast<std::size_t>(best_receive)].absolute_us - send.absolute_us
        : std::numeric_limits<std::uint64_t>::max();
    if (after != receive_order.begin()) {
        const auto before = std::prev(after);
        const PacketEventInfo& before_event = events[static_cast<std::size_t>(before->second)];
        const std::uint64_t before_delta = send.absolute_us >= before_event.absolute_us
            ? send.absolute_us - before_event.absolute_us
            : before_event.absolute_us - send.absolute_us;
        if (best_receive < 0 || before_delta < best_delta) {
            best_receive = before->second;
        }
    }
    return best_receive;
}

std::string packet_marker_label(const PacketEventInfo& event) {
    if (event.message == "client_input") {
        return event.input_frames.empty() || event.input_frames == "none"
            ? std::to_string(event.frame)
            : event.input_frames;
    }
    if (event.message == "server_update") {
        return event.server_frame.empty() ? std::to_string(event.frame) : event.server_frame;
    }
    return {};
}

bool packet_message_is_ping_or_pong(const PacketEventInfo& event) {
    return event.message == "ping" ||
        event.message == "pong" ||
        event.message == "client_ping" ||
        event.message == "server_pong";
}

bool packet_update_apply_failed(const PacketEventInfo& event) {
    return !event.send && event.message == "server_update" && event.applied == "false";
}

bool packet_update_apply_failed_from_stale_frame(const PacketEventInfo& event) {
    constexpr const char* stale_delta_suffix = "delta_stale_frame";
    constexpr const char* stale_full_suffix = "full_stale_frame";
    if (!packet_update_apply_failed(event)) {
        return false;
    }
    const auto matches_suffix = [&](const char* suffix) {
        const std::size_t suffix_len = std::strlen(suffix);
        return event.apply_failure == suffix ||
            (event.apply_failure.size() > suffix_len &&
                event.apply_failure.compare(event.apply_failure.size() - suffix_len, suffix_len, suffix) == 0);
    };
    return matches_suffix(stale_delta_suffix) || matches_suffix(stale_full_suffix);
}

float packet_marker_width_estimate(const PacketEventInfo& event) {
    if (event.send && (event.message == "client_input" || event.message == "server_update")) {
        const std::string label = packet_marker_label(event);
        return std::max(34.0f, static_cast<float>(label.size()) * 7.5f + 12.0f);
    }
    if (event.send && packet_message_is_ping_or_pong(event)) {
        return 18.0f;
    }
    return packet_marker_size;
}

void assign_packet_lanes(ViewerState& state) {
    std::vector<int> order(state.packet_events.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
        const PacketEventInfo& left = state.packet_events[static_cast<std::size_t>(lhs)];
        const PacketEventInfo& right = state.packet_events[static_cast<std::size_t>(rhs)];
        if (left.client != right.client) {
            return left.client < right.client;
        }
        if (left.server_side != right.server_side) {
            return left.server_side && !right.server_side;
        }
        return left.absolute_us < right.absolute_us;
    });

    ClientId active_client = invalid_client_id;
    bool active_server_side = false;
    std::vector<std::uint64_t> lane_times;
    std::vector<float> lane_widths;
    const std::uint64_t min_gap_us = static_cast<std::uint64_t>(packet_marker_size / packet_time_us_pitch) + 1U;
    for (int event_index : order) {
        PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
        if (event.client != active_client || event.server_side != active_server_side) {
            active_client = event.client;
            active_server_side = event.server_side;
            lane_times.clear();
            lane_widths.clear();
        }
        int selected_lane = 0;
        for (; selected_lane < static_cast<int>(lane_times.size()); ++selected_lane) {
            if (event.absolute_us >= lane_times[static_cast<std::size_t>(selected_lane)] + min_gap_us) {
                break;
            }
        }
        if (selected_lane == static_cast<int>(lane_times.size())) {
            lane_times.push_back(event.absolute_us);
            lane_widths.push_back(packet_marker_width_estimate(event));
        } else {
            lane_times[static_cast<std::size_t>(selected_lane)] = event.absolute_us;
            lane_widths[static_cast<std::size_t>(selected_lane)] =
                std::max(lane_widths[static_cast<std::size_t>(selected_lane)], packet_marker_width_estimate(event));
        }
        event.lane = selected_lane;
        event.lane_offset = 0.0f;
        for (int lane = 0; lane < selected_lane; ++lane) {
            event.lane_offset += lane_widths[static_cast<std::size_t>(lane)] * 0.5f + 7.0f;
        }
        if (selected_lane != 0) {
            event.lane_offset += packet_marker_width_estimate(event) * 0.5f;
        }
    }
}

std::vector<PacketFrameMarker> source_frame_markers(const KTraceSourceHistory& source) {
    std::vector<PacketFrameMarker> markers;
    std::unordered_set<SyncFrame> seen_frames;
    const std::uint64_t source_start_us = source_recorded_us(source);
    for (const KTraceRecord& record : source.records) {
        if (record.event.frame == 0U || seen_frames.count(record.event.frame) != 0U) {
            continue;
        }
        seen_frames.insert(record.event.frame);
        markers.push_back(PacketFrameMarker{
            record.event.frame,
            source_start_us + record.timestamp_us,
            0U});
    }
    std::sort(markers.begin(), markers.end(), [](const PacketFrameMarker& lhs, const PacketFrameMarker& rhs) {
        if (lhs.absolute_us != rhs.absolute_us) {
            return lhs.absolute_us < rhs.absolute_us;
        }
        return lhs.frame < rhs.frame;
    });
    return markers;
}

void rebuild_packet_log_rows(ViewerState& state) {
    state.packet_events.clear();
    state.packet_flows.clear();
    state.packet_clients.clear();
    state.packet_log_min_us = 0;
    state.packet_log_max_us = 0;
    bool initialized = false;
    for (int source_index = 0; source_index < static_cast<int>(state.history.sources.size()); ++source_index) {
        const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
        const std::uint64_t source_start_us = source_recorded_us(source);
        for (std::uint32_t record_index = 0; record_index < source.records.size(); ++record_index) {
            const KTraceRecord& record = source.records[record_index];
            if (record.event.type != SyncTraceEventType::PacketLog) {
                continue;
            }
            const PacketKeyValues values = parse_packet_data(record.event.data);
            const std::uint64_t absolute_us = source_start_us + record.timestamp_us;
            if (!initialized) {
                state.packet_log_min_us = absolute_us;
                state.packet_log_max_us = absolute_us;
                initialized = true;
            } else {
                state.packet_log_min_us = std::min(state.packet_log_min_us, absolute_us);
                state.packet_log_max_us = std::max(state.packet_log_max_us, absolute_us);
            }
            PacketEventInfo event;
            event.source_index = source_index;
            event.record_index = record_index;
            event.frame = record.event.frame;
            event.absolute_us = absolute_us;
            event.client = parse_packet_client(values, source, record.event);
            event.direction = packet_value(values, "direction");
            event.message = packet_value(values, "message");
            event.sequence = packet_value(values, "sequence");
            event.acks = packet_value(values, "acks");
            event.baseline = packet_value(values, "baseline");
            event.server_frame = packet_value(values, "server_frame");
            event.input_frames = packet_value(values, "input_frames");
            event.input_ack = packet_value(values, "input_ack");
            event.applied = packet_value(values, "applied");
            event.apply_failure = packet_value(values, "apply_failure");
            event.server_side = source.role == SyncTraceRole::Server;
            event.send = event.direction == "out";
            state.packet_events.push_back(std::move(event));
        }
    }
    for (PacketEventInfo& event : state.packet_events) {
        event.relative_us = event.absolute_us >= state.packet_log_min_us
            ? event.absolute_us - state.packet_log_min_us
            : 0U;
    }

    std::unordered_map<PacketEndpointKey, PacketReceiveOrder, PacketEndpointKeyHash> receive_events_by_endpoint;
    receive_events_by_endpoint.reserve(state.packet_events.size());
    for (int event_index = 0; event_index < static_cast<int>(state.packet_events.size()); ++event_index) {
        const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
        if (event.send) {
            continue;
        }
        receive_events_by_endpoint[packet_receive_endpoint_key(event)].insert({event.absolute_us, event_index});
    }
    for (int send_index = 0; send_index < static_cast<int>(state.packet_events.size()); ++send_index) {
        const PacketEventInfo& send = state.packet_events[static_cast<std::size_t>(send_index)];
        if (!send.send) {
            continue;
        }
        int best_receive = -1;
        auto endpoint = receive_events_by_endpoint.find(PacketEndpointKey{send.client, !send.server_side, packet_match_key(send)});
        if (endpoint != receive_events_by_endpoint.end()) {
            best_receive = choose_packet_receive_candidate(send, endpoint->second, state.packet_events);
        }
        PacketFlow flow;
        flow.send_event = send_index;
        flow.receive_event = best_receive;
        flow.client = send.client;
        flow.server_to_client = send.server_side;
        if (best_receive >= 0) {
            endpoint->second.erase({
                state.packet_events[static_cast<std::size_t>(best_receive)].absolute_us,
                best_receive});
        }
        state.packet_flows.push_back(flow);
        const int flow_index = static_cast<int>(state.packet_flows.size()) - 1;
        state.packet_events[static_cast<std::size_t>(send_index)].flow_index = flow_index;
        if (best_receive >= 0) {
            state.packet_events[static_cast<std::size_t>(best_receive)].flow_index = flow_index;
        }
    }

    for (int event_index = 0; event_index < static_cast<int>(state.packet_events.size()); ++event_index) {
        const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
        if (event.client == invalid_client_id) {
            continue;
        }
        auto found = std::find_if(state.packet_clients.begin(), state.packet_clients.end(), [&](const PacketClientTimeline& client) {
            return client.client == event.client;
        });
        if (found == state.packet_clients.end()) {
            state.packet_clients.push_back(PacketClientTimeline{event.client, {}, {}});
            found = state.packet_clients.end() - 1;
        }
        found->event_indices.push_back(event_index);
    }
    for (int flow_index = 0; flow_index < static_cast<int>(state.packet_flows.size()); ++flow_index) {
        const PacketFlow& flow = state.packet_flows[static_cast<std::size_t>(flow_index)];
        auto found = std::find_if(state.packet_clients.begin(), state.packet_clients.end(), [&](const PacketClientTimeline& client) {
            return client.client == flow.client;
        });
        if (found != state.packet_clients.end()) {
            found->flow_indices.push_back(flow_index);
        }
    }
    std::sort(state.packet_clients.begin(), state.packet_clients.end(), [](const PacketClientTimeline& lhs, const PacketClientTimeline& rhs) {
        return lhs.client < rhs.client;
    });
    std::vector<PacketFrameMarker> server_frames;
    for (const KTraceSourceHistory& source : state.history.sources) {
        std::vector<PacketFrameMarker> markers = source_frame_markers(source);
        for (PacketFrameMarker& marker : markers) {
            marker.relative_us = marker.absolute_us >= state.packet_log_min_us
                ? marker.absolute_us - state.packet_log_min_us
                : 0U;
        }
        if (source.role == SyncTraceRole::Server) {
            server_frames.insert(
                server_frames.end(),
                std::make_move_iterator(markers.begin()),
                std::make_move_iterator(markers.end()));
            continue;
        }
        auto found = std::find_if(state.packet_clients.begin(), state.packet_clients.end(), [&](const PacketClientTimeline& client) {
            return client.client == source.client;
        });
        if (found != state.packet_clients.end()) {
            found->client_frames = std::move(markers);
        }
    }
    std::sort(server_frames.begin(), server_frames.end(), [](const PacketFrameMarker& lhs, const PacketFrameMarker& rhs) {
        if (lhs.absolute_us != rhs.absolute_us) {
            return lhs.absolute_us < rhs.absolute_us;
        }
        return lhs.frame < rhs.frame;
    });
    for (PacketClientTimeline& client : state.packet_clients) {
        client.server_frames = server_frames;
    }
    state.selected_packet_client = std::min(
        state.selected_packet_client,
        std::max(0, static_cast<int>(state.packet_clients.size()) - 1));
    assign_packet_lanes(state);
    state.packet_log_dirty = false;
}

void ensure_packet_log_rows(ViewerState& state) {
    if (state.packet_log_dirty) {
        rebuild_packet_log_rows(state);
        state.packet_details_dirty = true;
    }
}

const PacketEventInfo* selected_packet_event(const ViewerState& state) {
    if (state.selected_packet.source_index < 0 ||
        state.selected_packet.event_index == std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    for (const PacketEventInfo& event : state.packet_events) {
        if (event.source_index == state.selected_packet.source_index &&
            event.record_index == state.selected_packet.event_index) {
            return &event;
        }
    }
    return nullptr;
}

std::string entity_label(const KTraceEntityRow& entity) {
    std::ostringstream out;
    if (entity.local_entity) {
        out << "local " << entity.local_entity.value;
    }
    if (entity.server_entity) {
        if (entity.local_entity) {
            out << " ";
        }
        out << "server " << entity.server_entity.value;
    }
    return out.str();
}

std::string component_label(const KTraceSourceHistory& source, ashiato::Entity component) {
    if (!component) {
        return "entity lifetime";
    }
    if (is_cue_row_component(component)) {
        return "cues";
    }
    const auto found = source.component_names.find(component.value);
    if (found != source.component_names.end() && !found->second.empty()) {
        return found->second;
    }
    return "component " + std::to_string(component.value);
}

std::string cue_label(const KTraceSourceHistory& source, SyncCueTypeId cue_type) {
    const auto found = source.cue_names.find(cue_type);
    if (found != source.cue_names.end() && !found->second.empty()) {
        return found->second;
    }
    return "cue " + std::to_string(cue_type);
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool text_contains_case_insensitive(const std::string& haystack, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    return to_lower_copy(haystack).find(to_lower_copy(needle)) != std::string::npos;
}

std::string format_bits(std::uint64_t bits) {
    std::ostringstream out;
    out << bits << " bits";
    if (bits >= 8U && (bits % 8U) == 0U) {
        out << " (" << (bits / 8U) << " B)";
    }
    return out.str();
}

std::string format_compact_bits(std::uint64_t bits) {
    std::ostringstream out;
    if (bits > 1000U * 1000U) {
        out << std::fixed << std::setprecision(2) << (static_cast<double>(bits) / 1000000.0) << "mb";
    } else if (bits > 1000U) {
        out << std::fixed << std::setprecision(2) << (static_cast<double>(bits) / 1000.0) << "kb";
    } else {
        out << bits;
    }
    return out.str();
}

std::uint64_t bits_per_second(std::uint64_t bits, std::uint64_t duration_us) {
    if (bits == 0U || duration_us == 0U) {
        return 0U;
    }
    const double bps = (static_cast<double>(bits) * 1000000.0) / static_cast<double>(duration_us);
    if (bps >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::llround(bps));
}

std::string format_bps(std::uint64_t bps) {
    std::ostringstream out;
    if (bps >= 1000U * 1000U * 1000U) {
        out << std::fixed << std::setprecision(2) << (static_cast<double>(bps) / 1000000000.0) << "gbps";
    } else if (bps >= 1000U * 1000U) {
        out << std::fixed << std::setprecision(2) << (static_cast<double>(bps) / 1000000.0) << "mbps";
    } else if (bps >= 1000U) {
        out << std::fixed << std::setprecision(2) << (static_cast<double>(bps) / 1000.0) << "kbps";
    } else {
        out << bps << " bps";
    }
    return out.str();
}

std::string payload_summary_owner_label(SyncTracePayloadSource source, ClientId client) {
    return source == SyncTracePayloadSource::Replay
        ? "replay"
        : "client " + std::to_string(client);
}

const char* payload_source_filter_label(PayloadSourceFilter filter) {
    switch (filter) {
    case PayloadSourceFilter::Network: return "Network";
    case PayloadSourceFilter::Replay: return "Replay";
    case PayloadSourceFilter::All:
    default:
        return "All";
    }
}

const char* payload_source_label(SyncTracePayloadSource source) {
    switch (source) {
    case SyncTracePayloadSource::Replay: return "replay";
    case SyncTracePayloadSource::Network:
    default:
        return "network";
    }
}

const char* payload_direction_filter_label(PayloadDirectionFilter filter) {
    switch (filter) {
    case PayloadDirectionFilter::Outgoing: return "Server -> Client";
    case PayloadDirectionFilter::Incoming: return "Client -> Server";
    case PayloadDirectionFilter::All:
    default:
        return "All";
    }
}

const char* payload_direction_label(std::uint8_t tag_bits) {
    if ((tag_bits & sync_trace_payload_tag_outgoing) != 0U) {
        return "server -> client";
    }
    if ((tag_bits & sync_trace_payload_tag_incoming) != 0U) {
        return "client -> server";
    }
    return "unknown";
}

bool payload_direction_matches_filter(PayloadDirectionFilter filter, std::uint8_t tag_bits) {
    switch (filter) {
    case PayloadDirectionFilter::Outgoing:
        return (tag_bits & sync_trace_payload_tag_outgoing) != 0U;
    case PayloadDirectionFilter::Incoming:
        return (tag_bits & sync_trace_payload_tag_incoming) != 0U;
    case PayloadDirectionFilter::All:
    default:
        return true;
    }
}

bool payloads_enabled_in_trace(const ViewerState& state) {
    return std::any_of(state.history.sources.begin(), state.history.sources.end(), [](const KTraceSourceHistory& source) {
        return (source.flags & ktrace_flag_serialization_payloads) != 0U;
    });
}

std::uint64_t nice_payload_bucket_us(std::uint64_t duration_us) {
    if (duration_us == 0U) {
        return 100000U;
    }
    const std::uint64_t target = std::max<std::uint64_t>(1U, duration_us / 72U);
    std::uint64_t scale = 1U;
    while (scale * 10U < target && scale <= std::numeric_limits<std::uint64_t>::max() / 10U) {
        scale *= 10U;
    }
    for (std::uint64_t multiplier : {1ULL, 2ULL, 5ULL, 10ULL}) {
        const std::uint64_t candidate = scale * multiplier;
        if (candidate >= target) {
            return candidate;
        }
    }
    return scale * 10U;
}

int payload_scope_depth(const std::vector<SyncPayloadTraceScope>& scopes, std::uint32_t scope_index) {
    int depth = 0;
    std::uint32_t parent = scope_index < scopes.size() ? scopes[scope_index].parent : UINT32_MAX;
    while (parent != UINT32_MAX && parent < scopes.size() && depth < 64) {
        ++depth;
        parent = scopes[parent].parent;
    }
    return depth;
}

std::string payload_scope_path(
    const std::vector<SyncPayloadTraceScope>& source_scopes,
    const std::vector<PayloadScopeInfo>& scopes,
    std::uint32_t scope_index) {
    if (scope_index >= source_scopes.size()) {
        return {};
    }
    const std::uint32_t parent = source_scopes[scope_index].parent;
    const std::string name = source_scopes[scope_index].name.empty() ? "<unnamed>" : source_scopes[scope_index].name;
    if (parent == UINT32_MAX || parent >= scopes.size()) {
        return name;
    }
    return scopes[parent].path + "/" + name;
}

bool payload_path_descends_from(const std::string& path, const std::string& root) {
    if (root.empty()) {
        return true;
    }
    return path.size() > root.size() &&
        path.compare(0, root.size(), root) == 0 &&
        path[root.size()] == '/';
}

bool payload_path_matches_or_descends_from(const std::string& path, const std::string& root) {
    return root.empty() || path == root || payload_path_descends_from(path, root);
}

std::string payload_parent_path(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

int payload_path_depth(const std::string& path) {
    if (path.empty()) {
        return 0;
    }
    return static_cast<int>(std::count(path.begin(), path.end(), '/'));
}

std::uint64_t payload_scope_end_bits(const PayloadScopeInfo& scope) {
    if (scope.end_bits > scope.begin_bits) {
        return scope.end_bits;
    }
    if (scope.payload_bits > std::numeric_limits<std::uint64_t>::max() - scope.begin_bits) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return scope.begin_bits + scope.payload_bits;
}

std::uint64_t payload_scope_exclusive_bits(const PayloadPacketInfo& packet, int scope_index) {
    if (scope_index < 0 || scope_index >= static_cast<int>(packet.scopes.size())) {
        return 0;
    }
    const PayloadScopeInfo& scope = packet.scopes[static_cast<std::size_t>(scope_index)];
    std::uint64_t child_bits = 0;
    for (const PayloadScopeInfo& child : packet.scopes) {
        if (child.parent != static_cast<std::uint32_t>(scope_index)) {
            continue;
        }
        if (child.payload_bits > std::numeric_limits<std::uint64_t>::max() - child_bits) {
            child_bits = std::numeric_limits<std::uint64_t>::max();
            break;
        }
        child_bits += child.payload_bits;
    }
    return child_bits >= scope.payload_bits ? 0U : scope.payload_bits - child_bits;
}

void clear_selected_payload_scope(ViewerState& state) {
    state.selected_payload_scope_path.clear();
    state.selected_payload_scope_packet = -1;
    state.selected_payload_scope_id = UINT32_MAX;
}

void select_payload_scope(ViewerState& state, int packet_index, const PayloadScopeInfo& scope) {
    state.selected_payload_scope_path = scope.path;
    state.selected_payload_scope_packet = packet_index;
    state.selected_payload_scope_id = scope.id;
}

void select_payload_scope_path(ViewerState& state, int packet_index, const std::string& path) {
    state.selected_payload_scope_path = path;
    state.selected_payload_scope_packet = packet_index;
    state.selected_payload_scope_id = UINT32_MAX;
    if (packet_index < 0 || packet_index >= static_cast<int>(state.payload_packets.size())) {
        return;
    }
    const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(packet_index)];
    for (const PayloadScopeInfo& scope : packet.scopes) {
        if (scope.path == path) {
            state.selected_payload_scope_id = scope.id;
            break;
        }
    }
}

void refresh_selected_payload_scope_instance(ViewerState& state) {
    if (state.selected_payload_scope_path.empty()) {
        state.selected_payload_scope_packet = -1;
        state.selected_payload_scope_id = UINT32_MAX;
        return;
    }
    select_payload_scope_path(state, state.selected_payload_packet, state.selected_payload_scope_path);
}

bool payload_scope_selected(const ViewerState& state, int packet_index, const PayloadScopeInfo& scope) {
    if (state.selected_payload_scope_path != scope.path) {
        return false;
    }
    if (state.selected_payload_scope_id == UINT32_MAX || state.selected_payload_scope_packet < 0) {
        return true;
    }
    return state.selected_payload_scope_packet == packet_index && state.selected_payload_scope_id == scope.id;
}

bool payload_packet_matches_time_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    return !state.payload_time_filter_enabled ||
        (packet.absolute_us >= state.payload_filter_begin_us && packet.absolute_us < state.payload_filter_end_us);
}

bool payload_packet_matches_name_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    if (!state.payload_filter_enabled || state.payload_filter[0] == '\0') {
        return true;
    }
    if (text_contains_case_insensitive(packet.root_name, state.payload_filter.data())) {
        return true;
    }
    for (const PayloadScopeInfo& scope : packet.scopes) {
        if (text_contains_case_insensitive(scope.name, state.payload_filter.data()) ||
            text_contains_case_insensitive(scope.path, state.payload_filter.data())) {
            return true;
        }
    }
    return false;
}

bool payload_source_matches_filter(PayloadSourceFilter filter, SyncTracePayloadSource source) {
    switch (filter) {
    case PayloadSourceFilter::Network:
        return source == SyncTracePayloadSource::Network;
    case PayloadSourceFilter::Replay:
        return source == SyncTracePayloadSource::Replay;
    case PayloadSourceFilter::All:
    default:
        return true;
    }
}

bool payload_packet_matches_source_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    return payload_source_matches_filter(state.payload_source_filter, packet.payload_source);
}

bool payload_packet_matches_direction_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    return payload_direction_matches_filter(state.payload_direction_filter, packet.payload_tag_bits);
}

bool payload_client_excluded(const ViewerState& state, SyncTracePayloadSource source, ClientId client) {
    return state.payload_excluded_clients.find(PayloadClientKey{source, client}) != state.payload_excluded_clients.end();
}

bool payload_packet_matches_client_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    return !payload_client_excluded(state, packet.payload_source, packet.client);
}

bool payload_packet_matches_scope_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    if (state.selected_payload_scope_path.empty()) {
        return true;
    }
    for (const PayloadScopeInfo& scope : packet.scopes) {
        if (payload_path_matches_or_descends_from(scope.path, state.selected_payload_scope_path)) {
            return true;
        }
    }
    return false;
}

bool payload_packet_matches_chart_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    return payload_packet_matches_source_filter(state, packet) &&
        payload_packet_matches_direction_filter(state, packet) &&
        payload_packet_matches_time_filter(state, packet) &&
        payload_packet_matches_name_filter(state, packet) &&
        payload_packet_matches_scope_filter(state, packet);
}

bool payload_packet_matches_active_filter(const ViewerState& state, const PayloadPacketInfo& packet) {
    return payload_packet_matches_chart_filter(state, packet) &&
        payload_packet_matches_client_filter(state, packet);
}

std::uint64_t payload_filter_duration_us(const ViewerState& state) {
    if (state.payload_time_filter_enabled && state.payload_filter_end_us > state.payload_filter_begin_us) {
        return state.payload_filter_end_us - state.payload_filter_begin_us;
    }
    return state.payload_max_us >= state.payload_min_us
        ? state.payload_max_us - state.payload_min_us + 1U
        : 0U;
}

PayloadSummary payload_filtered_summary(const ViewerState& state) {
    PayloadSummary summary;
    summary.duration_us = payload_filter_duration_us(state);
    for (int packet_index : state.payload_filtered_packets) {
        if (packet_index < 0 || packet_index >= static_cast<int>(state.payload_packets.size())) {
            continue;
        }
        const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(packet_index)];
        summary.total_bits += packet.wire_bits;
        ++summary.packets;
    }
    return summary;
}

std::vector<PayloadClientSummary> payload_filtered_client_summaries(const ViewerState& state) {
    std::vector<PayloadClientSummary> summaries;
    for (int packet_index : state.payload_filtered_packets) {
        if (packet_index < 0 || packet_index >= static_cast<int>(state.payload_packets.size())) {
            continue;
        }
        const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(packet_index)];
        auto found = std::find_if(summaries.begin(), summaries.end(), [&](const PayloadClientSummary& summary) {
            return summary.payload_source == packet.payload_source && summary.client == packet.client;
        });
        if (found == summaries.end()) {
            PayloadClientSummary summary;
            summary.payload_source = packet.payload_source;
            summary.client = packet.client;
            summaries.push_back(summary);
            found = summaries.end() - 1;
        }
        found->total_bits += packet.wire_bits;
        ++found->packets;
    }
    std::sort(summaries.begin(), summaries.end(), [](const PayloadClientSummary& lhs, const PayloadClientSummary& rhs) {
        if (lhs.payload_source != rhs.payload_source) {
            return static_cast<unsigned>(lhs.payload_source) < static_cast<unsigned>(rhs.payload_source);
        }
        return lhs.client < rhs.client;
    });
    return summaries;
}

void rebuild_payload_filtered_packets(ViewerState& state) {
    state.payload_filtered_packets.clear();
    state.payload_filtered_packets.reserve(state.payload_packets.size());
    for (int index = 0; index < static_cast<int>(state.payload_packets.size()); ++index) {
        const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(index)];
        if (payload_packet_matches_active_filter(state, packet)) {
            state.payload_filtered_packets.push_back(index);
        }
    }
    std::sort(state.payload_filtered_packets.begin(), state.payload_filtered_packets.end(), [&](int lhs_index, int rhs_index) {
        const PayloadPacketInfo& lhs = state.payload_packets[static_cast<std::size_t>(lhs_index)];
        const PayloadPacketInfo& rhs = state.payload_packets[static_cast<std::size_t>(rhs_index)];
        if (state.payload_sort == PayloadSortMode::SizeDescending) {
            if (lhs.wire_bits != rhs.wire_bits) {
                return lhs.wire_bits > rhs.wire_bits;
            }
        }
        if (lhs.absolute_us != rhs.absolute_us) {
            return lhs.absolute_us < rhs.absolute_us;
        }
        return lhs.root_name < rhs.root_name;
    });
}

void rebuild_payload_aggregate_rows(ViewerState& state) {
    state.payload_rows.clear();
    std::unordered_map<std::string, int> row_by_path;
    const int selected_root_depth = payload_path_depth(state.selected_payload_scope_path);
    for (int packet_index = 0; packet_index < static_cast<int>(state.payload_packets.size()); ++packet_index) {
        const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(packet_index)];
        if (!payload_packet_matches_source_filter(state, packet) ||
            !payload_packet_matches_direction_filter(state, packet) ||
            !payload_packet_matches_time_filter(state, packet) ||
            !payload_packet_matches_client_filter(state, packet)) {
            continue;
        }
        for (int scope_index = 0; scope_index < static_cast<int>(packet.scopes.size()); ++scope_index) {
            const PayloadScopeInfo& scope = packet.scopes[static_cast<std::size_t>(scope_index)];
            const bool in_hierarchy_root = state.payload_hierarchy_view
                ? payload_path_matches_or_descends_from(scope.path, state.selected_payload_scope_path)
                : true;
            if (!in_hierarchy_root) {
                continue;
            }
            if (state.payload_filter_enabled &&
                state.payload_filter[0] != '\0' &&
                !text_contains_case_insensitive(scope.name, state.payload_filter.data()) &&
                !text_contains_case_insensitive(scope.path, state.payload_filter.data())) {
                continue;
            }
            auto found = row_by_path.find(scope.path);
            if (found == row_by_path.end()) {
                PayloadAggregateRow row;
                row.path = scope.path;
                row.name = scope.name;
                row.depth = state.payload_hierarchy_view ? std::max(0, scope.depth - selected_root_depth) : 0;
                row_by_path[row.path] = static_cast<int>(state.payload_rows.size());
                state.payload_rows.push_back(std::move(row));
                found = row_by_path.find(scope.path);
            }
            PayloadAggregateRow& row = state.payload_rows[static_cast<std::size_t>(found->second)];
            const std::uint64_t exclusive_bits = payload_scope_exclusive_bits(packet, scope_index);
            ++row.count;
            row.total_bits += scope.payload_bits;
            row.total_exclusive_bits += exclusive_bits;
            if (scope.payload_bits >= row.max_bits) {
                row.max_bits = scope.payload_bits;
                row.max_packet_index = packet_index;
            }
            row.max_exclusive_bits = std::max(row.max_exclusive_bits, exclusive_bits);
        }
    }
    if (state.payload_hierarchy_view) {
        std::sort(state.payload_rows.begin(), state.payload_rows.end(), [](const PayloadAggregateRow& lhs, const PayloadAggregateRow& rhs) {
            return lhs.path < rhs.path;
        });
    }
    state.selected_payload_row = state.payload_rows.empty()
        ? -1
        : std::clamp(state.selected_payload_row, 0, static_cast<int>(state.payload_rows.size()) - 1);
}

std::uint64_t payload_row_average_bits(const PayloadAggregateRow& row) {
    return row.count == 0U ? 0U : row.total_bits / row.count;
}

std::uint64_t payload_row_average_exclusive_bits(const PayloadAggregateRow& row) {
    return row.count == 0U ? 0U : row.total_exclusive_bits / row.count;
}

int compare_payload_rows_by_sort_column(
    const PayloadAggregateRow& lhs,
    const PayloadAggregateRow& rhs,
    std::uint64_t duration_us,
    PayloadAggregateSortColumn column) {
    auto compare_u64 = [](std::uint64_t lhs_value, std::uint64_t rhs_value) {
        if (lhs_value < rhs_value) {
            return -1;
        }
        if (lhs_value > rhs_value) {
            return 1;
        }
        return 0;
    };
    switch (column) {
    case PayloadAggregateSortColumn::Count:
        return compare_u64(lhs.count, rhs.count);
    case PayloadAggregateSortColumn::TotalBits:
        return compare_u64(lhs.total_bits, rhs.total_bits);
    case PayloadAggregateSortColumn::BandwidthBps:
        return compare_u64(
            bits_per_second(lhs.total_bits, duration_us),
            bits_per_second(rhs.total_bits, duration_us));
    case PayloadAggregateSortColumn::AvgBits:
        return compare_u64(payload_row_average_bits(lhs), payload_row_average_bits(rhs));
    case PayloadAggregateSortColumn::MaxBits:
        return compare_u64(lhs.max_bits, rhs.max_bits);
    case PayloadAggregateSortColumn::TotalExclusiveBits:
        return compare_u64(lhs.total_exclusive_bits, rhs.total_exclusive_bits);
    case PayloadAggregateSortColumn::AvgExclusiveBits:
        return compare_u64(payload_row_average_exclusive_bits(lhs), payload_row_average_exclusive_bits(rhs));
    case PayloadAggregateSortColumn::Name:
    default:
        return lhs.path.compare(rhs.path);
    }
}

void sort_payload_aggregate_rows(ViewerState& state) {
    const std::uint64_t duration_us = payload_filter_duration_us(state);
    if (state.payload_hierarchy_view) {
        struct RowRange {
            int begin = 0;
            int end = 0;
        };
        auto compare_rows = [&](const PayloadAggregateRow& lhs, const PayloadAggregateRow& rhs) {
            int comparison = compare_payload_rows_by_sort_column(lhs, rhs, duration_us, state.payload_row_sort_column);
            if (comparison == 0 && state.payload_row_sort_column != PayloadAggregateSortColumn::Name) {
                comparison = lhs.name.compare(rhs.name);
            }
            if (comparison == 0) {
                comparison = lhs.path.compare(rhs.path);
            }
            return state.payload_row_sort_ascending ? comparison < 0 : comparison > 0;
        };
        auto sort_range = [&](auto&& self, int begin, int end, int depth) -> void {
            std::vector<RowRange> siblings;
            for (int index = begin; index < end;) {
                const int row_depth = state.payload_rows[static_cast<std::size_t>(index)].depth;
                if (row_depth < depth) {
                    ++index;
                    continue;
                }
                if (row_depth > depth) {
                    const int nested_begin = index;
                    do {
                        ++index;
                    } while (index < end && state.payload_rows[static_cast<std::size_t>(index)].depth >= row_depth);
                    self(self, nested_begin, index, row_depth);
                    continue;
                }
                int next = index + 1;
                while (next < end && state.payload_rows[static_cast<std::size_t>(next)].depth > row_depth) {
                    ++next;
                }
                self(self, index + 1, next, row_depth + 1);
                siblings.push_back(RowRange{index, next});
                index = next;
            }
            std::stable_sort(siblings.begin(), siblings.end(), [&](const RowRange& lhs, const RowRange& rhs) {
                return compare_rows(
                    state.payload_rows[static_cast<std::size_t>(lhs.begin)],
                    state.payload_rows[static_cast<std::size_t>(rhs.begin)]);
            });
            std::vector<PayloadAggregateRow> sorted;
            sorted.reserve(static_cast<std::size_t>(end - begin));
            for (const RowRange& sibling : siblings) {
                for (int index = sibling.begin; index < sibling.end; ++index) {
                    sorted.push_back(std::move(state.payload_rows[static_cast<std::size_t>(index)]));
                }
            }
            if (static_cast<int>(sorted.size()) == end - begin) {
                std::move(sorted.begin(), sorted.end(), state.payload_rows.begin() + begin);
            }
        };
        const auto min_depth = std::min_element(
            state.payload_rows.begin(),
            state.payload_rows.end(),
            [](const PayloadAggregateRow& lhs, const PayloadAggregateRow& rhs) {
                return lhs.depth < rhs.depth;
            });
        if (min_depth != state.payload_rows.end()) {
            sort_range(
                sort_range,
                0,
                static_cast<int>(state.payload_rows.size()),
                min_depth->depth);
        }
        return;
    }
    std::sort(state.payload_rows.begin(), state.payload_rows.end(), [&](const PayloadAggregateRow& lhs, const PayloadAggregateRow& rhs) {
        int comparison = compare_payload_rows_by_sort_column(lhs, rhs, duration_us, state.payload_row_sort_column);
        if (comparison == 0 && state.payload_row_sort_column != PayloadAggregateSortColumn::Name) {
            comparison = lhs.name.compare(rhs.name);
        }
        if (comparison == 0) {
            comparison = lhs.path.compare(rhs.path);
        }
        return state.payload_row_sort_ascending ? comparison < 0 : comparison > 0;
    });
}

void sync_selected_payload_row(ViewerState& state) {
    state.selected_payload_row = -1;
    if (state.selected_payload_scope_path.empty()) {
        return;
    }
    for (int index = 0; index < static_cast<int>(state.payload_rows.size()); ++index) {
        if (state.payload_rows[static_cast<std::size_t>(index)].path == state.selected_payload_scope_path) {
            state.selected_payload_row = index;
            return;
        }
    }
}

void select_payload_scope_hierarchy_item(
    ViewerState& state,
    int packet_index,
    const std::string& scope_path,
    int row_index = -1) {
    state.selected_payload_row = row_index;
    state.selected_payload_packet = packet_index;
    const bool entered_hierarchy_from_flat = !state.payload_hierarchy_view;
    select_payload_scope_path(state, packet_index, scope_path);
    state.payload_hierarchy_view = true;
    state.payload_return_to_flat_on_back_to_top = entered_hierarchy_from_flat;
    if (row_index < 0) {
        sync_selected_payload_row(state);
    }
    state.payload_view_dirty = true;
}

void select_payload_scope_hierarchy_item(
    ViewerState& state,
    int packet_index,
    const PayloadScopeInfo& scope) {
    state.selected_payload_row = -1;
    state.selected_payload_packet = packet_index;
    const bool entered_hierarchy_from_flat = !state.payload_hierarchy_view;
    select_payload_scope(state, packet_index, scope);
    state.payload_hierarchy_view = true;
    state.payload_return_to_flat_on_back_to_top = entered_hierarchy_from_flat;
    sync_selected_payload_row(state);
    state.payload_view_dirty = true;
}

void rebuild_payload_bandwidth_buckets(ViewerState& state) {
    state.payload_buckets.clear();
    std::unordered_map<std::string, int> bucket_by_key;
    for (const PayloadPacketInfo& packet : state.payload_packets) {
        if (!payload_packet_matches_chart_filter(state, packet)) {
            continue;
        }
        const std::uint64_t relative_us = packet.absolute_us >= state.payload_min_us ? packet.absolute_us - state.payload_min_us : 0U;
        const std::uint64_t bucket_offset = state.payload_bucket_us == 0U ? 0U : (relative_us / state.payload_bucket_us) * state.payload_bucket_us;
        const std::uint64_t begin_us = state.payload_min_us + bucket_offset;
        const std::string key = std::to_string(static_cast<unsigned>(packet.payload_source)) + "|" +
            std::to_string(packet.client) + "|" + std::to_string(packet.payload_tag_bits) + "|" + std::to_string(begin_us);
        auto found = bucket_by_key.find(key);
        if (found == bucket_by_key.end()) {
            PayloadBandwidthBucket bucket;
            bucket.client = packet.client;
            bucket.payload_source = packet.payload_source;
            bucket.payload_tag_bits = packet.payload_tag_bits;
            bucket.begin_us = begin_us;
            bucket.end_us = begin_us + state.payload_bucket_us;
            bucket_by_key[key] = static_cast<int>(state.payload_buckets.size());
            state.payload_buckets.push_back(bucket);
            found = bucket_by_key.find(key);
        }
        PayloadBandwidthBucket& bucket = state.payload_buckets[static_cast<std::size_t>(found->second)];
        bucket.total_bits += packet.wire_bits;
        ++bucket.packets;
    }
    std::sort(state.payload_buckets.begin(), state.payload_buckets.end(), [](const PayloadBandwidthBucket& lhs, const PayloadBandwidthBucket& rhs) {
        if (lhs.payload_source != rhs.payload_source) {
            return static_cast<unsigned>(lhs.payload_source) < static_cast<unsigned>(rhs.payload_source);
        }
        if (lhs.client != rhs.client) {
            return lhs.client < rhs.client;
        }
        if (lhs.payload_tag_bits != rhs.payload_tag_bits) {
            return lhs.payload_tag_bits < rhs.payload_tag_bits;
        }
        return lhs.begin_us < rhs.begin_us;
    });
}

void rebuild_payload_rows(ViewerState& state) {
    state.payload_packets.clear();
    state.payload_rows.clear();
    state.payload_buckets.clear();
    state.payload_min_us = 0;
    state.payload_max_us = 0;
    bool initialized = false;
    for (int source_index = 0; source_index < static_cast<int>(state.history.sources.size()); ++source_index) {
        const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
        const std::uint64_t source_start_us = source_recorded_us(source);
        for (std::uint32_t record_index = 0; record_index < source.records.size(); ++record_index) {
            const KTraceRecord& record = source.records[record_index];
            const SyncTraceEvent& trace = record.event;
            if (trace.type != SyncTraceEventType::SerializationPayload ||
                trace.payload_scopes.empty()) {
                continue;
            }
            const std::uint64_t wire_bits = trace.wire_bits != 0U ? trace.wire_bits : trace.payload_scopes.front().payload_bits;
            if (wire_bits == 0U) {
                continue;
            }
            const PacketKeyValues fields = parse_packet_data(trace.data);
            const ClientId client = parse_packet_client(fields, source, trace);
            if (trace.payload_source == SyncTracePayloadSource::Network && client == invalid_client_id) {
                continue;
            }
            PayloadPacketInfo packet;
            packet.source_index = source_index;
            packet.record_index = record_index;
            packet.frame = trace.frame;
            packet.client = client;
            packet.network_id = trace_event_network_key(trace);
            packet.component = trace.component;
            packet.server_entity = trace.server_entity;
            packet.payload_source = trace.payload_source;
            packet.payload_tag_bits = trace.payload_tag_bits;
            packet.source = source_label(source);
            packet.root_name = trace.payload_scopes.front().name.empty() ? "packet" : trace.payload_scopes.front().name;
            packet.data = trace.data;
            packet.wire_bits = wire_bits;
            packet.absolute_us = source_start_us + record.timestamp_us;
            packet.scopes.reserve(trace.payload_scopes.size());
            for (std::size_t scope_index = 0; scope_index < trace.payload_scopes.size(); ++scope_index) {
                const SyncPayloadTraceScope& source_scope = trace.payload_scopes[scope_index];
                PayloadScopeInfo scope;
                scope.id = source_scope.id;
                scope.parent = source_scope.parent;
                scope.depth = payload_scope_depth(trace.payload_scopes, static_cast<std::uint32_t>(scope_index));
                scope.name = source_scope.name.empty() ? "<unnamed>" : source_scope.name;
                scope.path = payload_scope_path(trace.payload_scopes, packet.scopes, static_cast<std::uint32_t>(scope_index));
                scope.begin_bits = source_scope.begin_bits;
                scope.end_bits = source_scope.end_bits;
                scope.payload_bits = source_scope.payload_bits;
                packet.scopes.push_back(std::move(scope));
            }
            if (!initialized) {
                state.payload_min_us = packet.absolute_us;
                state.payload_max_us = packet.absolute_us;
                initialized = true;
            } else {
                state.payload_min_us = std::min(state.payload_min_us, packet.absolute_us);
                state.payload_max_us = std::max(state.payload_max_us, packet.absolute_us);
            }
            state.payload_packets.push_back(std::move(packet));
        }
    }
    const std::uint64_t duration_us = state.payload_max_us >= state.payload_min_us
        ? state.payload_max_us - state.payload_min_us + 1U
        : 0U;
    state.payload_bucket_us = nice_payload_bucket_us(duration_us);
    if (state.payload_time_filter_enabled && state.payload_filter_end_us <= state.payload_filter_begin_us) {
        state.payload_time_filter_enabled = false;
    }
    state.selected_payload_packet = state.payload_packets.empty()
        ? -1
        : std::clamp(state.selected_payload_packet, 0, static_cast<int>(state.payload_packets.size()) - 1);
    if (state.payload_packets.empty()) {
        clear_selected_payload_scope(state);
    }
    state.payload_view_dirty = true;
}

void ensure_payload_rows(ViewerState& state) {
    if (state.payload_dirty) {
        rebuild_payload_rows(state);
        state.payload_dirty = false;
    }
}

void rebuild_payload_view(ViewerState& state) {
    rebuild_payload_filtered_packets(state);
    rebuild_payload_aggregate_rows(state);
    sort_payload_aggregate_rows(state);
    sync_selected_payload_row(state);
    rebuild_payload_bandwidth_buckets(state);
    if (!state.payload_filtered_packets.empty() &&
        std::find(
            state.payload_filtered_packets.begin(),
            state.payload_filtered_packets.end(),
            state.selected_payload_packet) == state.payload_filtered_packets.end()) {
        state.selected_payload_packet = state.payload_filtered_packets.front();
    }
    refresh_selected_payload_scope_instance(state);
    state.payload_view_dirty = false;
}

void ensure_payload_view(ViewerState& state) {
    ensure_payload_rows(state);
    if (state.payload_view_dirty) {
        rebuild_payload_view(state);
    }
}

bool packet_input_range_contains(const std::string& range, SyncFrame frame) {
    if (range.empty() || range == "none") {
        return false;
    }
    const std::size_t dash = range.find('-');
    try {
        if (dash == std::string::npos) {
            return static_cast<SyncFrame>(std::stoul(range)) == frame;
        }
        const SyncFrame first = static_cast<SyncFrame>(std::stoul(range.substr(0, dash)));
        const SyncFrame last = static_cast<SyncFrame>(std::stoul(range.substr(dash + 1U)));
        return first <= frame && frame <= last;
    } catch (...) {
        return false;
    }
}

SyncFrame packet_input_range_first_frame(const std::string& range, SyncFrame fallback) {
    if (range.empty() || range == "none") {
        return fallback;
    }
    const std::size_t dash = range.find('-');
    try {
        return static_cast<SyncFrame>(std::stoul(dash == std::string::npos ? range : range.substr(0, dash)));
    } catch (...) {
        return fallback;
    }
}

SyncFrame packet_event_primary_payload_frame(const PacketEventInfo& event) {
    if (event.message == "server_update" && !event.server_frame.empty()) {
        try {
            return static_cast<SyncFrame>(std::stoul(event.server_frame));
        } catch (...) {
            return event.frame;
        }
    }
    if (event.message == "client_input") {
        return packet_input_range_first_frame(event.input_frames, event.frame);
    }
    return event.frame;
}

bool packet_event_contains_frame(const PacketEventInfo& event, SyncFrame frame) {
    if (event.message == "server_update") {
        if (!event.server_frame.empty()) {
            try {
                return static_cast<SyncFrame>(std::stoul(event.server_frame)) == frame;
            } catch (...) {
                return event.frame == frame;
            }
        }
        return event.frame == frame;
    }
    if (event.message == "client_input") {
        return packet_input_range_contains(event.input_frames, frame);
    }
    return event.frame == frame;
}

std::string payload_message(const PayloadPacketInfo& packet) {
    const PacketKeyValues fields = parse_packet_data(packet.data);
    return packet_value(fields, "message");
}

bool payload_matches_frame_selection(const PayloadPacketInfo& packet, const SelectedCell& selected) {
    if (packet.source_index != selected.source_index || packet.frame != selected.frame) {
        return false;
    }
    if (selected.network_id != invalid_client_entity_network_id &&
        packet.network_id != invalid_client_entity_network_id &&
        packet.network_id != selected.network_id) {
        return false;
    }
    return !selected.component || !packet.component || packet.component == selected.component;
}

int find_payload_packet_for_selected_frame(ViewerState& state) {
    ensure_payload_rows(state);
    int best_index = -1;
    int best_score = std::numeric_limits<int>::max();
    for (int index = 0; index < static_cast<int>(state.payload_packets.size()); ++index) {
        const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(index)];
        if (!payload_matches_frame_selection(packet, state.selected)) {
            continue;
        }
        int score = 0;
        if (state.selected.component && packet.component != state.selected.component) {
            score += 10;
        }
        const std::string message = payload_message(packet);
        if (message == "server_update_record" || message == "client_input") {
            score -= 1;
        }
        if (score < best_score) {
            best_score = score;
            best_index = index;
        }
    }
    return best_index;
}

bool packet_event_matches_payload(const PacketEventInfo& event, const PayloadPacketInfo& packet) {
    if (event.source_index != packet.source_index || event.client != packet.client) {
        return false;
    }
    const std::string message = payload_message(packet);
    if (message == "server_update_record") {
        return event.message == "server_update" && packet_event_contains_frame(event, packet.frame);
    }
    if (message == "client_input") {
        return event.message == "client_input" && packet_event_contains_frame(event, packet.frame);
    }
    if (message == "client_ack") {
        return event.message == "client_ack" && event.frame == packet.frame;
    }
    return event.message == message && packet_event_contains_frame(event, packet.frame);
}

int find_packet_event_for_payload_packet(ViewerState& state, const PayloadPacketInfo& packet) {
    ensure_packet_log_rows(state);
    int best_index = -1;
    std::uint64_t best_delta = std::numeric_limits<std::uint64_t>::max();
    for (int index = 0; index < static_cast<int>(state.packet_events.size()); ++index) {
        const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(index)];
        if (!packet_event_matches_payload(event, packet)) {
            continue;
        }
        const std::uint64_t delta = event.absolute_us >= packet.absolute_us
            ? event.absolute_us - packet.absolute_us
            : packet.absolute_us - event.absolute_us;
        if (best_index < 0 || delta < best_delta) {
            best_index = index;
            best_delta = delta;
        }
    }
    return best_index;
}

int find_packet_event_for_selected_frame(ViewerState& state) {
    const int payload_index = find_payload_packet_for_selected_frame(state);
    if (payload_index >= 0) {
        const int event_index = find_packet_event_for_payload_packet(
            state,
            state.payload_packets[static_cast<std::size_t>(payload_index)]);
        if (event_index >= 0) {
            return event_index;
        }
    }
    ensure_packet_log_rows(state);
    int best_index = -1;
    for (int index = 0; index < static_cast<int>(state.packet_events.size()); ++index) {
        const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(index)];
        if (event.source_index == state.selected.source_index && packet_event_contains_frame(event, state.selected.frame)) {
            best_index = index;
            break;
        }
    }
    return best_index;
}

bool payload_matches_packet_event(const PayloadPacketInfo& packet, const PacketEventInfo& event) {
    if (packet.source_index != event.source_index || packet.client != event.client) {
        return false;
    }
    const std::string message = payload_message(packet);
    if (event.message == "server_update") {
        return message == "server_update_record" && packet_event_contains_frame(event, packet.frame);
    }
    if (event.message == "client_input") {
        return message == "client_input" && packet_event_contains_frame(event, packet.frame);
    }
    return message == event.message && packet_event_contains_frame(event, packet.frame);
}

const PacketEventInfo* packet_navigation_event(const ViewerState& state) {
    const PacketEventInfo* event = selected_packet_event(state);
    if (event == nullptr || event->send ||
        event->flow_index < 0 ||
        event->flow_index >= static_cast<int>(state.packet_flows.size())) {
        return event;
    }
    const PacketFlow& flow = state.packet_flows[static_cast<std::size_t>(event->flow_index)];
    if (flow.send_event < 0 || flow.send_event >= static_cast<int>(state.packet_events.size())) {
        return event;
    }
    return &state.packet_events[static_cast<std::size_t>(flow.send_event)];
}

int find_payload_packet_for_packet_event(ViewerState& state, const PacketEventInfo& event) {
    ensure_payload_rows(state);
    int best_index = -1;
    std::uint64_t best_delta = std::numeric_limits<std::uint64_t>::max();
    for (int index = 0; index < static_cast<int>(state.payload_packets.size()); ++index) {
        const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(index)];
        if (!payload_matches_packet_event(packet, event)) {
            continue;
        }
        const std::uint64_t delta = event.absolute_us >= packet.absolute_us
            ? event.absolute_us - packet.absolute_us
            : packet.absolute_us - event.absolute_us;
        if (best_index < 0 || delta < best_delta) {
            best_index = index;
            best_delta = delta;
        }
    }
    return best_index;
}

bool select_frame_cell(ViewerState& state, int source_index, SyncFrame frame, ClientEntityNetworkId network_id, ashiato::Entity component) {
    if (source_index < 0 || source_index >= static_cast<int>(state.history.sources.size())) {
        return false;
    }
    const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
    for (const KTraceEntityRow& entity : source.entities) {
        if (network_id != invalid_client_entity_network_id && entity.client_network_id != network_id) {
            continue;
        }
        for (const KTraceComponentRow& row : entity.components) {
            if (component && row.component != component) {
                continue;
            }
            for (KTraceRunId run_id = 0; run_id < row.runs.size(); ++run_id) {
                const KTraceFrameRun& run = row.runs[static_cast<std::size_t>(run_id)];
                const auto found = std::find_if(run.frames.begin(), run.frames.end(), [frame](const KTraceFrameCell& cell) {
                    return cell.frame == frame;
                });
                if (found != run.frames.end()) {
                    state.expanded_entities.insert(EntityExpansionKey{source_index, entity.client_network_id});
                    state.expanded_components.insert(ComponentExpansionKey{source_index, entity.client_network_id, row.component});
                    update_source_metrics(state, source_index);
                    move_selection_to(state, SelectedCell{source_index, entity.client_network_id, row.component, found->frame, run_id, found->event_indices});
                    state.selected_source = source_index;
                    state.selected_source_dirty = true;
                    state.mode = ViewerMode::Frames;
                    state.scroll_selected_frame_into_view = true;
                    return true;
                }
            }
        }
    }
    return false;
}

void select_payload_packet(ViewerState& state, int packet_index) {
    if (packet_index < 0 || packet_index >= static_cast<int>(state.payload_packets.size())) {
        return;
    }
    state.selected_payload_packet = packet_index;
    state.payload_source_filter = PayloadSourceFilter::All;
    state.payload_direction_filter = PayloadDirectionFilter::All;
    state.payload_excluded_clients.clear();
    state.payload_filter_enabled = false;
    state.payload_filter[0] = '\0';
    state.payload_time_filter_enabled = false;
    clear_selected_payload_scope(state);
    state.payload_view_dirty = true;
    state.mode = ViewerMode::Payloads;
}

void select_packet_event_by_index(ViewerState& state, int event_index) {
    if (event_index < 0 || event_index >= static_cast<int>(state.packet_events.size())) {
        return;
    }
    const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
    select_packet_event(state, event);
    for (int client_index = 0; client_index < static_cast<int>(state.packet_clients.size()); ++client_index) {
        if (state.packet_clients[static_cast<std::size_t>(client_index)].client == event.client) {
            state.selected_packet_client = client_index;
            break;
        }
    }
    state.mode = ViewerMode::EventLog;
    state.scroll_selected_packet_into_view = true;
}

void jump_selected_frame_to_event_log(ViewerState& state) {
    const int event_index = find_packet_event_for_selected_frame(state);
    if (event_index >= 0) {
        select_packet_event_by_index(state, event_index);
    } else {
        state.status = "no packet log record matched the selected frame";
        state.status_is_error = false;
    }
}

void jump_selected_frame_to_payload(ViewerState& state) {
    const int packet_index = find_payload_packet_for_selected_frame(state);
    if (packet_index >= 0) {
        select_payload_packet(state, packet_index);
    } else {
        state.status = "no bandwidth payload matched the selected frame";
        state.status_is_error = false;
    }
}

void jump_selected_packet_to_frame(ViewerState& state) {
    const PacketEventInfo* event = packet_navigation_event(state);
    if (event == nullptr ||
        !select_frame_cell(state, event->source_index, packet_event_primary_payload_frame(*event), invalid_client_entity_network_id, ashiato::Entity{})) {
        state.status = "no frame row matched the selected packet event";
        state.status_is_error = false;
    }
}

void jump_selected_packet_to_payload(ViewerState& state) {
    const PacketEventInfo* event = packet_navigation_event(state);
    const int packet_index = event != nullptr ? find_payload_packet_for_packet_event(state, *event) : -1;
    if (packet_index >= 0) {
        select_payload_packet(state, packet_index);
    } else {
        state.status = "no bandwidth payload matched the selected packet event";
        state.status_is_error = false;
    }
}

void jump_selected_payload_to_frame(ViewerState& state) {
    ensure_payload_rows(state);
    if (state.selected_payload_packet < 0 ||
        state.selected_payload_packet >= static_cast<int>(state.payload_packets.size())) {
        return;
    }
    const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(state.selected_payload_packet)];
    if (!select_frame_cell(state, packet.source_index, packet.frame, packet.network_id, packet.component)) {
        state.status = "no frame row matched the selected bandwidth payload";
        state.status_is_error = false;
    }
}

void jump_selected_payload_to_event_log(ViewerState& state) {
    ensure_payload_rows(state);
    if (state.selected_payload_packet < 0 ||
        state.selected_payload_packet >= static_cast<int>(state.payload_packets.size())) {
        return;
    }
    const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(state.selected_payload_packet)];
    const int event_index = find_packet_event_for_payload_packet(state, packet);
    if (event_index >= 0) {
        select_packet_event_by_index(state, event_index);
    } else {
        state.status = "no packet log record matched the selected bandwidth payload";
        state.status_is_error = false;
    }
}

CellVisual visual_for_cell(const KTraceFrameCell& cell, SyncTraceRole role) {
    if (has_state(cell, KTraceCellState::Removed) || has_state(cell, KTraceCellState::EntityDestroyed)) {
        return CellVisual{CellVisualKind::Removed, IM_COL32(112, 119, 132, 255), IM_COL32(230, 236, 245, 190), "removed"};
    }
    if (has_state(cell, KTraceCellState::CueRolledBack)) {
        return CellVisual{CellVisualKind::Cue, IM_COL32(190, 62, 78, 255), IM_COL32(255, 214, 220, 220), "cue rolled back"};
    }
    if (has_state(cell, KTraceCellState::CueConfirmed)) {
        return CellVisual{CellVisualKind::Cue, IM_COL32(54, 172, 118, 255), IM_COL32(199, 255, 226, 210), "cue confirmed"};
    }
    if (has_state(cell, KTraceCellState::CuePlayed)) {
        return CellVisual{CellVisualKind::Cue, IM_COL32(40, 159, 178, 255), IM_COL32(189, 247, 255, 190), "cue played"};
    }
    if (has_state(cell, KTraceCellState::CueReceived)) {
        return CellVisual{CellVisualKind::Cue, IM_COL32(66, 125, 218, 255), IM_COL32(196, 222, 255, 180), "cue received"};
    }
    if (has_state(cell, KTraceCellState::CueSent)) {
        return CellVisual{CellVisualKind::Cue, IM_COL32(117, 101, 222, 255), IM_COL32(218, 209, 255, 180), "cue sent"};
    }
    if (has_state(cell, KTraceCellState::CueEmitted)) {
        return CellVisual{CellVisualKind::Cue, IM_COL32(202, 126, 46, 255), IM_COL32(255, 224, 184, 190), "cue emitted"};
    }
    if (has_state(cell, KTraceCellState::InputReceived)) {
        return CellVisual{
            CellVisualKind::InputReceived,
            IM_COL32(53, 132, 246, 255),
            IM_COL32(176, 216, 255, 170),
            has_state(cell, KTraceCellState::Starved) ? "input starved" : "input received"};
    }
    if (has_state(cell, KTraceCellState::Starved)) {
        return CellVisual{CellVisualKind::Starved, IM_COL32(134, 31, 42, 255), IM_COL32(255, 126, 136, 220), "starved"};
    }
    if (has_state(cell, KTraceCellState::Mispredicted)) {
        return CellVisual{CellVisualKind::Mispredicted, IM_COL32(218, 56, 67, 255), IM_COL32(255, 213, 218, 230), "mispredicted"};
    }
    if (has_state(cell, KTraceCellState::PredictedCorrect)) {
        return CellVisual{CellVisualKind::CorrectPrediction, IM_COL32(51, 177, 103, 255), IM_COL32(190, 255, 213, 180), "correct prediction"};
    }
    if (has_state(cell, KTraceCellState::Resimulated)) {
        return CellVisual{CellVisualKind::Resimulated, IM_COL32(231, 151, 49, 255), IM_COL32(255, 224, 150, 210), "resimulated"};
    }
    if (has_state(cell, KTraceCellState::LocalPredicted)) {
        return CellVisual{CellVisualKind::Predicted, IM_COL32(53, 132, 246, 255), IM_COL32(176, 216, 255, 170), "predicted"};
    }
    if (has_state(cell, KTraceCellState::LocalInterpolated)) {
        return CellVisual{CellVisualKind::Interpolated, IM_COL32(152, 89, 217, 255), IM_COL32(222, 190, 255, 180), "interpolated"};
    }
    if (role == SyncTraceRole::Server || has_state(cell, KTraceCellState::ReceivedFromServer) ||
        has_state(cell, KTraceCellState::SentToClient)) {
        return CellVisual{CellVisualKind::ServerGolden, IM_COL32(46, 160, 91, 255), IM_COL32(174, 239, 196, 170), "server/golden"};
    }
    if (has_state(cell, KTraceCellState::Applied)) {
        return CellVisual{CellVisualKind::Applied, IM_COL32(45, 166, 179, 255), IM_COL32(180, 244, 250, 170), "applied"};
    }
    return CellVisual{CellVisualKind::Unknown, IM_COL32(100, 108, 122, 255), IM_COL32(210, 218, 232, 120), "unknown"};
}

int cue_count_for_cell(const KTraceSourceHistory& source, const KTraceFrameCell& cell) {
    struct CueLifecycleCounts {
        std::array<int, 6> counts{};
    };
    std::unordered_map<std::string, CueLifecycleCounts> cue_counts;
    for (std::uint32_t event_index : cell.event_indices) {
        if (event_index >= source.records.size()) {
            continue;
        }
        const SyncTraceEvent& event = source.records[event_index].event;
        const SyncTraceEventType type = event.type;
        if (!is_cue_event(type)) {
            continue;
        }
        ++cue_counts[cue_instance_key(event)].counts[cue_lifecycle_slot(type)];
    }
    int count = 0;
    for (const auto& item : cue_counts) {
        const int primary_count = std::max({
            item.second.counts[cue_lifecycle_slot(SyncTraceEventType::CueEmitted)],
            item.second.counts[cue_lifecycle_slot(SyncTraceEventType::CuePlayed)],
            item.second.counts[cue_lifecycle_slot(SyncTraceEventType::CueRolledBack)]});
        count += primary_count != 0 ? primary_count : 1;
    }
    return count;
}

void draw_starved_mark(ImDrawList* draw, const ImVec2& min, const ImVec2&, ImU32 color) {
    draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(min.x + 5.0f, min.y + 5.0f), color);
    draw->AddRectFilled(ImVec2(min.x + 8.0f, min.y + 2.0f), ImVec2(min.x + 11.0f, min.y + 5.0f), color);
    draw->AddRectFilled(ImVec2(min.x + 5.0f, min.y + 8.0f), ImVec2(min.x + 8.0f, min.y + 11.0f), color);
    draw->AddRectFilled(ImVec2(min.x + 11.0f, min.y + 8.0f), ImVec2(min.x + 14.0f, min.y + 11.0f), color);
}

void draw_cell_marks(ImDrawList* draw, const ImVec2& min, const ImVec2& max, const CellVisual& visual) {
    switch (visual.kind) {
    case CellVisualKind::Predicted:
        draw->AddRectFilled(min, ImVec2(max.x, min.y + 3.0f), visual.accent);
        break;
    case CellVisualKind::Interpolated:
        draw->AddRectFilled(ImVec2(min.x, max.y - 3.0f), max, visual.accent);
        break;
    case CellVisualKind::CorrectPrediction:
        draw->AddLine(ImVec2(min.x + 3.0f, min.y + 7.0f), ImVec2(min.x + 6.0f, max.y - 3.0f), visual.accent, 1.5f);
        draw->AddLine(ImVec2(min.x + 6.0f, max.y - 3.0f), ImVec2(max.x - 3.0f, min.y + 3.0f), visual.accent, 1.5f);
        break;
    case CellVisualKind::ServerGolden:
        draw->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, min.y), visual.accent, 1.0f);
        draw->AddLine(ImVec2(min.x + 5.0f, max.y), ImVec2(max.x, min.y + 5.0f), visual.accent, 1.0f);
        break;
    case CellVisualKind::Mispredicted:
        draw->AddLine(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(max.x - 2.0f, max.y - 2.0f), visual.accent, 2.0f);
        break;
    case CellVisualKind::Starved:
        draw_starved_mark(draw, min, max, visual.accent);
        break;
    case CellVisualKind::Removed:
        draw->AddLine(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(max.x - 2.0f, max.y - 2.0f), visual.accent, 1.5f);
        draw->AddLine(ImVec2(max.x - 2.0f, min.y + 2.0f), ImVec2(min.x + 2.0f, max.y - 2.0f), visual.accent, 1.5f);
        break;
    case CellVisualKind::Resimulated:
        draw->AddRectFilled(ImVec2(min.x, max.y - 4.0f), max, IM_COL32(146, 79, 24, 220));
        break;
    case CellVisualKind::Applied:
        draw->AddRectFilled(ImVec2(min.x + 5.0f, min.y), ImVec2(min.x + 9.0f, max.y), visual.accent);
        break;
    case CellVisualKind::InputReceived:
    case CellVisualKind::Cue:
    case CellVisualKind::Unknown:
        break;
    }
}

void apply_professional_dark_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 10.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.51f, 0.58f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.062f, 0.075f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.083f, 0.100f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.080f, 0.090f, 0.110f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.18f, 0.21f, 0.26f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.120f, 0.145f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.145f, 0.165f, 0.200f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.175f, 0.205f, 0.250f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.062f, 0.075f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.055f, 0.062f, 0.075f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.075f, 0.083f, 0.100f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.055f, 0.062f, 0.075f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.35f, 0.43f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.37f, 0.44f, 0.54f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.64f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.32f, 0.55f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.66f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.145f, 0.165f, 0.205f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.195f, 0.230f, 0.285f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.245f, 0.290f, 0.360f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.135f, 0.160f, 0.200f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.185f, 0.220f, 0.275f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.235f, 0.280f, 0.345f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.34f, 0.48f, 0.72f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.42f, 0.58f, 0.86f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.105f, 0.120f, 0.150f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.190f, 0.230f, 0.295f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.150f, 0.180f, 0.230f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.080f, 0.090f, 0.110f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.120f, 0.140f, 0.175f, 1.00f);
}

void draw_legend_item(const CellVisual& visual, const char* text) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 min(pos.x, pos.y + 2.0f);
    const ImVec2 max(min.x + pill_width, min.y + pill_height);
    draw->AddRectFilled(min, max, visual.fill);
    draw_cell_marks(draw, min, max, visual);
    ImGui::Dummy(ImVec2(pill_width, pill_height + 3.0f));
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
}

void render_legend() {
    draw_legend_item(CellVisual{CellVisualKind::Predicted, IM_COL32(53, 132, 246, 255), IM_COL32(176, 216, 255, 170), "predicted"}, "predicted");
    ImGui::SameLine(0.0f, 18.0f);
    draw_legend_item(CellVisual{CellVisualKind::Interpolated, IM_COL32(152, 89, 217, 255), IM_COL32(222, 190, 255, 180), "interpolated"}, "interpolated");
    ImGui::SameLine(0.0f, 18.0f);
    draw_legend_item(CellVisual{CellVisualKind::ServerGolden, IM_COL32(46, 160, 91, 255), IM_COL32(174, 239, 196, 170), "server/golden"}, "server/golden");
    ImGui::SameLine(0.0f, 18.0f);
    draw_legend_item(CellVisual{CellVisualKind::CorrectPrediction, IM_COL32(51, 177, 103, 255), IM_COL32(190, 255, 213, 180), "correct prediction"}, "correct");
    ImGui::SameLine(0.0f, 18.0f);
    draw_legend_item(CellVisual{CellVisualKind::Mispredicted, IM_COL32(218, 56, 67, 255), IM_COL32(255, 213, 218, 230), "mispredicted"}, "mispredict");
    ImGui::SameLine(0.0f, 18.0f);
    draw_legend_item(CellVisual{CellVisualKind::Starved, IM_COL32(134, 31, 42, 255), IM_COL32(255, 126, 136, 220), "starved"}, "starved");
    ImGui::SameLine(0.0f, 18.0f);
    draw_legend_item(CellVisual{CellVisualKind::Removed, IM_COL32(112, 119, 132, 255), IM_COL32(230, 236, 245, 190), "removed"}, "removed");
    ImGui::SameLine();
    draw_legend_item(CellVisual{CellVisualKind::Cue, IM_COL32(40, 159, 178, 255), IM_COL32(189, 247, 255, 190), "cue"}, "cue");
}

float frame_center_x(const ImVec2& origin, float scroll_x, SyncFrame min_frame, SyncFrame frame) {
    return origin.x + label_width + static_cast<float>(frame - min_frame) * frame_pitch - scroll_x + pill_width * 0.5f;
}

float row_center_y(const ImVec2& origin, float scroll_y, int row) {
    return origin.y + static_cast<float>(row) * row_height - scroll_y + 11.0f;
}

bool line_visible(const ImVec2& a, const ImVec2& b, const ImVec2& clip_min, const ImVec2& clip_max) {
    const float min_x = std::min(a.x, b.x);
    const float max_x = std::max(a.x, b.x);
    const float min_y = std::min(a.y, b.y);
    const float max_y = std::max(a.y, b.y);
    return max_x >= clip_min.x && min_x <= clip_max.x && max_y >= clip_min.y && min_y <= clip_max.y;
}

void draw_timeline_link(ImDrawList* draw, const ImVec2& a, const ImVec2& b, const ImVec2& clip_min, const ImVec2& clip_max) {
    if (line_visible(a, b, clip_min, clip_max)) {
        draw->AddLine(a, b, timeline_link_color, 1.2f);
    }
}

void draw_timeline_step_link(ImDrawList* draw, const ImVec2& a, const ImVec2& b, const ImVec2& clip_min, const ImVec2& clip_max) {
    if (a.y == b.y) {
        draw_timeline_link(draw, a, b, clip_min, clip_max);
        return;
    }
    const ImVec2 corner(b.x, a.y);
    draw_timeline_link(draw, a, corner, clip_min, clip_max);
    draw_timeline_link(draw, corner, b, clip_min, clip_max);
}

void draw_consecutive_cell_links(
    ImDrawList* draw,
    const KTraceFrameRun& run,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame first_visible_frame,
    SyncFrame last_visible_frame,
    int row,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    for (std::size_t index = 1; index < run.frames.size(); ++index) {
        const SyncFrame previous = run.frames[index - 1U].frame;
        const SyncFrame current = run.frames[index].frame;
        if (current < first_visible_frame) {
            continue;
        }
        if (previous > last_visible_frame) {
            break;
        }
        if (current != previous + 1U) {
            continue;
        }
        const float y = row_center_y(origin, scroll_y, row);
        draw_timeline_link(
            draw,
            ImVec2(frame_center_x(origin, scroll_x, min_frame, previous), y),
            ImVec2(frame_center_x(origin, scroll_x, min_frame, current), y),
            clip_min,
            clip_max);
    }
}

void draw_run_links(
    ImDrawList* draw,
    const KTraceFrameRun& run,
    const RunRenderItem& item,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame first_visible_frame,
    SyncFrame last_visible_frame,
    int run_base_row,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    if (item.end_frame < first_visible_frame || item.start_frame > last_visible_frame) {
        return;
    }
    auto it = std::lower_bound(
        run.frames.begin(),
        run.frames.end(),
        first_visible_frame,
        [](const KTraceFrameCell& cell, SyncFrame frame) {
            return cell.frame < frame;
        });
    if (it != run.frames.begin()) {
        --it;
    }
    for (; it != run.frames.end(); ++it) {
        const std::size_t index = static_cast<std::size_t>(it - run.frames.begin());
        if (index == 0U) {
            continue;
        }
        const SyncFrame previous = run.frames[index - 1U].frame;
        const SyncFrame current = run.frames[index].frame;
        if (current < first_visible_frame) {
            continue;
        }
        if (previous > last_visible_frame) {
            break;
        }
        if (current != previous + 1U) {
            continue;
        }
        const int previous_lane = run_lane_at_frame(item, previous);
        const int current_lane = run_lane_at_frame(item, current);
        if (previous_lane == unassigned_lane || current_lane == unassigned_lane) {
            continue;
        }
        draw_timeline_step_link(
            draw,
            ImVec2(frame_center_x(origin, scroll_x, min_frame, previous), row_center_y(origin, scroll_y, run_base_row + previous_lane)),
            ImVec2(frame_center_x(origin, scroll_x, min_frame, current), row_center_y(origin, scroll_y, run_base_row + current_lane)),
            clip_min,
            clip_max);
    }
}

void draw_run_drop_link(
    ImDrawList* draw,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame frame,
    int parent_row,
    int run_row,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    const float x = frame_center_x(origin, scroll_x, min_frame, frame);
    draw_timeline_link(
        draw,
        ImVec2(x, row_center_y(origin, scroll_y, parent_row)),
        ImVec2(x, row_center_y(origin, scroll_y, run_row)),
        clip_min,
        clip_max);
}

ImU32 color_with_alpha(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(value);
}

struct ProximityHighlight {
    bool active = false;
    ImVec2 mouse{};
    SyncFrame first_frame = 0;
    SyncFrame last_frame = 0;
    float inner_radius = 0.0f;
    float outer_radius = 0.0f;
    float inner_radius_sq = 0.0f;
    float outer_radius_sq = 0.0f;
};

float proximity_alpha(
    const ImVec2& mouse,
    const ImVec2& target,
    float inner_radius,
    float outer_radius,
    float inner_radius_sq,
    float outer_radius_sq) {
    const float dx = mouse.x - target.x;
    const float dy = mouse.y - target.y;
    const float distance_sq = dx * dx + dy * dy;
    if (distance_sq <= inner_radius_sq) {
        return 1.0f;
    }
    if (distance_sq >= outer_radius_sq || outer_radius <= inner_radius) {
        return 0.0f;
    }
    const float distance = std::sqrt(distance_sq);
    return std::clamp(1.0f - ((distance - inner_radius) / (outer_radius - inner_radius)), 0.0f, 1.0f);
}

template <typename Key, typename Hash>
float update_hover_alpha(std::unordered_map<Key, float, Hash>& alphas, const Key& key, float target) {
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
    const float in_speed = 10.0f;
    const float out_speed = 7.0f;
    float current = 0.0f;
    auto found = alphas.find(key);
    if (found != alphas.end()) {
        current = found->second;
    }
    const float clamped_target = std::clamp(target, 0.0f, 1.0f);
    const float step = dt * (clamped_target > current ? in_speed : out_speed);
    if (current < clamped_target) {
        current = std::min(clamped_target, current + step);
    } else if (current > clamped_target) {
        current = std::max(clamped_target, current - step);
    }
    if (clamped_target <= 0.0f && current <= 0.001f) {
        if (found != alphas.end()) {
            alphas.erase(found);
        }
        return 0.0f;
    }
    alphas[key] = current;
    return current;
}

template <typename Key, typename Hash>
float update_hover_alpha(std::unordered_map<Key, float, Hash>& alphas, const Key& key, bool hovered) {
    return update_hover_alpha(alphas, key, hovered ? 1.0f : 0.0f);
}

void draw_cell(
    ImDrawList* draw,
    const KTraceSourceHistory& source,
    const KTraceFrameCell& cell,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    int row,
    const KTraceEntityRow& entity,
    ashiato::Entity component,
    KTraceRunId run,
    int source_index,
    ViewerState& state,
    const ProximityHighlight& proximity,
    bool force_predicted_visual = false) {
    const float x = origin.x + label_width + static_cast<float>(cell.frame - min_frame) * frame_pitch - scroll_x;
    const float y = origin.y + static_cast<float>(row) * row_height - scroll_y + 4.0f;
    const ImVec2 min(x, y);
    const ImVec2 max(x + pill_width, y + pill_height);
    const CellVisual visual = force_predicted_visual
        ? CellVisual{CellVisualKind::Predicted, IM_COL32(53, 132, 246, 255), IM_COL32(176, 216, 255, 170), "predicted"}
        : visual_for_cell(cell, source.role);
    draw->AddRectFilled(min, max, visual.fill);
    draw_cell_marks(draw, min, max, visual);
    if (visual.kind != CellVisualKind::Starved && has_state(cell, KTraceCellState::Starved)) {
        draw_starved_mark(draw, min, max, IM_COL32(255, 126, 136, 220));
    }
    if (visual.kind == CellVisualKind::Cue) {
        const int cue_count = cue_count_for_cell(source, cell);
        if (cue_count > 1) {
            const std::string text = cue_count > 9 ? "9+" : std::to_string(cue_count);
            const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
            draw->AddText(
                ImVec2(
                    min.x + (pill_width - text_size.x) * 0.5f,
                    min.y + (pill_height - text_size.y) * 0.5f - 1.0f),
                IM_COL32(248, 252, 255, 255),
                text.c_str());
        }
    }

    const ImVec2 mouse = proximity.mouse;
    const bool hovered = mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
    const EventVisualKey visual_key{source_index, entity.client_network_id, component, cell.frame, run};
    const std::string popup_id = "frame_context##" +
        std::to_string(source_index) + ":" +
        std::to_string(entity.client_network_id) + ":" +
        std::to_string(component.value) + ":" +
        std::to_string(cell.frame) + ":" +
        std::to_string(run);
    const bool selected = state.selected.source_index == source_index &&
        state.selected.network_id == entity.client_network_id &&
        state.selected.component == component &&
        state.selected.frame == cell.frame &&
        state.selected.run == run &&
        state.selected.event_indices == cell.event_indices;
    float proximity_alpha_value = 0.0f;
    if (proximity.active && cell.frame >= proximity.first_frame && cell.frame <= proximity.last_frame) {
        proximity_alpha_value = proximity_alpha(
            proximity.mouse,
            ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
            proximity.inner_radius,
            proximity.outer_radius,
            proximity.inner_radius_sq,
            proximity.outer_radius_sq);
    }
    const float proximity_target = proximity_alpha_value * proximity_alpha_value * 0.5f;
    const float highlight_target = hovered ? 1.0f : proximity_target;
    const float highlight_alpha = update_hover_alpha(state.event_hover_alpha, visual_key, highlight_target);
    if (selected) {
        draw->AddRect(ImVec2(min.x - 2.0f, min.y - 2.0f), ImVec2(max.x + 2.0f, max.y + 2.0f), IM_COL32(255, 230, 155, 255), 0.0f, 0, 2.0f);
    } else if (highlight_alpha > 0.0f) {
        draw->AddRectFilled(
            ImVec2(min.x - 3.0f, min.y - 3.0f),
            ImVec2(max.x + 3.0f, max.y + 3.0f),
            color_with_alpha(IM_COL32(205, 226, 255, 82), highlight_alpha),
            3.0f);
        draw->AddRect(
            ImVec2(min.x - 1.5f, min.y - 1.5f),
            ImVec2(max.x + 1.5f, max.y + 1.5f),
            color_with_alpha(IM_COL32(235, 245, 255, 210), highlight_alpha),
            2.0f,
            0,
            1.2f);
    }
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s frame %u", visual.label, cell.frame);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            SelectedCell selected{source_index, entity.client_network_id, component, cell.frame, run, cell.event_indices};
            if (state.selected != selected) {
                state.selected = selected;
                state.details_dirty = true;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            SelectedCell selected{source_index, entity.client_network_id, component, cell.frame, run, cell.event_indices};
            if (state.selected != selected) {
                state.selected = selected;
                state.details_dirty = true;
            }
            ImGui::OpenPopup(popup_id.c_str());
        }
    }
    if (ImGui::BeginPopup(popup_id.c_str())) {
        if (ImGui::MenuItem("See in event viewer")) {
            jump_selected_frame_to_event_log(state);
        }
        if (ImGui::MenuItem("See in bandwidth viewer")) {
            jump_selected_frame_to_payload(state);
        }
        ImGui::EndPopup();
    }
}

bool entity_expanded(const ViewerState& state, int source_index, const KTraceEntityRow& entity) {
    return state.expanded_entities.count(EntityExpansionKey{source_index, entity.client_network_id}) != 0U;
}

bool component_expanded(
    const ViewerState& state,
    int source_index,
    const KTraceEntityRow& entity,
    ashiato::Entity component) {
    return state.expanded_components.count(ComponentExpansionKey{source_index, entity.client_network_id, component}) != 0U;
}

bool component_has_resim(const KTraceComponentRow& component) {
    return component.runs.size() > 1U;
}

bool component_has_run_lanes(const KTraceComponentRow& component) {
    return component.runs.size() > 1U;
}

void draw_expand_toggle(
    ImDrawList* draw,
    const ImVec2& center,
    bool expanded,
    bool hovered,
    bool enabled) {
    const ImU32 color = enabled
        ? (hovered ? IM_COL32(238, 244, 255, 255) : IM_COL32(176, 187, 204, 255))
        : IM_COL32(96, 106, 122, 180);
    if (expanded) {
        draw->AddTriangleFilled(
            ImVec2(center.x - 5.0f, center.y - 3.0f),
            ImVec2(center.x + 5.0f, center.y - 3.0f),
            ImVec2(center.x, center.y + 4.0f),
            color);
    } else {
        draw->AddTriangleFilled(
            ImVec2(center.x - 3.0f, center.y - 5.0f),
            ImVec2(center.x - 3.0f, center.y + 5.0f),
            ImVec2(center.x + 4.0f, center.y),
            color);
    }
}

bool mouse_in_rect(const ImVec2& min, const ImVec2& max) {
    const ImVec2 mouse = ImGui::GetMousePos();
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

void rebuild_source_metrics(ViewerState& state);

void toggle_entity_expanded(ViewerState& state, int source_index, const KTraceEntityRow& entity) {
    const EntityExpansionKey key{source_index, entity.client_network_id};
    if (state.expanded_entities.count(key) != 0U) {
        state.expanded_entities.erase(key);
    } else {
        state.expanded_entities.insert(key);
    }
    rebuild_source_metrics(state);
}

void toggle_component_expanded(
    ViewerState& state,
    int source_index,
    const KTraceEntityRow& entity,
    ashiato::Entity component) {
    const ComponentExpansionKey key{source_index, entity.client_network_id, component};
    if (state.expanded_components.count(key) != 0U) {
        state.expanded_components.erase(key);
    } else {
        state.expanded_components.insert(key);
    }
    rebuild_source_metrics(state);
}

void draw_entity_toggle(
    ImDrawList* draw,
    ViewerState& state,
    int source_index,
    const KTraceEntityRow& entity,
    const ImVec2& pos) {
    const bool expanded = entity_expanded(state, source_index, entity);
    const ImVec2 min(pos.x, pos.y);
    const ImVec2 max(pos.x + 18.0f, pos.y + 18.0f);
    const bool hovered = mouse_in_rect(min, max);
    draw_expand_toggle(draw, ImVec2(pos.x + 9.0f, pos.y + 9.0f), expanded, hovered, true);
    if (hovered) {
        ImGui::SetTooltip("%s entity", expanded ? "collapse" : "expand");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            toggle_entity_expanded(state, source_index, entity);
        }
    }
}

void draw_component_toggle(
    ImDrawList* draw,
    ViewerState& state,
    int source_index,
    const KTraceEntityRow& entity,
    ashiato::Entity component,
    const ImVec2& pos) {
    const auto found = std::find_if(entity.components.begin(), entity.components.end(), [component](const KTraceComponentRow& row) {
        return row.component == component;
    });
    const bool enabled = found != entity.components.end() && component_has_resim(*found);
    if (!enabled) {
        return;
    }
    const bool expanded = enabled && component_expanded(state, source_index, entity, component);
    const ImVec2 min(pos.x, pos.y);
    const ImVec2 max(pos.x + 18.0f, pos.y + 18.0f);
    const bool hovered = enabled && mouse_in_rect(min, max);
    draw_expand_toggle(draw, ImVec2(pos.x + 9.0f, pos.y + 9.0f), expanded, hovered, enabled);
    if (hovered) {
        ImGui::SetTooltip("%s component", expanded ? "collapse" : "expand");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            toggle_component_expanded(state, source_index, entity, component);
        }
    }
}

enum class SummaryState {
    None,
    Normal,
    Green,
    Mispredicted,
    Starved,
    Removed
};

void aggregate_entity_cell(SummaryState& summary, const KTraceFrameCell& cell) {
    if (has_state(cell, KTraceCellState::EntityDestroyed) || has_state(cell, KTraceCellState::Removed)) {
        summary = SummaryState::Removed;
    } else if (summary != SummaryState::Removed && has_state(cell, KTraceCellState::Starved)) {
        summary = SummaryState::Starved;
    } else if (summary != SummaryState::Removed && summary != SummaryState::Starved &&
        has_state(cell, KTraceCellState::Mispredicted)) {
        summary = SummaryState::Mispredicted;
    } else if (summary == SummaryState::None) {
        summary = SummaryState::Normal;
    }
}

void aggregate_component_cell(SummaryState& summary, const KTraceFrameCell& cell) {
    if (has_state(cell, KTraceCellState::Mispredicted)) {
        summary = SummaryState::Mispredicted;
    } else if (summary != SummaryState::Mispredicted &&
        (has_state(cell, KTraceCellState::PredictedCorrect) ||
            has_state(cell, KTraceCellState::SentToClient) ||
            has_state(cell, KTraceCellState::ReceivedFromServer))) {
        summary = SummaryState::Green;
    } else if (summary == SummaryState::None) {
        summary = SummaryState::Normal;
    }
}

template <typename Fn>
void for_each_component_cell(const KTraceComponentRow& component, Fn&& fn) {
    for (KTraceRunId run_id = 0; run_id < component.runs.size(); ++run_id) {
        const KTraceFrameRun& run = component.runs[run_id];
        for (const KTraceFrameCell& cell : run.frames) {
            fn(run_id, cell);
        }
    }
}

template <typename Fn>
void for_each_entity_cell(const KTraceEntityRow& entity, Fn&& fn) {
    for (const KTraceComponentRow& component : entity.components) {
        for_each_component_cell(component, [&](KTraceRunId run_id, const KTraceFrameCell& cell) {
            fn(component.component, run_id, cell);
        });
    }
}

void append_entity_frame_event_indices(
    const KTraceEntityRow& entity,
    SyncFrame frame,
    std::vector<std::uint32_t>& out) {
    for_each_entity_cell(entity, [&](ashiato::Entity, KTraceRunId, const KTraceFrameCell& cell) {
        if (cell.frame == frame) {
            out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
        }
    });
}

void append_component_frame_event_indices(
    const KTraceEntityRow& entity,
    ashiato::Entity component_id,
    SyncFrame frame,
    std::vector<std::uint32_t>& out) {
    for (const KTraceComponentRow& component : entity.components) {
        if (component.component != component_id) {
            continue;
        }
        for_each_component_cell(component, [&](KTraceRunId, const KTraceFrameCell& cell) {
            if (cell.frame == frame) {
                out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
            }
        });
    }
}

KTraceFrameCell summary_cell(SyncFrame frame, SummaryState summary) {
    KTraceFrameCell cell;
    cell.frame = frame;
    switch (summary) {
    case SummaryState::Green:
        cell.state_mask = static_cast<std::uint16_t>(KTraceCellState::PredictedCorrect);
        break;
    case SummaryState::Mispredicted:
        cell.state_mask = static_cast<std::uint16_t>(KTraceCellState::Mispredicted);
        break;
    case SummaryState::Starved:
        cell.state_mask = static_cast<std::uint16_t>(KTraceCellState::Starved);
        break;
    case SummaryState::Removed:
        cell.state_mask = static_cast<std::uint16_t>(KTraceCellState::EntityDestroyed);
        break;
    case SummaryState::Normal:
    case SummaryState::None:
        cell.state_mask = static_cast<std::uint16_t>(KTraceCellState::LocalPredicted);
        break;
    }
    return cell;
}

int draw_entity_summary_cells(
    ImDrawList* draw,
    const KTraceSourceHistory& source,
    const KTraceEntityRow& entity,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame first_visible_frame,
    SyncFrame last_visible_frame,
    int row,
    int source_index,
    ViewerState& state,
    const ProximityHighlight& proximity) {
    int drawn = 0;
    for (SyncFrame frame = first_visible_frame; frame <= last_visible_frame; ++frame) {
        SummaryState summary = SummaryState::None;
        for_each_entity_cell(entity, [&](ashiato::Entity, KTraceRunId, const KTraceFrameCell& cell) {
            if (cell.frame == frame) {
                aggregate_entity_cell(summary, cell);
            }
        });
        if (summary != SummaryState::None) {
            KTraceFrameCell cell = summary_cell(frame, summary);
            append_entity_frame_event_indices(entity, frame, cell.event_indices);
            draw_cell(draw, source, cell, origin, scroll_x, scroll_y, min_frame, row, entity, ashiato::Entity{}, 0, source_index, state, proximity);
            ++drawn;
        }
    }
    return drawn;
}

int draw_component_summary_cells(
    ImDrawList* draw,
    const KTraceSourceHistory& source,
    const KTraceEntityRow& entity,
    const KTraceComponentRow& component,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame first_visible_frame,
    SyncFrame last_visible_frame,
    int row,
    int source_index,
    ViewerState& state,
    const ProximityHighlight& proximity) {
    int drawn = 0;
    for (SyncFrame frame = first_visible_frame; frame <= last_visible_frame; ++frame) {
        SummaryState summary = SummaryState::None;
        for_each_component_cell(component, [&](KTraceRunId, const KTraceFrameCell& cell) {
            if (cell.frame == frame) {
                aggregate_component_cell(summary, cell);
            }
        });
        if (summary != SummaryState::None) {
            KTraceFrameCell cell = summary_cell(frame, summary);
            append_component_frame_event_indices(entity, component.component, frame, cell.event_indices);
            draw_cell(draw, source, cell, origin, scroll_x, scroll_y, min_frame, row, entity, component.component, 0, source_index, state, proximity);
            ++drawn;
        }
    }
    return drawn;
}

SummaryState entity_summary_at_frame(const KTraceEntityRow& entity, SyncFrame frame) {
    SummaryState summary = SummaryState::None;
    for_each_entity_cell(entity, [&](ashiato::Entity, KTraceRunId, const KTraceFrameCell& cell) {
        if (cell.frame == frame) {
            aggregate_entity_cell(summary, cell);
        }
    });
    return summary;
}

SummaryState component_summary_at_frame(
    const KTraceEntityRow& entity,
    const KTraceComponentRow& component,
    SyncFrame frame) {
    SummaryState summary = SummaryState::None;
    (void)entity;
    for_each_component_cell(component, [&](KTraceRunId, const KTraceFrameCell& cell) {
        if (cell.frame == frame) {
            aggregate_component_cell(summary, cell);
        }
    });
    return summary;
}

std::vector<SyncFrame> entity_summary_frames(const KTraceEntityRow& entity) {
    std::vector<SyncFrame> frames;
    for_each_entity_cell(entity, [&](ashiato::Entity, KTraceRunId, const KTraceFrameCell& cell) {
        frames.push_back(cell.frame);
    });
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

std::vector<SyncFrame> component_summary_frames(const KTraceEntityRow& entity, ashiato::Entity component_id) {
    std::vector<SyncFrame> frames;
    for (const KTraceComponentRow& component : entity.components) {
        if (component.component != component_id) {
            continue;
        }
        for_each_component_cell(component, [&](KTraceRunId, const KTraceFrameCell& cell) {
            frames.push_back(cell.frame);
        });
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

void append_nav_cell(
    std::vector<TimelineNavCell>& out,
    int source_index,
    const KTraceEntityRow& entity,
    ashiato::Entity component,
    const KTraceFrameCell& cell,
    KTraceRunId run,
    int row) {
    out.push_back(TimelineNavCell{
        SelectedCell{source_index, entity.client_network_id, component, cell.frame, run, cell.event_indices},
        row});
}

std::vector<TimelineNavCell> collect_timeline_nav_cells(
    ViewerState& state,
    const KTraceSourceHistory& source,
    int source_index) {
    std::vector<TimelineNavCell> cells;
    int row = 0;
    for (const KTraceEntityRow& entity : source.entities) {
        if (!entity_expanded(state, source_index, entity)) {
            for (SyncFrame frame : entity_summary_frames(entity)) {
                const SummaryState summary = entity_summary_at_frame(entity, frame);
                if (summary == SummaryState::None) {
                    continue;
                }
                KTraceFrameCell cell = summary_cell(frame, summary);
                append_entity_frame_event_indices(entity, frame, cell.event_indices);
                append_nav_cell(cells, source_index, entity, ashiato::Entity{}, cell, 0, row);
            }
            ++row;
            continue;
        }
        ++row;
        for (const KTraceComponentRow& component : entity.components) {
            const bool collapsible_component = component_has_resim(component);
            const bool expanded_component =
                !collapsible_component || component_expanded(state, source_index, entity, component.component);
            if (!expanded_component) {
                for (SyncFrame frame : component_summary_frames(entity, component.component)) {
                    const SummaryState summary = component_summary_at_frame(entity, component, frame);
                    if (summary == SummaryState::None) {
                        continue;
                    }
                    KTraceFrameCell cell = summary_cell(frame, summary);
                    append_component_frame_event_indices(entity, component.component, frame, cell.event_indices);
                    append_nav_cell(cells, source_index, entity, component.component, cell, 0, row);
                }
                ++row;
                continue;
            }
            if (!component.runs.empty()) {
                for (const KTraceFrameCell& cell : component.runs[0].frames) {
                    append_nav_cell(cells, source_index, entity, component.component, cell, 0, row);
                }
            }
            ++row;
            const int run_base_row = row;
            if (component_has_run_lanes(component)) {
                const RunLaneLayout& run_layout = get_run_lane_layout(state, source_index, entity, component);
                for (const RunRenderItem& item : run_layout.items) {
                    if (item.run_id >= component.runs.size()) {
                        continue;
                    }
                    const KTraceFrameRun& run = component.runs[item.run_id];
                    for (const KTraceFrameCell& cell : run.frames) {
                        const int cell_lane = run_lane_at_frame(item, cell.frame);
                        if (cell_lane != unassigned_lane) {
                            append_nav_cell(cells, source_index, entity, component.component, cell, item.run_id, run_base_row + cell_lane);
                        }
                    }
                }
                row += run_layout.lane_count;
            }
        }
    }
    std::sort(cells.begin(), cells.end(), [](const TimelineNavCell& lhs, const TimelineNavCell& rhs) {
        if (lhs.row != rhs.row) {
            return lhs.row < rhs.row;
        }
        return lhs.selected.frame < rhs.selected.frame;
    });
    return cells;
}

SourceMetrics compute_source_metrics(const KTraceSourceHistory& source, ViewerState& state, int source_index);
void rebuild_source_metrics(ViewerState& state);
void update_source_metrics(ViewerState& state, int source_index);

void apply_server_entity_links(SyncTraceHistory& history) {
    std::unordered_map<ClientEntityNetworkId, ashiato::Entity> server_entities_by_network_id;
    for (const KTraceSourceHistory& source : history.sources) {
        if (source.role != SyncTraceRole::Server) {
            continue;
        }
        for (const KTraceRecord& record : source.records) {
            const SyncTraceEvent& event = record.event;
            if (event.type == SyncTraceEventType::EntityStartedSyncing &&
                event.client_network_id != invalid_client_entity_network_id &&
                event.server_entity) {
                server_entities_by_network_id[event.client_network_id] = event.server_entity;
            }
        }
    }
    if (server_entities_by_network_id.empty()) {
        return;
    }
    for (KTraceSourceHistory& source : history.sources) {
        if (source.role != SyncTraceRole::Client) {
            continue;
        }
        for (KTraceEntityRow& entity : source.entities) {
            const auto found = server_entities_by_network_id.find(entity.client_network_id);
            if (found != server_entities_by_network_id.end()) {
                entity.server_entity = found->second;
            }
        }
    }
}

void link_client_source_to_server_entities(ViewerState& state, int source_index) {
    if (source_index < 0 || source_index >= static_cast<int>(state.history.sources.size()) ||
        state.server_entities_by_network_id.empty()) {
        return;
    }
    KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
    if (source.role != SyncTraceRole::Client) {
        return;
    }
    for (KTraceEntityRow& entity : source.entities) {
        const auto found = state.server_entities_by_network_id.find(entity.client_network_id);
        if (found != state.server_entities_by_network_id.end()) {
            entity.server_entity = found->second;
        }
    }
}

void publish_loaded_source(
    ViewerState& state,
    int source_index,
    KTraceSourceHistory&& source,
    const SourceMetrics& metrics,
    std::vector<SelectedCell>&& benchmark_candidates) {
    if (source_index < 0) {
        return;
    }
    clear_run_lane_cache_for_source(state, source_index);
    if (static_cast<int>(state.history.sources.size()) <= source_index) {
        state.history.sources.resize(static_cast<std::size_t>(source_index + 1));
    }
    state.history.sources[static_cast<std::size_t>(source_index)] = std::move(source);

    KTraceSourceHistory& published = state.history.sources[static_cast<std::size_t>(source_index)];
    if (published.role == SyncTraceRole::Server) {
        for (const KTraceRecord& record : published.records) {
            const SyncTraceEvent& event = record.event;
            if (event.type == SyncTraceEventType::EntityStartedSyncing &&
                event.client_network_id != invalid_client_entity_network_id &&
                event.server_entity) {
                state.server_entities_by_network_id[event.client_network_id] = event.server_entity;
            }
        }
        for (int index = 0; index < static_cast<int>(state.history.sources.size()); ++index) {
            link_client_source_to_server_entities(state, index);
        }
    } else {
        link_client_source_to_server_entities(state, source_index);
    }

    if (state.source_metrics.size() < state.history.sources.size()) {
        state.source_metrics.resize(state.history.sources.size());
    }
    state.source_metrics[static_cast<std::size_t>(source_index)] = metrics;
    state.benchmark_candidates.erase(
        std::remove_if(
            state.benchmark_candidates.begin(),
            state.benchmark_candidates.end(),
            [source_index](const SelectedCell& cell) {
                return cell.source_index == source_index;
            }),
        state.benchmark_candidates.end());
    state.benchmark_candidates.insert(
        state.benchmark_candidates.end(),
        std::make_move_iterator(benchmark_candidates.begin()),
        std::make_move_iterator(benchmark_candidates.end()));
    state.packet_log_dirty = true;
    state.payload_dirty = true;
    state.payload_view_dirty = true;
    state.details_dirty = true;
    state.packet_details_dirty = true;
    state.cached_details.clear();
    if (state.selected_source < 0) {
        state.selected_source = 0;
    }
}

void push_loader_message(TraceLoadState& loader, TraceLoadMessage message) {
    std::lock_guard<std::mutex> lock(loader.mutex);
    loader.messages.push_back(std::move(message));
}

void reset_loaded_trace(ViewerState& state) {
    state.history.sources.clear();
    state.selected = {};
    state.selected_packet = {};
    state.expanded_entities.clear();
    state.expanded_components.clear();
    state.run_lane_layouts.clear();
    state.event_hover_alpha.clear();
    state.packet_hover_alpha.clear();
    state.details_dirty = true;
    state.packet_details_dirty = true;
    state.cached_details.clear();
    state.source_metrics.clear();
    state.benchmark_candidates.clear();
    state.packet_events.clear();
    state.packet_flows.clear();
    state.packet_clients.clear();
    state.packet_log_min_us = 0;
    state.packet_log_max_us = 0;
    state.payload_packets.clear();
    state.payload_rows.clear();
    state.payload_buckets.clear();
    state.payload_filtered_packets.clear();
    state.selected_payload_row = -1;
    state.selected_payload_packet = -1;
    clear_selected_payload_scope(state);
    state.payload_source_filter = PayloadSourceFilter::All;
    state.payload_direction_filter = PayloadDirectionFilter::All;
    state.payload_excluded_clients.clear();
    state.payload_filter_enabled = false;
    state.payload_hierarchy_view = false;
    state.payload_return_to_flat_on_back_to_top = false;
    state.payload_filter[0] = '\0';
    state.payload_time_filter_enabled = false;
    state.payload_drag_selecting = false;
    state.payload_dirty = true;
    state.payload_view_dirty = true;
    state.server_entities_by_network_id.clear();
    state.packet_log_dirty = false;
    state.scroll_selected_frame_into_view = false;
    state.scroll_selected_packet_into_view = false;
    state.selected_source = 0;
    state.selected_source_dirty = true;
}

void stop_loading_directory(ViewerState& state) {
    if (state.loader.cancel) {
        state.loader.cancel->store(true);
    }
    if (state.loader.worker.joinable()) {
        state.loader.worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(state.loader.mutex);
        state.loader.messages.clear();
    }
    state.loader.cancel.reset();
    state.loader.active = false;
}

void process_loader_messages(ViewerState& state) {
    constexpr std::size_t max_messages_per_frame = 64;
    std::deque<TraceLoadMessage> messages;
    {
        std::lock_guard<std::mutex> lock(state.loader.mutex);
        bool source_ready_queued = false;
        while (!state.loader.messages.empty() && messages.size() < max_messages_per_frame) {
            if (state.loader.messages.front().type == TraceLoadMessage::Type::SourceReady) {
                if (source_ready_queued) {
                    break;
                }
                source_ready_queued = true;
            }
            messages.push_back(std::move(state.loader.messages.front()));
            state.loader.messages.pop_front();
        }
    }
    if (messages.empty()) {
        return;
    }

    for (TraceLoadMessage& message : messages) {
        state.loader.bytes_read = message.bytes_read;
        state.loader.total_bytes = message.total_bytes;
        switch (message.type) {
        case TraceLoadMessage::Type::SourceBegin: {
            if (message.source_index < 0) {
                break;
            }
            if (static_cast<int>(state.history.sources.size()) <= message.source_index) {
                state.history.sources.resize(static_cast<std::size_t>(message.source_index + 1));
            }
            KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(message.source_index)];
            source = {};
            source.role = message.source.role;
            source.client = message.source.client;
            source.path = message.source.path;
            source.recorded_unix_ns = message.source.recorded_unix_ns;
            source.flags = message.source.flags;
            state.selected_source = std::min(state.selected_source, static_cast<int>(state.history.sources.size()) - 1);
            state.selected_source_dirty = true;
            break;
        }
        case TraceLoadMessage::Type::Progress:
            break;
        case TraceLoadMessage::Type::SourceReady: {
            if (message.source_index < 0) {
                break;
            }
            publish_loaded_source(
                state,
                message.source_index,
                std::move(message.history),
                message.metrics,
                std::move(message.benchmark_candidates));
            break;
        }
        case TraceLoadMessage::Type::Finished:
            state.loader.active = false;
            state.loader.finished = true;
            state.loader.load_ms = message.load_ms;
            state.benchmark.load_ms = message.load_ms;
            state.status = "loaded " + std::to_string(state.history.sources.size()) + " trace source(s)";
            state.status_is_error = false;
            break;
        case TraceLoadMessage::Type::Failed:
            state.loader.active = false;
            state.loader.finished = true;
            state.benchmark.load_ms = 0.0;
            state.status = message.error;
            state.status_is_error = true;
            break;
        }
    }
}

SourceMetrics compute_collapsed_source_metrics(const KTraceSourceHistory& source) {
    SourceMetrics metrics;
    bool initialized = false;
    for (const KTraceRecord& record : source.records) {
        if (!initialized) {
            metrics.min_frame = record.event.frame;
            metrics.max_frame = record.event.frame;
            initialized = true;
        } else {
            metrics.min_frame = std::min(metrics.min_frame, record.event.frame);
            metrics.max_frame = std::max(metrics.max_frame, record.event.frame);
        }
    }
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceComponentRow& component : entity.components) {
            for (const KTraceFrameRun& run : component.runs) {
                const SyncFrame end = run_end_frame(run);
                metrics.max_frame = std::max(metrics.max_frame, end);
                if (end != std::numeric_limits<SyncFrame>::max()) {
                    metrics.max_frame = std::max(metrics.max_frame, end + 1U);
                }
                metrics.cells += static_cast<int>(run.frames.size());
            }
        }
    }
    metrics.rows = static_cast<int>(source.entities.size());
    return metrics;
}

std::vector<SelectedCell> collect_benchmark_candidates(const KTraceSourceHistory& source, int source_index) {
    std::vector<SelectedCell> candidates;
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceComponentRow& component : entity.components) {
            for (KTraceRunId run_id = 0; run_id < component.runs.size(); ++run_id) {
                for (const KTraceFrameCell& cell : component.runs[run_id].frames) {
                    candidates.push_back(
                        SelectedCell{source_index, entity.client_network_id, component.component, cell.frame, run_id});
                }
            }
        }
    }
    return candidates;
}

void stream_trace_directory(
    const std::string directory,
    TraceLoadState& loader,
    std::shared_ptr<std::atomic_bool> cancel) {
    constexpr std::size_t records_per_progress_update = 2048;
    const auto start = Clock::now();
    try {
        std::vector<KTraceFileHeader> sources;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
            if (cancel->load()) {
                return;
            }
            if (!entry.is_regular_file() || entry.path().extension() != ".ktrace") {
                continue;
            }
            sources.push_back(KTraceStreamReader(entry.path().string()).header());
        }
        std::sort(sources.begin(), sources.end(), [](const KTraceFileHeader& lhs, const KTraceFileHeader& rhs) {
            if (lhs.role != rhs.role) {
                return rhs.role == SyncTraceRole::Server;
            }
            if (lhs.client != rhs.client) {
                return lhs.client < rhs.client;
            }
            return lhs.path < rhs.path;
        });

        std::uint64_t total_bytes = 0;
        for (const KTraceFileHeader& source : sources) {
            total_bytes += source.file_size;
        }

        std::uint64_t completed_file_bytes = 0;
        for (int source_index = 0; source_index < static_cast<int>(sources.size()); ++source_index) {
            if (cancel->load()) {
                return;
            }
            const KTraceFileHeader& source = sources[static_cast<std::size_t>(source_index)];
            push_loader_message(loader, TraceLoadMessage{
                TraceLoadMessage::Type::SourceBegin,
                source_index,
                source,
                {},
                {},
                {},
                completed_file_bytes,
                total_bytes,
                0.0,
                {}});

            KTraceStreamReader reader(source.path);
            KTraceFile file;
            file.path = source.path;
            file.version = source.version;
            file.role = source.role;
            file.recorded_unix_ns = source.recorded_unix_ns;
            file.client = source.client;
            file.flags = source.flags;
            std::size_t records_since_progress_update = 0;
            KTraceRecord record;
            while (!cancel->load() && reader.read_next(record)) {
                file.records.push_back(std::move(record));
                ++records_since_progress_update;
                if (records_since_progress_update >= records_per_progress_update) {
                    records_since_progress_update = 0;
                    push_loader_message(loader, TraceLoadMessage{
                        TraceLoadMessage::Type::Progress,
                        source_index,
                        {},
                        {},
                        {},
                        {},
                        completed_file_bytes + std::min(reader.position(), source.file_size),
                        total_bytes,
                        0.0,
                        {}});
                }
            }
            if (!cancel->load()) {
                KTraceSourceHistory history = KTraceReader::build_source_history(std::move(file));
                SourceMetrics metrics = compute_collapsed_source_metrics(history);
                std::vector<SelectedCell> benchmark_candidates = collect_benchmark_candidates(history, source_index);
                push_loader_message(loader, TraceLoadMessage{
                    TraceLoadMessage::Type::SourceReady,
                    source_index,
                    {},
                    std::move(history),
                    metrics,
                    std::move(benchmark_candidates),
                    completed_file_bytes + source.file_size,
                    total_bytes,
                    0.0,
                    {}});
            }
            completed_file_bytes += source.file_size;
        }

        if (!cancel->load()) {
            push_loader_message(loader, TraceLoadMessage{
                TraceLoadMessage::Type::Finished,
                -1,
                {},
                {},
                {},
                {},
                total_bytes,
                total_bytes,
                elapsed_ms(start),
                {}});
        }
    } catch (const std::exception& error) {
        if (!cancel->load()) {
            push_loader_message(loader, TraceLoadMessage{
                TraceLoadMessage::Type::Failed,
                -1,
                {},
                {},
                {},
                {},
                0,
                0,
                elapsed_ms(start),
                error.what()});
        }
    }
}

void load_directory(ViewerState& state) {
    state.directory_has_ktrace_files = directory_contains_ktrace_files(state.directory.data());
    if (!state.directory_has_ktrace_files) {
        stop_loading_directory(state);
        reset_loaded_trace(state);
        state.status = "choose a directory containing .ktrace files";
        state.status_is_error = false;
        return;
    }

    stop_loading_directory(state);
    reset_loaded_trace(state);
    state.loader.bytes_read = 0;
    state.loader.total_bytes = 0;
    state.loader.load_ms = 0.0;
    state.loader.active = true;
    state.loader.finished = false;
    state.loader.cancel = std::make_shared<std::atomic_bool>(false);
    state.status = "loading trace data...";
    state.status_is_error = false;
    const std::string directory = state.directory.data();
    std::shared_ptr<std::atomic_bool> cancel = state.loader.cancel;
    state.loader.worker = std::thread([directory, &loader = state.loader, cancel]() {
        stream_trace_directory(directory, loader, cancel);
    });
}

std::string path_to_string(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

void set_char_buffer(std::array<char, 1024>& buffer, const std::string& value) {
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
}

bool directory_contains_ktrace_files(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(path, error) || error) {
        return false;
    }

    const fs::directory_options options = fs::directory_options::skip_permission_denied;
    for (fs::directory_iterator it(path, options, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error && it->path().extension() == ".ktrace") {
            return true;
        }
    }
    return false;
}

std::filesystem::path current_picker_path(const ViewerState& state) {
    std::filesystem::path path = state.picker_path.data();
    if (path.empty()) {
        path = std::filesystem::current_path();
    }
    if (path.is_relative()) {
        path = std::filesystem::absolute(path);
    }
    return path.lexically_normal();
}

void refresh_directory_picker(ViewerState& state) {
    namespace fs = std::filesystem;
    state.picker_entries.clear();
    state.picker_error.clear();
    state.picker_selected = -1;
    state.picker_path_has_ktrace_files = false;

    std::error_code error;
    fs::path path = current_picker_path(state);
    if (!fs::exists(path, error) || error) {
        state.picker_error = "directory does not exist";
        return;
    }
    if (!fs::is_directory(path, error) || error) {
        state.picker_error = "path is not a directory";
        return;
    }

    set_char_buffer(state.picker_path, path_to_string(path));
    state.picker_path_has_ktrace_files = directory_contains_ktrace_files(path);
    fs::directory_options options = fs::directory_options::skip_permission_denied;
    for (fs::directory_iterator it(path, options, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_directory(entry_error) || entry_error) {
            continue;
        }
        DirectoryPickerEntry entry;
        entry.path = path_to_string(it->path());
        entry.name = it->path().filename().string();
        if (entry.name.empty()) {
            entry.name = entry.path;
        }
        entry.has_ktrace_files = directory_contains_ktrace_files(it->path());
        state.picker_entries.push_back(std::move(entry));
    }
    if (error) {
        state.picker_error = error.message();
    }

    std::sort(state.picker_entries.begin(), state.picker_entries.end(), [](const DirectoryPickerEntry& lhs, const DirectoryPickerEntry& rhs) {
        return lhs.name < rhs.name;
    });
}

void open_directory_picker(ViewerState& state) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path start = state.directory[0] != '\0' ? fs::path(state.directory.data()) : fs::current_path(error);
    if (error || start.empty()) {
        start = fs::current_path();
    }
    if (start.is_relative()) {
        start = fs::absolute(start, error);
    }
    if (!error && !fs::is_directory(start, error)) {
        start = start.parent_path();
    }
    if (error || start.empty()) {
        start = fs::current_path();
    }
    set_char_buffer(state.picker_path, path_to_string(start));
    refresh_directory_picker(state);
    ImGui::OpenPopup("Choose Trace Directory");
}

void render_directory_picker(ViewerState& state) {
    if (!ImGui::BeginPopupModal("Choose Trace Directory", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::SetNextItemWidth(620.0f);
    const bool submitted = ImGui::InputText("##picker_path", state.picker_path.data(), state.picker_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(54.0f, 0.0f)) || submitted) {
        refresh_directory_picker(state);
    }

    const std::filesystem::path path = current_picker_path(state);
    const std::filesystem::path parent = path.parent_path();
    const bool has_parent = !parent.empty() && parent != path;
    if (!has_parent) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Up", ImVec2(54.0f, 0.0f))) {
        set_char_buffer(state.picker_path, path_to_string(parent));
        refresh_directory_picker(state);
    }
    if (!has_parent) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(78.0f, 0.0f))) {
        refresh_directory_picker(state);
    }

    if (!state.picker_error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.48f, 1.0f), "%s", state.picker_error.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.56f, 0.62f, 0.70f, 1.0f), "%zu directories", state.picker_entries.size());
    }

    ImGui::BeginChild("picker_entries", ImVec2(720.0f, 360.0f), true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.picker_entries.size()), ImGui::GetTextLineHeightWithSpacing());
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const DirectoryPickerEntry& entry = state.picker_entries[static_cast<std::size_t>(index)];
            const bool selected = state.picker_selected == index;
            const std::string label = entry.has_ktrace_files
                ? std::string("[trace] ") + entry.name
                : entry.name;
            if (entry.has_ktrace_files) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.86f, 0.66f, 1.0f));
            }
            const bool clicked = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick);
            if (entry.has_ktrace_files) {
                ImGui::PopStyleColor();
            }
            if (clicked) {
                state.picker_selected = index;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    set_char_buffer(state.picker_path, entry.path);
                    refresh_directory_picker(state);
                }
            }
        }
    }
    ImGui::EndChild();

    const bool can_choose = state.picker_error.empty() && state.picker_path_has_ktrace_files;
    if (!can_choose) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Open This Directory", ImVec2(150.0f, 0.0f))) {
        set_char_buffer(state.directory, path_to_string(current_picker_path(state)));
        state.directory_has_ktrace_files = state.picker_path_has_ktrace_files;
        load_directory(state);
        ImGui::CloseCurrentPopup();
    }
    if (!can_choose) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const bool has_selected =
        state.picker_selected >= 0 && state.picker_selected < static_cast<int>(state.picker_entries.size());
    const bool can_choose_selected =
        has_selected && state.picker_entries[static_cast<std::size_t>(state.picker_selected)].has_ktrace_files;
    if (!can_choose_selected) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Open Selected", ImVec2(126.0f, 0.0f))) {
        const DirectoryPickerEntry& entry = state.picker_entries[static_cast<std::size_t>(state.picker_selected)];
        set_char_buffer(state.directory, entry.path);
        state.directory_has_ktrace_files = entry.has_ktrace_files;
        load_directory(state);
        ImGui::CloseCurrentPopup();
    }
    if (!can_choose_selected) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(78.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void frame_range(const KTraceSourceHistory& source, SyncFrame& min_frame, SyncFrame& max_frame) {
    min_frame = 0;
    max_frame = 0;
    bool initialized = false;
    for (const KTraceRecord& record : source.records) {
        if (!initialized) {
            min_frame = record.event.frame;
            max_frame = record.event.frame;
            initialized = true;
        } else {
            min_frame = std::min(min_frame, record.event.frame);
            max_frame = std::max(max_frame, record.event.frame);
        }
    }
}

int row_count(const KTraceSourceHistory& source, ViewerState& state, int source_index) {
    int rows = 0;
    for (const KTraceEntityRow& entity : source.entities) {
        rows += 1;
        if (!entity_expanded(state, source_index, entity)) {
            continue;
        }
        for (const KTraceComponentRow& component : entity.components) {
            rows += 1;
            if (component_has_resim(component) &&
                !component_expanded(state, source_index, entity, component.component)) {
                continue;
            }
            if (component_has_run_lanes(component)) {
                rows += get_run_lane_layout(state, source_index, entity, component).lane_count;
            }
        }
    }
    return rows;
}

int cell_count(const KTraceSourceHistory& source) {
    int cells = 0;
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceComponentRow& component : entity.components) {
            for (const KTraceFrameRun& run : component.runs) {
                cells += static_cast<int>(run.frames.size());
            }
        }
    }
    return cells;
}

SourceMetrics compute_source_metrics(const KTraceSourceHistory& source, ViewerState& state, int source_index) {
    SourceMetrics metrics;
    frame_range(source, metrics.min_frame, metrics.max_frame);
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceComponentRow& component : entity.components) {
            for (const KTraceFrameRun& run : component.runs) {
                const SyncFrame end = run_end_frame(run);
                metrics.max_frame = std::max(metrics.max_frame, end);
                if (end != std::numeric_limits<SyncFrame>::max()) {
                    metrics.max_frame = std::max(metrics.max_frame, end + 1U);
                }
            }
        }
    }
    metrics.rows = row_count(source, state, source_index);
    metrics.cells = cell_count(source);
    return metrics;
}

void rebuild_source_metrics(ViewerState& state) {
    state.source_metrics.clear();
    state.source_metrics.reserve(state.history.sources.size());
    state.benchmark_candidates.clear();
    for (int source_index = 0; source_index < static_cast<int>(state.history.sources.size()); ++source_index) {
        const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
        state.source_metrics.push_back(compute_source_metrics(source, state, source_index));
        for (const KTraceEntityRow& entity : source.entities) {
            for (const KTraceComponentRow& component : entity.components) {
                for (KTraceRunId run_id = 0; run_id < component.runs.size(); ++run_id) {
                    for (const KTraceFrameCell& cell : component.runs[run_id].frames) {
                        state.benchmark_candidates.push_back(
                            SelectedCell{source_index, entity.client_network_id, component.component, cell.frame, run_id});
                    }
                }
            }
        }
    }
}

void update_source_metrics(ViewerState& state, int source_index) {
    if (source_index < 0 || source_index >= static_cast<int>(state.history.sources.size())) {
        return;
    }
    if (state.source_metrics.size() < state.history.sources.size()) {
        state.source_metrics.resize(state.history.sources.size());
    }
    state.benchmark_candidates.erase(
        std::remove_if(
            state.benchmark_candidates.begin(),
            state.benchmark_candidates.end(),
            [source_index](const SelectedCell& cell) {
                return cell.source_index == source_index;
            }),
        state.benchmark_candidates.end());

    const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
    state.source_metrics[static_cast<std::size_t>(source_index)] =
        compute_source_metrics(source, state, source_index);
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceComponentRow& component : entity.components) {
            for (KTraceRunId run_id = 0; run_id < component.runs.size(); ++run_id) {
                for (const KTraceFrameCell& cell : component.runs[run_id].frames) {
                    state.benchmark_candidates.push_back(
                        SelectedCell{source_index, entity.client_network_id, component.component, cell.frame, run_id});
                }
            }
        }
    }
}

double benchmark_phase(int frame_index, int frames, double offset) {
    if (frames <= 1) {
        return 0.0;
    }
    const double t = std::fmod((static_cast<double>(frame_index) / static_cast<double>(frames - 1)) + offset, 1.0);
    return t < 0.5 ? t * 2.0 : (1.0 - t) * 2.0;
}

void apply_benchmark_scroll(ViewerState& state, float max_scroll_x, float max_scroll_y) {
    if (!state.benchmark.options.enabled) {
        return;
    }
    const int frame = state.benchmark.frame_index;
    const int frames = std::max(1, state.benchmark.options.frames);
    ImGui::SetScrollX(static_cast<float>(benchmark_phase(frame, frames, 0.0) * max_scroll_x));
    ImGui::SetScrollY(static_cast<float>(benchmark_phase(frame, frames, 0.23) * max_scroll_y));
}

float packet_event_y(const ImVec2& origin, float scroll_y, std::uint64_t relative_us) {
    return origin.y + timeline_header_height + 32.0f + static_cast<float>(relative_us) * packet_time_us_pitch - scroll_y;
}

float packet_event_x(float column_x, const PacketEventInfo& event) {
    const float base = event.server_side
        ? column_x + packet_column_width - 42.0f
        : column_x + 28.0f;
    return event.server_side ? base - event.lane_offset : base + event.lane_offset;
}

bool packet_event_matches_selection(const PacketEventInfo& event, const SelectedPacketChip& selected) {
    return selected.source_index == event.source_index && selected.event_index == event.record_index;
}

bool packet_event_hit(const ImVec2& center, const ImVec2& mouse) {
    const ImVec2 min(center.x - packet_marker_size * 0.5f, center.y - packet_marker_size * 0.5f);
    const ImVec2 max(center.x + packet_marker_size * 0.5f, center.y + packet_marker_size * 0.5f);
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

ImVec2 packet_marker_size_for_event(const PacketEventInfo& event) {
    if (event.send && (event.message == "client_input" || event.message == "server_update")) {
        const std::string label = packet_marker_label(event);
        const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
        return ImVec2(std::max(34.0f, text_size.x + 12.0f), 18.0f);
    }
    if (event.send && packet_message_is_ping_or_pong(event)) {
        return ImVec2(18.0f, 18.0f);
    }
    return ImVec2(packet_marker_size, packet_marker_size);
}

bool packet_event_hit(const PacketEventInfo& event, const ImVec2& center, const ImVec2& mouse) {
    const ImVec2 size = packet_marker_size_for_event(event);
    const ImVec2 min(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
    const ImVec2 max(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

int selected_packet_event_index(const ViewerState& state) {
    if (state.selected_packet.source_index < 0 ||
        state.selected_packet.event_index == std::numeric_limits<std::uint32_t>::max()) {
        return -1;
    }
    for (int event_index = 0; event_index < static_cast<int>(state.packet_events.size()); ++event_index) {
        const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
        if (packet_event_matches_selection(event, state.selected_packet)) {
            return event_index;
        }
    }
    return -1;
}

bool packet_flow_active(const ViewerState& state, int flow_index, int hovered_event_index, int selected_event_index) {
    if (flow_index < 0 || flow_index >= static_cast<int>(state.packet_flows.size())) {
        return false;
    }
    const PacketFlow& flow = state.packet_flows[static_cast<std::size_t>(flow_index)];
    return hovered_event_index == flow.send_event ||
        hovered_event_index == flow.receive_event ||
        selected_event_index == flow.send_event ||
        selected_event_index == flow.receive_event;
}

void select_packet_event(ViewerState& state, const PacketEventInfo& event) {
    SelectedPacketChip selected;
    selected.source_index = event.source_index;
    selected.event_index = event.record_index;
    selected.event_indices.push_back({event.source_index, event.record_index});
    if (state.selected_packet != selected) {
        state.selected_packet = std::move(selected);
        state.packet_details_dirty = true;
    }
}

SyncFrame packet_local_frame_from_markers(
    const PacketEventInfo& event,
    const std::vector<PacketFrameMarker>& markers) {
    if (markers.empty()) {
        return event.frame;
    }
    if (event.send) {
        const PacketFrameMarker* selected = nullptr;
        for (const PacketFrameMarker& marker : markers) {
            if (marker.absolute_us > event.absolute_us) {
                break;
            }
            selected = &marker;
        }
        return selected != nullptr ? selected->frame : markers.front().frame;
    }
    for (const PacketFrameMarker& marker : markers) {
        if (marker.absolute_us >= event.absolute_us) {
            return marker.frame;
        }
    }
    return markers.back().frame;
}

void draw_packet_marker(
    ViewerState& state,
    ImDrawList* draw,
    const PacketEventInfo& event,
    SyncFrame local_frame,
    const ImVec2& center,
    bool hovered,
    bool highlighted,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    if (center.y + packet_marker_size < clip_min.y || center.y - packet_marker_size > clip_max.y) {
        return;
    }
    const ImVec2 marker_size = packet_marker_size_for_event(event);
    const ImVec2 min(center.x - marker_size.x * 0.5f, center.y - marker_size.y * 0.5f);
    const ImVec2 max(center.x + marker_size.x * 0.5f, center.y + marker_size.y * 0.5f);
    const bool selected = packet_event_matches_selection(event, state.selected_packet);
    const PacketVisualKey visual_key{event.source_index, event.record_index};
    const std::string popup_id = "packet_context##" +
        std::to_string(event.source_index) + ":" +
        std::to_string(event.record_index);
    const float hover_alpha = update_hover_alpha(state.packet_hover_alpha, visual_key, hovered);
    if (selected) {
        draw->AddRectFilled(
            ImVec2(min.x - 5.0f, min.y - 5.0f),
            ImVec2(max.x + 5.0f, max.y + 5.0f),
            IM_COL32(255, 218, 112, 80),
            4.0f);
    } else if (highlighted) {
        draw->AddRect(
            ImVec2(min.x - 3.0f, min.y - 3.0f),
            ImVec2(max.x + 3.0f, max.y + 3.0f),
            IM_COL32(255, 218, 112, 150),
            3.0f,
            0,
            1.4f);
    }
    if (event.send && packet_message_is_ping_or_pong(event)) {
        const ImVec2 top(center.x, min.y);
        const ImVec2 left(min.x, max.y);
        const ImVec2 right(max.x, max.y);
        draw->AddTriangleFilled(top, right, left, IM_COL32(145, 96, 232, 255));
        draw->AddTriangle(top, right, left, IM_COL32(226, 210, 255, 235), 1.4f);
    } else if (!event.send) {
        if (packet_update_apply_failed(event)) {
            if (packet_update_apply_failed_from_stale_frame(event)) {
                draw->AddCircleFilled(center, packet_marker_size * 0.5f, IM_COL32(247, 199, 74, 255));
                draw->AddCircle(center, packet_marker_size * 0.5f, IM_COL32(142, 105, 20, 230), 0, 1.2f);
            } else {
                draw->AddRectFilled(min, max, IM_COL32(255, 177, 184, 255), 2.0f);
                draw->AddRect(min, max, IM_COL32(160, 63, 73, 230), 2.0f, 0, 1.2f);
                const char* warning_text = "!";
                const ImVec2 warning_size = ImGui::CalcTextSize(warning_text);
                draw->AddText(
                    ImVec2(center.x - warning_size.x * 0.5f, center.y - warning_size.y * 0.5f - 0.5f),
                    IM_COL32(105, 28, 36, 255),
                    warning_text);
            }
        } else {
            draw->AddRectFilled(min, max, IM_COL32(51, 177, 103, 255), 2.0f);
            draw->AddRectFilled(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(205, 255, 222, 210), 1.0f);
        }
    } else if (event.message == "client_input" || event.message == "server_update") {
        const bool server_update = event.message == "server_update";
        const ImU32 fill = server_update ? IM_COL32(246, 248, 252, 255) : IM_COL32(53, 132, 246, 255);
        const ImU32 border = server_update ? IM_COL32(180, 190, 205, 255) : IM_COL32(141, 190, 255, 230);
        const ImU32 text = server_update ? IM_COL32(32, 38, 48, 255) : IM_COL32(250, 252, 255, 255);
        draw->AddRectFilled(min, max, fill, 3.0f);
        draw->AddRect(min, max, border, 3.0f, 0, 1.1f);
        const std::string label = packet_marker_label(event);
        const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
        draw->AddText(
            ImVec2(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f - 0.5f),
            text,
            label.c_str());
    } else {
        draw->AddRectFilled(min, max, IM_COL32(53, 132, 246, 255), 2.0f);
    }
    if (hover_alpha > 0.0f && !selected) {
        draw->AddRectFilled(
            ImVec2(min.x - 4.0f, min.y - 4.0f),
            ImVec2(max.x + 4.0f, max.y + 4.0f),
            color_with_alpha(IM_COL32(205, 226, 255, 82), hover_alpha),
            4.0f);
        draw->AddRect(
            ImVec2(min.x - 2.0f, min.y - 2.0f),
            ImVec2(max.x + 2.0f, max.y + 2.0f),
            color_with_alpha(IM_COL32(235, 245, 255, 220), hover_alpha),
            3.0f,
            0,
            1.2f);
    }
    if (selected) {
        draw->AddRect(ImVec2(min.x - 4.0f, min.y - 4.0f), ImVec2(max.x + 4.0f, max.y + 4.0f), IM_COL32(255, 218, 112, 255), 3.0f, 0, 2.6f);
        draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(255, 246, 205, 255), 2.0f, 0, 1.4f);
    } else if (highlighted) {
        draw->AddRect(ImVec2(min.x - 1.5f, min.y - 1.5f), ImVec2(max.x + 1.5f, max.y + 1.5f), IM_COL32(255, 218, 112, 155), 2.0f, 0, 1.2f);
    }
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (event.message == "client_input") {
            ImGui::BeginTooltip();
            ImGui::SetWindowFontScale(1.25f);
            ImGui::Text("Local frame %u", local_frame);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
            ImGui::Text("Input frames %s", event.input_frames.empty() ? "unknown" : event.input_frames.c_str());
            ImGui::Text(
                "%s %s at %.3fms",
                event.send ? "sent" : "received",
                event.message.c_str(),
                static_cast<double>(event.relative_us) / 1000.0);
            ImGui::EndTooltip();
        } else if (event.message == "server_update") {
            const std::string server_frame = event.server_frame.empty()
                ? std::to_string(event.frame)
                : event.server_frame;
            ImGui::BeginTooltip();
            ImGui::SetWindowFontScale(1.25f);
            ImGui::Text("Local frame %u", local_frame);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
            ImGui::Text("Server frame %s", server_frame.c_str());
            if (packet_update_apply_failed(event)) {
                ImGui::TextColored(ImVec4(0.98f, 0.52f, 0.56f, 1.0f), "Applied false");
                if (!event.apply_failure.empty()) {
                    ImGui::Text("Reason %s", event.apply_failure.c_str());
                }
            }
            ImGui::Text(
                "%s %s at %.3fms",
                event.send ? "sent" : "received",
                event.message.c_str(),
                static_cast<double>(event.relative_us) / 1000.0);
            ImGui::EndTooltip();
        } else {
            ImGui::SetTooltip(
                "%s %s at %.3fms",
                event.send ? "sent" : "received",
                event.message.c_str(),
                static_cast<double>(event.relative_us) / 1000.0);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            select_packet_event(state, event);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            select_packet_event(state, event);
            ImGui::OpenPopup(popup_id.c_str());
        }
    }
    if (ImGui::BeginPopup(popup_id.c_str())) {
        if (ImGui::MenuItem("See in frame viewer")) {
            jump_selected_packet_to_frame(state);
        }
        if (ImGui::MenuItem("See in bandwidth viewer")) {
            jump_selected_packet_to_payload(state);
        }
        ImGui::EndPopup();
    }
}

void draw_packet_frame_markers(
    ImDrawList* draw,
    const std::vector<PacketFrameMarker>& markers,
    const ImVec2& origin,
    float scroll_y,
    float column_x,
    float column_width,
    bool label_on_right,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    for (const PacketFrameMarker& marker : markers) {
        const float y = packet_event_y(origin, scroll_y, marker.relative_us);
        if (y < clip_min.y || y > clip_max.y) {
            continue;
        }
        draw->AddLine(
            ImVec2(column_x + 8.0f, y),
            ImVec2(column_x + column_width - 8.0f, y),
            IM_COL32(230, 238, 255, 80),
            1.0f);
        draw->AddCircleFilled(ImVec2(column_x + 8.0f, y), 2.0f, IM_COL32(230, 238, 255, 130));
        if ((marker.frame % 5U) == 0U) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "f%u", marker.frame);
            const ImVec2 text_size = ImGui::CalcTextSize(buffer);
            const float label_x = label_on_right
                ? column_x + column_width - 12.0f - text_size.x
                : column_x + 12.0f;
            draw->AddText(ImVec2(label_x, y + 2.0f), IM_COL32(166, 177, 195, 180), buffer);
        }
    }
}

void render_event_log(ViewerState& state) {
    const auto start = Clock::now();
    if (state.packet_clients.empty()) {
        std::size_t total_records = 0;
        for (const KTraceSourceHistory& source : state.history.sources) {
            total_records += source.records.size();
        }
        ImGui::BeginChild("packet_event_log", ImVec2(0.0f, -180.0f), true);
        ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "No packet events in this trace.");
        ImGui::TextColored(
            ImVec4(0.72f, 0.78f, 0.86f, 1.0f),
            "Packet logging is enabled for the trace files, but %zu loaded records contain zero packet-log records.",
            total_records);
        ImGui::TextColored(
            ImVec4(0.72f, 0.78f, 0.86f, 1.0f),
            "Regenerate the trace with the rebuilt plugin if this run was captured before packet tracing was compiled in.");
        ImGui::EndChild();
        state.current_timing.timeline_ms += elapsed_ms(start);
        return;
    }
    state.selected_packet_client = std::clamp(
        state.selected_packet_client,
        0,
        static_cast<int>(state.packet_clients.size()) - 1);
    if (ImGui::BeginTabBar("packet_clients")) {
        for (int i = 0; i < static_cast<int>(state.packet_clients.size()); ++i) {
            const std::string label = "client " + std::to_string(state.packet_clients[static_cast<std::size_t>(i)].client);
            if (ImGui::BeginTabItem(label.c_str())) {
                state.selected_packet_client = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    const PacketClientTimeline& client_timeline =
        state.packet_clients[static_cast<std::size_t>(state.selected_packet_client)];
    const std::uint64_t duration_us = state.packet_log_max_us >= state.packet_log_min_us
        ? state.packet_log_max_us - state.packet_log_min_us
        : 0U;
    const float width = packet_column_width * 2.0f + packet_column_gap + 80.0f;
    const float height = timeline_header_height + 72.0f + static_cast<float>(duration_us) * packet_time_us_pitch;

    ImGui::BeginChild("packet_event_log", ImVec2(0.0f, -180.0f), true);
    float scroll_y = ImGui::GetScrollY();
    const float max_scroll_y = std::max(0.0f, height - ImGui::GetWindowHeight());
    if (state.scroll_selected_packet_into_view) {
        const int selected_event = selected_packet_event_index(state);
        if (selected_event >= 0 &&
            std::find(client_timeline.event_indices.begin(), client_timeline.event_indices.end(), selected_event) != client_timeline.event_indices.end()) {
            const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(selected_event)];
            const float target_y =
                timeline_header_height + 32.0f + static_cast<float>(event.relative_us) * packet_time_us_pitch -
                ImGui::GetWindowHeight() * 0.5f;
            scroll_y = std::clamp(target_y, 0.0f, max_scroll_y);
            ImGui::SetScrollY(scroll_y);
            state.scroll_selected_packet_into_view = false;
        }
    }
    if (scroll_y > max_scroll_y) {
        scroll_y = max_scroll_y;
        ImGui::SetScrollY(scroll_y);
    }
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 origin(cursor.x, cursor.y + scroll_y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 clip_min = draw->GetClipRectMin();
    const ImVec2 clip_max = draw->GetClipRectMax();
    const float server_x = origin.x + 20.0f;
    const float client_x = server_x + packet_column_width + packet_column_gap;

    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + timeline_header_height), IM_COL32(22, 26, 32, 255));
    draw->AddRectFilled(ImVec2(server_x, origin.y + timeline_header_height), ImVec2(server_x + packet_column_width, origin.y + height), IM_COL32(26, 30, 37, 150));
    draw->AddRectFilled(ImVec2(client_x, origin.y + timeline_header_height), ImVec2(client_x + packet_column_width, origin.y + height), IM_COL32(26, 30, 37, 150));
    draw->AddText(ImVec2(server_x + 10.0f, origin.y + 5.0f), IM_COL32(238, 242, 247, 255), "server");
    const std::string client_title = "client " + std::to_string(client_timeline.client);
    draw->AddText(ImVec2(client_x + 10.0f, origin.y + 5.0f), IM_COL32(238, 242, 247, 255), client_title.c_str());

    const float first_visible_us =
        (clip_min.y - origin.y - timeline_header_height - 32.0f + scroll_y) / packet_time_us_pitch;
    const float last_visible_us =
        (clip_max.y - origin.y - timeline_header_height - 32.0f + scroll_y) / packet_time_us_pitch;
    const int first_tick = std::max(0, static_cast<int>(std::floor(first_visible_us / 100000.0f)) - 1);
    const int last_tick = std::max(first_tick, static_cast<int>(std::ceil(last_visible_us / 100000.0f)) + 1);
    for (int tick = first_tick; tick <= last_tick; ++tick) {
        const float us = static_cast<float>(tick) * 100000.0f;
        const float y = packet_event_y(origin, scroll_y, static_cast<std::uint64_t>(us));
        if (y < clip_min.y || y > clip_max.y) {
            continue;
        }
        draw->AddLine(ImVec2(server_x, y), ImVec2(client_x + packet_column_width, y), IM_COL32(78, 91, 112, 90), 1.0f);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1fs", us / 1000000.0f);
        draw->AddText(ImVec2(origin.x + width - 56.0f, y - 8.0f), IM_COL32(154, 166, 184, 255), buffer);
    }
    draw_packet_frame_markers(
        draw,
        client_timeline.server_frames,
        origin,
        scroll_y,
        server_x,
        packet_column_width,
        false,
        clip_min,
        clip_max);
    draw_packet_frame_markers(
        draw,
        client_timeline.client_frames,
        origin,
        scroll_y,
        client_x,
        packet_column_width,
        true,
        clip_min,
        clip_max);

    int hovered_event_index = -1;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        for (int event_index : client_timeline.event_indices) {
            if (event_index < 0 || event_index >= static_cast<int>(state.packet_events.size())) {
                continue;
            }
            const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
            const float column_x = event.server_side ? server_x : client_x;
            const ImVec2 center(
                packet_event_x(column_x, event),
                packet_event_y(origin, scroll_y, event.relative_us));
            if (center.y + packet_marker_size < clip_min.y || center.y - packet_marker_size > clip_max.y) {
                continue;
            }
            if (packet_event_hit(event, center, mouse)) {
                hovered_event_index = event_index;
            }
        }
    }
    const int selected_event_index = selected_packet_event_index(state);

    int drawn_cells = 0;
    for (int flow_index : client_timeline.flow_indices) {
        if (flow_index < 0 || flow_index >= static_cast<int>(state.packet_flows.size())) {
            continue;
        }
        const PacketFlow& flow = state.packet_flows[static_cast<std::size_t>(flow_index)];
        if (flow.send_event < 0 || flow.send_event >= static_cast<int>(state.packet_events.size())) {
            continue;
        }
        const PacketEventInfo& send = state.packet_events[static_cast<std::size_t>(flow.send_event)];
        const float send_column = send.server_side ? server_x : client_x;
        const ImVec2 send_point(
            packet_event_x(send_column, send),
            packet_event_y(origin, scroll_y, send.relative_us));
        ImVec2 receive_point = send_point;
        if (flow.receive_event >= 0 && flow.receive_event < static_cast<int>(state.packet_events.size())) {
            const PacketEventInfo& receive = state.packet_events[static_cast<std::size_t>(flow.receive_event)];
            const float receive_column = receive.server_side ? server_x : client_x;
            receive_point = ImVec2(
                packet_event_x(receive_column, receive),
                packet_event_y(origin, scroll_y, receive.relative_us));
        } else {
            receive_point.x = send.server_side ? client_x + 28.0f : server_x + packet_column_width - 42.0f;
        }
        if (line_visible(send_point, receive_point, clip_min, clip_max)) {
            const bool active_flow = packet_flow_active(state, flow_index, hovered_event_index, selected_event_index);
            const ImU32 line_color = active_flow ? IM_COL32(255, 210, 92, 255) : IM_COL32(194, 200, 210, 145);
            const ImU32 arrow_color = active_flow ? IM_COL32(255, 210, 92, 255) : IM_COL32(194, 200, 210, 160);
            draw->AddLine(send_point, receive_point, line_color, active_flow ? 3.0f : 1.4f);
            const float arrow_dir = receive_point.x >= send_point.x ? 1.0f : -1.0f;
            draw->AddTriangleFilled(
                receive_point,
                ImVec2(receive_point.x - arrow_dir * 8.0f, receive_point.y - 4.0f),
                ImVec2(receive_point.x - arrow_dir * 8.0f, receive_point.y + 4.0f),
                arrow_color);
        }
    }
    for (int event_index : client_timeline.event_indices) {
        if (event_index < 0 || event_index >= static_cast<int>(state.packet_events.size())) {
            continue;
        }
        const PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
        const float column_x = event.server_side ? server_x : client_x;
        const ImVec2 center(
            packet_event_x(column_x, event),
            packet_event_y(origin, scroll_y, event.relative_us));
        const std::vector<PacketFrameMarker>& local_markers =
            event.server_side ? client_timeline.server_frames : client_timeline.client_frames;
        const SyncFrame local_frame = packet_local_frame_from_markers(event, local_markers);
        bool highlighted_connected = false;
        if (event.flow_index >= 0 && event.flow_index < static_cast<int>(state.packet_flows.size())) {
            const PacketFlow& flow = state.packet_flows[static_cast<std::size_t>(event.flow_index)];
            highlighted_connected = (flow.send_event == event_index || flow.receive_event == event_index) &&
                packet_flow_active(state, event.flow_index, hovered_event_index, selected_event_index);
        }
        draw_packet_marker(
            state,
            draw,
            event,
            local_frame,
            center,
            hovered_event_index == event_index,
            highlighted_connected,
            clip_min,
            clip_max);
        ++drawn_cells;
    }
    ImGui::Dummy(ImVec2(width, height));
    ImGui::EndChild();
    state.current_timing.timeline_ms += elapsed_ms(start);
    state.current_timing.visible_rows += 2;
    state.current_timing.visible_cells += drawn_cells;
}

void render_timeline(ViewerState& state, int source_index) {
    const auto start = Clock::now();
    if (source_index < 0 || source_index >= static_cast<int>(state.history.sources.size())) {
        return;
    }
    const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
    const SourceMetrics metrics = source_index < static_cast<int>(state.source_metrics.size())
        ? state.source_metrics[static_cast<std::size_t>(source_index)]
        : compute_source_metrics(source, state, source_index);
    const SyncFrame min_frame = metrics.min_frame;
    const SyncFrame max_frame = metrics.max_frame;
    const int rows = metrics.rows;
    const float width = label_width + static_cast<float>(max_frame - min_frame + 1U) * frame_pitch + 80.0f;
    const float height = timeline_header_height + static_cast<float>(rows) * row_height;

    ImGui::BeginChild("timeline", ImVec2(0.0f, -180.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    const float max_scroll_x = std::max(0.0f, width - ImGui::GetContentRegionAvail().x);
    const float max_scroll_y = std::max(0.0f, height - ImGui::GetWindowHeight());
    apply_benchmark_scroll(
        state,
        max_scroll_x,
        max_scroll_y);
    if (state.scroll_selected_frame_into_view && state.selected.source_index == source_index) {
        const std::vector<TimelineNavCell> cells = collect_timeline_nav_cells(state, source, source_index);
        const auto found = std::find_if(cells.begin(), cells.end(), [&](const TimelineNavCell& cell) {
            return cell.selected == state.selected;
        });
        if (found != cells.end()) {
            const float target_x =
                label_width + static_cast<float>(state.selected.frame - min_frame) * frame_pitch -
                ImGui::GetContentRegionAvail().x * 0.5f;
            const float target_y =
                timeline_header_height + static_cast<float>(found->row) * row_height -
                ImGui::GetWindowHeight() * 0.5f;
            ImGui::SetScrollX(std::clamp(target_x, 0.0f, max_scroll_x));
            ImGui::SetScrollY(std::clamp(target_y, 0.0f, max_scroll_y));
        }
        state.scroll_selected_frame_into_view = false;
    }
    const float scroll_x = ImGui::GetScrollX();
    float scroll_y = ImGui::GetScrollY();
    if (scroll_y > max_scroll_y) {
        scroll_y = max_scroll_y;
        ImGui::SetScrollY(scroll_y);
    }
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 origin(cursor.x + scroll_x, cursor.y + scroll_y);
    const ImVec2 body_origin(origin.x, origin.y + timeline_header_height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 clip_min = draw->GetClipRectMin();
    const ImVec2 clip_max = draw->GetClipRectMax();
    const float sticky_label_x = clip_min.x;
    const float frame_clip_min_x = sticky_label_x + label_width;
    const auto row_visible = [&](int row_index) {
        const float y = body_origin.y + static_cast<float>(row_index) * row_height - scroll_y;
        return y + row_height >= clip_min.y && y <= clip_max.y;
    };
    const float first_visible_frame_value =
        (clip_min.x - origin.x - label_width + scroll_x) / frame_pitch;
    const float last_visible_frame_value =
        (clip_max.x - origin.x - label_width + scroll_x) / frame_pitch;
    const int first_visible_index = std::max(0, static_cast<int>(std::floor(first_visible_frame_value)) - 1);
    const int last_visible_index = std::max(0, static_cast<int>(std::ceil(last_visible_frame_value)) + 1);
    const SyncFrame first_visible_frame = min_frame + static_cast<SyncFrame>(first_visible_index);
    const SyncFrame last_visible_frame = std::min(max_frame, min_frame + static_cast<SyncFrame>(last_visible_index));
    const ImVec2 mouse = ImGui::GetMousePos();
    ProximityHighlight proximity;
    proximity.mouse = mouse;
    proximity.inner_radius = pill_width * 0.75f;
    proximity.outer_radius = frame_pitch * 2.5f;
    proximity.inner_radius_sq = proximity.inner_radius * proximity.inner_radius;
    proximity.outer_radius_sq = proximity.outer_radius * proximity.outer_radius;
    proximity.active =
        mouse.x >= frame_clip_min_x &&
        mouse.x <= clip_max.x &&
        mouse.y >= origin.y &&
        mouse.y <= clip_max.y;
    if (proximity.active) {
        const float center_base_x = origin.x + label_width - scroll_x + pill_width * 0.5f;
        const int max_frame_index = static_cast<int>(max_frame - min_frame);
        const int first_proximity_index = std::clamp(
            static_cast<int>(std::floor((mouse.x - proximity.outer_radius - center_base_x) / frame_pitch)),
            0,
            max_frame_index);
        const int last_proximity_index = std::clamp(
            static_cast<int>(std::ceil((mouse.x + proximity.outer_radius - center_base_x) / frame_pitch)),
            0,
            max_frame_index);
        proximity.first_frame = min_frame + static_cast<SyncFrame>(first_proximity_index);
        proximity.last_frame = min_frame + static_cast<SyncFrame>(last_proximity_index);
    }
    int drawn_rows = 0;
    int drawn_cells = 0;
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + timeline_header_height), IM_COL32(22, 26, 32, 255));
    draw->AddRectFilled(ImVec2(sticky_label_x, origin.y), ImVec2(sticky_label_x + label_width, origin.y + height), IM_COL32(26, 30, 37, 255));
    draw->AddLine(ImVec2(frame_clip_min_x, origin.y), ImVec2(frame_clip_min_x, origin.y + height), IM_COL32(70, 80, 96, 210), 1.0f);
    draw->AddText(ImVec2(sticky_label_x + 8.0f, origin.y + 5.0f), IM_COL32(151, 163, 180, 255), source_label(source).c_str());

    const SyncFrame header_first_frame = min_frame + static_cast<SyncFrame>(first_visible_index);
    const SyncFrame header_last_frame = std::min(max_frame, min_frame + static_cast<SyncFrame>(last_visible_index));
    const bool selected_frame_visible = state.selected.source_index == source_index &&
        state.selected.frame >= header_first_frame &&
        state.selected.frame <= header_last_frame;
    const float selected_frame_x = selected_frame_visible
        ? origin.x + label_width + static_cast<float>(state.selected.frame - min_frame) * frame_pitch - scroll_x
        : 0.0f;
    const float selected_frame_center_x = selected_frame_x + pill_width * 0.5f;

    draw->PushClipRect(ImVec2(frame_clip_min_x, clip_min.y), clip_max, true);
    if (selected_frame_visible) {
        draw->AddRectFilled(
            ImVec2(selected_frame_x, origin.y),
            ImVec2(selected_frame_x + frame_pitch, origin.y + timeline_header_height),
            IM_COL32(255, 230, 155, 34));
    }
    for (SyncFrame frame = header_first_frame; frame <= header_last_frame; ++frame) {
        const float x = origin.x + label_width + static_cast<float>(frame - min_frame) * frame_pitch - scroll_x;
        const bool striped = ((frame - min_frame) & 1U) != 0U;
        if (striped) {
            draw->AddRectFilled(
                ImVec2(x, origin.y + timeline_header_height),
                ImVec2(x + frame_pitch, origin.y + height),
                IM_COL32(255, 255, 255, 12));
        }
        const bool labeled = (frame % 10U) == 0U;
        if (labeled) {
            const float center_x = x + pill_width * 0.5f;
            draw->AddLine(
                ImVec2(center_x, origin.y + timeline_header_height),
                ImVec2(center_x, origin.y + height),
                IM_COL32(78, 91, 112, 130),
                1.0f);
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%u", frame);
            const ImVec2 text_size = ImGui::CalcTextSize(buffer);
            draw->AddText(ImVec2(center_x - text_size.x * 0.5f, origin.y + 5.0f), IM_COL32(154, 166, 184, 255), buffer);
        }
    }
    if (selected_frame_visible) {
        draw->AddLine(
            ImVec2(selected_frame_center_x, origin.y),
            ImVec2(selected_frame_center_x, origin.y + timeline_header_height),
            IM_COL32(255, 230, 155, 80),
            1.0f);
    }
    draw->PopClipRect();

    const auto draw_row_background = [&](int row_index, bool selected_row = false) {
        const float y = body_origin.y + static_cast<float>(row_index) * row_height - scroll_y;
        const ImU32 bg = (row_index % 2) == 0 ? IM_COL32(15, 18, 23, 55) : IM_COL32(255, 255, 255, 8);
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + row_height), bg);
        draw->AddRectFilled(ImVec2(sticky_label_x, y), ImVec2(sticky_label_x + label_width, y + row_height), IM_COL32(26, 30, 37, 235));
        if (selected_row) {
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + row_height), IM_COL32(255, 230, 155, 30));
            draw->AddRectFilled(
                ImVec2(sticky_label_x, y),
                ImVec2(sticky_label_x + label_width, y + row_height),
                IM_COL32(255, 230, 155, 38));
        }
        if (selected_frame_visible) {
            draw->AddRectFilled(
                ImVec2(selected_frame_x, y),
                ImVec2(selected_frame_x + frame_pitch, y + row_height),
                IM_COL32(255, 230, 155, 30));
            draw->AddLine(
                ImVec2(selected_frame_center_x, y),
                ImVec2(selected_frame_center_x, y + row_height),
                IM_COL32(255, 230, 155, 70),
                1.0f);
        }
    };

    int row = 0;
    for (const KTraceEntityRow& entity : source.entities) {
        const bool expanded_entity = entity_expanded(state, source_index, entity);
        if (row_visible(row)) {
            const bool selected_row = state.selected.source_index == source_index &&
                state.selected.network_id == entity.client_network_id &&
                !state.selected.component &&
                state.selected.run == 0;
            draw_row_background(row, selected_row);
            const float label_y = body_origin.y + static_cast<float>(row) * row_height - scroll_y + 4.0f;
            const std::string label = entity_label(entity);
            draw_entity_toggle(draw, state, source_index, entity, ImVec2(sticky_label_x + 5.0f, label_y - 1.0f));
            draw->AddText(ImVec2(sticky_label_x + 26.0f, label_y), IM_COL32(238, 242, 247, 255), label.c_str());
            const float label_x = sticky_label_x + 26.0f;
            const ImVec2 label_max(label_x + ImGui::CalcTextSize(label.c_str()).x, label_y + ImGui::GetTextLineHeight());
            if (mouse_in_rect(ImVec2(label_x, label_y), label_max)) {
                ImGui::SetTooltip("%s entity", expanded_entity ? "collapse" : "expand");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    toggle_entity_expanded(state, source_index, entity);
                }
            }
            if (!expanded_entity) {
                draw->PushClipRect(ImVec2(frame_clip_min_x, clip_min.y), clip_max, true);
                drawn_cells += draw_entity_summary_cells(
                    draw,
                    source,
                    entity,
                    body_origin,
                    scroll_x,
                    scroll_y,
                    min_frame,
                    first_visible_frame,
                    last_visible_frame,
                    row,
                    source_index,
                    state,
                    proximity);
                draw->PopClipRect();
            }
            ++drawn_rows;
        }
        ++row;
        if (!expanded_entity) {
            continue;
        }
        for (const KTraceComponentRow& component : entity.components) {
            const bool collapsible_component = component_has_resim(component);
            const bool expanded_component =
                !collapsible_component || component_expanded(state, source_index, entity, component.component);
            if (row_visible(row)) {
                const bool selected_row = state.selected.source_index == source_index &&
                    state.selected.network_id == entity.client_network_id &&
                    state.selected.component == component.component &&
                    state.selected.run == 0;
                draw_row_background(row, selected_row);
                const float y = body_origin.y + static_cast<float>(row) * row_height - scroll_y + 4.0f;
                const std::string label = "  " + component_label(source, component.component);
                draw_component_toggle(draw, state, source_index, entity, component.component, ImVec2(sticky_label_x + 20.0f, y - 1.0f));
                draw->AddText(ImVec2(sticky_label_x + 42.0f, y), IM_COL32(186, 196, 210, 255), label.c_str());
                const float label_x = sticky_label_x + 42.0f;
                const ImVec2 label_max(label_x + ImGui::CalcTextSize(label.c_str()).x, y + ImGui::GetTextLineHeight());
                if (collapsible_component && mouse_in_rect(ImVec2(label_x, y), label_max)) {
                    ImGui::SetTooltip("%s component", expanded_component ? "collapse" : "expand");
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        toggle_component_expanded(state, source_index, entity, component.component);
                    }
                }
                ++drawn_rows;
                if (expanded_component) {
                    draw->PushClipRect(ImVec2(frame_clip_min_x, clip_min.y), clip_max, true);
                    if (!component.runs.empty()) {
                        draw_consecutive_cell_links(
                            draw,
                            component.runs[0],
                            body_origin,
                            scroll_x,
                            scroll_y,
                            min_frame,
                            first_visible_frame,
                            last_visible_frame,
                            row,
                            clip_min,
                            clip_max);
                        const KTraceFrameRun& root = component.runs[0];
                        auto cell_it = std::lower_bound(
                            root.frames.begin(),
                            root.frames.end(),
                            first_visible_frame,
                            [](const KTraceFrameCell& cell, SyncFrame frame) {
                                return cell.frame < frame;
                            });
                        for (; cell_it != root.frames.end(); ++cell_it) {
                            const KTraceFrameCell& cell = *cell_it;
                            if (cell.frame > last_visible_frame) {
                                break;
                            }
                            draw_cell(draw, source, cell, body_origin, scroll_x, scroll_y, min_frame, row, entity, component.component, 0, source_index, state, proximity);
                            ++drawn_cells;
                        }
                    }
                    draw->PopClipRect();
                } else {
                    draw->PushClipRect(ImVec2(frame_clip_min_x, clip_min.y), clip_max, true);
                    drawn_cells += draw_component_summary_cells(
                        draw,
                        source,
                        entity,
                        component,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        first_visible_frame,
                        last_visible_frame,
                        row,
                        source_index,
                        state,
                        proximity);
                    draw->PopClipRect();
                }
            }
            ++row;
            if (!expanded_component) {
                continue;
            }
            if (!component_has_run_lanes(component)) {
                continue;
            }
            const RunLaneLayout& run_layout = get_run_lane_layout(state, source_index, entity, component);
            const int run_base_row = row;
            for (int lane = 0; lane < run_layout.lane_count; ++lane) {
                const int run_row = run_base_row + lane;
                if (row_visible(run_row)) {
                    bool selected_row = false;
                    if (state.selected.source_index == source_index &&
                        state.selected.network_id == entity.client_network_id &&
                        state.selected.component == component.component &&
                        state.selected.run != 0) {
                        for (const RunRenderItem& item : run_layout.items) {
                            if (item.run_id == state.selected.run &&
                                run_lane_at_frame(item, state.selected.frame) == lane) {
                                selected_row = true;
                                break;
                            }
                        }
                    }
                    draw_row_background(run_row, selected_row);
                    const float y = body_origin.y + static_cast<float>(run_row) * row_height - scroll_y + 4.0f;
                    const std::string label = "    run lane " + std::to_string(lane + 1);
                    draw->AddText(ImVec2(sticky_label_x + 32.0f, y), IM_COL32(237, 183, 112, 255), label.c_str());
                    ++drawn_rows;
                }
            }
            draw->PushClipRect(ImVec2(frame_clip_min_x, clip_min.y), clip_max, true);
            for (std::size_t run_index = 0; run_index < run_layout.items.size(); ++run_index) {
                const RunRenderItem& item = run_layout.items[run_index];
                if (item.run_id >= component.runs.size()) {
                    continue;
                }
                const KTraceFrameRun& run = component.runs[item.run_id];
                const int parent_row = item.parent_lane < 0 ? run_base_row - 1 : run_base_row + item.parent_lane;
                const int start_lane = run_lane_at_frame(item, item.start_frame);
                if (start_lane != unassigned_lane) {
                    draw_run_drop_link(
                        draw,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        item.start_frame,
                        parent_row,
                        run_base_row + start_lane,
                        clip_min,
                        clip_max);
                }
                draw_run_links(
                    draw,
                    run,
                    item,
                    body_origin,
                    scroll_x,
                    scroll_y,
                    min_frame,
                    first_visible_frame,
                    last_visible_frame,
                    run_base_row,
                    clip_min,
                    clip_max);
                if (item.end_frame < first_visible_frame || item.start_frame > last_visible_frame) {
                    continue;
                }
                auto cell_it = std::lower_bound(
                    run.frames.begin(),
                    run.frames.end(),
                    first_visible_frame,
                    [](const KTraceFrameCell& cell, SyncFrame frame) {
                        return cell.frame < frame;
                    });
                for (; cell_it != run.frames.end(); ++cell_it) {
                    const KTraceFrameCell& cell = *cell_it;
                    if (cell.frame > last_visible_frame) {
                        break;
                    }
                    const int cell_lane = run_lane_at_frame(item, cell.frame);
                    if (cell_lane == unassigned_lane) {
                        continue;
                    }
                    const int cell_row = run_base_row + cell_lane;
                    if (!row_visible(cell_row)) {
                        continue;
                    }
                    draw_cell(
                        draw,
                        source,
                        cell,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        cell_row,
                        entity,
                        component.component,
                        item.run_id,
                        source_index,
                        state,
                        proximity);
                    ++drawn_cells;
                }
            }
            draw->PopClipRect();
            row += run_layout.lane_count;
        }
    }
    draw->AddLine(ImVec2(frame_clip_min_x, origin.y), ImVec2(frame_clip_min_x, origin.y + height), IM_COL32(70, 80, 96, 230), 1.0f);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::EndChild();
    state.current_timing.timeline_ms += elapsed_ms(start);
    state.current_timing.visible_rows += drawn_rows;
    state.current_timing.visible_cells += drawn_cells;
}

bool record_matches_selection(const KTraceRecord& record, const SelectedCell& selected) {
    const SyncTraceEvent& event = record.event;
    if (event.frame != selected.frame) {
        return false;
    }
    if (event.client_network_id != selected.network_id &&
        make_client_entity_network_id(event.client, event.wire_network_id, event.network_version) != selected.network_id &&
        event.server_entity.value != selected.network_id) {
        return false;
    }
    return !selected.component || event.component == selected.component || event.type == SyncTraceEventType::EntityDestroyed;
}

void select_benchmark_cell(ViewerState& state) {
    if (!state.benchmark.options.enabled || state.history.sources.empty() ||
        state.benchmark.frame_index % 30 != 0 || state.benchmark_candidates.empty()) {
        return;
    }
    const std::size_t index =
        static_cast<std::size_t>(state.benchmark.frame_index / 30) % state.benchmark_candidates.size();
    if (state.selected != state.benchmark_candidates[index]) {
        state.selected = state.benchmark_candidates[index];
        state.details_dirty = true;
    }
}

bool same_selected_cell(const SelectedCell& lhs, const SelectedCell& rhs) {
    return lhs == rhs;
}

void move_selection_to(ViewerState& state, const SelectedCell& selected) {
    if (state.selected != selected) {
        state.selected = selected;
        state.details_dirty = true;
    }
}

void handle_timeline_keyboard(ViewerState& state, int source_index) {
    if (state.selected.source_index != source_index || ImGui::GetIO().WantTextInput) {
        return;
    }
    const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if (!left && !right && !up && !down) {
        return;
    }

    const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(source_index)];
    const std::vector<TimelineNavCell> cells = collect_timeline_nav_cells(state, source, source_index);
    const auto current = std::find_if(cells.begin(), cells.end(), [&](const TimelineNavCell& cell) {
        return same_selected_cell(cell.selected, state.selected);
    });
    if (current == cells.end()) {
        return;
    }

    if (left || right) {
        const int direction = right ? 1 : -1;
        auto best = cells.end();
        for (auto it = cells.begin(); it != cells.end(); ++it) {
            if (it->row != current->row) {
                continue;
            }
            if (direction < 0 && it->selected.frame < current->selected.frame &&
                (best == cells.end() || it->selected.frame > best->selected.frame)) {
                best = it;
            }
            if (direction > 0 && it->selected.frame > current->selected.frame &&
                (best == cells.end() || it->selected.frame < best->selected.frame)) {
                best = it;
            }
        }
        if (best != cells.end()) {
            move_selection_to(state, best->selected);
        }
        return;
    }

    const int direction = down ? 1 : -1;
    int target_row = current->row + direction;
    while (target_row >= 0) {
        auto best = cells.end();
        int best_distance = std::numeric_limits<int>::max();
        bool row_exists = false;
        for (auto it = cells.begin(); it != cells.end(); ++it) {
            if (it->row != target_row) {
                continue;
            }
            row_exists = true;
            const int distance = std::abs(static_cast<int>(it->selected.frame) - static_cast<int>(current->selected.frame));
            if (distance < best_distance ||
                (distance == best_distance && best != cells.end() && it->selected.frame < best->selected.frame)) {
                best = it;
                best_distance = distance;
            }
        }
        if (best != cells.end()) {
            move_selection_to(state, best->selected);
            return;
        }
        if (!row_exists && (direction < 0 || target_row > current->row + 10000)) {
            return;
        }
        target_row += direction;
        if (target_row > current->row + 10000) {
            return;
        }
    }
}

std::string detail_component_label(const KTraceSourceHistory& source, const SyncTraceEvent& event) {
    if (!event.component_name.empty()) {
        return event.component_name;
    }
    return component_label(source, event.component);
}

std::string detail_frame_component_label(const KTraceRecord& record, const KTraceSourceHistory& source) {
    const SyncTraceEvent& event = record.event;
    if (event.role == SyncTraceRole::Server) {
        if (event.client != invalid_client_id) {
            return "server " + detail_component_label(source, event) + " input received";
        }
        return "server " + detail_component_label(source, event);
    }
    std::string label = "client " + detail_component_label(source, event) + " ";
    switch (event.mode) {
    case ReplicationClientMode::Predict:
        if (event.type == SyncTraceEventType::ResimulatedFrameComponent) {
            label += "resimulated";
            return label;
        }
        label += "predicted";
        return label;
    case ReplicationClientMode::BufferedInterpolation:
        label += "interpolated";
        return label;
    case ReplicationClientMode::Snap:
        label += "snapped";
        return label;
    }
    label += "sampled";
    return label;
}

std::string detail_received_component_label(const SyncTraceEvent& event, const KTraceSourceHistory& source) {
    return "server " + detail_component_label(source, event) + " received";
}

const char* detail_source_label(const KTraceRecord& record, const char* source) {
    const SyncTraceEvent& event = record.event;
    if (is_cue_event(event.type)) {
        return event.role == SyncTraceRole::Server ? "server" : "client";
    }
    if (event.role == SyncTraceRole::Server) {
        return "server";
    }
    if (event.type == SyncTraceEventType::PacketLog) {
        return source;
    }
    switch (event.mode) {
    case ReplicationClientMode::Predict:
        return "client predicted";
    case ReplicationClientMode::BufferedInterpolation:
        return "client interpolated";
    case ReplicationClientMode::Snap:
        return "client snapped";
    }
    return source;
}

bool is_frame_component_detail(const SyncTraceEvent& event) {
    return event.type == SyncTraceEventType::FrameComponent ||
        event.type == SyncTraceEventType::ResimulatedFrameComponent;
}

bool is_received_component_detail(const SyncTraceEvent& event) {
    return event.type == SyncTraceEventType::ComponentReceived;
}

bool is_rollback_conflict_detail(const SyncTraceEvent& event) {
    return event.type == SyncTraceEventType::PredictionRollbackConflict;
}

bool is_rollback_reason_detail(const SyncTraceEvent& event) {
    return event.type == SyncTraceEventType::RollbackReason;
}

std::string cue_type_label(const SyncTraceEvent& event) {
    if (!event.component_name.empty()) {
        return event.component_name;
    }
    return "cue " + std::to_string(event.cue_type);
}

std::string cue_type_label(const SyncTraceEvent& event, const KTraceSourceHistory* source) {
    if (source != nullptr) {
        const auto found = source->cue_names.find(event.cue_type);
        if (found != source->cue_names.end() && !found->second.empty()) {
            return found->second;
        }
    }
    return cue_type_label(event);
}

std::string trace_data_field(const std::string& data, const std::string& key);
std::string trace_data_without_viewer_fields(const std::string& data);

struct CueDisplaySummary {
    const SyncTraceEvent* event = nullptr;
    const KTraceSourceHistory* source = nullptr;
    bool predicted = false;
    bool confirmed = false;
    bool rolled_back = false;
    std::string rollback_reason;
    std::string data;
};

void merge_cue_display_event(CueDisplaySummary& summary, const SelectedRecordDetail& detail) {
    if (detail.record == nullptr) {
        return;
    }
    const SyncTraceEvent& event = detail.record->event;
    if (!is_cue_event(event.type)) {
        return;
    }
    if (summary.event == nullptr ||
        event.type == SyncTraceEventType::CuePlayed ||
        (summary.event->type != SyncTraceEventType::CuePlayed && event.type == SyncTraceEventType::CueEmitted)) {
        summary.event = &event;
        summary.source = detail.source_history;
    }
    const std::string cue_source = trace_data_field(event.data, "source");
    summary.predicted = summary.predicted || cue_source == "local_prediction";
    summary.confirmed = summary.confirmed || event.type == SyncTraceEventType::CueConfirmed;
    summary.rolled_back = summary.rolled_back || event.type == SyncTraceEventType::CueRolledBack;
    const std::string rollback_reason = trace_data_field(event.data, "rollback_reason");
    if (!rollback_reason.empty()) {
        summary.rollback_reason = rollback_reason;
    }
    const std::string data = trace_data_without_viewer_fields(event.data);
    if (!data.empty() && summary.data.empty()) {
        summary.data = data;
    }
}

std::string cue_display_status(const CueDisplaySummary& summary) {
    if (summary.predicted) {
        if (summary.confirmed) {
            return "predicted, confirmed";
        }
        if (summary.rolled_back) {
            if (summary.rollback_reason == "resim_not_replayed") {
                return "predicted, rolled back from resim";
            }
            return "predicted, rolled back by server";
        }
        return "predicted";
    }
    return "from server";
}

void render_merged_cue_details(const std::vector<SelectedRecordDetail>& details) {
    std::vector<std::string> order;
    std::unordered_map<std::string, CueDisplaySummary> summaries;
    for (const SelectedRecordDetail& detail : details) {
        if (detail.record == nullptr || !is_cue_event(detail.record->event.type)) {
            continue;
        }
        const std::string key = cue_instance_key(detail.record->event);
        if (summaries.find(key) == summaries.end()) {
            order.push_back(key);
        }
        merge_cue_display_event(summaries[key], detail);
    }
    for (const std::string& key : order) {
        const auto found = summaries.find(key);
        if (found == summaries.end() || found->second.event == nullptr) {
            continue;
        }
        const CueDisplaySummary& summary = found->second;
        const std::string label = "cue played: " +
            cue_type_label(*summary.event, summary.source) +
            " (" + cue_display_status(summary) + ")";
        ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.86f, 1.0f), "%s", label.c_str());
        if (!summary.data.empty()) {
            ImGui::TextWrapped("  data: %s", summary.data.c_str());
        }
    }
}

std::string trace_data_field(const std::string& data, const std::string& key) {
    const std::string prefix = key + "=";
    std::size_t begin = 0;
    while (begin <= data.size()) {
        const std::size_t end = data.find(',', begin);
        const std::size_t token_end = end == std::string::npos ? data.size() : end;
        if (data.compare(begin, prefix.size(), prefix) == 0) {
            return data.substr(begin + prefix.size(), token_end - (begin + prefix.size()));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return {};
}

std::string trace_data_without_viewer_fields(const std::string& data) {
    std::string out;
    std::size_t begin = 0;
    while (begin <= data.size()) {
        const std::size_t end = data.find(',', begin);
        const std::size_t token_end = end == std::string::npos ? data.size() : end;
        const std::string token = data.substr(begin, token_end - begin);
        if (!token.empty() &&
            token.rfind("source=", 0) != 0 &&
            token.rfind("rollback_reason=", 0) != 0) {
            if (!out.empty()) {
                out += ",";
            }
            out += token;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return out;
}

void append_detail_line(std::string& out, const std::string& line = {}) {
    out += line;
    out += '\n';
}

void append_merged_cue_details_text(std::string& out, const std::vector<SelectedRecordDetail>& details) {
    std::vector<std::string> order;
    std::unordered_map<std::string, CueDisplaySummary> summaries;
    for (const SelectedRecordDetail& detail : details) {
        if (detail.record == nullptr || !is_cue_event(detail.record->event.type)) {
            continue;
        }
        const std::string key = cue_instance_key(detail.record->event);
        if (summaries.find(key) == summaries.end()) {
            order.push_back(key);
        }
        merge_cue_display_event(summaries[key], detail);
    }
    for (const std::string& key : order) {
        const auto found = summaries.find(key);
        if (found == summaries.end() || found->second.event == nullptr) {
            continue;
        }
        const CueDisplaySummary& summary = found->second;
        append_detail_line(
            out,
            "cue played: " + cue_type_label(*summary.event, summary.source) +
                " (" + cue_display_status(summary) + ")");
        if (!summary.data.empty()) {
            append_detail_line(out, "  data: " + summary.data);
        }
    }
}

void append_record_detail_text(
    std::string& out,
    const SelectedRecordDetail& detail,
    const std::vector<SelectedRecordDetail>* selection_details = nullptr) {
    if (detail.record == nullptr) {
        return;
    }
    const KTraceRecord& record = *detail.record;
    const SyncTraceEvent& event = record.event;
    if (is_cue_event(event.type)) {
        (void)selection_details;
        return;
    }
    if (is_rollback_conflict_detail(event)) {
        append_detail_line(out, "conflict triggered rollback");
        return;
    }
    if (is_rollback_reason_detail(event)) {
        if (!event.data.empty()) {
            append_detail_line(out, "  reason: " + event.data);
        }
        return;
    }
    if (detail.source_history != nullptr &&
        is_received_component_detail(event)) {
        append_detail_line(out, detail_received_component_label(event, *detail.source_history));
        if (!event.data.empty()) {
            append_detail_line(out, "  data: " + event.data);
        }
        return;
    }
    if (detail.source_history != nullptr &&
        is_frame_component_detail(event)) {
        append_detail_line(out, detail_frame_component_label(record, *detail.source_history));
        if (!event.data.empty()) {
            append_detail_line(out, "  data: " + event.data);
        }
        return;
    }
    append_detail_line(out, std::string(detail_source_label(record, detail.source.c_str())) + " " + event_name(event.type));
    if (!event.data.empty()) {
        append_detail_line(out, "  data: " + event.data);
    }
}

std::string build_record_details_text(const std::vector<SelectedRecordDetail>& details) {
    std::string out;
    append_merged_cue_details_text(out, details);
    for (const SelectedRecordDetail& detail : details) {
        append_record_detail_text(out, detail, &details);
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

void render_selectable_detail_text(const char* id, const std::string& text) {
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back('\0');
    ImGui::InputTextMultiline(
        id,
        buffer.data(),
        buffer.size(),
        ImVec2(-1.0f, -1.0f),
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
}

void render_record_detail(
    const SelectedRecordDetail& detail,
    const std::vector<SelectedRecordDetail>* selection_details = nullptr) {
    if (detail.record == nullptr) {
        return;
    }
    const KTraceRecord& record = *detail.record;
    const SyncTraceEvent& event = record.event;
    if (is_cue_event(event.type)) {
        (void)selection_details;
        return;
    }
    if (is_rollback_conflict_detail(event)) {
        ImGui::TextColored(
            ImVec4(0.72f, 0.78f, 0.86f, 1.0f),
            "conflict triggered rollback");
        return;
    }
    if (is_rollback_reason_detail(event)) {
        if (!event.data.empty()) {
            ImGui::TextWrapped("  reason: %s", event.data.c_str());
        }
        return;
    }
    if (detail.source_history != nullptr &&
        is_received_component_detail(event)) {
        const std::string label = detail_received_component_label(event, *detail.source_history);
        ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.86f, 1.0f), "%s", label.c_str());
        if (!event.data.empty()) {
            ImGui::TextWrapped("  data: %s", event.data.c_str());
        }
        return;
    }
    if (detail.source_history != nullptr &&
        is_frame_component_detail(event)) {
        const std::string label = detail_frame_component_label(record, *detail.source_history);
        ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.86f, 1.0f), "%s", label.c_str());
        if (!event.data.empty()) {
            ImGui::TextWrapped("  data: %s", event.data.c_str());
        }
        return;
    }
    ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.86f, 1.0f), "%s", detail_source_label(record, detail.source.c_str()));
    ImGui::SameLine();
    ImGui::TextUnformatted(event_name(event.type).c_str());
    if (!event.data.empty()) {
        ImGui::TextWrapped("  data: %s", event.data.c_str());
    }
}

void rebuild_detail_cache(ViewerState& state) {
    if (!state.details_dirty && state.cached_detail_selection == state.selected) {
        return;
    }
    const auto start = Clock::now();
    state.cached_details.clear();
    state.cached_detail_selection = state.selected;
    state.details_dirty = false;
    if (state.selected.source_index < 0 || state.selected.source_index >= static_cast<int>(state.history.sources.size())) {
        state.current_timing.selection_ms += elapsed_ms(start);
        return;
    }
    const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(state.selected.source_index)];
    const std::string label = source_label(source);
    if (!state.selected.event_indices.empty()) {
        for (std::uint32_t index : state.selected.event_indices) {
            if (index < source.records.size()) {
                const KTraceRecord& record = source.records[index];
                state.cached_details.push_back(SelectedRecordDetail{&record, &source, label});
            }
        }
    } else {
        for (const KTraceRecord& record : source.records) {
            if (record_matches_selection(record, state.selected)) {
                state.cached_details.push_back(SelectedRecordDetail{&record, &source, label});
            }
        }
    }
    std::stable_sort(
        state.cached_details.begin(),
        state.cached_details.end(),
        [](const SelectedRecordDetail& lhs, const SelectedRecordDetail& rhs) {
            const auto rank = [](const SelectedRecordDetail& detail) {
                if (detail.record == nullptr) {
                    return 3;
                }
                if (detail.record->event.type == SyncTraceEventType::PredictionRollbackConflict) {
                    return 0;
                }
                if (detail.record->event.type == SyncTraceEventType::RollbackReason) {
                    return 1;
                }
                return 2;
            };
            return rank(lhs) < rank(rhs);
        });
    state.current_timing.selection_ms += elapsed_ms(start);
}

void rebuild_packet_detail_cache(ViewerState& state) {
    if (!state.packet_details_dirty) {
        return;
    }
    const auto start = Clock::now();
    state.cached_details.clear();
    state.packet_details_dirty = false;
    const PacketEventInfo* event = selected_packet_event(state);
    if (event != nullptr &&
        event->source_index >= 0 &&
        event->source_index < static_cast<int>(state.history.sources.size())) {
        const KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(event->source_index)];
        if (event->record_index < source.records.size()) {
            state.cached_details.push_back(SelectedRecordDetail{&source.records[event->record_index], &source, source_label(source)});
        }
    }
    state.current_timing.selection_ms += elapsed_ms(start);
}

ImU32 payload_scope_color(const std::string& name) {
    const std::uint64_t hash = std::hash<std::string>{}(name);
    const float hue = static_cast<float>(hash % 360U) / 360.0f;
    const float saturation = 0.54f + static_cast<float>((hash >> 12U) % 24U) / 100.0f;
    const float value = 0.52f + static_cast<float>((hash >> 24U) % 22U) / 100.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, saturation, value, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

void set_payload_name_filter(ViewerState& state, const std::string& text) {
    state.payload_filter_enabled = true;
    std::snprintf(state.payload_filter.data(), state.payload_filter.size(), "%s", text.c_str());
    state.payload_view_dirty = true;
}

void render_payload_bandwidth_chart(ViewerState& state) {
    const float row_h = 48.0f;
    const float left_w = 92.0f;
    const float bucket_w = 14.0f;
    const float chart_h = 190.0f;
    struct PayloadLane {
        SyncTracePayloadSource source = SyncTracePayloadSource::Network;
        ClientId client = invalid_client_id;
    };
    struct StackedPayloadBucket {
        int lane_index = -1;
        std::uint64_t begin_us = 0;
        std::uint64_t end_us = 0;
        std::uint64_t incoming_bits = 0;
        std::uint64_t outgoing_bits = 0;
        std::uint64_t unknown_bits = 0;
        std::uint64_t packets = 0;
    };
    std::vector<PayloadLane> lanes;
    std::vector<StackedPayloadBucket> stacked_buckets;
    for (const PayloadBandwidthBucket& bucket : state.payload_buckets) {
        if (!payload_source_matches_filter(state.payload_source_filter, bucket.payload_source) ||
            !payload_direction_matches_filter(state.payload_direction_filter, bucket.payload_tag_bits)) {
            continue;
        }
        const PayloadLane lane{bucket.payload_source, bucket.client};
        auto lane_found = std::find_if(lanes.begin(), lanes.end(), [&](const PayloadLane& existing) {
            return existing.source == lane.source && existing.client == lane.client;
        });
        if (lane_found == lanes.end()) {
            lanes.push_back(lane);
            lane_found = lanes.end() - 1;
        }
        const int lane_index = static_cast<int>(std::distance(lanes.begin(), lane_found));
        auto stack_found = std::find_if(stacked_buckets.begin(), stacked_buckets.end(), [&](const StackedPayloadBucket& existing) {
            return existing.lane_index == lane_index && existing.begin_us == bucket.begin_us;
        });
        if (stack_found == stacked_buckets.end()) {
            StackedPayloadBucket stacked;
            stacked.lane_index = lane_index;
            stacked.begin_us = bucket.begin_us;
            stacked.end_us = bucket.end_us;
            stacked_buckets.push_back(stacked);
            stack_found = stacked_buckets.end() - 1;
        }
        if ((bucket.payload_tag_bits & sync_trace_payload_tag_incoming) != 0U) {
            stack_found->incoming_bits += bucket.total_bits;
        } else if ((bucket.payload_tag_bits & sync_trace_payload_tag_outgoing) != 0U) {
            stack_found->outgoing_bits += bucket.total_bits;
        } else {
            stack_found->unknown_bits += bucket.total_bits;
        }
        stack_found->packets += bucket.packets;
    }
    const std::uint64_t bucket_count = state.payload_bucket_us == 0U
        ? 1U
        : ((state.payload_max_us >= state.payload_min_us ? state.payload_max_us - state.payload_min_us : 0U) / state.payload_bucket_us) + 1U;
    const float width = left_w + std::max(1.0f, static_cast<float>(bucket_count) * bucket_w) + 32.0f;
    const float height = std::max(chart_h, 28.0f + static_cast<float>(lanes.size()) * row_h);
    const std::uint64_t max_bits = std::accumulate(
        stacked_buckets.begin(),
        stacked_buckets.end(),
        std::uint64_t{0},
        [](std::uint64_t value, const StackedPayloadBucket& bucket) {
            return std::max(value, bucket.incoming_bits + bucket.outgoing_bits + bucket.unknown_bits);
        });
    const PayloadSummary summary = payload_filtered_summary(state);
    const std::uint64_t summary_bps = bits_per_second(summary.total_bits, summary.duration_us);
    const std::string summary_text = "all clients " + format_compact_bits(summary.total_bits) +
        " | " + format_bps(summary_bps) +
        " | " + std::to_string(summary.packets) + " packets";

    ImGui::BeginChild("payload_bandwidth", ImVec2(0.0f, chart_h), true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 canvas = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##payload_bandwidth_canvas", ImVec2(width, height));
    const bool hovered_canvas = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas, ImVec2(canvas.x + width, canvas.y + height), IM_COL32(18, 22, 28, 255));
    draw->AddText(ImVec2(canvas.x + 8.0f, canvas.y + 7.0f), IM_COL32(160, 172, 190, 255), "bandwidth");
    draw->AddText(ImVec2(canvas.x + left_w, canvas.y + 7.0f), IM_COL32(220, 228, 240, 245), summary_text.c_str());

    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 clip_min = draw->GetClipRectMin();
    const ImVec2 clip_max = draw->GetClipRectMax();
    const float bandwidth_inner_radius = bucket_w * 1.25f;
    const float bandwidth_outer_radius = bucket_w * 4.0f;
    const float bandwidth_inner_radius_sq = bandwidth_inner_radius * bandwidth_inner_radius;
    const float bandwidth_outer_radius_sq = bandwidth_outer_radius * bandwidth_outer_radius;
    int hovered_bucket = -1;
    bool hovered_client_label = false;
    for (int lane_index = 0; lane_index < static_cast<int>(lanes.size()); ++lane_index) {
        const PayloadLane& lane = lanes[static_cast<std::size_t>(lane_index)];
        const std::string lane_label = payload_summary_owner_label(lane.source, lane.client);
        std::string lane_summary_text;
        std::uint64_t lane_bits = 0;
        std::uint64_t lane_packets = 0;
        for (const StackedPayloadBucket& bucket : stacked_buckets) {
            if (bucket.lane_index != lane_index) {
                continue;
            }
            lane_bits += bucket.incoming_bits + bucket.outgoing_bits + bucket.unknown_bits;
            lane_packets += bucket.packets;
        }
        if (lane_packets != 0U) {
            lane_summary_text = format_compact_bits(lane_bits);
            lane_summary_text += " | " + format_bps(bits_per_second(lane_bits, summary.duration_us));
            lane_summary_text += " | " + std::to_string(lane_packets) + " packets";
        }
        const float row_y = canvas.y + 28.0f + static_cast<float>(lane_index) * row_h;
        const bool excluded = payload_client_excluded(state, lane.source, lane.client);
        const ImVec2 label_min(canvas.x + 8.0f, row_y + 18.0f);
        const ImVec2 label_max(canvas.x + left_w - 6.0f, row_y + 38.0f);
        const bool label_hovered = mouse.x >= label_min.x && mouse.x <= label_max.x && mouse.y >= label_min.y && mouse.y <= label_max.y;
        hovered_client_label = hovered_client_label || label_hovered;
        if (label_hovered) {
            draw->AddRectFilled(label_min, label_max, excluded ? IM_COL32(115, 125, 145, 55) : IM_COL32(90, 130, 190, 65), 3.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s %s", excluded ? "Show" : "Hide", lane_label.c_str());
        }
        if (label_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const PayloadClientKey key{lane.source, lane.client};
            auto found = state.payload_excluded_clients.find(key);
            if (found == state.payload_excluded_clients.end()) {
                state.payload_excluded_clients.insert(key);
            } else {
                state.payload_excluded_clients.erase(found);
            }
            state.payload_view_dirty = true;
        }
        draw->AddText(
            ImVec2(canvas.x + 8.0f, row_y + 20.0f),
            excluded ? IM_COL32(155, 164, 180, 110) : IM_COL32(226, 234, 245, 235),
            lane_label.c_str());
        if (!lane_summary_text.empty()) {
            draw->AddText(
                ImVec2(canvas.x + left_w + 4.0f, row_y + 4.0f),
                excluded ? IM_COL32(150, 160, 176, 100) : IM_COL32(200, 211, 228, 245),
                lane_summary_text.c_str());
        }
        draw->AddLine(
            ImVec2(canvas.x + left_w, row_y + row_h),
            ImVec2(canvas.x + width, row_y + row_h),
            excluded ? IM_COL32(75, 86, 105, 45) : IM_COL32(75, 86, 105, 100),
            1.0f);
    }
    for (int bucket_index = 0; bucket_index < static_cast<int>(stacked_buckets.size()); ++bucket_index) {
        const StackedPayloadBucket& bucket = stacked_buckets[static_cast<std::size_t>(bucket_index)];
        if (bucket.lane_index < 0 || bucket.lane_index >= static_cast<int>(lanes.size())) {
            continue;
        }
        const PayloadLane& lane = lanes[static_cast<std::size_t>(bucket.lane_index)];
        const bool excluded = payload_client_excluded(state, lane.source, lane.client);
        const std::uint64_t total_bits = bucket.incoming_bits + bucket.outgoing_bits + bucket.unknown_bits;
        const std::uint64_t bucket_offset = state.payload_bucket_us == 0U ? 0U : (bucket.begin_us - state.payload_min_us) / state.payload_bucket_us;
        const float x0 = canvas.x + left_w + static_cast<float>(bucket_offset) * bucket_w + 2.0f;
        const float x1 = x0 + bucket_w - 4.0f;
        const float row_y = canvas.y + 28.0f + static_cast<float>(bucket.lane_index) * row_h;
        const float bar_h = max_bits == 0U ? 1.0f : std::max(2.0f, (static_cast<float>(total_bits) / static_cast<float>(max_bits)) * (row_h - 24.0f));
        const float bottom_y = row_y + row_h - 4.0f;
        const ImVec2 min(x0, bottom_y - bar_h);
        const ImVec2 max(x1, bottom_y);
        const bool selected = state.payload_time_filter_enabled &&
            bucket.begin_us >= state.payload_filter_begin_us &&
            bucket.end_us <= state.payload_filter_end_us;
        const bool hovered = mouse.x >= min.x && mouse.x <= max.x && mouse.y >= row_y && mouse.y <= row_y + row_h;
        if (hovered) {
            hovered_bucket = bucket_index;
        }
        float segment_bottom = bottom_y;
        auto draw_segment = [&](std::uint64_t bits, ImU32 color) {
            if (bits == 0U || total_bits == 0U) {
                return;
            }
            const float segment_h = std::max(1.0f, (static_cast<float>(bits) / static_cast<float>(total_bits)) * bar_h);
            draw->AddRectFilled(ImVec2(x0, segment_bottom - segment_h), ImVec2(x1, segment_bottom), color, 2.0f);
            segment_bottom -= segment_h;
        };
        draw_segment(bucket.incoming_bits, excluded ? IM_COL32(65, 135, 226, 70) : IM_COL32(65, 135, 226, 235));
        draw_segment(bucket.outgoing_bits, excluded ? IM_COL32(236, 184, 70, 72) : IM_COL32(236, 184, 70, 240));
        draw_segment(bucket.unknown_bits, excluded ? IM_COL32(135, 145, 160, 68) : IM_COL32(135, 145, 160, 225));
        const bool proximity_visible =
            max.x >= clip_min.x - bandwidth_outer_radius &&
            min.x <= clip_max.x + bandwidth_outer_radius &&
            max.y >= clip_min.y - bandwidth_outer_radius &&
            min.y <= clip_max.y + bandwidth_outer_radius;
        const float bar_proximity_alpha = hovered_canvas && proximity_visible && !selected
            ? proximity_alpha(
                mouse,
                ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                bandwidth_inner_radius,
                bandwidth_outer_radius,
                bandwidth_inner_radius_sq,
                bandwidth_outer_radius_sq)
            : 0.0f;
        if (bar_proximity_alpha > 0.0f) {
            const float proximity_visual_alpha = bar_proximity_alpha * bar_proximity_alpha * 0.5f;
            draw->AddRectFilled(
                ImVec2(min.x - 1.0f, min.y - 1.0f),
                ImVec2(max.x + 1.0f, max.y + 1.0f),
                color_with_alpha(IM_COL32(205, 226, 255, excluded ? 36 : 82), proximity_visual_alpha),
                2.0f);
            draw->AddRect(
                ImVec2(min.x - 1.0f, min.y - 1.0f),
                ImVec2(max.x + 1.0f, max.y + 1.0f),
                color_with_alpha(IM_COL32(238, 246, 255, excluded ? 92 : 230), proximity_visual_alpha),
                2.0f);
        }
        if (selected) {
            draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(255, 224, 128, 245), 2.0f, 0, 2.0f);
        }
        if (hovered) {
            const std::string tooltip_owner = lane.source == SyncTracePayloadSource::Replay
                ? "replay"
                : "client " + std::to_string(lane.client);
            draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(238, 246, 255, 230), 2.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip(
                "source %s\n%s\n%.3fms - %.3fms\nincoming %s\noutgoing %s\ntotal %s",
                payload_source_label(lane.source),
                tooltip_owner.c_str(),
                static_cast<double>(bucket.begin_us - state.payload_min_us) / 1000.0,
                static_cast<double>(bucket.end_us - state.payload_min_us) / 1000.0,
                format_bits(bucket.incoming_bits).c_str(),
                format_bits(bucket.outgoing_bits).c_str(),
                format_bits(total_bits).c_str());
        }
    }

    if (hovered_canvas && !hovered_client_label && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.payload_drag_selecting = true;
        state.payload_drag_start = mouse;
        state.payload_drag_current = mouse;
    }
    if (state.payload_drag_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.payload_drag_current = mouse;
        const ImVec2 min(std::min(state.payload_drag_start.x, state.payload_drag_current.x), std::min(state.payload_drag_start.y, state.payload_drag_current.y));
        const ImVec2 max(std::max(state.payload_drag_start.x, state.payload_drag_current.x), std::max(state.payload_drag_start.y, state.payload_drag_current.y));
        draw->AddRectFilled(min, max, IM_COL32(95, 145, 230, 48));
        draw->AddRect(min, max, IM_COL32(150, 195, 255, 190), 0.0f, 0, 1.0f);
    }
    if (state.payload_drag_selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float drag_distance = std::hypot(state.payload_drag_current.x - state.payload_drag_start.x, state.payload_drag_current.y - state.payload_drag_start.y);
        if (drag_distance > 4.0f) {
            const ImVec2 select_min(std::min(state.payload_drag_start.x, state.payload_drag_current.x), std::min(state.payload_drag_start.y, state.payload_drag_current.y));
            const ImVec2 select_max(std::max(state.payload_drag_start.x, state.payload_drag_current.x), std::max(state.payload_drag_start.y, state.payload_drag_current.y));
            bool found = false;
            std::uint64_t begin_us = 0;
            std::uint64_t end_us = 0;
            for (const StackedPayloadBucket& bucket : stacked_buckets) {
                if (bucket.lane_index < 0 || bucket.lane_index >= static_cast<int>(lanes.size())) {
                    continue;
                }
                const std::uint64_t bucket_offset = state.payload_bucket_us == 0U ? 0U : (bucket.begin_us - state.payload_min_us) / state.payload_bucket_us;
                const float x0 = canvas.x + left_w + static_cast<float>(bucket_offset) * bucket_w;
                const float x1 = x0 + bucket_w;
                const float y0 = canvas.y + 28.0f + static_cast<float>(bucket.lane_index) * row_h;
                const float y1 = y0 + row_h;
                const bool overlaps = x1 >= select_min.x && x0 <= select_max.x && y1 >= select_min.y && y0 <= select_max.y;
                if (!overlaps) {
                    continue;
                }
                begin_us = found ? std::min(begin_us, bucket.begin_us) : bucket.begin_us;
                end_us = found ? std::max(end_us, bucket.end_us) : bucket.end_us;
                found = true;
            }
            state.payload_time_filter_enabled = found;
            if (found) {
                state.payload_filter_begin_us = begin_us;
                state.payload_filter_end_us = end_us;
            }
            state.payload_view_dirty = true;
        } else if (hovered_bucket >= 0) {
            const StackedPayloadBucket& bucket = stacked_buckets[static_cast<std::size_t>(hovered_bucket)];
            state.payload_time_filter_enabled = true;
            state.payload_filter_begin_us = bucket.begin_us;
            state.payload_filter_end_us = bucket.end_us;
            state.payload_view_dirty = true;
        } else if (hovered_canvas) {
            state.payload_time_filter_enabled = false;
            state.payload_view_dirty = true;
        }
        state.payload_drag_selecting = false;
    }
    ImGui::EndChild();
}

void render_payload_filter_controls(ViewerState& state) {
    char time_filter_label[96]{};
    if (state.payload_time_filter_enabled) {
        std::snprintf(
            time_filter_label,
            sizeof(time_filter_label),
            "Time %.3f-%.3fms",
            static_cast<double>(state.payload_filter_begin_us - state.payload_min_us) / 1000.0,
            static_cast<double>(state.payload_filter_end_us - state.payload_min_us) / 1000.0);
    } else {
        std::snprintf(time_filter_label, sizeof(time_filter_label), "Time: all");
    }
    ImGui::BeginDisabled(!state.payload_time_filter_enabled);
    if (ImGui::Button(time_filter_label, ImVec2(170.0f, 0.0f))) {
        state.payload_time_filter_enabled = false;
        state.payload_view_dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("Name filter", &state.payload_filter_enabled)) {
        state.payload_view_dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::InputText("##payload_name_filter", state.payload_filter.data(), state.payload_filter.size())) {
        state.payload_filter_enabled = state.payload_filter[0] != '\0';
        state.payload_view_dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(105.0f);
    if (ImGui::BeginCombo("Source", payload_source_filter_label(state.payload_source_filter))) {
        if (ImGui::Selectable("All", state.payload_source_filter == PayloadSourceFilter::All)) {
            state.payload_source_filter = PayloadSourceFilter::All;
            state.payload_view_dirty = true;
        }
        if (ImGui::Selectable("Network", state.payload_source_filter == PayloadSourceFilter::Network)) {
            state.payload_source_filter = PayloadSourceFilter::Network;
            state.payload_view_dirty = true;
        }
        if (ImGui::Selectable("Replay", state.payload_source_filter == PayloadSourceFilter::Replay)) {
            state.payload_source_filter = PayloadSourceFilter::Replay;
            state.payload_view_dirty = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("Direction", payload_direction_filter_label(state.payload_direction_filter))) {
        if (ImGui::Selectable("All", state.payload_direction_filter == PayloadDirectionFilter::All)) {
            state.payload_direction_filter = PayloadDirectionFilter::All;
            state.payload_view_dirty = true;
        }
        if (ImGui::Selectable("Server -> Client", state.payload_direction_filter == PayloadDirectionFilter::Outgoing)) {
            state.payload_direction_filter = PayloadDirectionFilter::Outgoing;
            state.payload_view_dirty = true;
        }
        if (ImGui::Selectable("Client -> Server", state.payload_direction_filter == PayloadDirectionFilter::Incoming)) {
            state.payload_direction_filter = PayloadDirectionFilter::Incoming;
            state.payload_view_dirty = true;
        }
        ImGui::EndCombo();
    }
    if (!state.payload_excluded_clients.empty()) {
        ImGui::SameLine();
        const std::string hidden_label = "Hidden clients: " + std::to_string(state.payload_excluded_clients.size());
        if (ImGui::Button(hidden_label.c_str(), ImVec2(132.0f, 0.0f))) {
            state.payload_excluded_clients.clear();
            state.payload_view_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Hierarchy", &state.payload_hierarchy_view)) {
        state.payload_return_to_flat_on_back_to_top = false;
        if (!state.payload_hierarchy_view) {
            clear_selected_payload_scope(state);
            state.selected_payload_row = -1;
        }
        state.payload_view_dirty = true;
    }
    ImGui::SameLine();
    const char* current_sort = state.payload_sort == PayloadSortMode::Time ? "Time" : "Size desc";
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("Packet sort", current_sort)) {
        if (ImGui::Selectable("Time", state.payload_sort == PayloadSortMode::Time)) {
            state.payload_sort = PayloadSortMode::Time;
            state.payload_view_dirty = true;
        }
        if (ImGui::Selectable("Size desc", state.payload_sort == PayloadSortMode::SizeDescending)) {
            state.payload_sort = PayloadSortMode::SizeDescending;
            state.payload_view_dirty = true;
        }
        ImGui::EndCombo();
    }
}

void keep_selected_payload_packet_visible(ViewerState& state, const std::vector<int>& packets) {
    if (!packets.empty() && std::find(packets.begin(), packets.end(), state.selected_payload_packet) == packets.end()) {
        state.selected_payload_packet = packets.front();
    }
}

int payload_packet_position(const ViewerState& state, const std::vector<int>& packets) {
    const auto found = std::find(packets.begin(), packets.end(), state.selected_payload_packet);
    return found == packets.end() ? -1 : static_cast<int>(std::distance(packets.begin(), found));
}

void move_selected_payload_packet(ViewerState& state, const std::vector<int>& packets, int direction) {
    if (packets.empty() || direction == 0) {
        return;
    }
    const int packet_position = payload_packet_position(state, packets);
    const int packet_count = static_cast<int>(packets.size());
    int next = 0;
    if (direction < 0) {
        next = packet_position <= 0 ? packet_count - 1 : packet_position - 1;
    } else {
        next = packet_position < 0 || packet_position + 1 >= packet_count ? 0 : packet_position + 1;
    }
    state.selected_payload_packet = packets[static_cast<std::size_t>(next)];
    refresh_selected_payload_scope_instance(state);
}

void render_payload_rows_table(ViewerState& state) {
    const float table_height = std::max(170.0f, ImGui::GetContentRegionAvail().y * 0.42f);
    const std::uint64_t duration_us = payload_filter_duration_us(state);
    ImGui::BeginChild("payload_rows", ImVec2(0.0f, table_height), true);
    if (state.payload_rows.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "No payload rows match the active filters.");
    } else {
        ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_Sortable;
        if (ImGui::BeginTable("payload_table", 8, table_flags)) {
            const ImGuiTableColumnFlags sortable_flags = ImGuiTableColumnFlags_None;
            const ImGuiTableColumnFlags numeric_sort_flags = sortable_flags | ImGuiTableColumnFlags_PreferSortDescending;
            ImGui::TableSetupColumn(
                state.payload_hierarchy_view ? "Hierarchy" : "Name",
                sortable_flags |
                    ImGuiTableColumnFlags_WidthStretch |
                    (state.payload_row_sort_column == PayloadAggregateSortColumn::Name ? ImGuiTableColumnFlags_DefaultSort : ImGuiTableColumnFlags_None),
                1.0f,
                static_cast<ImGuiID>(PayloadAggregateSortColumn::Name));
            ImGui::TableSetupColumn("Count", numeric_sort_flags | ImGuiTableColumnFlags_WidthFixed, 68.0f, static_cast<ImGuiID>(PayloadAggregateSortColumn::Count));
            ImGui::TableSetupColumn(
                "Total",
                numeric_sort_flags |
                    ImGuiTableColumnFlags_WidthFixed |
                    (state.payload_row_sort_column == PayloadAggregateSortColumn::TotalBits ? ImGuiTableColumnFlags_DefaultSort : ImGuiTableColumnFlags_None),
                74.0f,
                static_cast<ImGuiID>(PayloadAggregateSortColumn::TotalBits));
            ImGui::TableSetupColumn(
                "Bandwidth/s",
                numeric_sort_flags |
                    ImGuiTableColumnFlags_WidthFixed |
                    (state.payload_row_sort_column == PayloadAggregateSortColumn::BandwidthBps ? ImGuiTableColumnFlags_DefaultSort : ImGuiTableColumnFlags_None),
                92.0f,
                static_cast<ImGuiID>(PayloadAggregateSortColumn::BandwidthBps));
            ImGui::TableSetupColumn("Avg", numeric_sort_flags | ImGuiTableColumnFlags_WidthFixed, 70.0f, static_cast<ImGuiID>(PayloadAggregateSortColumn::AvgBits));
            ImGui::TableSetupColumn("Max", numeric_sort_flags | ImGuiTableColumnFlags_WidthFixed, 70.0f, static_cast<ImGuiID>(PayloadAggregateSortColumn::MaxBits));
            ImGui::TableSetupColumn("Total (excl)", numeric_sort_flags | ImGuiTableColumnFlags_WidthFixed, 96.0f, static_cast<ImGuiID>(PayloadAggregateSortColumn::TotalExclusiveBits));
            ImGui::TableSetupColumn("Avg (excl)", numeric_sort_flags | ImGuiTableColumnFlags_WidthFixed, 86.0f, static_cast<ImGuiID>(PayloadAggregateSortColumn::AvgExclusiveBits));
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
                    state.payload_row_sort_column = static_cast<PayloadAggregateSortColumn>(spec.ColumnUserID);
                    state.payload_row_sort_ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
                    sort_payload_aggregate_rows(state);
                    sync_selected_payload_row(state);
                    sort_specs->SpecsDirty = false;
                }
            }
            if (state.payload_hierarchy_view && !state.selected_payload_scope_path.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID("payload_hierarchy_up");
                const bool clicked_up = ImGui::Selectable(
                    "##payload_hierarchy_up_selectable",
                    false,
                    ImGuiSelectableFlags_SpanAllColumns,
                    ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()));
                const ImVec2 row_min = ImGui::GetItemRectMin();
                const ImVec2 row_max = ImGui::GetItemRectMax();
                const float center_y = (row_min.y + row_max.y) * 0.5f;
                const ImU32 up_color = ImGui::GetColorU32(ImGuiCol_Text);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float icon_x = row_min.x + 8.0f;
                draw->AddLine(ImVec2(icon_x + 16.0f, center_y + 4.0f), ImVec2(icon_x + 6.0f, center_y + 4.0f), up_color, 1.5f);
                draw->AddLine(ImVec2(icon_x + 6.0f, center_y + 4.0f), ImVec2(icon_x + 6.0f, center_y - 5.0f), up_color, 1.5f);
                draw->AddLine(ImVec2(icon_x + 6.0f, center_y - 5.0f), ImVec2(icon_x + 2.0f, center_y - 1.0f), up_color, 1.5f);
                draw->AddLine(ImVec2(icon_x + 6.0f, center_y - 5.0f), ImVec2(icon_x + 10.0f, center_y - 1.0f), up_color, 1.5f);
                draw->AddText(ImVec2(row_min.x + 32.0f, row_min.y + 2.0f), up_color, "Back to top");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                if (clicked_up) {
                    clear_selected_payload_scope(state);
                    state.selected_payload_row = -1;
                    if (state.payload_return_to_flat_on_back_to_top) {
                        state.payload_hierarchy_view = false;
                        state.payload_return_to_flat_on_back_to_top = false;
                    }
                    state.payload_view_dirty = true;
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("-");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted("-");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted("-");
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted("-");
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted("-");
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted("-");
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted("-");
            }
            for (int row_index = 0; row_index < static_cast<int>(state.payload_rows.size()); ++row_index) {
                const PayloadAggregateRow& row = state.payload_rows[static_cast<std::size_t>(row_index)];
                const std::uint64_t avg = payload_row_average_bits(row);
                const std::uint64_t avg_exclusive = payload_row_average_exclusive_bits(row);
                const std::uint64_t bandwidth_bps = bits_per_second(row.total_bits, duration_us);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool selected = state.selected_payload_scope_path == row.path;
                std::string label = state.payload_hierarchy_view
                    ? std::string(static_cast<std::size_t>(std::max(0, row.depth)) * 2U, ' ') + row.name
                    : row.path;
                ImGui::PushID(row.path.c_str());
                const bool clicked_row = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                if (clicked_row) {
                    select_payload_scope_hierarchy_item(state, row.max_packet_index, row.path, row_index);
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", static_cast<unsigned long long>(row.count));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(format_compact_bits(row.total_bits).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(format_bps(bandwidth_bps).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(format_compact_bits(avg).c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(format_compact_bits(row.max_bits).c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(format_compact_bits(row.total_exclusive_bits).c_str());
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted(format_compact_bits(avg_exclusive).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void render_payload_packet_nav(ViewerState& state, const std::vector<int>& packets) {
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            move_selected_payload_packet(state, packets, -1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            move_selected_payload_packet(state, packets, 1);
        }
    }

    const int packet_position = payload_packet_position(state, packets);
    const bool previous_clicked = ImGui::Button("Previous");
    if (!packets.empty() && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (previous_clicked && !packets.empty()) {
        move_selected_payload_packet(state, packets, -1);
    }
    ImGui::SameLine();
    const bool next_clicked = ImGui::Button("Next");
    if (!packets.empty() && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (next_clicked && !packets.empty()) {
        move_selected_payload_packet(state, packets, 1);
    }
    ImGui::SameLine();
    ImGui::Text(
        "packet # %d / %d",
        packets.empty() || packet_position < 0 ? 0 : packet_position + 1,
        static_cast<int>(packets.size()));
    if (!state.selected_payload_scope_path.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.78f, 0.84f, 0.93f, 1.0f), "below %s", state.selected_payload_scope_path.c_str());
    }
}

void render_payload_packet_view(ViewerState& state, const std::vector<int>& packets) {
    ImGui::BeginChild("payload_packet_view", ImVec2(0.0f, 0.0f), true);
    if (state.selected_payload_packet < 0 ||
        state.selected_payload_packet >= static_cast<int>(state.payload_packets.size()) ||
        packets.empty()) {
        ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.86f, 1.0f), "No packet matches the active filters.");
        ImGui::EndChild();
        return;
    }
    const PayloadPacketInfo& packet = state.payload_packets[static_cast<std::size_t>(state.selected_payload_packet)];
    const std::string packet_owner = packet.payload_source == SyncTracePayloadSource::Replay
        ? "replay"
        : "client " + std::to_string(packet.client);
    ImGui::Text(
        "%s %s frame %u %.3fms %s",
        packet.source.c_str(),
        packet_owner.c_str(),
        packet.frame,
        static_cast<double>(packet.absolute_us - state.payload_min_us) / 1000.0,
        format_bits(packet.wire_bits).c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("payload_packet_context");
    }
    ImGui::Separator();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float available_w = std::max(280.0f, ImGui::GetContentRegionAvail().x - 12.0f);
    const float row_h = 30.0f;
    int max_depth = 0;
    std::uint64_t timeline_bits = packet.wire_bits;
    for (const PayloadScopeInfo& scope : packet.scopes) {
        max_depth = std::max(max_depth, scope.depth);
        timeline_bits = std::max(timeline_bits, payload_scope_end_bits(scope));
    }
    timeline_bits = std::max<std::uint64_t>(timeline_bits, 1U);
    const float height = static_cast<float>(max_depth + 1) * row_h + 8.0f;
    ImGui::InvisibleButton("##payload_packet_canvas", ImVec2(available_w, height));
    const bool packet_canvas_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("payload_packet_context");
    }
    if (ImGui::BeginPopup("payload_packet_context")) {
        if (ImGui::MenuItem("See in frame viewer")) {
            jump_selected_payload_to_frame(state);
        }
        if (ImGui::MenuItem("See in event viewer")) {
            jump_selected_payload_to_event_log(state);
        }
        ImGui::EndPopup();
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    bool clicked_bar = false;
    for (int scope_index = 0; scope_index < static_cast<int>(packet.scopes.size()); ++scope_index) {
        const PayloadScopeInfo& scope = packet.scopes[static_cast<std::size_t>(scope_index)];
        const std::uint64_t begin_bits = std::min(scope.begin_bits, timeline_bits);
        const std::uint64_t minimum_end_bits = begin_bits < timeline_bits ? begin_bits + 1U : begin_bits;
        const std::uint64_t end_bits = std::min(std::max(payload_scope_end_bits(scope), minimum_end_bits), timeline_bits);
        const float x0 = origin.x + (static_cast<float>(static_cast<double>(begin_bits) / static_cast<double>(timeline_bits)) * available_w);
        float x1 = origin.x + (static_cast<float>(static_cast<double>(end_bits) / static_cast<double>(timeline_bits)) * available_w);
        x1 = std::max(x0 + 2.0f, x1);
        const float y = origin.y + static_cast<float>(scope.depth) * row_h + 3.0f;
        const ImVec2 min(x0, y);
        const ImVec2 max(std::min(origin.x + available_w, x1), y + row_h - 7.0f);
        const bool hovered = mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
        const bool selected = payload_scope_selected(state, state.selected_payload_packet, scope);
        draw->AddRectFilled(min, max, payload_scope_color(scope.name), 3.0f);
        if (selected) {
            draw->AddRect(ImVec2(min.x - 2.0f, min.y - 2.0f), ImVec2(max.x + 2.0f, max.y + 2.0f), IM_COL32(255, 226, 135, 255), 3.0f, 0, 2.0f);
        } else if (hovered) {
            draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(238, 246, 255, 220), 3.0f);
        }
        if (max.x - min.x >= 36.0f) {
            const std::string text = scope.name + " " + std::to_string(scope.payload_bits) + " bits";
            ImGui::PushClipRect(ImVec2(min.x + 3.0f, min.y), ImVec2(max.x - 3.0f, max.y), true);
            draw->AddText(ImVec2(min.x + 7.0f, min.y + 4.0f), IM_COL32(250, 252, 255, 245), text.c_str());
            ImGui::PopClipRect();
        }
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip(
                "%s\nrange %llu-%llu\n%s",
                scope.path.c_str(),
                static_cast<unsigned long long>(begin_bits),
                static_cast<unsigned long long>(end_bits),
                format_bits(scope.payload_bits).c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                select_payload_scope_hierarchy_item(state, state.selected_payload_packet, scope);
                clicked_bar = true;
            }
        }
    }
    if (packet_canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !clicked_bar) {
        if (!state.selected_payload_scope_path.empty()) {
            clear_selected_payload_scope(state);
            state.payload_view_dirty = true;
        }
    }
    ImGui::EndChild();
}

void render_payloads(ViewerState& state) {
    const auto start = Clock::now();
    ensure_payload_rows(state);

    if (state.payload_packets.empty()) {
        ImGui::BeginChild("payload_empty", ImVec2(0.0f, 0.0f), true);
        ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "No serialization payloads found.");
        ImGui::TextColored(
            ImVec4(0.72f, 0.78f, 0.86f, 1.0f),
            payloads_enabled_in_trace(state)
                ? "This trace has serialization payload tracing enabled, but no payload records were loaded."
                : "Capture a trace with TraceOptions::serialization_payloads enabled to inspect packet hierarchy bandwidth.");
        ImGui::EndChild();
        state.current_timing.timeline_ms += elapsed_ms(start);
        return;
    }

    render_payload_filter_controls(state);
    ensure_payload_view(state);
    render_payload_bandwidth_chart(state);
    ensure_payload_view(state);

    render_payload_rows_table(state);
    ensure_payload_view(state);
    render_payload_packet_nav(state, state.payload_filtered_packets);
    render_payload_packet_view(state, state.payload_filtered_packets);
    state.current_timing.timeline_ms += elapsed_ms(start);
}

void render_details(ViewerState& state) {
    const auto start = Clock::now();
    ImGui::BeginChild("details", ImVec2(0.0f, 0.0f), true);
    if (state.mode == ViewerMode::EventLog) {
        const PacketEventInfo* event = selected_packet_event(state);
        if (event == nullptr) {
            ImGui::TextColored(ImVec4(0.56f, 0.62f, 0.70f, 1.0f), "Select a packet event.");
            ImGui::EndChild();
            state.current_timing.details_ms += elapsed_ms(start);
            return;
        }
        rebuild_packet_detail_cache(state);
        ImGui::SetWindowFontScale(1.2f);
        ImGui::TextColored(
            ImVec4(0.82f, 0.88f, 0.96f, 1.0f),
            "%s %s %.3fms",
            event->send ? "Sent" : "Received",
            event->message.c_str(),
            static_cast<double>(event->relative_us) / 1000.0);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        if (packet_update_apply_failed(*event)) {
            ImGui::TextColored(ImVec4(0.98f, 0.52f, 0.56f, 1.0f), "Applied false");
            if (!event->apply_failure.empty()) {
                ImGui::Text("Reason %s", event->apply_failure.c_str());
            }
            ImGui::Separator();
        }
        const std::string details_text = build_record_details_text(state.cached_details);
        render_selectable_detail_text("##event_detail_text", details_text);
        ImGui::EndChild();
        state.current_timing.details_ms += elapsed_ms(start);
        return;
    }
    if (state.selected.source_index < 0 || state.selected.source_index >= static_cast<int>(state.history.sources.size())) {
        ImGui::TextColored(ImVec4(0.56f, 0.62f, 0.70f, 1.0f), "Select a frame.");
        ImGui::EndChild();
        state.current_timing.details_ms += elapsed_ms(start);
        return;
    }
    rebuild_detail_cache(state);

    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextColored(ImVec4(0.82f, 0.88f, 0.96f, 1.0f), "Frame %u", state.selected.frame);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();

    const std::string details_text = build_record_details_text(state.cached_details);
    render_selectable_detail_text("##frame_detail_text", details_text);
    ImGui::EndChild();
    state.current_timing.details_ms += elapsed_ms(start);
}

void render_app(ViewerState& state) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::Begin("Ashiato Sync Trace Viewer", nullptr,
        ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(ImVec4(0.86f, 0.91f, 0.98f, 1.0f), "Ashiato Sync Trace Viewer");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.48f, 0.55f, 0.65f, 1.0f), "%zu sources", state.history.sources.size());

    ImGui::SetNextItemWidth(-252.0f);
    if (ImGui::InputText("##trace_directory", state.directory.data(), state.directory.size())) {
        state.directory_has_ktrace_files = directory_contains_ktrace_files(state.directory.data());
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(84.0f, 0.0f))) {
        open_directory_picker(state);
    }
    ImGui::SameLine();
    if (!state.directory_has_ktrace_files) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Load", ImVec2(70.0f, 0.0f))) {
        load_directory(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(70.0f, 0.0f))) {
        load_directory(state);
    }
    if (!state.directory_has_ktrace_files) {
        ImGui::EndDisabled();
    }
    if (state.loader.active) {
        const float progress = state.loader.total_bytes != 0U
            ? std::min(1.0f, static_cast<float>(
                  static_cast<double>(state.loader.bytes_read) / static_cast<double>(state.loader.total_bytes)))
            : 0.0f;
        char progress_text[96];
        std::snprintf(
            progress_text,
            sizeof(progress_text),
            "loading trace data... %.0f%% complete",
            static_cast<double>(progress) * 100.0);
        const ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        const float bar_width = ImGui::GetContentRegionAvail().x;
        const float bar_height = ImGui::GetFrameHeight();
        const ImVec2 bar_size{bar_width, bar_height};
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImU32 background_color = ImGui::GetColorU32(ImGuiCol_FrameBg);
        const ImU32 fill_color = IM_COL32(31, 107, 224, 255);
        const ImU32 text_color = IM_COL32(222, 232, 245, 255);
        const float rounding = ImGui::GetStyle().FrameRounding;
        draw_list->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_size.x, bar_pos.y + bar_size.y), background_color, rounding);
        draw_list->AddRectFilled(
            bar_pos,
            ImVec2(bar_pos.x + bar_size.x * progress, bar_pos.y + bar_size.y),
            fill_color,
            rounding);
        const ImVec2 text_pos{
            bar_pos.x + ImGui::GetStyle().FramePadding.x,
            bar_pos.y + (bar_size.y - ImGui::GetTextLineHeight()) * 0.5f};
        draw_list->AddText(text_pos, text_color, progress_text);
        ImGui::Dummy(bar_size);
    } else if (!state.status.empty()) {
        const ImVec2 status_pos = ImGui::GetCursorScreenPos();
        const ImVec2 status_size{
            ImGui::GetContentRegionAvail().x,
            state.status_is_error ? ImGui::GetTextLineHeightWithSpacing() * 2.0f : ImGui::GetFrameHeight()};
        const ImVec2 text_pos{
            status_pos.x + (state.status_is_error ? ImGui::GetStyle().FramePadding.x : 0.0f),
            status_pos.y + (status_size.y - ImGui::GetTextLineHeight()) * 0.5f};
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        if (state.status_is_error) {
            draw_list->AddRectFilled(
                status_pos,
                ImVec2(status_pos.x + status_size.x, status_pos.y + status_size.y),
                IM_COL32(172, 32, 44, 255),
                ImGui::GetStyle().FrameRounding);
        }
        draw_list->AddText(
            text_pos,
            state.status_is_error
                ? IM_COL32(255, 255, 255, 255)
                : ImGui::GetColorU32(ImVec4(0.58f, 0.66f, 0.76f, 1.0f)),
            state.status.c_str());
        ImGui::Dummy(status_size);
    }
    render_directory_picker(state);

    const bool has_packet_logs = packet_logs_enabled_in_trace(state);
    if (!has_packet_logs && state.mode == ViewerMode::EventLog) {
        state.mode = ViewerMode::Frames;
    }
    {
        const char* current_mode = state.mode == ViewerMode::Frames
            ? "Frames"
            : (state.mode == ViewerMode::EventLog ? "Event Log" : "Bandwidth");
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("View", current_mode)) {
            const bool frames_selected = state.mode == ViewerMode::Frames;
            if (ImGui::Selectable("Frames", frames_selected)) {
                state.mode = ViewerMode::Frames;
            }
            if (frames_selected) {
                ImGui::SetItemDefaultFocus();
            }
            const bool event_selected = state.mode == ViewerMode::EventLog;
            if (ImGui::Selectable("Event Log", event_selected)) {
                state.mode = ViewerMode::EventLog;
            }
            if (event_selected) {
                ImGui::SetItemDefaultFocus();
            }
            const bool payload_selected = state.mode == ViewerMode::Payloads;
            if (ImGui::Selectable("Bandwidth", payload_selected)) {
                state.mode = ViewerMode::Payloads;
            }
            if (payload_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (state.mode == ViewerMode::EventLog) {
        ensure_packet_log_rows(state);
        render_event_log(state);
    } else if (state.mode == ViewerMode::Payloads) {
        render_payloads(state);
    } else {
        render_legend();
        ImGui::Spacing();
        if (ImGui::BeginTabBar("sources")) {
            for (int i = 0; i < static_cast<int>(state.history.sources.size()); ++i) {
                const std::string label = source_label(state.history.sources[static_cast<std::size_t>(i)]);
                ImGuiTabItemFlags flags = 0;
                if (state.selected_source_dirty && i == state.selected_source) {
                    flags |= ImGuiTabItemFlags_SetSelected;
                }
                if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                    if (state.selected_source != i) {
                        state.selected_source = i;
                    }
                    state.selected_source_dirty = false;
                    render_timeline(state, i);
                    handle_timeline_keyboard(state, i);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
    }
    select_benchmark_cell(state);
    render_details(state);

    if (!state.benchmark.options.enabled) {
        ImGui::Separator();
        ImGui::Text(
            "viewer %.3fms timeline %.3fms details %.3fms selection %.3fms rows %d cells %d",
            state.current_timing.total_ms,
            state.current_timing.timeline_ms,
            state.current_timing.details_ms,
            state.current_timing.selection_ms,
            state.current_timing.visible_rows,
            state.current_timing.visible_cells);
    }

    ImGui::End();
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double clamped = std::clamp(p, 0.0, 1.0);
    const std::size_t index = static_cast<std::size_t>(clamped * static_cast<double>(values.size() - 1U));
    return values[index];
}

double average(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

void write_metric_block(std::ofstream& out, const char* name, const std::vector<double>& values, bool comma) {
    const double max_value = values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    out << "    \"" << name << "\": {"
        << "\"avg_ms\": " << average(values)
        << ", \"p50_ms\": " << percentile(values, 0.50)
        << ", \"p95_ms\": " << percentile(values, 0.95)
        << ", \"p99_ms\": " << percentile(values, 0.99)
        << ", \"max_ms\": " << max_value
        << "}" << (comma ? "," : "") << "\n";
}

void write_benchmark_report(const ViewerState& state) {
    const std::string path = state.benchmark.options.report_path.empty()
        ? std::string("/tmp/ashiato_sync_trace_viewer_benchmark.json")
        : state.benchmark.options.report_path;
    std::ofstream out(path);
    if (!out) {
        return;
    }

    std::vector<double> total;
    std::vector<double> timeline;
    std::vector<double> details;
    std::vector<double> selection;
    int max_rows = 0;
    int max_cells = 0;
    for (const FrameTiming& frame : state.benchmark.frames) {
        total.push_back(frame.total_ms);
        timeline.push_back(frame.timeline_ms);
        details.push_back(frame.details_ms);
        selection.push_back(frame.selection_ms);
        max_rows = std::max(max_rows, frame.visible_rows);
        max_cells = std::max(max_cells, frame.visible_cells);
    }

    std::size_t source_count = state.history.sources.size();
    std::size_t entity_count = 0;
    std::size_t component_row_count = 0;
    std::size_t record_count = 0;
    for (const KTraceSourceHistory& source : state.history.sources) {
        record_count += source.records.size();
        entity_count += source.entities.size();
        for (const KTraceEntityRow& entity : source.entities) {
            component_row_count += entity.components.size();
        }
    }

    out << "{\n";
    out << "  \"trace_directory\": \"" << state.directory.data() << "\",\n";
    out << "  \"load_ms\": " << state.benchmark.load_ms << ",\n";
    out << "  \"frames\": " << state.benchmark.frames.size() << ",\n";
    out << "  \"sources\": " << source_count << ",\n";
    out << "  \"records\": " << record_count << ",\n";
    out << "  \"entities\": " << entity_count << ",\n";
    out << "  \"component_rows\": " << component_row_count << ",\n";
    out << "  \"max_rendered_rows\": " << max_rows << ",\n";
    out << "  \"max_rendered_cells\": " << max_cells << ",\n";
    out << "  \"metrics\": {\n";
    write_metric_block(out, "total", total, true);
    write_metric_block(out, "timeline", timeline, true);
    write_metric_block(out, "details", details, true);
    write_metric_block(out, "selection", selection, false);
    out << "  }\n";
    out << "}\n";
}

bool parse_int_arg(const std::string& arg, const char* name, int& out) {
    try {
        out = std::stoi(arg);
        return out > 0;
    } catch (...) {
        std::fprintf(stderr, "%s must be a positive integer\n", name);
        return false;
    }
}

bool parse_args(int argc, char** argv, ViewerState& state) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&]() -> const char* {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++index];
        };
        try {
            if (arg == "--trace-dir") {
                std::snprintf(state.directory.data(), state.directory.size(), "%s", require_value());
            } else if (arg == "--benchmark") {
                state.benchmark.options.enabled = true;
            } else if (arg == "--benchmark-report") {
                state.benchmark.options.report_path = require_value();
            } else if (arg == "--benchmark-frames") {
                if (!parse_int_arg(require_value(), "--benchmark-frames", state.benchmark.options.frames)) {
                    return false;
                }
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
            } else if (arg == "--control-socket") {
                state.control_socket_path = require_value();
#endif
            } else if (arg.rfind("--", 0) == 0) {
                std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
                return false;
            } else if (state.directory[0] == '\0') {
                std::snprintf(state.directory.data(), state.directory.size(), "%s", arg.c_str());
            } else {
                std::fprintf(stderr, "unexpected positional argument: %s\n", arg.c_str());
                return false;
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return false;
        }
    }
    return true;
}

void glfw_error_callback(int code, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description != nullptr ? description : "unknown error");
}

}  // namespace

int main(int argc, char** argv) {
    ViewerState state;
    if (!parse_args(argc, argv, state)) {
        return 2;
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to initialize GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1400, 900, "Ashiato Sync Trace Viewer", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "failed to create trace viewer window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    apply_professional_dark_style();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    if (state.directory[0] != '\0') {
        load_directory(state);
    }

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    ControlServer control_server;
    std::string control_error;
    if (!start_control_server(control_server, state.control_socket_path, control_error)) {
        std::fprintf(stderr, "failed to start control socket: %s\n", control_error.c_str());
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!state.control_socket_path.empty()) {
        state.status = "control socket listening at " + state.control_socket_path;
        state.status_is_error = false;
        std::fprintf(stderr, "control socket listening at %s\n", state.control_socket_path.c_str());
    }
    AutomationInput automation_input;
#endif

    while (!glfwWindowShouldClose(window)) {
        if (state.benchmark.options.enabled && !state.history.sources.empty()) {
            const int source_count = static_cast<int>(state.history.sources.size());
            const int benchmark_source = (state.benchmark.frame_index / 120) % std::max(1, source_count);
            if (state.selected_source != benchmark_source) {
                state.selected_source = benchmark_source;
                state.selected_source_dirty = true;
            }
        }
        state.current_timing = {};
        const auto frame_start = Clock::now();
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        poll_control_server(control_server, state, automation_input, window);
#endif
        process_loader_messages(state);
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        apply_automation_input(automation_input);
#endif
        ImGui::NewFrame();
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        if (!state.benchmark.options.enabled && ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
            request_screenshot(state);
        }
#endif

        render_app(state);
        state.current_timing.total_ms = elapsed_ms(frame_start);

        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        if (state.screenshot_requested) {
            const std::string path = screenshot_output_path(state);
            std::string error;
            if (write_framebuffer_png(path, width, height, error)) {
                state.last_screenshot_path = path;
                state.status = "screenshot saved to " + path;
                state.status_is_error = false;
                std::fprintf(stderr, "screenshot saved to %s\n", path.c_str());
            } else {
                state.screenshot_failed = true;
                state.status = "screenshot failed: " + error;
                state.status_is_error = true;
                std::fprintf(stderr, "screenshot failed: %s\n", error.c_str());
            }
            state.screenshot_requested = false;
        }
#endif
        glfwSwapBuffers(window);

        if (state.benchmark.options.enabled) {
            state.benchmark.frames.push_back(state.current_timing);
            ++state.benchmark.frame_index;
            if (state.benchmark.frame_index >= state.benchmark.options.frames) {
                write_benchmark_report(state);
                state.benchmark.report_written = true;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }
    }

    if (state.benchmark.options.enabled && !state.benchmark.report_written) {
        write_benchmark_report(state);
    }

#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    stop_control_server(control_server);
#endif
    stop_loading_directory(state);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
#if defined(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    return state.screenshot_failed ? 1 : 0;
#else
    return 0;
#endif
}
