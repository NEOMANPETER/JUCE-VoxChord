# VoxChord (v0.1)

VST3 plugin for FL Studio. Hum or sing a melody -> it finds the notes and key -> suggests chords -> plays both as MIDI in sync with your project. Works fully offline.

Build: push to GitHub, then open the Actions tab and download the artifact "VoxChord-VST3-Windows".
Source layout: Source/Analysis.h (pitch/notes/key/chords), Source/PluginProcessor.* (audio+MIDI), Source/PluginEditor.* (screen).
Tests of the analysis engine: g++ -std=c++17 tests/test_analysis.cpp -o t && ./t   (also tests/test_realistic.cpp)
