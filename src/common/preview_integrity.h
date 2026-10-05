#pragma once
#include <cstdint>

namespace pulse::preview {
// Complete describes the declared scope (for example, a page), not fidelity
// to every capability of the originating application. Unknown is deliberate
// for opaque Shell providers. Zero totals mean unknown, never an empty file.
enum class IntegrityState : uint32_t { Unknown, Loading, Complete, Partial, Unsupported, Failed };
enum class IntegrityReason : uint32_t {
    None, Limit, ReadFailure, UnsupportedFeature, OnDemand, ReadingConversion,
    SourceFallback, ProviderDefined, IncompleteDirectory, Unavailable
};
enum class IntegrityUnit : uint32_t { None, Bytes, Entries, Sheets, Frames, Pages, Chapters };
struct Integrity {
    IntegrityState state = IntegrityState::Unknown;
    IntegrityReason reason = IntegrityReason::None;
    IntegrityUnit unit = IntegrityUnit::None;
    uint32_t reserved = 0;
    uint64_t loaded = 0;
    uint64_t total = 0;
};
}
