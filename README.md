# Field Pay (Field Store)

The money for a pop-up city. The organizer creates a token with a fixed
supply on the Logos Execution Zone (LEZ) and gives every resident 1,000 of it
on arrival. Residents then pay the station shop and each other like in any
payments app, **publicly** or **privately**, as they choose. Other residency
modules (cycle hire, …) charge through it with payment requests the resident
approves.

Faucet tokens can't be used to buy anything. The shop's token is an LEZ
Token-program token whose mint authority is a key held only by the
organizer's wallet. The shop recognizes it by its definition account ID, so a
look-alike token with the same name doesn't count.

```
logos-fieldpay/
├── store-core/   fieldstore_core — universal C++ module, links the LEZ wallet FFI
│   ├── src/token_codec.h        hand-written Borsh for the Token program
│   ├── src/fieldstore_impl.*    wallet, jobs, balances, till feed
│   └── tests/                   offline codec checks + live testnet harness
├── token-program/  the LEZ Token program (vendored), deployed by Field Store itself
├── store-ui/     fieldstore — QML (Logos.Theme / Logos.Controls)
└── install.sh    drop both into a local Basecamp
```

## What residents see

The app calls itself **Field Pay**. The module IDs stay `fieldstore` and
`fieldstore_core`, so existing wallets keep their data.

| Screen | What's on it |
|---|---|
| **Home** | One balance, **Send · Receive · Pay shop**, payment requests from other apps, and an activity list (sent, received, in progress, failed). |
| **Send** | Amount, recipient (contacts as chips, with Shop first, or a pasted payment link), an optional note, and one switch: **Send privately**. |
| **Receive** | Your **payment link** (`fieldpay:<address>:<name>`) and the name inside it. |
| **More** | Backup words, private ⇄ public balance, network fees (LGO), and the station team's tools: **Shop counter** and **Organizer**. |

Everything blockchain-specific stays out of the way:
- **Gas** is topped up automatically before an ordinary payment.
- **Private payments** fund themselves: if the private balance is short, the
  wallet shields the shortfall first, as part of the same payment.
- **Payment links** become contacts the first time you use one.

## For other modules (cycle hire, …)

Declare `fieldstore_core` as a dependency and call it:

| Method | Returns |
|---|---|
| `requestPayment(app, to, amount, memo)` | `{"ok":true,"request":id}`. `to` is an address, a `fieldpay:` link, or `"shop"`. |
| `requestStatus(id)` | `{"status": pending \| paying \| paid \| declined \| failed, "tx", "error", …}` |
| `summary()` | `{"address","name","link","balance","token"}`: who this resident is and what they hold |

A request shows up on Field Pay's Home screen as "*Cycle Hire* asks you to pay
20 FIELD · 2h bike rental · to Alex" with **Pay / Pay privately / Decline**.
**No module can move tokens without the resident tapping Pay.** To charge
for something, your module asks, then polls `requestStatus` until `paid`.
To get paid, it hands out its owner's payment link.

`send(to, amount, mode, note)` exists too, but that's the resident acting
directly in this UI. Other modules should go through requests.

## Public vs private

| | Public | Private |
|---|---|---|
| What's on chain | sender address, till, amount | till and amount only; the payer's accounts don't appear |
| Time | about one block (31 s measured) | proof on the laptop (~110 s on an M-series, all cores) + one block |
| Gas | ~0.0003 LGO | none: private transactions are fee-exempt under the v0.3 interim policy |
| Spends from | public balance | private balance |

Tokens always arrive in the public balance. **To private →** shields some of
them. The shielding transfer itself shows the move from the public balance;
spending afterwards doesn't show who is paying. A private payment pays into
the shop's *public* till. That way the cashier sees every payment instantly
without having to sync private state, and the payer stays anonymous.

## Re-verified on LEZ v0.3.1 (2026-10-23)

After the reset, against our own Token deployment: `tests/run.sh live` created
a station (30,000 supply), issued 1,000 + gas, paid 12 publicly, shielded 100
(about 4 minutes) and paid 7 privately (about 6 minutes, of which ~110 s is
proving). The till read 19 and the resident 888 public + 93 private.

## Verified on the v0.3 public testnet (2026-10-01)

`store-core/tests/run.sh live` runs an organizer and a resident against
`https://testnet.lez.logos.co`. Every path below was run end to end:

| Step | Result | Wall time |
|---|---|---|
| create station (faucet gas + `NewFungibleDefinition`) | supply 30,000 in treasury | 3 min |
| issue 1,000 + 1 LGO gas to a brand-new account | resident 1,000 | 2 min |
| pay 12 publicly | till 12, gas −0.00028 | 31 s |
| move 100 to private | private 100, no gas | 2 min 9 s |
| pay 7 privately | till 19, private 93, no gas | 2 min 20 s |
| move 20 back to public | public 908, private 73 | 2 min 20 s |
| mint 500 | supply 30,500 | 31 s |
| resident joins a second token; issuing to their old address | refused with the reason; new address works | — |
| send 3 to a person via payment link | delivered | 1–4 min (slow blocks that hour) |
| send 2 privately with an empty private balance | auto-shielded, then paid | 4 min 20 s |
| app requests 4 for the shop; resident approves | `requestStatus` → `paid` + tx | 30 s |

The private payment transaction (247 KB, mostly proof) contains neither of the
resident's account IDs; the public one contains the sender's address, as
expected. The modules also load in Basecamp 0.2.3 and show live chain state.

