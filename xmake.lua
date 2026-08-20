set_xmakever("3.0.6")
set_version("0.1.4")
includes("@builtin/xpack")

-- Add require FFmpeg
add_requires("ffmpeg-btbn n7.1.2", {configs = {shared = true, runtime = "MD"}})

-- Add require OpenCV
add_requires("opencv 4.12.0", {configs = {shared = true, ffmpeg = false}})

-- Add require WebView
add_requires("webview 0.11.0")

-- Add require nlohmann_json
add_requires("nlohmann_json 3.12.0")

-- Add require gtest
add_requires("gtest")

add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "$(builddir)"})

if is_mode("release") then
    set_runtimes("MD")
elseif is_mode("debug") then
    set_runtimes("MDd")
end

target("core")
    set_kind("binary")
    set_basename("ArkBattleRecordClipper")

    set_optimize("fastest")
    set_languages("cxx20")
    set_warnings("all", "error")
    set_encodings("utf-8")

    set_prefixdir("/", {bindir = "/"})
    set_installdir("ArkBattleRecordClipper")

    add_files("src/*.cpp")

    -- XMake integrated embed plugin
    add_rules("utils.bin2obj", {extensions = {".html"}})
    add_files("ui/index.html", {zeroend = true})

    add_packages("ffmpeg-btbn", "opencv", "webview", "nlohmann_json")
    add_syslinks("dwmapi")

    add_installfiles("(assets/**)")

    before_run(function (target) 
        os.cp("$(projectdir)/assets", target:targetdir())
        os.cp("$(projectdir)/ui", target:targetdir())
    end)

    after_clean(function (target) 
        os.rm(target:targetdir() .. "/assets")
        os.rm(target:targetdir() .. "/ui")
    end)

target("tests")
    set_kind("binary")
    set_default(false)

    set_languages("cxx20")
    set_warnings("all", "error")
    set_encodings("utf-8")

    add_files("src/tests/**.cpp")
    add_files("src/*.cpp|main.cpp")
    add_includedirs("src")

    add_packages("gtest", "opencv", "ffmpeg-btbn", "nlohmann_json")
    add_syslinks("kernel32")

xpack("core")
    set_formats("srczip")
    set_basename("Source Code")
    add_sourcefiles("(assets/**)")
    add_sourcefiles("(src/**)")
    add_sourcefiles("(ui/**)")
    add_sourcefiles(".gitignore", "NOTICE", "README.md", "xmake.lua")

-- Define FFmpeg package (BtbN LGPL build)
package("ffmpeg-btbn")
    set_homepage("https://www.ffmpeg.org")
    set_description("A collection of libraries to process multimedia content such as audio, video, subtitles and related metadata.")
    set_license("LGPL-3.0")

    if is_plat("windows") then
        add_urls("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2025-10-31-13-40/ffmpeg-n7.1.2-7-g24c44c34dc-win64-lgpl-shared-7.1.zip",
                {alias = "BtbN"})
        add_versions("BtbN:n7.1.2", "0a65ca92d0ac2a7bcbda1fd7d0e5d511fc2eade57d44fde67a8051955af054c3")
        add_links("avfilter", "avdevice", "avformat", "avcodec", "swscale", "swresample", "avutil")
    end

    on_install("windows", function (package)
        os.cp("include", package:installdir())
        os.cp("lib/*.lib", package:installdir("lib"))
        os.cp("lib/*.def", package:installdir("lib"))
        os.cp("bin/*.dll", package:installdir("bin"))
    end)
package_end()

-- Nuget package "Microsoft.Web.WebView2"
package("mswebview2")
    set_kind("library")
    set_homepage("https://aka.ms/webview")
    set_description("The WebView2 control enables you to embed web technologies (HTML, CSS, and JavaScript) in your native applications powered by Microsoft Edge (Chromium).")
    set_license("https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4078.44/License")

    add_urls("https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4078.44#pkg.zip")  -- Hook xmake to consider .nupkg as .zip
    add_versions("1.0", "dc4d1d9168df26b830398303e50210b6e1729f6ce5a7ac69d2c766852f489962")      -- Xmake reject non-semver version, freeze it into url
    
    on_install(function (package)
        os.cp("build/native/include", package:installdir())
        if is_plat("windows") and package:is_arch("x64") then
            os.cp("build/native/x64/*", package:installdir("bin"))
        end
    end)
package_end()

package("webview")
    set_kind("library", {headeronly = true})
    set_homepage("https://github.com/webview/webview")
    set_description("Tiny cross-platform webview library for C/C++.")
    set_license("MIT")
    set_urls("https://github.com/webview/webview.git")

    add_versions("0.11.0", "ca3ea702596fec8d6b8aee2bbcb7edeeb95a3d85f61acccb4e6d11a22fa9598e")
    
    if is_plat("windows") then
        add_deps("mswebview2 1.0")
    end

    on_install("windows", function (package)
        os.cp("core/include", package:installdir())
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("webview/webview.h"))
        assert(package:has_cxxfuncs("webview_create", {includes = "webview/webview.h"}))
    end)
package_end()