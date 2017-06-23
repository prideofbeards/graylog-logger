from conans import ConanFile, CMake, tools
import os


class GraylogloggerConan(ConanFile):
    name = "graylog-logger"
    version = "1.0.2"
    license = "BSD 2-Clause"
    url = "https://bintray.com/amues/graylog-logger"
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False]}
    default_options = "shared=True"
    generators = "cmake"

    def source(self):
        self.run("git clone https://github.com/ess-dmsc/graylog-logger.git")
        self.run("cd graylog-logger && git checkout v1.0.2")
        tools.replace_in_file("graylog-logger/CMakeLists.txt", "PROJECT(dm-graylog-logger)", '''PROJECT(dm-graylog-logger)
include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup()''')

    def build(self):
        cmake = CMake(self)
        shared = "-DBUILD_SHARED_LIBS=ON" if self.options.shared else ""
        self.run('cmake graylog-logger %s %s' % (cmake.command_line, shared))
        self.run("cmake --build . %s" % cmake.build_config)

    def package(self):
        self.copy("*.h", dst="include/graylog_logger", src="graylog-logger/include/graylog_logger")
        self.copy("*.hpp", dst="include/graylog_logger", src="graylog-logger/include/graylog_logger")
        self.copy("*.so", dst="lib", keep_path=False)
        self.copy("*.a", dst="lib", keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["graylog_logger"]
