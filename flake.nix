{
  description = "gm_market: a Discord bot that runs a prediction market on who says GM";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";

    # The libraries the bot is built on. Not used as a flake: nix/deps.nix
    # builds it from the revision flake.lock pins here, with this project's
    # compiler. `nix flake update bedrock` moves to its latest revision.
    bedrock = {
      url = "github:lukeyeh/bedrock";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The LLVM version everything is compiled with. The exact compiler is
      # whatever flake.lock pins for this major version.
      llvmFor = pkgs: pkgs.llvmPackages_21;
    in
    {
      # The program, for deployment: `nix build`, `nix run`.
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/package.nix {
          stdenv = (llvmFor pkgs).stdenv;
          # The same libraries Bazel builds against.
          inherit (import ./nix/deps.nix pkgs) bedrock;
        };
      });

      # `nix flake check` builds the package.
      checks = forAllSystems (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      devShells = forAllSystems (pkgs:
        let
          # The compiler Bazel builds with. Bazel picks up $CC from this shell.
          llvm = llvmFor pkgs;
        in
        {
          default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
            packages = [
              # nixpkgs' plain `bazel` is an older major version.
              pkgs.bazel_9
              # buildifier, the BUILD file formatter.
              pkgs.bazel-buildtools
              # clangd, clang-format and clang-tidy, matching the compiler.
              llvm.clang-tools
              # The sqlite3 shell, for looking inside predictions.db.
              pkgs.sqlite
            ];
          };
        });
    };
}
