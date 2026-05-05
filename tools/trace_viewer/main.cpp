#include "kage/sync/tracing.hpp"
#include "kage/sync/types.hpp"

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
#include <atomic>
#include <deque>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#endif

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION) && !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace {

using namespace kage::sync;

constexpr float label_width = 340.0f;
constexpr float row_height = 24.0f;
constexpr float frame_pitch = 18.0f;
constexpr float packet_time_us_pitch = 0.0035f;
constexpr float packet_column_width = 300.0f;
constexpr float packet_column_gap = 140.0f;
constexpr float packet_marker_size = 13.0f;
constexpr float packet_lane_spacing = 20.0f;
constexpr float pill_width = 14.0f;
constexpr float pill_height = 14.0f;
constexpr float timeline_header_height = 24.0f;
constexpr ImU32 timeline_link_color = IM_COL32(194, 200, 210, 125);

struct SelectedCell {
    int source_index = -1;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
    ecs::Entity component{};
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
    bool server_side = false;
    bool send = false;
    int lane = 0;
    int flow_index = -1;
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
        Records,
        Finished,
        Failed
    };

    Type type = Type::Records;
    int source_index = -1;
    KTraceFileHeader source;
    std::vector<KTraceRecord> records;
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
    EventLog
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
    ecs::Entity component{};
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

struct ViewerState {
    SyncTraceHistory history;
    std::array<char, 1024> directory{};
    std::string status;
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
    int selected_packet_client = 0;
    std::array<char, 1024> picker_path{};
    std::vector<DirectoryPickerEntry> picker_entries;
    std::string picker_error;
    int picker_selected = -1;
    bool directory_has_ktrace_files = false;
    bool picker_path_has_ktrace_files = false;
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    std::string screenshot_path;
    std::string last_screenshot_path;
    std::string control_socket_path;
#endif
    std::unordered_set<EntityExpansionKey, EntityExpansionKeyHash> expanded_entities;
    std::unordered_set<ComponentExpansionKey, ComponentExpansionKeyHash> expanded_components;
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    std::uint32_t screenshot_counter = 0;
    bool screenshot_requested = false;
    bool screenshot_failed = false;
#endif
    bool selected_source_dirty = true;
    bool details_dirty = true;
    bool packet_details_dirty = true;
    TraceLoadState loader;
};

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
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

struct RunRenderItem {
    const KTraceFrameRun* run = nullptr;
    KTraceRunId run_id = invalid_trace_run;
    int lane = 0;
    SyncFrame end_frame = 0;
    std::vector<std::pair<SyncFrame, int>> frame_lanes;
};

constexpr int component_lane = -1;
constexpr int unassigned_lane = -2;

struct RunLaneLayout {
    std::vector<RunRenderItem> items;
    int lane_count = 0;
};

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

bool is_cue_row_component(ecs::Entity component) noexcept {
    return (component.value & (std::uint64_t{1} << 63U)) != 0U;
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end = Clock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
std::string default_screenshot_path(ViewerState& state) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "kage_sync_trace_viewer_" + std::to_string(ms) + "_" + std::to_string(state.screenshot_counter++) + ".png";
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

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
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
    const std::vector<RunVerticalSpan>& spans,
    SyncFrame frame,
    int from_lane,
    int to_lane) {
    if (from_lane == to_lane) {
        return true;
    }
    for (const RunVerticalSpan& span : spans) {
        if (span.frame == frame && vertical_spans_overlap(from_lane, to_lane, span.from_lane, span.to_lane)) {
            return false;
        }
    }
    return true;
}

void reserve_vertical_span(
    std::vector<RunVerticalSpan>& spans,
    SyncFrame frame,
    int from_lane,
    int to_lane) {
    if (from_lane == to_lane) {
        return;
    }
    spans.push_back(RunVerticalSpan{frame, from_lane, to_lane});
}

int first_available_run_lane(
    const std::vector<SyncFrame>& lane_ends,
    const std::vector<RunVerticalSpan>& spans,
    int first_lane,
    SyncFrame frame,
    int parent_lane) {
    int lane = std::max(0, first_lane);
    for (; lane < static_cast<int>(lane_ends.size()); ++lane) {
        if (lane_has_gap_before(lane_ends[static_cast<std::size_t>(lane)], frame) &&
            vertical_span_available(spans, frame, parent_lane, lane)) {
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
            &run,
            static_cast<KTraceRunId>(run_index),
            0,
            run_end_frame(run),
            {}});
    }
    std::sort(layout.items.begin(), layout.items.end(), [](const RunRenderItem& lhs, const RunRenderItem& rhs) {
        if (lhs.run->start_frame != rhs.run->start_frame) {
            return lhs.run->start_frame < rhs.run->start_frame;
        }
        return lhs.end_frame < rhs.end_frame;
    });

    std::vector<SyncFrame> lane_ends;
    std::vector<RunVerticalSpan> vertical_spans;
    SyncFrame component_end = component.runs.empty() ? 0 : run_end_frame(component.runs[0]);
    for (RunRenderItem& item : layout.items) {
        const std::vector<SyncFrame> frames = run_frames_for_layout(*item.run);
        int current_lane = unassigned_lane;
        item.frame_lanes.reserve(frames.size());
        const std::size_t item_index = static_cast<std::size_t>(&item - layout.items.data());
        const int parent_lane = run_parent_lane(layout.items, item_index);
        for (SyncFrame frame : frames) {
            int lane = unassigned_lane;
            if (current_lane == unassigned_lane) {
                lane = first_available_run_lane(lane_ends, vertical_spans, parent_lane + 1, frame, parent_lane);
                reserve_vertical_span(vertical_spans, frame, parent_lane, lane);
            } else {
                for (int candidate = component_lane; candidate < current_lane; ++candidate) {
                    if (candidate >= static_cast<int>(lane_ends.size())) {
                        break;
                    }
                    if (lane_has_open_frame_before(lane_end_at(lane_ends, component_end, candidate), frame) &&
                        vertical_span_available(vertical_spans, frame, current_lane, candidate)) {
                        lane = candidate;
                        break;
                    }
                }
                if (lane == unassigned_lane) {
                    lane = current_lane;
                } else {
                    reserve_vertical_span(vertical_spans, frame, current_lane, lane);
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
    for (const auto& frame_lane : item.frame_lanes) {
        if (frame_lane.first == frame) {
            return frame_lane.second;
        }
    }
    return unassigned_lane;
}

int run_parent_lane(const std::vector<RunRenderItem>& runs, std::size_t run_index) {
    const RunRenderItem& run = runs[run_index];
    if (run.run == nullptr || run.run->prev == invalid_trace_run || run.run->prev == 0) {
        return component_lane;
    }
    int parent_lane = component_lane;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index == run_index) {
            continue;
        }
        const RunRenderItem& candidate = runs[index];
        if (candidate.run_id == run.run->prev) {
            parent_lane = run_lane_at_frame(candidate, run.run->start_frame);
            break;
        }
    }
    return parent_lane;
}

int packed_run_lane_count(const KTraceComponentRow& component) {
    return pack_run_lanes(component).lane_count;
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

std::string packet_match_key(const PacketEventInfo& event) {
    std::ostringstream out;
    out << event.client << "|" << event.message << "|";
    if (event.message == "server_update") {
        out << event.sequence;
    } else if (event.message == "client_input") {
        out << event.acks << "|" << event.baseline << "|" << event.input_frames;
    } else {
        out << event.acks;
    }
    return out.str();
}

bool packet_endpoints_match(const PacketEventInfo& send, const PacketEventInfo& receive) {
    if (!send.send || receive.send ||
        send.client != receive.client ||
        send.message != receive.message ||
        send.server_side == receive.server_side) {
        return false;
    }
    return packet_match_key(send) == packet_match_key(receive);
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
    const std::uint64_t min_gap_us = static_cast<std::uint64_t>(packet_marker_size / packet_time_us_pitch) + 1U;
    for (int event_index : order) {
        PacketEventInfo& event = state.packet_events[static_cast<std::size_t>(event_index)];
        if (event.client != active_client || event.server_side != active_server_side) {
            active_client = event.client;
            active_server_side = event.server_side;
            lane_times.clear();
        }
        int selected_lane = 0;
        for (; selected_lane < static_cast<int>(lane_times.size()); ++selected_lane) {
            if (event.absolute_us >= lane_times[static_cast<std::size_t>(selected_lane)] + min_gap_us) {
                break;
            }
        }
        if (selected_lane == static_cast<int>(lane_times.size())) {
            lane_times.push_back(event.absolute_us);
        } else {
            lane_times[static_cast<std::size_t>(selected_lane)] = event.absolute_us;
        }
        event.lane = selected_lane;
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

    std::vector<bool> receive_used(state.packet_events.size(), false);
    for (int send_index = 0; send_index < static_cast<int>(state.packet_events.size()); ++send_index) {
        const PacketEventInfo& send = state.packet_events[static_cast<std::size_t>(send_index)];
        if (!send.send) {
            continue;
        }
        int best_receive = -1;
        std::uint64_t best_delta = std::numeric_limits<std::uint64_t>::max();
        for (int receive_index = 0; receive_index < static_cast<int>(state.packet_events.size()); ++receive_index) {
            if (receive_used[static_cast<std::size_t>(receive_index)]) {
                continue;
            }
            const PacketEventInfo& receive = state.packet_events[static_cast<std::size_t>(receive_index)];
            if (!packet_endpoints_match(send, receive)) {
                continue;
            }
            const std::uint64_t delta = receive.absolute_us >= send.absolute_us
                ? receive.absolute_us - send.absolute_us
                : send.absolute_us - receive.absolute_us;
            if (best_receive < 0 ||
                (receive.absolute_us >= send.absolute_us && state.packet_events[static_cast<std::size_t>(best_receive)].absolute_us < send.absolute_us) ||
                delta < best_delta) {
                best_receive = receive_index;
                best_delta = delta;
            }
        }
        PacketFlow flow;
        flow.send_event = send_index;
        flow.receive_event = best_receive;
        flow.client = send.client;
        flow.server_to_client = send.server_side;
        if (best_receive >= 0) {
            receive_used[static_cast<std::size_t>(best_receive)] = true;
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

std::string component_label(const KTraceSourceHistory& source, ecs::Entity component) {
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
    if (item.run == nullptr) {
        return;
    }
    for (std::size_t index = 1; index < item.run->frames.size(); ++index) {
        const SyncFrame previous = item.run->frames[index - 1U].frame;
        const SyncFrame current = item.run->frames[index].frame;
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
    ecs::Entity component,
    KTraceRunId run,
    int source_index,
    ViewerState& state,
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

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
    const bool selected = state.selected.source_index == source_index &&
        state.selected.network_id == entity.client_network_id &&
        state.selected.component == component &&
        state.selected.frame == cell.frame &&
        state.selected.run == run &&
        state.selected.event_indices == cell.event_indices;
    if (selected) {
        draw->AddRect(ImVec2(min.x - 2.0f, min.y - 2.0f), ImVec2(max.x + 2.0f, max.y + 2.0f), IM_COL32(255, 230, 155, 255), 0.0f, 0, 2.0f);
    } else if (hovered) {
        draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(230, 238, 255, 210), 0.0f, 0, 1.0f);
    }
    if (hovered) {
        ImGui::SetTooltip("%s frame %u", visual.label, cell.frame);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            SelectedCell selected{source_index, entity.client_network_id, component, cell.frame, run, cell.event_indices};
            if (state.selected != selected) {
                state.selected = selected;
                state.details_dirty = true;
            }
        }
    }
}

bool entity_expanded(const ViewerState& state, int source_index, const KTraceEntityRow& entity) {
    return state.expanded_entities.count(EntityExpansionKey{source_index, entity.client_network_id}) != 0U;
}

bool component_expanded(
    const ViewerState& state,
    int source_index,
    const KTraceEntityRow& entity,
    ecs::Entity component) {
    return state.expanded_components.count(ComponentExpansionKey{source_index, entity.client_network_id, component}) != 0U;
}

bool component_has_resim(const KTraceComponentRow& component) {
    for (std::size_t run_index = 1; run_index < component.runs.size(); ++run_index) {
        for (const KTraceFrameCell& cell : component.runs[run_index].frames) {
            if (has_state(cell, KTraceCellState::Resimulated)) {
                return true;
            }
        }
    }
    return false;
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
    ecs::Entity component) {
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
    ecs::Entity component,
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
    for_each_entity_cell(entity, [&](ecs::Entity, KTraceRunId, const KTraceFrameCell& cell) {
        if (cell.frame == frame) {
            out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
        }
    });
}

void append_component_frame_event_indices(
    const KTraceEntityRow& entity,
    ecs::Entity component_id,
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
    ViewerState& state) {
    int drawn = 0;
    for (SyncFrame frame = first_visible_frame; frame <= last_visible_frame; ++frame) {
        SummaryState summary = SummaryState::None;
        for_each_entity_cell(entity, [&](ecs::Entity, KTraceRunId, const KTraceFrameCell& cell) {
            if (cell.frame == frame) {
                aggregate_entity_cell(summary, cell);
            }
        });
        if (summary != SummaryState::None) {
            KTraceFrameCell cell = summary_cell(frame, summary);
            append_entity_frame_event_indices(entity, frame, cell.event_indices);
            draw_cell(draw, source, cell, origin, scroll_x, scroll_y, min_frame, row, entity, ecs::Entity{}, 0, source_index, state);
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
    ViewerState& state) {
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
            draw_cell(draw, source, cell, origin, scroll_x, scroll_y, min_frame, row, entity, component.component, 0, source_index, state);
            ++drawn;
        }
    }
    return drawn;
}

SummaryState entity_summary_at_frame(const KTraceEntityRow& entity, SyncFrame frame) {
    SummaryState summary = SummaryState::None;
    for_each_entity_cell(entity, [&](ecs::Entity, KTraceRunId, const KTraceFrameCell& cell) {
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
    for_each_entity_cell(entity, [&](ecs::Entity, KTraceRunId, const KTraceFrameCell& cell) {
        frames.push_back(cell.frame);
    });
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

std::vector<SyncFrame> component_summary_frames(const KTraceEntityRow& entity, ecs::Entity component_id) {
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
    ecs::Entity component,
    const KTraceFrameCell& cell,
    KTraceRunId run,
    int row) {
    out.push_back(TimelineNavCell{
        SelectedCell{source_index, entity.client_network_id, component, cell.frame, run, cell.event_indices},
        row});
}

std::vector<TimelineNavCell> collect_timeline_nav_cells(
    const ViewerState& state,
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
                append_nav_cell(cells, source_index, entity, ecs::Entity{}, cell, 0, row);
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
            const RunLaneLayout run_layout = pack_run_lanes(component);
            for (const RunRenderItem& item : run_layout.items) {
                if (item.run == nullptr) {
                    continue;
                }
                for (const KTraceFrameCell& cell : item.run->frames) {
                    const int cell_lane = run_lane_at_frame(item, cell.frame);
                    if (cell_lane != unassigned_lane) {
                        append_nav_cell(cells, source_index, entity, component.component, cell, item.run_id, run_base_row + cell_lane);
                    }
                }
            }
            row += run_layout.lane_count;
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

SourceMetrics compute_source_metrics(const KTraceSourceHistory& source, const ViewerState& state, int source_index);
void rebuild_source_metrics(ViewerState& state);

void apply_server_entity_links(SyncTraceHistory& history) {
    std::unordered_map<ClientEntityNetworkId, ecs::Entity> server_entities_by_network_id;
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

void rebuild_streamed_source(ViewerState& state, int source_index) {
    if (source_index < 0 || source_index >= static_cast<int>(state.history.sources.size())) {
        return;
    }
    KTraceSourceHistory& current = state.history.sources[static_cast<std::size_t>(source_index)];
    KTraceFile file;
    file.path = current.path;
    file.role = current.role;
    file.client = current.client;
    file.recorded_unix_ns = current.recorded_unix_ns;
    file.flags = current.flags;
    file.records = std::move(current.records);
    current = KTraceReader::build_source_history(std::move(file));
}

void process_loader_messages(ViewerState& state) {
    constexpr std::size_t max_messages_per_frame = 8;
    std::deque<TraceLoadMessage> messages;
    {
        std::lock_guard<std::mutex> lock(state.loader.mutex);
        while (!state.loader.messages.empty() && messages.size() < max_messages_per_frame) {
            messages.push_back(std::move(state.loader.messages.front()));
            state.loader.messages.pop_front();
        }
    }
    if (messages.empty()) {
        return;
    }

    bool data_changed = false;
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
            data_changed = true;
            break;
        }
        case TraceLoadMessage::Type::Records: {
            if (message.source_index < 0 || message.source_index >= static_cast<int>(state.history.sources.size())) {
                break;
            }
            KTraceSourceHistory& source = state.history.sources[static_cast<std::size_t>(message.source_index)];
            source.records.insert(
                source.records.end(),
                std::make_move_iterator(message.records.begin()),
                std::make_move_iterator(message.records.end()));
            rebuild_streamed_source(state, message.source_index);
            data_changed = true;
            break;
        }
        case TraceLoadMessage::Type::Finished:
            state.loader.active = false;
            state.loader.finished = true;
            state.loader.load_ms = message.load_ms;
            state.benchmark.load_ms = message.load_ms;
            state.status = "loaded " + std::to_string(state.history.sources.size()) + " trace source(s)";
            break;
        case TraceLoadMessage::Type::Failed:
            state.loader.active = false;
            state.loader.finished = true;
            state.benchmark.load_ms = 0.0;
            state.status = message.error;
            break;
        }
    }

    if (data_changed) {
        apply_server_entity_links(state.history);
        rebuild_source_metrics(state);
        rebuild_packet_log_rows(state);
        state.details_dirty = true;
        state.packet_details_dirty = true;
        state.cached_details.clear();
        if (state.selected_source < 0) {
            state.selected_source = 0;
        }
    }
}

void stream_trace_directory(
    const std::string directory,
    TraceLoadState& loader,
    std::shared_ptr<std::atomic_bool> cancel) {
    constexpr std::size_t records_per_chunk = 2048;
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
                completed_file_bytes,
                total_bytes,
                0.0,
                {}});

            KTraceStreamReader reader(source.path);
            std::vector<KTraceRecord> chunk;
            chunk.reserve(records_per_chunk);
            KTraceRecord record;
            while (!cancel->load() && reader.read_next(record)) {
                chunk.push_back(std::move(record));
                if (chunk.size() >= records_per_chunk) {
                    push_loader_message(loader, TraceLoadMessage{
                        TraceLoadMessage::Type::Records,
                        source_index,
                        {},
                        std::move(chunk),
                        completed_file_bytes + std::min(reader.position(), source.file_size),
                        total_bytes,
                        0.0,
                        {}});
                    chunk.clear();
                    chunk.reserve(records_per_chunk);
                }
            }
            if (!chunk.empty() && !cancel->load()) {
                push_loader_message(loader, TraceLoadMessage{
                    TraceLoadMessage::Type::Records,
                    source_index,
                    {},
                    std::move(chunk),
                    completed_file_bytes + std::min(reader.position(), source.file_size),
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

int row_count(const KTraceSourceHistory& source, const ViewerState& state, int source_index) {
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
            rows += packed_run_lane_count(component);
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

SourceMetrics compute_source_metrics(const KTraceSourceHistory& source, const ViewerState& state, int source_index) {
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
    const float lane_offset = static_cast<float>(event.lane) * packet_lane_spacing;
    return event.server_side ? base - lane_offset : base + lane_offset;
}

bool packet_event_matches_selection(const PacketEventInfo& event, const SelectedPacketChip& selected) {
    return selected.source_index == event.source_index && selected.event_index == event.record_index;
}

bool packet_event_hit(const ImVec2& center, const ImVec2& mouse) {
    const ImVec2 min(center.x - packet_marker_size * 0.5f, center.y - packet_marker_size * 0.5f);
    const ImVec2 max(center.x + packet_marker_size * 0.5f, center.y + packet_marker_size * 0.5f);
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
    const ImVec2 min(center.x - packet_marker_size * 0.5f, center.y - packet_marker_size * 0.5f);
    const ImVec2 max(center.x + packet_marker_size * 0.5f, center.y + packet_marker_size * 0.5f);
    const bool selected = packet_event_matches_selection(event, state.selected_packet);
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
    const ImU32 fill = event.send ? IM_COL32(53, 132, 246, 255) : IM_COL32(51, 177, 103, 255);
    draw->AddRectFilled(min, max, fill, 2.0f);
    if (!event.send) {
        draw->AddRectFilled(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(205, 255, 222, 210), 1.0f);
    }
    if (selected) {
        draw->AddRect(ImVec2(min.x - 4.0f, min.y - 4.0f), ImVec2(max.x + 4.0f, max.y + 4.0f), IM_COL32(255, 218, 112, 255), 3.0f, 0, 2.6f);
        draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(255, 246, 205, 255), 2.0f, 0, 1.4f);
    } else if (highlighted) {
        draw->AddRect(ImVec2(min.x - 1.5f, min.y - 1.5f), ImVec2(max.x + 1.5f, max.y + 1.5f), IM_COL32(255, 218, 112, 155), 2.0f, 0, 1.2f);
    }
    if (hovered && !selected) {
        draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(230, 238, 255, 210), 2.0f, 0, 1.0f);
    }
    if (hovered) {
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
        ImGui::BeginChild("packet_event_log", ImVec2(0.0f, -180.0f), true);
        ImGui::TextColored(ImVec4(0.56f, 0.62f, 0.70f, 1.0f), "No packet events.");
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
            if (packet_event_hit(center, mouse)) {
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
                    state);
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
                        for (const KTraceFrameCell& cell : component.runs[0].frames) {
                            if (cell.frame < first_visible_frame) {
                                continue;
                            }
                            if (cell.frame > last_visible_frame) {
                                break;
                            }
                            draw_cell(draw, source, cell, body_origin, scroll_x, scroll_y, min_frame, row, entity, component.component, 0, source_index, state);
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
                        state);
                    draw->PopClipRect();
                }
            }
            ++row;
            if (!expanded_component) {
                continue;
            }
            const RunLaneLayout run_layout = pack_run_lanes(component);
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
                if (item.run == nullptr) {
                    continue;
                }
                const int parent_lane = run_parent_lane(run_layout.items, run_index);
                const int parent_row = parent_lane < 0 ? run_base_row - 1 : run_base_row + parent_lane;
                const int start_lane = run_lane_at_frame(item, item.run->start_frame);
                if (start_lane != unassigned_lane) {
                    draw_run_drop_link(
                        draw,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        item.run->start_frame,
                        parent_row,
                        run_base_row + start_lane,
                        clip_min,
                        clip_max);
                }
                draw_run_links(
                    draw,
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
                for (const KTraceFrameCell& cell : item.run->frames) {
                    if (cell.frame < first_visible_frame) {
                        continue;
                    }
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
                        state);
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
        render_merged_cue_details(state.cached_details);
        for (const SelectedRecordDetail& detail : state.cached_details) {
            render_record_detail(detail, &state.cached_details);
        }
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

    render_merged_cue_details(state.cached_details);
    for (const SelectedRecordDetail& detail : state.cached_details) {
        render_record_detail(detail, &state.cached_details);
    }
    ImGui::EndChild();
    state.current_timing.details_ms += elapsed_ms(start);
}

void render_app(ViewerState& state) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::Begin("Kage Sync Trace Viewer", nullptr,
        ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(ImVec4(0.86f, 0.91f, 0.98f, 1.0f), "Kage Sync Trace Viewer");
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
    if (state.loader.active || state.loader.bytes_read != 0U || state.loader.total_bytes != 0U) {
        const float progress = state.loader.total_bytes != 0U
            ? std::min(1.0f, static_cast<float>(
                  static_cast<double>(state.loader.bytes_read) / static_cast<double>(state.loader.total_bytes)))
            : 0.0f;
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(0.48f, 0.55f, 0.65f, 1.0f),
            "%.0f%% loaded",
            static_cast<double>(progress) * 100.0);
    }
    if (!state.status.empty()) {
        ImGui::TextColored(ImVec4(0.58f, 0.66f, 0.76f, 1.0f), "%s", state.status.c_str());
    }
    render_directory_picker(state);
    render_legend();
    ImGui::Spacing();

    const bool has_packet_logs = packet_logs_enabled_in_trace(state);
    if (!has_packet_logs && state.mode == ViewerMode::EventLog) {
        state.mode = ViewerMode::Frames;
    }
    if (has_packet_logs) {
        const char* current_mode = state.mode == ViewerMode::Frames ? "Frames" : "Event Log";
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
            ImGui::EndCombo();
        }
    }

    if (state.mode == ViewerMode::EventLog) {
        render_event_log(state);
    } else if (ImGui::BeginTabBar("sources")) {
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
        ? std::string("/tmp/kage_sync_trace_viewer_benchmark.json")
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
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
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
    GLFWwindow* window = glfwCreateWindow(1400, 900, "Kage Sync Trace Viewer", nullptr, nullptr);
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

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
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
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        poll_control_server(control_server, state, automation_input, window);
#endif
        process_loader_messages(state);
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        apply_automation_input(automation_input);
#endif
        ImGui::NewFrame();
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
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
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
        if (state.screenshot_requested) {
            const std::string path = screenshot_output_path(state);
            std::string error;
            if (write_framebuffer_png(path, width, height, error)) {
                state.last_screenshot_path = path;
                state.status = "screenshot saved to " + path;
                std::fprintf(stderr, "screenshot saved to %s\n", path.c_str());
            } else {
                state.screenshot_failed = true;
                state.status = "screenshot failed: " + error;
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

#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    stop_control_server(control_server);
#endif
    stop_loading_directory(state);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
#if defined(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    return state.screenshot_failed ? 1 : 0;
#else
    return 0;
#endif
}
