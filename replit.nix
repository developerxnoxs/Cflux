{pkgs}: {
  deps = [
    pkgs.curl
    pkgs.libuv
    pkgs.libpq
    pkgs.postgresql
  ];
}
