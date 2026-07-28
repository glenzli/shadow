//! Domain-separated 256-bit identities for repository objects and commits.

use std::{fmt, str::FromStr};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de};

use super::error::EditRepositoryError;

const OBJECT_HASH_DOMAIN: &[u8] = b"shadow.edit.object.v1";
const COMMIT_HASH_DOMAIN: &[u8] = b"shadow.edit.commit.v1";

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

pub(super) fn object_id(kind: &str, format_version: u32, payload: &[u8]) -> EditObjectId {
    EditObjectId::from_bytes(domain_hash(
        OBJECT_HASH_DOMAIN,
        kind.as_bytes(),
        format_version,
        payload,
    ))
}

pub(super) fn commit_id(format_version: u32, payload: &[u8]) -> EditCommitId {
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
mod tests;
