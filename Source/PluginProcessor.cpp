/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <iostream>
#include <chrono>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdlib.h>
#include <zlib.h> // MAME uses standard zlib
#include <new>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

// ==============================================================================
// MANDATORY MAME MACROS - MUST BE DEFINED BEFORE ANY MAME INCLUDES!
// ==============================================================================
#define PTR64 1
#define LSB_FIRST 1
#define NDEBUG 1
#define __STDC_LIMIT_MACROS 1
#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1

// --- MAME Core Includes ---
#include "emu.h"
#include "esqpanel.h"
#include "frontend/mame/ui/ui.h"
#include "osd/modules/lib/osdobj_common.h"
#include "uiinput.h"
#include "inputdev.h"
#include "emuopts.h"
#include "render.h"
#include "osdepend.h"
#include "frontend/mame/mame.h"
#include "frontend/mame/clifront.h"
#include "frontend/mame/mameopts.h"
#include "drivenum.h"
#include "diimage.h"

#include <iomanip>
#include <fstream>

// MAME Versioning stubs required by the linker
extern const char bare_build_version[] = "0.287";
extern const char bare_vcs_revision[] = "";
extern const char build_version[] = "0.287";

const char * emulator_info::get_appname() { return "mame"; }
const char * emulator_info::get_appname_lower() { return "mame"; }
const char * emulator_info::get_configname() { return "mame"; }
const char * emulator_info::get_copyright() { return "Copyright"; }
const char * emulator_info::get_copyright_info() { return "Copyright"; }

// ==============================================================================
// AUDIO STREAMING & THROTTLING
// ==============================================================================
void EnsoniqSD1AudioProcessor::pushAudioFromMame(const int16_t* pcmBuffer, int numSamples) {
    if (!isMameRunningFlag()) return;

    // AUDIO-DRIVEN THROTTLING:
    // MAME runs on a separate thread and can generate audio much faster than real-time.
    // We throttle the MAME thread here by putting it to sleep if our ring buffer has 
    // more unread samples than the defined threshold. This prevents buffer overflows.
    
        while (isMameRunningFlag()) {
            
                        // --- DEADLOCK BREAKER ---

                        if (requestMameSave.load(std::memory_order_acquire) || requestMameLoad.load(std::memory_order_acquire)) {
                            break;
                        }
                        
                        // --- ANCHOR DEADLOCK BREAKER ---

                        if (needAnchorSync.load(std::memory_order_acquire)) {
                            break;
                        }
            
            uint64_t writePos = getTotalWritten();
            uint64_t readPos = getTotalRead();
            int64_t available = writePos - readPos;

            int maxAllowedBuffer = mameBufferThreshold.load(std::memory_order_relaxed);
            
            if (isNonRealtime()) {
                maxAllowedBuffer = maxOfflineBuffer.load(std::memory_order_relaxed);
            }
                               
            if (available < maxAllowedBuffer) {
                break; // There is enough room, write
            }

            // Buffer is full. Sleep the MAME thread until processBlock consumes some data.
            
// --- OPTIMIZATION START ---
#ifdef _WIN32
        // Default Windows scheduler resolution is ~15.6ms. Waiting for 5ms might 
        // put the thread to sleep for 16ms, causing severe buffer underruns in MAME.
        // We force a much tighter wake-up interval on Windows.
            mameThrottleEvent.wait(1);
#else
        // Original macOS behavior preserved
            mameThrottleEvent.wait(5);
#endif
            // --- OPTIMIZATION END ---
        }
        
    uint64_t currentWritePos = totalWritten.load(std::memory_order_relaxed);
        
    if (needAnchorSync.load(std::memory_order_acquire) && mameMachine != nullptr) {
            anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
            anchorDawSample.store(currentWritePos, std::memory_order_relaxed);
            needAnchorSync.store(false, std::memory_order_release);
        }
        
    // MAME outputs interleaved audio. STRIDE = 5 (Main L, Main R, Aux L, Aux R, Floppy)
    for (int i = 0; i < numSamples; ++i) {

                int index = currentWritePos & (RING_BUFFER_SIZE - 1);

                ringBufferL[index] = pcmBuffer[i * 5 + 0] / 32768.0f;
                ringBufferR[index] = pcmBuffer[i * 5 + 1] / 32768.0f;

                ringBufferAuxL[index] = pcmBuffer[i * 5 + 2] / 32768.0f;
                ringBufferAuxR[index] = pcmBuffer[i * 5 + 3] / 32768.0f;

        currentWritePos++;
    }
    
    totalWritten.store(currentWritePos, std::memory_order_release);
    
    if (needAnchorSync.load(std::memory_order_acquire) && mameMachine != nullptr) {
        anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
        anchorDawSample.store(currentWritePos, std::memory_order_relaxed);
        needAnchorSync.store(false, std::memory_order_release);
    }

}
    
// ==============================================================================
// VST MIDI PORT IMPLEMENTATION (TIMESTAMPED)
// ==============================================================================
class VstMidiInputPort : public osd::midi_input_port {
private:
    EnsoniqSD1AudioProcessor* processor;
public:
    VstMidiInputPort(EnsoniqSD1AudioProcessor* p) : processor(p) {}
    virtual ~VstMidiInputPort() {}
    
    virtual bool poll() override {
        return processor->pollMidiData();
    }
    
    virtual int read(uint8_t *pOut) override {
        if (processor->pollMidiData()) {
            *pOut = static_cast<uint8_t>(processor->readMidiByte());
            return 1;
        }
        return 0;
    }
};

// ==============================================================================
// VST MIDI OUTPUT PORT (SD-1 DUART TX → JUCE MIDI Out)
// ==============================================================================
class VstMidiOutputPort : public osd::midi_output_port {
private:
    EnsoniqSD1AudioProcessor* processor;
public:
    VstMidiOutputPort(EnsoniqSD1AudioProcessor* p) : processor(p) {}
    virtual ~VstMidiOutputPort() {}
    
    virtual void write(uint8_t data) override {
        processor->pushMidiOutByte(data);
    }
};

// ==============================================================================
// MIDI OUTPUT RING BUFFER
// ==============================================================================
void EnsoniqSD1AudioProcessor::pushMidiOutByte(uint8_t data) {
    int currentWrite = midiOutWritePos.load(std::memory_order_relaxed);
    int nextWrite = (currentWrite + 1) & (MIDI_OUT_BUFFER_SIZE - 1);
    if (nextWrite != midiOutReadPos.load(std::memory_order_acquire)) {
        midiOutBuffer[currentWrite] = data;
        midiOutWritePos.store(nextWrite, std::memory_order_release);
    }
}

// ==============================================================================
// TIMESTAMPED MIDI QUEUE LOGIC (PURE ANCHOR)
// ==============================================================================
void EnsoniqSD1AudioProcessor::pushMidiByte(uint8_t data, double targetMameTime) {
    int currentWrite = midiWritePos.load(std::memory_order_relaxed);

    // --- OPTIMIZATION ---
    // Fast wrap-around for MIDI ring buffer
    int nextWrite = (currentWrite + 1) & (MIDI_BUFFER_SIZE - 1);

    if (nextWrite != midiReadPos.load(std::memory_order_acquire)) {
        midiBuffer[currentWrite].data = data;
        midiBuffer[currentWrite].targetMameTime = targetMameTime;
        midiWritePos.store(nextWrite, std::memory_order_release);
    }
}

// ========================================================
// FORCE COMPARE OFF
// ========================================================
void EnsoniqSD1AudioProcessor::forceCompareOff()
{
    if (!mameMachine) return;
    
    // Check if we are in the File Manager (using the saved state struct)
    if (fileManagerState.visible || requestedViewIndex.load() == 4)
        {
            juce::String cat = fileManagerState.category;
            
            //CHECK IF INTERNAL (INT, ROM, CART)
            bool isInternal = (cat == "INT (RAM)") ||
                                      (cat == "ROM0") ||
                                      (cat == "ROM1") ||
                                      (cat == "CART");

            // IF EXTERNAL THEN EXIT
            if (!isInternal && cat.isNotEmpty())
            {
                return;
            }
        }
    
    auto* osram_share = mameMachine->root_device().memshare("osram");
    if (!osram_share || osram_share->bytes() <= 0x92DC) return;
    
    uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
    if (osram[0x92DC] != 0x00)
        {
            double now = mameMachine->time().as_double();
            
            // No incoming DAW MIDI
            suppressMidiInput.store(true, std::memory_order_release);
            pushMidiByte(0xF7, now + 0.01);
            
            // Helper lambda to send a properly nibblized Virtual Button press
            auto sendBtn = [&](int buttonNum, double downTime, double upTime) {
                        
                        // Button DOWN: F0 0F 05 00 00 00 00 00 [hi lo] F7
                        uint8_t downMsg[11] = {
                            0xF0, 0x0F, 0x05, 0x00, 0x00, 0x00,  // header (ch=0, msgType=0x00)
                            0x00, 0x00,                          // command type 0x00 nibblized
                            static_cast<uint8_t>((buttonNum >> 4) & 0x0F),
                            static_cast<uint8_t>(buttonNum & 0x0F),
                            0xF7
                        };
                        for (int i = 0; i < 11; ++i) {
                            pushMidiByte(downMsg[i], downTime + i * 0.00035);
                        }
                        
                        // Button UP: buttonNum + 96
                        int upNum = buttonNum + 96;
                        uint8_t upMsg[11] = {
                            0xF0, 0x0F, 0x05, 0x00, 0x00, 0x00,
                            0x00, 0x00,
                            static_cast<uint8_t>((upNum >> 4) & 0x0F),
                            static_cast<uint8_t>(upNum & 0x0F),
                            0xF7
                        };
                        for (int i = 0; i < 11; ++i) {
                            pushMidiByte(upMsg[i], upTime + i * 0.00035);
                        }
                    };
                    
            // Compare (63) — toggles compare OFF
            sendBtn(63, now + 0.5, now + 0.8);

            // Async turn on MIDI input after — unless a file manager close cancelled it first
            juce::Timer::callAfterDelay(1000, [this]() {
                if (!midiOpCancelled.load(std::memory_order_acquire))
                    suppressMidiInput.store(false, std::memory_order_release);
            });
        }
}

void EnsoniqSD1AudioProcessor::clearMidiBuffer() {
    // Clear MIDI buffer
    midiReadPos.store(midiWritePos.load(std::memory_order_acquire), std::memory_order_release);
}

bool EnsoniqSD1AudioProcessor::pollMidiData() {
    if (mameMachine == nullptr) return false;
    
    int currentRead = midiReadPos.load(std::memory_order_relaxed);
    if (currentRead == midiWritePos.load(std::memory_order_acquire)) return false;
    
    // The MAME processor accurately waits for the calculated microsecond!
    return mameMachine->time().as_double() >= midiBuffer[currentRead].targetMameTime;
}

int EnsoniqSD1AudioProcessor::readMidiByte() {
    if (mameMachine == nullptr) return 0;

    int currentRead = midiReadPos.load(std::memory_order_relaxed);
    int currentWrite = midiWritePos.load(std::memory_order_acquire);
    if (currentRead == currentWrite) return 0;

    if (mameMachine->time().as_double() >= midiBuffer[currentRead].targetMameTime) {
        uint8_t data = midiBuffer[currentRead].data;

        // Fast wrap-around using bitwise AND
        int nextRead = (currentRead + 1) & (MIDI_BUFFER_SIZE - 1);
        midiReadPos.store(nextRead, std::memory_order_release);

        return data;
    }
    return 0;
}

// ==============================================================================
// HEADLESS OSD (Operating System Dependent) INTERFACE
// This class acts as the bridge between MAME's core and the JUCE environment.
// ==============================================================================
class VstOsdInterface : public osd_common_t
{
private:
    EnsoniqSD1AudioProcessor* processor;
    running_machine* mame_machine = nullptr;
    uint64_t lastFrameHash = 0;
    uint32_t lastMouseButtons = 0;
    int audioMacroHeldBank = -1;   // audio-thread-only: which bank is electronically held
    
    render_target* main_target = nullptr;

    int saveFrameDelay = 0;
    int loadFrameDelay = 0;
    int frameSkipCounter = 0;
    int bootGraceFrames = 0;
    
public:
    
    virtual void process_events() override {}
    virtual bool has_focus() const override { return true; }
    
    VstOsdInterface(EnsoniqSD1AudioProcessor* p, osd_options &options)
    : osd_common_t(options), processor(p) {}
    
    virtual ~VstOsdInterface() {}
    
    virtual void init(running_machine &machine) override {
        
        // REQUIRED: Initializes MAME's core sound, mouse, and keyboard modules
        osd_common_t::init(machine);
              
        mame_machine = &machine;
        processor->mameMachine = &machine;
        
        // --- HOOK INTO MAME OUTPUTS (VFD & LEDs) ---
        machine.output().set_global_notifier(EnsoniqSD1AudioProcessor::mameOutputNotifier, processor);
                        
        int targetViewIdx = processor->requestedViewIndex.load(std::memory_order_acquire);
        int startW = processor->windowWidth.load(std::memory_order_acquire);
        int startH = processor->windowHeight.load(std::memory_order_acquire);

        if (startW <= 0) startW = 1200;
        if (startH <= 0) startH = 539;

        // SINGLE ALLOCATION with exact dimensions
        main_target = machine.render().target_alloc();
        main_target->set_bounds(startW, startH);

        int mameViewMap[4] = { 1, 0, 2, 3 };
        main_target->set_view(mameViewMap[targetViewIdx % 4]);
        
    };
        
    virtual void osd_exit() override {
        if (mame_machine != nullptr && main_target != nullptr) {
            // Gracefully return the render target to MAME to avoid crashes during cleanup
            mame_machine->render().target_free(main_target);
            main_target = nullptr;
        }
        if (processor != nullptr) {
                processor->mameMachine = nullptr;
            }
        osd_common_t::osd_exit();
    }
    
