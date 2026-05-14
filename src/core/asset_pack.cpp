#include "core/asset_pack.hpp"

#include <cstdlib>
#include <system_error>

namespace core {
namespace {

bool IsSafeToken(const std::string& value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  if (value.find_first_of("\\/") != std::string::npos) {
    return false;
  }
  if (value.find("..") != std::string::npos) {
    return false;
  }
  if (value.find(':') != std::string::npos) {
    return false;
  }
  return true;
}

}  // namespace

std::filesystem::path ResolveLibraryRoot() {
  std::filesystem::path root = "checklists";
  if (const char* env = std::getenv("CHAX_CHECKLISTS_ROOT")) {
    root = std::filesystem::path(env);
  }
  if (root.is_absolute()) {
    return root;
  }

  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  std::filesystem::path best = cwd / root;

  for (int depth = 0; depth <= 2; ++depth) {
    std::filesystem::path candidate = cwd;
    for (int i = 0; i < depth; ++i) {
      candidate = candidate.parent_path();
    }
    if (candidate.empty()) {
      break;
    }
    const std::filesystem::path joined = candidate / root;
    if (std::filesystem::exists(joined, ec)) {
      return joined;
    }
  }
  return best;
}

bool IsSafePackToken(const std::string& value) {
  return IsSafeToken(value);
}

bool IsSafeChecklistToken(const std::string& value) {
  return IsSafeToken(value);
}

std::vector<std::filesystem::path> ListPackRoots(const std::filesystem::path& library_root) {
  std::vector<std::filesystem::path> packs;
  std::error_code ec;
  if (!std::filesystem::exists(library_root, ec) ||
      !std::filesystem::is_directory(library_root, ec)) {
    return packs;
  }
  for (const auto& entry : std::filesystem::directory_iterator(library_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    packs.push_back(entry.path());
  }
  return packs;
}

std::vector<AssetPackChecklist> ListChecklistMarkdown(const std::filesystem::path& library_root) {
  std::vector<AssetPackChecklist> items;
  std::error_code ec;
  for (const auto& pack_root : ListPackRoots(library_root)) {
    const std::string pack = pack_root.filename().string();
    for (const auto& entry : std::filesystem::directory_iterator(pack_root, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_directory()) {
        continue;
      }
      const std::string checklist = entry.path().filename().string();
      const std::filesystem::path md_path = entry.path() / "checklist.md";
      if (!std::filesystem::exists(md_path, ec)) {
        continue;
      }
      items.push_back({pack, checklist, entry.path()});
    }
  }
  return items;
}

std::vector<AssetPackChecklist> FindChecklistRoots(const std::filesystem::path& library_root,
                                                    const std::string& checklist) {
  std::vector<AssetPackChecklist> matches;
  if (!IsSafeChecklistToken(checklist)) {
    return matches;
  }
  std::error_code ec;
  for (const auto& pack_root : ListPackRoots(library_root)) {
    const std::filesystem::path checklist_root = pack_root / checklist;
    if (std::filesystem::exists(checklist_root, ec) &&
        std::filesystem::is_directory(checklist_root, ec)) {
      matches.push_back(
          {pack_root.filename().string(), checklist, std::filesystem::path(checklist_root)});
    }
  }
  return matches;
}

}  // namespace core
