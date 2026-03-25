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

#include <fstream>

// Global lock to prevent multiple MAME instances from running in the same process
static std::atomic<bool> globalMameLock { false };

// --- AU Highlander Instance Manager ---
static std::mutex instanceMutex;
static EnsoniqSD1AudioProcessor* activeMameProcessor = nullptr;

// MAME Versioning stubs required by the linker
extern const char bare_build_version[] = "0.286";
extern const char bare_vcs_revision[] = "";
extern const char build_version[] = "0.286";

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
        
        // --- VISSZAÁLLÍTVA AZ EREDETI ATOMPONTOS VST3 MATEKRA ---
        if (needAnchorSync.load(std::memory_order_acquire) && mameMachine != nullptr) {
            anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
            anchorDawSample.store(currentWritePos, std::memory_order_relaxed);
            needAnchorSync.store(false, std::memory_order_release);
        }

    // MAME outputs interleaved audio. STRIDE = 5 (Main L, Main R, Aux L, Aux R, Floppy)
    for (int i = 0; i < numSamples; ++i) {

        // --- OPTIMIZATION ---
        // Replace slow modulo (%) with ultra-fast bitwise AND (&).
        // This relies on RING_BUFFER_SIZE being a strict power of 2!
        int index = currentWritePos & (RING_BUFFER_SIZE - 1);

        ringBufferL[index] = pcmBuffer[i * 5 + 0] / 32768.0f;
        ringBufferR[index] = pcmBuffer[i * 5 + 1] / 32768.0f;

        ringBufferAuxL[index] = pcmBuffer[i * 5 + 2] / 32768.0f;
        ringBufferAuxR[index] = pcmBuffer[i * 5 + 3] / 32768.0f;

        currentWritePos++;
    }
    
    totalWritten.store(currentWritePos, std::memory_order_release);
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

        // IF BUFFER IS EMPTY AND SENDING SYSEX ---
        if (nextRead == currentWrite && isTransmittingSysEx.load(std::memory_order_acquire)) {
            isTransmittingSysEx.store(false, std::memory_order_release);
        }

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
    
    render_target* main_target = nullptr;

    int saveFrameDelay = 0;
    int loadFrameDelay = 0;
    int frameSkipCounter = 0;
    
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
        
        if (!processor->isMameRunningFlag()) {
                mame_machine->schedule_exit();
                return;
            }
        
        if (skip_redraw) return;
        
        // --- DYNAMIC VIEW & RESIZE SWITCHING ---
        bool viewChanged = processor->requestViewChange.exchange(false, std::memory_order_acquire);
                bool sizeChanged = processor->requestRenderResize.exchange(false, std::memory_order_acquire);

                if (viewChanged || sizeChanged) {
                    int targetViewIdx = processor->requestedViewIndex.load(std::memory_order_acquire);
                    int newW = processor->windowWidth.load(std::memory_order_acquire);
                    int newH = processor->windowHeight.load(std::memory_order_acquire);

                    if (newW > 0 && newH > 0) {
                        if (viewChanged) {
                            int mameViewMap[4] = { 1, 0, 2, 3 };
                            main_target->set_view(mameViewMap[targetViewIdx % 4]);
                        }
                        main_target->set_bounds(newW, newH);
                    }
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
                    image.load(processor->pendingFloppyPath);
                    break;
                }
            }
        }

        // --- CARTRIDGE MOUNTING ---
        if (processor->requestCartLoad.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(processor->mediaMutex);
            
            for (device_image_interface &image : image_interface_enumerator(mame_machine->root_device())) {
                if (image.brief_instance_name() == "cart" || image.brief_instance_name() == "cartridge") {
                    image.load(processor->pendingCartPath);
                    break;
                }
            }
        }
        
        // Disable on-screen popups (e.g., "State loaded")
        mame_machine->ui().popup_time(0, " ");
        
        // --- FRAME SKIPPING
        frameSkipCounter++;
        if (frameSkipCounter % 2 != 0) {
            return;
        }

        render_target *target = mame_machine->render().first_target();
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
                
                if (isMouseCursor) continue; // Kihagyjuk a Hash-ből! A kép stabil marad.

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
            render_target* target = mame_machine->render().first_target();
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
            // --- 2. VST BUTTON AUTOMATION (Direct Hardware Memory Injection) ---
            // ==============================================================================
            // We bypass the mouse completely and directly pull the circuits on the MAME motherboard!
            for (size_t i = 0; i < processor->sd1Buttons.size(); ++i) {
                
                // Lock-free read from the DAW's automation lane
                float val = processor->buttonParams[i]->load(std::memory_order_relaxed);
                bool isPressed = (val > 0.5f);

                // Locate the specific hardware port (e.g., ":buttons_32")
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
    
    virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) override { return {}; };

    virtual std::vector<osd::midi_port_info> list_midi_ports() override {
        std::vector<osd::midi_port_info> ports;
        osd::midi_port_info info;
        
        info.name = "VST MIDI"; 
        info.input = true;
        info.output = false;
        info.default_input = true;
        info.default_output = false;
        
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

    juce::StringArray bufferSizes = { "256", "512", "1024", "2048", "4096", "8192" };
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
                    int sizes[] = { 256, 512, 1024, 2048, 4096, 8192 };
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
    juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::File settingsFile = docsDir.getChildFile("EnsoniqSD1").getChildFile("settings.xml");

    if (settingsFile.existsAsFile()) {
        if (auto xml = juce::XmlDocument::parse(settingsFile)) {
            // Read saved attributes from the XML file
            int bufIdx = xml->getIntAttribute("buffer_size", 2);
            int viewIdx = xml->getIntAttribute("layout_view", 0);
            savedWindowWidth = xml->getIntAttribute("window_width", 1200);
            savedWindowHeight = xml->getIntAttribute("window_height", 900);

            // PROTECTION: NEVER call setValueNotifyingHost() inside the processor's constructor!
            // Doing so forces the host (especially strict VST3 hosts like Studio One/Pro) to
            // recalculate parameters and buffers while it's still initializing the plugin,
            // causing immediate EXC_BAD_ACCESS memory crashes.
            // p->setValue() quietly updates the APVTS state without disrupting the DAW.
            if (auto* p = apvts.getParameter("buffer_size"))
                p->setValue(p->convertTo0to1(bufIdx));
                
            if (auto* p = apvts.getParameter("layout_view"))
                p->setValue(p->convertTo0to1(viewIdx));

            // Since the "silent" setValue might not immediately trigger the parameterChanged
            // callback during the constructor phase, we manually update our critical internal
            // atomic variables here just to be absolutely safe:
            int sizes[] = { 256, 512, 1024, 2048, 4096, 8192 };
            if (bufIdx >= 0 && bufIdx < 6) {
                mameBufferThreshold.store(sizes[bufIdx], std::memory_order_relaxed);
            }
            requestedViewIndex.store(viewIdx, std::memory_order_release);
        }
    }
}

