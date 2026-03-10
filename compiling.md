# Compiling the Ensoniq® SD-1 32 voices VST emulation

# Prequisites
You have to compile a special MAME 0.286. You have to modify 2 some files first:
- a stripped down version of: mame/src/frontend/mame/ui/ui.cpp
- modified version of device declaration file beacuse we must tell Mame tht our synth is working:/mame/src/mame/ensoniq/esq5505.cpp
Compile with SOURCES=src/mame/ensoniq/esq5505.cpp USE_BGFX=0
