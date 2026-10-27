#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logos_module_context.h"
#include "token_codec.h"

struct WalletHandle;

/**
 * @brief A closed-loop shop currency on the Logos Execution Zone.
 *
 * Field Station runs a physical store that takes a token only residents hold.
 * The token is an ordinary LEZ Token-program token with a fixed supply; only
 * the organizer's definition key can mint more, so faucet tokens buy nothing.
 *
 * Three roles share one module and one embedded LEZ wallet:
 *
 *  - **Resident** — a public account (where the organizer sends the 1,000
 *    welcome tokens) and a private account. Paying the shop is either a public
 *    token transfer (about a block, costs a little gas) or a private one: a
 *    zero-knowledge proof spends from the private balance into the shop's
 *    public till, so the shop sees the amount and nothing about who paid.
 *  - **Counter** — anyone with the station code can watch the till; every
 *    increase shows up as a payment line. No keys needed.
 *  - **Organizer** — creates the token, the treasury and the till, issues
 *    welcome allowances, and can mint more (e.g. to replace a lost wallet).
 *
 * Residents also pay each other: a payment link (`fieldpay:<address>:<name>`)
 * names a person, and pasting one turns them into a contact.
 *
 * **Other modules** ask for money through `requestPayment`; the request waits
 * in Field Pay until the resident approves or declines it, and the caller
 * follows it with `requestStatus`. Nothing moves tokens without the resident's
 * say-so — a module that depends on fieldstore_core gets requests, not a
 * blank cheque.
 *
 * This is a universal module: these public methods *are* the API, and the Qt
 * plugin glue is generated from this header. Every wallet call happens on one
 * worker thread — a private payment is minutes of proving and must never sit
 * on the IPC thread — so the methods below only queue work and `state()`
 * returns a cached snapshot the UI polls.
 *
 * Methods that take an amount take it as a decimal string: the IPC layer has
 * no u128 and token amounts are u128 on chain.
 */
class FieldStoreImpl : public LogosModuleContext
{
public:
    FieldStoreImpl();
    ~FieldStoreImpl();

    /// Everything the UI draws, as one JSON object. Never blocks on the network.
    std::string state();

    /// Join a station from the code the organizer shares.
    std::string joinStation(const std::string& code);
    /// Forget the station (keeps the wallet and its balances).
    std::string leaveStation();

    /// Send tokens. `to` is an address, a payment link, or "shop" for the till;
    /// `mode` is "public" or "private". A private send tops up the private
    /// balance from the public one first if it has to.
    std::string send(const std::string& to, const std::string& amount, const std::string& mode,
                     const std::string& note);
    /// Pay the till (same as send("shop", …)).
    std::string pay(const std::string& amount, const std::string& mode, const std::string& note);

    /// The name other people see in your payment link.
    std::string setName(const std::string& name);
    /// Remember (or rename) a contact. `address` may be a payment link.
    std::string saveContact(const std::string& name, const std::string& address);
    std::string removeContact(const std::string& address);

    /// For other modules: ask the resident to pay `amount` to `to` (address or
    /// payment link). Returns {"ok":true,"request":id}; nothing moves until the
    /// resident approves it in Field Pay.
    std::string requestPayment(const std::string& app, const std::string& to, const std::string& amount,
                               const std::string& memo);
    /// For other modules: {"status": pending|paying|paid|declined|failed, "tx", "error", …}
    std::string requestStatus(int64_t id);
    /// The resident approves a request, publicly or privately.
    std::string approveRequest(int64_t id, const std::string& mode);
    std::string declineRequest(int64_t id);
    /// For other modules: this wallet's payment address, name and balance.
    std::string summary();
    /// Move tokens from the public balance into the private one.
    std::string moveToPrivate(const std::string& amount);
    /// Move tokens from the private balance back to the public one.
    std::string moveToPublic(const std::string& amount);
    /// Top up the public account's gas from the testnet's shared faucet account.
    std::string getGas();

