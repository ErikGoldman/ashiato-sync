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
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

using namespace kage::sync;

constexpr float label_width = 340.0f;
constexpr float row_height = 24.0f;
constexpr float frame_pitch = 18.0f;
constexpr float pill_width = 14.0f;
constexpr float pill_height = 14.0f;
constexpr float timeline_header_height = 24.0f;
constexpr ImU32 timeline_link_color = IM_COL32(194, 200, 210, 125);

struct SelectedCell {
    int source_index = -1;
    ClientEntityNetworkId network_id = invalid_client_entity_network_id;
    ecs::Entity component{};
    SyncFrame frame = 0;
    bool branch = false;
    std::vector<std::uint32_t> event_indices;
};

bool operator==(const SelectedCell& lhs, const SelectedCell& rhs) {
    return lhs.source_index == rhs.source_index &&
        lhs.network_id == rhs.network_id &&
        lhs.component == rhs.component &&
        lhs.frame == rhs.frame &&
        lhs.branch == rhs.branch &&
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

struct DirectoryPickerEntry {
    std::string name;
    std::string path;
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
    int selected_source = 0;
    BenchmarkState benchmark;
    FrameTiming current_timing;
    SelectedCell cached_detail_selection;
    std::vector<SelectedRecordDetail> cached_details;
    std::vector<SourceMetrics> source_metrics;
    std::vector<SelectedCell> benchmark_candidates;
    std::array<char, 1024> picker_path{};
    std::vector<DirectoryPickerEntry> picker_entries;
    std::string picker_error;
    int picker_selected = -1;
    std::string screenshot_path;
    std::string last_screenshot_path;
    std::string control_socket_path;
    std::unordered_set<EntityExpansionKey, EntityExpansionKeyHash> expanded_entities;
    std::unordered_set<ComponentExpansionKey, ComponentExpansionKeyHash> expanded_components;
    std::uint32_t screenshot_counter = 0;
    bool screenshot_requested = false;
    bool screenshot_failed = false;
    bool selected_source_dirty = true;
    bool details_dirty = true;
};

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

void load_directory(ViewerState& state);

struct BranchRenderItem {
    const KTraceEntityBranch* branch = nullptr;
    int lane = 0;
    SyncFrame end_frame = 0;
    std::vector<std::pair<SyncFrame, int>> frame_lanes;
};

constexpr int component_lane = -1;
constexpr int unassigned_lane = -2;

struct BranchLaneLayout {
    std::vector<BranchRenderItem> items;
    int lane_count = 0;
};

enum class CellVisualKind {
    Unknown,
    Applied,
    ServerGolden,
    Interpolated,
    Predicted,
    CorrectPrediction,
    Mispredicted,
    Starved,
    Removed,
    Resimulated
};

struct CellVisual {
    CellVisualKind kind = CellVisualKind::Unknown;
    ImU32 fill = IM_COL32(120, 120, 120, 255);
    ImU32 accent = IM_COL32(255, 255, 255, 180);
    const char* label = "unknown";
};

bool has_state(const KTraceFrameCell& cell, KTraceCellState state) {
    return (cell.state_mask & static_cast<std::uint16_t>(state)) != 0U;
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end = Clock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

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

void append_u32_be(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    out.push_back(static_cast<unsigned char>(value & 0xffU));
}

std::uint32_t crc32_bytes(const unsigned char* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t adler32_bytes(const std::vector<unsigned char>& data) {
    constexpr std::uint32_t mod = 65521U;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (unsigned char byte : data) {
        a = (a + byte) % mod;
        b = (b + a) % mod;
    }
    return (b << 16U) | a;
}

void append_png_chunk(std::vector<unsigned char>& png, const char type[4], const std::vector<unsigned char>& data) {
    append_u32_be(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t crc_start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    append_u32_be(png, crc32_bytes(png.data() + crc_start, png.size() - crc_start));
}

std::vector<unsigned char> zlib_store(const std::vector<unsigned char>& data) {
    std::vector<unsigned char> zlib;
    zlib.reserve(data.size() + (data.size() / 65535U + 1U) * 5U + 6U);
    zlib.push_back(0x78U);
    zlib.push_back(0x01U);

    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const std::uint16_t block_size = static_cast<std::uint16_t>(std::min<std::size_t>(remaining, 65535U));
        const bool final_block = offset + block_size == data.size();
        zlib.push_back(final_block ? 0x01U : 0x00U);
        zlib.push_back(static_cast<unsigned char>(block_size & 0xffU));
        zlib.push_back(static_cast<unsigned char>((block_size >> 8U) & 0xffU));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~block_size);
        zlib.push_back(static_cast<unsigned char>(inverse & 0xffU));
        zlib.push_back(static_cast<unsigned char>((inverse >> 8U) & 0xffU));
        zlib.insert(zlib.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + block_size));
        offset += block_size;
    }

    append_u32_be(zlib, adler32_bytes(data));
    return zlib;
}

bool write_png_rgb(const std::string& path, int width, int height, const std::vector<unsigned char>& bottom_up_rgb, std::string& error) {
    if (bottom_up_rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U) {
        error = "invalid screenshot buffer size";
        return false;
    }

    std::vector<unsigned char> scanlines;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 3U;
    scanlines.reserve((row_bytes + 1U) * static_cast<std::size_t>(height));
    for (int row = height - 1; row >= 0; --row) {
        scanlines.push_back(0U);
        const unsigned char* row_start = bottom_up_rgb.data() + static_cast<std::size_t>(row) * row_bytes;
        scanlines.insert(scanlines.end(), row_start, row_start + row_bytes);
    }

    std::vector<unsigned char> png;
    const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    png.insert(png.end(), signature, signature + 8);

    std::vector<unsigned char> ihdr;
    append_u32_be(ihdr, static_cast<std::uint32_t>(width));
    append_u32_be(ihdr, static_cast<std::uint32_t>(height));
    ihdr.push_back(8U);  // bit depth
    ihdr.push_back(2U);  // RGB color
    ihdr.push_back(0U);  // deflate
    ihdr.push_back(0U);  // adaptive filters
    ihdr.push_back(0U);  // no interlace
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", zlib_store(scanlines));
    append_png_chunk(png, "IEND", {});

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

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        error = "failed to open screenshot file";
        return false;
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
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

#ifndef _WIN32
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

SyncFrame component_end_frame(const KTraceComponentRow& component, SyncFrame fallback) {
    SyncFrame end = fallback;
    for (const KTraceFrameCell& cell : component.cells) {
        end = std::max(end, cell.frame);
    }
    return end;
}

SyncFrame branch_end_frame(const KTraceEntityBranch& branch) {
    SyncFrame end = branch.from_frame;
    for (const KTraceComponentRow& component : branch.components) {
        end = component_end_frame(component, end);
    }
    return end;
}

const KTraceComponentRow* find_branch_component(const KTraceEntityBranch& branch, ecs::Entity component) {
    const auto found = std::find_if(branch.components.begin(), branch.components.end(), [component](const KTraceComponentRow& row) {
        return row.component == component;
    });
    return found != branch.components.end() ? &*found : nullptr;
}

SyncFrame branch_component_end_frame(const KTraceEntityBranch& branch, ecs::Entity component, SyncFrame fallback) {
    const KTraceComponentRow* row = find_branch_component(branch, component);
    return row != nullptr ? component_end_frame(*row, fallback) : fallback;
}

std::vector<SyncFrame> branch_component_frames(const KTraceEntityBranch& branch, ecs::Entity component) {
    std::vector<SyncFrame> frames;
    frames.push_back(branch.from_frame);
    const KTraceComponentRow* row = find_branch_component(branch, component);
    if (row != nullptr) {
        for (const KTraceFrameCell& cell : row->cells) {
            frames.push_back(cell.frame);
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

bool lane_has_gap_before(SyncFrame lane_end, SyncFrame frame) {
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

int first_available_branch_lane(const std::vector<SyncFrame>& lane_ends, int first_lane, SyncFrame frame) {
    int lane = std::max(0, first_lane);
    for (; lane < static_cast<int>(lane_ends.size()); ++lane) {
        if (lane_has_gap_before(lane_ends[static_cast<std::size_t>(lane)], frame)) {
            return lane;
        }
    }
    return lane;
}

int branch_lane_at_frame(const BranchRenderItem& item, SyncFrame frame);
int branch_parent_lane(const std::vector<BranchRenderItem>& branches, std::size_t branch_index);

BranchLaneLayout pack_branch_lanes(const KTraceEntityRow& entity, const KTraceComponentRow& component) {
    BranchLaneLayout layout;
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        if (find_branch_component(branch, component.component) == nullptr) {
            continue;
        }
        layout.items.push_back(BranchRenderItem{
            &branch,
            0,
            branch_component_end_frame(branch, component.component, branch.from_frame),
            {}});
    }
    std::sort(layout.items.begin(), layout.items.end(), [](const BranchRenderItem& lhs, const BranchRenderItem& rhs) {
        if (lhs.branch->from_frame != rhs.branch->from_frame) {
            return lhs.branch->from_frame < rhs.branch->from_frame;
        }
        return lhs.end_frame < rhs.end_frame;
    });

    std::vector<SyncFrame> lane_ends;
    SyncFrame component_end = component_end_frame(component, 0);
    for (BranchRenderItem& item : layout.items) {
        const std::vector<SyncFrame> frames = branch_component_frames(*item.branch, component.component);
        int current_lane = unassigned_lane;
        item.frame_lanes.reserve(frames.size());
        const std::size_t item_index = static_cast<std::size_t>(&item - layout.items.data());
        const int parent_lane = branch_parent_lane(layout.items, item_index);
        for (SyncFrame frame : frames) {
            int lane = unassigned_lane;
            if (current_lane == unassigned_lane) {
                lane = first_available_branch_lane(lane_ends, parent_lane + 1, frame);
            } else {
                for (int candidate = component_lane; candidate < current_lane; ++candidate) {
                    if (candidate >= static_cast<int>(lane_ends.size())) {
                        break;
                    }
                    if (lane_has_gap_before(lane_end_at(lane_ends, component_end, candidate), frame)) {
                        lane = candidate;
                        break;
                    }
                }
                if (lane == unassigned_lane) {
                    lane = current_lane;
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

int branch_lane_at_frame(const BranchRenderItem& item, SyncFrame frame) {
    for (const auto& frame_lane : item.frame_lanes) {
        if (frame_lane.first == frame) {
            return frame_lane.second;
        }
    }
    return unassigned_lane;
}

int branch_parent_lane(const std::vector<BranchRenderItem>& branches, std::size_t branch_index) {
    const BranchRenderItem& branch = branches[branch_index];
    int parent_lane = component_lane;
    SyncFrame parent_from = 0;
    for (std::size_t index = 0; index < branches.size(); ++index) {
        if (index == branch_index) {
            continue;
        }
        const BranchRenderItem& candidate = branches[index];
        const int candidate_lane = branch_lane_at_frame(candidate, branch.branch->from_frame);
        if (candidate.branch->from_frame >= branch.branch->from_frame || candidate_lane == unassigned_lane) {
            continue;
        }
        if (parent_lane == component_lane || candidate.branch->from_frame > parent_from) {
            parent_lane = candidate_lane;
            parent_from = candidate.branch->from_frame;
        }
    }
    return parent_lane;
}

int packed_branch_lane_count(const KTraceEntityRow& entity, ecs::Entity component_id) {
    for (const KTraceComponentRow& component : entity.components) {
        if (component.component == component_id) {
            return pack_branch_lanes(entity, component).lane_count;
        }
    }
    KTraceComponentRow component;
    component.component = component_id;
    return pack_branch_lanes(entity, component).lane_count;
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
    case SyncTraceEventType::CueInvoked: return "cue invoked";
    case SyncTraceEventType::CueRolledBack: return "cue rolled back";
    case SyncTraceEventType::CueReceived: return "cue received";
    case SyncTraceEventType::TagReceived: return "tag received";
    case SyncTraceEventType::EntityDestroyed: return "entity destroyed";
    case SyncTraceEventType::ResimulatedFrameComponent: return "resimulated frame component";
    case SyncTraceEventType::ComponentName: return "component name";
    case SyncTraceEventType::RollbackReason: return "rollback reason";
    }
    return "unknown";
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
        draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(min.x + 5.0f, min.y + 5.0f), visual.accent);
        draw->AddRectFilled(ImVec2(min.x + 8.0f, min.y + 2.0f), ImVec2(min.x + 11.0f, min.y + 5.0f), visual.accent);
        draw->AddRectFilled(ImVec2(min.x + 5.0f, min.y + 8.0f), ImVec2(min.x + 8.0f, min.y + 11.0f), visual.accent);
        draw->AddRectFilled(ImVec2(min.x + 11.0f, min.y + 8.0f), ImVec2(min.x + 14.0f, min.y + 11.0f), visual.accent);
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
    const KTraceComponentRow& component,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame first_visible_frame,
    SyncFrame last_visible_frame,
    int row,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    for (std::size_t index = 1; index < component.cells.size(); ++index) {
        const SyncFrame previous = component.cells[index - 1U].frame;
        const SyncFrame current = component.cells[index].frame;
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

void draw_branch_component_links(
    ImDrawList* draw,
    const BranchRenderItem& item,
    const KTraceComponentRow& component,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame first_visible_frame,
    SyncFrame last_visible_frame,
    int branch_base_row,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    for (std::size_t index = 1; index < component.cells.size(); ++index) {
        const SyncFrame previous = component.cells[index - 1U].frame;
        const SyncFrame current = component.cells[index].frame;
        if (current < first_visible_frame) {
            continue;
        }
        if (previous > last_visible_frame) {
            break;
        }
        if (current != previous + 1U) {
            continue;
        }
        const int previous_lane = branch_lane_at_frame(item, previous);
        const int current_lane = branch_lane_at_frame(item, current);
        if (previous_lane == unassigned_lane || current_lane == unassigned_lane) {
            continue;
        }
        draw_timeline_step_link(
            draw,
            ImVec2(frame_center_x(origin, scroll_x, min_frame, previous), row_center_y(origin, scroll_y, branch_base_row + previous_lane)),
            ImVec2(frame_center_x(origin, scroll_x, min_frame, current), row_center_y(origin, scroll_y, branch_base_row + current_lane)),
            clip_min,
            clip_max);
    }
}

void draw_branch_drop_link(
    ImDrawList* draw,
    const ImVec2& origin,
    float scroll_x,
    float scroll_y,
    SyncFrame min_frame,
    SyncFrame frame,
    int parent_row,
    int branch_row,
    const ImVec2& clip_min,
    const ImVec2& clip_max) {
    const float x = frame_center_x(origin, scroll_x, min_frame, frame);
    draw_timeline_link(
        draw,
        ImVec2(x, row_center_y(origin, scroll_y, parent_row)),
        ImVec2(x, row_center_y(origin, scroll_y, branch_row)),
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
    bool branch,
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

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
    const bool selected = state.selected.source_index == source_index &&
        state.selected.network_id == entity.client_network_id &&
        state.selected.component == component &&
        state.selected.frame == cell.frame &&
        state.selected.branch == branch &&
        state.selected.event_indices == cell.event_indices;
    if (selected) {
        draw->AddRect(ImVec2(min.x - 2.0f, min.y - 2.0f), ImVec2(max.x + 2.0f, max.y + 2.0f), IM_COL32(255, 230, 155, 255), 0.0f, 0, 2.0f);
    } else if (hovered) {
        draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(230, 238, 255, 210), 0.0f, 0, 1.0f);
    }
    if (hovered) {
        ImGui::SetTooltip("%s frame %u", visual.label, cell.frame);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            SelectedCell selected{source_index, entity.client_network_id, component, cell.frame, branch, cell.event_indices};
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

bool component_has_resim(const KTraceEntityRow& entity, ecs::Entity component_id) {
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& component : branch.components) {
            if (component.component != component_id) {
                continue;
            }
            for (const KTraceFrameCell& cell : component.cells) {
                if (has_state(cell, KTraceCellState::Resimulated)) {
                    return true;
                }
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
    const bool enabled = component_has_resim(entity, component);
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

void append_entity_frame_event_indices(
    const KTraceEntityRow& entity,
    SyncFrame frame,
    std::vector<std::uint32_t>& out) {
    for (const KTraceComponentRow& component : entity.components) {
        for (const KTraceFrameCell& cell : component.cells) {
            if (cell.frame == frame) {
                out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
            }
        }
    }
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& component : branch.components) {
            for (const KTraceFrameCell& cell : component.cells) {
                if (cell.frame == frame) {
                    out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
                }
            }
        }
    }
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
        for (const KTraceFrameCell& cell : component.cells) {
            if (cell.frame == frame) {
                out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
            }
        }
    }
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& component : branch.components) {
            if (component.component != component_id) {
                continue;
            }
            for (const KTraceFrameCell& cell : component.cells) {
                if (cell.frame == frame) {
                    out.insert(out.end(), cell.event_indices.begin(), cell.event_indices.end());
                }
            }
        }
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
        for (const KTraceComponentRow& component : entity.components) {
            for (const KTraceFrameCell& cell : component.cells) {
                if (cell.frame == frame) {
                    aggregate_entity_cell(summary, cell);
                }
            }
        }
        for (const KTraceEntityBranch& branch : entity.rollback_branches) {
            for (const KTraceComponentRow& component : branch.components) {
                for (const KTraceFrameCell& cell : component.cells) {
                    if (cell.frame == frame) {
                        aggregate_entity_cell(summary, cell);
                    }
                }
            }
        }
        if (summary != SummaryState::None) {
            KTraceFrameCell cell = summary_cell(frame, summary);
            append_entity_frame_event_indices(entity, frame, cell.event_indices);
            draw_cell(draw, source, cell, origin, scroll_x, scroll_y, min_frame, row, entity, ecs::Entity{}, false, source_index, state);
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
        for (const KTraceFrameCell& cell : component.cells) {
            if (cell.frame == frame) {
                aggregate_component_cell(summary, cell);
            }
        }
        for (const KTraceEntityBranch& branch : entity.rollback_branches) {
            for (const KTraceComponentRow& branch_component : branch.components) {
                if (branch_component.component != component.component) {
                    continue;
                }
                for (const KTraceFrameCell& cell : branch_component.cells) {
                    if (cell.frame == frame) {
                        aggregate_component_cell(summary, cell);
                    }
                }
            }
        }
        if (summary != SummaryState::None) {
            KTraceFrameCell cell = summary_cell(frame, summary);
            append_component_frame_event_indices(entity, component.component, frame, cell.event_indices);
            draw_cell(draw, source, cell, origin, scroll_x, scroll_y, min_frame, row, entity, component.component, false, source_index, state);
            ++drawn;
        }
    }
    return drawn;
}

SummaryState entity_summary_at_frame(const KTraceEntityRow& entity, SyncFrame frame) {
    SummaryState summary = SummaryState::None;
    for (const KTraceComponentRow& component : entity.components) {
        for (const KTraceFrameCell& cell : component.cells) {
            if (cell.frame == frame) {
                aggregate_entity_cell(summary, cell);
            }
        }
    }
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& component : branch.components) {
            for (const KTraceFrameCell& cell : component.cells) {
                if (cell.frame == frame) {
                    aggregate_entity_cell(summary, cell);
                }
            }
        }
    }
    return summary;
}

SummaryState component_summary_at_frame(
    const KTraceEntityRow& entity,
    const KTraceComponentRow& component,
    SyncFrame frame) {
    SummaryState summary = SummaryState::None;
    for (const KTraceFrameCell& cell : component.cells) {
        if (cell.frame == frame) {
            aggregate_component_cell(summary, cell);
        }
    }
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& branch_component : branch.components) {
            if (branch_component.component != component.component) {
                continue;
            }
            for (const KTraceFrameCell& cell : branch_component.cells) {
                if (cell.frame == frame) {
                    aggregate_component_cell(summary, cell);
                }
            }
        }
    }
    return summary;
}

std::vector<SyncFrame> entity_summary_frames(const KTraceEntityRow& entity) {
    std::vector<SyncFrame> frames;
    for (const KTraceComponentRow& component : entity.components) {
        for (const KTraceFrameCell& cell : component.cells) {
            frames.push_back(cell.frame);
        }
    }
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& component : branch.components) {
            for (const KTraceFrameCell& cell : component.cells) {
                frames.push_back(cell.frame);
            }
        }
    }
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
        for (const KTraceFrameCell& cell : component.cells) {
            frames.push_back(cell.frame);
        }
    }
    for (const KTraceEntityBranch& branch : entity.rollback_branches) {
        for (const KTraceComponentRow& component : branch.components) {
            if (component.component != component_id) {
                continue;
            }
            for (const KTraceFrameCell& cell : component.cells) {
                frames.push_back(cell.frame);
            }
        }
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
    bool branch,
    int row) {
    out.push_back(TimelineNavCell{
        SelectedCell{source_index, entity.client_network_id, component, cell.frame, branch, cell.event_indices},
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
                append_nav_cell(cells, source_index, entity, ecs::Entity{}, cell, false, row);
            }
            ++row;
            continue;
        }
        ++row;
        for (const KTraceComponentRow& component : entity.components) {
            const bool collapsible_component = component_has_resim(entity, component.component);
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
                    append_nav_cell(cells, source_index, entity, component.component, cell, false, row);
                }
                ++row;
                continue;
            }
            for (const KTraceFrameCell& cell : component.cells) {
                append_nav_cell(cells, source_index, entity, component.component, cell, false, row);
            }
            ++row;
            const int branch_base_row = row;
            const BranchLaneLayout branch_layout = pack_branch_lanes(entity, component);
            for (const BranchRenderItem& item : branch_layout.items) {
                for (const KTraceComponentRow& branch_component : item.branch->components) {
                    if (branch_component.component != component.component) {
                        continue;
                    }
                    for (const KTraceFrameCell& cell : branch_component.cells) {
                        const int cell_lane = branch_lane_at_frame(item, cell.frame);
                        if (cell_lane != unassigned_lane) {
                            append_nav_cell(cells, source_index, entity, branch_component.component, cell, true, branch_base_row + cell_lane);
                        }
                    }
                }
            }
            row += branch_layout.lane_count;
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

void load_directory(ViewerState& state) {
    try {
        const auto start = Clock::now();
        KTraceReader reader;
        state.history = reader.read_directory(state.directory.data());
        std::sort(state.history.sources.begin(), state.history.sources.end(), [](const KTraceSourceHistory& lhs, const KTraceSourceHistory& rhs) {
            if (lhs.role != rhs.role) {
                return rhs.role == SyncTraceRole::Server;
            }
            return lhs.client < rhs.client;
        });
        state.expanded_entities.clear();
        state.expanded_components.clear();
        rebuild_source_metrics(state);
        state.benchmark.load_ms = elapsed_ms(start);
        state.status = "loaded " + std::to_string(state.history.sources.size()) + " trace source(s)";
        state.selected = {};
        state.details_dirty = true;
        state.cached_details.clear();
        state.selected_source = std::min(state.selected_source, static_cast<int>(state.history.sources.size()) - 1);
        if (state.selected_source < 0) {
            state.selected_source = 0;
        }
        state.selected_source_dirty = true;
    } catch (const std::exception& error) {
        state.history.sources.clear();
        state.benchmark.load_ms = 0.0;
        state.selected = {};
        state.expanded_entities.clear();
        state.expanded_components.clear();
        state.details_dirty = true;
        state.cached_details.clear();
        state.source_metrics.clear();
        state.benchmark_candidates.clear();
        state.status = error.what();
    }
}

std::string path_to_string(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

void set_char_buffer(std::array<char, 1024>& buffer, const std::string& value) {
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
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
            if (ImGui::Selectable(entry.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                state.picker_selected = index;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    set_char_buffer(state.picker_path, entry.path);
                    refresh_directory_picker(state);
                }
            }
        }
    }
    ImGui::EndChild();

    const bool can_choose = state.picker_error.empty();
    if (!can_choose) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Open This Directory", ImVec2(150.0f, 0.0f))) {
        set_char_buffer(state.directory, path_to_string(current_picker_path(state)));
        load_directory(state);
        ImGui::CloseCurrentPopup();
    }
    if (!can_choose) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (state.picker_selected < 0 || state.picker_selected >= static_cast<int>(state.picker_entries.size())) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Open Selected", ImVec2(126.0f, 0.0f))) {
        const DirectoryPickerEntry& entry = state.picker_entries[static_cast<std::size_t>(state.picker_selected)];
        set_char_buffer(state.directory, entry.path);
        load_directory(state);
        ImGui::CloseCurrentPopup();
    }
    if (state.picker_selected < 0 || state.picker_selected >= static_cast<int>(state.picker_entries.size())) {
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
            if (component_has_resim(entity, component.component) &&
                !component_expanded(state, source_index, entity, component.component)) {
                continue;
            }
            rows += packed_branch_lane_count(entity, component.component);
        }
    }
    return rows;
}

