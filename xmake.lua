add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server")
option_end()

add_requires("levilamina 26.60.7", {configs = {target_type = "server"}})
add_requires("levibuildscript")
add_requires("cpp-httplib 0.63.0")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- LLVM 22 clang-cl is required (the LeviLamina server SDK does not build with Visual Studio's clang 19).
set_toolchains("clang-cl")
set_languages("c++20")

target("Karakuri")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
    set_exceptions("none")
    add_cxflags("/EHa", "/utf-8", "/W4")
    add_cxflags("/EHs", "-Wno-microsoft-cast", "-Wno-invalid-offsetof", "-Wno-pragma-system-header-outside-header",
        {tools = {"clang_cl"}})
    add_packages("levilamina", "cpp-httplib")
    set_kind("shared")
    set_symbols("debug")
    add_includedirs("src")
    add_headerfiles("src/**.h")
    add_files("src/core/**.cpp", "src/mod/**.cpp")

-- Pure tests: no game types, so they build and run without a server.
target("KarakuriTests")
    set_kind("binary")
    set_default(false)
    add_cxflags("/utf-8", "/W4", "-Wno-missing-designated-field-initializers")
    add_includedirs("src")
    add_files("tests/*.cpp", "src/core/**.cpp")
