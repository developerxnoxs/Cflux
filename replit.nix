{pkgs}: {
  deps = [
    pkgs.openssl
    pkgs.curl
    pkgs.libuv
    pkgs.libpq
    pkgs.postgresql
  ];
}