    virtual void update(bool skip_redraw) override {
                
        if (mame_machine == nullptr) return;
        
            // Set the flag on the very first frame update.
            // This confirms that MAME device clocks are initialized and safe for time calculations.
            if (!processor->mameIsFullyBooted.load(std::memory_order_relaxed)) {
                processor->mameIsFullyBooted.store(true, std::memory_order_release);
            }
        
        if (!processor->isMameRunningFlag()) {
                mame_machine->schedule_exit();
                return;
            }
                
                // ==============================================================================
                // --- THE PERFECT SILENT WATCHER (DIRECT MEMSHARE, CRASH-PROOF) ---
                // ==============================================================================
                // Bypassing the CPU API (which causes Access Violation on the video thread).
                // Instead, we write directly to the raw C++ memory array of "osram"!
                if (mame_machine != nullptr) {
                    auto* osram_share = mame_machine->root_device().memshare("osram");
                    
                    // Only proceed if RAM exists and has been allocated
                    if (osram_share != nullptr && osram_share->bytes() >= 0xCE0C) {
                        uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
                        
                        // The CPU writes to 0xFFCE0B. In the Little-Endian array, this is 0xCE0A or 0xCE0B.
                        // To be safe and prevent freezes, we keep both locked at 0x01!
                        // No thread collision, no CPU calls, just raw byte overwriting.
                        if (osram[0xCE0A] != 0x01) osram[0xCE0A] = 0x01;
                        if (osram[0xCE0B] != 0x01) osram[0xCE0B] = 0x01;
                    }
                }
        
        if (skip_redraw) return;
                
        // --- DYNAMIC VIEW & RESIZE SWITCHING ---
        bool viewChanged = processor->requestViewChange.exchange(false, std::memory_order_acquire);
        bool sizeChanged = processor->requestRenderResize.exchange(false, std::memory_order_acquire);

                if (viewChanged || sizeChanged) {
                    int targetViewIdx = processor->requestedViewIndex.load(std::memory_order_acquire);
                    int newW = processor->windowWidth.load(std::memory_order_acquire);
                    int newH = processor->windowHeight.load(std::memory_order_acquire);

                    /*if (newW > 0 && newH > 0) {
                        if (viewChanged) {
                            int mameViewMap[4] = { 1, 0, 2, 3 };
                            main_target->set_view(mameViewMap[targetViewIdx % 4]);
                        }
                        main_target->set_bounds(newW, newH);
                    }*/
                    // debug rack
#ifdef SD1_DEBUG_RACK_PANEL
                    if (newW > 0 && newH > 0) {
                                            if (viewChanged) {
                                                int mameViewMap[4] = { 1, 0, 2, 3 };
                                                int effectiveView = (targetViewIdx == 4) ? 2 : (targetViewIdx % 4);
                                                main_target->set_view(mameViewMap[effectiveView]);
                                            }
                                            main_target->set_bounds(newW, newH);
                                        }
#else
                    if (newW > 0 && newH > 0) {
                        if (viewChanged) {
                            int mameViewMap[4] = { 1, 0, 2, 3 };
                            main_target->set_view(mameViewMap[targetViewIdx % 4]);
                        }
                        main_target->set_bounds(newW, newH);
                    }
#endif // SD1_DEBUG_RACK_PANEL
                }
                        
        // --- SYNCHRONIZED MAME STATE SAVING ---
        if (processor->requestMameSave.load(std::memory_order_acquire)) {
        if (saveFrameDelay == 0) { // Only call it the first time
            mame_machine->schedule_save("vst_temp");
            saveFrameDelay = 3;
            }
        }

                if (saveFrameDelay > 0) {
                    saveFrameDelay--;
                    if (saveFrameDelay == 0) {
                        processor->requestMameSave.store(false, std::memory_order_release); // Clear the flag here!
                        processor->mameStateIsReady.store(true, std::memory_order_release);
                        processor->mameStateEvent.signal();
                    }
                }

        // --- SYNCHRONIZED MAME STATE LOADING ---
        if (processor->requestMameLoad.load(std::memory_order_acquire)) {
            if (loadFrameDelay == 0) { // Only call it the first time
                mame_machine->schedule_load("vst_temp");
                loadFrameDelay = 3;
                }
        }

                if (loadFrameDelay > 0) {
                    loadFrameDelay--;
                    if (loadFrameDelay == 0) {
                        processor->requestMameLoad.store(false, std::memory_order_release); // Clear the flag here!
                        processor->mameStateIsReady.store(true, std::memory_order_release);
                        processor->mameStateEvent.signal();
                    }
                }
        
        // --- FLOPPY MOUNTING ---
        if (processor->requestFloppyLoad.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(processor->mediaMutex);
                    
                    for (device_image_interface &image : image_interface_enumerator(mame_machine->root_device())) {
                        if (image.brief_instance_name() == "flop" || image.brief_instance_name() == "floppydisk") {
                            if (processor->pendingFloppyPath.empty()) {
                                image.unload(); // EJECT!
                            } else {
                                image.load(processor->pendingFloppyPath);
                            }
                            break;
                        }
                    }
        }

        // --- CARTRIDGE MOUNTING ---
        if (processor->requestCartLoad.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(processor->mediaMutex);
                    
                    for (device_image_interface &image : image_interface_enumerator(mame_machine->root_device())) {
                        if (image.brief_instance_name() == "cart" || image.brief_instance_name() == "cartridge") {
                            if (processor->pendingCartPath.empty()) {
                                image.unload(); // EJECT!
                            } else {
                                image.load(processor->pendingCartPath);
                            }
                            break;
                        }
                    }
        }
        
        // ==============================================================================
        // --- ASYNCHRONOUS RAM INJECTION & WARM BOOT ---
        // ==============================================================================
        
        if (processor->pendingRamInjection.exchange(false, std::memory_order_acquire)) {
                    
                    auto* osram_share = mame_machine->root_device().memshare("osram");
                    auto* seqram_share = mame_machine->root_device().memshare("seqram");

                    // 1. Inject the OS RAM
                    if (osram_share != nullptr && processor->pendingOsram.getSize() == osram_share->bytes()) {
                            std::memcpy(osram_share->ptr(), processor->pendingOsram.getData(), osram_share->bytes());
                    }
             
                    // 2. Load the Sequencer RAM
                    if (seqram_share != nullptr && processor->pendingSeqRam.getSize() == seqram_share->bytes()) {
                        std::memcpy(seqram_share->ptr(), processor->pendingSeqRam.getData(), seqram_share->bytes());
                    }

                    // 3. Reset the CPU so the SD-1 OS re-evaluates the fresh RAM, RESET SYS-EX in memory
                    device_t* cpu = mame_machine->root_device().subdevice("maincpu");
                    if (cpu != nullptr) {
                            cpu->memory().space(AS_PROGRAM).write_byte(0xFFCE0B, 0x01);
                            cpu->reset();
                    }

                    // 4. Free memory
                    processor->pendingOsram.setSize(0);
                    processor->pendingSeqRam.setSize(0);
                    processor->panicDelaySamples.store(static_cast<int>(processor->getHostSampleRate() * 0.5), std::memory_order_release);
                    
                }
        
        // ==============================================================================
        // --- BANK INJECTION (60-program bank into osram, NO CPU reset) ---
        // ==============================================================================
        // Writes interleaved program data directly into the bank area of osram.
        // Unlike full RAM injection, this does NOT reset the CPU — the firmware
        // picks up the new data when the user presses a Bank button.
        
        if (processor->pendingBankInjection.exchange(false, std::memory_order_acquire)) {
                    auto* osram_share = mame_machine->root_device().memshare("osram");
                    if (osram_share != nullptr && processor->pendingBankData.getSize() > 0) {
                        uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
                        const uint8_t* src = static_cast<const uint8_t*>(processor->pendingBankData.getData());
                        size_t len = processor->pendingBankData.getSize();
                        
                        // Motorola 68000 big-endian → LE host byte swap: address ^ 1
                        for (size_t i = 0; i < len; ++i) {
                            osram[(0x0003C8 + i) ^ 1] = src[i];
                        }
                    }
                    processor->pendingBankData.setSize(0);
                }
        
        // Disable on-screen popups (e.g., "State loaded")
        mame_machine->ui().popup_time(0, " ");
        
        // --- FRAME SKIPPING
        frameSkipCounter++;
        if (frameSkipCounter % 2 != 0) {
            return;
        }

        render_target *target = main_target;
        if (target == nullptr) return;
        
        render_primitive_list &prims = target->get_primitives();
        prims.acquire_lock();
        
            // =========================================================================
            // OPTIMIZATION: DIRTY FRAME DETECTION (PRIMITIVE HASHING)
            // =========================================================================
            // Calculate a blazing fast FNV-1a hash of the current MAME UI elements.
            uint64_t currentHash = 14695981039346656037ULL;
            
            for (render_primitive *prim = prims.first(); prim != nullptr; prim = prim->next()) {
                
                // --- NO MOUSE CURSOR (HASH) ---
                bool isMouseCursor = (prim->next() == nullptr && prim->type == render_primitive::QUAD &&
                                      (prim->bounds.x1 - prim->bounds.x0) < 64.0f &&
                                      (prim->bounds.y1 - prim->bounds.y0) < 64.0f);
                
                if (isMouseCursor) continue; 

                auto mix = [&currentHash](const void* data, size_t len) {
                    const uint8_t* p = static_cast<const uint8_t*>(data);
                    for (size_t i = 0; i < len; ++i) {
                        currentHash ^= p[i];
                        currentHash *= 1099511628211ULL;
                    }
                };
                mix(&prim->type, sizeof(prim->type));
                mix(&prim->bounds, sizeof(prim->bounds));
                mix(&prim->color, sizeof(prim->color));
                mix(&prim->texture.base, sizeof(prim->texture.base));
            }

                        // If nothing visually changed, and the user didn't resize the VST window,
                        // ABORT the expensive pixel rendering completely!
                        if (currentHash == lastFrameHash && !viewChanged && !sizeChanged) {
                            prims.release_lock();
                            return; // -> GUI CPU USAGE DROPS
                        }
                        
                        lastFrameHash = currentHash;
                        // =========================================================================
        
        // --- DOUBLE BUFFERED RENDERING ---
        // Draw into the buffer that is currently NOT being read by the JUCE GUI thread
        int writeIndex = 1 - processor->readyBufferIndex.load(std::memory_order_acquire);
        juce::Graphics g(processor->screenBuffers[writeIndex]);
        g.fillAll(juce::Colours::black);
        
        for (render_primitive *prim = prims.first(); prim != nullptr; prim = prim->next()) {
                
                // --- FILTER CURSOR ---
                bool isMouseCursor = (prim->next() == nullptr && prim->type == render_primitive::QUAD &&
                                      (prim->bounds.x1 - prim->bounds.x0) < 64.0f &&
                                      (prim->bounds.y1 - prim->bounds.y0) < 64.0f);
                
                if (isMouseCursor) continue; // NO CURSOR

                juce::Rectangle<float> rect(
                                            prim->bounds.x0, prim->bounds.y0,
                                            prim->bounds.x1 - prim->bounds.x0,
                                            prim->bounds.y1 - prim->bounds.y0
                                            );
                    
            if (prim->type == render_primitive::QUAD) {
                if (prim->texture.base != nullptr) {
                    if (prim->texture.width > processor->cachedTexture.getWidth() ||
                        prim->texture.height > processor->cachedTexture.getHeight()) {
                        continue; // Failsafe to prevent out-of-bounds rendering
                    }
                    
                    uint32_t format = PRIMFLAG_GET_TEXFORMAT(prim->flags);
                    
                    // Pre-calculate fixed color multipliers for fast pixel processing
                    const uint32_t rT = (uint32_t)(prim->color.r * 255.0f);
                    const uint32_t gT = (uint32_t)(prim->color.g * 255.0f);
                    const uint32_t bT = (uint32_t)(prim->color.b * 255.0f);
                    const uint32_t aT = (uint32_t)(prim->color.a * 255.0f);
                    
                    const int width = prim->texture.width;
                    const int height = prim->texture.height;
                    const uint32_t srcPitch = prim->texture.rowpixels; 
                    
                    {
                        juce::Image::BitmapData texData(processor->cachedTexture, juce::Image::BitmapData::writeOnly);
                        
                        // 1. ARGB32 MODE (Each pixel has its own Alpha channel)
                        if (format == TEXFORMAT_ARGB32) {
                            for (int y = 0; y < height; ++y) {

                                const uint32_t* __restrict srcRow = static_cast<const uint32_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* __restrict dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));

                                for (int x = 0; x < width; ++x) {
                                    uint32_t p = srcRow[x];
                                    uint32_t a = (p >> 24);
                                    uint32_t r = (p >> 16) & 0xff;
                                    uint32_t g = (p >> 8) & 0xff;
                                    uint32_t b = p & 0xff;

                                    a = (a * aT) >> 8;
                                    r = (((r * rT) >> 8) * a) >> 8;
                                    g = (((g * gT) >> 8) * a) >> 8;
                                    b = (((b * bT) >> 8) * a) >> 8;

                                    dstRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                        // 2. RGB32 MODE (Alpha is fixed to 255 - Massive optimization!)
                        else if (format == TEXFORMAT_RGB32) {
                            const uint32_t finalA = (255 * aT) >> 8;
                            const uint32_t rMult = (rT * finalA) >> 8;
                            const uint32_t gMult = (gT * finalA) >> 8;
                            const uint32_t bMult = (bT * finalA) >> 8;
                            
                            for (int y = 0; y < height; ++y) {
                                const uint32_t* srcRow = static_cast<const uint32_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));
                                
                                for (int x = 0; x < width; ++x) {
                                    uint32_t p = srcRow[x];
                                    uint32_t r = (p >> 16) & 0xff;
                                    uint32_t g = (p >> 8) & 0xff;
                                    uint32_t b = p & 0xff;
                                    
                                    r = (r * rMult) >> 8;
                                    g = (g * gMult) >> 8;
                                    b = (b * bMult) >> 8;
                                    
                                    dstRow[x] = (finalA << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                        // 3. PALETTE16 MODE (MAME's internal palette uses rgb_t, which is 32-bit ARGB)
                        else if (format == TEXFORMAT_PALETTE16) { 
                            const rgb_t* palette = prim->texture.palette;
                            
                            for (int y = 0; y < height; ++y) {
                                const uint16_t* srcRow = static_cast<const uint16_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));
                                
                                for (int x = 0; x < width; ++x) {
                                    uint32_t p = palette[srcRow[x]];
                                    
                                    uint32_t a = 255;
                                    uint32_t r = (p >> 16) & 0xff;
                                    uint32_t g = (p >> 8) & 0xff;
                                    uint32_t b = p & 0xff;
                                    
                                    a = (a * aT) >> 8;
                                    r = (((r * rT) >> 8) * a) >> 8;
                                    g = (((g * gT) >> 8) * a) >> 8;
                                    b = (((b * bT) >> 8) * a) >> 8;
                                    
                                    dstRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                    } // Scoped BitmapData write lock released here
                    
                    g.drawImage(processor->cachedTexture,
                                static_cast<int>(rect.getX()), static_cast<int>(rect.getY()),
                                static_cast<int>(rect.getWidth()), static_cast<int>(rect.getHeight()),
                                0, 0, width, height, false);
                    
                } else {
                    juce::Colour color((uint8_t)(prim->color.r * 255.0f), (uint8_t)(prim->color.g * 255.0f),
                                       (uint8_t)(prim->color.b * 255.0f), (uint8_t)(prim->color.a * 255.0f));
                    g.setColour(color);
                    g.fillRect(rect);
                }
                
            } else if (prim->type == render_primitive::LINE) {
                juce::Colour color((uint8_t)(prim->color.r * 255.0f), (uint8_t)(prim->color.g * 255.0f),
                                   (uint8_t)(prim->color.b * 255.0f), (uint8_t)(prim->color.a * 255.0f));
                g.setColour(color);
                
                g.drawLine(prim->bounds.x0, prim->bounds.y0, prim->bounds.x1, prim->bounds.y1, prim->width);
            }
        }
        
        prims.release_lock();
                            
        // Swap the ready buffer index. The JUCE GUI will now pick up the fresh frame.
        processor->readyBufferIndex.store(writeIndex, std::memory_order_release);
        processor->getFrameFlag().store(true, std::memory_order_release);
    }
        
    virtual void input_update(bool relative_reset) override {
        
            if (mame_machine == nullptr) return;
            
            // ==============================================================================
            // --- 1. MOUSE INJECTION ---
            // ==============================================================================
            render_target* target = main_target;
            if (target != nullptr) {
                int x = processor->mouseX.load(std::memory_order_relaxed);
                int y = processor->mouseY.load(std::memory_order_relaxed);
                uint32_t currentBtns = processor->mouseButtons.load(std::memory_order_relaxed);
                
                int32_t pressed =  ((currentBtns & 1) && !(lastMouseButtons & 1)) ? 1 : 0;
                int32_t released = (!(currentBtns & 1) && (lastMouseButtons & 1)) ? 1 : 0;
                int32_t clicks = pressed;

                mame_machine->ui_input().push_pointer_update(
                    target, ui_input_manager::pointer::MOUSE, 0, 0,
                    x, y, currentBtns, pressed, released, clicks
                );
                
                lastMouseButtons = currentBtns;
            }

                    // ==============================================================================
                    // --- SAVE PROGRAM MACRO: bank detection + electronic hold (audio thread) ---
                    // ==============================================================================
                    if (processor->isSaveMacroActive.load(std::memory_order_relaxed)) {
                        auto* panel = mame_machine->root_device().subdevice<esqpanel2x40_vfx_device>("panel");
                        if (panel != nullptr) {
                            // Bank button codes from esq5505.cpp (set_button / m_pressed_buttons)
                            static const int bankCodes[] = { 55, 56, 57, 46, 47, 48, 49, 35, 34, 25 };
                            
                            // Maintain electronic hold (re-assert every frame; set_button is idempotent)
                            int wantBank = processor->macroBankToHold.load(std::memory_order_relaxed);
                            if (wantBank != audioMacroHeldBank) {
                                if (audioMacroHeldBank >= 0)
                                    panel->set_button((uint8_t)bankCodes[audioMacroHeldBank], false);
                                audioMacroHeldBank = wantBank;
                            }
                            if (audioMacroHeldBank >= 0)
                                panel->set_button((uint8_t)bankCodes[audioMacroHeldBank], true);
                            
                            // Detect which banks are pressed (mouse clicks + our hold)
                            int mask = 0;
                            for (int i = 0; i < 10; ++i)
                                if (panel->is_button_pressed(bankCodes[i]))
                                    mask |= (1 << i);
                            processor->detectedBankMask.store(mask, std::memory_order_release);
                        }
                    } else {
                        audioMacroHeldBank = -1;
                    }

                    // ==============================================================================
                    // --- 2. VST BUTTON AUTOMATION (Direct Hardware Memory Injection) ---
                    // ==============================================================================
                    // We bypass the mouse completely and directly pull the circuits on the MAME motherboard!
                    bool macroActive = processor->isSaveMacroActive.load(std::memory_order_relaxed);
                    for (size_t i = 0; i < processor->sd1Buttons.size(); ++i) {
                        
                        // Lock-free read from the DAW's automation lane
                        float val = processor->buttonParams[i]->load(std::memory_order_relaxed);
                        bool isPressed = (val > 0.5f);

                        // During the Save Program macro, don't touch bank buttons (indices 10-19).
                        // They are controlled by MAME UI (mouse → PORT_CHANGED → set_button) for
                        // detection, and by our set_button hold above. The APVTS set_value(0) would
                        // fire PORT_CHANGED(false) and break both.
                        if (macroActive && i >= 10 && i <= 19 && !isPressed)
                            continue;

                        // PORT CALLING
                        ioport_port* port = mame_machine->root_device().ioport(processor->sd1Buttons[i].ioportTag);
                        
                        if (port != nullptr) {
                            // Locate the exact button on the circuit board using its hex mask
                            ioport_field* field = port->field(processor->sd1Buttons[i].ioportMask);
                            if (field != nullptr) {
                                // Electronically press or release the button!
                                field->set_value(isPressed ? 1 : 0);
                            }
                        }
                    }
        }
    
    virtual void check_osd_inputs() override {};
    virtual void set_verbose(bool print_verbose) override {};

    virtual void init_debugger() override {};
    virtual void wait_for_debugger(device_t &device, bool firststop) override {};

    virtual bool no_sound() override { return false; };
    virtual bool sound_external_per_channel_volume() override { return false; };
    virtual bool sound_split_streams_per_source() override { return false; };
            
    virtual osd::audio_info sound_get_information() override {
        osd::audio_info info;
        osd::audio_info::node_info node;
        
        node.m_name = "vst_audio";
        node.m_display_name = "VST Audio Output";
        node.m_id = 1;
        
        // Force MAME to generate audio at the exact sample rate required by the DAW host.
        node.m_rate = { static_cast<uint32_t>(processor->getHostSampleRate()) };
        node.m_sinks = 1;
        node.m_sources = 0;
        
        info.m_nodes.push_back(node);
        info.m_default_sink = 1;
        info.m_generation = 1;
        
        return info;
    };
    
    virtual uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) override { return 1; };
    virtual void sound_stream_close(uint32_t id) override {};
    
    virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override {
        if (processor != nullptr) {
            processor->pushAudioFromMame(buffer, samples_this_frame);
        }
    };
    
    virtual uint32_t sound_stream_source_open(uint32_t node, std::string name, uint32_t rate) override { return 0; };
    virtual uint32_t sound_get_generation() override { return 1; };
    
    virtual void sound_stream_source_update(uint32_t id, int16_t *buffer, int samples_this_frame) override {};
    virtual void sound_stream_set_volumes(uint32_t id, const std::vector<float> &db) override {};
    virtual void sound_begin_update() override {};
    virtual void sound_end_update() override {};
    virtual void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) override {};

    virtual void customize_input_type_list(std::vector<input_type_entry> &typelist) override { typelist.clear(); };
    virtual std::vector<ui::menu_item> get_slider_list() override { return {}; };

    virtual osd_font::ptr font_alloc() override { return nullptr; };
    virtual bool get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string> > &result) override { return false; };
    virtual bool execute_command(const char *command) override { return false; };

    virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override {
        return std::make_unique<VstMidiInputPort>(processor);
    };
    
    virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) override {
        return std::make_unique<VstMidiOutputPort>(processor);
    };

