#include <cstdlib>

namespace review_model_test {

void role_names_and_types_are_stable();
void library_state_is_catalog_authoritative_and_photo_scoped();
void decision_updates_project_to_the_single_photo_row();
void visual_sources_use_encoded_tickets_and_current_generation();
void generation_advance_reuses_immutable_grid_visuals();
void absence_and_legitimate_zero_are_distinct();
void replace_and_append_keep_their_items_intact();
void snapshot_reconciliation_updates_visual_and_technical_roles_in_place();
void photo_identity_survives_representation_relink();
void local_source_availability_is_exact_and_remote_rows_are_excluded();
void snapshot_reconciliation_moves_rows_without_losing_persistent_identity();
void snapshot_reconciliation_inserts_and_removes_keyed_rows();
void prefix_reconciliation_updates_the_front_without_dropping_loaded_tail();
void prefix_and_append_reject_duplicates_without_mutating_the_model();
void snapshot_reconciliation_rejects_wrong_generation_without_mutation();
void identical_snapshot_reconciliation_is_a_signal_free_no_op();
void remote_rows_use_local_proxy_urls_and_replace_without_resetting_local_rows();
void local_and_remote_rows_share_one_presentation_order();
void remote_replacement_rejects_local_items_and_key_collisions();

} // namespace review_model_test

int main() {
    using namespace review_model_test;
    role_names_and_types_are_stable();
    library_state_is_catalog_authoritative_and_photo_scoped();
    decision_updates_project_to_the_single_photo_row();
    visual_sources_use_encoded_tickets_and_current_generation();
    generation_advance_reuses_immutable_grid_visuals();
    absence_and_legitimate_zero_are_distinct();
    replace_and_append_keep_their_items_intact();
    snapshot_reconciliation_updates_visual_and_technical_roles_in_place();
    photo_identity_survives_representation_relink();
    local_source_availability_is_exact_and_remote_rows_are_excluded();
    snapshot_reconciliation_moves_rows_without_losing_persistent_identity();
    snapshot_reconciliation_inserts_and_removes_keyed_rows();
    prefix_reconciliation_updates_the_front_without_dropping_loaded_tail();
    prefix_and_append_reject_duplicates_without_mutating_the_model();
    snapshot_reconciliation_rejects_wrong_generation_without_mutation();
    identical_snapshot_reconciliation_is_a_signal_free_no_op();
    remote_rows_use_local_proxy_urls_and_replace_without_resetting_local_rows();
    local_and_remote_rows_share_one_presentation_order();
    remote_replacement_rejects_local_items_and_key_collisions();
    return EXIT_SUCCESS;
}
