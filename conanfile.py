import subprocess
from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake
from conan.tools.files import copy
from os.path import join

required_conan_version = ">=1.60.0"

class HomestoreConan(ConanFile):
    name = "homestore"
    version = "5.3.1"

    homepage = "https://github.com/eBay/Homestore"
    description = "HomeStore Storage Engine"
    topics = ("ebay", "nublox")
    url = "https://github.com/eBay/Homestore"
    license = "Apache-2.0"

    settings = "arch", "os", "compiler", "build_type"

    options = {
                "shared": ['True', 'False'],
                "fPIC": ['True', 'False'],
                "coverage": ['True', 'False'],
                "sanitize": ['True', 'False'],
                "testing" : ['full', 'min', 'off', 'epoll_mode', 'spdk_mode'],
            }
    default_options = {
                'shared':       False,
                'fPIC':         True,
                'coverage':     False,
                'sanitize':     False,
                'testing':      'epoll_mode',
            }

    exports_sources = "cmake/*", "src/*", "CMakeLists.txt", "test_wrap.sh", "LICENSE"
    keep_imports = True

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        if self.settings.build_type == "Debug":
            if self.options.coverage and self.options.sanitize:
                raise ConanInvalidConfiguration("Sanitizer does not work with Code Coverage!")

    def build_requirements(self):
        self.test_requires("benchmark/1.8.2")
        self.test_requires("gtest/1.14.0")

    def requirements(self):
        # Core async / coroutines
        self.requires("folly/2024.08.12.00", transitive_headers=True)

        # Logging
        self.requires("spdlog/1.12.0", transitive_headers=True)
        self.requires("fmt/10.0.0", transitive_headers=True, override=True)

        # Data structures / utilities (formerly from sisl)
        self.requires("boost/1.85.0", transitive_headers=True)
        self.requires("nlohmann_json/3.11.2", transitive_headers=True)
        self.requires("userspace-rcu/0.14.0", transitive_headers=True)
        self.requires("snappy/1.2.1", transitive_headers=True)

        # Settings (flatbuffers-based config)
        self.requires("flatbuffers/23.5.26", transitive_headers=True)

        # Options parsing
        self.requires("cxxopts/3.1.1", transitive_headers=True)

        self.requires("farmhash/cci.20190513", transitive_headers=True)
        if self.settings.arch in ['x86', 'x86_64']:
            self.requires("isa-l/2.30.0", transitive_headers=True)

        # Tests require OpenSSL 3.x
        self.requires("openssl/[^3.1]", override=True)

    def layout(self):
        self.folders.source = "."
        if self.options.get_safe("sanitize"):
            self.folders.build = join("build", "Sanitized")
        elif self.options.get_safe("coverage"):
            self.folders.build = join("build", "Coverage")
        else:
            self.folders.build = join("build", str(self.settings.build_type))
        self.folders.generators = join(self.folders.build, "generators")

        self.cpp.source.includedirs = ["src/include"]
        self.cpp.build.libdirs = ["src"]
        self.cpp.package.libs = ["homestore"]
        self.cpp.package.includedirs = ["include"]
        self.cpp.package.libdirs = ["lib"]

        if not self.settings.arch in ['x86', 'x86_64']:
            self.cpp.package.defines.append("NO_ISAL")

    def generate(self):
        tc = CMakeToolchain(self)
        if self.options.testing != "off":
            tc.variables["TEST_TARGET"] = self.options.testing
        tc.variables["CONAN_CMAKE_SILENT_OUTPUT"] = "ON"
        tc.variables['CMAKE_EXPORT_COMPILE_COMMANDS'] = 'ON'
        tc.variables["CTEST_OUTPUT_ON_FAILURE"] = "ON"
        tc.variables["MEMORY_SANITIZER_ON"] = "OFF"
        tc.variables["BUILD_COVERAGE"] = "OFF"
        if self.settings.build_type == "Debug":
            if self.options.get_safe("coverage"):
                tc.variables['BUILD_COVERAGE'] = 'ON'
            elif self.options.get_safe("sanitize"):
                tc.variables['MEMORY_SANITIZER_ON'] = 'ON'
        tc.variables["CONAN_PACKAGE_NAME"] = self.name
        tc.variables["CONAN_PACKAGE_VERSION"] = self.version
        # On macOS with Unix Makefiles generator, cmake passes CMAKE_OSX_SYSROOT
        # literally to clang. Resolve the symbolic SDK name to a real path here so
        # the toolchain file contains an absolute path that clang can use.
        if self.settings.os == "Macos":
            try:
                sdk_path = subprocess.check_output(
                    ["xcrun", "--show-sdk-path", "--sdk", "macosx"], text=True
                ).strip()
                tc.cache_variables["CMAKE_OSX_SYSROOT"] = sdk_path
            except Exception:
                pass
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if not self.conf.get("tools.build:skip_test", default=False):
            cmake.test()

    def package(self):
        copy(self, "LICENSE", self.source_folder, join(self.package_folder, "licenses"), keep_path=False)
        copy(self, "*.h", join(self.source_folder, "src", "include"), join(self.package_folder, "include"), keep_path=True)
        copy(self, "*.hpp", join(self.source_folder, "src", "include"), join(self.package_folder, "include"), keep_path=True)
        copy(self, "*.ipp", join(self.source_folder, "src", "include"), join(self.package_folder, "include"), keep_path=True)
        copy(self, "*.a", self.build_folder, join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.so", self.build_folder, join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.dylib", self.build_folder, join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.dll", self.build_folder, join(self.package_folder, "lib"), keep_path=False)

    def package_info(self):
        if self.options.sanitize:
            self.cpp_info.sharedlinkflags.append("-fsanitize=address")
            self.cpp_info.exelinkflags.append("-fsanitize=address")
            self.cpp_info.sharedlinkflags.append("-fsanitize=undefined")
            self.cpp_info.exelinkflags.append("-fsanitize=undefined")
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.append("aio")