    virtual std::vector<osd::midi_port_info> list_midi_ports() override {
        std::vector<osd::midi_port_info> ports;
        osd::midi_port_info info;
        
        info.name = "VST MIDI"; 
        info.input = true;
        info.output = true;
        info.default_input = true;
        info.default_output = true;
        
        ports.push_back(info);
        return ports;
    };

    virtual std::unique_ptr<osd::network_device> open_network_device(int id, osd::network_handler &handler) override { return {}; };
    virtual std::vector<osd::network_device_info> list_network_devices() override { return {}; };
};

// ==============================================================================
// VST AUTOMATION (APVTS) DEFINITIONS
// ==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout EnsoniqSD1AudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Automated parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>("volume", "Volume", 0.0f, 1.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("data_entry", "Data Entry", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("pitch_bend", "Pitch Bend", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("mod_wheel", "Mod Wheel", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("sustain_pedal", "Sustain Pedal", 0.0f, 1.0f, 0.0f));
    
    // --- AUTOMATED BUTTONS (Generated dynamically from sd1Buttons array) ---
    for (const auto& btn : sd1Buttons) {
            params.push_back(std::make_unique<juce::AudioParameterBool>(btn.paramID, btn.paramName, false));
        }
        
    // --- No automation of settings ---
    auto nonAutomatable = juce::AudioParameterChoiceAttributes().withAutomatable(false);

    juce::StringArray bufferSizes = { "128", "256", "512", "1024", "2048", "4096", "8192" };
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("buffer_size", 1), "Internal Buffer", bufferSizes, 2, nonAutomatable));

    // Dynamic Panel Layout Selector
    juce::StringArray views = { "Compact (Default)", "Full Keyboard", "Rack Panel", "Tablet View" };
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("layout_view", 1), "Panel Layout", views, 0, nonAutomatable));

    return { params.begin(), params.end() };
}

void EnsoniqSD1AudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
        
    // --- 1. SETUP ---
    if (parameterID == "buffer_size") {
                auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("buffer_size"));
                if (choiceParam != nullptr) {
                    int sizes[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
                    int newThreshold = sizes[choiceParam->getIndex()];
                    mameBufferThreshold.store(newThreshold, std::memory_order_relaxed);
                    
                    if ((juce::MessageManager::getInstanceWithoutCreating() != nullptr && juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread())) {
                        setLatencySamples(newThreshold + getInternalHardwareLatencySamples());
                    }
                    // NEW: AU audio-thread fallback
                    else if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
                        juce::MessageManager::callAsync([this, newThreshold]() {
                            setLatencySamples(newThreshold + getInternalHardwareLatencySamples());
                        });
                    }
                }
                requestGlobalSave.store(true, std::memory_order_release);
            }
                
    else if (parameterID == "layout_view") {
            auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("layout_view"));
            int idx = choiceParam != nullptr ? choiceParam->getIndex() : 0;
            
            requestedViewIndex.store(idx, std::memory_order_release);
            requestViewChange.store(true, std::memory_order_release);
            requestGlobalSave.store(true, std::memory_order_release);
        }
    
        // --- 2. AUTOMATION ---
        else {
            uint64_t targetSample = totalRead.load(std::memory_order_acquire) + mameBufferThreshold.load(std::memory_order_relaxed);
            double sr = hostSampleRate.load(std::memory_order_relaxed);
            double t_anchor = anchorMameTime.load(std::memory_order_relaxed);
            uint64_t s_anchor = anchorDawSample.load(std::memory_order_relaxed);
                        
            // Same clean math as in processBlock
            double targetMameTime = t_anchor + static_cast<double>(targetSample - s_anchor) / sr;

            if (parameterID == "volume") {
                uint8_t val = static_cast<uint8_t>(newValue * 127.0f);
                pushMidiByte(0xB0, targetMameTime);
                pushMidiByte(0x07, targetMameTime);
                pushMidiByte(val, targetMameTime);
            }
        }
}
// ==============================================================================

