#include "review_visual_request.hpp"

#include <QUrl>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Review visual request contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QString provider_id(const QString& source) {
    const QUrl url(source);
    QString id = url.path();
    if (id.startsWith(QLatin1Char('/'))) {
        id.remove(0, 1);
    }
    if (!url.query(QUrl::FullyEncoded).isEmpty()) {
        id += QLatin1Char('?');
        id += url.query(QUrl::FullyEncoded);
    }
    return id;
}

void canonical_sources_round_trip_opaque_tickets() {
    const QString ticket = QStringLiteral("ticket:/a b?x=1&literal=%2F#tail");
    for (const auto lifetime : {
             ReviewVisualLifetime::Grid,
             ReviewVisualLifetime::Comparison,
         }) {
        const QString source = reviewVisualSource(ticket, 42, lifetime);
        const auto request = parseReviewVisualRequest(provider_id(source));
        require(
            QUrl(source).scheme() == QStringLiteral("image")
                && QUrl(source).host() == QStringLiteral("shadow")
                && QUrl(source).path() == QStringLiteral("/visual") && request
                && request->ticket == ticket
                && request->generation == (lifetime == ReviewVisualLifetime::Grid ? 0 : 42)
                && request->lifetime == lifetime,
            "the canonical URL must preserve its exact ticket, generation, and lifetime"
        );
    }
    require(
        reviewVisualSource({}, 42, ReviewVisualLifetime::Grid).isEmpty(),
        "an empty ticket must not produce a provider request"
    );
}

void lifetimes_define_distinct_generation_and_receipt_policy() {
    const ReviewVisualRequest grid{
        .lifetime = ReviewVisualLifetime::Grid,
        .generation = 7,
        .ticket = QStringLiteral("grid"),
    };
    const ReviewVisualRequest comparison{
        .lifetime = ReviewVisualLifetime::Comparison,
        .generation = 7,
        .ticket = QStringLiteral("comparison"),
    };
    require(
        !grid.requiresCurrentGeneration() && !grid.requiresFrameReceipt(),
        "a signed immutable grid request must be allowed to finish across generations"
    );
    require(
        comparison.requiresCurrentGeneration() && comparison.requiresFrameReceipt(),
        "a comparison request must remain generation-bound and receipt-bound"
    );
}

void malformed_or_ambiguous_requests_fail_closed() {
    for (const QString& id : {
             QStringLiteral("other?generation=1&lifetime=grid&ticket=a"),
             QStringLiteral("visual?lifetime=grid&ticket=a"),
             QStringLiteral("visual?generation=x&lifetime=grid&ticket=a"),
             QStringLiteral("visual?generation=1&lifetime=future&ticket=a"),
             QStringLiteral("visual?generation=1&lifetime=grid&ticket="),
             QStringLiteral(
                 "visual?generation=1&generation=2&lifetime=grid&ticket=a"
             ),
             QStringLiteral(
                 "visual?generation=1&lifetime=grid&ticket=a&extra=true"
             ),
         }) {
        require(
            !parseReviewVisualRequest(id),
            "unknown, incomplete, duplicate, or extended URL grammar must be rejected"
        );
    }
}

} // namespace

int main() {
    canonical_sources_round_trip_opaque_tickets();
    lifetimes_define_distinct_generation_and_receipt_policy();
    malformed_or_ambiguous_requests_fail_closed();
    return EXIT_SUCCESS;
}
