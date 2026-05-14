#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "core/mcp_bridge.hpp"
#include "nlohmann/json.hpp"

namespace {

std::string GetEnv(const char* key, const std::string& fallback) {
  if (const char* value = std::getenv(key)) {
    return value;
  }
  return fallback;
}

std::optional<nlohmann::json> ReadMessage() {
  std::string line;
  std::size_t content_length = 0;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](unsigned char ch) { return std::isspace(ch) != 0; }),
              key.end());
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(),
                             [](unsigned char ch) { return std::isspace(ch) == 0; }));
    if (!value.empty() && value.back() == '\r') {
      value.pop_back();
    }

    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (key == "content-length") {
      content_length = static_cast<std::size_t>(std::stoul(value));
    }
  }

  if (content_length == 0) {
    return std::nullopt;
  }

  std::string body(content_length, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(content_length));
  if (std::cin.gcount() != static_cast<std::streamsize>(content_length)) {
    return std::nullopt;
  }

  auto parsed = nlohmann::json::parse(body, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  return parsed;
}

void WriteMessage(const nlohmann::json& payload) {
  const std::string serialized = payload.dump();
  std::cout << "Content-Length: " << serialized.size() << "\r\n\r\n" << serialized;
  std::cout.flush();
}

nlohmann::json MakeResultPayload(const nlohmann::json& id, const nlohmann::json& result) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

nlohmann::json MakeErrorPayload(const nlohmann::json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

int main() {
  try {
    std::string base_url =
        GetEnv("CHAX_MCP_BASE_URL", "http://" + GetEnv("CHAX_HOST", "127.0.0.1") + ":" +
                                         GetEnv("CHAX_PORT", "8080"));

    core::mcp::Bridge bridge(base_url);
    bool initialized = false;

    while (true) {
      auto message = ReadMessage();
      if (!message) {
        break;
      }

      const auto method = message->value("method", std::string{});
      const auto id_it = message->find("id");
      const nlohmann::json id = (id_it != message->end()) ? *id_it : nlohmann::json();

      if (method == "initialize") {
        initialized = true;
        nlohmann::json result = {
            {"serverInfo", {{"name", "chax-mcp-bridge"}, {"version", "0.2.0"}}},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
        };
        WriteMessage(MakeResultPayload(id, result));
        continue;
      }

      if (method == "shutdown") {
        initialized = false;
        WriteMessage(MakeResultPayload(id, nlohmann::json::object()));
        continue;
      }

      if (method == "tools/list") {
        WriteMessage(MakeResultPayload(id, {{"tools", bridge.ToolSchemasJson()}}));
        continue;
      }

      if (method == "tools/call") {
        try {
          const auto& params = message->at("params");
          const std::string tool_name = params.at("name").get<std::string>();
          const auto arguments =
              params.contains("arguments") ? params.at("arguments") : nlohmann::json::object();
          const auto response = bridge.CallTool(tool_name, arguments);
          nlohmann::json content = {
              {"content", nlohmann::json::array({{{"type", "text"},
                                                  {"text", core::mcp::FormatResponseForDisplay(response)}}})}};
          WriteMessage(MakeResultPayload(id, content));
        } catch (const std::exception& ex) {
          WriteMessage(MakeErrorPayload(id, -32000, ex.what()));
        }
        continue;
      }

      if (method == "ping") {
        WriteMessage(MakeResultPayload(id, nlohmann::json::object()));
        continue;
      }

      if (method == "exit") {
        break;
      }

      if (!method.empty()) {
        WriteMessage(MakeErrorPayload(id, -32601, "Unsupported method: " + method));
      }
    }

    if (initialized) {
      WriteMessage(MakeErrorPayload(nullptr, -32000, "MCP host terminated without shutdown."));
    }
  } catch (const std::exception& ex) {
    std::cerr << "Fatal MCP bridge error: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