void EnsoniqSD1AudioProcessor::loadGlobalSettings()
{
    juce::InterProcessLock settingsLock("EnsoniqSD1_Settings_Lock");
    if (!settingsLock.enter(500)) return;  // wait up to 500ms for another instance to finish writing

    juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::File settingsFile = docsDir.getChildFile("EnsoniqSD1").getChildFile("settings.xml");

    for (int i = 0; i < 5; ++i) {
        if (settingsFile.existsAsFile()) {
            if (auto xml = juce::XmlDocument::parse(settingsFile)) {
                
                int bufIdx = xml->getIntAttribute("buffer_size", 2);
                int viewIdx = xml->getIntAttribute("layout_view", 0);
                savedWindowWidth = xml->getIntAttribute("window_width", 1200);
                savedWindowHeight = xml->getIntAttribute("window_height", 900);

                fileManagerState.fmWindowWidth = xml->getIntAttribute("fm_window_width", 1200);
                fileManagerState.fmWindowHeight = xml->getIntAttribute("fm_window_height", 925);

                showWelcomeMessage.store(!xml->getBoolAttribute("welcome_shown", false), std::memory_order_release);
                customRomPath = xml->getStringAttribute("rom_path", "");

                lastBrowsedFolder = xml->getStringAttribute("last_browsed_folder");
                lastMediaFolder   = xml->getStringAttribute("last_media_folder");
                lastRomFolder     = xml->getStringAttribute("last_rom_folder");
                myComputerPath    = xml->getStringAttribute("my_computer_path");

                bookmarkFolders.clear();
                for (int j = 0; j < 10; ++j) {
                    juce::String key = "bookmark_" + juce::String(j);
                    juce::String path = xml->getStringAttribute(key, "");
                    if (path.isNotEmpty())
                        bookmarkFolders.add(path);
                }

                if (auto* p = apvts.getParameter("buffer_size"))
                    p->setValue(p->convertTo0to1(bufIdx));

                if (auto* p = apvts.getParameter("layout_view"))
                    p->setValue(p->convertTo0to1(viewIdx));

                int sizes[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
                if (bufIdx >= 0 && bufIdx < 6) {
                    mameBufferThreshold.store(sizes[bufIdx], std::memory_order_relaxed);
                }
                requestedViewIndex.store(viewIdx, std::memory_order_release);
                
                settingsLock.exit();
                return;
            }
        } else {
            showWelcomeMessage.store(true, std::memory_order_release);
            settingsLock.exit();
            return; 
        }
        juce::Thread::sleep(10);
    }
    settingsLock.exit();
}

EnsoniqSD1AudioProcessor::EnsoniqSD1AudioProcessor()
     : AudioProcessor (BusesProperties()
                       .withOutput ("Main Out", juce::AudioChannelSet::stereo(), true)
                       .withOutput ("Aux Out",  juce::AudioChannelSet::stereo(), false)
                       ),
       apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    // Generate a unique sandbox directory for this instance's NVRAM/CFG
    instanceTempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("EnsoniqSD1_Instance_" + juce::Uuid().toString())
                            .getFullPathName();
    
    apvts.addParameterListener("volume", this);
    apvts.addParameterListener("data_entry", this);
    apvts.addParameterListener("pitch_bend", this);
    apvts.addParameterListener("mod_wheel", this);
    apvts.addParameterListener("buffer_size", this);
    apvts.addParameterListener("layout_view", this);
    
    // Initialize VFD array to 0 (blank segments)
    for (int i = 0; i < VFD_SIZE; ++i) {
        vfdSegments[i].store(0, std::memory_order_relaxed);
    }
        
    // Build the VFD text dictionary from the ROM file
    buildVfdDictionary();
    
    // --- CACHE VST BUTTON POINTERS FOR MAME (0% CPU overhead) ---
    // We store the raw atomic pointers so the MAME audio thread can read them instantly
    // without locking or string lookups.
    for (const auto& btn : sd1Buttons) {
        buttonParams.push_back(apvts.getRawParameterValue(btn.paramID));
    }
    
    // Loading global settings at start
    loadGlobalSettings();
    
#ifdef _WIN32
        // Request 1ms timer resolution from the Windows OS scheduler.
        // By default, Windows thread sleeping (e.g., wait(1)) can overshoot up to 15.6ms.
        // This strict 1ms resolution ensures the MAME background thread wakes up precisely,
        // preventing audio dropouts during low-latency real-time playback, while keeping
        // offline rendering (bounce) mathematically intact and fully synchronized.
        timeBeginPeriod(1);
#endif
}

EnsoniqSD1AudioProcessor::~EnsoniqSD1AudioProcessor()
{
        // safely shut down MAME engine
        shutdownMame();

#ifdef _WIN32
        // Release the high-resolution timer request gracefully when the plugin is destroyed
        // or removed from the DAW track, returning Windows to its default scheduler resolution.
        timeEndPeriod(1);
#endif
}

void EnsoniqSD1AudioProcessor::shutdownMame()
{
        isMameRunning.store(false, std::memory_order_release);
                
        mameThrottleEvent.signal();
        mameStateEvent.signal();
        
        if (mameThread.joinable()) {
            mameThread.join();
        }
       
        // --- SYNC NVRAM TO MASTER AND CLEANUP SANDBOX ---
        juce::File instDir(instanceTempDir);
        if (instDir.exists()) {
            juce::InterProcessLock nvramLock("EnsoniqSD1_NVRAM_Lock");
            
            // Wait up to 2 seconds for another instance to finish saving
            if (nvramLock.enter(2000)) {
                juce::File masterDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                        .getChildFile("EnsoniqSD1").getChildFile("GlobalState");
                masterDir.createDirectory();
                
                juce::File instNvram = instDir.getChildFile("nvram").getChildFile("sd132");
                juce::File masterNvram = masterDir.getChildFile("nvram").getChildFile("sd132");
                
                if (instNvram.exists() && instNvram.isDirectory()) {
                    masterNvram.createDirectory();
                    juce::File instOsram = instNvram.getChildFile("osram");
                    juce::File instSeqram = instNvram.getChildFile("seqram");
                    juce::File destOsram = masterNvram.getChildFile("osram");
                    juce::File destSeqram = masterNvram.getChildFile("seqram");
                    
                    if (instOsram.existsAsFile()) {
                        destOsram.deleteFile();
                        instOsram.copyFileTo(destOsram);
                    }
                    if (instSeqram.existsAsFile()) {
                        destSeqram.deleteFile();
                        instSeqram.copyFileTo(destSeqram);
                    }
                }
                nvramLock.exit();
            }
            // Delete the unique temp directory completely
            instDir.deleteRecursively();
        }

        mameHasStarted.store(false, std::memory_order_release);
}

//==============================================================================
const juce::String EnsoniqSD1AudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool EnsoniqSD1AudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool EnsoniqSD1AudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool EnsoniqSD1AudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double EnsoniqSD1AudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int EnsoniqSD1AudioProcessor::getNumPrograms()
{
    return 1;
}

int EnsoniqSD1AudioProcessor::getCurrentProgram()
{
    return 0;
}

void EnsoniqSD1AudioProcessor::setCurrentProgram (int index)
{
}

const juce::String EnsoniqSD1AudioProcessor::getProgramName (int index)
{
    return {};
}

void EnsoniqSD1AudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void EnsoniqSD1AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{

    // AU anchor reset
#if JucePlugin_Build_AU
    auAnchorSet.store(false, std::memory_order_release);
#endif
        
    juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
    if (!ensoniqDir.exists()) ensoniqDir.createDirectory();

    hostSampleRate.store(sampleRate);
    
    auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("buffer_size"));
        if (choiceParam != nullptr) {
            int sizes[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
            mameBufferThreshold.store(sizes[choiceParam->getIndex()], std::memory_order_relaxed);
        }

        // Report buffer for Latency Compensation (PDC)
        int currentThreshold = mameBufferThreshold.load(std::memory_order_relaxed);
        int hwLatency = getInternalHardwareLatencySamples();
        
        setLatencySamples(currentThreshold + hwLatency);
    
        // Only boot MAME the very first time play is prepared
        if (!mameHasStarted.exchange(true)) {
            
                        // --- VST SCANNER PROTECTION ---
                        // We check which process is currently running the plugin
                        juce::String hostPath = juce::PluginHostType().getHostPath().toLowerCase();
                        
                        // Detect Maschine once, shared by all processBlock Maschine-specific branches
                        isMaschineHost = hostPath.contains("maschine");
                        
                        // If this is a plugin scanner or validator (e.g., Cubase vstscanner.exe),
                        // we have absolutely no intention of running MAME and writing to the file system!
                        if (hostPath.contains("scanner") || hostPath.contains("validator")) {
                            isMameRunning = false;
                            return;
                        }
        
        initialSampleRate.store(sampleRate);
            
                        // 1. Run rigorous Self-Check
                        if (runSelfCheck()) {
                        // 2. If healthy, proceed to ROM check and MAME boot
                            checkRomAndBootMame();
                        } else {
                            // Self-check failed, halt completely
                            isMameRunning = false;
                        }
        
    } else {
        
        // MAME cannot change sample rates on the fly. If the DAW changes it mid-session, we must halt processing.
        if (sampleRate != initialSampleRate.load()) {
            sampleRateMismatch.store(true, std::memory_order_release); 
        } else {
            sampleRateMismatch.store(false, std::memory_order_release); 
        }
    }
    
        // --- Buffer reset & PDC Pre-fill ---
        totalRead.store(0, std::memory_order_release);
        
        // We shift the MAME write pointer forward by the specified PDC delay!
        // This ensures that the first 'currentThreshold' samples will be pure silence,
        // which the DAW's PDC will be able to trim precisely from the beginning of the file.

        // We shift the MAME write pointer forward by the specified PDC delay!
        totalWritten.store(currentThreshold, std::memory_order_release);
        
        // --- Reset interpolator ---
        needAnchorSync.store(true, std::memory_order_release);
            
        midiReadPos.store(0, std::memory_order_release);
        midiWritePos.store(0, std::memory_order_release);
    
#if JucePlugin_Build_AU
    pendingAUMidi.clear();
#endif

        // We clear the entire ring buffer to ensure that the preloaded section is completely silent
        for (int i = 0; i < RING_BUFFER_SIZE; ++i) {
            ringBufferL[i] = 0.0f;
            ringBufferR[i] = 0.0f;
            ringBufferAuxL[i] = 0.0f;
            ringBufferAuxR[i] = 0.0f;
        }
    
                // NEW: AU Flag that prepareToPlay just finished
                if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
                    prepareWasCalled.store(true, std::memory_order_release);
                    maxOfflineBuffer.store(mameBufferThreshold.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
}

void EnsoniqSD1AudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool EnsoniqSD1AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 1. Main Out MUST be active and stereo
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // 2. Aux Out must be stereo ONLY IF the user explicitly enables it in the DAW
    auto auxBus = layouts.getChannelSet(false, 1);
    if (auxBus != juce::AudioChannelSet::disabled() && auxBus != juce::AudioChannelSet::stereo())
        return false;

    return true;
}
#endif

