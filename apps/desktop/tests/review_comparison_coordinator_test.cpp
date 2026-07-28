#include <QCoreApplication>

#include <cstdlib>

namespace review_comparison_test {

void run_presentation_contracts();
void run_evidence_lifecycle_contracts();
void run_receipt_validation_contracts();
void run_failure_lifetime_contracts();

} // namespace review_comparison_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_comparison_test::run_presentation_contracts();
    review_comparison_test::run_evidence_lifecycle_contracts();
    review_comparison_test::run_receipt_validation_contracts();
    review_comparison_test::run_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
