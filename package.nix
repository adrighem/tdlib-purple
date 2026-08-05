/* Originally taken from Nixpkgs */

{ lib, stdenv, cmake, libwebp, pidgin, qrencode, tdlib, openssl } :

stdenv.mkDerivation {
  pname = "tdlib-purple";
  version = "2.0.0"; # x-release-please-version

  src = ./.;

  preConfigure = ''
    sed -i -e 's|DESTINATION.*PURPLE_PLUGIN_DIR}|DESTINATION "lib/purple-2|' CMakeLists.txt
    sed -i -e 's|DESTINATION.*PURPLE_DATA_DIR}|DESTINATION "share|' CMakeLists.txt
  '';

  nativeBuildInputs = [ cmake ];
  buildInputs = [ libwebp pidgin qrencode tdlib openssl ];

  cmakeFlags = [ "-DNoVoip=True" ]; # libtgvoip required

  meta = with lib; {
    homepage = "https://github.com/adrighem/tdlib-purple";
    description = "Telegram client for libpurple using TDLib";
    license = licenses.gpl3Plus;
    maintainers = [ ];
    platforms = platforms.unix;
  };
}
