cmake_minimum_required(VERSION 3.21)
project(ChorusCut VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Gui Quick Qml QuickControls2 Sql Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavutil)
add_library(chorus_editor STATIC src/lyrics.cpp src/editor.cpp src/editor.h src/xml.cpp src/project.cpp
            src/waveform.cpp src/waveform.h src/library.cpp src/library.h)
target_include_directories(chorus_editor PUBLIC src)
target_link_libraries(chorus_editor PUBLIC Qt6::Quick Qt6::Sql PkgConfig::MLT PRIVATE Qt6::Concurrent PkgConfig::FFMPEG)
set(CHORUS_ALIGNMENT_PYTHON "python3" CACHE STRING "Python with torch and torchaudio for local lyric alignment")
target_compile_definitions(chorus_editor PRIVATE CHORUS_ALIGNMENT_PYTHON="${CHORUS_ALIGNMENT_PYTHON}")
set_source_files_properties(alignment/align.py PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
qt_add_executable(ChorusCut MACOSX_BUNDLE src/main.cpp alignment/align.py)
qt_add_resources(ChorusCut ui PREFIX "/" FILES src/Main.qml src/Timeline.qml src/Inspector.qml src/ProjectManager.qml)
target_link_libraries(ChorusCut PRIVATE chorus_editor Qt6::QuickControls2)
set_target_properties(ChorusCut PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER org.choruscut.prototype)

# Uses the same editor commands as QML. Build explicitly for the import regression.
qt_add_executable(ChorusCutImportCheck EXCLUDE_FROM_ALL tests/import_check.cpp)
target_link_libraries(ChorusCutImportCheck PRIVATE chorus_editor)

qt_add_executable(ChorusCutLyricsCheck MACOSX_BUNDLE EXCLUDE_FROM_ALL tests/lyrics_check.cpp alignment/align.py)
qt_add_resources(ChorusCutLyricsCheck lyrics_ui PREFIX "/" FILES src/Main.qml src/Timeline.qml src/Inspector.qml src/ProjectManager.qml)
target_link_libraries(ChorusCutLyricsCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Real mouse/keyboard checks of the timeline. Build explicitly; see docs/TESTING.md.

qt_add_executable(ChorusCutTimelineCheck EXCLUDE_FROM_ALL tests/timeline_check.cpp)
qt_add_resources(ChorusCutTimelineCheck timeline_ui PREFIX "/" FILES src/Main.qml src/Timeline.qml src/Inspector.qml src/ProjectManager.qml)
target_link_libraries(ChorusCutTimelineCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)

# Real mouse/keyboard checks of the Project Manager. Build explicitly; see docs/TESTING.md.
qt_add_executable(ChorusCutProjectsCheck EXCLUDE_FROM_ALL tests/projects_check.cpp)
qt_add_resources(ChorusCutProjectsCheck projects_ui PREFIX "/" FILES src/Main.qml src/Timeline.qml src/Inspector.qml src/ProjectManager.qml)
target_link_libraries(ChorusCutProjectsCheck PRIVATE chorus_editor Qt6::QuickControls2 Qt6::Test)
