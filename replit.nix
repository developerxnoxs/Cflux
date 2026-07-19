{pkgs}: {
  deps = [
    pkgs.wslay
    pkgs.openssl
    pkgs.curl
    pkgs.libuv
    pkgs.libpq
    pkgs.postgresql
  ];
}
