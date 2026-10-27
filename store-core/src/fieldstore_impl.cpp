#include "fieldstore_impl.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "wallet_ffi.h"
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

using nlohmann::json;
using namespace fieldstore;
namespace fs = std::filesystem;

namespace {

// The testnet ships two genesis accounts whose keys are public on purpose —
// the official LEZ wallet UI uses them as a shared faucet. This one is
// `PRIVATE_KEY_PUB_ACC_B` in lez/testnet_initial_state (Public/7wHg9sbJ…).
// Only `getGas` and organizer setup touch it, and only for small amounts.
constexpr const char* kFaucetKeyHex =
    "717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b";
constexpr const char* kFaucetB58 = "7wHg9sbJwc6h3NP1S9bekfAzB8CHifEcxKswCKUt3YQo";

// The Token program's binary (src/token.bin), for proving private payments.
const uint8_t kTokenElf[] = {
#include "token_elf.inc"
};

constexpr u128 kLgo = 1000000000;                 // 10^9 atomic units
constexpr u128 kResidentGas  = kLgo;              // ~100 public payments' worth
constexpr u128 kOrganizerGas = 20 * kLgo;         // creating + issuing to 30 people
constexpr int64_t kTickMs = 6000;
constexpr int64_t kConfirmTimeoutMs = 4 * 60 * 1000;
constexpr const char* kCodePrefix = "fieldstore1";
constexpr const char* kLinkPrefix = "fieldpay:";

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

FfiBytes32 ffi(const Bytes32& b)
{
    FfiBytes32 f{};
    std::copy(b.begin(), b.end(), f.data);
    return f;
}

Bytes32 fromFfi(const FfiBytes32& f)
{
    Bytes32 b{};
    std::copy(std::begin(f.data), std::end(f.data), b.begin());
    return b;
}

/** Decimal string → u128; whole tokens only, so no decimals and no sign. */
bool parseAmount(const std::string& s, u128& out)
{
    std::string t;
    for (char c : s) if (c != ' ' && c != ',' && c != '_') t.push_back(c);
    if (t.empty() || t.size() > 30) return false;
    u128 v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + u128(c - '0');
    }
    out = v;
    return v > 0;
}

std::string errorText(int code)
{
    switch (code) {
    case NETWORK_ERROR:      return "Couldn't reach the LEZ sequencer.";
    case ACCOUNT_NOT_FOUND:  return "That account isn't in this wallet.";
    case KEY_NOT_FOUND:      return "This wallet doesn't hold the key for that account.";
    case INSUFFICIENT_FUNDS: return "Not enough balance.";
    case INVALID_ACCOUNT_ID: return "That address isn't a valid LEZ account id.";
    case PAYER_CANNOT_FUND:  return "Not enough gas to pay the network fee — tap Get gas.";
    case SYNC_ERROR:         return "Couldn't sync private state.";
    case STORAGE_ERROR:      return "Couldn't read or write the wallet file.";
    case CONFIG_ERROR:       return "The wallet config is invalid.";
    case INTERNAL_ERROR:     return "The wallet couldn't build that transaction (error 99) — the Basecamp log has the details.";
    default:                 return "Wallet error " + std::to_string(code) + ".";
    }
}

struct WalletError : std::runtime_error {
    explicit WalletError(int code, const std::string& what = "")
        : std::runtime_error(what.empty() ? errorText(code) : what) {}
};

void check(WalletFfiError e, const char* what)
{
    if (e != SUCCESS) {
        fprintf(stderr, "[fieldstore] %s failed: %d\n", what, int(e));
        throw WalletError(int(e));
    }
}

std::string jsonOk(const json& extra = json::object())
{
    json j = extra;
    j["ok"] = true;
    return j.dump();
}

std::string jsonErr(const std::string& msg)
{
    return json{{"ok", false}, {"error", msg}}.dump();
}

} // namespace


FieldStoreImpl::FieldStoreImpl() = default;

FieldStoreImpl::~FieldStoreImpl()
{
    m_running = false;
    m_qcv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_wallet) {
        wallet_ffi_save(m_wallet);
        wallet_ffi_destroy(m_wallet);
    }
}

void FieldStoreImpl::onContextReady()
{
    start();
}

void FieldStoreImpl::start()
{
    if (m_running.exchange(true)) return;
    m_worker = std::thread([this] { workerLoop(); });
}

std::string FieldStoreImpl::dataDir() const
{
    if (const char* env = std::getenv("FIELDSTORE_DATA_DIR"); env && *env) return env;
    if (isContextReady() && !instancePersistencePath().empty()) return instancePersistencePath();
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.fieldstore";
}

// ── persistence ─────────────────────────────────────────────────────────

