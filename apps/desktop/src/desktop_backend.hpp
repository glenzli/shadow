#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

struct BackendScanReport final {
    QString folder_path;
    std::uint64_t files_seen = 0;
    std::uint64_t supported_files = 0;
    std::uint64_t decode_queued = 0;
    std::uint64_t issue_count = 0;
};

struct BackendReviewItem final {
    QString photo_id;
    QString representation_id;
    QString title;
    QString source_path;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
};

struct BackendReviewPage final {
    QVector<BackendReviewItem> items;
    QString next_cursor_path;
    QString next_cursor_representation_id;
    std::uint64_t total_items = 0;
    bool has_more = false;
};

class DesktopBackend final {
public:
    DesktopBackend(const QString& catalog_path, const QString& cache_root);
    ~DesktopBackend();

    DesktopBackend(const DesktopBackend&) = delete;
    DesktopBackend& operator=(const DesktopBackend&) = delete;

    [[nodiscard]] BackendScanReport scanFolder(const QString& folder_path) const;
    [[nodiscard]] BackendReviewPage reviewPage(
        const QString& cursor_path,
        const QString& cursor_representation_id,
        std::uint32_t limit
    ) const;
    [[nodiscard]] QByteArray loadReviewVisual(const QString& representation_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