EnsoniqSD1AudioProcessor::EnsoniqSD1AudioProcessor()
     : AudioProcessor (BusesProperties()
                       .withOutput ("Main Out", juce::AudioChannelSet::stereo(), true)
                       .withOutput ("Aux Out",  juce::AudioChannelSet::stereo(), false)
                       ),
       apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    apvts.addParameterListener("volume", this);
    apvts.addParameterListener("data_entry", this);
    apvts.addParameterListener("pitch_bend", this);
    apvts.addParameterListener("mod_wheel", this);
    apvts.addParameterListener("buffer_size", this);
    apvts.addParameterListener("layout_view", this);
    
        // --- CACHE VST BUTTON POINTERS FOR MAME (0% CPU overhead) ---
        // We store the raw atomic pointers so the MAME audio thread can read them instantly
        // without locking or string lookups.
    for (const auto& btn : sd1Buttons) {
            buttonParams.push_back(apvts.getRawParameterValue(btn.paramID));
        }
    
    // --- SINGLE INSTANCE LOCKING ---
        isMasterInstance = false;
        isBlockedByAnotherInstance.store(false, std::memory_order_release);

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
    // AU Highlander cleanup
        {
            std::lock_guard<std::mutex> lock(instanceMutex);
            if (activeMameProcessor == this) {
                activeMameProcessor = nullptr;
            }
        }
        
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
    if (isMasterInstance) {
        isMameRunning.store(false, std::memory_order_release);
                
        mameThrottleEvent.signal();
        mameStateEvent.signal();
        
        if (mameThread.joinable()) {
            mameThread.join();
        }
       
        isMasterInstance = false;
        mameHasStarted.store(false, std::memory_order_release);
        isBlockedByAnotherInstance.store(true, std::memory_order_release);
        
        // No lock for VST3
        globalMameLock.store(false);
    }
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

    // SINGLE INSTANCE LOCK (Format-specific handling)

        if (!isMasterInstance) {
            bool acquired = false;

            if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
                            // AU Branch: Forced takeover (Highlander)
                            std::lock_guard<std::mutex> lock(instanceMutex);
                            
                            if (activeMameProcessor != this) {
                                if (activeMameProcessor != nullptr) {
                                    // NEW: Asynchronous shutdown instead of blocking join()
                                    auto* old = activeMameProcessor;
                                    old->isMameRunning.store(false, std::memory_order_release);
                                    old->mameThrottleEvent.signal();
                                    old->mameStateEvent.signal();
                                    
                                    // Poll with a fast timeout to keep the Logic audio thread alive
                                    int waited = 0;
                                    while (old->mameHasStarted.load() && waited < 100) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                        waited++;
                                    }
                                    
                                    // Join is now practically instantaneous if MAME exited
                                    if (old->mameThread.joinable()) {
                                        old->mameThread.join();
                                    }
                                }
                                
                                // Now we (the new version) are the Masters
                                activeMameProcessor = this;
                                acquired = true;
                            }
            } else {
                // VST3 BRANCH: Original blocking (synchronous) wait
                // VST3 waits properly until the host program terminates the old instance.
                int retries = 60;
                while (retries > 0) {
                    bool expected = false;
                    if (globalMameLock.compare_exchange_strong(expected, true)) {
                        acquired = true;
                        break; // We successfully obtained the lock!
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    retries--;
                }
            }

            // --- CONTINUE IF SUCCESSFUL ---
            if (acquired) {
                isMasterInstance = true;
                isBlockedByAnotherInstance.store(false, std::memory_order_release);
                
                juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
                juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
                if (!ensoniqDir.exists()) ensoniqDir.createDirectory();
            } else {
                // If the 3-second wait has elapsed on the VST3 thread
                isBlockedByAnotherInstance.store(true, std::memory_order_release);
                return;
            }
        }
    
    hostSampleRate.store(sampleRate);
    
    auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("buffer_size"));
        if (choiceParam != nullptr) {
            int sizes[] = { 256, 512, 1024, 2048, 4096, 8192 };
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
                        
                        // If this is a plugin scanner or validator (e.g., Cubase vstscanner.exe),
                        // we have absolutely no intention of running MAME and writing to the file system!
                        if (hostPath.contains("scanner") || hostPath.contains("validator")) {
                            isMameRunning = false;
                            return;
                        }
        
        initialSampleRate.store(sampleRate); 
        
        juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
        juce::File romFile = ensoniqDir.getChildFile("sd132.zip");

            // ROM CHECK
            
            if (!romFile.existsAsFile()) {
                        isRomMissing.store(true, std::memory_order_release);
                        isRomInvalid.store(false, std::memory_order_release);
                        isMameRunning = false;
                    } else if (!verifyRomChecksums(romFile)) {
                        // File exists but contains incorrect or missing inner files
                        isRomMissing.store(false, std::memory_order_release);
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

        // We clear the entire ring buffer to ensure that the preloaded section is completely silent
        for (int i = 0; i < RING_BUFFER_SIZE; ++i) {
            ringBufferL[i] = 0.0f;
            ringBufferR[i] = 0.0f;
            ringBufferAuxL[i] = 0.0f;
            ringBufferAuxR[i] = 0.0f;
        }
        // NEW: Flag that prepareToPlay just finished
        if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
            prepareWasCalled.store(true, std::memory_order_release);
        }
}