void FieldStoreImpl::loadMeta()
{
    std::ifstream f(dataDir() + "/fieldstore.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return;

    std::lock_guard<std::mutex> lk(m_mu);
    m_password      = j.value("password", "");
    m_mnemonic      = j.value("mnemonic", "");
    m_recoveryAcked = j.value("recoveryAcked", false);
    if (j.contains("me")) {
        m_haveMe = unhex(j["me"].value("public", ""), m_mePublic.data(), 32)
                && unhex(j["me"].value("private", ""), m_mePrivate.data(), 32);
    }
    m_haveFaucet = j.value("faucetImported", false);
    if (j.contains("station")) {
        const auto& s = j["station"];
        m_joined = unhex(s.value("token", ""), m_token.data(), 32)
                && unhex(s.value("till", ""), m_till.data(), 32);
        m_stationName = s.value("name", "");
        m_haveHold = unhex(s.value("holdPublic", ""), m_holdPublic.data(), 32)
                  && unhex(s.value("holdPrivate", ""), m_holdPrivate.data(), 32);
    }
    if (j.contains("organizer")) {
        const auto& o = j["organizer"];
        m_organizer = unhex(o.value("treasury", ""), m_treasury.data(), 32);
        for (const auto& i : o.value("issued", json::array()))
            m_issued.push_back({i.value("to", ""), i.value("amount", ""), i.value("tx", ""), i.value("at", int64_t(0))});
    }
    m_myName = j.value("name", "");
    for (const auto& c : j.value("contacts", json::array()))
        m_contacts.push_back({c.value("name", ""), c.value("address", "")});
    for (const auto& a : j.value("activity", json::array())) {
        Activity x;
        x.id = m_nextActivity++;
        x.dir = a.value("dir", "");
        x.amount = a.value("amount", "");
        x.address = a.value("address", "");
        x.note = a.value("note", "");
        x.isPrivate = a.value("private", false);
        x.atMs = a.value("at", int64_t(0));
        x.status = a.value("status", "done");
        x.tx = a.value("tx", "");
        x.error = a.value("error", "");
        if (x.status != "done" && x.status != "failed") {
            x.status = "failed";
            x.error = "Interrupted when Basecamp closed — check your balance.";
        }
        m_activity.push_back(x);
    }
    for (const auto& r : j.value("requests", json::array())) {
        Request q;
        q.id = r.value("id", int64_t(0));
        q.app = r.value("app", "");
        q.to = r.value("to", "");
        q.amount = r.value("amount", "");
        q.memo = r.value("memo", "");
        q.status = r.value("status", "pending");
        q.atMs = r.value("at", int64_t(0));
        q.tx = r.value("tx", "");
        q.error = r.value("error", "");
        if (q.status == "paying") { q.status = "failed"; q.error = "Interrupted when Basecamp closed."; }
        m_nextRequest = std::max(m_nextRequest, q.id + 1);
        m_requests.push_back(q);
    }
    if (j.contains("pubBaseline")) {
        u128 v = 0;
        for (char c : j["pubBaseline"].get<std::string>()) v = v * 10 + u128(c - '0');
        m_pubBaseline = v;
        m_pubSeen = true;
    }
    for (const auto& e : j.value("feed", json::array()))
        m_feed.push_back({e.value("amount", ""), e.value("at", int64_t(0)), e.value("block", int64_t(0))});
    if (j.contains("tillSeen")) {
        u128 v = 0;
        for (char c : j["tillSeen"].get<std::string>()) v = v * 10 + u128(c - '0');
        m_tillBalance = v;
        m_tillSeen = true;
    }
}

void FieldStoreImpl::saveMeta()
{
    json j;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        j["version"]       = 1;
        j["password"]      = m_password;
        j["mnemonic"]      = m_mnemonic;
        j["recoveryAcked"] = m_recoveryAcked;
        if (m_haveMe)
            j["me"] = {{"public", hex(m_mePublic.data(), 32)}, {"private", hex(m_mePrivate.data(), 32)}};
        j["faucetImported"] = m_haveFaucet;
        if (m_joined)
        {
            j["station"] = {{"token", hex(m_token.data(), 32)}, {"till", hex(m_till.data(), 32)},
                            {"name", m_stationName}};
            if (m_haveHold) {
                j["station"]["holdPublic"] = hex(m_holdPublic.data(), 32);
                j["station"]["holdPrivate"] = hex(m_holdPrivate.data(), 32);
            }
        }
        if (m_organizer) {
            json issued = json::array();
            for (const auto& i : m_issued)
                issued.push_back({{"to", i.to}, {"amount", i.amount}, {"tx", i.tx}, {"at", i.atMs}});
            j["organizer"] = {{"treasury", hex(m_treasury.data(), 32)}, {"issued", issued}};
        }
        json feed = json::array();
        for (const auto& e : m_feed)
            feed.push_back({{"amount", e.amount}, {"at", e.atMs}, {"block", e.block}});
        j["feed"] = feed;
        j["name"] = m_myName;
        json contacts = json::array();
        for (const auto& c : m_contacts) contacts.push_back({{"name", c.name}, {"address", c.address}});
        j["contacts"] = contacts;
        json activity = json::array();
        for (const auto& a : m_activity)
            activity.push_back({{"dir", a.dir}, {"amount", a.amount}, {"address", a.address}, {"note", a.note},
                                {"private", a.isPrivate}, {"at", a.atMs}, {"status", a.status}, {"tx", a.tx},
                                {"error", a.error}});
        j["activity"] = activity;
        json requests = json::array();
        for (const auto& q : m_requests)
            requests.push_back({{"id", q.id}, {"app", q.app}, {"to", q.to}, {"amount", q.amount}, {"memo", q.memo},
                                {"status", q.status}, {"at", q.atMs}, {"tx", q.tx}, {"error", q.error}});
        j["requests"] = requests;
        if (m_pubSeen) j["pubBaseline"] = u128ToString(m_pubBaseline);
        if (m_tillSeen) j["tillSeen"] = u128ToString(m_tillBalance);
    }
    const std::string path = dataDir() + "/fieldstore.json";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, ec);
}

// ── worker ──────────────────────────────────────────────────────────────

void FieldStoreImpl::workerLoop()
{
    try {
        openWallet();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_phase = "error";
        m_error = e.what();
        return;
    }

    int64_t lastRefresh = 0;
    while (m_running) {
        int64_t jobId = 0;
        {
            std::unique_lock<std::mutex> lk(m_qmu);
            m_qcv.wait_for(lk, std::chrono::milliseconds(500),
                           [this] { return !m_running || !m_queue.empty() || m_kick; });
            if (!m_running) break;
            if (!m_queue.empty()) { jobId = m_queue.front(); m_queue.pop_front(); }
        }

        if (jobId) {
            std::function<void(Job&)> run;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                for (auto& j : m_jobs)
                    if (j.id == jobId) { j.status = "working"; run = j.run; }
            }
            Job scratch;
            scratch.id = jobId;
            try {
                if (run) run(scratch);
                setJob(jobId, [&](Job& j) {
                    if (j.status != "failed") j.status = "done";
                    j.finishedMs = nowMs();
                });
            } catch (const std::exception& e) {
                setJob(jobId, [&](Job& j) {
                    j.status = "failed";
                    j.error = e.what();
                    j.finishedMs = nowMs();
                });
            }
            {
                std::lock_guard<std::mutex> lk(m_mu);
                std::string st, tx, err;
                for (const auto& j : m_jobs)
                    if (j.id == jobId) { st = j.status; tx = j.tx; err = j.error; }
                for (auto& a : m_activity)
                    if (a.job == jobId) { a.status = st; a.tx = tx; a.error = err; }
                for (auto& q : m_requests)
                    if (q.job == jobId) { q.status = st == "done" ? "paid" : "failed"; q.tx = tx; q.error = err; }
            }
            saveWallet();
            saveMeta();
            lastRefresh = 0;                // show the result straight away
        }

        if (m_dirty.exchange(false)) saveMeta();

        if (m_kick.exchange(false) || nowMs() - lastRefresh >= kTickMs) {
            try {
                refreshBalances();
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = true;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = false;
                fprintf(stderr, "[fieldstore] refresh: %s\n", e.what());
            }
            lastRefresh = nowMs();
        }
    }
}

void FieldStoreImpl::openWallet()
{
    const std::string dir = dataDir();
    fs::create_directories(dir + "/wallet");
    loadMeta();

    const std::string config  = dir + "/wallet/config.json";
    const std::string storage = dir + "/wallet/storage.json";

    // Configs written before LEZ v0.3.1 list "sequencers"; v0.3.1 wants one
    // "sequencer" and refuses the old shape, so rewrite those.
    bool writeConfig = !fs::exists(config);
    if (!writeConfig) {
        std::ifstream f(config);
        const json old = json::parse(f, nullptr, false);
        writeConfig = old.is_discarded() || !old.contains("sequencer");
    }
    if (writeConfig) {
        // Same shape and defaults as the wallet's own Default, pointed at the
        // public testnet. A longer poll window than the CLI's 12 s because
        // testnet blocks arrive in bursts with quiet gaps of a minute or more.
        // The v0.3.1 testnet can take 10+ blocks to include a big transaction.
        json cfg = {
            {"sequencer", {{"sequencer_addr", "https://testnet.lez.logos.co"}}},
            {"seq_poll_timeout", "30s"},
            {"seq_tx_poll_max_blocks", 30},
            {"seq_poll_max_retries", 8},
            {"seq_block_poll_max_amount", 100},
        };
        if (const char* seq = std::getenv("FIELDSTORE_SEQUENCER"); seq && *seq)
            cfg["sequencer"]["sequencer_addr"] = seq;
        std::ofstream(config) << cfg.dump(2);
    }

    if (fs::exists(storage)) {
        m_wallet = wallet_ffi_open(config.c_str(), storage.c_str());
        if (!m_wallet) throw std::runtime_error("Couldn't open the wallet in " + dir);
    } else {
        std::string password;
        {
            std::random_device rd;
            uint8_t raw[24];
            for (auto& b : raw) b = static_cast<uint8_t>(rd() & 0xff);
            password = hex(raw, sizeof raw);
        }
        FfiCreateWalletOutput out = wallet_ffi_create_new(config.c_str(), storage.c_str(), password.c_str());
        if (!out.wallet) throw std::runtime_error("Couldn't create a wallet in " + dir);
        m_wallet = out.wallet;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_password = password;
            m_mnemonic = out.mnemonic ? out.mnemonic : "";
            m_haveMe = false;
            m_haveFaucet = false;
        }
        if (out.mnemonic) wallet_ffi_free_string(out.mnemonic);

        // A wallet created just now can't have been paid privately before it
        // existed, so start its private sync at the current block instead of
        // scanning the whole chain (60k+ blocks, ~20 minutes on the testnet).
        // The FFI has no setter, so write last_synced_block and reopen.
        uint64_t height = 0;
        if (wallet_ffi_get_current_block_height(m_wallet, &height) == SUCCESS && height > 0) {
            wallet_ffi_save(m_wallet);
            wallet_ffi_destroy(m_wallet);
            m_wallet = nullptr;
            {
                std::ifstream in(storage);
                json st = json::parse(in, nullptr, false);
                if (!st.is_discarded() && st.contains("last_synced_block")) {
                    st["last_synced_block"] = height;
                    in.close();
                    std::ofstream(storage) << st.dump();
                }
            }
            m_wallet = wallet_ffi_open(config.c_str(), storage.c_str());
            if (!m_wallet) throw std::runtime_error("Couldn't reopen the new wallet in " + dir);
        }
    }

    if (!m_haveMe) {
        const Bytes32 pub = newPublicAccount();
        const Bytes32 priv = newPrivateAccount();
        std::lock_guard<std::mutex> lk(m_mu);
        m_mePublic = pub;
        m_mePrivate = priv;
        m_haveMe = true;
    }
    if (char* addr = wallet_ffi_get_sequencer_addr(m_wallet)) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_sequencer = addr;
        wallet_ffi_free_string(addr);
    }

    const std::string pub58 = toBase58(m_mePublic);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_mePublicB58 = pub58;
    }
    if (m_joined) chooseHoldings();

    const std::string token58 = m_joined ? toBase58(m_token) : "";
    const std::string till58 = m_joined ? toBase58(m_till) : "";
    const std::string treasury58 = m_organizer ? toBase58(m_treasury) : "";
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_tokenB58 = token58;
        m_tillB58 = till58;
        m_treasuryB58 = treasury58;
        m_phase = "ready";
    }
    saveWallet();
    saveMeta();
}

