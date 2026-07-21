cmake_minimum_required(VERSION 3.20)
project(DMG-Converter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ───────────────────────────────────────────────────────────────────
add_executable(DMG-Converter WIN32
    main.cpp
    gui.cpp
    dmg-converter.cpp
    app.rc           # embeds icon.ico as the application icon
)

# ── Link libraries ────────────────────────────────────────────────────────────
# cabinet.lib provides CreateDecompressor/Decompress (Windows Compression API)
# No external zlib needed — we use the Windows-native MSZIP decompressor.
target_link_libraries(DMG-Converter PRIVATE
    cabinet
    ole32
    oleaut32
    shell32
    comdlg32
    gdi32
    user32
    kernel32
)

# ── Unicode ───────────────────────────────────────────────────────────────────
target_compile_definitions(DMG-Converter PRIVATE UNICODE _UNICODE)

# ── MSVC-specific flags ───────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(DMG-Converter PRIVATE /W3 /MP)
    target_compile_definitions(DMG-Converter PRIVATE _CRT_SECURE_NO_WARNINGS)

    # Statically link the MSVC runtime so the exe needs no VC++ Redistributable.
    # Switch /MDd→/MTd (Debug) and /MD→/MT (Release) on all targets.
    set_property(TARGET DMG-Converter PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
