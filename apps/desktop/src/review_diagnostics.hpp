#pragma once

#include "review_diagnostic_log.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>

// Process-local, bounded Review diagnostics. Timing is opt-in; failures are
// summarized even without timing so a broken visual cannot flood the log.
// No photo identity, path, ticket, or image payload enters this recorder.
class ReviewDiagnostics final {
  public:
    enum class Stage : std::uint8_t {
        PageWait,
        PageBackend,
        PageProjection,
        GroupingCompile,
        LayoutRebuild,
        LayoutAppend,
        ThumbnailLoad,
        ThumbnailDecode,
        Count,
    };

    enum class Counter : std::uint8_t {
        ThumbnailCacheHit,
        StalePage,
        Count,
    };

    enum class Failure : std::uint8_t {
        Page,
        ThumbnailLoad,
        ThumbnailDecode,
        ThumbnailReceipt,
        Count,
    };

    static ReviewDiagnostics& instance() {
        static ReviewDiagnostics diagnostics;
        return diagnostics;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_;
    }

    [[nodiscard]] bool configureLogDirectory(const QString& application_data) {
        const QMutexLocker lock(&mutex_);
        return log_.configure(application_data);
    }

    // Persist the completed first page before the next event window. This
    // makes short startup failures inspectable after an abnormal exit.
    void checkpoint() {
        if (!enabled_) {
            return;
        }
        const QMutexLocker lock(&mutex_);
        if (events_ != 0) {
            flush(hasFailures());
        }
    }

    void record(const Stage stage, const qint64 duration_ms, const int units = 0) {
        if (!enabled_) {
            return;
        }
        const QMutexLocker lock(&mutex_);
        metrics_[static_cast<std::size_t>(stage)].add(duration_ms, units);
        ++events_;
        flushIfDue();
    }

    void count(const Counter counter) {
        if (!enabled_) {
            return;
        }
        const QMutexLocker lock(&mutex_);
        ++counters_[static_cast<std::size_t>(counter)];
        ++events_;
        flushIfDue();
    }

    void failure(const Failure kind) {
        const QMutexLocker lock(&mutex_);
        ++failures_[static_cast<std::size_t>(kind)];
        ++events_;
        // Preserve the first failure immediately. Subsequent failures share
        // the same bounded event/time window as performance observations.
        if (!failure_reported_) {
            failure_reported_ = true;
            flush(true);
        } else {
            flushIfDue();
        }
    }

  private:
    static constexpr std::array<qint64, 13> bucket_limits{
        1,
        2,
        4,
        8,
        16,
        32,
        64,
        128,
        256,
        512,
        1024,
        2048,
        4096,
    };
    static constexpr std::array<const char*, 8> stage_names{
        "page_request_to_rows",
        "page_backend",
        "page_projection",
        "grouping_compile",
        "layout_rebuild",
        "layout_append",
        "thumbnail_load",
        "thumbnail_decode",
    };
    static constexpr std::array<const char*, 2> counter_names{
        "thumbnail_cache_hits",
        "stale_pages",
    };
    static constexpr std::array<const char*, 4> failure_names{
        "page_errors",
        "thumbnail_load_errors",
        "thumbnail_decode_errors",
        "thumbnail_receipt_errors",
    };

    struct Metric final {
        quint64 count = 0;
        qint64 max_ms = 0;
        int max_units = 0;
        std::array<quint64, bucket_limits.size() + 1> buckets{};

        void add(const qint64 duration_ms, const int units) {
            const qint64 bounded_ms = std::max<qint64>(0, duration_ms);
            ++count;
            max_ms = std::max(max_ms, bounded_ms);
            max_units = std::max(max_units, units);
            std::size_t bucket = 0;
            while (bucket < bucket_limits.size() && bounded_ms > bucket_limits[bucket]) {
                ++bucket;
            }
            ++buckets[bucket];
        }

        [[nodiscard]] qint64 p95UpperMs() const {
            const quint64 rank = (count * 95 + 99) / 100;
            quint64 cumulative = 0;
            for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
                cumulative += buckets[bucket];
                if (cumulative >= rank) {
                    return bucket < bucket_limits.size() ? bucket_limits[bucket] : max_ms;
                }
            }
            return 0;
        }
    };

    ReviewDiagnostics() :
        enabled_(
            qEnvironmentVariable("SHADOW_REVIEW_DIAGNOSTICS") == "1"
            || qEnvironmentVariableIsSet("SHADOW_INTERACTIVE_TIMING")
        ) {
        window_clock_.start();
    }

    ~ReviewDiagnostics() {
        const QMutexLocker lock(&mutex_);
        if (events_ != 0) {
            flush(hasFailures());
        }
    }

    void flushIfDue() {
        if ((!first_timing_reported_ && enabled_ && events_ >= 16) || events_ >= 2048
            || window_clock_.elapsed() >= 30'000) {
            flush(hasFailures());
        }
    }

    [[nodiscard]] bool hasFailures() const {
        return std::any_of(failures_.cbegin(), failures_.cend(), [](const quint64 count) {
            return count != 0;
        });
    }

    void flush(const bool warning) {
        if (events_ == 0) {
            return;
        }
        QStringList fields{
            QStringLiteral("Review diagnostics"),
            QStringLiteral("window_ms=%1").arg(window_clock_.elapsed()),
            QStringLiteral("events=%1").arg(events_),
        };
        for (std::size_t index = 0; index < metrics_.size(); ++index) {
            const Metric& metric = metrics_[index];
            if (metric.count == 0) {
                continue;
            }
            fields.push_back(QStringLiteral("%1=%2,p95_le_ms:%3,max_ms:%4,max_units:%5")
                                 .arg(QLatin1String(stage_names[index]))
                                 .arg(metric.count)
                                 .arg(metric.p95UpperMs())
                                 .arg(metric.max_ms)
                                 .arg(metric.max_units));
        }
        for (std::size_t index = 0; index < counters_.size(); ++index) {
            if (counters_[index] != 0) {
                fields.push_back(QStringLiteral("%1=%2")
                                     .arg(QLatin1String(counter_names[index]))
                                     .arg(counters_[index]));
            }
        }
        for (std::size_t index = 0; index < failures_.size(); ++index) {
            if (failures_[index] != 0) {
                fields.push_back(QStringLiteral("%1=%2")
                                     .arg(QLatin1String(failure_names[index]))
                                     .arg(failures_[index]));
            }
        }
        const QString summary = fields.join(QLatin1Char(' '));
        if (log_.configured() && !log_.append(summary) && !persistence_warning_reported_) {
            persistence_warning_reported_ = true;
            qWarning() << "Review diagnostic log is unavailable; summaries remain on the console";
        }
        if (warning) {
            qWarning().noquote() << summary;
        } else {
            qInfo().noquote() << summary;
        }
        metrics_ = {};
        counters_ = {};
        failures_ = {};
        events_ = 0;
        first_timing_reported_ = true;
        window_clock_.restart();
    }

    const bool enabled_;
    QMutex mutex_;
    QElapsedTimer window_clock_;
    std::array<Metric, static_cast<std::size_t>(Stage::Count)> metrics_{};
    std::array<quint64, static_cast<std::size_t>(Counter::Count)> counters_{};
    std::array<quint64, static_cast<std::size_t>(Failure::Count)> failures_{};
    quint64 events_ = 0;
    bool first_timing_reported_ = false;
    bool failure_reported_ = false;
    ReviewDiagnosticLog log_;
    bool persistence_warning_reported_ = false;
};
