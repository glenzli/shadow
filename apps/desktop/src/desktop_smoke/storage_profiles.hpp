#pragma once

#include <memory>

class QApplication;
class QObject;
class QQmlApplicationEngine;
class DesktopBackend;
class EditController;
class StorageProfileController;

namespace DesktopSmoke {
// Opt-in real edit/save/restart acceptance. One explicitly named local source;
// the lifetime owner waits for its worker before releasing the profile lock.
std::unique_ptr<QObject> installStorageProfileSwitch(
    QApplication& application,
    QQmlApplicationEngine& engine,
    const std::shared_ptr<DesktopBackend>& backend,
    EditController& editor,
    StorageProfileController& profiles
);
} // namespace DesktopSmoke
