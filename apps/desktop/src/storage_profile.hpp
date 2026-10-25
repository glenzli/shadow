#pragma once

#include <QCoreApplication>
#include <QStringList>

#include <memory>
#include <optional>

class QLockFile;

enum class StorageProfileProcess { Desktop, LibraryServer };

struct StorageProfileLaunch final {
    QStringList arguments;
    QString daily_root;
    QString legacy_brush_file;
    QString override_root;
    bool override_root_set = false;
    bool debug_build = false;
    // A management preflight may enroll identity and probe the writer lock.
    // Only the actual launch preserves legacy settings using its launch inputs.
    bool enroll_only = false;
    StorageProfileProcess process = StorageProfileProcess::Desktop;
};

struct StorageProfileInfo final {
    QString root;
    QString kind;
    QString id;
    // Empty keeps the translated built-in name, or the custom directory name.
    QString display_name;
    [[nodiscard]] QString name() const;
};

// One immutable storage owner per process. Desktop and the independent Library
// Server each have one writer, with serialized enrollment of their shared profile.
class StorageProfile final {
    Q_DECLARE_TR_FUNCTIONS(StorageProfile)

  public:
    static std::unique_ptr<StorageProfile> open(const StorageProfileLaunch& launch, QString* error);
    static QString canonicalRoot(const QString& path);
    static bool rootsOverlap(const QString& left, const QString& right);
    static bool rootContains(const QString& root, const QString& path);
    static std::optional<StorageProfileInfo>
    inspect(const QString& root, const QString& daily_root, QString* error);
    static bool validDisplayName(const QString& name);
    ~StorageProfile();

    [[nodiscard]] QString root() const;
    [[nodiscard]] QString kind() const;
    [[nodiscard]] QString id() const;
    [[nodiscard]] StorageProfileInfo info() const;
    bool rename(const QString& display_name, QString* error);
    [[nodiscard]] bool isDaily() const;
    [[nodiscard]] QString catalogPath() const;
    [[nodiscard]] QString cacheRoot() const;
    [[nodiscard]] QString settingsFile() const;
    [[nodiscard]] QString brushSettingsFile() const;
    bool bindProcessEnvironment() const;

  private:
    StorageProfile(QString root, QString kind, StorageProfileProcess process);
    bool initialize(
        const QString& legacy_brush_file,
        const QString& expected_id,
        bool enroll_only,
        QString* error
    );
    bool migrateDailySettings(const QString& legacy_brush_file, QString* error);

    QString root_;
    QString kind_;
    QString id_;
    QString display_name_;
    StorageProfileProcess process_;
    std::unique_ptr<QLockFile> lock_;
};
