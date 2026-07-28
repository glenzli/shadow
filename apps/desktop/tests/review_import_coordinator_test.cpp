#include <QCoreApplication>

#include <cstdlib>

namespace review_import_test {

void run_import_progress_contracts();
void run_cancellation_failure_lifetime_contracts();

} // namespace review_import_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_import_test::run_import_progress_contracts();
    review_import_test::run_cancellation_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
