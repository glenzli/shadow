//! Streaming bounds for persisted v1 AI contracts.
//!
//! The helpers reject oversized sequences while the deserializer is still
//! streaming them, before an untrusted length can become an allocation hint.

use std::collections::BTreeSet;
use std::fmt;
use std::marker::PhantomData;

use serde::de::{Error, IgnoredAny, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};

pub(crate) fn vec_5<'de, D, T>(deserializer: D) -> Result<Vec<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    bounded_vec::<D, T, 5>(deserializer)
}

pub(crate) fn vec_16<'de, D, T>(deserializer: D) -> Result<Vec<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    bounded_vec::<D, T, 16>(deserializer)
}

pub(crate) fn vec_64<'de, D, T>(deserializer: D) -> Result<Vec<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    bounded_vec::<D, T, 64>(deserializer)
}

pub(crate) fn vec_128<'de, D, T>(deserializer: D) -> Result<Vec<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    bounded_vec::<D, T, 128>(deserializer)
}

pub(crate) fn set_16<'de, D, T>(deserializer: D) -> Result<BTreeSet<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de> + Ord,
{
    bounded_set::<D, T, 16>(deserializer)
}

fn bounded_vec<'de, D, T, const LIMIT: usize>(deserializer: D) -> Result<Vec<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    deserializer.deserialize_seq(BoundedVecVisitor::<T, LIMIT>(PhantomData))
}

struct BoundedVecVisitor<T, const LIMIT: usize>(PhantomData<T>);

impl<'de, T, const LIMIT: usize> Visitor<'de> for BoundedVecVisitor<T, LIMIT>
where
    T: Deserialize<'de>,
{
    type Value = Vec<T>;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "a sequence containing at most {LIMIT} items")
    }

    fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
    where
        A: SeqAccess<'de>,
    {
        if sequence.size_hint().is_some_and(|hint| hint > LIMIT) {
            return Err(A::Error::custom(format_args!(
                "sequence exceeds the {LIMIT}-item v1 contract bound"
            )));
        }
        let mut values = Vec::with_capacity(sequence.size_hint().unwrap_or(0).min(LIMIT));
        while values.len() < LIMIT {
            match sequence.next_element()? {
                Some(value) => values.push(value),
                None => return Ok(values),
            }
        }
        if sequence.next_element::<IgnoredAny>()?.is_some() {
            return Err(A::Error::custom(format_args!(
                "sequence exceeds the {LIMIT}-item v1 contract bound"
            )));
        }
        Ok(values)
    }
}

fn bounded_set<'de, D, T, const LIMIT: usize>(deserializer: D) -> Result<BTreeSet<T>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de> + Ord,
{
    deserializer.deserialize_seq(BoundedSetVisitor::<T, LIMIT>(PhantomData))
}

struct BoundedSetVisitor<T, const LIMIT: usize>(PhantomData<T>);

impl<'de, T, const LIMIT: usize> Visitor<'de> for BoundedSetVisitor<T, LIMIT>
where
    T: Deserialize<'de> + Ord,
{
    type Value = BTreeSet<T>;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "a duplicate-free set containing at most {LIMIT} items"
        )
    }

    fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
    where
        A: SeqAccess<'de>,
    {
        if sequence.size_hint().is_some_and(|hint| hint > LIMIT) {
            return Err(A::Error::custom(format_args!(
                "set exceeds the {LIMIT}-item v1 contract bound"
            )));
        }
        let mut values = BTreeSet::new();
        while values.len() < LIMIT {
            let Some(value) = sequence.next_element()? else {
                return Ok(values);
            };
            if !values.insert(value) {
                return Err(A::Error::custom(
                    "duplicate set member is not canonical in a v1 contract",
                ));
            }
        }
        if sequence.next_element::<IgnoredAny>()?.is_some() {
            return Err(A::Error::custom(format_args!(
                "set exceeds the {LIMIT}-item v1 contract bound"
            )));
        }
        Ok(values)
    }
}
