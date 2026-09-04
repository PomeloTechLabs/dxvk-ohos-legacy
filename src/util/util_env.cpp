#include <array>
#include <cstdlib>
#include <filesystem>
#include <numeric>

#if defined(__linux__) || defined(DXVK_NATIVE_OHOS)
#include <unistd.h>
#include <limits.h>
#endif

#include "util_env.h"

#include "./com/com_include.h"

namespace dxvk::env {

  std::string getEnvVar(const char* name) {
#ifdef _WIN32
    std::vector<WCHAR> result;
    result.resize(MAX_PATH + 1);

    DWORD len = ::GetEnvironmentVariableW(str::tows(name).c_str(), result.data(), MAX_PATH);
    result.resize(len);

    return str::fromws(result.data());
#else
    const char* result = std::getenv(name);
    return result ? result : "";
#endif
  }


  size_t matchFileExtension(const std::string& name, const char* ext) {
    auto pos = name.find_last_of('.');

    if (pos == std::string::npos)
      return pos;

    bool matches = std::accumulate(name.begin() + pos + 1, name.end(), true,
      [&ext] (bool current, char a) {
        if (a >= 'A' && a <= 'Z')
          a += 'a' - 'A';
        return current && *ext && a == *(ext++);
      });

    return matches ? pos : std::string::npos;
  }


  std::string getExeName() {
    std::string fullPath = getExePath();
    auto n = fullPath.find_last_of(env::PlatformDirSlash);
    
    return (n != std::string::npos)
      ? fullPath.substr(n + 1)
      : fullPath;
  }


  std::string getExeBaseName() {
    auto exeName = getExeName();
#ifdef _WIN32
    auto extp = matchFileExtension(exeName, "exe");

    if (extp != std::string::npos)
      exeName.erase(extp);
#endif

    return exeName;
  }


  std::string getExePath() {
#if defined(_WIN32)
    std::vector<WCHAR> exePath;
    exePath.resize(MAX_PATH + 1);

    DWORD len = ::GetModuleFileNameW(NULL, exePath.data(), MAX_PATH);
    exePath.resize(len);

    return str::fromws(exePath.data());
#elif defined(__linux__) || defined(DXVK_NATIVE_OHOS)
    std::array<char, PATH_MAX> exePath = {};

    const ssize_t count = readlink("/proc/self/exe", exePath.data(), exePath.size());

    return count > 0
      ? std::string(exePath.data(), static_cast<size_t>(count))
      : std::string();
#else
    return std::string();
#endif
  }
  
  
  void setThreadName(const std::string& name) {
#ifdef _WIN32
    using SetThreadDescriptionProc = HRESULT (WINAPI *) (HANDLE, PCWSTR);

    static auto proc = reinterpret_cast<SetThreadDescriptionProc>(
      ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));

    if (proc != nullptr) {
      auto wideName = std::vector<WCHAR>(name.length() + 1);
      str::tows(name.c_str(), wideName.data(), wideName.size());
      (*proc)(::GetCurrentThread(), wideName.data());
    }
#else
    std::array<char, 16> posixName = {};
    dxvk::str::strlcpy(posixName.data(), name.c_str(), 16);
    ::pthread_setname_np(pthread_self(), posixName.data());
#endif
  }


  bool createDirectory(const std::string& path) {
#ifdef _WIN32
    WCHAR widePath[MAX_PATH];
    str::tows(path.c_str(), widePath);
    return !!CreateDirectoryW(widePath, nullptr);
#else
    // DXVK treats the state-cache path as optional. On OHOS, unlike Win32
    // CreateDirectoryW, std::filesystem throws for an empty path, so avoid
    // making device creation depend on an optional cache directory.
    if (path.empty())
      return false;

    std::error_code error;
    if (std::filesystem::create_directories(path, error))
      return true;

    return !error && std::filesystem::is_directory(path, error);
#endif
  }
  
}
