# docs/CMakeLists.txt
#
# Generates HTML API documentation with Doxygen when available.
# The `docs` target is optional — the project builds without it.
#
# Generate docs:
#   cmake --build build --target docs

find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME    "ultrafast-feed")
    set(DOXYGEN_PROJECT_VERSION "${PROJECT_VERSION}")
    set(DOXYGEN_PROJECT_BRIEF   "Ultra-low-latency AF_XDP market data feed handler")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set(DOXYGEN_EXTRACT_ALL         YES)
    set(DOXYGEN_RECURSIVE           YES)
    set(DOXYGEN_GENERATE_HTML       YES)
    set(DOXYGEN_GENERATE_LATEX      NO)
    set(DOXYGEN_QUIET               YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
    # Use architecture.md as the landing page
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE
        "${CMAKE_CURRENT_SOURCE_DIR}/architecture.md")
    # Exclude third-party fetched content from the documentation
    set(DOXYGEN_EXCLUDE_PATTERNS
        "*/googletest/*"
        "*/benchmark/*"
        "*/_deps/*")

    doxygen_add_docs(docs
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/architecture.md"
        COMMENT "Generating API documentation"
    )
    message(STATUS "[ultrafast-feed] Documentation target: cmake --build . --target docs")
else()
    message(STATUS "[ultrafast-feed] Doxygen not found — 'docs' target disabled")
endif()
