#include "review_visual_request.hpp"

#include <QUrl>
#include <QUrlQuery>

namespace {

[[nodiscard]] QString lifetime_name(const ReviewVisualLifetime lifetime) {
    switch (lifetime) {
    case ReviewVisualLifetime::Grid:
        return QStringLiteral("grid");
    case ReviewVisualLifetime::Comparison:
        return QStringLiteral("comparison");
    }
    return {};
}

[[nodiscard]] std::optional<ReviewVisualLifetime> parse_lifetime(
    const QString& value
) {
    if (value == QStringLiteral("grid")) {
        return ReviewVisualLifetime::Grid;
    }
    if (value == QStringLiteral("comparison")) {
        return ReviewVisualLifetime::Comparison;
    }
    return std::nullopt;
}

} // namespace

QString reviewVisualSource(
    const QString& ticket,
    const quint64 generation,
    const ReviewVisualLifetime lifetime
) {
    if (ticket.isEmpty()) {
        return {};
    }

    QUrlQuery query;
    // Grid tickets identify immutable artifacts. Keep Qt's decoded-image/texture
    // cache identity stable across navigation; comparisons still expire per query.
    const quint64 request_generation = lifetime == ReviewVisualLifetime::Grid ? 0 : generation;
    query.addQueryItem(QStringLiteral("generation"), QString::number(request_generation));
    query.addQueryItem(QStringLiteral("lifetime"), lifetime_name(lifetime));
    query.addQueryItem(
        QStringLiteral("ticket"),
        QString::fromLatin1(QUrl::toPercentEncoding(ticket))
    );
    return QStringLiteral("image://shadow/visual?%1")
        .arg(query.toString(QUrl::FullyEncoded));
}

std::optional<ReviewVisualRequest> parseReviewVisualRequest(const QString& id) {
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QString resource = query_start >= 0 ? id.left(query_start) : id;
    if (resource != QStringLiteral("visual") || query_start < 0) {
        return std::nullopt;
    }

    const QUrlQuery query(id.mid(query_start + 1));
    if (query.queryItems(QUrl::FullyDecoded).size() != 3) {
        return std::nullopt;
    }
    const auto generation_values = query.allQueryItemValues(
        QStringLiteral("generation"),
        QUrl::FullyDecoded
    );
    const auto lifetime_values = query.allQueryItemValues(
        QStringLiteral("lifetime"),
        QUrl::FullyDecoded
    );
    const auto ticket_values = query.allQueryItemValues(
        QStringLiteral("ticket"),
        QUrl::FullyDecoded
    );
    if (generation_values.size() != 1 || lifetime_values.size() != 1
        || ticket_values.size() != 1 || ticket_values.constFirst().isEmpty()) {
        return std::nullopt;
    }

    bool valid_generation = false;
    const quint64 generation =
        generation_values.constFirst().toULongLong(&valid_generation);
    const auto lifetime = parse_lifetime(lifetime_values.constFirst());
    if (!valid_generation || !lifetime) {
        return std::nullopt;
    }
    return ReviewVisualRequest{
        .lifetime = *lifetime,
        .generation = generation,
        .ticket = ticket_values.constFirst(),
    };
}