## How it works

- **Our own Token program.** LEZ v0.3.0 shipped a built-in Token program;
  v0.3.1 removed it. `token-program/` is that program's source, vendored from
  LEZ and built against v0.3.1 (`./build.sh`, no Docker needed). It's deployed
  once to the testnet at `z4Bw6JARL5sKDLad19Bgxtb82sTqsviGkygR7vghtBp`
  (`kTokenProgramId` in `token_codec.h`), and the core embeds the binary
  (`src/token.bin`) for proving private payments. It handles
  `NewFungibleDefinition`, `Transfer`, `Mint` and `Burn`. `Mint` requires the
  definition account's signature, which is what makes the supply closed.
- **The core links `libwallet_ffi` directly** rather than calling the stock
  `lez_core` module. `lez_core`'s private-transaction API only accepts accounts
  the wallet owns. A private payment needs "my private account → someone
  else's public till", which the FFI supports through `PublicNoSign` mentions.
- **Hand-written Borsh** (`token_codec.h`, pinned byte-for-byte by
  `tests/harness codec`). Token shards are keyed by the program's address.
- **One worker thread owns the wallet.** Proving takes minutes and must not sit
  on the IPC thread, so every method queues a job and returns at once.
  `state()` returns a cached snapshot, and the UI polls it every second.
- **Gas.** v0.3 has no Piñata program anymore. The testnet's two genesis
  accounts have keys published on purpose (the official wallet UI uses them as
  a shared faucet). The module imports one of them and transfers small amounts
  from it: 20 LGO for the organizer, 1 LGO per resident. Issuing an allowance
  sends the gas along with it, so most residents never touch the faucet.
- **One kind of token per account, forever.** The Token program binds an
  account's holding to the first token it receives, and burning doesn't undo
  it. So the wallet keeps its LGO in one main account and gives each station
  its own public and private holding accounts, creating fresh ones when an
  account is already tied to another token. The main account pays the fees
  for every other account as a co-signing payer: holdings, treasury and the
  definition account never need gas.
- **Inclusion isn't success.** A transaction can land in a block and pay its
  fee while the program inside it panics and changes nothing. Every job
  re-reads the sender's balance after inclusion and fails loudly if it didn't
  move. Issuing also refuses an address that's already tied to another token.

## Build and install

```bash
# core (first build compiles the LEZ wallet FFI: ~5 min, needs Xcode's Metal
# toolchain — `xcodebuild -downloadComponent MetalToolchain`)
cd store-core && nix build 'path:.#lgx-portable' -o result-portable

# ui
cd ../store-ui && nix build 'path:.#lgx-portable' -o result-portable \
    --override-input fieldstore_core path:../store-core

cd .. && ./install.sh            # quits Basecamp, installs, re-signs dylibs
```

Tests:

```bash
store-core/tests/run.sh          # offline: Borsh layout, decoding, formatting
store-core/tests/run.sh live     # creates a throwaway token on the testnet (~12 min)
```

## Running the residency

1. More → Organizer → **Create** (name `FIELD`, supply `30000` for 30
   people). Keep this laptop's wallet safe: it holds the mint key. Back up
   its words.
2. Post the **station code**.
3. Each arrival opens Field Pay, pastes the code, sets their name on
   **Receive** and sends you their payment link. You paste it into
   **Welcome a resident** (1,000 plus a little LGO).
4. Put a laptop at the till on More → **Shop counter**. Hand over goods when
   the amount appears there, not on the customer's screen.

## Limits worth knowing

- **Testnet resets wipe balances.** Keep the Organizer tab's issued list (it's
  in `module_data/fieldstore_core/*/fieldstore.json`) to re-issue after a
  reset.
- **The counter feed is a balance delta.** Two payments landing in the same
  block show as one line with the combined amount, and payments made while the
  counter laptop is closed show up as one catch-up line.
- **Private payments take about two minutes.** That's fine at a pop-up shop if
  people pay while you bag their things, but not for a queue. Public is the
  fast path.
- **Every Basecamp install has its own wallet.** Losing it without the
  recovery words means minting a replacement allowance. You can mint, but you
  can't claw tokens back.
- **The pinned LEZ commit must match the testnet.** A breaking v0.4 means
  bumping the `logos-execution-zone` rev in `store-core/flake.nix` and
  re-checking `token_codec.h` against `lez/programs/token/core`.
- Tested on Basecamp 0.2.3 (macOS arm64). It isn't yet tested on Basecamp
  0.3.x or Linux builds.

## After a testnet reset

The testnet was reset to LEZ v0.3.1 around 2026-10-13. Everything on the old
chain is gone, and wallets built for v0.3.0 can't read the new blocks
("Couldn't sync private state"). To bring Field Pay back after a reset:

```bash
cd token-program && ./build.sh && cp token.bin ../store-core/src/   # only if LEZ changed
cd ../store-core
FIELDSTORE_DATA_DIR=~/fieldstore-deployer tests/harness deploy-token src/token.bin
#   → paste the printed address into kTokenProgramId (src/token_codec.h)
tests/run.sh live                                                  # end to end
nix build 'path:.#lgx-portable' -o result-portable && cd .. && ./install.sh
```

Then reset each Field Pay wallet's data (Basecamp `module_data/fieldstore_core`)
and create a new station. Old stations, tokens and balances don't carry over.