void EnsoniqSD1AudioProcessor::releaseResources()
{
}

// ==============================================================================
// SYSEX FILE IMPORTER (HARDWARE BAUD RATE EMULATION)
// ==============================================================================
void EnsoniqSD1AudioProcessor::loadSysExFile(const juce::File& syxFile)
{
    if (mameMachine == nullptr) return;

    juce::MemoryBlock syxData;
    if (syxFile.loadFileAsData(syxData) && syxData.getSize() > 0) { // security check
        const uint8_t* data = static_cast<const uint8_t*>(syxData.getData());
        size_t size = syxData.getSize();

        // 0. signal to gui
        isTransmittingSysEx.store(true, std::memory_order_release);

        // 1. get internal MAME time
        double startTime = mameMachine->time().as_double() + 0.5;

        // 2. calculate MIDI speed (0.00035 mp / byte)
        double timePerByte = 0.00035;

        // 3. psuh it to MIDI buffer
        for (size_t i = 0; i < size; ++i) {
            double targetTime = startTime + (i * timePerByte);
            pushMidiByte(data[i], targetTime);
        }
    }
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
    
    // 1. Get the ACTUAL number of channels provided by the host in this specific callback
    int numChannels = buffer.getNumChannels();
    int totalNumInputChannels = getTotalNumInputChannels();
    
    // 2. PROTECTION: Safely clear only the output channels that physically exist in the buffer.
    // The VST3 standard (especially in strict DAWs like Studio One/Pro) might pass nullptrs
    // for unrouted outputs even if the reported channel count is higher. We MUST check for nullptr!
    for (int i = totalNumInputChannels; i < numChannels; ++i) {
        auto* channelData = buffer.getWritePointer(i);
        if (channelData != nullptr) {
            juce::FloatVectorOperations::clear(channelData, buffer.getNumSamples());
        }
    }
    
    // --- State and Boot Checks ---
    if (isBlockedByAnotherInstance.load(std::memory_order_acquire)) return;
    if (sampleRateMismatch.load(std::memory_order_acquire)) return;
    if (!isMameRunningFlag()) return;
    
    int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return; // Safety check
    
    uint64_t currentReadPos = totalRead.load(std::memory_order_acquire);
    int threshold = mameBufferThreshold.load(std::memory_order_relaxed);
    double sr = hostSampleRate.load(std::memory_order_relaxed);
    
    // Security boot check
    if (mameMachine == nullptr) {
        totalRead.store(currentReadPos + numSamples, std::memory_order_release);
        return;
    }
    
    // ========================================================
    // AU SPECIFIC LOGIC PRO SYNC FIX (VST3 completely bypassed)
    // ========================================================
    if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
        bool currentOffline = isNonRealtime();
        bool offlineChanged = (currentOffline != lastOfflineState);
        lastOfflineState = currentOffline;
        
        bool freshPrepare = prepareWasCalled.exchange(false, std::memory_order_acq_rel);
        bool transportJumped = false;
        
        if (auto* ph = getPlayHead()) {
            if (auto pos = ph->getPosition()) {
                if (pos->getIsPlaying()) {
                    int64_t currentPos = pos->getTimeInSamples().orFallback(0);
                    if (std::abs(currentPos - lastPlayheadPos) > numSamples * 4) {
                        transportJumped = true;
                    }
                    lastPlayheadPos = currentPos + numSamples;
                } else {
                    lastPlayheadPos = pos->getTimeInSamples().orFallback(0);
                }
            }
        }
        
        if (offlineChanged && !freshPrepare) {
            needAnchorSync.store(true, std::memory_order_release);
            uint64_t newWritePos = currentReadPos + mameBufferThreshold.load(std::memory_order_relaxed);
            totalWritten.store(newWritePos, std::memory_order_release);
            
            for (int j = 0; j < RING_BUFFER_SIZE; ++j) {
                ringBufferL[j] = 0.0f; ringBufferR[j] = 0.0f;
                ringBufferAuxL[j] = 0.0f; ringBufferAuxR[j] = 0.0f;
            }
            
            midiReadPos.store(midiWritePos.load(std::memory_order_acquire), std::memory_order_release);
        }
        
        if (transportJumped && !currentOffline) {
            needAnchorSync.store(true, std::memory_order_release);
        }
    }
    
    
    bool isOffline = isNonRealtime();
        
        // ========================================================
        // 0. WAIT FOR PERFECT ANCHOR (HYBRID MODEL)
        // ========================================================
        if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
            // --- AU INSTANT ANCHOR FIX ---
            if (needAnchorSync.exchange(false, std::memory_order_acq_rel) && mameMachine != nullptr) {
                anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
                anchorDawSample.store(totalWritten.load(std::memory_order_acquire), std::memory_order_relaxed);
            }
        } else {
            // --- ORIGINAL VST3 WAIT LOOP (UNTOUCHED) ---
            if (needAnchorSync.load(std::memory_order_acquire)) {
                mameThrottleEvent.signal(); // Wake up MAME!

                // We determine how long we are allowed to block the audio thread.
                int timeoutMs = isNonRealtime() ? 2000 : 2;
                int waitMs = 0;

                // Wait loop: Check if MAME has established the exact audio anchor yet
                while (needAnchorSync.load(std::memory_order_acquire) && waitMs < timeoutMs) {
#if defined(_WIN32)
    // --- WINDOWS ---
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

#elif defined(__APPLE__)
    // --- macOS ---
    if (pthread_main_np() == 0) { // Only wait if we are NOT on the main thread
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    else {
        break; // Safety breakout for macOS main thread rendering
    }

#else
    // --- LINUX ---
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
                        anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
                        anchorDawSample.store(totalWritten.load(std::memory_order_acquire), std::memory_order_relaxed);
                        needAnchorSync.store(false, std::memory_order_release);
                    }
                    else {
                        return;
                    }
                }
            }
        }
        
        double t_anchor = anchorMameTime.load(std::memory_order_relaxed);
        uint64_t s_anchor = anchorDawSample.load(std::memory_order_relaxed);
        
        // ========================================================
        // 1. TIMESTAMPED MIDI INJECTION
        // ========================================================
        
        bool isPlaying = false;
        if (auto* ph = getPlayHead()) {
            if (auto pos = ph->getPosition()) {
                isPlaying = pos->getIsPlaying();
            }
        }
        
        for (const auto metadata : midiMessages) {
                int eventOffset = metadata.samplePosition;
                double targetMameTime;

                if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit && !isPlaying && !isOffline) {
                    // Piano Roll Instant Fix
                    targetMameTime = mameMachine->time().as_double() + (static_cast<double>(eventOffset) / sr);
                } else {
                    // --- ISSUE FIX: SAFE MATH ---
                    uint64_t targetSample = currentReadPos + eventOffset + threshold;
                    targetMameTime = t_anchor + static_cast<double>(static_cast<int64_t>(targetSample) - static_cast<int64_t>(s_anchor)) / sr;
                }
                
                auto msg = metadata.getMessage();
                const uint8_t* rawData = msg.getRawData();
                for (int i = 0; i < msg.getRawDataSize(); ++i) {
                    pushMidiByte(rawData[i], targetMameTime);
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
            
            // --- SPONTANEOUS AU MIDI DELAY FIX ---
            if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
                needAnchorSync.store(true, std::memory_order_release);
            }
        }
        
        totalRead.store(currentReadPos, std::memory_order_release);
        mameThrottleEvent.signal(); // Wake MAME for the next round
        
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

