{
  description = "libdtfl -- timely and differential dataflow for C";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Version from configure.ac's AC_INIT, so the flake and the build
        # agree without a second source of truth.  Parse the AC_INIT line
        # by splitting on brackets (its second [...] field is the version),
        # which avoids a regex engine that rejects escaped brackets.
        version =
          let
            lines = pkgs.lib.splitString "\n" (builtins.readFile ./configure.ac);
            acLine = pkgs.lib.findFirst
              (l: pkgs.lib.hasPrefix "AC_INIT([libdtfl]" l) "" lines;
            # "AC_INIT([libdtfl], [0.0.1], ..." splits on "[" and "]" so
            # element 3 is the version string.
            parts = builtins.filter builtins.isString
              (builtins.split "[][]" acLine);
          in if builtins.length parts >= 4
             then builtins.elemAt parts 3
             else "0.0.0";

        nativeBuildInputs = with pkgs; [
          autoconf automake libtool pkg-config texinfo mandoc
        ];
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "libdtfl";
          inherit version;
          src = ./.;
          inherit nativeBuildInputs;
          # bootstrap generates configure from the git checkout.
          preConfigure = "./bootstrap";
          doCheck = true;
          meta = with pkgs.lib; {
            description = "Timely and differential dataflow for C";
            homepage = "https://codeberg.org/gregburd/libdtfl";
            license = licenses.mit;
            platforms = platforms.unix;
          };
        };

        devShells.default = pkgs.mkShell {
          packages = nativeBuildInputs ++ (with pkgs; [ gcc clang gdb valgrind ]);
        };
      });
}
