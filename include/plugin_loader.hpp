#ifndef BAMBU_BRIDGE_PLUGIN_LOADER_HPP
#define BAMBU_BRIDGE_PLUGIN_LOADER_HPP

// ============================================================================
// plugin_loader.hpp
//
// Cross-platform loader for the Bambu networking plugin.
//   - Linux:   dlopen()   "libbambu_networking.so"
//   - macOS:   dlopen()   "libbambu_networking.dylib" (untested by author)
//   - Windows: LoadLibrary("bambu_networking.dll")
//
// Also handles the companion "BambuSource" module that the networking plugin
// references (and its dependencies live beside it).
// ============================================================================

#include <string>
#include <vector>

namespace bambu {

struct LoadResult {
    bool        ok              = false;
    std::string library_path;   // resolved absolute path that was loaded
    std::string error;          // readable error message on failure
};

// A thin RAII wrapper around a loaded dynamic module handle.
// Handles dlopen/LoadLibrary and dlclose/FreeLibrary on destruction.
class PluginHandle {
public:
    PluginHandle() = default;
    ~PluginHandle();

    // Move-only
    PluginHandle(PluginHandle&&) noexcept;
    PluginHandle& operator=(PluginHandle&&) noexcept;
    PluginHandle(const PluginHandle&) = delete;
    PluginHandle& operator=(const PluginHandle&) = delete;

    bool load(const std::string& path, std::string* err = nullptr);
    void unload();
    void* raw() const { return handle_; }

    // Resolve a C symbol by name. Returns nullptr if not found.
    void* resolve(const std::string& name) const;

    bool is_loaded() const { return handle_ != nullptr; }
    const std::string& path() const { return path_; }

private:
    void*   handle_ = nullptr;   // HANDLE on Windows, void* (dlopen) elsewhere
    std::string path_;
};

// Resolve a dynamic library basename on the current platform:
//   "bambu_networking"  ->  "libbambu_networking.so" / "bambu_networking.dll"
std::string platform_library_name(const std::string& base, bool include_prefix = true);

// Try to load a library by name, searching these locations in order:
//   1. <dir>/<platform_name>
//   2. <dir>/<basename>.so / <basename>.dll (passed literally)
//   3. the raw path as given
// Returns the handle and the resolved path via `out`.
PluginHandle load_network_plugin(const std::string& dir, std::string* out_error);

// Load the BambuSource companion module (same directory as the networking plugin).
PluginHandle load_source_plugin(const std::string& dir, std::string* out_error);

// Make companion libraries in `dir` resolvable (Windows DLL search path / Linux
// preload of live555, BambuSource, agora, ...). Call before load_network_plugin.
void prepare_plugin_search_path(const std::string& dir);

// Load optional companion libraries so they stay mapped for the process lifetime.
std::vector<PluginHandle> load_companion_plugins(const std::string& dir);

} // namespace bambu

#endif // BAMBU_BRIDGE_PLUGIN_LOADER_HPP