//==============================================================================
void EnsoniqSD1AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // 1. Save VST parameters to XML
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    // --- SAVE WINDOW SIZE TO XML ---
    xml->setAttribute("ui_width", savedWindowWidth);
    xml->setAttribute("ui_height", savedWindowHeight);

    if (isMameRunningFlag()) {
        // 2. Request MAME to save its RAM state and wait (up to 2 seconds) for it to finish.
        mameStateEvent.reset();
        mameStateIsReady.store(false, std::memory_order_release);
        requestMameSave.store(true, std::memory_order_release);
        
        // Wake up MAME in case it was throttled
        mameThrottleEvent.signal();
        
        if (mameStateEvent.wait(2000)) {
            // 3. Once MAME is done, read the temporary state file
            juce::File tempStateFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sd132").getChildFile("vst_temp.sta");
            
            if (tempStateFile.existsAsFile()) {
                juce::MemoryBlock mameData;
                tempStateFile.loadFileAsData(mameData);
                // Encode the entire MAME hardware state (RAM, CPU, etc.) as Base64 and embed it in the DAW's project file.
                xml->setAttribute("mame_state", mameData.toBase64Encoding());
            }
        }
    }
    
    copyXmlToBinary (*xml, destData);
}

void EnsoniqSD1AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr) {
        
        // 1. Restore VST Automation Parameters
        if (xmlState->hasTagName (apvts.state.getType())) {
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
        }

        // --- RESTORE WINDOW SIZE ---
        savedWindowWidth = xmlState->getIntAttribute("ui_width", 0);
        savedWindowHeight = xmlState->getIntAttribute("ui_height", 0);
        
        // 2. Restore complete MAME hardware state
        if (xmlState->hasAttribute("mame_state")) {
            juce::MemoryBlock mameData;
            mameData.fromBase64Encoding(xmlState->getStringAttribute("mame_state"));
            
            // Always save temp file (either MAME booted or not)
            juce::File tempStateDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sd132");
            tempStateDir.createDirectory();
            
            juce::File tempStateFile = tempStateDir.getChildFile("vst_temp.sta");
            tempStateFile.replaceWithData(mameData.getData(), mameData.getSize());
            
            // Load flag, MAME read it if booted
            requestMameLoad.store(true, std::memory_order_release);
            
            // Wait only for load when MAME is already run (e.g. preset change)

#if defined(__APPLE__)
    // --- macOS ---
    if (pthread_main_np() == 0) {
        mameStateEvent.wait(2000);
    }
#else
    // --- WINDOWS and LINUX ---
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        mameStateEvent.wait(2000);
    }
#endif
                                
                }
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
// ROM VALIDATION (USING JUCE_CRYPTOGRAPHY)
// ==============================================================================
bool EnsoniqSD1AudioProcessor::verifyRomChecksums(const juce::File& zipFile)
{
    juce::ZipFile zip(zipFile);
    
    // Expected files and their exact SHA-256 hashes
    std::map<juce::String, juce::String> expectedHashes = {
        {"esqvfd_font_vfx.bin", "ab2f7ddc6ab7fafaf07985d01788197849cdaeb5a4a7d9f2f85098dfd65edf01"},
        {"sd1_32_402_hi.bin",   "90ae35de8661f5de0793b6ea59a4d6524e90c0828a29e6ea8906ff759116136d"},
        {"sd1_32_402_lo.bin",   "6b0c1235c4f813ce8698e89d66933e9c7c9168f4a095c9e2a50add7fe729481c"},
        {"sd1_410_hi.bin",      "1d6d6150373fb070da8b1a6da57762749bda9210e0ca5536441bb8194a3cafb7"},
        {"sd1_410_lo.bin",      "e3e42beca41989561c0d2a8266e48549561650a7606bb8a0d75b438847e8bd0c"},
        {"u34.bin",             "7a6e6e76da7eb8de5cbc3a0a2bfb27a461e312facdcc0b7ecc42b9d1eb261e12"},
        {"u35.bin",             "1df911a97e0e5a334d9345ba5e47eac7794d083282012f7ecf70901b88cf7e08"},
        {"u36.bin",             "2fdb401bea78eb323fa55408760a73319aeae68b465f193dc7a46d1b21277cdd"},
        {"u37.bin",             "e08931013c8aca2460b4f2c3512e1d3e9a610a7f921e22012bb13bd23a3e56d7"},
        {"u38.bin",             "2f185a185961a1c14472c2b706642c0d9e7a0792d57d946a349840905782e5ca"}
    };

    for (const auto& pair : expectedHashes)
    {
        int index = zip.getIndexOfFileName(pair.first);
        
        // Return false if a required ROM file is entirely missing from the zip
        if (index < 0)
            return false;

        // Create an input stream to read the file directly from the zip without extracting
        std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(index));
        if (stream == nullptr)
            return false;

        // Calculate SHA-256 hash directly from the input stream
        juce::SHA256 hash(*stream);
        
        // Compare with the expected hash (case-insensitive)
        if (hash.toHexString().toLowerCase() != pair.second.toLowerCase())
            return false;
    }

    return true; // All files present and hashes matched
}

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

    // Prevent MAME from hijacking the host OS audio/video drivers
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
    
    juce::File ensoniqDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1");
    
