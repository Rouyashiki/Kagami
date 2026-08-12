#pragma once

#include <string>

#include "kagami/config.hpp"

namespace kagami::mount::storage {

// OverlayFS and Kasumi share a clean module mirror: neither backend may serve
// redirected files directly from /data because of SELinux contexts. We provide
// the mirror as tmpfs (when it preserves overlay xattrs), an ext4 loop image,
// or a read-only erofs image paired with a tmpfs writable layer.
enum class Mode { Tmpfs, Ext4, Erofs };

const char* mode_name(Mode mode);

struct Handle {
    bool ok = false;
    Mode mode = Mode::Ext4;
    std::string content_dir; // per-module lower trees live at content_dir/<id>/<part>
    std::string rw_dir;      // writable fs for per-partition upperdir/workdir
};

// Mount or reuse the shared mirror per config.fs_type ("auto" => tmpfs if
// overlay xattrs work, else ext4). Must run inside the init mount namespace;
// marks new mounts private and registers them with KernelSU. Returns
// Handle{ok=false} on failure.
Handle setup(const Config& config);

// Unmount an acquired base (content + writable). The ext4/erofs image is left
// on disk. Backend code should normally use teardown_shared() after both
// OverlayFS and Kasumi are inactive.
void teardown(const Handle& handle);

// Detach the one shared mirror after every backend has released it. Magic Mount
// never calls setup(), so an all-Magic configuration does not create this mount.
void teardown_shared(const Config& config);

} // namespace kagami::mount::storage
