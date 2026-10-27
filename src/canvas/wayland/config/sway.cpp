// Display images inside a terminal
// Copyright (C) 2023  JustKidding
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "sway.hpp"
#include "application.hpp"
#include "os.hpp"
#include "tmux.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <stack>
#include <string>

#include <fmt/format.h>
#include <range/v3/all.hpp>

using njson = nlohmann::json;

constexpr auto ipc_magic = std::string_view{"i3-ipc"};
constexpr auto ipc_header_size = ipc_magic.size() + 8;

SwaySocket::SwaySocket(const std::string_view endpoint)
    : socket(endpoint),
      logger(spdlog::get("wayland"))
{
    logger->info("Using sway socket {}", endpoint);
    set_active_output_info();
    try {
        // prefer the terminal's output over the focused one
        std::ignore = get_window_info();
    } catch (const njson::exception &err) {
        logger->debug("Could not find terminal window, using focused output");
    }
}

struct __attribute__((packed)) ipc_header {
    std::array<char, ipc_magic.size()> magic;
    uint32_t len;
    uint32_t type;
};

auto SwaySocket::get_window_info() -> struct WaylandWindowGeometry {
    const std::scoped_lock lock{mutex};
    const auto nodes = get_nodes();
    const auto window = get_active_window(nodes);
    const auto &rect = window.at("rect");
    const int width = rect.at("width");
    const int height = rect.at("height");
    const int xcoord = rect.at("x");
    const int ycoord = rect.at("y");
    // the terminal can be on any output, each with its own scale
    set_output_info_at(xcoord + (width / 2), ycoord + (height / 2));
    return {.width = width,
            .height = height,
            .x = xcoord - output_info.x,
            .y = ycoord - output_info.y,
            .scale = output_info.scale};
}

auto SwaySocket::get_active_window(const std::vector<nlohmann::json>& nodes) -> nlohmann::json
{
    // once found, follow the terminal by its container id, the pid can't
    // tell apart windows that share a process (e.g. alacritty msg create-window)
    if (window_id.has_value()) {
        const auto found = ranges::find_if(
            nodes, [this](const njson &json) -> bool { return json.value("id", int64_t{-1}) == *window_id; });
        if (found != nodes.end()) {
            return *found;
        }
        window_id.reset();
    }

    const auto pids = tmux::get_client_pids().value_or(std::vector<int>{Application::parent_pid});

    for (const auto pid : pids) {
        const auto tree = util::get_process_tree(pid);
        std::vector<njson> matches;
        for (const auto &node : nodes) {
            if (ranges::find(tree, node.value("pid", -1)) != tree.end()) {
                matches.push_back(node);
            }
        }
        if (matches.empty()) {
            continue;
        }
        // with several windows per process, the one being used is focused
        const auto focused =
            ranges::find_if(matches, [](const njson &json) -> bool { return json.value("focused", false); });
        const auto &window = focused != matches.end() ? *focused : matches.front();
        window_id = window.at("id").get<int64_t>();
        return window;
    }
    return nullptr;
}

auto SwaySocket::get_focused_output_name() -> std::string
{
    const std::scoped_lock lock{mutex};
    return output_info.name;
};

void SwaySocket::set_output_info(const nlohmann::json &output)
{
    const auto &rect = output.at("rect");
    output_info = {
        .x = rect.at("x"),
        .y = rect.at("y"),
        .scale = output.at("scale"),
        .name = output.at("name"),
    };
}

void SwaySocket::set_active_output_info()
{
    const auto outputs = ipc_message(IPC_GET_OUTPUTS);
    for (const auto &node : outputs) {
        bool focused = node.at("focused");
        if (focused) {
            set_output_info(node);
            break;
        }
    }
}

void SwaySocket::set_output_info_at(int xcoord, int ycoord)
{
    const auto outputs = ipc_message(IPC_GET_OUTPUTS);
    for (const auto &node : outputs) {
        if (!node.value("active", false)) {
            continue;
        }
        const auto &rect = node.at("rect");
        const int out_x = rect.at("x");
        const int out_y = rect.at("y");
        const int out_w = rect.at("width");
        const int out_h = rect.at("height");
        if (xcoord >= out_x && xcoord < out_x + out_w && ycoord >= out_y && ycoord < out_y + out_h) {
            set_output_info(node);
            return;
        }
    }
}

void SwaySocket::initial_setup(const std::string_view appid)
{
    disable_focus(appid);
    enable_floating(appid);
}

void SwaySocket::disable_focus(const std::string_view appid)
{
    const auto payload = fmt::format(R"(no_focus [app_id="{}"])", appid);
    ipc_command(payload);
}

void SwaySocket::enable_floating(const std::string_view appid)
{
    ipc_command(appid, "floating enable");
}

void SwaySocket::move_window(const std::string_view appid, int xcoord, int ycoord)
{
    // coordinates are logical and relative to the terminal's output, move
    // absolutely so the image follows it even if it spawned on another output
    const std::scoped_lock lock{mutex};
    const auto payload = fmt::format(R"([app_id="{}"] move absolute position {} {})", appid,
                                     output_info.x + xcoord, output_info.y + ycoord);
    ipc_command(payload);
}

auto SwaySocket::ipc_message(ipc_message_type type, const std::string_view payload) const -> nlohmann::json
{
    struct ipc_header header;
    header.len = payload.size();
    header.type = type;
    ipc_magic.copy(header.magic.data(), ipc_magic.size());

    if (!payload.empty()) {
        logger->debug("Running socket command {}", payload);
    }
    const std::scoped_lock lock{mutex};
    socket.write(&header, ipc_header_size);
    socket.write(payload.data(), payload.size());

    socket.read(&header, ipc_header_size);
    std::string buff(header.len, 0);
    socket.read(buff.data(), buff.size());
    return njson::parse(buff);
}

auto SwaySocket::get_nodes() const -> std::vector<nlohmann::json>
{
    logger->debug("Obtaining sway tree");
    const auto tree = ipc_message(IPC_GET_TREE);
    std::stack<njson> nodes_st;
    std::vector<njson> nodes_vec;

    nodes_st.push(tree);

    while (!nodes_st.empty()) {
        const auto top = nodes_st.top();
        nodes_st.pop();
        nodes_vec.push_back(top);
        for (const auto &node : top.at("nodes")) {
            nodes_st.push(node);
        }
        for (const auto &node : top.at("floating_nodes")) {
            nodes_st.push(node);
        }
    }
    return nodes_vec;
}

void SwaySocket::ipc_command(const std::string_view appid, const std::string_view command) const
{
    const auto payload = fmt::format(R"(for_window [app_id="{}"] {})", appid, command);
    ipc_command(payload);
}

void SwaySocket::ipc_command(const std::string_view payload) const
{
    std::ignore = ipc_message(IPC_COMMAND, payload);
}
