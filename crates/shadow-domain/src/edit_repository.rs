use std::{collections::BTreeSet, fmt, str::FromStr};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de, de::DeserializeOwned};

const OBJECT_HASH_DOMAIN: &[u8] = b"shadow.edit.object.v1";
const COMMIT_HASH_DOMAIN: &[u8] = b"shadow.edit.commit.v1";
const MAX_EDGE_ROLE_BYTES: usize = 64;
const MAX_ENTITY_KEY_BYTES: usize = 512;
const MAX_COMMIT_MESSAGE_BYTES: usize = 4 * 1024;
const MAX_COMMIT_PARENTS: usize = 8;

macro_rules! digest_id {
    ($name:ident) => {
        #[derive(Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash)]
        pub struct $name([u8; 32]);

        impl $name {
            #[must_use]
            pub const fn from_bytes(bytes: [u8; 32]) -> Self {
                Self(bytes)
            }

            #[must_use]
            pub const fn as_bytes(&self) -> &[u8; 32] {
                &self.0
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                for byte in self.0 {
                    write!(formatter, "{byte:02x}")?;
                }
                Ok(())
            }
        }

        impl fmt::Debug for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter
                    .debug_tuple(stringify!($name))
                    .field(&self.to_string())
                    .finish()
            }
        }

        impl FromStr for $name {
            type Err = EditRepositoryError;

            fn from_str(value: &str) -> Result<Self, Self::Err> {
                if value.len() != 64 || !value.is_ascii() {
                    return Err(EditRepositoryError::InvalidDigest(value.to_owned()));
                }
                let mut bytes = [0_u8; 32];
                for (index, output) in bytes.iter_mut().enumerate() {
                    let offset = index * 2;
                    let pair = &value.as_bytes()[offset..offset + 2];
                    let high = decode_hex(pair[0])
                        .ok_or_else(|| EditRepositoryError::InvalidDigest(value.to_owned()))?;
                    let low = decode_hex(pair[1])
                        .ok_or_else(|| EditRepositoryError::InvalidDigest(value.to_owned()))?;
                    *output = (high << 4) | low;
                }
                Ok(Self(bytes))
            }
        }

        impl Serialize for $name {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: Serializer,
            {
                serializer.serialize_str(&self.to_string())
            }
        }

        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: Deserializer<'de>,
            {
                let value = String::deserialize(deserializer)?;
                value.parse().map_err(de::Error::custom)
            }
        }
    };
}

digest_id!(EditObjectId);
digest_id!(EditCommitId);

const fn decode_hex(value: u8) -> Option<u8> {
    match value {
        b'0'..=b'9' => Some(value - b'0'),
        b'a'..=b'f' => Some(value - b'a' + 10),
        b'A'..=b'F' => Some(value - b'A' + 10),
        _ => None,
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EditObjectKind {
    LibraryRoot,
    EntityMap,
    PhotoEditState,
    GradeNodeRevision,
    MaskRevision,
    StyleRevision,
    OutputState,
    LegacyRecipe,
}

impl EditObjectKind {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::LibraryRoot => "library_root",
            Self::EntityMap => "entity_map",
            Self::PhotoEditState => "photo_edit_state",
            Self::GradeNodeRevision => "grade_node_revision",
            Self::MaskRevision => "mask_revision",
            Self::StyleRevision => "style_revision",
            Self::OutputState => "output_state",
            Self::LegacyRecipe => "legacy_recipe",
        }
    }
}