void EnsoniqSD1AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    
      int numSamples = buffer.getNumSamples();
      if (numSamples <= 0) return; // Safety check
        
        // CRITICAL SAFETY GATE:
        // Logic Pro (AU) often calls processBlock before MAME's background thread is ready.
        // If we call mameMachine->time().as_double() before clocks are set, it triggers SIGFPE (divide by zero).
        if (!mameIsFullyBooted.load(std::memory_order_acquire)) {
            buffer.clear(); // Output silence until the engine is stable
            return;
        }
    
    // 1. Get the ACTUAL number of channels provided by the host in this specific callback
    int numChannels = buffer.getNumChannels();
    int totalNumInputChannels = getTotalNumInputChannels();
    
    // 2. PROTECTION: Safely clear only the output channels that physically exist in the buffer.
    // The VST3 standard (especially in strict DAWs like Studio One/Pro) might pass nullptrs
    // for unrouted outputs even if the reported channel count is higher. We MUST check for nullptr!
    for (int i = totalNumInputChannels; i < numChannels; ++i) {
        auto* channelData = buffer.getWritePointer(i);
        if (channelData != nullptr) {
            juce::FloatVectorOperations::clear(channelData, numSamples);
        }
    }
    
    // --- State and Boot Checks ---
    if (sampleRateMismatch.load(std::memory_order_acquire)) return;
    if (!isMameRunningFlag()) return;
        
        // ========================================================
        // WARM BOOT PRE-ROLL (LOGIC PRO ONLY - Fixes silent 1st note)
        // ========================================================
        if (needsBootPreRoll.exchange(false, std::memory_order_acquire)) {
            if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit && mameMachine != nullptr) {
                
                // 1. Wait for MAME to consume the RAM injection and reset the virtual CPU
                int injTimeout = 1000;
                while (pendingRamInjection.load(std::memory_order_acquire) && injTimeout > 0) {
                    mameThrottleEvent.signal();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    injTimeout--;
                }

                // 2. Fast-forward MAME by 2.0 virtual seconds to let the SD-1 OS fully boot!
                double targetTime = mameMachine->time().as_double() + 2.0;
                int timeout = 3000;
                
                while (mameMachine->time().as_double() < targetTime && timeout > 0) {
                    uint64_t wPos = totalWritten.load(std::memory_order_relaxed);
                    if (wPos > totalRead.load(std::memory_order_relaxed)) {
                        totalRead.store(wPos, std::memory_order_relaxed); // Consume audio silently
                    }
                    mameThrottleEvent.signal();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    timeout--;
                }
                                
                // 3. Clean slate for the incoming MIDI notes
                totalRead.store(0, std::memory_order_release);
                totalWritten.store(mameBufferThreshold.load(std::memory_order_relaxed), std::memory_order_release);
                needAnchorSync.store(true, std::memory_order_release);
            }
        }
    
    uint64_t currentReadPos = totalRead.load(std::memory_order_acquire);
    int threshold = mameBufferThreshold.load(std::memory_order_relaxed);
    double sr = hostSampleRate.load(std::memory_order_relaxed);
    
    // Security boot check
    if (mameMachine == nullptr) {
        totalRead.store(currentReadPos + numSamples, std::memory_order_release);
        return;
    }
    
    // ==========================================================
    // AU SPECIFIC LOGIC PRO SYNC FIX (VST3 completely bypassed)
    // ==========================================================
        
    if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
        
        bool currentOffline = isNonRealtime();
        bool offlineChanged = (currentOffline != lastOfflineState);
        lastOfflineState = currentOffline;
        bool freshPrepare = prepareWasCalled.exchange(false, std::memory_order_acq_rel);

        // Clear stale queue whenever prepareToPlay just ran OR on fresh start
        if (freshPrepare || currentReadPos == 0) {
            pendingAUMidi.clear();
        }

        // offlineChanged mid-session reset
        if (offlineChanged && !freshPrepare && currentReadPos > 0) {
            pendingAUMidi.clear();
            uint64_t newWritePos = currentReadPos + mameBufferThreshold.load(std::memory_order_relaxed);
            totalWritten.store(newWritePos, std::memory_order_release);
            maxOfflineBuffer.store(mameBufferThreshold.load(std::memory_order_relaxed), std::memory_order_relaxed);
            for (int j = 0; j < RING_BUFFER_SIZE; ++j) {
                ringBufferL[j] = 0.0f; ringBufferR[j] = 0.0f;
                ringBufferAuxL[j] = 0.0f; ringBufferAuxR[j] = 0.0f;
            }
            midiReadPos.store(midiWritePos.load(std::memory_order_acquire), std::memory_order_release);
        }
    }
    
    // =====================================================================
    // DETECT TRANSPORT ORIGINAL
    // =====================================================================
    
    bool isPlaying = false;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            isPlaying = pos->getIsPlaying();
        }
    }
    
    // --- DETECT LOGIC TRANSPORT & BOUNCE START ---
    bool justStartedPlaying = (isPlaying && !lastIsPlaying);
    lastIsPlaying = isPlaying;
    
    // Self-contained offline tracker to fix the scope issue
    bool isOffline = isNonRealtime();
    bool isBounceStart = (isOffline && !localLastOffline);
    localLastOffline = isOffline;

    // ==============================================================
    // MASCHINE PLAY-START FLUSH (wrapper-specific)
    // ==============================================================
    // When Maschine starts playback, the ring buffer contains stale audio
    // generated during idle time. This audio predates any MIDI events in
    // this block. If we read it, the first notes are silent/delayed by ~30ms.
    // Fix: at play-start, skip all stale audio so MIDI events and audio
    // are generated together from the same point in time.
    {
        if (isMaschineHost && justStartedPlaying) {
            maschineInFastRender = false;

            bool mameBooted = (mameMachine != nullptr && mameMachine->time().as_double() > 3.0);
            if (mameBooted) {
                uint64_t writePos = totalWritten.load(std::memory_order_acquire);
                if (writePos > currentReadPos) {
                    currentReadPos = writePos;
                    totalRead.store(currentReadPos, std::memory_order_release);
                }
                needAnchorSync.store(true, std::memory_order_release);
            }
        }

        if (isMaschineHost && !isPlaying)
            maschineInFastRender = false;
    }
    
    // =====================================================================
    // AU CLOCK DRIFT
    // =====================================================================
    if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
        if (justStartedPlaying || isBounceStart) {
            needAnchorSync.store(true, std::memory_order_release);
        }
    }
                
                // ========================================================
                // WAIT FOR PERFECT ANCHOR
                // ========================================================
                
                if (needAnchorSync.load(std::memory_order_acquire)) {
                    mameThrottleEvent.signal(); // Wake up MAME!
                    
                    // We determine how long we are allowed to block the audio thread.
                    int timeoutMs = isNonRealtime() ? 2000 : 2;
                    int waitMs = 0;
                    
                    // Wait loop: Check if MAME has established the exact audio anchor yet
                    while (needAnchorSync.load(std::memory_order_acquire) && waitMs < timeoutMs) {
    #if defined(_WIN32)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    #elif defined(__APPLE__)
                        if (pthread_main_np() == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        else {
                            break; // Safety breakout for macOS main thread rendering
                        }
    #else
                        if (! juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        else {
                            break;
                        }
    #endif
                        waitMs++;
                    }

                    // EMERGENCY FALLBACK (REALTIME ONLY):
                    if (needAnchorSync.load(std::memory_order_acquire)) {
                        if (!isNonRealtime() && mameMachine != nullptr) {
#if JucePlugin_Build_AU
        uint64_t anchorSample = totalWritten.load(std::memory_order_acquire);
        double capturedMameTime = mameMachine->time().as_double();
        anchorDawSample.store(anchorSample, std::memory_order_relaxed);
        anchorMameTime.store(capturedMameTime, std::memory_order_relaxed);
#else
                            anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
                            anchorDawSample.store(totalWritten.load(std::memory_order_acquire), std::memory_order_relaxed);
                    #endif
                            needAnchorSync.store(false, std::memory_order_release);
                        }
                    }
                }
        
        // FRESH ANCHOR
        double t_anchor = anchorMameTime.load(std::memory_order_relaxed);
        uint64_t s_anchor = anchorDawSample.load(std::memory_order_relaxed);
    
    // Trigger the Logic Chase filter on block 0, transport start, or bounce start
    bool isLogicChaseDump = (currentReadPos == 0) || justStartedPlaying || isBounceStart;
        
    // -------------------------------
    // AU MIDI dispatch
    // -------------------------------
    
    // Suppress all incoming DAW MIDI during Write Single Program
    // (our pushMidiByte calls bypass this — they go directly to the ring buffer)
    if (suppressMidiInput.load(std::memory_order_acquire))
        midiMessages.clear();

    if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit)
    {
        bool mameHasAudio = totalWritten.load(std::memory_order_acquire) > static_cast<uint64_t>(threshold);
        
        double currentMameTime = (mameMachine != nullptr) ? mameMachine->time().as_double() : 0.0;

        // Anchor the clock to NOW so we don't accidentally stagger into the past
        if (isLogicChaseDump) {
            lastAuMidiTime = currentMameTime;
        }

        if (needAnchorSync.load(std::memory_order_acquire) || !mameHasAudio)
        {
            if (pendingAUMidi.empty()) {
                captureReadPos = currentReadPos;
            }

            for (const auto metadata : midiMessages) {
                auto msg = metadata.getMessage();
                
                if (isLogicChaseDump) {
                    bool keep = false;
                    // AGGRESSIVE CHASE FILTER: Drops ALL CCs on block 0!
                    // Keeps the UART buffer 100% clear for the initial Note On.
                    if (msg.isNoteOn() || msg.isNoteOff() || msg.isPitchWheel() || msg.isAftertouch() || msg.isChannelPressure()) {
                        keep = true;
                    }
                    if (!keep) continue;
                }
                
                int absoluteOffset = metadata.samplePosition + static_cast<int>(currentReadPos - captureReadPos);
                pendingAUMidi.push_back({msg, absoluteOffset});
            }
            mameThrottleEvent.signal();
        }
        else
        {
            // 1. Flush pending
            if (!pendingAUMidi.empty()) {
                for (auto& pending : pendingAUMidi) {
                    auto msg = pending.first;
                    
                    uint64_t absoluteDawSample = captureReadPos + pending.second;
                    uint64_t targetSample = absoluteDawSample + threshold;
                    
                    double targetMameTime = t_anchor + static_cast<double>(
                        static_cast<int64_t>(targetSample) - static_cast<int64_t>(s_anchor)) / sr;

                    const uint8_t* rawData = msg.getRawData();
                    for (int i = 0; i < msg.getRawDataSize(); ++i) {
                        
                        // FIX: Prevent 0-cycle UART Overrun!
                        // If queued events calculate to the past, clamp them to the present so the virtual CPU can read them.
                        if (targetMameTime < currentMameTime) targetMameTime = currentMameTime;
                        
                        if (targetMameTime < lastAuMidiTime + 0.00032) {
                            targetMameTime = lastAuMidiTime + 0.00032;
                        }
                        pushMidiByte(rawData[i], targetMameTime);
                        lastAuMidiTime = targetMameTime;
                    }
                }
                pendingAUMidi.clear();
            }

            // 2. Process current block
            for (const auto metadata : midiMessages) {
                auto msg = metadata.getMessage();
                int eventOffset = metadata.samplePosition;
                
                if (isLogicChaseDump) {
                    bool keep = false;
                    if (msg.isNoteOn() || msg.isNoteOff() || msg.isPitchWheel() || msg.isAftertouch() || msg.isChannelPressure()) {
                        keep = true;
                    }
                    if (!keep) continue;
                }

                double targetMameTime;

                if (!isPlaying && !isOffline) {
                    targetMameTime = currentMameTime + static_cast<double>(eventOffset) / sr;
                } else {
                    uint64_t targetSample = currentReadPos + eventOffset + threshold;
                    targetMameTime = t_anchor + static_cast<double>(
                        static_cast<int64_t>(targetSample) - static_cast<int64_t>(s_anchor)) / sr;
                }

                const uint8_t* rawData = msg.getRawData();
                for (int i = 0; i < msg.getRawDataSize(); ++i) {
                    
                    // FIX: Prevent 0-cycle UART Overrun!
                    if (targetMameTime < currentMameTime) targetMameTime = currentMameTime;
                    
                    if (targetMameTime < lastAuMidiTime + 0.00032) {
                        targetMameTime = lastAuMidiTime + 0.00032;
                    }
                    pushMidiByte(rawData[i], targetMameTime);
                    lastAuMidiTime = targetMameTime;
                }
            }
        }
    }
        else
        {
            // ========================================================
            // VST3 STANDARD MIDI DISPATCH
            // ========================================================
            
            // Maschine play-start chase filter: drop CCs on transport start (same as AU isLogicChaseDump).
            bool maschineChaseFilter = isMaschineHost && justStartedPlaying;
            
            // Effective threshold for Maschine:
            //   block 0 (justStartedPlaying): effectiveThreshold=0
            //     → past-clamp restores threshold effect in real-time (MAME is ahead)
            //     → in WAV export after flush (MAME at currentReadPos) gives minimal offset
            //   block 1+ (maschineInFastRender confirmed by wait loop): effectiveThreshold=0
            // effectiveThreshold: 0 for Maschine render blocks, normal otherwise
            bool isMaschineRenderBlock = isMaschineHost && (justStartedPlaying || maschineInFastRender);
            uint64_t effectiveThreshold = isMaschineRenderBlock ? 0u : static_cast<uint64_t>(threshold);

            double currentMameTimeVST = (mameMachine != nullptr) ? mameMachine->time().as_double() : 0.0;

            for (const auto& metadata : midiMessages) {
                auto msg = metadata.getMessage();
                int eventOffset = metadata.samplePosition;

                if (maschineChaseFilter) {
                    if (!msg.isNoteOn() && !msg.isNoteOff() && !msg.isPitchWheel()
                        && !msg.isAftertouch() && !msg.isChannelPressure())
                        continue;
                }

                double targetMameTime;
                if (!isPlaying && !isOffline) {
                    targetMameTime = currentMameTimeVST + static_cast<double>(eventOffset) / sr;
                } else {
                    uint64_t targetSample = currentReadPos + static_cast<uint64_t>(eventOffset) + effectiveThreshold;
                    targetMameTime = t_anchor + static_cast<double>(
                        static_cast<int64_t>(targetSample) - static_cast<int64_t>(s_anchor)) / sr;
                }
                if (targetMameTime < currentMameTimeVST) targetMameTime = currentMameTimeVST;

                const uint8_t* rawData = msg.getRawData();
                for (int i = 0; i < msg.getRawDataSize(); ++i)
                    pushMidiByte(rawData[i], targetMameTime);
            }
        }
    
        // ========================================================
        // 1. TIMESTAMPED MIDI INJECTION
        // ========================================================
        
            // --- DELAYED INTERNAL MIDI PANIC ---
            int currentPanicDelay = panicDelaySamples.load(std::memory_order_acquire);
            if (currentPanicDelay > 0) {
                currentPanicDelay -= numSamples;
                
                if (currentPanicDelay <= 0) {
                    // Send CC 123 and 120 safely, RESPECTING the physical MIDI baud rate!
                    // 31250 bps = ~0.00032 seconds per byte. We space them out to prevent CPU interrupt flood.
                    double timePerByte = 0.00032;
                    int byteCount = 0;

                    for (uint8_t ch = 0; ch < 16; ++ch) {
                        pushMidiByte(0xB0 | ch, t_anchor + (byteCount++ * timePerByte));
                        pushMidiByte(123,       t_anchor + (byteCount++ * timePerByte));
                        pushMidiByte(0,         t_anchor + (byteCount++ * timePerByte));

                        pushMidiByte(0xB0 | ch, t_anchor + (byteCount++ * timePerByte));
                        pushMidiByte(120,       t_anchor + (byteCount++ * timePerByte));
                        pushMidiByte(0,         t_anchor + (byteCount++ * timePerByte));
                    }
                    panicDelaySamples.store(0, std::memory_order_release);
                } else {
                    panicDelaySamples.store(currentPanicDelay, std::memory_order_release);
                }
            }
                                                        
    // ========================================================
    // 2. AUDIO OUT & RING BUFFER CONSUMPTION
    // ========================================================
    
    int timeoutMs = 0;
    
    if (isOffline) {
            timeoutMs = 2000;
            // --- BOUNCE JITTER FIX ---
            maxOfflineBuffer.store(numSamples + mameBufferThreshold.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    
    if (timeoutMs > 0) {
        int elapsedMs = 0;
        uint64_t targetWritePos = currentReadPos + numSamples;
        
        if (isOffline) {
            targetWritePos += mameBufferThreshold.load(std::memory_order_relaxed);
        }
        
        while (isMameRunningFlag()) {
            uint64_t writePos = totalWritten.load(std::memory_order_acquire);
            
            // Ultra-stable baseline wait loop
            if (writePos >= targetWritePos) break;
            if (elapsedMs >= timeoutMs) break;
            
            mameThrottleEvent.signal();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            elapsedMs++;
        }
    }
    
    // ==============================================================
    // MASCHINE WAV RENDER FIX (wrapper-specific, does NOT touch above)
    // ==============================================================
    // Maschine renders WAV in real-time mode (isNonRealtime()=false) but
    // calls processBlock at max CPU speed (~100x faster than real-time).
    // The MAME thread runs at real-time speed → ring buffer underruns → silence gaps.
    // Fix: wait for MAME to produce enough samples, same as the offline path.
    {
        if (isMaschineHost && !isOffline && isPlaying && isMameRunningFlag()) {
            bool mameBooted = (mameMachine != nullptr && mameMachine->time().as_double() > 3.0);

            if (mameBooted) {
                uint64_t targetWritePos = currentReadPos + static_cast<uint64_t>(numSamples);
                int elapsedMs = 0;

                while (isMameRunningFlag()) {
                    uint64_t writePos = totalWritten.load(std::memory_order_acquire);
                    if (writePos >= targetWritePos) break;
                    if (elapsedMs >= 2000) break;
                    mameThrottleEvent.signal();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    elapsedMs++;
                }

                // Confirm fast-render mode for next block's effectiveThreshold detection.
                if (elapsedMs > 0)
                    maschineInFastRender = true;
            }
        }
    }
    
        uint64_t currentWritePos = totalWritten.load(std::memory_order_acquire);
        int64_t available = static_cast<int64_t>(currentWritePos) - static_cast<int64_t>(currentReadPos);
        // Calculate how many samples we can push
        int samplesToProcess = 0;
        if (available > 0) {
            samplesToProcess = (available < numSamples) ? static_cast<int>(available) : static_cast<int>(numSamples);
        }
        
        // 3. PROTECTION: Safely retrieve write pointers based on the actual buffer dimensions!
        // If the DAW nullified the bus, getWritePointer will return nullptr.
        auto* outL    = (numChannels > 0) ? buffer.getWritePointer(0) : nullptr;
        auto* outR    = (numChannels > 1) ? buffer.getWritePointer(1) : nullptr;
        auto* outAuxL = (numChannels > 2) ? buffer.getWritePointer(2) : nullptr;
        auto* outAuxR = (numChannels > 3) ? buffer.getWritePointer(3) : nullptr;
        
        // Consume samples from the ring buffers
        for (int i = 0; i < samplesToProcess; ++i) {
            
            // --- OPTIMIZATION ---
            // Bitwise AND (&) wrap-around instead of modulo (%)
            uint64_t idx = currentReadPos & (RING_BUFFER_SIZE - 1);
            
            // 4. PROTECTION: Even Main L/R must be checked against nullptr to be 100% crash-proof
            if (outL != nullptr)    outL[i]    = ringBufferL[idx];
            if (outR != nullptr)    outR[i]    = ringBufferR[idx];
            if (outAuxL != nullptr) outAuxL[i] = ringBufferAuxL[idx];
            if (outAuxR != nullptr) outAuxR[i] = ringBufferAuxR[idx];
            
            currentReadPos++;
        }
        
        // Underrun protection: pad remaining required samples with zeroes
        if (!isNonRealtime() && samplesToProcess < numSamples) {
            for (int i = samplesToProcess; i < numSamples; ++i) {
                if (outL != nullptr)    outL[i]    = 0.0f;
                if (outR != nullptr)    outR[i]    = 0.0f;
                if (outAuxL != nullptr) outAuxL[i] = 0.0f;
                if (outAuxR != nullptr) outAuxR[i] = 0.0f;
            }
        }
        
        totalRead.store(currentReadPos, std::memory_order_release);
        mameThrottleEvent.signal(); // Wake MAME for the next round
        
        // ==============================================================
        // MIDI OUTPUT: Drain the SD-1 DUART TX → JUCE MidiBuffer
        // ==============================================================
        {
            int readPos = midiOutReadPos.load(std::memory_order_acquire);
            int writePos = midiOutWritePos.load(std::memory_order_acquire);
            
            while (readPos != writePos) {
                uint8_t byte = midiOutBuffer[readPos & (MIDI_OUT_BUFFER_SIZE - 1)];
                readPos = (readPos + 1) & (MIDI_OUT_BUFFER_SIZE - 1);
                
                // Real-time messages (F8-FF): always 1 byte, can appear anywhere
                if (byte >= 0xF8) {
                    midiMessages.addEvent(&byte, 1, 0);
                    continue;
                }
                
                // SysEx handling
                if (byte == 0xF0) {
                    midiOutMsg.clear();
                    midiOutMsg.push_back(byte);
                    midiOutInSysEx = true;
                    continue;
                }
                
                if (midiOutInSysEx) {
                    midiOutMsg.push_back(byte);
                    if (byte == 0xF7) {
                        // Complete SysEx message
                        midiMessages.addEvent(midiOutMsg.data(), (int)midiOutMsg.size(), 0);
                        midiOutMsg.clear();
                        midiOutInSysEx = false;
                    }
                    // Safety: cap SysEx at 64KB to prevent runaway accumulation
                    if (midiOutMsg.size() > 65536) {
                        midiOutMsg.clear();
                        midiOutInSysEx = false;
                    }
                    continue;
                }
                
                // Status byte (new message)
                if (byte & 0x80) {
                    midiOutMsg.clear();
                    midiOutMsg.push_back(byte);
                    midiOutRunningStatus = byte;
                    
                    // System common messages with no data bytes
                    if (byte == 0xF6 || byte == 0xF7) {
                        midiMessages.addEvent(midiOutMsg.data(), 1, 0);
                        midiOutMsg.clear();
                    }
                    continue;
                }
                
                // Data byte
                midiOutMsg.push_back(byte);
                
                // Determine expected message length from status byte
                int expectedLen = 0;
                uint8_t status = (midiOutRunningStatus & 0xF0);
                if (status == 0xC0 || status == 0xD0) expectedLen = 2;       // Program Change, Channel Pressure
                else if (status >= 0x80 && status <= 0xE0) expectedLen = 3;  // Note, CC, Pitch Bend, etc.
                else if (midiOutRunningStatus == 0xF1 || midiOutRunningStatus == 0xF3) expectedLen = 2;  // MTC, Song Select
                else if (midiOutRunningStatus == 0xF2) expectedLen = 3;      // Song Position
                
                if (expectedLen > 0 && (int)midiOutMsg.size() >= expectedLen) {
                    midiMessages.addEvent(midiOutMsg.data(), (int)midiOutMsg.size(), 0);
                    midiOutMsg.clear();
                }
            }
            
            midiOutReadPos.store(readPos, std::memory_order_release);
        }
        
        // --- ANTI-SMART-DISABLE Protection ---
        // Flipping sign sample-by-sample to generate inaudible Nyquist noise
        // NEW: Only inject noise during real-time playback
        if (!isNonRealtime()) {
            static float antiDisable = 1e-8f;
            for (int i = 0; i < numSamples; ++i) {
                antiDisable = -antiDisable; // Sample-by-sample flip!
                if (outL != nullptr) outL[i] += antiDisable;
                if (outR != nullptr) outR[i] += antiDisable;
            }
        }
    
                // ========================================================
                // AU BOUNCE "NO-READ-AHEAD" GATE (Logic Pro specifically isolated)
                // ========================================================
                if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit && isNonRealtime()) {
                    maxOfflineBuffer.store(mameBufferThreshold.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
    
    }

//==============================================================================

bool EnsoniqSD1AudioProcessor::hasEditor() const
{
    return true; 
}

juce::AudioProcessorEditor* EnsoniqSD1AudioProcessor::createEditor()
{
    return new EnsoniqSD1AudioProcessorEditor (*this);
}

// ==============================================================================
// LEGACY MAME STATE EXTRACTOR (v0.9.7 to v0.9.8 Bridge)
// ==============================================================================
bool EnsoniqSD1AudioProcessor::extractLegacyMameState(const juce::String& base64State,
                                                      juce::MemoryBlock& outOsram,
                                                      juce::MemoryBlock& outSeqram)
{
    juce::MemoryBlock compressedBlock;
    
    // The string arrives here as valid Base64 from the JUCE XML parser
    if (!compressedBlock.fromBase64Encoding(base64State) || compressedBlock.getSize() < 100) {
        return false;
    }

    const uint8_t* compData = static_cast<const uint8_t*>(compressedBlock.getData());
    size_t compSize = compressedBlock.getSize();

    // 1. Validating the MAME header on the ORIGINAL (unpacked) data!
    // The first 32 bytes of the MAME .sta file are NOT compressed!
    if (compSize < 32 || memcmp(compData, "MAMESAVE", 8) != 0) {
        return false;
    }

    // 2. Exactly locate the ZLIB header (it is usually located at offset 32 after the MAME header)
    int zlibOffset = 32;
    if (compData[zlibOffset] != 0x78) {
        // If for some reason it doesn't start at byte 32, we'll look for it:
        zlibOffset = -1;
        for (size_t i = 32; i < compSize - 1; ++i) {
            if (compData[i] == 0x78 && (compData[i+1] == 0x9C || compData[i+1] == 0xDA || compData[i+1] == 0x01)) {
                zlibOffset = static_cast<int>(i);
                break;
            }
        }
    }

    if (zlibOffset < 0) return false;

    // 3. Unpacking to raw memory
    juce::MemoryBlock uncompressedBlock;
    uLongf uncompressedSize = 10 * 1024 * 1024; // 10 MB safety limit for MAME states
    uncompressedBlock.setSize(uncompressedSize, true);
    
    int zResult = uncompress(static_cast<Bytef*>(uncompressedBlock.getData()),
                             &uncompressedSize,
                             compData + zlibOffset,
                             static_cast<uLong>(compSize - zlibOffset));
    if (zResult != Z_OK) return false;
    uncompressedBlock.setSize(uncompressedSize);

    const uint8_t* data = static_cast<const uint8_t*>(uncompressedBlock.getData());
    size_t size = uncompressedBlock.getSize();

    outOsram.setSize(0);
    outSeqram.setSize(0);

    // ====================================================================
    // 4. HARDCODED OFFSETS FOR MAME 0.286 (IN THE ZLIB PAYLOAD)
    // ====================================================================
    // Since zlib uncompress omitted the 32-byte header from the decompressed
    // buffer, we SUBTRACTED 32 FROM the offsets obtained from the MAME dumper.
    // 2104650 - 32 = 2104618
    // 2170186 - 32 = 2170154
    size_t osram_offset  = 2104618;
    size_t seqram_offset = 2170154;

    // 5. Direct copying with the correct offset
    if (osram_offset + 65536 <= size) {
        outOsram.append(data + osram_offset, 65536);
    }
    if (seqram_offset + 327680 <= size) {
        outSeqram.append(data + seqram_offset, 327680);
    }

    return (outOsram.getSize() == 65536 && outSeqram.getSize() == 327680);
}

//==============================================================================
// SAVE STATE
//==============================================================================
void EnsoniqSD1AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // 1. Save VST parameters to XML
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    // --- 1. VERSION STAMPING ---
    xml->setAttribute("plugin_version", "1.0.0");
    
    // --- 2. SAVE UI DIMENSIONS ---
    xml->setAttribute("ui_width", savedWindowWidth);
    xml->setAttribute("ui_height", savedWindowHeight);

    // --- 3. SAVE MEDIA PATHS NATIVELY ---
    {
        std::lock_guard<std::mutex> lock(mediaMutex);
        xml->setAttribute("floppy_path", juce::String(pendingFloppyPath));
        xml->setAttribute("cart_path", juce::String(pendingCartPath));
        xml->setAttribute("is_floppy_loaded", isFloppyLoaded.load());
        xml->setAttribute("is_cart_loaded", isCartLoaded.load());
    }

    // --- 4. DIRECT RAM EXTRACTION (Version Independent) ---
        if (isMameRunningFlag() && mameMachine != nullptr) {
            
            // Request the memory block pointers directly from MAME
            auto* osram_share = mameMachine->root_device().memshare("osram");
            auto* seqram_share = mameMachine->root_device().memshare("seqram");

            if (osram_share != nullptr) {
                juce::MemoryBlock osBlock(osram_share->ptr(), osram_share->bytes());
                xml->setAttribute("ram_osram", osBlock.toBase64Encoding());
            }
            
            if (seqram_share != nullptr) {
                juce::MemoryBlock seqBlock(seqram_share->ptr(), seqram_share->bytes());
                xml->setAttribute("ram_seqram", seqBlock.toBase64Encoding());
            }
        }
    
    // --- 4.5 FLUSH ACTIVE UI STATE ---
        if (auto* editor = dynamic_cast<EnsoniqSD1AudioProcessorEditor*>(getActiveEditor())) {
            editor->flushFileManagerState();
        }
    
    // --- 5. SAVE FILE MANAGER STATE ---
    {
        auto& fms = fileManagerState;
        xml->setAttribute("fm_visible", fms.visible);
        xml->setAttribute("fm_category", fms.category);
        xml->setAttribute("fm_file", fms.openedFilePath);
        xml->setAttribute("fm_viewingBank", fms.viewingDiskBank);
        xml->setAttribute("fm_diskBankName", fms.openedDiskBankName);
        xml->setAttribute("fm_selectedRow", fms.selectedRow);
        xml->setAttribute("fm_bankRow", fms.bankSelectedRow);
        xml->setAttribute("fm_selectedName", fms.selectedName);
        xml->setAttribute("fm_bankSelectedName", fms.bankSelectedName);
        xml->setAttribute("fm_scroll", fms.scrollPosition);
        xml->setAttribute("fm_bankScroll", fms.bankScrollPosition);
        xml->setAttribute("fm_activeBookmark", fms.activeBookmark);
        xml->setAttribute("fm_viewBefore", fms.viewBeforeBrowser);
        xml->setAttribute("fm_width", fms.fmWindowWidth);
        xml->setAttribute("fm_height", fms.fmWindowHeight);
    }
    
    copyXmlToBinary (*xml, destData);
}

//==============================================================================
// LOAD STATE
//==============================================================================
void EnsoniqSD1AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr) {
        
        // 1. Restore VST Automation Parameters
        if (xmlState->hasTagName (apvts.state.getType())) {
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
        }

        // 2. Restore UI Dimensions
        savedWindowWidth = xmlState->getIntAttribute("ui_width", 0);
        savedWindowHeight = xmlState->getIntAttribute("ui_height", 0);

        // =================================================================
        // RAM EXTRACTION & MEDIA PATH RESTORATION
        // =================================================================
        juce::String savedVersion = xmlState->getStringAttribute("plugin_version", "0.9.7");
        
                // --- 3. APPLY MEDIA PATHS AND TRIGGER MOUNTING ---
                // Legacy 0.9.7 projects won't have these XML attributes. They default to empty strings.
                // For 0.9.8+, we restore the paths and strictly verify physical file existence on the disk.
                {
                    std::lock_guard<std::mutex> lock(mediaMutex);
                    juce::String savedFloppy = xmlState->getStringAttribute("floppy_path", "");
                    juce::String savedCart = xmlState->getStringAttribute("cart_path", "");
                    
                    bool floppyWasLoaded = xmlState->getBoolAttribute("is_floppy_loaded", false);
                    bool cartWasLoaded = xmlState->getBoolAttribute("is_cart_loaded", false);

                    // VERIFY FLOPPY EXISTENCE
                    if (floppyWasLoaded) {
                        if (juce::File(savedFloppy).existsAsFile()) {
                #ifdef _WIN32
                            pendingFloppyPath = savedFloppy.toUTF8().getAddress();
                #else
                            pendingFloppyPath = savedFloppy.toStdString();
                #endif
                            requestFloppyLoad.store(true, std::memory_order_release);
                            isFloppyLoaded.store(true, std::memory_order_release);
                            loadedFloppyName = juce::File(savedFloppy).getFileName();
                        } else {
                            pendingFloppyPath = "";
                            requestFloppyLoad.store(false, std::memory_order_release);
                            isFloppyLoaded.store(true, std::memory_order_release);
                            loadedFloppyName = "Missing file!";
                        }
                    } else {
                        pendingFloppyPath = "";
                        requestFloppyLoad.store(false, std::memory_order_release);
                        isFloppyLoaded.store(false, std::memory_order_release);
                        loadedFloppyName = "";
                    }

                    // VERIFY CARTRIDGE EXISTENCE
                    if (cartWasLoaded) {
                        if (juce::File(savedCart).existsAsFile()) {
                #ifdef _WIN32
                            pendingCartPath = savedCart.toUTF8().getAddress();
                #else
                            pendingCartPath = savedCart.toStdString();
                #endif
                            requestCartLoad.store(true, std::memory_order_release);
                            isCartLoaded.store(true, std::memory_order_release);
                            loadedCartName = juce::File(savedCart).getFileName();
                        } else {
                            pendingCartPath = "";
                            requestCartLoad.store(false, std::memory_order_release);
                            isCartLoaded.store(true, std::memory_order_release);
                            loadedCartName = "Missing file!";
                        }
                    } else {
                        pendingCartPath = "";
                        requestCartLoad.store(false, std::memory_order_release);
                        isCartLoaded.store(false, std::memory_order_release);
                        loadedCartName = "";
                    }
                }

                // --- 4. RAM INJECTION BRANCHING ---
                if (savedVersion == "0.9.7" && xmlState->hasAttribute("mame_state")) {
                    
                    juce::String b64String = xmlState->getStringAttribute("mame_state");

                    // LEGACY LOAD (v0.9.7)
                    if (extractLegacyMameState(b64String, pendingOsram, pendingSeqRam)) {
                        pendingRamInjection.store(true, std::memory_order_release);
                        needsBootPreRoll.store(true, std::memory_order_release);
                        isWarmBoot.store(true, std::memory_order_release); // LOAD STATE FLAG
                    }
                }
                else if (xmlState->hasAttribute("ram_osram")) {
                    
                    // LOAD FROM NEW XML FORMAT
                    pendingOsram.fromBase64Encoding(xmlState->getStringAttribute("ram_osram"));
                    pendingSeqRam.fromBase64Encoding(xmlState->getStringAttribute("ram_seqram"));
                    pendingRamInjection.store(true, std::memory_order_release);
                    needsBootPreRoll.store(true, std::memory_order_release);
                    isWarmBoot.store(true, std::memory_order_release); // LOAD STATE FLAG
                }

                requestMameLoad.store(false, std::memory_order_release);
            }
        
            // --- RESTORE FILE MANAGER STATE ---
            {
                auto& fms = fileManagerState;
                fms.visible = xmlState->getBoolAttribute("fm_visible", false);
                fms.category = xmlState->getStringAttribute("fm_category", "");
                fms.openedFilePath = xmlState->getStringAttribute("fm_file", "");
                fms.viewingDiskBank = xmlState->getBoolAttribute("fm_viewingBank", false);
                fms.openedDiskBankName = xmlState->getStringAttribute("fm_diskBankName", "");
                fms.selectedRow = xmlState->getIntAttribute("fm_selectedRow", -1);
                fms.bankSelectedRow = xmlState->getIntAttribute("fm_bankRow", -1);
                fms.selectedName = xmlState->getStringAttribute("fm_selectedName", "");
                fms.bankSelectedName = xmlState->getStringAttribute("fm_bankSelectedName", "");
                fms.scrollPosition = xmlState->getIntAttribute("fm_scroll", 0);
                fms.bankScrollPosition = xmlState->getIntAttribute("fm_bankScroll", 0);
                fms.activeBookmark = xmlState->getStringAttribute("fm_activeBookmark", "");
                fms.viewBeforeBrowser = xmlState->getIntAttribute("fm_viewBefore", 0);
                fms.fmWindowWidth = xmlState->getIntAttribute("fm_width", 1200);
                fms.fmWindowHeight = xmlState->getIntAttribute("fm_height", 925);
                
                // If the song had an active bookmark, add it to bookmarks if not already there
                if (fms.activeBookmark.isNotEmpty()) {
                    juce::File bookmarkDir(fms.activeBookmark);
                    if (bookmarkDir.isDirectory() && !bookmarkFolders.contains(fms.activeBookmark)) {
                        if (bookmarkFolders.size() >= 5)
                            bookmarkFolders.remove(0);
                        bookmarkFolders.add(fms.activeBookmark);
                    }
                }
                
                // Validate saved paths/categories; fall back to defaults if missing
                if (fms.openedFilePath.isNotEmpty() && !juce::File(fms.openedFilePath).existsAsFile()) {
                    fms.openedFilePath = "";
                    fms.selectedName = "";       // file gone → no name-based restore
                    fms.viewingDiskBank = false;
                    fms.openedDiskBankName = "";
                }
                if (fms.category.startsWith("BOOKMARK:")) {
                    juce::String path = fms.category.substring(9);
                    if (!juce::File(path).isDirectory()) {
                        // Bookmarked folder gone → fall back to INT RAM
                        fms.category = "INT (RAM)";
                        fms.selectedRow = -1;
                        fms.selectedName = "";
                        fms.activeBookmark = "";
                    }
                }
            }
            stateJustLoaded.store(true, std::memory_order_release);
            requestFileManagerUIRefresh.store(true, std::memory_order_release);
    
            // Ensure COMPARE is properly evaluated even if the Editor GUI is closed during load
            if (mameMachine != nullptr) {
                    scheduledCompareResetTime.store(mameMachine->time().as_double() + 3.5, std::memory_order_release);
                } else {
                    scheduledCompareResetTime.store(3.5, std::memory_order_release);
                }
    
}

