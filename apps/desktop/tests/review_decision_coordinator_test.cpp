#include <QCoreApplication>

#include <cstdlib>

namespace review_decision_test {

void run_admission_projection_contracts();
void run_failure_undo_contracts();
void run_lifetime_contracts();

} // namespace review_decision_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_decision_test::run_admission_projection_contracts();
    review_decision_test::run_failure_undo_contracts();
    review_decision_test::run_lifetime_contracts();
    return EXIT_SUCCESS;
}
