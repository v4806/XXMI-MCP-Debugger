#pragma once

#include <cstdint>

class MigotoAdapter;

void MCP_Start(MigotoAdapter* adapter);
bool MCP_Stop(std::uint32_t timeout_ms = 2000);
std::uint32_t MCP_RenderWaitMsForCommand(std::uint16_t command);
