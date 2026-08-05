#pragma once

#include <QString>

/// Last synchronous guard before an original path enters the edit pipeline.
/// It deliberately performs no decode; a missing path must be reported as a
/// Library recovery condition instead of surfacing later as a decoder error.
[[nodiscard]] bool editSourceIsAvailable(const QString& source_path);