void FieldStoreImpl::refreshBalances()
{
    uint64_t height = 0;
    check(wallet_ffi_get_current_block_height(m_wallet, &height), "get_current_block_height");

    syncPrivate();

    Bytes32 token{}, till{}, treasury{}, holdPub{}, holdPriv{};
    bool joined, organizer;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        token = m_token; till = m_till; treasury = m_treasury;
        holdPub = m_holdPublic; holdPriv = m_holdPrivate;
        joined = m_joined && m_haveHold; organizer = m_organizer;
    }

    const u128 gas = nativeBalance(m_mePublic);
    u128 pubTok = 0, privTok = 0, tillBal = 0, treasuryBal = 0, supply = 0;
    std::string name;
    if (joined) {
        pubTok  = tokenBalance(holdPub, false, token);
        privTok = tokenBalance(holdPriv, true, token);
        tillBal = tokenBalance(till, false, token);
        std::vector<uint8_t> raw;
        if (readPublicShard(token, kTokenProgramId, raw))
            if (auto d = decodeDefinition(raw.data(), raw.size())) { supply = d->totalSupply; name = d->name; }
        if (organizer) treasuryBal = tokenBalance(treasury, false, token);
    }

    bool feedChanged = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        // Published with the balances, so a block height means they're loaded.
        m_block = int64_t(height);
        m_myGas = gas;
        m_myPublicTokens = pubTok;
        m_myPrivateTokens = privTok;
        if (joined) {
            if (m_pubSeen && pubTok > m_pubBaseline) {
                Activity a;
                a.id = m_nextActivity++;
                a.dir = "in";
                a.amount = u128ToString(pubTok - m_pubBaseline);
                a.atMs = nowMs();
                a.status = "done";
                m_activity.insert(m_activity.begin(), a);
                if (m_activity.size() > 200) m_activity.resize(200);
                feedChanged = true;
            }
            if (!m_pubSeen || pubTok != m_pubBaseline) feedChanged = true;
            m_pubBaseline = pubTok;
            m_pubSeen = true;
        }
        m_treasuryBalance = treasuryBal;
        if (supply) m_supply = supply;
        if (!name.empty()) m_stationName = name;
        if (joined) {
            // The counter view: every rise in the till is a payment. The first
            // reading is a baseline, not a sale.
            if (m_tillSeen && tillBal > m_tillBalance) {
                m_feed.insert(m_feed.begin(), {u128ToString(tillBal - m_tillBalance), nowMs(), int64_t(height)});
                if (m_feed.size() > 300) m_feed.resize(300);
                feedChanged = true;
            }
            if (!m_tillSeen || tillBal != m_tillBalance) feedChanged = true;
            m_tillBalance = tillBal;
            m_tillSeen = true;
        }
    }
    if (feedChanged) saveMeta();
}

void FieldStoreImpl::syncPrivate()
{
    uint64_t height = 0, synced = 0;
    check(wallet_ffi_get_current_block_height(m_wallet, &height), "get_current_block_height");
    check(wallet_ffi_get_last_synced_block(m_wallet, &synced), "get_last_synced_block");
    if (height > synced) {
        check(wallet_ffi_sync_to_block(m_wallet, height), "sync_to_block");
        synced = height;
    }
    std::lock_guard<std::mutex> lk(m_mu);
    m_syncedBlock = int64_t(synced);
}

// ── job plumbing ────────────────────────────────────────────────────────

std::string FieldStoreImpl::enqueue(const std::string& kind, const std::string& label,
                                    const std::string& amount, std::function<void(Job&)> run)
{
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_phase != "ready") return jsonErr("The wallet is still starting.");
        Job j;
        j.id = id = m_nextJob++;
        j.kind = kind;
        j.label = label;
        j.amount = amount;
        j.status = "queued";
        j.createdMs = nowMs();
        j.run = std::move(run);
        m_jobs.insert(m_jobs.begin(), std::move(j));
        if (m_jobs.size() > 40) m_jobs.resize(40);
    }
    {
        std::lock_guard<std::mutex> lk(m_qmu);
        m_queue.push_back(id);
    }
    m_qcv.notify_all();
    return jsonOk({{"job", id}});
}

void FieldStoreImpl::setJob(int64_t id, const std::function<void(Job&)>& f)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& j : m_jobs)
        if (j.id == id) { f(j); return; }
}

void FieldStoreImpl::confirm(const std::string& txHash, int64_t jobId)
{
    setJob(jobId, [&](Job& j) { j.tx = txHash; j.status = "confirming"; });
    FfiBytes32 h{};
    if (!unhex(txHash, h.data, 32)) return;      // nothing to poll on; the submit itself succeeded
    const int64_t deadline = nowMs() + kConfirmTimeoutMs;
    while (m_running && nowMs() < deadline) {
        bool included = false;
        if (wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included) return;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    throw std::runtime_error("Submitted, but not seen in a block after 4 minutes. It may still land — check the balance.");
}

// ── wallet primitives ───────────────────────────────────────────────────

Bytes32 FieldStoreImpl::newPublicAccount()
{
    FfiBytes32 id{};
    check(wallet_ffi_create_account_public(m_wallet, &id), "create_account_public");
    return fromFfi(id);
}

Bytes32 FieldStoreImpl::newPrivateAccount()
{
    FfiBytes32 id{};
    check(wallet_ffi_create_account_private(m_wallet, &id), "create_account_private");
    return fromFfi(id);
}

std::string FieldStoreImpl::toBase58(const Bytes32& id)
{
    const FfiBytes32 f = ffi(id);
    char* s = wallet_ffi_account_id_to_base58(&f);
    if (!s) return hex(id.data(), 32);
    std::string out = s;
    wallet_ffi_free_string(s);
    return out;
}

bool FieldStoreImpl::fromBase58(const std::string& in, Bytes32& out)
{
    std::string s = in;
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\n' || c == '\t'; }), s.end());
    for (const char* p : {"Public/", "public/"})
        if (s.rfind(p, 0) == 0) s = s.substr(std::char_traits<char>::length(p));
    if (s.size() == 64 && unhex(s, out.data(), 32)) return true;
    FfiBytes32 f{};
    if (wallet_ffi_account_id_from_base58(s.c_str(), &f) != SUCCESS) return false;
    out = fromFfi(f);
    return true;
}

void FieldStoreImpl::saveWallet()
{
    if (m_wallet) wallet_ffi_save(m_wallet);
}

bool FieldStoreImpl::readPublicShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out)
{
    out.clear();
    const FfiBytes32 id = ffi(account);
    FfiAccount acc{};
    if (wallet_ffi_get_account_public(m_wallet, &id, &acc) != SUCCESS) return false;
    bool found = false;
    for (uintptr_t i = 0; i < acc.shards_len; ++i) {
        if (std::equal(program.begin(), program.end(), acc.shards[i].program.data)) {
            out.assign(acc.shards[i].data, acc.shards[i].data + acc.shards[i].data_len);
            found = true;
            break;
        }
    }
    wallet_ffi_free_account_data(&acc);
    return found;
}

