/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>

#ifndef WIN32
#include <glob.h>
#include <pwd.h>
#include <sys/time.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <osquery/core.h>
#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/sql.h>
#include <osquery/system.h>

#include "osquery/core/json.h"
#include "osquery/filesystem/fileops.h"

namespace pt = boost::property_tree;
namespace fs = boost::filesystem;
namespace errc = boost::system::errc;

namespace osquery {

FLAG(uint64, read_max, 50 * 1024 * 1024, "Maximum file read size");
FLAG(uint64, read_user_max, 10 * 1024 * 1024, "Maximum non-su read size");

/// See reference #1382 for reasons why someone would allow unsafe.
HIDDEN_FLAG(bool, allow_unsafe, false, "Allow unsafe executable permissions");

/// Disable forensics (atime/mtime preserving) file reads.
HIDDEN_FLAG(bool, disable_forensic, true, "Disable atime/mtime preservation");

static const size_t kMaxRecursiveGlobs = 64;

Status writeTextFile(const fs::path& path,
                     const std::string& content,
                     int permissions,
                     bool force_permissions) {
  // Open the file with the request permissions.
  PlatformFile output_fd(
      path.string(), PF_OPEN_ALWAYS | PF_WRITE | PF_APPEND, permissions);
  if (!output_fd.isValid()) {
    return Status(1, "Could not create file: " + path.string());
  }

  // If the file existed with different permissions before our open
  // they must be restricted.
  if (!platformChmod(path.string(), permissions)) {
    // Could not change the file to the requested permissions.
    return Status(1, "Failed to change permissions for file: " + path.string());
  }

  ssize_t bytes = output_fd.write(content.c_str(), content.size());
  if (static_cast<size_t>(bytes) != content.size()) {
    return Status(1, "Failed to write contents to file: " + path.string());
  }

  return Status(0, "OK");
}

struct OpenReadableFile {
 public:
  explicit OpenReadableFile(const fs::path& path, bool blocking = false) {
#ifndef WIN32
    dropper_ = DropPrivileges::get();
    if (dropper_->dropToParent(path)) {
#endif
      int mode = PF_OPEN_EXISTING | PF_READ;
      if (!blocking) {
        mode |= PF_NONBLOCK;
      }

      // Open the file descriptor and allow caller to perform error checking.
      fd.reset(new PlatformFile(path.string(), mode));
#ifndef WIN32
    }
#endif
  }

  ~OpenReadableFile() {}