// ==============================================================================

void EnsoniqSD1AudioProcessor::injectMouseMove(int x, int y) {
    mouseX.store(x, std::memory_order_relaxed);
    mouseY.store(y, std::memory_order_relaxed);
}

void EnsoniqSD1AudioProcessor::injectMouseDown(int x, int y) {
    mouseX.store(x, std::memory_order_relaxed);
    mouseY.store(y, std::memory_order_relaxed);
    mouseButtons.fetch_or(1, std::memory_order_relaxed); // Set Left Click Bit
}

void EnsoniqSD1AudioProcessor::injectMouseUp(int x, int y) {
    mouseX.store(x, std::memory_order_relaxed);
    mouseY.store(y, std::memory_order_relaxed);
    mouseButtons.fetch_and(~1, std::memory_order_relaxed); // Clear Left Click Bit
}

//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EnsoniqSD1AudioProcessor();
}

// ==============================================================================
// ENSONIQ MAME DRIVER LIST REGISTRATION
// ==============================================================================
extern const game_driver driver____empty;
extern const game_driver driver_eps;
extern const game_driver driver_eps16p;
extern const game_driver driver_ks32;
extern const game_driver driver_sd1;
extern const game_driver driver_sd132;
extern const game_driver driver_sq1;
extern const game_driver driver_sq2;
extern const game_driver driver_sqrack;
extern const game_driver driver_vfx;
extern const game_driver driver_vfxsd;

