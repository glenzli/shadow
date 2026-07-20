#include "edit_preview_contract.hpp"
#include "edit_preview_provider.hpp"

#include <QBuffer>
#include <QColor>
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

void slots_have_independent_generations() {
    EditPreviewStore store;
    store.publish(
        EditPreviewSlot::Current,
        QByteArrayLiteral("current"),
        QSize(20, 10),
        7
    );
    store.publish(
        EditPreviewSlot::Before,
        QByteArrayLiteral("before"),
        QSize(30, 15),
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
    store->publish(EditPreviewSlot::Current, encoded_square(Qt::red), QSize(2, 2), 10);
    store->publish(EditPreviewSlot::Before, encoded_square(Qt::blue), QSize(2, 2), 20);
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

void stale_result_rules_are_kind_specific() {
    constexpr EditPreviewGeneration current{
        .kind = EditPreviewKind::Current,
        .photo = 4,
        .current_revision = 9,
    };
    constexpr EditPreviewGeneration before{
        .kind = EditPreviewKind::NeutralBefore,
        .photo = 4,
        .current_revision = 0,
    };
    static_assert(accepts_edit_preview(current, 4, 9));
    static_assert(!accepts_edit_preview(current, 4, 10));
    static_assert(!accepts_edit_preview(current, 5, 9));
    static_assert(accepts_edit_preview(before, 4, 99));
    static_assert(!accepts_edit_preview(before, 5, 99));
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
    stale_result_rules_are_kind_specific();
    before_waits_for_the_latest_current_preview();
    return EXIT_SUCCESS;
}
