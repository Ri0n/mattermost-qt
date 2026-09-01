add_library(qsourcehighlite STATIC
    qsourcehighliter.cpp
    qsourcehighliterthemes.cpp
    languagedata.cpp
)

target_include_directories(qsourcehighlite
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(qsourcehighlite
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Gui
)

# Keep warnings from vendored upstream visible, but do not make third-party
# diagnostics fail our strict application build.
if(MSVC)
    target_compile_options(qsourcehighlite PRIVATE /WX-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qsourcehighlite PRIVATE -Wno-error)
endif()
