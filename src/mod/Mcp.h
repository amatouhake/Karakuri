#pragma once

// MCP server (Streamable HTTP transport, JSON responses) inside the BDS process
// (docs/DESIGN.md 6, "External interface"). It listens on localhost only. Requests arrive on the
// HTTP thread; anything that touches the game runs on the server thread through a queue, and
// waiting (wait_for_event, wait_seconds) happens on the HTTP thread.

#include <filesystem>

namespace karakuri::mcp {

struct Config {
    bool        enabled{true};
    std::string host{"127.0.0.1"};
    int         port{47821};
};

Config loadConfig(std::filesystem::path const& dir);
bool   start(Config const& cfg);
void   stop();

} // namespace karakuri::mcp