int cell_count(const KTraceSourceHistory& source) {
    int cells = 0;
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceComponentRow& component : entity.components) {
            cells += static_cast<int>(component.cells.size());
        }
        for (const KTraceEntityBranch& branch : entity.rollback_branches) {
            for (const KTraceComponentRow& component : branch.components) {
                cells += static_cast<int>(component.cells.size());
            }
        }
    }
    return cells;
}

SourceMetrics compute_source_metrics(const KTraceSourceHistory& source, const ViewerState& state, int source_index) {
    SourceMetrics metrics;
    frame_range(source, metrics.min_frame, metrics.max_frame);
    for (const KTraceEntityRow& entity : source.entities) {
        for (const KTraceEntityBranch& branch : entity.rollback_branches) {
            const SyncFrame end = branch_end_frame(branch);
            metrics.max_frame = std::max(metrics.max_frame, end);
            if (end != std::numeric_limits<SyncFrame>::max()) {
                metrics.max_frame = std::max(metrics.max_frame, end + 1U);
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
                for (const KTraceFrameCell& cell : component.cells) {
                    state.benchmark_candidates.push_back(
                        SelectedCell{source_index, entity.client_network_id, component.component, cell.frame, false});
                }
            }
            for (const KTraceEntityBranch& branch : entity.rollback_branches) {
                for (const KTraceComponentRow& component : branch.components) {
                    for (const KTraceFrameCell& cell : component.cells) {
                        state.benchmark_candidates.push_back(
                            SelectedCell{source_index, entity.client_network_id, component.component, cell.frame, true});
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
                !state.selected.branch;
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
            const bool collapsible_component = component_has_resim(entity, component.component);
            const bool expanded_component =
                !collapsible_component || component_expanded(state, source_index, entity, component.component);
            if (row_visible(row)) {
                const bool selected_row = state.selected.source_index == source_index &&
                    state.selected.network_id == entity.client_network_id &&
                    state.selected.component == component.component &&
                    !state.selected.branch;
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
                    draw_consecutive_cell_links(
                        draw,
                        component,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        first_visible_frame,
                        last_visible_frame,
                        row,
                        clip_min,
                        clip_max);
                    for (const KTraceFrameCell& cell : component.cells) {
                        if (cell.frame < first_visible_frame) {
                            continue;
                        }
                        if (cell.frame > last_visible_frame) {
                            break;
                        }
                        draw_cell(draw, source, cell, body_origin, scroll_x, scroll_y, min_frame, row, entity, component.component, false, source_index, state);
                        ++drawn_cells;
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
            const BranchLaneLayout branch_layout = pack_branch_lanes(entity, component);
            const int branch_base_row = row;
            for (int lane = 0; lane < branch_layout.lane_count; ++lane) {
                const int branch_row = branch_base_row + lane;
                if (row_visible(branch_row)) {
                    bool selected_row = false;
                    if (state.selected.source_index == source_index &&
                        state.selected.network_id == entity.client_network_id &&
                        state.selected.component == component.component &&
                        state.selected.branch) {
                        for (const BranchRenderItem& item : branch_layout.items) {
                            if (branch_lane_at_frame(item, state.selected.frame) == lane) {
                                selected_row = true;
                                break;
                            }
                        }
                    }
                    draw_row_background(branch_row, selected_row);
                    const float y = body_origin.y + static_cast<float>(branch_row) * row_height - scroll_y + 4.0f;
                    const std::string label = "    rollback lane " + std::to_string(lane + 1);
                    draw->AddText(ImVec2(sticky_label_x + 32.0f, y), IM_COL32(237, 183, 112, 255), label.c_str());
                    ++drawn_rows;
                }
            }
            draw->PushClipRect(ImVec2(frame_clip_min_x, clip_min.y), clip_max, true);
            for (std::size_t branch_index = 0; branch_index < branch_layout.items.size(); ++branch_index) {
                const BranchRenderItem& item = branch_layout.items[branch_index];
                const int parent_lane = branch_parent_lane(branch_layout.items, branch_index);
                const int parent_row = parent_lane < 0 ? branch_base_row - 1 : branch_base_row + parent_lane;
                const int start_lane = branch_lane_at_frame(item, item.branch->from_frame);
                if (start_lane != unassigned_lane) {
                    draw_branch_drop_link(
                        draw,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        item.branch->from_frame,
                        parent_row,
                        branch_base_row + start_lane,
                        clip_min,
                        clip_max);
                }
                for (const KTraceComponentRow& branch_component : item.branch->components) {
                    if (branch_component.component != component.component) {
                        continue;
                    }
                    draw_branch_component_links(
                        draw,
                        item,
                        branch_component,
                        body_origin,
                        scroll_x,
                        scroll_y,
                        min_frame,
                        first_visible_frame,
                        last_visible_frame,
                        branch_base_row,
                        clip_min,
                        clip_max);
                    for (const KTraceFrameCell& cell : branch_component.cells) {
                        if (cell.frame < first_visible_frame) {
                            continue;
                        }
                        if (cell.frame > last_visible_frame) {
                            break;
                        }
                        const int cell_lane = branch_lane_at_frame(item, cell.frame);
                        if (cell_lane == unassigned_lane) {
                            continue;
                        }
                        const int cell_row = branch_base_row + cell_lane;
                        if (!row_visible(cell_row)) {
                            continue;
                        }
                        const bool force_predicted_visual =
                            cell_lane != item.lane &&
                            !has_state(cell, KTraceCellState::Mispredicted) &&
                            !has_state(cell, KTraceCellState::Starved) &&
                            !has_state(cell, KTraceCellState::Removed) &&
                            !has_state(cell, KTraceCellState::EntityDestroyed);
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
                            branch_component.component,
                            true,
                            source_index,
                            state,
                            force_predicted_visual);
                        ++drawn_cells;
                    }
                }
            }
            draw->PopClipRect();
            row += branch_layout.lane_count;
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

void render_record_detail(const SelectedRecordDetail& detail) {
    if (detail.record == nullptr) {
        return;
    }
    const KTraceRecord& record = *detail.record;
    const SyncTraceEvent& event = record.event;
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

void render_details(ViewerState& state) {
    const auto start = Clock::now();
    ImGui::BeginChild("details", ImVec2(0.0f, 0.0f), true);
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

    for (const SelectedRecordDetail& detail : state.cached_details) {
        render_record_detail(detail);
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
    ImGui::InputText("##trace_directory", state.directory.data(), state.directory.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(84.0f, 0.0f))) {
        open_directory_picker(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(70.0f, 0.0f))) {
        load_directory(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(70.0f, 0.0f))) {
        load_directory(state);
    }
    if (!state.status.empty()) {
        ImGui::TextColored(ImVec4(0.58f, 0.66f, 0.76f, 1.0f), "%s", state.status.c_str());
    }
    render_directory_picker(state);
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
            } else if (arg == "--control-socket") {
                state.control_socket_path = require_value();
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
        poll_control_server(control_server, state, automation_input, window);
        apply_automation_input(automation_input);
        ImGui::NewFrame();
        if (!state.benchmark.options.enabled && ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
            request_screenshot(state);
        }

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

    stop_control_server(control_server);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return state.screenshot_failed ? 1 : 0;
}
