#include "storage_profile_registry.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
constexpr auto registry_name = "library-profiles.json";
constexpr qsizetype maximum_recent = 12;
constexpr qint64 maximum_registry_bytes = 1024 * 1024;

QJsonObject encode(const StorageProfileInfo& info) {
    return {
        {"root", info.root},
        {"kind", info.kind},
        {"id", info.id},
        {"displayName", info.display_name}
    };
}
StorageProfileInfo decode(const QJsonObject& value) {
    return {
        value.value("root").toString(),
        value.value("kind").toString(),
        value.value("id").toString(),
        value.value("displayName").toString()
    };
}
bool explicitSelection(const StorageProfileLaunch& launch) {
    return launch.override_root_set
           || std::any_of(
               launch.arguments.cbegin(),
               launch.arguments.cend(),
               [](const QString& arg) {
                   return arg == "--profile" || arg.startsWith("--profile=")
                          || arg == "--profile-root" || arg.startsWith("--profile-root=");
               }
           );
}
} // namespace

StorageProfileRegistry::StorageProfileRegistry(QString daily_root, QString registry_root) :
    daily_root_(StorageProfile::canonicalRoot(daily_root)),
    registry_root_(StorageProfile::canonicalRoot(registry_root)) {}

QString StorageProfileRegistry::dailyRoot() const {
    return daily_root_;
}
QString StorageProfileRegistry::registryRoot() const {
    return registry_root_;
}

QString
StorageProfileRegistry::launchContext(const StorageProfileLaunch& launch, QString* const error) {
    QString context;
    for (qsizetype i = 1; i < launch.arguments.size(); ++i) {
        const auto& arg = launch.arguments[i];
        if (arg != "--profile-launch-context" && !arg.startsWith("--profile-launch-context="))
            continue;
        QString value;
        if (arg.contains('='))
            value = arg.sliced(arg.indexOf('=') + 1);
        else if (i + 1 < launch.arguments.size())
            value = launch.arguments[++i];
        if (!context.isEmpty() || (value != "normal" && value != "debug")) {
            *error = tr("The library launch context is invalid.");
            return {};
        }
        context = value;
    }
    return context.isEmpty() ? (launch.debug_build ? "debug" : "normal") : context;
}

bool StorageProfileRegistry::read(QJsonObject* const document, QString* const error) const {
    if (daily_root_.isEmpty() || registry_root_.isEmpty()
        || StorageProfile::rootsOverlap(daily_root_, registry_root_)
        || StorageProfile::rootsOverlap(daily_root_ + "-debug", registry_root_)) {
        *error = tr("The library registry must be outside the library directories.");
        return false;
    }
    const QString path = QDir(registry_root_).filePath(registry_name);
    if (StorageProfile::canonicalRoot(path) != path) {
        *error = tr("The library registry location is unavailable.");
        return false;
    }
    if (!QFileInfo::exists(path)) {
        *document = {{"schemaVersion", 1}, {"recent", QJsonArray{}}, {"launch", QJsonObject{}}};
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximum_registry_bytes) {
        *error = tr("Could not read recent libraries.");
        return false;
    }
    *document = QJsonDocument::fromJson(file.readAll()).object();
    if (file.error() != QFileDevice::NoError || document->value("schemaVersion").toInt() != 1
        || !document->value("recent").isArray() || !document->value("launch").isObject()
        || document->value("recent").toArray().size() > maximum_recent) {
        *error = tr("The library registry is incompatible or damaged.");
        return false;
    }
    const auto valid_entry = [this](const QJsonValue& entry) {
        if (!entry.isObject())
            return false;
        const auto info = decode(entry.toObject());
        const QString kind = info.root == daily_root_              ? "daily"
                             : info.root == daily_root_ + "-debug" ? "debug"
                                                                   : "custom";
        return (!entry.toObject().contains("displayName")
                || entry.toObject().value("displayName").isString())
               && !info.root.isEmpty() && StorageProfile::canonicalRoot(info.root) == info.root
               && !QUuid(info.id).isNull() && info.kind == kind
               && (info.kind == "daily" || !StorageProfile::rootsOverlap(daily_root_, info.root))
               && (info.kind == "debug"
                   || !StorageProfile::rootsOverlap(daily_root_ + "-debug", info.root))
               && !StorageProfile::rootsOverlap(registry_root_, info.root)
               && (info.display_name.isEmpty()
                   || StorageProfile::validDisplayName(info.display_name));
    };
    for (const auto& entry : document->value("recent").toArray()) {
        if (!valid_entry(entry)) {
            *error = tr("The library registry is incompatible or damaged.");
            return false;
        }
    }
    const auto lanes = document->value("launch").toObject();
    for (auto it = lanes.begin(); it != lanes.end(); ++it) {
        if ((it.key() != "normal" && it.key() != "debug") || !valid_entry(it.value())
            || (it.key() == "debug" && decode(it.value().toObject()).kind == "daily")) {
            *error = tr("The library registry is incompatible or damaged.");
            return false;
        }
    }
    return true;
}