bool FieldStoreImpl::readPrivateShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out)
{
    out.clear();
    const FfiBytes32 id = ffi(account);
    FfiAccount acc{};
    if (wallet_ffi_get_account_private(m_wallet, &id, &acc) != SUCCESS) return false;
    bool found = false;
    for (uintptr_t i = 0; i < acc.shards_len; ++i) {
        if (std::equal(program.begin(), program.end(), acc.shards[i].program.data)) {
            out.assign(acc.shards[i].data, acc.shards[i].data + acc.shards[i].data_len);
            found = true;
            break;
        }
    }
    wallet_ffi_free_account_data(&acc);
    return found;
}

u128 FieldStoreImpl::tokenBalance(const Bytes32& account, bool isPrivate, const Bytes32& definition)
{
    std::vector<uint8_t> raw;
    const bool ok = isPrivate ? readPrivateShard(account, kTokenProgramId, raw)
                              : readPublicShard(account, kTokenProgramId, raw);
    if (!ok) return 0;
    auto h = decodeHolding(raw.data(), raw.size());
    if (!h || h->definitionId != definition) return 0;   // someone else's token
    return h->balance;
}

u128 FieldStoreImpl::nativeBalance(const Bytes32& account)
{
    const FfiBytes32 id = ffi(account);
    uint8_t out[16] = {};
    if (wallet_ffi_get_balance(m_wallet, &id, true, &out) != SUCCESS) return 0;
    return getU128(out);
}

std::string FieldStoreImpl::sendPublicCall(const std::vector<std::pair<Bytes32, bool>>& accounts,
                                           const std::vector<uint8_t>& instruction,
                                           const Bytes32* payer)
{
    std::vector<FfiAccountMention> mentions;
    mentions.reserve(accounts.size());
    for (const auto& [id, sign] : accounts) {
        FfiAccountMention m{};
        check(wallet_ffi_resolve_public_account(ffi(id), sign, &m.identity), "resolve_public_account");
        m.program_account_id = ffi(kTokenProgramId);
        mentions.push_back(m);
    }
    FfiBytes32 payerF{};
    if (payer) payerF = ffi(*payer);

    FfiTransactionResult res{};
    const WalletFfiError e = wallet_ffi_send_generic_public_transaction(
        m_wallet, mentions.data(), mentions.size(), instruction.data(), instruction.size(),
        ffi(kTokenProgramId), payer ? &payerF : nullptr, &res);
    for (auto& m : mentions) wallet_ffi_free_account_identity(&m.identity);
    check(e, "send_generic_public_transaction");

    std::string tx = res.tx_hash ? res.tx_hash : "";
    const bool ok = res.success;
    wallet_ffi_free_transaction_result(&res);
    if (!ok) throw std::runtime_error("The sequencer rejected the transaction.");
    return tx;
}

std::string FieldStoreImpl::sendTokenPublic(const Bytes32& from, const Bytes32& to, u128 amount,
                                            const Bytes32& definition)
{
    // All LGO lives in the wallet's main public account; any other sender
    // (a treasury, a per-station holding account) has it co-sign as payer.
    const Bytes32* payer = nullptr;
    if (from != m_mePublic) payer = &m_mePublic;
    // The wallet refuses a fee payer that appears in the call without signing,
    // which is exactly the organizer issuing to their own address.
    const bool toSigns = payer && to == *payer;
    return sendPublicCall({{from, true}, {to, toSigns}}, encodeTransfer(amount, definition), payer);
}

std::string FieldStoreImpl::sendTokenPrivate(bool fromPrivate, const Bytes32& from,
                                             bool toPrivate, const Bytes32& to,
                                             u128 amount, const Bytes32& definition)
{
    // A privacy-preserving call: the wallet runs the Token program locally,
    // proves it, and submits only commitments for the private side. Public
    // accounts in the same call are visible as usual — paying the public till
    // from a private balance reveals the amount and the till, not the payer.
    auto mention = [&](bool isPrivate, const Bytes32& id, bool sign) {
        FfiAccountMention m{};
        if (isPrivate)
            check(wallet_ffi_resolve_private_account(m_wallet, ffi(id), &m.identity), "resolve_private_account");
        else
            check(wallet_ffi_resolve_public_account(ffi(id), sign, &m.identity), "resolve_public_account");
        m.program_account_id = ffi(kTokenProgramId);
        return m;
    };
    std::vector<FfiAccountMention> mentions;
    mentions.push_back(mention(fromPrivate, from, true));
    mentions.push_back(mention(toPrivate, to, false));

    // Proving needs the program binary itself: our own Token deployment,
    // embedded at build time (see CMakeLists.txt).
    FfiDependency dep{};
    dep.program.elf_data = kTokenElf;
    dep.program.elf_size = sizeof kTokenElf;
    dep.account_id = ffi(kTokenProgramId);
    dep.kind = PROGRAM_DISCLOSED;
    FfiProgramWithDependencies prog{};
    prog.self_account_id = ffi(kTokenProgramId);
    prog.programs = &dep;
    prog.programs_size = 1;

    const std::vector<uint8_t> instruction = encodeTransfer(amount, definition);
    FfiTransactionResult res{};
    const WalletFfiError e = wallet_ffi_send_generic_private_transaction(
        m_wallet, mentions.data(), mentions.size(), instruction.data(), instruction.size(), &prog, &res);
    for (auto& m : mentions) wallet_ffi_free_account_identity(&m.identity);
    check(e, "send_generic_private_transaction");

    std::string tx = res.tx_hash ? res.tx_hash : "";
    const bool ok = res.success;
    wallet_ffi_free_transaction_result(&res);
    if (!ok) throw std::runtime_error("The sequencer rejected the private transaction.");
    return tx;
}

std::string FieldStoreImpl::sendNative(const Bytes32& from, const Bytes32& to, u128 amount)
{
    const FfiBytes32 f = ffi(from), t = ffi(to);
    const auto le = u128Le(amount);
    uint8_t buf[16];
    std::copy(le.begin(), le.end(), buf);
    FfiTransferResult res{};
    check(wallet_ffi_transfer_public(m_wallet, &f, &t, &buf, &res), "transfer_public");
    std::string tx = res.tx_hash ? res.tx_hash : "";
    const bool ok = res.success;
    wallet_ffi_free_transfer_result(&res);
    if (!ok) throw std::runtime_error("The gas transfer was rejected.");
    return tx;
}

