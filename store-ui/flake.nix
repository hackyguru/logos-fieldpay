{
  description = "fieldstore — QML frontend for the fieldstore_core module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # The core module from this same repo. To build against a local checkout:
    #   nix build --override-input fieldstore_core path:../store-core '.#lgx-portable'
    fieldstore_core.url = "github:hackyguru/logos-fieldpay?dir=store-core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
