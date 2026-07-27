//! Actor messages for human decisions and explicit feedback evidence.

use std::{collections::BTreeSet, sync::mpsc::SyncSender};

use shadow_ai::{
    FeedbackEvent, FeedbackForgetFact, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact,
};
use shadow_domain::{NewPhotoDecisionEvent, PhotoDecisionEvent, PhotoDecisionState, PhotoId};

use crate::{CatalogError, FeedbackPage, PhotoDecisionPage};

pub(in crate::writer) enum DecisionMessage {
    State(
        PhotoId,
        SyncSender<Result<PhotoDecisionState, CatalogError>>,
    ),
    Append(
        Box<NewPhotoDecisionEvent>,
        SyncSender<Result<PhotoDecisionEvent, CatalogError>>,
    ),
    EventsAfter(
        PhotoId,
        u64,
        usize,
        SyncSender<Result<PhotoDecisionPage, CatalogError>>,
    ),
}

pub(in crate::writer) enum FeedbackMessage {
    AppendEvent(
        Box<NewFeedbackEvent>,
        SyncSender<Result<FeedbackEvent, CatalogError>>,
    ),
    EventsAfter(
        LearningScope,
        u64,
        usize,
        SyncSender<Result<FeedbackPage, CatalogError>>,
    ),
    AppendForgetFact(
        Box<NewFeedbackForgetFact>,
        SyncSender<Result<FeedbackForgetFact, CatalogError>>,
    ),
    ForgottenEventIds(
        LearningScope,
        SyncSender<Result<BTreeSet<String>, CatalogError>>,
    ),
}