void FieldStoreImpl::ensureFaucet()
{
    Bytes32 id{};
    if (!fromBase58(kFaucetB58, id)) throw std::runtime_error("Bad faucet address.");
    if (!m_haveFaucet) {
        const WalletFfiError e = wallet_ffi_import_public_account(m_wallet, kFaucetKeyHex);
        // Importing twice is harmless; anything else is a real failure.
        if (e != SUCCESS) fprintf(stderr, "[fieldstore] faucet import returned %d\n", int(e));
        std::lock_guard<std::mutex> lk(m_mu);
        m_haveFaucet = true;
    }
    m_faucet = id;
}

void FieldStoreImpl::ensureGas(u128 atLeast, u128 topUp)
{
    if (nativeBalance(m_mePublic) >= atLeast) return;
    ensureFaucet();
    // Everyone at the station shares this faucet account, so two top-ups in
    // the same block collide on its nonce. Retry a couple of blocks later.
    std::string lastErr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        try {
            const std::string tx = sendNative(m_faucet, m_mePublic, topUp);
            FfiBytes32 h{};
            if (unhex(tx, h.data, 32)) {
                const int64_t deadline = nowMs() + kConfirmTimeoutMs;
                while (nowMs() < deadline) {
                    bool included = false;
                    if (wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included) break;
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            }
            if (nativeBalance(m_mePublic) >= atLeast) return;
        } catch (const std::exception& e) {
            lastErr = e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(15 + 10 * attempt));
    }
    throw std::runtime_error("Couldn't get gas from the testnet faucet" + (lastErr.empty() ? "." : ": " + lastErr));
}

// ── holdings and verification ───────────────────────────────────────

/** The name of a different token this account's token shard is bound to, or "". */
std::string FieldStoreImpl::otherTokenOn(const Bytes32& account, const Bytes32& definition, bool isPrivate)
{
    std::vector<uint8_t> raw;
    const bool ok = isPrivate ? readPrivateShard(account, kTokenProgramId, raw)
                              : readPublicShard(account, kTokenProgramId, raw);
    if (!ok || raw.empty()) return "";
    auto h = decodeHolding(raw.data(), raw.size());
    if (h && h->definitionId == definition) return "";
    std::string name = "other";
    if (h) {
        std::vector<uint8_t> def;
        if (readPublicShard(h->definitionId, kTokenProgramId, def))
            if (auto d = decodeDefinition(def.data(), def.size())) name = d->name;
    }
    return name;
}

/**
 * The Token program binds an account's token shard to the first token it ever
 * receives, for good — burning doesn't release it. So each station gets its
 * own public and private holding accounts: the wallet's first pair if they're
 * free (or already this token's), otherwise fresh ones. LGO for fees stays in
 * the main public account, which co-signs as payer.
 */
void FieldStoreImpl::chooseHoldings()
{
    Bytes32 token{}, pub{}, priv{};
    bool have;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        token = m_token;
        have = m_haveHold;
        pub = have ? m_holdPublic : m_mePublic;
        priv = have ? m_holdPrivate : m_mePrivate;
    }
    syncPrivate();
    bool changed = !have;
    if (!otherTokenOn(pub, token, false).empty()) { pub = newPublicAccount(); changed = true; }
    if (!otherTokenOn(priv, token, true).empty()) { priv = newPrivateAccount(); changed = true; }
    const std::string pub58 = toBase58(pub);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_holdPublic = pub;
        m_holdPrivate = priv;
        m_holdPublicB58 = pub58;
        m_haveHold = true;
        if (changed) m_pubSeen = false;   // a new account's balance is a baseline, not income
    }
    if (changed) { saveWallet(); m_dirty = true; }
}

/**
 * A transaction can be included in a block — and pay its fee — while the
 * program inside it panics and changes nothing. Inclusion isn't success; the
 * sender's balance moving is.
 */
void FieldStoreImpl::expectMoved(const Bytes32& account, bool isPrivate, const Bytes32& definition,
                                 u128 before, const std::string& what)
{
    for (int i = 0; i < 6; ++i) {
        if (isPrivate) syncPrivate();
        if (tokenBalance(account, isPrivate, definition) != before) return;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    throw std::runtime_error(what + " landed in a block but the Token program rejected it, so no tokens moved "
                             "(the network fee was still charged).");
}

// ── public API ──────────────────────────────────────────────────────────

std::string FieldStoreImpl::state()
{
    start();     // outside Basecamp (tests) there is no onContextReady
    std::lock_guard<std::mutex> lk(m_mu);
    json j;
    j["phase"] = m_phase;
    j["error"] = m_error;
    j["network"] = {{"sequencer", m_sequencer}, {"block", m_block}, {"synced", m_syncedBlock}, {"online", m_online}};
    j["me"] = {
        {"address", m_haveHold ? m_holdPublicB58 : std::string()},
        {"gasAddress", m_mePublicB58},
        {"publicTokens", u128ToString(m_myPublicTokens)},
        {"privateTokens", u128ToString(m_myPrivateTokens)},
        {"gas", formatLgo(m_myGas)},
        {"hasGas", m_myGas >= kLgo / 100},
    };
    j["me"]["name"] = m_myName;
    j["me"]["total"] = u128ToString(m_myPublicTokens + m_myPrivateTokens);
    j["me"]["link"] = m_haveHold ? std::string(kLinkPrefix) + m_holdPublicB58 + (m_myName.empty() ? "" : ":" + m_myName) : "";
    j["recovery"] = {{"acknowledged", m_recoveryAcked}, {"available", !m_mnemonic.empty()}};

    json contacts = json::array();
    if (m_joined) contacts.push_back({{"name", "Shop"}, {"address", m_tillB58}, {"shop", true}});
    for (const auto& c : m_contacts)
        if (c.address != m_tillB58) contacts.push_back({{"name", c.name}, {"address", c.address}, {"shop", false}});
    j["contacts"] = contacts;

    auto liveJob = [&](int64_t id) -> const Job* {
        for (const auto& x : m_jobs) if (id && x.id == id) return &x;
        return nullptr;
    };
    json activity = json::array();
    for (const auto& a : m_activity) {
        std::string status = a.status, tx = a.tx, error = a.error;
        if (const Job* jb = liveJob(a.job)) {
            status = jb->status;
            if (tx.empty()) tx = jb->tx;
            if (error.empty()) error = jb->error;
        }
        const std::string who = nameFor(a.address);
        activity.push_back({{"id", a.id}, {"dir", a.dir}, {"amount", a.amount}, {"address", a.address},
                            {"who", who}, {"note", a.note}, {"private", a.isPrivate}, {"at", a.atMs},
                            {"status", status}, {"tx", tx}, {"error", error}, {"job", a.job}});
    }
    j["activity"] = activity;

    json requests = json::array();
    for (const auto& q : m_requests) {
        std::string status = q.status, error = q.error, tx = q.tx;
        if (const Job* jb = liveJob(q.job)) {
            if (jb->status == "failed") { status = "failed"; error = jb->error; }
            if (tx.empty()) tx = jb->tx;
        }
        requests.push_back({{"id", q.id}, {"app", q.app}, {"to", q.to}, {"who", nameFor(q.to)},
                            {"amount", q.amount}, {"memo", q.memo}, {"status", status}, {"at", q.atMs},
                            {"tx", tx}, {"error", error}});
    }
    j["requests"] = requests;

    std::string code;
    if (m_joined) code = std::string(kCodePrefix) + ":" + m_tokenB58 + ":" + m_tillB58 + ":" + m_stationName;
    j["station"] = {
        {"joined", m_joined},
        {"name", m_stationName},
        {"token", m_tokenB58},
        {"till", m_tillB58},
        {"supply", u128ToString(m_supply)},
        {"code", code},
    };
    json feed = json::array();
    for (const auto& e : m_feed) feed.push_back({{"amount", e.amount}, {"at", e.atMs}, {"block", e.block}});
    j["counter"] = {{"tillBalance", u128ToString(m_tillBalance)}, {"feed", feed}};

    json issued = json::array();
    for (const auto& i : m_issued)
        issued.push_back({{"to", i.to}, {"who", nameFor(i.to)}, {"amount", i.amount}, {"tx", i.tx}, {"at", i.atMs}});
    j["organizer"] = {
        {"active", m_organizer},
        {"treasury", m_treasuryB58},
        {"treasuryBalance", u128ToString(m_treasuryBalance)},
        {"issued", issued},
    };

    json jobs = json::array();
    bool busy = false;
    for (const auto& x : m_jobs) {
        jobs.push_back({{"id", x.id}, {"kind", x.kind}, {"label", x.label}, {"status", x.status},
                        {"tx", x.tx}, {"error", x.error}, {"amount", x.amount},
                        {"created", x.createdMs}, {"finished", x.finishedMs}});
        if (x.status != "done" && x.status != "failed") busy = true;
    }
    j["jobs"] = jobs;
    j["busy"] = busy;
    return j.dump();
}

std::string FieldStoreImpl::joinStation(const std::string& codeIn)
{
    start();
    std::string code = codeIn;
    code.erase(0, code.find_first_not_of(" \n\t"));
    code.erase(code.find_last_not_of(" \n\t") + 1);
    const std::string prefix = std::string(kCodePrefix) + ":";
    if (code.rfind(prefix, 0) != 0) return jsonErr("That isn't a Field Store station code.");
    code = code.substr(prefix.size());
    const size_t a = code.find(':');
    const size_t b = a == std::string::npos ? a : code.find(':', a + 1);
    if (a == std::string::npos) return jsonErr("The station code is incomplete.");
    const std::string token58 = code.substr(0, a);
    const std::string till58 = code.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    const std::string name = b == std::string::npos ? "" : code.substr(b + 1);

    // Base58 decoding goes through the wallet, which lives on the worker.
    return enqueue("join", "Join " + (name.empty() ? std::string("station") : name), "",
                   [this, token58, till58, name](Job&) {
        Bytes32 token{}, till{};
        if (!fromBase58(token58, token) || !fromBase58(till58, till))
            throw std::runtime_error("The station code has a malformed address.");
        {
            std::lock_guard<std::mutex> lk(m_mu);
            const bool same = m_joined && m_token == token && m_till == till;
            m_token = token; m_till = till;
            m_tokenB58 = token58; m_tillB58 = till58;
            if (!name.empty()) m_stationName = name;
            m_joined = true;
            if (!same) { m_tillSeen = false; m_feed.clear(); m_haveHold = false; }
        }
        chooseHoldings();
        m_kick = true;
    });
}

std::string FieldStoreImpl::leaveStation()
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_organizer) return jsonErr("This wallet runs the station; it can't leave it.");
    m_joined = false;
    m_tokenB58.clear(); m_tillB58.clear(); m_stationName.clear();
    m_feed.clear(); m_tillSeen = false;
    m_supply = m_tillBalance = 0;
    m_myPublicTokens = m_myPrivateTokens = 0;
    m_dirty = true;   // saveMeta takes m_mu itself; the worker writes it
    m_kick = true;
    return jsonOk();
}

