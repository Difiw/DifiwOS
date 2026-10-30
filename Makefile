# Difiw OS 0.1.0 Core - top-level entry points (run inside Linux/WSL).
# From Windows:  powershell -File scripts/wsl.ps1 build
.PHONY: check fetch configure build verify run test clean distclean legal-info

check:
	./scripts/check-host.sh
fetch:
	./scripts/fetch-buildroot.sh
configure:
	./scripts/configure.sh
build:
	./scripts/build.sh
verify:
	./scripts/verify.sh
run:
	./scripts/run-qemu.sh
test:
	$(MAKE) -C src/difiw-shell test
clean:
	./scripts/clean.sh
distclean:
	./scripts/clean.sh --all --artifacts
legal-info:
	./scripts/legal-info.sh