impl FromStr for EditObjectKind {
    type Err = EditRepositoryError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "library_root" => Ok(Self::LibraryRoot),
            "entity_map" => Ok(Self::EntityMap),
            "photo_edit_state" => Ok(Self::PhotoEditState),
            "grade_node_revision" => Ok(Self::GradeNodeRevision),
            "mask_revision" => Ok(Self::MaskRevision),
            "style_revision" => Ok(Self::StyleRevision),
            "output_state" => Ok(Self::OutputState),
            "legacy_recipe" => Ok(Self::LegacyRecipe),
            _ => Err(EditRepositoryError::UnknownObjectKind(value.to_owned())),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditObject {
    id: EditObjectId,
    kind: EditObjectKind,
    format_version: u32,
    canonical_json: Vec<u8>,
}

impl EditObject {
    /// Serializes one supported versioned payload into canonical JSON and
    /// derives its domain-separated content identifier.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for an unsupported version or a payload
    /// that cannot be represented as JSON.
    pub fn from_canonical_json<T: Serialize>(
        kind: EditObjectKind,
        format_version: u32,
        value: &T,
    ) -> Result<Self, EditRepositoryError> {
        if format_version == 0 {
            return Err(EditRepositoryError::InvalidFormatVersion);
        }
        validate_supported_object_version(kind, format_version)?;
        let canonical_json = canonical_json(value)?;
        let id = object_id(kind, format_version, &canonical_json);
        Ok(Self {
            id,
            kind,
            format_version,
            canonical_json,
        })
    }

    /// Reconstructs an object only when its bytes are canonical and match the
    /// supplied content identifier.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for unsupported versions, malformed or
    /// non-canonical JSON, or a digest mismatch.
    pub fn from_stored_parts(
        id: EditObjectId,
        kind: EditObjectKind,
        format_version: u32,
        canonical_json: Vec<u8>,
    ) -> Result<Self, EditRepositoryError> {
        if format_version == 0 {
            return Err(EditRepositoryError::InvalidFormatVersion);
        }
        validate_supported_object_version(kind, format_version)?;
        if object_id(kind, format_version, &canonical_json) != id {
            return Err(EditRepositoryError::ObjectDigestMismatch { id });
        }
        let value: serde_json::Value = serde_json::from_slice(&canonical_json)?;
        if serde_json::to_vec(&value)? != canonical_json {
            return Err(EditRepositoryError::NonCanonicalObjectPayload);
        }
        Ok(Self {
            id,
            kind,
            format_version,
            canonical_json,
        })
    }

    #[must_use]
    pub const fn id(&self) -> EditObjectId {
        self.id
    }

    #[must_use]
    pub const fn kind(&self) -> EditObjectKind {
        self.kind
    }

    #[must_use]
    pub const fn format_version(&self) -> u32 {
        self.format_version
    }

    #[must_use]
    pub fn canonical_json(&self) -> &[u8] {
        &self.canonical_json
    }

    /// Decodes an integrity-checked object payload into its versioned type.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the requested type does not match
    /// the stored JSON payload.
    pub fn decode<T: DeserializeOwned>(&self) -> Result<T, EditRepositoryError> {
        serde_json::from_slice(&self.canonical_json).map_err(Into::into)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Serialize, Deserialize)]
pub struct EditObjectEdge {
    role: String,
    position: u32,
    target: EditObjectId,
}

impl EditObjectEdge {
    /// Creates one indexed reference from an object payload to another object.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the role is empty, too long, or
    /// contains characters outside the stable edge-role alphabet.
    pub fn new(
        role: impl Into<String>,
        position: u32,
        target: EditObjectId,
    ) -> Result<Self, EditRepositoryError> {
        let role = role.into();
        validate_edge_role(&role)?;
        Ok(Self {
            role,
            position,
            target,
        })
    }

    #[must_use]
    pub fn role(&self) -> &str {
        &self.role
    }

    #[must_use]
    pub const fn position(&self) -> u32 {
        self.position
    }