    /// The wallet's 24 recovery words.
    std::string recoveryPhrase();
    /// The resident has written the words down; stop nagging.
    std::string acknowledgeRecovery();

    /// Organizer: create the token, a treasury holding the whole supply, and a till.
    std::string createStation(const std::string& name, const std::string& supply);
    /// Organizer: send `amount` tokens (and optionally a little gas) to a resident.
    std::string issue(const std::string& residentAddress, const std::string& amount, bool withGas);
    /// Organizer: mint more supply into the treasury.
    std::string mint(const std::string& amount);

    /// Counter: clear the payment feed.
    std::string clearFeed();
    /// Ask the worker to refresh balances now rather than at the next tick.
    std::string refresh();

protected:
    void onContextReady() override;

private:
    struct Job {
        int64_t     id = 0;
        std::string kind;        // pay-public, pay-private, shield, unshield, gas, create, issue, mint
        std::string label;       // what the UI shows
        std::string status;      // queued, working, proving, confirming, done, failed
        std::string tx;
        std::string error;
        std::string amount;
        int64_t     createdMs = 0;
        int64_t     finishedMs = 0;
        std::function<void(Job&)> run;
    };

    struct FeedEntry {
        std::string amount;
        int64_t     atMs = 0;
        int64_t     block = 0;
    };

    struct Activity {
        int64_t     id = 0;
        std::string dir;         // out, in
        std::string amount;
        std::string address;     // counterparty (base58), empty when unknown
        std::string note;
        bool        isPrivate = false;
        int64_t     atMs = 0;
        int64_t     job = 0;     // the job carrying an outgoing payment
        std::string status;      // done, failed, or the job's live status
        std::string tx;
        std::string error;
    };

    struct Contact {
        std::string name;
        std::string address;
    };

    struct Request {
        int64_t     id = 0;
        std::string app;
        std::string to;          // base58
        std::string amount;
        std::string memo;
        std::string status;      // pending, paying, paid, declined, failed
        int64_t     atMs = 0;
        int64_t     job = 0;
        std::string tx;
        std::string error;
    };

    struct Issued {
        std::string to;
        std::string amount;
        std::string tx;
        int64_t     atMs = 0;
    };

    // ── worker ─────────────────────────────────────────────────────────
    void start();
    void workerLoop();
    void openWallet();
    void refreshBalances();
    std::string enqueue(const std::string& kind, const std::string& label,
                        const std::string& amount, std::function<void(Job&)> run);
    void setJob(int64_t id, const std::function<void(Job&)>& f);

    // ── wallet operations (worker thread only) ────────────────────────
    std::string sendTokenPublic(const fieldstore::Bytes32& from, const fieldstore::Bytes32& to,
                                fieldstore::u128 amount, const fieldstore::Bytes32& definition);
    std::string sendTokenPrivate(bool fromPrivate, const fieldstore::Bytes32& from,
                                 bool toPrivate, const fieldstore::Bytes32& to,
                                 fieldstore::u128 amount, const fieldstore::Bytes32& definition);
    std::string sendPublicCall(const std::vector<std::pair<fieldstore::Bytes32, bool>>& accounts,
                               const std::vector<uint8_t>& instruction,
                               const fieldstore::Bytes32* payer);
    std::string sendNative(const fieldstore::Bytes32& from, const fieldstore::Bytes32& to,
                           fieldstore::u128 amount);
    void confirm(const std::string& txHash, int64_t jobId);
    void ensureFaucet();
    void ensureGas(fieldstore::u128 atLeast, fieldstore::u128 topUp);
    void syncPrivate();
    std::string queueSend(const std::string& toB58, fieldstore::u128 amount, const std::string& amountStr,
                          bool priv, const std::string& note, int64_t requestId);
    std::string nameFor(const std::string& address) const;   // m_mu held
    static bool parseLink(const std::string& in, std::string& address, std::string& name);
    void chooseHoldings();
    std::string otherTokenOn(const fieldstore::Bytes32& account, const fieldstore::Bytes32& definition,
                             bool isPrivate = false);
    void expectMoved(const fieldstore::Bytes32& account, bool isPrivate, const fieldstore::Bytes32& definition,
                     fieldstore::u128 before, const std::string& what);
    fieldstore::Bytes32 newPublicAccount();
    fieldstore::Bytes32 newPrivateAccount();
    bool readPublicShard(const fieldstore::Bytes32& account, const fieldstore::Bytes32& program,
                         std::vector<uint8_t>& out);
    bool readPrivateShard(const fieldstore::Bytes32& account, const fieldstore::Bytes32& program,
                          std::vector<uint8_t>& out);
    fieldstore::u128 tokenBalance(const fieldstore::Bytes32& account, bool isPrivate,
                                  const fieldstore::Bytes32& definition);
    fieldstore::u128 nativeBalance(const fieldstore::Bytes32& account);
    std::string toBase58(const fieldstore::Bytes32& id);
    bool fromBase58(const std::string& s, fieldstore::Bytes32& out);
    void saveWallet();

