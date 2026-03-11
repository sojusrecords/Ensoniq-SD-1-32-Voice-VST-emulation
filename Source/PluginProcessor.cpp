/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.
    
    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <iostream>
#include <chrono>
#include <pthread.h>
#include <stdlib.h>
#include <new>

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
        uint64_t writePos = getTotalWritten();
        uint64_t readPos = getTotalRead();
        int64_t available = writePos - readPos;

        if (available < mameBufferThreshold.load(std::memory_order_relaxed)) {
            break; // Buffer has enough space, proceed with writing
        }

        // Buffer is full. Sleep the MAME thread until processBlock consumes some data.
        mameThrottleEvent.wait(5);
    }

    uint64_t currentWritePos = totalWritten.load(std::memory_order_relaxed);

    // MAME outputs interleaved audio. STRIDE = 5 (Main L, Main R, Aux L, Aux R, Floppy)
    for (int i = 0; i < numSamples; ++i) {
        int index = currentWritePos % RING_BUFFER_SIZE;
        
        ringBufferL[index]    = pcmBuffer[i * 5 + 0] / 32768.0f;
        ringBufferR[index]    = pcmBuffer[i * 5 + 1] / 32768.0f;
        
        ringBufferAuxL[index] = pcmBuffer[i * 5 + 2] / 32768.0f;
        ringBufferAuxR[index] = pcmBuffer[i * 5 + 3] / 32768.0f;
        
        currentWritePos++;
    }
    
    totalWritten.store(currentWritePos, std::memory_order_release);
}
    
// ==============================================================================
// VST MIDI PORT IMPLEMENTATION
// ==============================================================================
class VstMidiInputPort : public osd::midi_input_port {
private:
    EnsoniqSD1AudioProcessor* processor;
public:
    VstMidiInputPort(EnsoniqSD1AudioProcessor* p) : processor(p) {}
    virtual ~VstMidiInputPort() {}
    
    virtual bool poll() override {
        return processor->hasMidiData();
    }
    
    virtual int read(uint8_t *pOut) override {
        if (processor->hasMidiData()) {
            *pOut = static_cast<uint8_t>(processor->readMidiByte());
            return 1;
        }
        return 0;
    }
};

// ==============================================================================
// HEADLESS OSD (Operating System Dependent) INTERFACE
// This class acts as the bridge between MAME's core and the JUCE environment.
// ==============================================================================
class VstOsdInterface : public osd_common_t
{
private:
    EnsoniqSD1AudioProcessor* processor;
    running_machine* mame_machine = nullptr;
    uint32_t lastMouseButtons = 0;
    
    ioport_port* portVolume = nullptr;
    ioport_port* portDataEntry = nullptr;
    ioport_port* portPitchBend = nullptr;
    ioport_port* portModWheel = nullptr;
    render_target* main_target = nullptr;

    int saveFrameDelay = 0;
    int loadFrameDelay = 0;
    
public:
    
    virtual void process_events() override {}
    virtual bool has_focus() const override { return true; }
    
    VstOsdInterface(EnsoniqSD1AudioProcessor* p, osd_options &options)
    : osd_common_t(options), processor(p) {}
    
    virtual ~VstOsdInterface() {}
    
    virtual void init(running_machine &machine) override {
        // REQUIRED: Initializes MAME's core sound, mouse, and keyboard modules
        osd_common_t::init(machine);
        
        portVolume = machine.root_device().ioport("analog_volume");
        portDataEntry = machine.root_device().ioport("analog_data_entry");
        portPitchBend = machine.root_device().ioport("analog_pitch_bend");
        portModWheel = machine.root_device().ioport("analog_mod_wheel");
        
        mame_machine = &machine;
        processor->mameMachine = &machine;
        
        // Safely retrieve the initial requested view
        std::string targetView = "Compact";
        int startW = 1600; int startH = 720;
        {
            std::lock_guard<std::mutex> lock(processor->viewMutex);
            targetView = processor->requestedViewName;
            startW = processor->mameInternalWidth.load();
            startH = processor->mameInternalHeight.load();
        }

        // SINGLE ALLOCATION with exact dimensions to prevent memory leaks on exit
        main_target = machine.render().target_alloc();
        main_target->set_bounds(startW, startH);
        
        // Find and set the requested layout view
        for (int i = 0; ; i++) {
            const char *vname = main_target->view_name(i);
            if (vname == nullptr) break;
            
            std::string nameStr(vname);
            if (nameStr.find(targetView) != std::string::npos) {
                main_target->set_view(i);
                break;
            }
        }
    };
    