bool FieldStoreImpl::parseLink(const std::string& in, std::string& address, std::string& name)
{
    std::string t = in;
    t.erase(0, t.find_first_not_of(" \n\t"));
    t.erase(t.find_last_not_of(" \n\t") + 1);
    name.clear();
    if (t.rfind(kLinkPrefix, 0) == 0) {
        t = t.substr(std::char_traits<char>::length(kLinkPrefix));
        const size_t colon = t.find(':');
        if (colon != std::string::npos) { name = t.substr(colon + 1); t = t.substr(0, colon); }
    }
    for (const char* p : {"Public/", "public/"})
        if (t.rfind(p, 0) == 0) t = t.substr(std::char_traits<char>::length(p));
    address = t;
    return !address.empty() && address.find(' ') == std::string::npos && address.rfind("Private/", 0) != 0;
}

std::string FieldStoreImpl::nameFor(const std::string& address) const
{
    if (address.empty()) return "";
    if (address == m_tillB58) return "Shop";
    if (address == m_holdPublicB58) return m_myName.empty() ? "You" : m_myName;
    for (const auto& c : m_contacts) if (c.address == address) return c.name;
    return "";
}

std::string FieldStoreImpl::send(const std::string& toIn, const std::string& amountStr, const std::string& mode,
                                 const std::string& note)
{
    u128 amount = 0;
    if (!parseAmount(amountStr, amount)) return jsonErr("Enter a whole number of tokens.");
    if (mode != "public" && mode != "private") return jsonErr("Mode must be public or private.");
    std::string address, linkName;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_joined || !m_haveHold) return jsonErr("Join a station first.");
        if (toIn == "shop") address = m_tillB58;
    }
    if (address.empty() && !parseLink(toIn, address, linkName))
        return jsonErr("Paste an address or a fieldpay: link.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (address == m_holdPublicB58) return jsonErr("That's your own address.");
        const u128 have = mode == "private" ? m_myPublicTokens + m_myPrivateTokens : m_myPublicTokens;
        if (amount > have) {
            if (mode == "public" && amount <= m_myPublicTokens + m_myPrivateTokens)
                return jsonErr("Part of your balance is private — send this one privately, or move it back to public first.");
            return jsonErr("You don't have that many " + (m_stationName.empty() ? std::string("tokens") : m_stationName) + ".");
        }
        if (!linkName.empty()) {
            bool known = false;
            for (auto& c : m_contacts) if (c.address == address) { c.name = linkName; known = true; }
            if (!known) m_contacts.push_back({linkName, address});
            m_dirty = true;
        }
    }
    return queueSend(address, amount, amountStr, mode == "private", note, 0);
}

std::string FieldStoreImpl::pay(const std::string& amountStr, const std::string& mode, const std::string& note)
{
    return send("shop", amountStr, mode, note);
}

