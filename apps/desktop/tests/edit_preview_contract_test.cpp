#include "edit_preview_contract.hpp"
#include "edit_preview_provider.hpp"

#include <QBuffer>
#include <QColor>
#include <QColorSpace>
#include <QImage>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit preview contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QByteArray encoded_square(const QColor color) {
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly), "test image buffer must open");
    require(image.save(&buffer, "PNG"), "test image must encode");
    return bytes;
}

[[nodiscard]] QByteArray rgb_square(const QColor color) {
    QByteArray bytes(2 * 2 * 3, Qt::Uninitialized);
    for (qsizetype index = 0; index < bytes.size(); index += 3) {
        bytes[index] = static_cast<char>(color.red());
        bytes[index + 1] = static_cast<char>(color.green());
        bytes[index + 2] = static_cast<char>(color.blue());
    }
    return bytes;
}

void slots_have_independent_generations() {
    EditPreviewStore store;
    store.publish(
        EditPreviewSlot::Current,
        QByteArrayLiteral("current"),
        QSize(20, 10),
        {},
        7
    );
    store.publish(
        EditPreviewSlot::Before,
        QByteArrayLiteral("before"),
        QSize(30, 15),
        {},
        3
    );

    require(
        store.snapshot(EditPreviewSlot::Current, 7).bytes == QByteArrayLiteral("current"),
        "current bytes must be read only with the current generation"
    );
    require(
        store.snapshot(EditPreviewSlot::Before, 3).bytes == QByteArrayLiteral("before"),
        "before bytes must be read only with the before generation"
    );
    require(
        store.snapshot(EditPreviewSlot::Current, 3).bytes.isEmpty(),
        "a before generation must not address the current slot"
    );
    require(
        store.snapshot(EditPreviewSlot::Before, 7).bytes.isEmpty(),
        "a current generation must not address the before slot"
    );

    store.clearAll(8, 4);
    require(
        store.snapshot(EditPreviewSlot::Current, 7).bytes.isEmpty()
            && store.snapshot(EditPreviewSlot::Before, 3).bytes.isEmpty(),
        "changing photos must invalidate both old slot generations"
    );
}

void provider_routes_only_named_slots() {
    auto store = std::make_shared<EditPreviewStore>();
    store->publish(
        EditPreviewSlot::Current, encoded_square(Qt::red), QSize(2, 2), {}, 10
    );
    store->publish(
        EditPreviewSlot::Before, encoded_square(Qt::blue), QSize(2, 2), {}, 20
    );
    EditPreviewProvider provider(store);

    QSize decoded_size;
    const QImage current = provider.requestImage(
        QStringLiteral("current?generation=10"),
        &decoded_size,
        {}
    );
    require(!current.isNull() && current.pixelColor(0, 0) == QColor(Qt::red),
            "the current URL must decode the current slot");
    const QImage before = provider.requestImage(
        QStringLiteral("before?generation=20"),
        &decoded_size,
        {}
    );
    require(!before.isNull() && before.pixelColor(0, 0) == QColor(Qt::blue),
            "the before URL must decode the before slot");
    require(
        provider.requestImage(QStringLiteral("before?generation=10"), nullptr, {}).isNull(),
        "a generation from the other slot must be rejected"
    );
    require(
        provider.requestImage(QStringLiteral("raw?generation=20"), nullptr, {}).isNull(),
        "an unknown semantic slot must be rejected"
    );
}

void detail_tiles_are_atomic_and_generation_guarded() {
    auto store = std::make_shared<EditPreviewStore>();
    constexpr EditDetailGeneration first{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 2,
    };
    QVector<EditPreviewStore::DetailPublication> publications;
    publications.push_back({
        .ticket = QStringLiteral("0-0"),
        .bytes = rgb_square(Qt::green),
        .dimensions = QSize(2, 2),
        .row_stride_bytes = 6,
    });
    store->publishDetails(std::move(publications), first);
    const auto stored_pixels = store->detailSnapshot(QStringLiteral("0-0"), first);
    const auto* const stored_address = reinterpret_cast<const uchar*>(
        stored_pixels.bytes.constData()
    );
    EditPreviewProvider provider(store);

    const QImage current = provider.requestImage(
        QStringLiteral("detail/0-0?photo=4&recipe=9&viewport=2"),
        nullptr,
        {}
    );
    require(
        !current.isNull() && current.pixelColor(0, 0) == QColor(Qt::green),
        "a detail URL must resolve only its exact generation"
    );
    require(
        current.colorSpace() == QColorSpace(QColorSpace::SRgb),
        "raw detail pixels must carry an explicit display-sRGB contract"
    );
    require(
        current.constBits() == stored_address,
        "detail provider must retain the immutable store bytes without a viewport copy"
    );
    require(
        provider
            .requestImage(
                QStringLiteral("detail/0-0?photo=4&recipe=10&viewport=2"),
                nullptr,
                {}
            )
            .isNull(),
        "a stale Recipe generation must not address detail pixels"
    );

    store->clearDetails(EditDetailGeneration{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 3,
    });
    require(
        provider
            .requestImage(
                QStringLiteral("detail/0-0?photo=4&recipe=9&viewport=2"),
                nullptr,
                {}
            )
            .isNull(),
        "advancing the viewport invalidates every prior tile atomically"
    );
    require(
        current.pixelColor(0, 0) == QColor(Qt::green),
        "an image already handed to Qt must retain its pixels after store invalidation"
    );

    constexpr EditDetailGeneration malformed{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 4,
    };
    QVector<EditPreviewStore::DetailPublication> malformed_publications;
    malformed_publications.push_back({
        .ticket = QStringLiteral("bad-stride"),
        .bytes = rgb_square(Qt::red),
        .dimensions = QSize(2, 2),
        .row_stride_bytes = 5,
    });
    store->publishDetails(std::move(malformed_publications), malformed);
    require(
        provider
            .requestImage(
                QStringLiteral("detail/bad-stride?photo=4&recipe=9&viewport=4"),
                nullptr,
                {}
            )
            .isNull(),
        "detail provider must reject non-tight RGB8 rows"
    );
}

