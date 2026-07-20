use std::{fmt, str::FromStr};

use serde::{Deserialize, Serialize};
use uuid::Uuid;

/// Common behavior for all strongly typed, persistent Shadow identifiers.
pub trait EntityId:
    Copy + Clone + Eq + Ord + std::hash::Hash + fmt::Debug + fmt::Display + Send + Sync + 'static
{
    fn new_v7() -> Self;
    fn from_uuid(value: Uuid) -> Self;
    fn as_uuid(self) -> Uuid;

    fn as_bytes(&self) -> &[u8; 16];
}

macro_rules! entity_id {
    ($name:ident) => {
        #[derive(
            Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Debug, Serialize, Deserialize,
        )]
        #[serde(transparent)]
        pub struct $name(Uuid);

        impl EntityId for $name {
            fn new_v7() -> Self {
                Self(Uuid::now_v7())
            }

            fn from_uuid(value: Uuid) -> Self {
                Self(value)
            }

            fn as_uuid(self) -> Uuid {
                self.0
            }

            fn as_bytes(&self) -> &[u8; 16] {
                self.0.as_bytes()
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.fmt(formatter)
            }
        }

        impl FromStr for $name {
            type Err = uuid::Error;

            fn from_str(value: &str) -> Result<Self, Self::Err> {
                Uuid::parse_str(value).map(Self)
            }
        }
    };
}

entity_id!(PhotoId);
entity_id!(RepresentationId);
entity_id!(LocationId);
entity_id!(ImportSessionId);
entity_id!(RecipeId);
entity_id!(RecipeCommitId);
entity_id!(NodeId);
entity_id!(LayerId);
entity_id!(LayerInstanceId);
entity_id!(LayerRevisionId);
entity_id!(MaskId);
entity_id!(BranchId);
entity_id!(VersionId);
entity_id!(StyleId);
entity_id!(GroupId);
entity_id!(ShootId);
entity_id!(SelectionId);
entity_id!(CollectionId);
entity_id!(OutputTargetId);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn typed_ids_round_trip_without_losing_their_type() {
        let original = PhotoId::new_v7();
        let text = original.to_string();
        let parsed: PhotoId = text.parse().expect("valid photo id");

        assert_eq!(original, parsed);
    }

    #[test]
    fn typed_ids_serialize_as_uuid_strings() {
        let id = RepresentationId::new_v7();
        let json = serde_json::to_string(&id).expect("serialize representation id");
        let decoded: RepresentationId =
            serde_json::from_str(&json).expect("deserialize representation id");

        assert_eq!(id, decoded);
    }
}
