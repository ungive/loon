.PHONY: all docs

all:

docs:
	doxygen Doxyfile

run-test-server:
	go run ../cmd/loon server -addr localhost:8071 -config ./test/server.config.yml

define conan_install
	conan install . \
		--profile:all="./conan/profiles/$(1)" \
		--settings:all="build_type=$(2)" \
		--output-folder="./conan/out/$(1)" \
		--build=missing
endef

conan-install-linux-gcc-x86_64-debug:
	$(call conan_install,linux-gcc-x86_64,Debug)

conan-install-linux-gcc-x86_64-release:
	$(call conan_install,linux-gcc-x86_64,Release)

conan-install-linux-gcc-x86_64: \
	conan-install-linux-gcc-x86_64-debug \
	conan-install-linux-gcc-x86_64-release

conan-install-windows-msvc-visual-studio-x86_64-debug:
	$(call conan_install,windows-msvc-visual-studio-x86_64,Debug)

conan-install-windows-msvc-visual-studio-x86_64-release:
	$(call conan_install,windows-msvc-visual-studio-x86_64,Release)

conan-install-windows-msvc-visual-studio-x86_64: \
	conan-install-windows-msvc-visual-studio-x86_64-debug \
	conan-install-windows-msvc-visual-studio-x86_64-release

conan-install-mac-clang-armv8-debug:
	$(call conan_install,mac-clang-armv8,Debug)

conan-install-mac-clang-armv8-release:
	$(call conan_install,mac-clang-armv8,Release)

conan-install-mac-clang-armv8: \
	conan-install-mac-clang-armv8-debug \
	conan-install-mac-clang-armv8-release

conan-install-mac-clang-x86_64-debug:
	$(call conan_install,mac-clang-x86_64,Debug)

conan-install-mac-clang-x86_64-release:
	$(call conan_install,mac-clang-x86_64,Release)

conan-install-mac-clang-x86_64: \
	conan-install-mac-clang-x86_64-debug \
	conan-install-mac-clang-x86_64-release

# Convenience targets for development

dev-lin: \
	conan-install-linux-gcc-x86_64

dev-win: \
	conan-install-windows-msvc-visual-studio-x86_64

dev-mac: \
	conan-install-mac-clang-armv8

dev-mac-intel: \
	conan-install-mac-clang-x86_64

compile-commands-lin:
	ln -s ./build/linux-gcc-debug/compile_commands.json compile_commands.json
