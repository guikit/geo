cmake_minimum_required(VERSION 3.21)
project(guikit_geo VERSION 1.0.0 LANGUAGES CXX)

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

option(GUIKIT_GEO_BUILD_TESTS "Build GuiKit Geo unit tests" ${PROJECT_IS_TOP_LEVEL})
option(GUIKIT_GEO_EMBED_ASSETS "Embed built-in hydrographic GeoJSON chart into binary" ON)
option(GUIKIT_GEO_INSTALL "Enable CMake install targets for guikit_geo" ${PROJECT_IS_TOP_LEVEL})

if (GUIKIT_GEO_BUILD_TESTS)
    enable_testing()
endif()

if (NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()

if (NOT TARGET guikit::core)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/guikit/CMakeLists.txt")
        add_subdirectory(third_party/guikit EXCLUDE_FROM_ALL)
    else()
        find_package(guikit REQUIRED)
    endif()
endif()

if (NOT TARGET keel::http)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/http/CMakeLists.txt")
        if (GUIKIT_GEO_INSTALL)
            add_subdirectory(third_party/http)
        else()
            add_subdirectory(third_party/http EXCLUDE_FROM_ALL)
        endif()
    else()
        find_package(keel_http REQUIRED)
    endif()
endif()

set(GUIKIT_GEO_SOURCES
    src/guikit/geo/geo_json_io.cpp
    src/guikit/geo/geo_types.cpp
    src/guikit/geo/gis_render_engine.cpp
    src/guikit/geo/map_view.cpp
    src/guikit/geo/osm_raster_tile_provider.cpp
    src/guikit/geo/polygon_triangulator.cpp
)

set(GUIKIT_GEO_GENERATED_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/geo_embedded_assets.cpp")
if (GUIKIT_GEO_EMBED_ASSETS)
    file(GLOB_RECURSE GUIKIT_GEO_ASSET_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/*"
    )
    add_custom_command(
        OUTPUT "${GUIKIT_GEO_GENERATED_ASSETS_CPP}"
        COMMAND "${CMAKE_COMMAND}"
            "-DASSETS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "-DOUTPUT_FILE=${GUIKIT_GEO_GENERATED_ASSETS_CPP}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateGeoAssets.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateGeoAssets.cmake"
            ${GUIKIT_GEO_ASSET_FILES}
        COMMENT "Embedding GuiKit Geo assets into C++ binary"
        VERBATIM
    )
else()
    file(WRITE "${GUIKIT_GEO_GENERATED_ASSETS_CPP}"
        "namespace guikit::geo { void RegisterEmbeddedAssets() noexcept {} }\n"
    )
endif()

add_library(guikit_geo STATIC
    ${GUIKIT_GEO_SOURCES}
    "${GUIKIT_GEO_GENERATED_ASSETS_CPP}"
)
add_library(guikit::geo ALIAS guikit_geo)
set_target_properties(guikit_geo PROPERTIES EXPORT_NAME geo)

target_compile_features(guikit_geo PUBLIC cxx_std_20)
target_include_directories(guikit_geo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(guikit_geo PUBLIC guikit::core keel::http)
if (NOT EMSCRIPTEN)
    target_link_libraries(guikit_geo PUBLIC Threads::Threads)
endif()

if (COMMAND guikit_enforce_guidelines)
    guikit_enforce_guidelines(guikit_geo
        HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
        STANDALONE_HEADERS ${GUIKIT_GEO_BUILD_TESTS}
        TEST_NAME guikit_geo_header_purity_scan
    )
endif()

if (GUIKIT_GEO_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS guikit_geo
        EXPORT guikitGeoTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/guikit"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(EXPORT guikitGeoTargets
        FILE guikitGeoTargets.cmake
        NAMESPACE guikit::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/guikit_geo"
    )
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/guikit_geoConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/guikit_geoConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/guikit_geo"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/guikit_geoConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/guikit_geoConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/guikit_geoConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/guikit_geo"
    )
endif()

if (GUIKIT_GEO_BUILD_TESTS AND TARGET guikit::guikit)
    add_executable(test_geo
        tests/test_geo.cpp
    )
    target_link_libraries(test_geo PRIVATE guikit::geo guikit::guikit)
    if (COMMAND guikit_enforce_guidelines)
        guikit_enforce_guidelines(test_geo)
    endif()
    add_test(NAME geo COMMAND test_geo)
endif()