    #[must_use]
    pub const fn target(&self) -> EditObjectId {
        self.target
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditObjectPack {
    object: EditObject,
    edges: Vec<EditObjectEdge>,
}

impl EditObjectPack {
    /// Binds one immutable object to the edge index derived from its payload.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for duplicate edges or when the index
    /// disagrees with a typed payload such as a Library root or entity map.
    pub fn new(
        object: EditObject,
        mut edges: Vec<EditObjectEdge>,
    ) -> Result<Self, EditRepositoryError> {
        edges.sort();
        if edges
            .windows(2)
            .any(|pair| pair[0].role == pair[1].role && pair[0].position == pair[1].position)
        {
            return Err(EditRepositoryError::DuplicateObjectEdge);
        }
        let pack = Self { object, edges };
        pack.validate_typed_edges()?;
        Ok(pack)
    }

    #[must_use]
    pub const fn object(&self) -> &EditObject {
        &self.object
    }

    #[must_use]
    pub fn edges(&self) -> &[EditObjectEdge] {
        &self.edges
    }

    fn validate_typed_edges(&self) -> Result<(), EditRepositoryError> {
        let expected = match (self.object.kind, self.object.format_version) {
            (EditObjectKind::LibraryRoot, 1) => {
                let root: LibraryRootV1 = serde_json::from_slice(self.object.canonical_json())?;
                root.edges()?
            }
            (EditObjectKind::EntityMap, 1) => {
                let map: EditEntityMapV1 = serde_json::from_slice(self.object.canonical_json())?;
                map.validate()?;
                map.edges()?
            }
            (EditObjectKind::LegacyRecipe, 1) => {
                let recipe: crate::RecipeCommit =
                    serde_json::from_slice(self.object.canonical_json())?;
                recipe
                    .validate()
                    .map_err(|error| EditRepositoryError::InvalidLegacyRecipe(error.to_string()))?;
                Vec::new()
            }
            (EditObjectKind::GradeNodeRevision, 1) => {
                let revision: crate::LayerRevision =
                    serde_json::from_slice(self.object.canonical_json())?;
                revision.validate().map_err(|error| {
                    EditRepositoryError::InvalidGradeNodeRevision(error.to_string())
                })?;
                Vec::new()
            }
            _ => Vec::new(),
        };
        if expected != self.edges {
            return Err(EditRepositoryError::ObjectEdgesDoNotMatchPayload);
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct LibraryRootV1 {
    pub photo_recipes: Option<EditObjectId>,
    pub shared_grade_heads: Option<EditObjectId>,
    pub masks: Option<EditObjectId>,
    pub styles: Option<EditObjectId>,
    pub output_states: Option<EditObjectId>,
}

impl LibraryRootV1 {
    /// Decodes a v1 Library root from an object of the exact expected type.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for a kind/version mismatch or malformed
    /// payload JSON.
    pub fn from_object(object: &EditObject) -> Result<Self, EditRepositoryError> {
        require_object_type(object, EditObjectKind::LibraryRoot, 1)?;
        object.decode()
    }

    /// Encodes this root and its deterministic role edges as one object pack.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] if an edge role or payload cannot be
    /// encoded under the v1 contract.
    pub fn into_object_pack(self) -> Result<EditObjectPack, EditRepositoryError> {
        let edges = self.edges()?;
        let object = EditObject::from_canonical_json(EditObjectKind::LibraryRoot, 1, &self)?;
        EditObjectPack::new(object, edges)
    }

    fn edges(self) -> Result<Vec<EditObjectEdge>, EditRepositoryError> {
        let mut edges = Vec::new();
        for (role, target) in [
            ("photo_recipes", self.photo_recipes),
            ("shared_grade_heads", self.shared_grade_heads),
            ("masks", self.masks),
            ("styles", self.styles),
            ("output_states", self.output_states),
        ] {
            if let Some(target) = target {
                edges.push(EditObjectEdge::new(role, 0, target)?);
            }
        }
        Ok(edges)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct EditEntityEntryV1 {
    pub key: String,
    pub value: EditObjectId,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct EditEntityMapV1 {
    entries: Vec<EditEntityEntryV1>,
}

impl EditEntityMapV1 {
    /// Decodes and validates a sorted v1 entity map from an exact map object.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for a kind/version mismatch, malformed
    /// JSON, invalid keys, or non-canonical entry order.
    pub fn from_object(object: &EditObject) -> Result<Self, EditRepositoryError> {
        require_object_type(object, EditObjectKind::EntityMap, 1)?;
        let map: Self = object.decode()?;
        map.validate()?;
        Ok(map)
    }

    /// Creates a deterministic map, sorting entries by their stable key.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for invalid or duplicate keys.
    pub fn new(mut entries: Vec<EditEntityEntryV1>) -> Result<Self, EditRepositoryError> {
        entries.sort_by(|left, right| left.key.cmp(&right.key));
        let map = Self { entries };
        map.validate()?;
        Ok(map)
    }

    #[must_use]
    pub fn entries(&self) -> &[EditEntityEntryV1] {
        &self.entries
    }

    #[must_use]
    pub fn get(&self, key: &str) -> Option<EditObjectId> {
        self.entries
            .binary_search_by(|entry| entry.key.as_str().cmp(key))
            .ok()
            .map(|index| self.entries[index].value)
    }

    /// Returns a new map with one key inserted or atomically replaced.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the supplied key is invalid.
    pub fn with_entry(
        &self,
        key: impl Into<String>,
        value: EditObjectId,
    ) -> Result<Self, EditRepositoryError> {
        let key = key.into();
        let mut entries = self.entries.clone();
        match entries.binary_search_by(|entry| entry.key.cmp(&key)) {
            Ok(index) => entries[index].value = value,
            Err(index) => entries.insert(index, EditEntityEntryV1 { key, value }),
        }
        Self::new(entries)
    }

    #[must_use]
    pub fn without_entry(&self, key: &str) -> Self {
        let mut entries = self.entries.clone();
        if let Ok(index) = entries.binary_search_by(|entry| entry.key.as_str().cmp(key)) {
            entries.remove(index);
        }
        Self { entries }
    }

    #[must_use]
    pub fn diff(&self, after: &Self) -> Vec<EditEntityChangeV1> {
        let mut changes = Vec::new();
        let (mut before_index, mut after_index) = (0, 0);
        while before_index < self.entries.len() || after_index < after.entries.len() {
            match (
                self.entries.get(before_index),
                after.entries.get(after_index),
            ) {
                (Some(before), Some(next)) if before.key == next.key => {
                    if before.value != next.value {
                        changes.push(EditEntityChangeV1 {
                            key: before.key.clone(),
                            before: Some(before.value),
                            after: Some(next.value),
                        });
                    }
                    before_index += 1;
                    after_index += 1;
                }
                (Some(before), Some(next)) if before.key < next.key => {
                    changes.push(EditEntityChangeV1 {
                        key: before.key.clone(),
                        before: Some(before.value),
                        after: None,
                    });
                    before_index += 1;
                }
                (Some(_) | None, Some(next)) => {
                    changes.push(EditEntityChangeV1 {
                        key: next.key.clone(),
                        before: None,
                        after: Some(next.value),
                    });
                    after_index += 1;
                }
                (Some(before), None) => {
                    changes.push(EditEntityChangeV1 {
                        key: before.key.clone(),
                        before: Some(before.value),
                        after: None,
                    });
                    before_index += 1;
                }
                (None, None) => break,
            }
        }
        changes
    }

    /// Encodes this map and its ordered entry edges as one object pack.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the map violates the v1 key or
    /// ordering contract, or contains too many entries to index safely.
    pub fn into_object_pack(self) -> Result<EditObjectPack, EditRepositoryError> {
        self.validate()?;
        let edges = self.edges()?;
        let object = EditObject::from_canonical_json(EditObjectKind::EntityMap, 1, &self)?;
        EditObjectPack::new(object, edges)
    }

    fn validate(&self) -> Result<(), EditRepositoryError> {
        let mut previous: Option<&str> = None;
        for entry in &self.entries {
            if entry.key.is_empty()
                || entry.key.len() > MAX_ENTITY_KEY_BYTES
                || entry.key.chars().any(char::is_control)
            {
                return Err(EditRepositoryError::InvalidEntityKey(entry.key.clone()));
            }
            if previous.is_some_and(|value| value >= entry.key.as_str()) {
                return Err(EditRepositoryError::DuplicateOrUnsortedEntityKey(
                    entry.key.clone(),
                ));
            }
            previous = Some(&entry.key);
        }
        Ok(())
    }

    fn edges(&self) -> Result<Vec<EditObjectEdge>, EditRepositoryError> {
        self.entries
            .iter()
            .enumerate()
            .map(|(index, entry)| {
                let position =
                    u32::try_from(index).map_err(|_| EditRepositoryError::TooManyEntityEntries)?;
                EditObjectEdge::new("entry", position, entry.value)
            })
            .collect()
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditEntityChangeV1 {
    pub key: String,
    pub before: Option<EditObjectId>,
    pub after: Option<EditObjectId>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct EditRepositoryCommitPayloadV1 {
    pub root: EditObjectId,
    pub parents: Vec<EditCommitId>,
    pub message: Option<String>,
    pub created_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditRepositoryCommit {
    id: EditCommitId,
    payload: EditRepositoryCommitPayloadV1,
    canonical_json: Vec<u8>,
}

impl EditRepositoryCommit {
    /// Creates one immutable Library-wide commit from a validated payload.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for duplicate/excessive parents, an
    /// invalid message, or JSON serialization failure.
    pub fn new(payload: EditRepositoryCommitPayloadV1) -> Result<Self, EditRepositoryError> {
        validate_commit_payload(&payload)?;
        let canonical_json = serde_json::to_vec(&payload)?;
        let id = commit_id(1, &canonical_json);
        Ok(Self {
            id,
            payload,
            canonical_json,
        })
    }

    /// Reconstructs a commit only when its canonical bytes match its identifier.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for malformed/non-canonical JSON, an
    /// invalid payload, or a digest mismatch.
    pub fn from_stored_parts(
        id: EditCommitId,
        canonical_json: Vec<u8>,
    ) -> Result<Self, EditRepositoryError> {
        let payload: EditRepositoryCommitPayloadV1 = serde_json::from_slice(&canonical_json)?;
        validate_commit_payload(&payload)?;
        if serde_json::to_vec(&payload)? != canonical_json {
            return Err(EditRepositoryError::NonCanonicalCommitPayload);
        }
        if commit_id(1, &canonical_json) != id {
            return Err(EditRepositoryError::CommitDigestMismatch { id });
        }
        Ok(Self {
            id,
            payload,
            canonical_json,
        })
    }

    #[must_use]
    pub const fn id(&self) -> EditCommitId {
        self.id
    }

    #[must_use]
    pub const fn payload(&self) -> &EditRepositoryCommitPayloadV1 {
        &self.payload
    }

    #[must_use]
    pub fn canonical_json(&self) -> &[u8] {
        &self.canonical_json
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum EditRepositoryRefKind {
    Branch,
    NamedVersion,
    Tag,
}

impl EditRepositoryRefKind {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Branch => "branch",
            Self::NamedVersion => "named_version",
            Self::Tag => "tag",
        }
    }
}

impl FromStr for EditRepositoryRefKind {
    type Err = EditRepositoryError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "branch" => Ok(Self::Branch),
            "named_version" => Ok(Self::NamedVersion),
            "tag" => Ok(Self::Tag),
            _ => Err(EditRepositoryError::UnknownRefKind(value.to_owned())),
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum EditRepositoryRefExpectation {
    Missing,
    At(EditCommitId),
}

#[derive(Debug, thiserror::Error)]
pub enum EditRepositoryError {
    #[error("invalid 256-bit edit digest: {0:?}")]
    InvalidDigest(String),
    #[error("edit object format versions start at one")]
    InvalidFormatVersion,
    #[error("unknown edit object kind: {0}")]
    UnknownObjectKind(String),
    #[error("unknown edit repository ref kind: {0}")]
    UnknownRefKind(String),
    #[error("edit object {id} does not match its content digest")]
    ObjectDigestMismatch { id: EditObjectId },
    #[error("edit object payload is not canonical JSON")]
    NonCanonicalObjectPayload,
    #[error("unsupported {kind:?} edit object format version {format_version}")]
    UnsupportedObjectFormatVersion {
        kind: EditObjectKind,
        format_version: u32,
    },
    #[error(
        "edit object type mismatch: expected {expected_kind:?} v{expected_version}, got {actual_kind:?} v{actual_version}"
    )]
    ObjectTypeMismatch {
        expected_kind: EditObjectKind,
        expected_version: u32,
        actual_kind: EditObjectKind,
        actual_version: u32,
    },
    #[error("edit commit {id} does not match its content digest")]
    CommitDigestMismatch { id: EditCommitId },
    #[error("edit commit payload is not canonical JSON")]
    NonCanonicalCommitPayload,
    #[error("edit object contains duplicate edge role/position")]
    DuplicateObjectEdge,
    #[error("edit object edges do not match its typed payload")]
    ObjectEdgesDoNotMatchPayload,
    #[error("invalid legacy Recipe edit object: {0}")]
    InvalidLegacyRecipe(String),
    #[error("invalid shared Grade Node revision: {0}")]
    InvalidGradeNodeRevision(String),
    #[error("invalid edit object edge role: {0:?}")]
    InvalidEdgeRole(String),
    #[error("invalid edit entity key: {0:?}")]
    InvalidEntityKey(String),
    #[error("duplicate or unsorted edit entity key: {0:?}")]
    DuplicateOrUnsortedEntityKey(String),
    #[error("edit entity map is too large")]
    TooManyEntityEntries,
    #[error("edit repository commit has too many parents")]
    TooManyCommitParents,
    #[error("edit repository commit repeats one parent")]
    DuplicateCommitParent,
    #[error("edit repository commit message is invalid")]
    InvalidCommitMessage,
    #[error("edit repository JSON error: {0}")]
    Json(#[from] serde_json::Error),
}

fn validate_edge_role(role: &str) -> Result<(), EditRepositoryError> {
    if role.is_empty()
        || role.len() > MAX_EDGE_ROLE_BYTES
        || !role.bytes().all(|value| {
            value.is_ascii_lowercase() || value.is_ascii_digit() || b"_.-".contains(&value)
        })
    {
        return Err(EditRepositoryError::InvalidEdgeRole(role.to_owned()));
    }
    Ok(())
}

fn validate_supported_object_version(
    kind: EditObjectKind,
    format_version: u32,
) -> Result<(), EditRepositoryError> {
    if format_version != 1 {
        return Err(EditRepositoryError::UnsupportedObjectFormatVersion {
            kind,
            format_version,
        });
    }
    Ok(())
}

fn require_object_type(
    object: &EditObject,
    expected_kind: EditObjectKind,
    expected_version: u32,
) -> Result<(), EditRepositoryError> {
    if object.kind != expected_kind || object.format_version != expected_version {
        return Err(EditRepositoryError::ObjectTypeMismatch {
            expected_kind,
            expected_version,
            actual_kind: object.kind,
            actual_version: object.format_version,
        });
    }
    Ok(())
}

fn canonical_json<T: Serialize>(value: &T) -> Result<Vec<u8>, EditRepositoryError> {
    // Serializing through Value sorts object keys (serde_json's default map is
    // a BTreeMap). This makes semantically identical map inputs independent of
    // HashMap iteration order before they enter the content-addressed store.
    let value = serde_json::to_value(value)?;
    serde_json::to_vec(&value).map_err(Into::into)
}

fn validate_commit_payload(
    payload: &EditRepositoryCommitPayloadV1,
) -> Result<(), EditRepositoryError> {
    if payload.parents.len() > MAX_COMMIT_PARENTS {
        return Err(EditRepositoryError::TooManyCommitParents);
    }
    let unique: BTreeSet<_> = payload.parents.iter().copied().collect();
    if unique.len() != payload.parents.len() {
        return Err(EditRepositoryError::DuplicateCommitParent);
    }
    if payload.message.as_ref().is_some_and(|message| {
        message.is_empty()
            || message.len() > MAX_COMMIT_MESSAGE_BYTES
            || message.chars().any(|value| value == '\0')
    }) {
        return Err(EditRepositoryError::InvalidCommitMessage);
    }
    Ok(())
}

fn object_id(kind: EditObjectKind, format_version: u32, payload: &[u8]) -> EditObjectId {
    EditObjectId::from_bytes(domain_hash(
        OBJECT_HASH_DOMAIN,
        kind.as_str().as_bytes(),
        format_version,
        payload,
    ))
}

fn commit_id(format_version: u32, payload: &[u8]) -> EditCommitId {
    EditCommitId::from_bytes(domain_hash(
        COMMIT_HASH_DOMAIN,
        b"repository_commit",
        format_version,
        payload,
    ))
}

fn domain_hash(domain: &[u8], kind: &[u8], format_version: u32, payload: &[u8]) -> [u8; 32] {
    let mut hasher = blake3::Hasher::new();
    hasher.update(&hash_length(domain.len()));
    hasher.update(domain);
    hasher.update(&hash_length(kind.len()));
    hasher.update(kind);
    hasher.update(&format_version.to_be_bytes());
    hasher.update(&hash_length(payload.len()));
    hasher.update(payload);
    *hasher.finalize().as_bytes()
}

fn hash_length(length: usize) -> [u8; 8] {
    u64::try_from(length)
        .expect("supported targets cannot address more than u64::MAX bytes")
        .to_be_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn leaf(label: &str) -> EditObjectPack {
        let object = EditObject::from_canonical_json(
            EditObjectKind::PhotoEditState,
            1,
            &serde_json::json!({ "label": label }),
        )
        .expect("valid leaf");
        EditObjectPack::new(object, Vec::new()).expect("leaf has no edges")
    }

    #[test]
    fn object_ids_are_domain_separated_and_serde_as_hex() {
        let photo = EditObject::from_canonical_json(
            EditObjectKind::PhotoEditState,
            1,
            &serde_json::json!({ "value": 1 }),
        )
        .expect("photo object");
        let style = EditObject::from_canonical_json(
            EditObjectKind::StyleRevision,
            1,
            &serde_json::json!({ "value": 1 }),
        )
        .expect("style object");
        assert_ne!(photo.id(), style.id());
        let encoded = serde_json::to_string(&photo.id()).expect("serialize id");
        assert_eq!(encoded.len(), 66);
        assert_eq!(
            serde_json::from_str::<EditObjectId>(&encoded).unwrap(),
            photo.id()
        );
    }

    #[test]
    fn entity_map_is_order_independent_and_edges_match_payload() {
        let first = leaf("first");
        let second = leaf("second");
        let left = EditEntityMapV1::new(vec![
            EditEntityEntryV1 {
                key: "photo/b".into(),
                value: second.object().id(),
            },
            EditEntityEntryV1 {
                key: "photo/a".into(),
                value: first.object().id(),
            },
        ])
        .unwrap()
        .into_object_pack()
        .unwrap();
        let right = EditEntityMapV1::new(vec![
            EditEntityEntryV1 {
                key: "photo/a".into(),
                value: first.object().id(),
            },
            EditEntityEntryV1 {
                key: "photo/b".into(),
                value: second.object().id(),
            },
        ])
        .unwrap()
        .into_object_pack()
        .unwrap();
        assert_eq!(left, right);
        assert_eq!(left.edges().len(), 2);
    }

    #[test]
    fn library_root_and_commit_are_content_addressed() {
        let map = EditEntityMapV1::new(Vec::new())
            .unwrap()
            .into_object_pack()
            .unwrap();
        let root = LibraryRootV1 {
            photo_recipes: Some(map.object().id()),
            shared_grade_heads: None,
            masks: None,
            styles: None,
            output_states: None,
        }
        .into_object_pack()
        .unwrap();
        let commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
            root: root.object().id(),
            parents: Vec::new(),
            message: Some("Initial Library edit state".into()),
            created_at_ms: 1_721_500_000_000,
        })
        .unwrap();
        assert_eq!(
            EditRepositoryCommit::from_stored_parts(commit.id(), commit.canonical_json().to_vec())
                .unwrap(),
            commit
        );
    }

    #[test]
    fn typed_payload_rejects_forged_edges() {
        let map = EditEntityMapV1::new(Vec::new())
            .unwrap()
            .into_object_pack()
            .unwrap();
        let root = LibraryRootV1 {
            photo_recipes: Some(map.object().id()),
            shared_grade_heads: None,
            masks: None,
            styles: None,
            output_states: None,
        };
        let object =
            EditObject::from_canonical_json(EditObjectKind::LibraryRoot, 1, &root).unwrap();
        assert!(matches!(
            EditObjectPack::new(object, Vec::new()),
            Err(EditRepositoryError::ObjectEdgesDoNotMatchPayload)
        ));
    }

    #[test]
    fn canonical_object_ids_ignore_source_map_iteration_order() {
        let left = std::collections::HashMap::from([("b", 2), ("a", 1)]);
        let right = std::collections::HashMap::from([("a", 1), ("b", 2)]);
        let left = EditObject::from_canonical_json(EditObjectKind::OutputState, 1, &left).unwrap();
        let right =
            EditObject::from_canonical_json(EditObjectKind::OutputState, 1, &right).unwrap();
        assert_eq!(left, right);
    }

    #[test]
    fn stored_objects_reject_noncanonical_json_and_unknown_versions() {
        let bytes = br#"{"b":2,"a":1}"#.to_vec();
        let id = object_id(EditObjectKind::OutputState, 1, &bytes);
        assert!(matches!(
            EditObject::from_stored_parts(id, EditObjectKind::OutputState, 1, bytes),
            Err(EditRepositoryError::NonCanonicalObjectPayload)
        ));
        assert!(matches!(
            EditObject::from_canonical_json(EditObjectKind::OutputState, 2, &serde_json::json!({})),
            Err(EditRepositoryError::UnsupportedObjectFormatVersion {
                format_version: 2,
                ..
            })
        ));
    }

    #[test]
    fn entity_map_updates_and_diffs_are_sorted_and_semantic() {
        let first = leaf("first").object().id();
        let second = leaf("second").object().id();
        let third = leaf("third").object().id();
        let before = EditEntityMapV1::new(vec![
            EditEntityEntryV1 {
                key: "photo/a".into(),
                value: first,
            },
            EditEntityEntryV1 {
                key: "photo/b".into(),
                value: second,
            },
        ])
        .unwrap();
        let after = before
            .without_entry("photo/a")
            .with_entry("photo/b", third)
            .unwrap()
            .with_entry("photo/c", first)
            .unwrap();
        assert_eq!(after.get("photo/b"), Some(third));
        assert_eq!(
            before.diff(&after),
            vec![
                EditEntityChangeV1 {
                    key: "photo/a".into(),
                    before: Some(first),
                    after: None,
                },
                EditEntityChangeV1 {
                    key: "photo/b".into(),
                    before: Some(second),
                    after: Some(third),
                },
                EditEntityChangeV1 {
                    key: "photo/c".into(),
                    before: None,
                    after: Some(first),
                },
            ]
        );
    }
}
