/* Originally taken from Nixpkgs */

{ lib, stdenv, cmake, libwebp, pidgin, tdlib, openssl_3 } :

stdenv.mkDerivation {
  pname = "tdlib-purple";
  version = "0.8.1";

  src = ./.;

  preConfigure = ''
    sed -i -e 's|DESTINATION.*PURPLE_PLUGIN_DIR}|DESTINATION "lib/purple-2|' CMakeLists.txt
    sed -i -e 's|DESTINATION.*PURPLE_DATA_DIR}|DESTINATION "share|' CMakeLists.txt
  '';

  nativeBuildInputs = [ cmake ];
  buildInputs = [ libwebp pidgin tdlib openssl_3 ];

  cmakeFlags = [ "-DNoVoip=True" ]; # libtgvoip required

  meta = with lib; {
    homepage = "https://github.com/ars3niy/tdlib-purple";
    description = "libpurple Telegram plugin using tdlib";
    license = licenses.gpl2Plus;
    maintainers = [ ];
    platforms = platforms.unix;
  };
}