const game_driver * const driver_list::s_drivers_sorted[11] =
{
    &driver____empty,
    &driver_eps,
    &driver_eps16p,
    &driver_ks32,
    &driver_sd1,
    &driver_sd132,
    &driver_sq1,
    &driver_sq2,
    &driver_sqrack,
    &driver_vfx,
    &driver_vfxsd,
};

const std::size_t driver_list::s_driver_count = 11;

// ==============================================================================
// Selfcheck
// ==============================================================================

bool EnsoniqSD1AudioProcessor::runSelfCheck()
{
    juce::StringArray errors;
    
        // --- UNSUPPORTED AU HOST WARNING CHECK ---
        // Whitelist DAWs that correctly support the Logic AU synchronization path.
        // For other hosts (like FL Studio), we flag to advise the VST3 version.
        if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
            juce::PluginHostType host;
            juce::String hostPath = host.getHostPath().toLowerCase();
            if (!host.isLogic() &&
                !host.isGarageBand() &&
                !host.isAbletonLive() &&
                !host.isReaper() &&
                !host.isStudioOne() &&
                !hostPath.contains("fender") &&
                !hostPath.contains("studio pro")) {
                
                isUnsupportedAUHost.store(true, std::memory_order_release);
            }
        }
    
    // --- 1. DOCUMENTS FOLDER WRITE ACCESS (For settings.xml and Master State) ---
        juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
        
        if (!ensoniqDir.exists()) {
            auto result = ensoniqDir.createDirectory();
            if (!result.wasOk()) {
                errors.add("- Cannot create directory: " + ensoniqDir.getFullPathName());
            }
        }
        
        if (ensoniqDir.exists()) {
            // Test parent folder
            juce::File testFile = ensoniqDir.getChildFile(".write_test");
            if (testFile.replaceWithText("test")) {
                testFile.deleteFile();
            } else {
                errors.add("- No write permission to: " + ensoniqDir.getFullPathName());
            }

            // --- 1.5 GLOBAL STATE FOLDER CHECK (New for Master Sync logic) ---
            juce::File globalStateDir = ensoniqDir.getChildFile("GlobalState");
            if (!globalStateDir.exists()) {
                auto result = globalStateDir.createDirectory();
                if (!result.wasOk()) {
                    errors.add("- Cannot create GlobalState directory in Documents!");
                }
            } else {
                // Test write access to existing GlobalState
                juce::File gsTestFile = globalStateDir.getChildFile(".gs_write_test");
                if (gsTestFile.replaceWithText("test")) {
                    gsTestFile.deleteFile();
                } else {
                    errors.add("- No write permission to GlobalState folder (used for Master NVRAM)!");
                }
            }
        }

    // --- 2. TEMP FOLDER WRITE ACCESS (Critical for MAME plugins ---
    juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    juce::File tempTestFile = tempDir.getChildFile(".vst_temp_test");
    if (tempTestFile.replaceWithText("test")) {
        tempTestFile.deleteFile();
    } else {
        errors.add("- No write permission to OS Temp directory!");
    }

    // --- 3. PLUGINS FOLDER EXISTENCE & EXTRACTION (Cross-platform Sandbox & Antivirus check) ---
    #ifndef _WIN32
        // macOS / Linux AU/VST3 Sandbox check
        juce::File exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        juce::File contentsDir = exeFile.getParentDirectory().getParentDirectory();
        juce::File pluginsDir = contentsDir.getChildFile("Resources").getChildFile("plugins");
        
        if (!pluginsDir.isDirectory() && wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
            juce::File candidate = exeFile;
            for (int i = 0; i < 6 && candidate.exists(); ++i) {
                if (candidate.getFileExtension() == ".component") {
                    pluginsDir = candidate.getChildFile("Contents/Resources/plugins");
                    break;
                }
                candidate = candidate.getParentDirectory();
            }
        }

        if (!pluginsDir.isDirectory()) {
            errors.add("- Missing MAME plugins folder: Resources/plugins");
        }
    #else
        // Windows: Extract plugins to Temp dir safely using a lock to prevent concurrent extraction
            juce::File tempMameDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("EnsoniqSD1_MAME_Data");
            juce::File requiredPluginFile = tempMameDir.getChildFile("plugins").getChildFile("layout").getChildFile("init.lua");

            if (!requiredPluginFile.existsAsFile())
            {
                juce::InterProcessLock extractLock("EnsoniqSD1_Plugin_Extract_Lock");
                if (extractLock.enter(5000)) // Wait up to 5s for another instance to finish unzipping
                {
                    // Double check after lock acquisition
                    if (!requiredPluginFile.existsAsFile())
                    {
                        tempMameDir.createDirectory();
                        juce::MemoryInputStream zipStream(BinaryData::mame_plugins_zip, BinaryData::mame_plugins_zipSize, false);
                        juce::ZipFile zip(zipStream);
                        auto result = zip.uncompressTo(tempMameDir);
                        if (!result.wasOk()) {
                            errors.add("- Failed to extract MAME plugins. Blocked by Antivirus or permissions?");
                        }
                    }
                    extractLock.exit();
                }
            }
        
        // Double-check that it actually exists after potential extraction (Catches aggressive real-time Antivirus deletion)
        if (!requiredPluginFile.existsAsFile()) {
            errors.add("- MAME layout plugins are missing from the Windows Temp directory.");
        }
    #endif

    // --- EVALUATE RESULTS ---
    if (errors.size() > 0) {
        isSelfCheckFailed.store(true, std::memory_order_release);
        selfCheckErrorMsg = errors.joinIntoString("\n");
        return false;
    }
    
    isSelfCheckFailed.store(false, std::memory_order_release);
    return true;
}

// ==============================================================================
// Check ROMS
// ==============================================================================

void EnsoniqSD1AudioProcessor::checkRomAndBootMame()
{
    // --- 0. LEGACY FALLBACK (For existing users) ---
    // Check if the old 'sd132.zip' exists in the base folder and migrate it
        juce::File baseDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1");
        juce::File legacyZip = baseDir.getChildFile("sd132.zip");
        if (legacyZip.existsAsFile()) {
            extractRomsFromZip(legacyZip);
        }
    
    // 1. Silent Auto-Fix: Check if the specific target files are in the ROMs folder
        juce::File romsDir = baseDir.getChildFile("ROMs");
        if (romsDir.exists() && romsDir.isDirectory()) {
            
            // Specifically look for our target zip files (instant check)
            juce::File zip1 = romsDir.getChildFile("sd132.zip");
            if (zip1.existsAsFile()) extractRomsFromZip(zip1);
            
            juce::File zip2 = romsDir.getChildFile("esq2x40_vfx.zip");
            if (zip2.existsAsFile()) extractRomsFromZip(zip2);

            // Auto-copy any loose .bin files placed directly in the ROMs folder
            copyRomsFromFolder(romsDir);
        }

    // 2. Verify the unzipped files in ROMs/sd132
    if (!verifyRomFiles()) {
        isRomMissing.store(true, std::memory_order_release);
        isRomInvalid.store(true, std::memory_order_release);
        isMameRunning = false;
    } else {
        isRomMissing.store(false, std::memory_order_release);
        isRomInvalid.store(false, std::memory_order_release);
        isMameRunning = true;
        
        // Safety net against std::terminate if thread wasn't joined
        if (mameThread.joinable()) {
            mameThread.join();
        }
        mameThread = std::thread(&EnsoniqSD1AudioProcessor::runMameEngine, this);
    }
}

