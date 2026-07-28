#include <QCoreApplication>

#include <cstdlib>

namespace review_source_health_test {

void run_source_health_refresh_contracts();
void run_missing_location_review_contracts();
void run_relink_lifetime_contracts();

} // namespace review_source_health_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_source_health_test::run_source_health_refresh_contracts();
    review_source_health_test::run_missing_location_review_contracts();
    review_source_health_test::run_relink_lifetime_contracts();
    return EXIT_SUCCESS;
}