void stale_result_rules_are_kind_specific() {
    constexpr EditPreviewGeneration current{
        .policy = EditPreviewPolicy::Settled,
        .photo = 4,
        .current_revision = 9,
    };
    constexpr EditPreviewGeneration before{
        .policy = EditPreviewPolicy::NeutralBefore,
        .photo = 4,
        .current_revision = 0,
    };
    static_assert(accepts_edit_preview(current, 4, 9));
    static_assert(!accepts_edit_preview(current, 4, 10));
    static_assert(!accepts_edit_preview(current, 5, 9));
    static_assert(accepts_edit_preview(before, 4, 99));
    static_assert(!accepts_edit_preview(before, 5, 99));
    static_assert(can_present_edit_preview(current, 4, 9));
    static_assert(can_present_edit_preview(current, 4, 10));
    static_assert(!can_present_edit_preview(current, 5, 10));
    static_assert(!can_present_edit_preview(before, 4, 10));

    static_assert(edit_preview_kind(EditPreviewPolicy::Interactive)
                  == EditPreviewKind::Current);
    static_assert(edit_preview_kind(EditPreviewPolicy::Settled)
                  == EditPreviewKind::Current);
    static_assert(edit_preview_kind(EditPreviewPolicy::NeutralBefore)
                  == EditPreviewKind::NeutralBefore);
    static_assert(!edit_preview_requires_analysis(EditPreviewPolicy::Interactive));
    static_assert(edit_preview_requires_analysis(EditPreviewPolicy::Settled));
    static_assert(edit_preview_requires_analysis(EditPreviewPolicy::NeutralBefore));
    static_assert(!edit_preview_admits_durable_cache(EditPreviewPolicy::Interactive));
    static_assert(edit_preview_admits_durable_cache(EditPreviewPolicy::Settled));
    static_assert(!edit_preview_admits_durable_cache(EditPreviewPolicy::NeutralBefore));
    static_assert(
        !edit_preview_requires_display_diagnostics(EditPreviewPolicy::Interactive)
    );

    constexpr EditDetailGeneration detail{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 3,
    };
    static_assert(accepts_edit_detail(detail, 4, 9, 3));
    static_assert(!accepts_edit_detail(detail, 5, 9, 3));
    static_assert(!accepts_edit_detail(detail, 4, 10, 3));
    static_assert(!accepts_edit_detail(detail, 4, 9, 4));
}

void before_waits_for_the_latest_current_preview() {
    constexpr NeutralBeforeStartState ready{
        .requested = true,
        .active = true,
        .settled_current_revision = 12,
        .current_revision = 12,
    };
    static_assert(can_start_neutral_before(ready));

    auto unsettled = ready;
    unsettled.current_revision = 13;
    require(
        !can_start_neutral_before(unsettled),
        "before must wait for the latest current revision to settle"
    );
    auto rendering = ready;
    rendering.current_rendering = true;
    require(
        !can_start_neutral_before(rendering),
        "before must never render concurrently with current"
    );
    auto scheduled = ready;
    scheduled.current_scheduled = true;
    require(
        !can_start_neutral_before(scheduled),
        "before must not jump ahead of a debounced current render"
    );
}

} // namespace

int main() {
    slots_have_independent_generations();
    provider_routes_only_named_slots();
    detail_tiles_are_atomic_and_generation_guarded();
    stale_result_rules_are_kind_specific();
    before_waits_for_the_latest_current_preview();
    return EXIT_SUCCESS;
}