std::optional<StorageProfileLaunch> StorageProfileRegistry::resolveLaunch(
    const StorageProfileLaunch& input,
    const QString& context,
    QString* const error
) const {
    auto launch = input;
    if (explicitSelection(input))
        return launch;
    QJsonObject document;
    if (!read(&document, error))
        return std::nullopt;
    const auto remembered = document.value("launch").toObject().value(context);
    if (remembered.isUndefined()) {
        launch.arguments.append({"--profile", context == "debug" ? "debug" : "daily"});
        return launch;
    }
    const auto stored = decode(remembered.toObject());
    auto actual = StorageProfile::inspect(stored.root, daily_root_, error);
    if (!actual || actual->id != stored.id) {
        *error =
            tr("The last library is unavailable or its identity has changed. Choose a library to "
               "continue.");
        return std::nullopt;
    }
    launch.arguments.append(argumentsFor(*actual));
    return launch;
}

QVector<StorageProfileInfo> StorageProfileRegistry::recent(QString* const error) const {
    QJsonObject document;
    QVector<StorageProfileInfo> profiles;
    if (!read(&document, error))
        return profiles;
    for (const auto& value : document.value("recent").toArray())
        profiles.push_back(decode(value.toObject()));
    for (const auto* kind : {"daily", "debug"}) {
        const QString root =
            QString::fromLatin1(kind) == "daily" ? daily_root_ : daily_root_ + "-debug";
        if (std::none_of(profiles.cbegin(), profiles.cend(), [&root](const auto& p) {
                return p.root == root;
            }))
            profiles.push_back({root, QString::fromLatin1(kind), {}, {}});
    }
    return profiles;
}

QStringList StorageProfileRegistry::argumentsFor(const StorageProfileInfo& info) const {
    QStringList args = info.root == daily_root_ ? QStringList{"--profile", "daily"}
                       : info.root == daily_root_ + "-debug"
                           ? QStringList{"--profile", "debug"}
                           : QStringList{"--profile-root", info.root};
    if (!info.id.isEmpty())
        args.append({"--profile-id", info.id});
    return args;
}

bool StorageProfileRegistry::locationAllowed(
    const QString& root,
    const QVector<StorageProfileInfo>& known,
    QString* const error
) const {
    if (root.isEmpty() || StorageProfile::rootsOverlap(registry_root_, root)) {
        *error = tr("Choose a library directory outside the library registry.");
        return false;
    }
    for (const auto& profile : known) {
        if (profile.root != root && StorageProfile::rootsOverlap(profile.root, root)) {
            *error = tr("Library directories must not contain one another.");
            return false;
        }
    }
    // Also protect a known profile that has fallen out of the recent list.
    QString ancestor = QFileInfo(root).absolutePath();
    while (ancestor != QFileInfo(ancestor).absolutePath()) {
        if (QFileInfo::exists(QDir(ancestor).filePath("storage-profile.json"))) {
            *error = tr("Library directories must not contain one another.");
            return false;
        }
        ancestor = QFileInfo(ancestor).absolutePath();
    }
    return true;
}

std::optional<StorageProfileInfo> StorageProfileRegistry::prepare(
    const QString& requested_root,
    const QString& expected_id,
    QString* const error
) const {
    const QString root = StorageProfile::canonicalRoot(requested_root);
    auto known = recent(error);
    if (!error->isEmpty() || !locationAllowed(root, known, error))
        return std::nullopt;
    const bool builtin = root == daily_root_ || root == daily_root_ + "-debug";
    if (!builtin || QFileInfo::exists(QDir(root).filePath("storage-profile.json"))) {
        const auto inspected = StorageProfile::inspect(root, daily_root_, error);
        if (!inspected)
            return std::nullopt;
        if (!expected_id.isEmpty() && inspected->id != expected_id) {
            *error = tr(
                "The library at this location has changed. Open it again to confirm its identity."
            );
            return std::nullopt;
        }
    } else if (!expected_id.isEmpty()) {
        *error =
            tr("The library at this location has changed. Open it again to confirm its identity.");
        return std::nullopt;
    }
    StorageProfileInfo selection{
        root,
        root == daily_root_ ? "daily"
        : builtin           ? "debug"
                            : "custom",
        expected_id,
        {}
    };
    StorageProfileLaunch launch;
    launch.daily_root = daily_root_;
    launch.enroll_only = true;
    launch.arguments = {"Shadow"};
    launch.arguments.append(argumentsFor(selection));
    auto opened = StorageProfile::open(launch, error);
    return opened ? std::optional{opened->info()} : std::nullopt;
}

