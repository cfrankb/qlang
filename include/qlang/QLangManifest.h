#pragma once

// QLM1 type-manifest codec (docs/SERIALIZATION.md §11;
// docs/plans/portable-module-plan.md P2).
//
// A manifest is the serializable mirror of the QL_OBJECT types a host
// exposes: interface *shape* only — names, portable QTypeTags, enum tables,
// clause presence — with every closure erased. It is a compile-time input
// for `qlc` (P4) and an embedded section of shipped packages (P5), so one
// serializer and one code path serve the dumper, the compiler input and the
// package writer: fingerprint equality between host and manifest is then
// equality by construction.
//
// File layout (little-endian, same writer/Reader discipline as
// QLangBytecodeIO.cpp; strict version like `.qlc`):
//
//   magic       4s    "QLM1"
//   version     u32   = kQlmetaVersion (readers accept 1 only)
//   typeCount   u32
//   types       typeCount x entry
//
// entry = fingerprint u64 + body, where fingerprint = FNV-1a 64 over the
// body bytes. The body is one canonical, self-delimiting byte form
// (append-only for a future v2):
//
//   name        String   u32 size + bytes
//   flags       u8       bit0 hasStr, bit1 hasCtor
//   memberCount u32
//   members     each: kind u8 (0 rw-prop, 1 ro-prop, 2 method)
//                    name String
//                    prop:   getTag u8, setTag u8
//                    method: retTag u8, argCount u8, argTags u8x
//   enumCount   u32
//   enums       each: typeName String, entryCount u32,
//                    entries: name String + value i64
//   signalCount u32
//   signals     each: name String, argCount u8, argTags u8x
//   ctorArgs    when flags bit1: argCount u8, argTags u8x
//               (an empty list = context-taking QL_CTOR: presence-only checks)
//
// Tags, never mangled names, are the portable vocabulary: `typeid` strings
// are compiler-private, `QTypeTag` (QLangMeta.h) is the shared one.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "qlang/QLangMeta.h"
#include "qlang/QLangTypes.h"

namespace qlang {

// QLM1 format version. Written by writeManifest, validated (strictly) by
// readManifest; docs/SERIALIZATION.md §11 references this constant.
constexpr uint32_t kQlmetaVersion = 1;

// FNV-1a 64 — the manifest fingerprint function (also used by the P5
// package handshake). Stable across builds and compilers: it hashes only
// the canonical body bytes.
inline uint64_t fnv1a64(const std::string &s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  return h;
}

// One serializable member: the closure-free mirror of `Member` (kind and
// tags already live on Member since P1; this drops the std::functions).
struct ManifestMember {
  uint8_t kind = 0;  // 0 rw-prop, 1 ro-prop, 2 method (same as Member::kind)
  std::string name;
  QTypeTag getTag = QTypeTag::Unknown;
  QTypeTag setTag = QTypeTag::Unknown;
  QTypeTag retTag = QTypeTag::Unknown;
  std::vector<QTypeTag> argTags;
};

// One signal (mirror of SignalMeta without the arg-check closures).
struct ManifestSignal {
  std::string name;
  std::vector<QTypeTag> argTags;
};

// The serializable mirror of `TypeMeta` (no closures, no type_info).
struct ManifestType {
  std::string name;
  bool hasStr = false;
  bool hasCtor = false;
  std::vector<QTypeTag> ctorArgTags;  // empty + hasCtor = context-taking form
  std::vector<ManifestMember> members;
  std::vector<EnumMeta> enums;        // closure-free already
  std::vector<ManifestSignal> signals;
};

// Convert real host meta into the manifest form (P1 fields flow straight
// through).
ManifestType manifestTypeFrom(const TypeMeta &m);
ManifestType manifestTypeFrom(const TypeDescriptor &d);

// Dump the process's meta registry (every QL_OBJECT type whose meta has
// been referenced, P3) — the host one-liner for binaries that are not
// qlang_run. Two registry entries sharing a type name refuse the dump with
// QLangError(Io) naming it: the registry keeps duplicates, the manifest
// refuses them (never silently dropped).
void writeTypeManifest(std::ostream &os);

// Canonical byte form of one entry: fingerprint u64 + body. The fingerprint
// is computed by this serializer alone — host and reader can never disagree
// by construction.
std::string serializeTypeEntry(const ManifestType &t);

// Parse one entry from `data` at `pos` (advances it). Throws
// QLangError(Io, "qlm: ... (byte offset N)") on truncation, an invalid
// kind/tag byte, or a fingerprint mismatch against the recomputed body hash.
ManifestType parseTypeEntry(const std::string &data, size_t &pos);

// Write the full manifest. Throws QLangError(Io) on stream failure.
void writeManifest(std::ostream &os, const std::vector<ManifestType> &types);

// Read a full manifest, treating the input as untrusted (docs/
// SERIALIZATION.md §8 caps, §11 load contract): every read bounds-checked,
// bad magic/version, out-of-range kind/tag, fingerprint mismatch, duplicate
// type name or trailing bytes -> QLangError(Io, "qlm: ...").
std::vector<ManifestType> readManifest(std::istream &is);

}  // namespace qlang
