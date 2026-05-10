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
                "sanitize": ['auto', 'True', 'False'],
                "testing" : ['full', 'min', 'off'],
                "malloc_impl": ['libc', 'tcmalloc', 'jemalloc'],
            }
    default_options = {
                'shared':       False,
                'fPIC':         True,
                'coverage':     False,
                'sanitize':     'auto',
                'testing':      'min',
                'malloc_impl':  'jemalloc',
            }

    exports_sources = "cmake/*", "src/*", "CMakeLists.txt", "test_wrap.sh", "LICENSE"
    keep_imports = True

    # ── Helpers — derive effective values without mutating self.options ─────────────────────────────────────────────
    # Conan 2 forbids assigning to self.options.<x> after the recipe options have been frozen (which is by the time
    # configure() runs).  These helpers compute the effective sanitize / malloc_impl on demand instead.
    def _is_sanitize_on(self):
        if str(self.options.sanitize) == 'True':
            return True
        if (str(self.options.sanitize) == 'auto'
                and self.settings.build_type == "Debug"
                and not self.options.coverage):
            return True
        return False

    def _malloc_impl(self):
        # ASAN intercepts malloc/free/new/delete and conflicts with tcmalloc/jemalloc which override the same
        # symbols.  Force libc when sanitize is on, regardless of what the user requested.
        if self._is_sanitize_on():
            return 'libc'
        return str(self.options.malloc_impl)

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

        # sanitize is only valid for Debug builds.  Reject explicit True on Release/RelWithDebInfo so it cannot be
        # turned on accidentally.
        if str(self.options.sanitize) == 'True' and self.settings.build_type != "Debug":
            raise ConanInvalidConfiguration(
                "sanitize=True is only valid for build_type=Debug (got {})".format(self.settings.build_type))

        if self._is_sanitize_on() and self.options.coverage:
            raise ConanInvalidConfiguration("Sanitizer does not work with Code Coverage!")

    def build_requirements(self):
        self.test_requires("benchmark/1.8.2")
        self.test_requires("gtest/1.14.0")

    def requirements(self):
        # Core async / coroutines
        self.requires("folly/2024.08.12.00", transitive_headers=True)

        # Logging
        self.requires("spdlog/1.17.0", transitive_headers=True)
        self.requires("fmt/11.1.4", transitive_headers=True, override=True)

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

        # semver200.h (used by sisl/version.h for VersionMgr)
        self.requires("zmarok-semver/1.1.0", transitive_headers=True)

        # Tests require OpenSSL 3.x
        self.requires("openssl/[^3.1]", override=True)

        # Memory allocation — _malloc_impl() folds in the ASAN auto-coerce.
        impl = self._malloc_impl()
        if impl == "tcmalloc":
            self.requires("gperftools/2.15", transitive_headers=True)
        elif impl == "jemalloc":
            self.requires("jemalloc/5.3.0", transitive_headers=True)

    def layout(self):
        self.folders.source = "."
        if self._is_sanitize_on():
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
            tc.cache_variables["BUILD_TESTING"] = True
        tc.variables["CONAN_CMAKE_SILENT_OUTPUT"] = "ON"
        tc.variables['CMAKE_EXPORT_COMPILE_COMMANDS'] = 'ON'
        tc.variables["CTEST_OUTPUT_ON_FAILURE"] = "ON"
        tc.variables["MEMORY_SANITIZER_ON"] = "OFF"
        tc.variables["BUILD_COVERAGE"] = "OFF"
        if self.settings.build_type == "Debug":
            if self.options.get_safe("coverage"):
                tc.variables['BUILD_COVERAGE'] = 'ON'
            elif self._is_sanitize_on():
                tc.variables['MEMORY_SANITIZER_ON'] = 'ON'
        tc.variables["CONAN_PACKAGE_NAME"] = self.name
        tc.variables["CONAN_PACKAGE_VERSION"] = self.version
        tc.variables["MALLOC_IMPL"] = self._malloc_impl()
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
        target = self.conf.get("user.cmake:build_target", default=None, check_type=str)
        cmake.build(target=target)
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
        if self._is_sanitize_on():
            self.cpp_info.sharedlinkflags.append("-fsanitize=address")
            self.cpp_info.exelinkflags.append("-fsanitize=address")
            self.cpp_info.sharedlinkflags.append("-fsanitize=undefined")
            self.cpp_info.exelinkflags.append("-fsanitize=undefined")
        impl = self._malloc_impl()
        if impl == 'jemalloc':
            self.cpp_info.defines.append("USE_JEMALLOC=1")
            self.cpp_info.requires.extend(["jemalloc::jemalloc"])
        elif impl == 'tcmalloc':
            self.cpp_info.defines.append("USING_TCMALLOC=1")
            self.cpp_info.requires.extend(["gperftools::gperftools"])
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.append("aio")
