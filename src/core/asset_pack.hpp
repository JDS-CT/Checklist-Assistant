#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace core {

struct AssetPackChecklist {
  std::string pack;
  std::string checklist;
  std::filesystem::path root;
};

std::filesystem::path ResolveLibraryRoot();
bool IsSafePackToken(const std::string& value);
bool IsSafeChecklistToken(const std::string& value);
std::vector<std::filesystem::path> ListPackRoots(const std::filesystem::path& library_root);
std::vector<AssetPackChecklist> ListChecklistMarkdown(const std::filesystem::path& library_root);
std::vector<AssetPackChecklist> FindChecklistRoots(const std::filesystem::path& library_root,
                                                    const std::string& checklist);

}  // namespace core