std::optional<StorageProfileInfo> StorageProfileRegistry::create(
    const QString& name,
    const QString& parent,
    QString* const error
) const {
    if (!StorageProfile::validDisplayName(name.trimmed())) {
        *error = tr("Use a library name of 1–80 characters without control characters.");
        return std::nullopt;
    }
    const QString canonical_parent = StorageProfile::canonicalRoot(parent);
    if (canonical_parent.isEmpty() || !QFileInfo(canonical_parent).isDir()) {
        *error = tr("Choose an existing folder for the new library.");
        return std::nullopt;
    }
    const QString directory =
        "Shadow-Library-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString root = QDir(canonical_parent).filePath(directory);
    const auto known = recent(error);
    if (!error->isEmpty() || !locationAllowed(root, known, error))
        return std::nullopt;
    StorageProfileLaunch launch;
    launch.daily_root = daily_root_;
    launch.arguments = {"Shadow", "--profile-root", root};
    auto profile = StorageProfile::open(launch, error);
    if (!profile || !profile->rename(name, error))
        return std::nullopt;
    return profile->info();
}

bool StorageProfileRegistry::remember(
    const StorageProfileInfo& info,
    const QString& context,
    QString* const error
) const {
    if (!context.isEmpty() && context != "normal" && context != "debug") {
        *error = tr("The library launch context is invalid.");
        return false;
    }
    QJsonObject document;
    if (!read(&document, error))
        return false;
    if (!QDir().mkpath(registry_root_)) {
        *error = tr("Could not save recent libraries.");
        return false;
    }
    const QString lock_path = QDir(registry_root_).filePath("registry.lock");
    if (StorageProfile::canonicalRoot(lock_path) != lock_path) {
        *error = tr("The library registry location is unavailable.");
        return false;
    }
    QLockFile lock(lock_path);
    lock.setStaleLockTime(0);
    if (!lock.tryLock() || !read(&document, error)) {
        if (error->isEmpty())
            *error = tr("Could not save recent libraries.");
        return false;
    }
    const auto actual = StorageProfile::inspect(info.root, daily_root_, error);
    if (!actual || actual->id != info.id) {
        if (error->isEmpty())
            *error = tr(
                "The library at this location has changed. Open it again to confirm its identity."
            );
        return false;
    }
    if (!locationAllowed(info.root, recent(error), error))
        return false;
    QJsonArray recent_entries{encode(*actual)};
    for (const auto& value : document.value("recent").toArray()) {
        if (decode(value.toObject()).root != info.root && recent_entries.size() < maximum_recent)
            recent_entries.append(value);
    }
    document.insert("recent", recent_entries);
    auto lanes = document.value("launch").toObject();
    // An intentional daily open in a Debug process never changes its safe
    // automatic startup choice. The normal lane is untouched as well.
    if (!context.isEmpty() && (context != "debug" || actual->kind != "daily"))
        lanes.insert(context, encode(*actual));
    document.insert("launch", lanes);
    QSaveFile file(QDir(registry_root_).filePath(registry_name));
    const auto bytes = QJsonDocument(document).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = tr("Could not save recent libraries.");
        return false;
    }
    return true;
}

bool StorageProfileRegistry::resetRecentRecords(QString* const error) const {
    const QString path = QDir(registry_root_).filePath(registry_name);
    if (registry_root_.isEmpty() || daily_root_.isEmpty()
        || StorageProfile::rootsOverlap(daily_root_, registry_root_)
        || StorageProfile::rootsOverlap(daily_root_ + "-debug", registry_root_)
        || StorageProfile::canonicalRoot(path) != path) {
        *error = tr("The library registry location is unavailable.");
        return false;
    }
    const QString lock_path = QDir(registry_root_).filePath("registry.lock");
    if (StorageProfile::canonicalRoot(lock_path) != lock_path) {
        *error = tr("The library registry location is unavailable.");
        return false;
    }
    QLockFile lock(lock_path);
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        *error = tr("Could not save recent libraries.");
        return false;
    }
    const QString backup =
        path + ".preserved-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (QFileInfo::exists(path) && !QFile::rename(path, backup)) {
        *error = tr("Could not preserve the previous library records.");
        return false;
    }
    return true;
}
