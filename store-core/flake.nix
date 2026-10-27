{
  description = "fieldstore_core — fixed-supply LEZ token payments for a pop-up shop (links the LEZ wallet FFI directly)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # LEZ v0.3.1, what the public testnet runs since its ~Oct 13 2026 reset.
    logos-execution-zone.url = "github:logos-blockchain/logos-execution-zone?rev=1a003bfbfb89fcf987f62975cd08a401169e1b8c";
    # Its lock pins a rust-overlay whose "stable.latest" predates the Rust 1.98
    # that v0.3.1 needs, so follow a current one.
    rust-overlay.url = "github:oxalica/rust-overlay";
    logos-execution-zone.inputs.rust-overlay.follows = "rust-overlay";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        wallet_ffi = {
          input = inputs.logos-execution-zone;
          packages.default = "wallet";
        };
      };
    };
}
