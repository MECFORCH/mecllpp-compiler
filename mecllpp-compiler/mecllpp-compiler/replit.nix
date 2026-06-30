{pkgs}: {
  deps = [
    pkgs.lld
    pkgs.qemu
    pkgs.llvm
    pkgs.clang
  ];
}
