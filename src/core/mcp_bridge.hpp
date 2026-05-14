#pragma once

#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "platform/http_client.hpp"

namespace core::mcp {

struct ToolDefinition {
  std::string name;
  std::string method;
  std::string path;
  std::string description;
  nlohmann::json input_schema;
};

class Bridge {
 public:
  explicit Bridge(std::string base_url);

  void SetBearerToken(std::string token);
  nlohmann::json ToolSchemasJson() const;
  const std::vector<ToolDefinition>& Definitions() const;

  platform::HttpClientResponse CallTool(const std::string& name,
                                        const nlohmann::json& arguments) const;

 private:
  platform::HttpClientResponse Get(const std::string& path,
                                   const std::map<std::string, std::string>& query = {}) const;
  platform::HttpClientResponse Post(const std::string& path, const std::string& body,
                                    const std::map<std::string, std::string>& query = {},
                                    const std::string& content_type = "application/json") const;
  platform::HttpClientResponse Patch(const std::string& path, const std::string& body,
                                     const std::map<std::string, std::string>& query = {},
                                     const std::string& content_type = "application/json") const;

  platform::HttpClient client_;
  std::map<std::string, std::string> default_headers_;
};

std::string FormatResponseForDisplay(const platform::HttpClientResponse& response);

}  // namespace core::mcp
