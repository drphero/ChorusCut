cmake_minimum_required(VERSION 3.21)
project(ChorusCut VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Gui Network Quick Qml QuickControls2 Sql Test WebSockets)
find_package(Qt6Keychain REQUIRED)  # ComfyUI access tokens in the system keychain
find_package(PkgConfig REQUIRED)
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavutil)
add_library(chorus_editor STATIC src/lyrics.cpp src/editor.cpp src/editor.h src/xml.cpp src/project.cpp
            src/waveform.cpp src/waveform.h src/library.cpp src/library.h src/comfy.cpp src/comfy.h src/plan.cpp src/plan.h
            src/agent.cpp src/agent.h src/agent_tools.cpp src/agent_tools.h
            src/workflows.cpp src/workflows.h src/generate.cpp src/generate.h)
target_include_directories(chorus_editor PUBLIC src)
target_link_libraries(chorus_editor PUBLIC Qt6::Quick Qt6::Sql Qt6::Network Qt6::WebSockets PkgConfig::MLT PRIVATE Qt6::Concurrent PkgConfig::FFMPEG qt6keychain)
set(CHORUS_ALIGNMENT_PYTHON "python3" CACHE STRING "Python with torch and torchaudio for local lyric alignment")
target_compile_definitions(chorus_editor PRIVATE CHORUS_ALIGNMENT_PYTHON="${CHORUS_ALIGNMENT_PYTHON}")
set_source_files_properties(alignment/align.py PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set(CHORUS_QML src/Main.qml src/Timeline.qml src/Inspector.qml src/ProjectManager.qml src/PlanWorkspace.qml
               src/PlanField.qml src/PlanText.qml src/PlanCombo.qml src/Checks.qml src/ReferenceStrip.qml
               src/LinksPanel.qml src/LinkChip.qml src/WorkflowsSheet.qml src/PromptEditor.qml)
qt_add_executable(ChorusCut MACOSX_BUNDLE src/main.cpp alignment/align.py)
qt_add_resources(ChorusCut ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCut PRIVATE chorus_editor Qt6::QuickControls2)

# The stdio MCP server agents launch; it lives inside the app bundle beside the app.
qt_add_executable(choruscut-mcp src/mcp_shim.cpp src/agent_tools.cpp)
target_link_libraries(choruscut-mcp PRIVATE Qt6::Core Qt6::Network)
set_target_properties(choruscut-mcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ChorusCut.app/Contents/MacOS")
add_dependencies(ChorusCut choruscut-mcp)
set_target_properties(ChorusCut PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER org.choruscut.prototype)

# Uses the same editor commands as QML. Build explicitly for the import regression.
qt_add_executable(ChorusCutImportCheck EXCLUDE_FROM_ALL tests/import_check.cpp)
target_link_libraries(ChorusCutImportCheck PRIVATE chorus_editor)

qt_add_executable(ChorusCutLyricsCheck MACOSX_BUNDLE EXCLUDE_FROM_ALL tests/lyrics_check.cpp alignment/align.py)
qt_add_resources(ChorusCutLyricsCheck lyrics_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutLyricsCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Real mouse/keyboard checks of the timeline. Build explicitly; see docs/TESTING.md.

qt_add_executable(ChorusCutTimelineCheck EXCLUDE_FROM_ALL tests/timeline_check.cpp)
qt_add_resources(ChorusCutTimelineCheck timeline_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutTimelineCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Real mouse/keyboard checks of the Project Manager. Build explicitly; see docs/TESTING.md.
qt_add_executable(ChorusCutProjectsCheck EXCLUDE_FROM_ALL tests/projects_check.cpp)
qt_add_resources(ChorusCutProjectsCheck projects_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutProjectsCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# ComfyUI job handling against tests/comfy_stub.py, with real input. Build explicitly; see docs/TESTING.md.
qt_add_executable(ChorusCutComfyCheck EXCLUDE_FROM_ALL tests/comfy_check.cpp)
qt_add_resources(ChorusCutComfyCheck comfy_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutComfyCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Workflow adapters and reference generation against the ComfyUI stub, with real input. Build explicitly; see docs/TESTING.md.
qt_add_executable(ChorusCutGenerateCheck EXCLUDE_FROM_ALL tests/generate_check.cpp)
qt_add_resources(ChorusCutGenerateCheck generate_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutGenerateCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Planning commands, Plan workspace, shot cards and checks with real input. Build explicitly; see docs/TESTING.md.
qt_add_executable(ChorusCutPlanCheck EXCLUDE_FROM_ALL tests/plan_check.cpp)
qt_add_resources(ChorusCutPlanCheck plan_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutPlanCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Agent control through the real choruscut-mcp over stdio, with real input. Build explicitly; see docs/TESTING.md.
qt_add_executable(ChorusCutMcpCheck EXCLUDE_FROM_ALL tests/mcp_check.cpp)
qt_add_resources(ChorusCutMcpCheck mcp_ui PREFIX "/" FILES ${CHORUS_QML})
target_link_libraries(ChorusCutMcpCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)
add_dependencies(ChorusCutMcpCheck choruscut-mcp)
