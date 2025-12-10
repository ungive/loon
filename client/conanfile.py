from conan import ConanFile
from conan.tools.cmake import cmake_layout


class Loon(ConanFile):
    name = "loon"
    description = "Make a local file accessible online instantly via HTTP"
    author = "Jonas van den Berg"
    version = "0.1"

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    requires = (
        "openssl/[~3.5]",  # LTS version until Apr 2030
        "protobuf/3.21.12",
        # Using libdatachannel as a git submodule until the following PR is merged:
        # https://github.com/paullouisageneau/libdatachannel/pull/1488
        # "libdatachannel/0.23.2",
    )

    test_requires = "libcurl/[~8]"

    tool_requires = "cmake/3.31.6"

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

        self.options["openssl"].shared = True
        if self.settings.os == "Macos":
            self.options["openssl"].openssldir = "/etc/ssl"

        if self.settings.os == "Windows":
            self.options["libcurl"].with_ssl = "schannel"

    def layout(self):
        cmake_layout(self, src_folder="src")