#ifdef _WIN32
    // Ensure strict UTF-8 string conversion on Windows to prevent MAME boot failures
    // if the user's home directory contains non-ASCII characters (e.g., accents like "Géza").
    std::string safePath = ensoniqDir.getFullPathName().toUTF8().getAddress();
#else
    // macOS natively handles paths well, but we keep it standardized
    std::string safePath = ensoniqDir.getFullPathName().toStdString();
#endif

    // Set the base ROM path
    args.push_back("-rompath");
    args.push_back(safePath);
                     
    // --- 3. PLUGINS PATH CONFIGURATION ---
#ifdef _WIN32
    // Windows: Unzip Lua plugins to Temp dir safely
    juce::File tempMameDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("EnsoniqSD1_MAME_Data");

    // Only extract on the first run
    if (!tempMameDir.exists())
    {
        tempMameDir.createDirectory();

        // Extracting from BinaryData
        juce::MemoryInputStream zipStream(BinaryData::mame_plugins_zip, BinaryData::mame_plugins_zipSize, false);
        juce::ZipFile zip(zipStream);
        zip.uncompressTo(tempMameDir);
    }

    // Real Windows path (UTF-8 protected)
    juce::String finalPluginsPath = tempMameDir.getChildFile("plugins").getFullPathName();
    args.push_back("-pluginspath");
    args.push_back(finalPluginsPath.toUTF8().getAddress());
#else
        // Original macOS solution
        juce::File exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        juce::File pluginsDir = exeFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("plugins");
        
        // NEW: AU Sandbox fallback routine
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
    
    // --- 4. STATE DIRECTORY CONFIGURATION ---
    // Ensure the temp state directory uses UTF-8 to prevent Windows save/load failures
    args.push_back("-state_directory");
#ifdef _WIN32
    args.push_back(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toUTF8().getAddress());
#else
    args.push_back(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toStdString());
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