std::string FieldStoreImpl::queueSend(const std::string& toB58, u128 amount, const std::string& amountStr,
                                      bool priv, const std::string& note, int64_t requestId)
{
    Bytes32 token{}, holdPub{}, holdPriv{};
    std::string who;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        token = m_token; holdPub = m_holdPublic; holdPriv = m_holdPrivate;
        who = nameFor(toB58);
    }
    if (who.empty()) who = toB58.size() > 10 ? toB58.substr(0, 4) + "…" + toB58.substr(toB58.size() - 4) : toB58;

    const std::string r = enqueue(priv ? "send-private" : "send-public", "Sent " + amountStr + " to " + who, amountStr,
                                  [this, toB58, amount, priv, token, holdPub, holdPriv](Job& job) {
        Bytes32 to{};
        if (!fromBase58(toB58, to)) throw std::runtime_error("That isn't a valid LEZ address.");
        const std::string other = otherTokenOn(to, token);
        if (!other.empty())
            throw std::runtime_error("That address holds " + other + " tokens and can't receive these — ask them "
                                     "for the address Field Pay shows them now.");
        if (!priv) {
            ensureGas(kLgo / 100, kResidentGas);
            const u128 before = tokenBalance(holdPub, false, token);
            const std::string tx = sendTokenPublic(holdPub, to, amount, token);
            confirm(tx, job.id);
            expectMoved(holdPub, false, token, before, "The payment");
            std::lock_guard<std::mutex> lk(m_mu);
            m_pubBaseline = m_pubBaseline > amount ? m_pubBaseline - amount : 0;
            return;
        }
        // Private: spend from the private balance; top it up from the public
        // one first if it's short (that step shows on chain, the payment doesn't).
        syncPrivate();
        const u128 have = tokenBalance(holdPriv, true, token);
        if (have < amount) {
            const u128 shortfall = amount - have;
            setJob(job.id, [](Job& j) { j.status = "preparing"; });
            const u128 pubBefore = tokenBalance(holdPub, false, token);
            const std::string tx = sendTokenPrivate(false, holdPub, true, holdPriv, shortfall, token);
            confirm(tx, job.id);
            expectMoved(holdPub, false, token, pubBefore, "Preparing private funds");
            {
                std::lock_guard<std::mutex> lk(m_mu);
                m_pubBaseline = m_pubBaseline > shortfall ? m_pubBaseline - shortfall : 0;
            }
            syncPrivate();
        }
        setJob(job.id, [](Job& j) { j.status = "proving"; });
        const u128 before = tokenBalance(holdPriv, true, token);
        const std::string tx = sendTokenPrivate(true, holdPriv, false, to, amount, token);
        confirm(tx, job.id);
        expectMoved(holdPriv, true, token, before, "The payment");
    });

    json reply = json::parse(r, nullptr, false);
    if (reply.is_discarded() || !reply.value("ok", false)) return r;
    const int64_t jobId = reply["job"];
    std::lock_guard<std::mutex> lk(m_mu);
    Activity a;
    a.id = m_nextActivity++;
    a.dir = "out";
    a.amount = amountStr;
    a.address = toB58;
    a.note = note;
    a.isPrivate = priv;
    a.atMs = nowMs();
    a.job = jobId;
    a.status = "queued";
    m_activity.insert(m_activity.begin(), a);
    if (m_activity.size() > 200) m_activity.resize(200);
    if (requestId) {
        for (auto& q : m_requests)
            if (q.id == requestId) { q.status = "paying"; q.job = jobId; }
    }
    m_dirty = true;
    return r;
}

std::string FieldStoreImpl::setName(const std::string& nameIn)
{
    std::string name = nameIn;
    name.erase(std::remove(name.begin(), name.end(), ':'), name.end());
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    if (name.size() > 40) return jsonErr("Keep your name under 40 characters.");
    std::lock_guard<std::mutex> lk(m_mu);
    m_myName = name;
    m_dirty = true;
    return jsonOk();
}

std::string FieldStoreImpl::saveContact(const std::string& nameIn, const std::string& addressIn)
{
    std::string address, linkName;
    if (!parseLink(addressIn, address, linkName)) return jsonErr("That isn't an address or a fieldpay: link.");
    std::string name = nameIn.empty() ? linkName : nameIn;
    name.erase(std::remove(name.begin(), name.end(), ':'), name.end());
    if (name.empty()) return jsonErr("Give the contact a name.");
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& c : m_contacts)
        if (c.address == address) { c.name = name; m_dirty = true; return jsonOk(); }
    m_contacts.push_back({name, address});
    m_dirty = true;
    return jsonOk();
}

std::string FieldStoreImpl::removeContact(const std::string& addressIn)
{
    std::string address, ignored;
    parseLink(addressIn, address, ignored);
    std::lock_guard<std::mutex> lk(m_mu);
    m_contacts.erase(std::remove_if(m_contacts.begin(), m_contacts.end(),
                                    [&](const Contact& c) { return c.address == address; }), m_contacts.end());
    m_dirty = true;
    return jsonOk();
}

std::string FieldStoreImpl::requestPayment(const std::string& app, const std::string& to, const std::string& amountStr,
                                           const std::string& memo)
{
    u128 amount = 0;
    if (!parseAmount(amountStr, amount)) return jsonErr("The amount must be a whole number of tokens.");
    std::string address, linkName;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (to == "shop") address = m_tillB58;
    }
    if (address.empty() && !parseLink(to, address, linkName)) return jsonErr("`to` must be an address, a fieldpay: link or \"shop\".");
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_joined) return jsonErr("This wallet hasn't joined a station yet.");
    if (!linkName.empty()) {
        bool known = false;
        for (const auto& c : m_contacts) if (c.address == address) known = true;
        if (!known) m_contacts.push_back({linkName, address});
    }
    Request q;
    q.id = m_nextRequest++;
    q.app = app.empty() ? "An app" : app;
    q.to = address;
    q.amount = amountStr;
    q.memo = memo;
    q.status = "pending";
    q.atMs = nowMs();
    m_requests.insert(m_requests.begin(), q);
    if (m_requests.size() > 100) m_requests.resize(100);
    m_dirty = true;
    return jsonOk({{"request", q.id}});
}

std::string FieldStoreImpl::requestStatus(int64_t id)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto& q : m_requests) {
        if (q.id != id) continue;
        std::string tx = q.tx, error = q.error, status = q.status;
        for (const auto& j : m_jobs)
            if (q.job && j.id == q.job) {
                if (tx.empty()) tx = j.tx;
                if (j.status == "failed") { status = "failed"; error = j.error; }
            }
        return jsonOk({{"id", q.id}, {"status", status}, {"amount", q.amount}, {"to", q.to},
                       {"memo", q.memo}, {"tx", tx}, {"error", error}});
    }
    return jsonErr("No such request.");
}

std::string FieldStoreImpl::approveRequest(int64_t id, const std::string& mode)
{
    std::string to, amountStr, memo, app;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        bool found = false;
        for (const auto& q : m_requests)
            if (q.id == id && q.status == "pending") { to = q.to; amountStr = q.amount; memo = q.memo; app = q.app; found = true; }
        if (!found) return jsonErr("That request isn't waiting any more.");
    }
    u128 amount = 0;
    parseAmount(amountStr, amount);
    const std::string r = send(to, amountStr, mode, app + (memo.empty() ? "" : " · " + memo));
    json reply = json::parse(r, nullptr, false);
    if (reply.is_discarded() || !reply.value("ok", false)) return r;
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& q : m_requests)
        if (q.id == id) { q.status = "paying"; q.job = reply["job"]; }
    m_dirty = true;
    return r;
}

std::string FieldStoreImpl::declineRequest(int64_t id)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& q : m_requests)
        if (q.id == id && q.status == "pending") { q.status = "declined"; m_dirty = true; return jsonOk(); }
    return jsonErr("That request isn't waiting any more.");
}

std::string FieldStoreImpl::summary()
{
    start();
    std::lock_guard<std::mutex> lk(m_mu);
    return jsonOk({{"joined", m_joined}, {"token", m_stationName}, {"address", m_haveHold ? m_holdPublicB58 : ""},
                   {"name", m_myName},
                   {"link", m_haveHold ? std::string(kLinkPrefix) + m_holdPublicB58 + (m_myName.empty() ? "" : ":" + m_myName) : ""},
                   {"balance", u128ToString(m_myPublicTokens + m_myPrivateTokens)}});
}

std::string FieldStoreImpl::moveToPrivate(const std::string& amountStr)
{
    u128 amount = 0;
    if (!parseAmount(amountStr, amount)) return jsonErr("Enter a whole number of tokens.");
    Bytes32 token{}, holdPub{}, holdPriv{};
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_joined || !m_haveHold) return jsonErr("Join a station first.");
        if (amount > m_myPublicTokens) return jsonErr("Your public balance is too low.");
        token = m_token;
        holdPub = m_holdPublic;
        holdPriv = m_holdPrivate;
    }
    return enqueue("shield", "Moved " + amountStr + " to private", amountStr, [this, amount, token, holdPub, holdPriv](Job& job) {
        const u128 before = tokenBalance(holdPub, false, token);
        setJob(job.id, [](Job& j) { j.status = "proving"; });
        const std::string tx = sendTokenPrivate(false, holdPub, true, holdPriv, amount, token);
        confirm(tx, job.id);
        expectMoved(holdPub, false, token, before, "The move");
        syncPrivate();
        std::lock_guard<std::mutex> lk(m_mu);
        m_pubBaseline = m_pubBaseline > amount ? m_pubBaseline - amount : 0;
    });
}

