#pragma once

// Borsh encoding for the LEZ Token program, written out by hand.
//
// The wallet FFI sends any program call as raw instruction bytes, so the
// module only needs the handful of shapes it actually uses. Each one mirrors
// `token-program/core/src/lib.rs` (vendored from LEZ v0.3.0) exactly — variant order
// is the wire format, so a reordered enum upstream breaks payments and must be
// re-checked here (tests/harness.cpp pins every byte).
//
//   Instruction:  0 Transfer · 1 NewFungibleDefinition · 2 NewDefinitionWithMetadata
//                 3 InitializeAccount · 4 Burn · 5 Mint · 6 PrintNft
//   TokenHolding: 0 Fungible { definition_id, balance } · 1 NftMaster · 2 NftPrintedCopy
//   TokenDefinition: 0 Fungible { name, total_supply, metadata_id: Option } · 1 NonFungible
//   TokenKind:    0 Fungible · 1 NftMaster · 2 NftPrintedCopy

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fieldstore {

using Bytes32 = std::array<uint8_t, 32>;
using u128    = unsigned __int128;

/// Field Store's own deployment of the Token program (../token-program) on
/// the LEZ v0.3.1 testnet: z4Bw6JARL5sKDLad19Bgxtb82sTqsviGkygR7vghtBp.
/// v0.3.1 dropped the built-in Token program (formerly at
/// sha256("/LEE-BuiltinProgram/v1/AccountId" || "token")), so after a testnet
/// reset redeploy with `tests/harness deploy-token src/token.bin` and update this.
/// Every token shard is keyed by this address.
constexpr Bytes32 kTokenProgramId = {
    0x0e, 0x9d, 0xb6, 0x1b, 0x90, 0x65, 0xcd, 0xd3, 0xb7, 0xea, 0x23, 0x3e, 0xab, 0x3e, 0x80, 0xb8,
    0x8e, 0x9d, 0x63, 0x9b, 0xda, 0x70, 0x64, 0xf0, 0xbb, 0xf6, 0x74, 0xae, 0x16, 0x5e, 0x5a, 0xef,
};

/// The native token lives in the shard of program [0; 32].
constexpr Bytes32 kNativeProgramId = {};

inline void putU128(std::vector<uint8_t>& out, u128 v)
{
    for (int i = 0; i < 16; ++i) { out.push_back(static_cast<uint8_t>(v & 0xff)); v >>= 8; }
}

inline void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>(v & 0xff)); v >>= 8; }
}

inline u128 getU128(const uint8_t* p)
{
    u128 v = 0;
    for (int i = 15; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

inline std::array<uint8_t, 16> u128Le(u128 v)
{
    std::array<uint8_t, 16> a{};
    for (int i = 0; i < 16; ++i) { a[i] = static_cast<uint8_t>(v & 0xff); v >>= 8; }
    return a;
}

/// Transfer { amount_to_transfer, descriptor: { definition_id, kind: Fungible } }
inline std::vector<uint8_t> encodeTransfer(u128 amount, const Bytes32& definitionId)
{
    std::vector<uint8_t> out;
    out.push_back(0);
    putU128(out, amount);
    out.insert(out.end(), definitionId.begin(), definitionId.end());
    out.push_back(0);
    return out;
}

/// NewFungibleDefinition { name, total_supply }
inline std::vector<uint8_t> encodeNewFungible(const std::string& name, u128 totalSupply)
{
    std::vector<uint8_t> out;
    out.push_back(1);
    putU32(out, static_cast<uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    putU128(out, totalSupply);
    return out;
}

/// Mint { amount_to_mint }
inline std::vector<uint8_t> encodeMint(u128 amount)
{
    std::vector<uint8_t> out;
    out.push_back(5);
    putU128(out, amount);
    return out;
}

struct Holding {
    Bytes32 definitionId{};
    u128    balance = 0;
};

/// A fungible holding, or nothing if the shard is empty or holds something else.
inline std::optional<Holding> decodeHolding(const uint8_t* p, size_t n)
{
    if (n < 1 + 32 + 16 || p[0] != 0) return std::nullopt;
    Holding h;
    for (int i = 0; i < 32; ++i) h.definitionId[i] = p[1 + i];
    h.balance = getU128(p + 33);
    return h;
}

struct Definition {
    std::string name;
    u128        totalSupply = 0;
};

inline std::optional<Definition> decodeDefinition(const uint8_t* p, size_t n)
{
    if (n < 1 + 4 || p[0] != 0) return std::nullopt;
    const uint32_t len = uint32_t(p[1]) | (uint32_t(p[2]) << 8) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 24);
    if (n < size_t(5) + len + 16) return std::nullopt;
    Definition d;
    d.name.assign(reinterpret_cast<const char*>(p + 5), len);
    d.totalSupply = getU128(p + 5 + len);
    return d;
}

/// Native balances are a bare little-endian u128, and an empty shard means 0.
inline u128 decodeNative(const uint8_t* p, size_t n)
{
    return n >= 16 ? getU128(p) : 0;
}

inline std::string u128ToString(u128 v)
{
    if (v == 0) return "0";
    std::string s;
    while (v > 0) { s.insert(s.begin(), char('0' + int(v % 10))); v /= 10; }
    return s;
}

/// LGO has 9 decimals on LEZ (10^9 atomic units per LGO); show at most 4.
inline std::string formatLgo(u128 atomic)
{
    const u128 unit = 1000000000;
    std::string whole = u128ToString(atomic / unit);
    std::string frac  = u128ToString(atomic % unit);
    frac.insert(frac.begin(), 9 - frac.size(), '0');
    frac.resize(4);
    while (frac.size() > 2 && frac.back() == '0') frac.pop_back();
    return whole + "." + frac;
}

} // namespace fieldstore
