# Core library: strong ids, revisions, results, diagnostics.
add_library(pf_core STATIC
    core/Diagnostic.cpp
    core/IdGenerator.cpp
    core/Json.cpp
)

target_include_directories(pf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pf_core PUBLIC cxx_std_20)
target_link_libraries(pf_core PUBLIC Threads::Threads)

# Math: math-expression core types, LaTeX input boundary and generator.
add_library(pf_math STATIC
    math/MathGenerator.cpp
    math/MathValidator.cpp
)
target_link_libraries(pf_math PUBLIC pf_core)

# Document model
add_library(pf_document STATIC
    document/Document.cpp
    document/DocumentEditor.cpp
    document/DocumentIndex.cpp
    document/DocumentTraversal.cpp
    document/InlineText.cpp
)
target_link_libraries(pf_document PUBLIC pf_core pf_math)

# Editing protocol
add_library(pf_editing STATIC
    editing/EditingSystem.cpp
    editing/UndoHistory.cpp
    editing/AnchorResolver.cpp
)
target_link_libraries(pf_editing PUBLIC pf_document)

# Template registry
add_library(pf_template STATIC
    template/TemplateRegistry.cpp
)
target_link_libraries(pf_template PUBLIC pf_document)

# Assets
add_library(pf_asset STATIC
    asset/AssetManager.cpp
)
target_link_libraries(pf_asset PUBLIC pf_core)

# Bibliography (BibTeX parsing + search)
add_library(pf_bibliography STATIC
    bibliography/BibliographyService.cpp
)
target_link_libraries(pf_bibliography PUBLIC pf_core)

# Citation numbering policy (citation plan §3): citation key -> display number
# for the whole document. Sits above document + bibliography; the GUI renders
# its pills from here so the editor shows exactly the numbers citation-order
# BibTeX styles (unsrtnat / IEEEtran) will print in the PDF.
add_library(pf_numbering STATIC
    numbering/CitationNumberResolver.cpp
)
target_link_libraries(pf_numbering PUBLIC pf_document pf_bibliography)

# Validation
add_library(pf_validation STATIC
    validation/Validator.cpp
)
target_link_libraries(pf_validation PUBLIC pf_document pf_template)

# Rendering: semantic document -> LaTeX
add_library(pf_render STATIC
    render/LatexRenderer.cpp
    render/SourceMap.cpp
)
target_link_libraries(pf_render PUBLIC pf_document pf_template pf_validation)

# Build pipeline
add_library(pf_build STATIC
    build/BuildEvent.cpp
    build/Compiler.cpp
    build/CompilerFactory.cpp
    build/DiagnosticMapper.cpp
    build/BuildCoordinator.cpp
    build/RuntimeManager.cpp
)
target_link_libraries(pf_build PUBLIC pf_render)

# Persistence
add_library(pf_persistence STATIC
    persistence/ProjectMigrator.cpp
    persistence/ProjectPersistence.cpp
    persistence/SaveCoordinator.cpp
)
target_link_libraries(pf_persistence PUBLIC pf_document pf_asset pf_bibliography)

# Application session
add_library(pf_app STATIC
    project/ApplicationEvent.cpp
    project/PreviewGate.cpp
    project/ProjectState.cpp
    project/SnapshotFactory.cpp
    project/ProjectSession.cpp
)
target_link_libraries(pf_app PUBLIC pf_build pf_persistence pf_editing pf_numbering)

# CLI executable
add_executable(paperforge
    cli/Main.cpp
    cli/Commands.cpp
)
target_link_libraries(paperforge PRIVATE pf_app)
target_compile_definitions(paperforge PRIVATE PF_TECTONIC="${TECTONIC_BIN}")

# Qt6 GUI (optional; enabled when Qt6 is available)
find_package(Qt6 QUIET COMPONENTS Widgets Test)
if(Qt6_FOUND)
    message(STATUS "Qt6 found - building PaperForge GUI")
    add_subdirectory(app)
else()
    message(STATUS "Qt6 NOT found - GUI skipped (core library + CLI only)")
endif()