// ==============================================================================
// ROM VALIDATION & EXTRACTION
// ==============================================================================
bool EnsoniqSD1AudioProcessor::verifyRomFiles()
{
    missingFilesList.clear();
    juce::File sd132Dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                             .getChildFile("EnsoniqSD1").getChildFile("ROMs").getChildFile("sd132");
    
    juce::StringArray expectedFiles = {
        "esqvfd_font_vfx.bin", "sd1_32_402_hi.bin", "sd1_32_402_lo.bin",
        "sd1_410_hi.bin", "sd1_410_lo.bin", "u34.bin", "u35.bin",
        "u36.bin", "u37.bin", "u38.bin"
    };

    bool allFound = true;
    for (const auto& expected : expectedFiles) {
        juce::File romFile = sd132Dir.getChildFile(expected);
        if (!romFile.existsAsFile()) {
            allFound = false;
            missingFilesList << "- " << expected << "\n";
        }
    }
    return allFound;
}

bool EnsoniqSD1AudioProcessor::extractRomsFromZip(const juce::File& zipFile)
{
    juce::File sd132Dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                             .getChildFile("EnsoniqSD1").getChildFile("ROMs").getChildFile("sd132");
    sd132Dir.createDirectory();

    juce::ZipFile zip(zipFile);
    bool extractedSomething = false;
    
    juce::StringArray expectedFiles = {
        "esqvfd_font_vfx.bin", "sd1_32_402_hi.bin", "sd1_32_402_lo.bin",
        "sd1_410_hi.bin", "sd1_410_lo.bin", "u34.bin", "u35.bin",
        "u36.bin", "u37.bin", "u38.bin"
    };

    for (int i = 0; i < zip.getNumEntries(); ++i) {
        auto* entry = zip.getEntry(i);
        if (entry != nullptr) {
            juce::String entryName = entry->filename.replaceCharacter('\\', '/');
            juce::String baseName = entryName.substring(entryName.lastIndexOfChar('/') + 1).toLowerCase();

            if (expectedFiles.contains(baseName)) {
                std::unique_ptr<juce::InputStream> inStream(zip.createStreamForEntry(i));
                if (inStream != nullptr) {
                    juce::File destFile = sd132Dir.getChildFile(baseName);
                    destFile.deleteFile(); // Overwrite if exists
                    std::unique_ptr<juce::OutputStream> outStream(destFile.createOutputStream());
                    if (outStream != nullptr) {
                        outStream->writeFromInputStream(*inStream, -1);
                        extractedSomething = true;
                    }
                }
            }
        }
    }
    return extractedSomething;
}

bool EnsoniqSD1AudioProcessor::copyRomsFromFolder(const juce::File& sourceDir)
{
    juce::File sd132Dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                             .getChildFile("EnsoniqSD1").getChildFile("ROMs").getChildFile("sd132");
    sd132Dir.createDirectory();

    bool copiedSomething = false;
    juce::StringArray expectedFiles = {
        "esqvfd_font_vfx.bin", "sd1_32_402_hi.bin", "sd1_32_402_lo.bin",
        "sd1_410_hi.bin", "sd1_410_lo.bin", "u34.bin", "u35.bin",
        "u36.bin", "u37.bin", "u38.bin"
    };

    for (const auto& expected : expectedFiles) {
        juce::File srcFile = sourceDir.getChildFile(expected);
        if (srcFile.existsAsFile()) {
            juce::File destFile = sd132Dir.getChildFile(expected);
            // Prevent copying onto itself if the source IS the destination
            if (srcFile != destFile) {
                destFile.deleteFile(); // Overwrite if exists
                if (srcFile.copyFileTo(destFile)) {
                    copiedSomething = true;
                }
            }
        }
    }
    return copiedSomething;
}

// ==============================================================================
// HARDWARE STATE DECODING API & ROM PARSER
// ==============================================================================

void EnsoniqSD1AudioProcessor::buildVfdDictionary()
{
    segmentToAscii.clear();
    
    // The SD-1 uses ASCII characters from 32 (Space) to 95 ('_').
    // If a segment mask doesn't match, we default to space.
    segmentToAscii[0x0000] = ' ';

    juce::File fontRom = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("EnsoniqSD1").getChildFile("ROMs")
        .getChildFile("sd132").getChildFile("esqvfd_font_vfx.bin");

    // The esqvfd_font_vfx.bin ROM is exactly 192 bytes (96 characters x 16-bit words)
    if (fontRom.existsAsFile() && fontRom.getSize() == 192) {
        juce::MemoryBlock romData;
        fontRom.loadFileAsData(romData);
        const uint8_t* data = static_cast<const uint8_t*>(romData.getData());
        
        for (int i = 0; i < 64; ++i) {
            uint16_t segmentMask = (data[i * 2] << 8) | data[(i * 2) + 1];
            char asciiChar = static_cast<char>(32 + i); // 32 is ' ' in ASCII

            if (segmentMask != 0x0000 && segmentToAscii.find(segmentMask) == segmentToAscii.end()) {
                segmentToAscii[segmentMask] = asciiChar;
            }
        }
    }
}

bool EnsoniqSD1AudioProcessor::isHardwareLedOn(int ledBitIndex)
{
    uint32_t mask = ledStateMask.load(std::memory_order_relaxed);
    return (mask & (1U << ledBitIndex)) != 0;
}

juce::String EnsoniqSD1AudioProcessor::getHardwareVfdText()
{
    juce::String vfdText = "";
    
    for (int i = 0; i < VFD_SIZE; ++i) {
        uint16_t rawMask = vfdSegments[i].load(std::memory_order_relaxed);
        
        // MAME sets the highest bit (0x8000) for the underline attribute.
        // We strip it here to match the pure character shape from our dictionary.
        uint16_t cleanSegment = rawMask & 0x7FFF;
        
        auto it = segmentToAscii.find(cleanSegment);
        if (it != segmentToAscii.end()) {
            vfdText += it->second;
        } else {
            vfdText += " "; // Fallback for unrecognized symbols to keep UI clean
        }
    }
    return vfdText;
}

// ==============================================================================
// MAME OUTPUT NOTIFIER (VFD & LED CATCHER)
// ==============================================================================
void EnsoniqSD1AudioProcessor::mameOutputNotifier(const char *outname, s32 value, void *param)
{
    auto* processor = static_cast<EnsoniqSD1AudioProcessor*>(param);
    if (processor == nullptr || outname == nullptr) return;

    // 1. Catch the global LED state (32-bit bitmask)
    if (strcmp(outname, "lights") == 0) {
        processor->ledStateMask.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
    }
    // 2. Catch the VFD 14-segment characters (vfd0 through vfd79)
    else if (strncmp(outname, "vfd", 3) == 0) {
        int idx = atoi(outname + 3);
        if (idx >= 0 && idx < VFD_SIZE) {
            processor->vfdSegments[idx].store(static_cast<uint16_t>(value), std::memory_order_relaxed);
        }
    }
}

// ==============================================================================
// RUN MAME
// ==============================================================================

void EnsoniqSD1AudioProcessor::runMameEngine()
{
#ifdef _WIN32
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (hTask != NULL) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
    }
    else {
        // Fallback if no MMCSS
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
#endif

    // --- 1. Prevent MAME from hijacking the host OS audio/video drivers ---
#ifdef _WIN32
    _putenv("SDL_VIDEODRIVER=dummy");
    _putenv("SDL_AUDIODRIVER=dummy");
#else
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("SDL_MAC_BACKGROUND_APP", "1", 1);
    setenv("SDL_EVENT_HANDLING", "0", 1);
#endif
    
    std::vector<std::string> args;
    args.push_back("mame");
    args.push_back("sd132");
    
    // --- 2. ROM PATH CONFIGURATION ---
    juce::File romsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                     .getChildFile("EnsoniqSD1").getChildFile("ROMs");
    romsDir.createDirectory();

        #ifdef _WIN32
            // Ensure strict UTF-8 string conversion on Windows to prevent MAME boot failures
            // if the directory contains non-ASCII characters.
            std::string safePath = romsDir.getFullPathName().toUTF8().getAddress();
        #else
            // macOS/Linux natively handle paths well
            std::string safePath = romsDir.getFullPathName().toStdString();
        #endif

            // Set the base ROM path for MAME
            args.push_back("-rompath");
            args.push_back(safePath);
    
        // --- 2.5 INITIAL MEDIA MOUNTING (COLD BOOT) ---
        // At DAW project load, MAME is booted from scratch. We must pass the media via
        // command line so the SD-1 OS detects them correctly during hardware initialization!
        {
            std::lock_guard<std::mutex> lock(mediaMutex);
            if (isFloppyLoaded.load() && !pendingFloppyPath.empty()) {
                args.push_back("-flop");
                args.push_back(pendingFloppyPath);
                
                // Clear the dynamic load flag so VstOsdInterface doesn't try to double-load it
                requestFloppyLoad.store(false, std::memory_order_release);
            }
            if (isCartLoaded.load() && !pendingCartPath.empty()) {
                args.push_back("-cart");
                args.push_back(pendingCartPath);
                
                // Clear the dynamic load flag so VstOsdInterface doesn't try to double-load it
                requestCartLoad.store(false, std::memory_order_release);
            }
        }
    
    // --- 3. PLUGINS PATH CONFIGURATION ---
    #ifdef _WIN32
        // Extraction and verification is now securely handled in runSelfCheck() beforehand
        juce::File tempMameDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("EnsoniqSD1_MAME_Data");
        juce::String finalPluginsPath = tempMameDir.getChildFile("plugins").getFullPathName();
        
        args.push_back("-pluginspath");
        args.push_back(finalPluginsPath.toUTF8().getAddress());
    #else
        // Original macOS solution
        juce::File exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        juce::File pluginsDir = exeFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("plugins");
        
        // AU Sandbox fallback routine
        if (!pluginsDir.isDirectory() && wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
            juce::File candidate = exeFile;
            for (int i = 0; i < 6 && candidate.exists(); ++i) {
                if (candidate.getFileExtension() == ".component") {
                    pluginsDir = candidate.getChildFile("Contents/Resources/plugins");
                    break;
                }
                candidate = candidate.getParentDirectory();
            }
        }
        
        args.push_back("-pluginspath");
        args.push_back(pluginsDir.getFullPathName().toStdString());
    #endif

    args.push_back("-plugin");
    args.push_back("layout");

    // Use software rendering and our custom OSD sound module
    args.push_back("-video");
    args.push_back("soft");
    args.push_back("-sound");
    args.push_back("osd");
    args.push_back("-midiin");
    args.push_back("VST MIDI");
    
    // Disable MAME's internal pacing (we control this strictly via audio throttle)
    args.push_back("-nothrottle");
    args.push_back("-nosleep");
    args.push_back("-nowaitvsync");
    args.push_back("-nowindow");
    args.push_back("-nobackground_input");

    args.push_back("-noreadconfig");
    args.push_back("-skip_gameinfo");
    args.push_back("-samplerate");
    args.push_back(std::to_string(static_cast<int>(hostSampleRate.load())));
    
    // Disable physical inputs to prevent MAME from stealing focus
    args.push_back("-keyboardprovider");
    args.push_back("none");
    args.push_back("-mouseprovider");
    args.push_back("none");
    args.push_back("-joystickprovider");
    args.push_back("none");
        
    // --- 4. SANDBOX & MASTER STATE SYNCHRONIZATION ---
        juce::File instDir(instanceTempDir);
        instDir.createDirectory();

        juce::File masterDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                .getChildFile("EnsoniqSD1").getChildFile("GlobalState");
        masterDir.createDirectory();

        // Safely copy NVRAM from Master to Instance Sandbox
        juce::File masterNvram = masterDir.getChildFile("nvram").getChildFile("sd132");
        juce::File instNvram = instDir.getChildFile("nvram").getChildFile("sd132");
        
        if (masterNvram.exists() && masterNvram.isDirectory()) {
            instNvram.createDirectory();
            juce::File srcOsram = masterNvram.getChildFile("osram");
            juce::File srcSeqram = masterNvram.getChildFile("seqram");
            
            if (srcOsram.existsAsFile()) srcOsram.copyFileTo(instNvram.getChildFile("osram"));
            if (srcSeqram.existsAsFile()) srcSeqram.copyFileTo(instNvram.getChildFile("seqram"));
        }

        // Assign MAME isolated paths for this instance
        args.push_back("-nvram_directory");
    #ifdef _WIN32
        args.push_back(instDir.getChildFile("nvram").getFullPathName().toUTF8().getAddress());
        args.push_back("-cfg_directory");
        args.push_back(instDir.getChildFile("cfg").getFullPathName().toUTF8().getAddress());
        args.push_back("-state_directory");
        args.push_back(instDir.getChildFile("state").getFullPathName().toUTF8().getAddress());
    #else
        args.push_back(instDir.getChildFile("nvram").getFullPathName().toStdString());
        args.push_back("-cfg_directory");
        args.push_back(instDir.getChildFile("cfg").getFullPathName().toStdString());
        args.push_back("-state_directory");
        args.push_back(instDir.getChildFile("state").getFullPathName().toStdString());
    #endif
           
    // Boot the headless CLI Frontend
    auto* mameOpts = new osd_options();
    auto* headlessOsd = new VstOsdInterface(this, *mameOpts);
    auto* frontend = new cli_frontend(*mameOpts, *headlessOsd);
    
    frontend->execute(args);
    
    delete frontend;
    delete headlessOsd;
    delete mameOpts;
}
