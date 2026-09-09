{ pkgs ? import <nixpkgs> { } }:

let
  # Keep corrected GL ownership isolated from any other librashader user inside KWin.
  shaderLibrary =
    assert pkgs.lib.assertMsg (pkgs.librashader.version == "0.10.1")
      "WaylandShader's GL ownership patch currently targets librashader 0.10.1.";
    pkgs.librashader.overrideAttrs (old: {
      pname = "waylandshader-librashader";
      patches = (old.patches or [ ]) ++ [ ./patches/librashader-gl-lifetime.patch ];
      postInstall = (old.postInstall or "") + ''
        mv "$out/lib/librashader.so.2" "$out/lib/libwaylandshader-rashader.so.2"
        rm "$out/lib/librashader.so"
        ln -s libwaylandshader-rashader.so.2 "$out/lib/libwaylandshader-rashader.so"
        patchelf --set-soname libwaylandshader-rashader.so.2 "$out/lib/libwaylandshader-rashader.so.2"
      '';
    });
in
pkgs.stdenv.mkDerivation {
  pname = "waylandshader";
  version = "0.1.0";
  src = pkgs.lib.cleanSourceWith {
    src = ./.;
    filter = path: type:
      let name = baseNameOf path;
      in !(builtins.elem name [ "build" "result" ".git" ".cache" ])
        && pkgs.lib.cleanSourceFilter path type;
  };

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    kdePackages.extra-cmake-modules
    kdePackages.wrapQtAppsHook
  ];
  buildInputs = with pkgs; [
    kdePackages.kwin
    kdePackages.qtbase
    kdePackages.qtdeclarative
    kdePackages.kconfig
    kdePackages.kcoreaddons
    libepoxy
    libdrm
    wayland
    libglvnd
    shaderLibrary
  ];

  meta = {
    description = "Native RetroArch slang preset effects for KWin";
    platforms = pkgs.lib.platforms.linux;
    mainProgram = "waylandshader-controller";
  };
}
