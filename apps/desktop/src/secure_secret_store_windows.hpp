#pragma once

#include <QString>

// Exact regular-file ownership for the Windows local credential store.
[[nodiscard]] bool restrictWindowsCredentialFile(const QString& path);