  std::unique_ptr<PlatformFile> fd{nullptr};

#ifndef WIN32
 private:
  DropPrivilegesRef dropper_{nullptr};
#endif
};

Status readFile(const fs::path& path,
                size_t size,
                size_t block_size,
                bool dry_run,
                bool preserve_time,
                std::function<void(std::string& buffer, size_t size)> predicate,
                bool blocking) {
  OpenReadableFile handle(path, blocking);
  if (handle.fd == nullptr || !handle.fd->isValid()) {
    return Status(1, "Cannot open file for reading: " + path.string());
  }

  off_t file_size = static_cast<off_t>(handle.fd->size());
  if (handle.fd->isSpecialFile() && size > 0) {
    file_size = static_cast<off_t>(size);
  }

  // Apply the max byte-read based on file/link target ownership.
  off_t read_max =
      static_cast<off_t>((handle.fd->isOwnerRoot().ok())
                             ? FLAGS_read_max
                             : std::min(FLAGS_read_max, FLAGS_read_user_max));
  if (file_size > read_max) {
    VLOG(1) << "Cannot read " << path << " size exceeds limit: " << file_size
            << " > " << read_max;
    return Status(1, "File exceeds read limits");
  }

  if (dry_run) {
    // The caller is only interested in performing file read checks.
    boost::system::error_code ec;
    return Status(0, fs::canonical(path, ec).string());
  }

  PlatformTime times;
  handle.fd->getFileTimes(times);

  if (file_size == 0) {
    off_t total_bytes = 0;
    ssize_t part_bytes = 0;
    do {
      auto part = std::string(4096, '\0');
      part_bytes = handle.fd->read(&part[0], block_size);
      if (part_bytes > 0) {
        total_bytes += static_cast<off_t>(part_bytes);
        if (total_bytes >= read_max) {
          return Status(1, "File exceeds read limits");
        }
        //        content += part.substr(0, part_bytes);
        predicate(part, part_bytes);
      }
    } while (part_bytes > 0);
  } else {
    auto content = std::string(file_size, '\0');
    handle.fd->read(&content[0], file_size);
    predicate(content, file_size);
  }

  // Attempt to restore the atime and mtime before the file read.
  if (preserve_time && !FLAGS_disable_forensic) {
    handle.fd->setFileTimes(times);
  }
  return Status(0, "OK");
}

Status readFile(const fs::path& path,
                std::string& content,
                size_t size,
                bool dry_run,
                bool preserve_time,
                bool blocking) {
  return readFile(path,
                  size,
                  4096,
                  dry_run,
                  preserve_time,
                  ([&content](std::string& buffer, size_t size) {
                    if (buffer.size() == size) {
                      content += std::move(buffer);
                    } else {
                      content += buffer.substr(0, size);
                    }
                  }),
                  blocking);
}

Status readFile(const fs::path& path, bool blocking) {
  std::string blank;
  return readFile(path, blank, 0, true, false, blocking);
}

Status forensicReadFile(const fs::path& path,
                        std::string& content,
                        bool blocking) {
  return readFile(path, content, 0, false, true, blocking);
}

Status isWritable(const fs::path& path) {
  auto path_exists = pathExists(path);
  if (!path_exists.ok()) {
    return path_exists;
  }

  if (platformAccess(path.string(), W_OK) == 0) {
    return Status(0, "OK");
  }

  return Status(1, "Path is not writable: " + path.string());
}

Status isReadable(const fs::path& path) {
  auto path_exists = pathExists(path);
  if (!path_exists.ok()) {
    return path_exists;
  }

  if (platformAccess(path.string(), R_OK) == 0) {
    return Status(0, "OK");
  }

  return Status(1, "Path is not readable: " + path.string());
}

Status pathExists(const fs::path& path) {
  boost::system::error_code ec;
  if (path.empty()) {
    return Status(1, "-1");
  }

  // A tri-state determination of presence
  if (!fs::exists(path, ec) || ec.value() != errc::success) {
    return Status(1, ec.message());
  }
  return Status(0, "1");
}

Status remove(const fs::path& path) {
  auto status_code = std::remove(path.string().c_str());
  return Status(status_code, "N/A");
}

static void genGlobs(std::string path,
                     std::vector<std::string>& results,
                     GlobLimits limits) {
  // Use our helped escape/replace for wildcards.
  replaceGlobWildcards(path, limits);

  // Generate a glob set and recurse for double star.
  size_t glob_index = 0;
  while (++glob_index < kMaxRecursiveGlobs) {
    auto glob_results = platformGlob(path);

    for (auto const& result_path : glob_results) {
      results.push_back(result_path);
    }

    // The end state is a non-recursive ending or empty set of matches.
    size_t wild = path.rfind("**");
    // Allow a trailing slash after the double wild indicator.
    if (glob_results.size() == 0 || wild > path.size() ||
        wild < path.size() - 3) {
      break;
    }
    path += "/**";
  }

  // Prune results based on settings/requested glob limitations.
  auto end = std::remove_if(
      results.begin(), results.end(), [limits](const std::string& found) {
        return !(((found[found.length() - 1] == '/' ||
                   found[found.length() - 1] == '\\') &&
                  limits & GLOB_FOLDERS) ||
                 ((found[found.length() - 1] != '/' &&
                   found[found.length() - 1] != '\\') &&
                  limits & GLOB_FILES));
      });
  results.erase(end, results.end());
}

Status resolveFilePattern(const fs::path& fs_path,
                          std::vector<std::string>& results) {
  return resolveFilePattern(fs_path, results, GLOB_ALL);
}

Status resolveFilePattern(const fs::path& fs_path,
                          std::vector<std::string>& results,
                          GlobLimits setting) {
  genGlobs(fs_path.string(), results, setting);
  return Status(0, "OK");
}

fs::path getSystemRoot() {
#ifdef WIN32
  char winDirectory[MAX_PATH] = {0};
  GetWindowsDirectory(winDirectory, MAX_PATH);
  return fs::path(winDirectory);
#else
  return fs::path("/");
#endif
}

inline void replaceGlobWildcards(std::string& pattern, GlobLimits limits) {
  // Replace SQL-wildcard '%' with globbing wildcard '*'.
  if (pattern.find("%") != std::string::npos) {
    boost::replace_all(pattern, "%", "*");
  }

  // Relative paths are a bad idea, but we try to accommodate.
  if ((pattern.size() == 0 || ((pattern[0] != '/' && pattern[0] != '\\') &&
                               (pattern.size() > 3 && pattern[1] != ':' &&
                                pattern[2] != '\\' && pattern[2] != '/'))) &&
      pattern[0] != '~') {
    boost::system::error_code ec;
    pattern = (fs::current_path(ec) / pattern).make_preferred().string();
  }

  auto base =
      fs::path(pattern.substr(0, pattern.find('*'))).make_preferred().string();

  if (base.size() > 0) {
    boost::system::error_code ec;
    auto canonicalized = ((limits & GLOB_NO_CANON) == 0)
                             ? fs::canonical(base, ec).make_preferred().string()
                             : base;

    if (canonicalized.size() > 0 && canonicalized != base) {
      if (isDirectory(canonicalized)) {
        // Canonicalized directory paths will not include a trailing '/'.
        // However, if the wildcards are applied to files within a directory
        // then the missing '/' changes the wildcard meaning.
        canonicalized += '/';
      }
      // We are unable to canonicalize the meaning of post-wildcard limiters.
      pattern = fs::path(canonicalized + pattern.substr(base.size()))
                    .make_preferred()
                    .string();
    }
  }
}

inline Status listInAbsoluteDirectory(const fs::path& path,
                                      std::vector<std::string>& results,
                                      GlobLimits limits) {
  if (path.filename() == "*" && !pathExists(path.parent_path())) {
    return Status(1, "Directory not found: " + path.parent_path().string());
  }

  if (path.filename() == "*" && !isDirectory(path.parent_path())) {
    return Status(1, "Path not a directory: " + path.parent_path().string());
  }

  genGlobs(path.string(), results, limits);
  return Status(0, "OK");
}

Status listFilesInDirectory(const fs::path& path,
                            std::vector<std::string>& results,
                            bool recursive) {
  return listInAbsoluteDirectory(
      (path / ((recursive) ? "**" : "*")), results, GLOB_FILES);
}

Status listDirectoriesInDirectory(const fs::path& path,
                                  std::vector<std::string>& results,
                                  bool recursive) {
  return listInAbsoluteDirectory(
      (path / ((recursive) ? "**" : "*")), results, GLOB_FOLDERS);
}

Status isDirectory(const fs::path& path) {
  boost::system::error_code ec;
  if (fs::is_directory(path, ec)) {
    return Status(0, "OK");
  }

  // The success error code is returned for as a failure (undefined error)
  // We need to flip that into an error, a success would have falling through
  // in the above conditional.
  if (ec.value() == errc::success) {
    return Status(1, "Path is not a directory: " + path.string());
  }
  return Status(ec.value(), ec.message());
}

std::set<fs::path> getHomeDirectories() {
  std::set<fs::path> results;

  auto users = SQL::selectAllFrom("users");
  for (const auto& user : users) {
    if (user.at("directory").size() > 0) {
      results.insert(user.at("directory"));
    }
  }

  return results;
}

bool safePermissions(const std::string& dir,
                     const std::string& path,
                     bool executable) {
  if (!platformIsFileAccessible(path).ok()) {
    // Path was not real, had too may links, or could not be accessed.
    return false;
  }

  if (FLAGS_allow_unsafe) {
    return true;
  }

  Status result = platformIsTmpDir(dir);
  if (!result.ok() && result.getCode() < 0) {
    // An error has occurred in stat() on dir, most likely because the file path
    // does not exist
    return false;
  } else if (result.ok()) {
    // Do not load modules from /tmp-like directories.
    return false;
  }

  PlatformFile fd(path, PF_OPEN_EXISTING | PF_READ);
  if (!fd.isValid()) {
    return false;
  }

  result = isDirectory(path);
  if (!result.ok() && result.getCode() < 0) {
    // Something went wrong when determining the file's directoriness
    return false;
  } else if (result.ok()) {
    // Only load file-like nodes (not directories).
    return false;
  }

  if (fd.isOwnerCurrentUser().ok() || fd.isOwnerRoot().ok()) {
    result = fd.isExecutable();

    // Otherwise, require matching or root file ownership.
    if (executable && (result.getCode() > 0 || !fd.isNonWritable().ok())) {
      // Require executable, implies by the owner.
      return false;
    }

    return true;
  }

  // Do not load modules not owned by the user.
  return false;
}

const std::string& osqueryHomeDirectory() {
  static std::string homedir;

  if (homedir.size() == 0) {
    // Try to get the caller's home directory
    boost::system::error_code ec;
    auto userdir = getHomeDirectory();
    if (userdir.is_initialized() && isWritable(*userdir).ok()) {
      auto osquery_dir = (fs::path(*userdir) / ".osquery");
      if (isWritable(osquery_dir) ||
          boost::filesystem::create_directories(osquery_dir, ec)) {
        homedir = osquery_dir.make_preferred().string();
        return homedir;
      }
    }

    // Fail over to a temporary directory (used for the shell).
    auto temp =
        fs::temp_directory_path(ec) / fs::unique_path("osquery%%%%%%%%", ec);
    homedir = temp.make_preferred().string();
  }

  return homedir;
}

std::string lsperms(int mode) {
  static const char rwx[] = {'0', '1', '2', '3', '4', '5', '6', '7'};
  std::string bits;

  bits += rwx[(mode >> 9) & 7];
  bits += rwx[(mode >> 6) & 7];
  bits += rwx[(mode >> 3) & 7];
  bits += rwx[(mode >> 0) & 7];
  return bits;
}

Status parseJSON(const fs::path& path, pt::ptree& tree) {
  std::string json_data;
  if (!readFile(path, json_data).ok()) {
    return Status(1, "Could not read JSON from file");
  }

  return parseJSONContent(json_data, tree);
}

Status parseJSONContent(const std::string& content, pt::ptree& tree) {
  // Read the extensions data into a JSON blob, then property tree.
  try {
    std::stringstream json_stream;
    json_stream << content;
    pt::read_json(json_stream, tree);
  } catch (const pt::json_parser::json_parser_error& /* e */) {
    return Status(1, "Could not parse JSON from file");
  }
  return Status(0, "OK");
}
}
