#include <QCoreApplication>

#include <cstdlib>

namespace review_shared_grade_test {

void run_projection_apply_contracts();
void run_failure_admission_contracts();

} // namespace review_shared_grade_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_shared_grade_test::run_projection_apply_contracts();
    review_shared_grade_test::run_failure_admission_contracts();
    return EXIT_SUCCESS;
}