std::string FieldStoreImpl::moveToPublic(const std::string& amountStr)
{
    u128 amount = 0;
    if (!parseAmount(amountStr, amount)) return jsonErr("Enter a whole number of tokens.");
    Bytes32 token{}, holdPub{}, holdPriv{};
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_joined || !m_haveHold) return jsonErr("Join a station first.");
        if (amount > m_myPrivateTokens) return jsonErr("Your private balance is too low.");
        token = m_token;
        holdPub = m_holdPublic;
        holdPriv = m_holdPrivate;
    }
    return enqueue("unshield", "Moved " + amountStr + " to public", amountStr, [this, amount, token, holdPub, holdPriv](Job& job) {
        const u128 before = tokenBalance(holdPriv, true, token);
        setJob(job.id, [](Job& j) { j.status = "proving"; });
        const std::string tx = sendTokenPrivate(true, holdPriv, false, holdPub, amount, token);
        confirm(tx, job.id);
        expectMoved(holdPriv, true, token, before, "The move");
        std::lock_guard<std::mutex> lk(m_mu);
        m_pubBaseline += amount;
    });
}

std::string FieldStoreImpl::getGas()
{
    return enqueue("gas", "Got gas from the testnet faucet", "", [this](Job&) {
        ensureGas(kResidentGas / 2, kResidentGas);
    });
}

std::string FieldStoreImpl::recoveryPhrase()
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_mnemonic.empty()) return jsonErr("No recovery phrase stored for this wallet.");
    return jsonOk({{"words", m_mnemonic}});
}

std::string FieldStoreImpl::acknowledgeRecovery()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_recoveryAcked = true;
    }
    m_dirty = true;
    return jsonOk();
}

std::string FieldStoreImpl::createStation(const std::string& nameIn, const std::string& supplyStr)
{
    u128 supply = 0;
    if (!parseAmount(supplyStr, supply)) return jsonErr("Enter the total supply as a whole number.");
    std::string name = nameIn;
    name.erase(std::remove(name.begin(), name.end(), ':'), name.end());
    if (name.empty() || name.size() > 32) return jsonErr("Give the token a name of 1–32 characters.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_organizer) return jsonErr("This wallet already runs a station.");
    }
    return enqueue("create", "Created " + name + " · supply " + supplyStr, supplyStr,
                   [this, name, supply](Job& job) {
        ensureGas(kOrganizerGas / 2, kOrganizerGas);
        // The definition account's key is the mint authority: whoever holds
        // it can mint, nobody else can. It never holds a balance.
        const Bytes32 definition = newPublicAccount();
        const Bytes32 treasury = newPublicAccount();
        const Bytes32 till = newPublicAccount();
        saveWallet();      // the keys must outlive a crash mid-submit
        const std::string tx = sendPublicCall({{definition, true}, {treasury, true}},
                                              encodeNewFungible(name, supply), &m_mePublic);
        confirm(tx, job.id);
        if (tokenBalance(treasury, false, definition) != supply)
            throw std::runtime_error("The token creation landed in a block but the Token program rejected it.");

        const std::string def58 = toBase58(definition), tre58 = toBase58(treasury), till58 = toBase58(till);
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_organizer = true;
            m_treasury = treasury; m_treasuryB58 = tre58;
            m_token = definition; m_tokenB58 = def58;
            m_till = till; m_tillB58 = till58;
            m_stationName = name;
            m_supply = supply;
            m_joined = true;
            m_tillSeen = false;
            m_feed.clear();
            m_haveHold = false;
        }
        chooseHoldings();
        m_kick = true;
    });
}

std::string FieldStoreImpl::issue(const std::string& residentAddress, const std::string& amountStr, bool withGas)
{
    u128 amount = 0;
    if (!parseAmount(amountStr, amount)) return jsonErr("Enter a whole number of tokens.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_organizer) return jsonErr("Only the station's organizer can issue tokens.");
        if (amount > m_treasuryBalance) return jsonErr("The treasury doesn't hold that many — mint more first.");
    }
    std::string addr, linkName;
    if (!parseLink(residentAddress, addr, linkName))
        return jsonErr("Paste the resident's fieldpay: link or address.");
    if (!linkName.empty()) {
        std::lock_guard<std::mutex> lk(m_mu);
        bool known = false;
        for (const auto& c : m_contacts) if (c.address == addr) known = true;
        if (!known) { m_contacts.push_back({linkName, addr}); m_dirty = true; }
    }
    const std::string shortAddr = addr.size() > 10 ? addr.substr(0, 6) + "…" + addr.substr(addr.size() - 4) : addr;

    return enqueue("issue", "Issued " + amountStr + " to " + shortAddr, amountStr,
                   [this, addr, amount, amountStr, withGas](Job& job) {
        Bytes32 to{};
        if (!fromBase58(addr, to)) throw std::runtime_error("That isn't a valid LEZ address.");
        const std::string other = otherTokenOn(to, m_token);
        if (!other.empty())
            throw std::runtime_error("That address already holds " + other + " tokens, and an LEZ account can only ever "
                                     "hold one kind of token. Ask the resident to open Field Store after joining this "
                                     "station and copy the address it shows now.");
        ensureGas(kLgo, kOrganizerGas);
        const u128 before = tokenBalance(m_treasury, false, m_token);
        const std::string tx = sendTokenPublic(m_treasury, to, amount, m_token);
        confirm(tx, job.id);
        expectMoved(m_treasury, false, m_token, before, "The issue");
        if (withGas && to != m_mePublic && nativeBalance(to) < kResidentGas / 2) {
            try {
                confirm(sendNative(m_mePublic, to, kResidentGas), job.id);
            } catch (const std::exception& e) {
                fprintf(stderr, "[fieldstore] gas for %s failed: %s\n", addr.c_str(), e.what());
            }
            setJob(job.id, [&](Job& j) { j.tx = tx; });
        }
        std::lock_guard<std::mutex> lk(m_mu);
        m_issued.insert(m_issued.begin(), {addr, amountStr, tx, nowMs()});
    });
}

std::string FieldStoreImpl::mint(const std::string& amountStr)
{
    u128 amount = 0;
    if (!parseAmount(amountStr, amount)) return jsonErr("Enter a whole number of tokens.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_organizer) return jsonErr("Only the station's organizer can mint.");
    }
    return enqueue("mint", "Minted " + amountStr + " into the treasury", amountStr, [this, amount](Job& job) {
        ensureGas(kLgo, kOrganizerGas);
        // Mint needs the definition account's signature; the treasury just receives.
        const u128 before = tokenBalance(m_treasury, false, m_token);
        const std::string tx = sendPublicCall({{m_token, true}, {m_treasury, false}}, encodeMint(amount), &m_mePublic);
        confirm(tx, job.id);
        expectMoved(m_treasury, false, m_token, before, "The mint");
    });
}

std::string FieldStoreImpl::clearFeed()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_feed.clear();
    }
    m_dirty = true;
    return jsonOk();
}

std::string FieldStoreImpl::refresh()
{
    start();
    m_kick = true;
    m_qcv.notify_all();
    return jsonOk();
}