    virtual void osd_exit() override {
        if (mame_machine != nullptr && main_target != nullptr) {
            // Gracefully return the render target to MAME to avoid crashes during cleanup
            mame_machine->render().target_free(main_target);
            main_target = nullptr;
        }
        osd_common_t::osd_exit();
    }
    
    virtual void update(bool skip_redraw) override {
        if (skip_redraw || mame_machine == nullptr) return;
        
        // --- DYNAMIC VIEW SWITCHING ---
        if (processor->requestViewChange.exchange(false, std::memory_order_acquire)) {
            std::string targetView;
            int newW, newH;
            {
                std::lock_guard<std::mutex> lock(processor->viewMutex);
                targetView = processor->requestedViewName;
                newW = processor->mameInternalWidth.load();
                newH = processor->mameInternalHeight.load();
            }
            
            for (int i = 0; ; i++) {
                const char *vname = main_target->view_name(i);
                if (vname == nullptr) break;
                
                std::string nameStr(vname);
                if (nameStr.find(targetView) != std::string::npos) {
                    main_target->set_view(i);
                    break;
                }
            }
            main_target->set_bounds(newW, newH); 
            
            // Clear buffers to black to prevent ghosting from the previous layout
            for(int i=0; i<2; i++) {
                juce::Graphics g2(processor->screenBuffers[i]);
                g2.fillAll(juce::Colours::black);
            }
        }
        
        // --- SYNCHRONIZED MAME STATE SAVING ---
        if (processor->requestMameSave.exchange(false, std::memory_order_acquire)) {
            mame_machine->schedule_save("vst_temp");
            // Give MAME a few frames (approx. 50ms) to ensure the state file is fully written to disk
            saveFrameDelay = 3; 
        }

        if (saveFrameDelay > 0) {
            saveFrameDelay--;
            if (saveFrameDelay == 0) {
                processor->mameStateIsReady.store(true, std::memory_order_release);
                processor->mameStateEvent.signal();
            }
        }

        // --- SYNCHRONIZED MAME STATE LOADING ---
        if (processor->requestMameLoad.exchange(false, std::memory_order_acquire)) {
            mame_machine->schedule_load("vst_temp");
            loadFrameDelay = 3; 
        }

        if (loadFrameDelay > 0) {
            loadFrameDelay--;
            if (loadFrameDelay == 0) {
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
        
        render_target *target = mame_machine->render().first_target();
        if (target == nullptr) return;
        
        render_primitive_list &prims = target->get_primitives();
        prims.acquire_lock();
        
        // --- DOUBLE BUFFERED RENDERING ---
        // Draw into the buffer that is currently NOT being read by the JUCE GUI thread
        int writeIndex = 1 - processor->readyBufferIndex.load(std::memory_order_acquire);
        juce::Graphics g(processor->screenBuffers[writeIndex]);
        g.fillAll(juce::Colours::black);
        
        for (render_primitive *prim = prims.first(); prim != nullptr; prim = prim->next()) {
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
                                const uint32_t* srcRow = static_cast<const uint32_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));
                                
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
        
        // ========================================================================
        // DAW AUTOMATION INJECTION (Analog hardware port emulation)
        // ========================================================================
        if (mame_machine != nullptr) {
            auto setPortValue = [](ioport_port* port, int value) {
                if (port != nullptr) {
                    for (ioport_field& field : port->fields()) {
                        field.set_value(value);
                    }
                }
            };
            
            setPortValue(portVolume, processor->mameVolume.load(std::memory_order_relaxed));
            setPortValue(portDataEntry, processor->mameDataEntry.load(std::memory_order_relaxed));
            setPortValue(portPitchBend, processor->mamePitchBend.load(std::memory_order_relaxed));
            setPortValue(portModWheel, processor->mameModWheel.load(std::memory_order_relaxed));
        }
            
        // Swap the ready buffer index. The JUCE GUI will now pick up the fresh frame.
        processor->readyBufferIndex.store(writeIndex, std::memory_order_release);
        processor->getFrameFlag().store(true, std::memory_order_release);
    }
        
    virtual void input_update(bool relative_reset) override {
        if (mame_machine == nullptr) return;
        render_target* target = mame_machine->render().first_target();
        
        if (target != nullptr) {
            int x = processor->mouseX.load(std::memory_order_relaxed);
            int y = processor->mouseY.load(std::memory_order_relaxed);
            uint32_t currentBtns = processor->mouseButtons.load(std::memory_order_relaxed);
            
            // Edge detection for mouse clicks
            int32_t pressed =  ((currentBtns & 1) && !(lastMouseButtons & 1)) ? 1 : 0; 
            int32_t released = (!(currentBtns & 1) && (lastMouseButtons & 1)) ? 1 : 0; 
            int32_t clicks = pressed; 
            
            mame_machine->ui_input().push_pointer_update(
                target, ui_input_manager::pointer::MOUSE, 0, 0,
                x, y, currentBtns, pressed, released, clicks
            );
            
            lastMouseButtons = currentBtns;
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
// MIDI INPUT HANDLING (JUCE -> MAME)
// ==============================================================================
void EnsoniqSD1AudioProcessor::pushMidiByte(uint8_t data) {
    int currentWrite = midiWritePos.load();
    int nextWrite = (currentWrite + 1) % MIDI_BUFFER_SIZE;
    if (nextWrite != midiReadPos.load()) {
        midiBuffer[currentWrite] = data;
        midiWritePos.store(nextWrite);
    }
}

bool EnsoniqSD1AudioProcessor::hasMidiData() {
    return midiReadPos.load() != midiWritePos.load();
}

int EnsoniqSD1AudioProcessor::readMidiByte() {
    int currentRead = midiReadPos.load();
    if (currentRead == midiWritePos.load()) return 0; // Buffer empty
    
    int data = midiBuffer[currentRead];
    midiReadPos.store((currentRead + 1) % MIDI_BUFFER_SIZE);
    return data;
}

// ==============================================================================
// VST AUTOMATION (APVTS) DEFINITIONS
// ==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout EnsoniqSD1AudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Define the automatable parameters visible to the DAW
    params.push_back(std::make_unique<juce::AudioParameterFloat>("volume", "Volume", 0.0f, 1.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("data_entry", "Data Entry", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("pitch_bend", "Pitch Bend", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("mod_wheel", "Mod Wheel", 0.0f, 1.0f, 0.0f));
    
    juce::StringArray bufferSizes = { "256", "512", "1024", "2048", "4096", "8192" };
    params.push_back(std::make_unique<juce::AudioParameterChoice>("buffer_size", "Internal Buffer", bufferSizes, 2));

    // Dynamic Panel Layout Selector
    juce::StringArray views = { "Compact (Default)", "Full Keyboard", "Rack Panel", "Tablet View" };
    params.push_back(std::make_unique<juce::AudioParameterChoice>("layout_view", "Panel Layout", views, 0));

    return { params.begin(), params.end() };
}

void EnsoniqSD1AudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // Convert JUCE's normalized 0.0 - 1.0 range to MAME's expected 0 - 1023 (10-bit) hardware range.
    int mameValue = static_cast<int>(newValue * 1023.0f);

    if (parameterID == "volume") {
        mameVolume.store(mameValue, std::memory_order_relaxed);
    }
    else if (parameterID == "data_entry") {
        mameDataEntry.store(mameValue, std::memory_order_relaxed);
    }
    else if (parameterID == "pitch_bend") {
        mamePitchBend.store(mameValue, std::memory_order_relaxed);
    }
    else if (parameterID == "mod_wheel") {
        mameModWheel.store(mameValue, std::memory_order_relaxed);
    }
    else if (parameterID == "buffer_size") {
        int sizes[] = { 256, 512, 1024, 2048, 4096, 8192 };
        int index = juce::roundToInt(newValue * 5.0f); 
        mameBufferThreshold.store(sizes[index], std::memory_order_relaxed);
    }
    else if (parameterID == "layout_view") {
        auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("layout_view"));
        int idx = choiceParam != nullptr ? choiceParam->getIndex() : 0;
        
        std::string mameViewName = "Compact";
        int newW = 2048; int newH = 921; 
        
        // Map the selected index to MAME's internal panel names. 
        // Resolutions are upscaled to 2048 width for razor-sharp "High Resampling" rendering.
        if (idx == 0) {
            mameViewName = "Compact"; newW = 2048; newH = 921;
        } else if (idx == 1) {
            mameViewName = "Full";    newW = 2048; newH = 671;
        } else if (idx == 2) {
            mameViewName = "Panel";   newW = 2048; newH = 379;
        } else if (idx == 3) {
            mameViewName = "Tablet";  newW = 2048; newH = 1476;
        }
        
        std::lock_guard<std::mutex> lock(viewMutex);
        requestedViewName = mameViewName;
        mameInternalWidth.store(newW, std::memory_order_release);
        mameInternalHeight.store(newH, std::memory_order_release);
        requestViewChange.store(true, std::memory_order_release);
    }
}
// ==============================================================================

EnsoniqSD1AudioProcessor::EnsoniqSD1AudioProcessor()
     : AudioProcessor (BusesProperties()
                       .withOutput ("Main Out", juce::AudioChannelSet::stereo(), true)
                       .withOutput ("Aux Out",  juce::AudioChannelSet::stereo(), true) 
                       ),
       apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    apvts.addParameterListener("volume", this);
    apvts.addParameterListener("data_entry", this);
    apvts.addParameterListener("pitch_bend", this);
    apvts.addParameterListener("mod_wheel", this);
    apvts.addParameterListener("buffer_size", this);
    apvts.addParameterListener("layout_view", this);
    
    // --- SINGLE INSTANCE LOCKING ---
    // MAME uses a lot of global state and singletons. Running two MAME instances in the 
    // same process (DAW) will cause an immediate crash. We use an atomic boolean lock 
    // to ensure only the first loaded VST instance actually boots MAME. 
    bool expected = false;
    if (globalMameLock.compare_exchange_strong(expected, true)) {
        isMasterInstance = true;
        isBlockedByAnotherInstance.store(false);
        
        // Only the Master instance is allowed to create folders and boot the engine
        juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
        if (!ensoniqDir.exists()) {
            ensoniqDir.createDirectory();
        }
    } else {
        // Another instance is already running! This instance will remain silent and show a warning UI.
        isMasterInstance = false;
        isBlockedByAnotherInstance.store(true);
    }
}

EnsoniqSD1AudioProcessor::~EnsoniqSD1AudioProcessor()
{
    // Only the Master instance is allowed to shut down MAME
    if (isMasterInstance) {
        isMameRunning.store(false, std::memory_order_release);

        if (mameMachine != nullptr) {
            mameMachine->schedule_exit();
        }

        mameThrottleEvent.signal();
        mameStateEvent.signal();

        if (mameThread.joinable()) {
            mameThread.join();
        }
        
        // Release the lock for future plugin instances
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
    if (!isMasterInstance) return;
    hostSampleRate.store(sampleRate);

    // Only boot MAME the very first time play is prepared
    if (!mameHasStarted.exchange(true)) {
        
        initialSampleRate.store(sampleRate); 
        
        juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
        juce::File romFile = ensoniqDir.getChildFile("sd132.zip");

        if (!romFile.existsAsFile()) {
            isRomMissing.store(true, std::memory_order_release);
            isMameRunning = false;
        } else {
            isRomMissing.store(false, std::memory_order_release);
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
}

void EnsoniqSD1AudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool EnsoniqSD1AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 1. Main Out MUST be stereo
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // 2. Aux Out must also be stereo if enabled by the DAW host
    auto auxBus = layouts.getChannelSet(false, 1);
    if (auxBus != juce::AudioChannelSet::disabled() && auxBus != juce::AudioChannelSet::stereo())
        return false;

    return true;
}
#endif

void EnsoniqSD1AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());
    
    // Mute audio if this is a blocked secondary instance or if sample rate mismatched
    if (isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
        return;
    }

    if (sampleRateMismatch.load(std::memory_order_acquire)) {
        return;
    }

    // ========================================================
    // 1. MIDI IN
    // ========================================================
    for (const auto metadata : midiMessages) {
        auto msg = metadata.getMessage();
        const uint8_t* rawData = msg.getRawData();
        for (int i = 0; i < msg.getRawDataSize(); ++i) {
            pushMidiByte(rawData[i]);
        }
    }

    // ========================================================
    // 2. AUDIO OUT & RING BUFFER CONSUMPTION
    // ========================================================
    int numSamples = buffer.getNumSamples();
    uint64_t currentReadPos = totalRead.load(std::memory_order_acquire);
    uint64_t currentWritePos = totalWritten.load(std::memory_order_acquire);
    int64_t available = static_cast<int64_t>(currentWritePos) - static_cast<int64_t>(currentReadPos);

    // Output pointers (0, 1 = Main L/R | 2, 3 = Aux L/R)
    auto* outL = buffer.getWritePointer(0);
    auto* outR = (totalNumOutputChannels > 1) ? buffer.getWritePointer(1) : nullptr;
    auto* outAuxL = (totalNumOutputChannels > 2) ? buffer.getWritePointer(2) : nullptr;
    auto* outAuxR = (totalNumOutputChannels > 3) ? buffer.getWritePointer(3) : nullptr;

    int samplesToProcess = (available < numSamples) ? static_cast<int>(available) : numSamples;

    // Consume samples from the ring buffers
    for (int i = 0; i < samplesToProcess; ++i) {
        uint64_t idx = currentReadPos % RING_BUFFER_SIZE;
        
        outL[i] = ringBufferL[idx];
        if (outR != nullptr) outR[i] = ringBufferR[idx];
        
        if (outAuxL != nullptr) outAuxL[i] = ringBufferAuxL[idx];
        if (outAuxR != nullptr) outAuxR[i] = ringBufferAuxR[idx];
        
        currentReadPos++;
    }

    // Underrun protection: pad remaining required samples with zeroes
    if (samplesToProcess < numSamples) {
        for (int i = samplesToProcess; i < numSamples; ++i) {
            outL[i] = 0.0f;
            if (outR != nullptr) outR[i] = 0.0f;
            if (outAuxL != nullptr) outAuxL[i] = 0.0f;
            if (outAuxR != nullptr) outAuxR[i] = 0.0f;
        }
    }

    totalRead.store(currentReadPos, std::memory_order_release);
    mameThrottleEvent.signal(); // Wake up MAME to generate more audio
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
        if (xmlState->hasAttribute("mame_state") && isMameRunningFlag()) {
            juce::MemoryBlock mameData;
            mameData.fromBase64Encoding(xmlState->getStringAttribute("mame_state"));

            // Write state back to a temp file for MAME to consume
            juce::File tempStateDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sd132");
            tempStateDir.createDirectory(); 
            
            juce::File tempStateFile = tempStateDir.getChildFile("vst_temp.sta");
            tempStateFile.replaceWithData(mameData.getData(), mameData.getSize());

            // Signal MAME to load the state
            mameStateEvent.reset();
            requestMameLoad.store(true, std::memory_order_release);
            mameThrottleEvent.signal();
            mameStateEvent.wait(2000); 
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

void EnsoniqSD1AudioProcessor::runMameEngine()
{
    // Prevent MAME from hijacking the host OS audio/video drivers
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    
    std::vector<std::string> args;
    args.push_back("mame");
    args.push_back("sd132");
    
    juce::File ensoniqDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1");
    args.push_back("-rompath");
    args.push_back(ensoniqDir.getFullPathName().toStdString());

    // Setup internal Layouts plugin path
    juce::File exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::File pluginsDir = exeFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("plugins");
    
    args.push_back("-pluginspath");
    args.push_back(pluginsDir.getFullPathName().toStdString());
    args.push_back("-plugin");
    args.push_back("layout");

    // Use software rendering and our custom OSD sound module
    args.push_back("-video");
    args.push_back("soft");
    args.push_back("-sound");
    args.push_back("osd");
    args.push_back("-midiin");
    args.push_back("VST MIDI");
    
    // Disable MAME's internal pacing (we control this via audio throttle)
    args.push_back("-nothrottle");
    args.push_back("-nosleep");
    args.push_back("-nowaitvsync");
    args.push_back("-nowindow");
    args.push_back("-nobackground_input");

    args.push_back("-noreadconfig");
    args.push_back("-skip_gameinfo");
    args.push_back("-samplerate");
    args.push_back(std::to_string(static_cast<int>(hostSampleRate.load())));
    
    args.push_back("-keyboardprovider");
    args.push_back("none");
    args.push_back("-mouseprovider");
    args.push_back("none");
    args.push_back("-joystickprovider");
    args.push_back("none");
    
    args.push_back("-state_directory");
    args.push_back(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toStdString());
       
    // Boot the headless CLI Frontend
    auto* mameOpts = new osd_options();
    auto* headlessOsd = new VstOsdInterface(this, *mameOpts);
    auto* frontend = new cli_frontend(*mameOpts, *headlessOsd);
    
    frontend->execute(args);
    
    delete frontend;
    delete headlessOsd;
    delete mameOpts;
}