// Field Store harness.
//
//   harness codec                 offline: pins every Token-program byte we emit
//   harness <cmd> [args…]         live: drives FieldStoreImpl against the testnet
//   harness deploy-token BIN      live: deploy the Token program from token.bin
//                                 (paid by the testnet faucet), print its address
//
// Live commands run one module instance in this process, in the data dir named
// by FIELDSTORE_DATA_DIR, wait for the queued job to finish, and print the
// resulting state. tests/run.sh plays an organizer and a resident against each
// other with two data dirs.
#include "fieldstore_impl.h"
#include "token_codec.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "wallet_ffi.h"
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using nlohmann::json;
using namespace fieldstore;

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!ok) ++g_fail;
}

static std::string hexOf(const std::vector<uint8_t>& v)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s.push_back(d[b >> 4]); s.push_back(d[b & 15]); }
    return s;
}

static int codec()
{
    printf("=== token codec ===\n");
    Bytes32 def{};
    for (int i = 0; i < 32; ++i) def[i] = uint8_t(i);

    // Transfer { 1000, { def, Fungible } } = 00 | e803 00…(16) | def | 00
    const auto t = encodeTransfer(1000, def);
    check(t.size() == 1 + 16 + 32 + 1, "Transfer is 50 bytes");
    check(hexOf(t) == "00e8030000000000000000000000000000"
                      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f00",
          "Transfer bytes match Borsh layout");

    // NewFungibleDefinition { "FIELD", 30000 } = 01 | 05000000 FIELD | 3075 00…
    const auto n = encodeNewFungible("FIELD", 30000);
    check(hexOf(n) == "01050000004649454c4430750000000000000000000000000000", "NewFungibleDefinition bytes");

    check(hexOf(encodeMint(7)) == "0507" + std::string(30, '0'), "Mint bytes");

    // TokenHolding::Fungible round-trip from what the program writes.
    std::vector<uint8_t> holding{0};
    holding.insert(holding.end(), def.begin(), def.end());
    putU128(holding, 1234);
    auto h = decodeHolding(holding.data(), holding.size());
    check(h && h->balance == 1234 && h->definitionId == def, "TokenHolding::Fungible decodes");
    std::vector<uint8_t> nft{1};
    nft.insert(nft.end(), 48, 0);
    check(!decodeHolding(nft.data(), nft.size()), "an NFT holding is not a fungible balance");

    std::vector<uint8_t> d{0, 5, 0, 0, 0, 'F', 'I', 'E', 'L', 'D'};
    putU128(d, 30000);
    d.push_back(0);   // metadata_id: None
    auto dd = decodeDefinition(d.data(), d.size());
    check(dd && dd->name == "FIELD" && dd->totalSupply == 30000, "TokenDefinition::Fungible decodes");

    check(formatLgo(1500000000) == "1.50", "1.5 LGO formats as 1.50");
    check(formatLgo(20000000000000ull) == "20000.00", "genesis faucet balance formats");
    check(formatLgo(999720104) == "0.9997", "gas shows at most 4 decimals");
    check(u128ToString(u128(1) << 100) == "1267650600228229401496703205376", "u128 to decimal");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

/**
 * LEZ v0.3.1 has no built-in Token program, so Field Store deploys its own.
 * Uses a throwaway wallet in $FIELDSTORE_DATA_DIR/deployer and the testnet's
 * public faucet key as the fee payer. Prints the program address as hex; it
 * goes in kTokenProgramId (src/token_codec.h).
 */
static int deployToken(const std::string& binPath)
{
    std::ifstream f(binPath, std::ios::binary);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // A risc0 ProgramBinary: "R0BF" | u32 version | u32 header_len | header |
    // u32 user_len | user elf | kernel. Only the user elf is uploaded, in
    // write-once 96 KiB segments, and the count has to match exactly.
    auto u32at = [&](size_t o) { return uint32_t(elf[o]) | uint32_t(elf[o + 1]) << 8 | uint32_t(elf[o + 2]) << 16 | uint32_t(elf[o + 3]) << 24; };
    if (elf.size() < 16 || std::string(elf.begin(), elf.begin() + 4) != "R0BF") { fprintf(stderr, "%s is not a risc0 program binary\n", binPath.c_str()); return 2; }
    const size_t userLen = u32at(12 + u32at(8));
    constexpr size_t kSegment = 96 * 1024;
    const size_t segments = (userLen + kSegment - 1) / kSegment;
    printf("token.bin: %zu bytes, user elf %zu bytes -> %zu segment(s)\n", elf.size(), userLen, segments);

    const char* base = std::getenv("FIELDSTORE_DATA_DIR");
    const std::string dir = std::string(base && *base ? base : ".") + "/deployer";
    std::filesystem::create_directories(dir);
    const std::string config = dir + "/config.json", storage = dir + "/storage.json";
    std::ofstream(config) << json{{"sequencer", {{"sequencer_addr", "https://testnet.lez.logos.co"}}},
                                  {"seq_poll_timeout", "30s"}, {"seq_tx_poll_max_blocks", 60},
                                  {"seq_poll_max_retries", 8}, {"seq_block_poll_max_amount", 100}}.dump(2);
    WalletHandle* w = nullptr;
    if (std::filesystem::exists(storage)) {
        w = wallet_ffi_open(config.c_str(), storage.c_str());
    } else {
        FfiCreateWalletOutput out = wallet_ffi_create_new(config.c_str(), storage.c_str(), "deployer");
        w = out.wallet;
    }
    if (!w) { fprintf(stderr, "couldn't open the deployer wallet in %s\n", dir.c_str()); return 2; }

    // PRIVATE_KEY_PUB_ACC_B from lez/testnet_initial_state: 7wHg9sbJ…
    const WalletFfiError ie = wallet_ffi_import_public_account(w, "717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b");
    if (ie != SUCCESS) fprintf(stderr, "faucet import returned %d (fine if already imported)\n", int(ie));
    FfiBytes32 payer{};
    {
        // Resolve the faucet's account id from its base58 address.
        static const char* A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
        std::vector<uint8_t> n(32, 0);
        for (char c : std::string("7wHg9sbJwc6h3NP1S9bekfAzB8CHifEcxKswCKUt3YQo")) {
            int carry = int(std::string(A).find(c));
            for (int i = 31; i >= 0; --i) { carry += 58 * n[i]; n[i] = uint8_t(carry & 0xff); carry >>= 8; }
        }
        std::copy(n.begin(), n.end(), payer.data);
    }

    FfiBytes32 header{};
    if (wallet_ffi_create_account_public(w, &header) != SUCCESS) { fprintf(stderr, "create_account_public failed\n"); return 2; }
    std::vector<FfiBytes32> segs(segments);
    for (auto& s : segs)
        if (wallet_ffi_create_account_public(w, &s) != SUCCESS) { fprintf(stderr, "create_account_public failed\n"); return 2; }
    wallet_ffi_save(w);

    printf("deploying (several blocks; a few minutes)...\n");
    fflush(stdout);
    FfiTransactionResult res{};
    const WalletFfiError e = wallet_ffi_program_loader_deploy(w, &header, segs.data(), segs.size(), elf.data(), elf.size(),
                                                              true, &payer, &res);
    const bool ok = e == SUCCESS && res.success;
    wallet_ffi_free_transaction_result(&res);
    wallet_ffi_save(w);
    if (!ok) { fprintf(stderr, "deploy failed (%d)\n", int(e)); return 1; }
    std::vector<uint8_t> id(header.data, header.data + 32);
    printf("token program address (hex): %s\n", hexOf(id).c_str());
    wallet_ffi_destroy(w);
    return 0;
}

static json stateOf(FieldStoreImpl& m)
{
    return json::parse(m.state());
}

static json waitReady(FieldStoreImpl& m)
{
    for (int i = 0; i < 240; ++i) {
        json s = stateOf(m);
        if (s["phase"] == "error") { fprintf(stderr, "wallet error: %s\n", s["error"].get<std::string>().c_str()); exit(2); }
        if (s["phase"] == "ready" && s["network"]["block"].get<int64_t>() > 0) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "wallet never became ready\n");
    exit(2);
}

/** Wait for a queued job to settle, printing status changes. */
static json waitJob(FieldStoreImpl& m, const std::string& reply)
{
    json r = json::parse(reply);
    if (!r.value("ok", false)) { fprintf(stderr, "refused: %s\n", r.value("error", "").c_str()); exit(3); }
    const int64_t id = r["job"];
    std::string last;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        json s = stateOf(m);
        for (const auto& j : s["jobs"]) {
            if (j["id"] != id) continue;
            const std::string st = j["status"];
            if (st != last) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                printf("  [%3llds] %-10s %s\n", (long long)secs, st.c_str(), j["label"].get<std::string>().c_str());
                fflush(stdout);
                last = st;
            }
            if (st == "done" || st == "failed") {
                if (st == "failed") printf("  error: %s\n", j["error"].get<std::string>().c_str());
                if (!j["tx"].get<std::string>().empty()) printf("  tx: %s\n", j["tx"].get<std::string>().c_str());
                // Let the worker's post-job refresh land before we print balances.
                std::this_thread::sleep_for(std::chrono::seconds(2));
                m.refresh();
                std::this_thread::sleep_for(std::chrono::seconds(4));
                return j;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

static void printState(FieldStoreImpl& m)
{
    json s = stateOf(m);
    json out = {
        {"block", s["network"]["block"]},
        {"address", s["me"]["address"]},
        {"publicTokens", s["me"]["publicTokens"]},
        {"privateTokens", s["me"]["privateTokens"]},
        {"gas", s["me"]["gas"]},
        {"station", s["station"]},
        {"till", s["counter"]["tillBalance"]},
        {"feed", s["counter"]["feed"]},
        {"treasury", s["organizer"]["treasuryBalance"]},
    };
    printf("%s\n", out.dump(2).c_str());
}

int main(int argc, char** argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "codec";
    if (cmd == "codec") return codec();
    if (cmd == "deploy-token" && argc > 2) return deployToken(argv[2]);

    FieldStoreImpl m;
    waitReady(m);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    json job;

    if (cmd == "state") {
        m.refresh();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    } else if (cmd == "create" && argc > 3) {
        job = waitJob(m, m.createStation(argv[2], argv[3]));
    } else if (cmd == "join" && argc > 2) {
        job = waitJob(m, m.joinStation(argv[2]));
    } else if (cmd == "issue" && argc > 3) {
        job = waitJob(m, m.issue(argv[2], argv[3], argc > 4 && std::string(argv[4]) == "gas"));
    } else if (cmd == "mint" && argc > 2) {
        job = waitJob(m, m.mint(argv[2]));
    } else if (cmd == "pay" && argc > 3) {
        job = waitJob(m, m.pay(argv[3], argv[2], "harness"));
    } else if (cmd == "shield" && argc > 2) {
        job = waitJob(m, m.moveToPrivate(argv[2]));
    } else if (cmd == "unshield" && argc > 2) {
        job = waitJob(m, m.moveToPublic(argv[2]));
    } else if (cmd == "send" && argc > 4) {
        job = waitJob(m, m.send(argv[2], argv[3], argv[4], "harness"));
    } else if (cmd == "link") {
        if (argc > 2) m.setName(argv[2]);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("%s\n", json::parse(m.summary())["link"].get<std::string>().c_str());
        fflush(stdout);
        return 0;
    } else if (cmd == "request" && argc > 4) {
        // Plays another module: ask, approve as the resident, then follow the request.
        json r = json::parse(m.requestPayment("Cycle Hire", argv[2], argv[3], "harness ride"));
        if (!r.value("ok", false)) { fprintf(stderr, "refused: %s\n", r.value("error", "").c_str()); return 3; }
        const int64_t id = r["request"];
        printf("  request %lld: %s\n", (long long)id, json::parse(m.requestStatus(id))["status"].get<std::string>().c_str());
        job = waitJob(m, m.approveRequest(id, argv[4]));
        printf("  request %lld: %s\n", (long long)id, m.requestStatus(id).c_str());
    } else if (cmd == "activity") {
        m.refresh();
        std::this_thread::sleep_for(std::chrono::seconds(6));
        const json s = stateOf(m);
        for (const auto& a : s["activity"])
            printf("  %-3s %6s %-10s %-8s %s\n", a["dir"].get<std::string>().c_str(), a["amount"].get<std::string>().c_str(),
                   a["who"].get<std::string>().c_str(), a["status"].get<std::string>().c_str(),
                   a["private"].get<bool>() ? "private" : "");
        printf("  (%zu entries)\n", stateOf(m)["activity"].size());
        fflush(stdout);
        return 0;
    } else if (cmd == "gas") {
        job = waitJob(m, m.getGas());
    } else {
        fprintf(stderr, "usage: harness codec | state | create NAME SUPPLY | join CODE | issue ADDR AMOUNT [gas]\n"
                        "               | mint AMOUNT | pay public|private AMOUNT | shield AMOUNT | unshield AMOUNT | gas\n");
        return 64;
    }
    printState(m);
    return job.is_object() && job["status"] == "failed" ? 1 : 0;
}