    // ── persistence ───────────────────────────────────────────────────
    std::string dataDir() const;
    void loadMeta();
    void saveMeta();

    // ── fields ────────────────────────────────────────────────────────
    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::mutex              m_qmu;      // guards m_queue
    std::condition_variable m_qcv;
    std::deque<int64_t>     m_queue;
    std::atomic<bool>       m_kick{false};
    std::atomic<bool>       m_dirty{false};   // meta needs writing

    WalletHandle* m_wallet = nullptr;   // worker thread only

    mutable std::mutex m_mu;            // guards everything below
    std::string m_phase = "starting";   // starting, ready, error
    std::string m_error;
    std::string m_sequencer;
    int64_t     m_block = 0;
    int64_t     m_syncedBlock = 0;
    bool        m_online = false;

    std::string m_mnemonic;
    std::string m_password;
    bool        m_recoveryAcked = false;

    bool                m_haveMe = false;
    fieldstore::Bytes32 m_mePublic{};
    fieldstore::Bytes32 m_mePrivate{};
    std::string         m_mePublicB58, m_mePrivateB58;
    // Per-station token accounts (see chooseHoldings); m_mePublic keeps the LGO.
    bool                m_haveHold = false;
    fieldstore::Bytes32 m_holdPublic{}, m_holdPrivate{};
    std::string         m_holdPublicB58;
    fieldstore::u128    m_myPublicTokens = 0, m_myPrivateTokens = 0, m_myGas = 0;

    bool                m_haveFaucet = false;
    fieldstore::Bytes32 m_faucet{};

    bool                m_joined = false;
    fieldstore::Bytes32 m_token{}, m_till{};
    std::string         m_tokenB58, m_tillB58, m_stationName;
    fieldstore::u128    m_supply = 0, m_tillBalance = 0;
    bool                m_tillSeen = false;
    std::vector<FeedEntry> m_feed;

    bool                m_organizer = false;
    fieldstore::Bytes32 m_treasury{};
    std::string         m_treasuryB58;
    fieldstore::u128    m_treasuryBalance = 0;
    std::vector<Issued> m_issued;

    std::vector<Job> m_jobs;
    int64_t          m_nextJob = 1;

    std::string           m_myName;
    std::vector<Contact>  m_contacts;
    std::vector<Activity> m_activity;
    int64_t               m_nextActivity = 1;
    std::vector<Request>  m_requests;
    int64_t               m_nextRequest = 1;
    // Incoming payments are rises in the public holding that our own jobs
    // don't explain; jobs adjust the baseline by what they moved.
    bool                  m_pubSeen = false;
    fieldstore::u128      m_pubBaseline = 0;
};
