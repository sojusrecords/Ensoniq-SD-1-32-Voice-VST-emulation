/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

EnsoniqSD1AudioProcessorEditor::EnsoniqSD1AudioProcessorEditor (EnsoniqSD1AudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // Start the UI polling timer (30 Hz is enough for smooth UI and MAME frame fetching)
    startTimerHz(30);
    
    this->setWantsKeyboardFocus(false);
    this->setMouseClickGrabsKeyboardFocus(false);
    
    // --- BASE LAYOUT DIMENSIONS ---
    int viewIdx = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
    lastView = viewIdx;

    int baseW = 2048;
    int baseH = 925; // Default Compact
    if (viewIdx == 1) baseH = 671;
    else if (viewIdx == 2) baseH = 379;
    else if (viewIdx == 3) baseH = 1476;

    float aspect = (float)baseW / (float)baseH;

    // --- WINDOW SETTINGS & RESIZER ---
    int startW = audioProcessor.savedWindowWidth >= 900 ? audioProcessor.savedWindowWidth : 1200;
    int startH = juce::roundToInt(startW / aspect);

    setResizable(true, true);
    getConstrainer()->setFixedAspectRatio(aspect);
    setResizeLimits(900, juce::roundToInt(900.0f / aspect), 2400, juce::roundToInt(2400.0f / aspect));
    setSize(startW, startH);
    
    // --- BUTTON INITIALIZATION ---
    addAndMakeVisible(loadMediaButton);
    loadMediaButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    loadMediaButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    loadMediaButton.setWantsKeyboardFocus(false);
    loadMediaButton.setMouseClickGrabsKeyboardFocus(false);
    loadMediaButton.onClick = [this] { loadMediaButtonClicked(); };
    floppyIcon = juce::Drawable::createFromImageData(BinaryData::floppy_svg, BinaryData::floppy_svgSize);
    cartIcon = juce::Drawable::createFromImageData(BinaryData::cart_svg, BinaryData::cart_svgSize);
    
    if (audioProcessor.isRomMissing.load(std::memory_order_acquire) ||
        audioProcessor.isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
        loadMediaButton.setVisible(false);
    }
    
    // --- SETTINGS BUTTON & PANEL INITIALIZATION ---
    addAndMakeVisible(settingsButton);
    settingsButton.setWantsKeyboardFocus(false);
    settingsButton.setMouseClickGrabsKeyboardFocus(false);
    settingsButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    settingsButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    settingsButton.onClick = [this] { toggleSettings(); };

    // GroupComponent and its children are hidden by default
    addChildComponent(settingsGroup);
    settingsGroup.setColour(juce::GroupComponent::textColourId, juce::Colours::orange);
    settingsGroup.setColour(juce::GroupComponent::outlineColourId, juce::Colours::grey);

    addChildComponent(bufferLabel);
    bufferLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addChildComponent(bufferCombo);
    bufferCombo.addItemList({ "256", "512", "1024", "2048", "4096", "8192" }, 1);
    bufferAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(audioProcessor.apvts, "buffer_size", bufferCombo);

    addChildComponent(viewLabel);
    viewLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    viewLabel.setJustificationType(juce::Justification::centredRight);

    addChildComponent(viewCombo);
    viewCombo.addItemList({ "Compact (Default)", "Full Keyboard", "Rack Panel", "Tablet View" }, 1);
    viewAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(audioProcessor.apvts, "layout_view", viewCombo);
        
    addChildComponent(aboutLabel);
    aboutLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    aboutLabel.setJustificationType(juce::Justification::centred);

    addChildComponent(webLink);

    addChildComponent(closeSettingsButton);
    closeSettingsButton.setWantsKeyboardFocus(false);
    closeSettingsButton.setMouseClickGrabsKeyboardFocus(false);
    closeSettingsButton.onClick = [this] { toggleSettings(); };
    
    // Hide UI elements if the plugin is in a disabled state
    if (audioProcessor.isRomMissing.load(std::memory_order_acquire) ||
            audioProcessor.isRomInvalid.load(std::memory_order_acquire) ||
            audioProcessor.isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
            loadMediaButton.setVisible(false);
            settingsButton.setVisible(false);
        }
}

EnsoniqSD1AudioProcessorEditor::~EnsoniqSD1AudioProcessorEditor()
{
    stopTimer();
}

void EnsoniqSD1AudioProcessorEditor::timerCallback()
{
    // SYSEX TRANSMITING START
    bool currentSysEx = audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire);
        
        if (currentSysEx != lastSysExState) {
            lastSysExState = currentSysEx;

            loadMediaButton.setEnabled(!currentSysEx);
            settingsButton.setEnabled(!currentSysEx);
            
            repaint();
        }

        else if (currentSysEx) {
            repaint();
        }
    
    // Standard offline protection
    if (audioProcessor.isNonRealtime()) {
        audioProcessor.getFrameFlag().store(false, std::memory_order_relaxed);
        return;
    }

    // Global settings save triggered from background
    if (audioProcessor.requestGlobalSave.exchange(false, std::memory_order_acquire)) {
        saveGlobalSettings();
    }
    
    // Protection against drawing on the dead window
    if (!isShowing() || getWidth() <= 0 || getHeight() <= 0 || getTopLevelComponent()->getPeer() == nullptr) {
        audioProcessor.getFrameFlag().store(false, std::memory_order_relaxed);
        return;
    }
    
    // Dynamic Window Aspect Ratio Monitor
    int currentView = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
    if (currentView != lastView) {
        lastView = currentView;
        updateWindowSize(); // Automatically snaps window to the new layout's aspect ratio
    }

    // Frame Update
    if (audioProcessor.getFrameFlag().exchange(false, std::memory_order_acquire)) {
        repaint();
    }
}

void EnsoniqSD1AudioProcessorEditor::paint (juce::Graphics& g)
{
    // ==============================================================================
    // --- SAFEGUARDS & ERROR SCREENS ---
    // ==============================================================================
    
    // --- INSTANCE LIMIT WARNING MESSAGE ---
    if (audioProcessor.isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::red);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nInstance Limit Reached!\n\nDue to MAME engine limitations, only ONE instance\nof this plugin can run at a time in the host.\n\nThis instance has been disabled to prevent crashes.";
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }
    
    // --- SAMPLE RATE MISMATCH WARNING MESSAGE ---
    if (audioProcessor.sampleRateMismatch.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::orange);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nSample Rate Changed!\n\nThe host sample rate has changed since the plugin was loaded.\nTo prevent audio glitches, please reload the plugin or restart your DAW.";
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }
    
    // --- MISSING ROM WARNING MESSAGE ---
    if (audioProcessor.isRomMissing.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::red);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nError! Roms not found!\n\nPlease copy the 'sd132.zip' file into the following folder:\n\n";
        errorMsg += juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getFullPathName();
        errorMsg += "\n\nThen restart the plugin!";
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }
    
    // --- INVALID ROM WARNING MESSAGE ---
    if (audioProcessor.isRomInvalid.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::red);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nError! Invalid ROM Checksum!\n\nThe 'sd132.zip' file was found but contains incorrect or modified files.\nPlease ensure you have the exact, unmodified MAME ROM dump.\n\nThen restart the plugin!";
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }

    // ==============================================================================
    // --- 1. MAME RENDER ROUTINE (Base Layer: Physical machine, grey panel, buttons)
    // ==============================================================================
    int readIndex = audioProcessor.readyBufferIndex.load(std::memory_order_acquire);
    
    // SAFE BOUNDS: Never try to read more pixels than physically exist in the current window bounds or the buffer!
    // This prevents Out-Of-Bounds (EXC_BAD_ACCESS) crashes when the DAW aggressively resizes the host window.
    int renderW = juce::jmin(getWidth(), audioProcessor.screenBuffers[readIndex].getWidth());
    int renderH = juce::jmin(getHeight(), audioProcessor.screenBuffers[readIndex].getHeight());
    
    // Resampling quality
    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
        
    // MAME draws the original Ensoniq grey background, buttons, VFD display, and piano keys!
    // The image is drawn 1:1 onto the screen, resulting in ultra-sharp visuals and low CPU usage.
    g.drawImage(audioProcessor.screenBuffers[readIndex],
                0, 0, renderW, renderH, // Destination
                0, 0, renderW, renderH, // Source
                false); // 'false' because MAME's base layer does not need alpha blending

    // ==============================================================================
    // --- 2. JUCE PNG LABELS OVERLAY (Top Layer: Transparent decal over the machine)
    // ==============================================================================
    int viewIdx = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
    juce::Image overlayImage;

    // Load the appropriate transparent PNG from memory (BinaryData) based on the active view.
    // ImageCache is extremely fast: it decodes the PNG only once and stores it in memory.
    // Index Mapping: 0 = Compact, 1 = Full, 2 = Rack, 3 = Tablet
    if (viewIdx == 0) {
        overlayImage = juce::ImageCache::getFromMemory(BinaryData::labels_compact_png, BinaryData::labels_compact_pngSize);
    } else if (viewIdx == 1) {
        overlayImage = juce::ImageCache::getFromMemory(BinaryData::labels_full_png, BinaryData::labels_full_pngSize);
    } else if (viewIdx == 2) {
        overlayImage = juce::ImageCache::getFromMemory(BinaryData::labels_rack_png, BinaryData::labels_rack_pngSize);
    } else if (viewIdx == 3) {
        overlayImage = juce::ImageCache::getFromMemory(BinaryData::labels_tablet_png, BinaryData::labels_tablet_pngSize);
    }

    if (overlayImage.isValid()) {
        // Draw the labels overlay on top of MAME's rendered interface!
        // MAME's interactive buttons and the VFD display will be perfectly visible under/around the text.
        g.drawImage(overlayImage, getLocalBounds().toFloat());
    }

    // ==============================================================================
    // --- 3. SETTINGS OVERLAY (Dimmed background & Settings Panel)
    // ==============================================================================
    if (isSettingsVisible) {
        g.fillAll(juce::Colour(0x4d000000)); // Dim the entire UI with semi-transparent black
        
        auto panelRect = settingsGroup.getBounds().toFloat();
        panelRect.removeFromTop(8.0f);
        
        g.setColour(juce::Colour(0xcc000000)); // Solid dark background for the settings panel
        g.fillRoundedRectangle(panelRect, 5.0f);
    }
        
        // ==============================================================================
        // --- 4. MEDIA LOADED INDICATOR (Floppy / Cartridge) ---
        // ==============================================================================

        bool hasFloppy = audioProcessor.isFloppyLoaded.load(std::memory_order_acquire);
        bool hasCart = audioProcessor.isCartLoaded.load(std::memory_order_acquire);

        if (hasFloppy || hasCart) {

            int viewIdx = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
            int baseW = 2048;
            int baseH = 925; // Default Compact
            if (viewIdx == 1) baseH = 671;
            else if (viewIdx == 2) baseH = 379;
            else if (viewIdx == 3) baseH = 1476;

            float scaleX = getWidth() / (float)baseW;
            float scaleY = getHeight() / (float)baseH;

            float vfdX = 0.0f;
            float vfdY = 0.0f;

            if (viewIdx == 0) { // Compact
                vfdX = 304.0f; vfdY = 409.0f;
            } else if (viewIdx == 1) { // Full
                vfdX = 92.0f; vfdY = 366.0f;
            } else if (viewIdx == 2) { // Rack
                vfdX = 443.0f; vfdY = 222.0f;
            } else if (viewIdx == 3) { // Tablet
                vfdX = 289.0f; vfdY = 482.0f;
            }

            float finalX = vfdX * scaleX;
            float finalY = vfdY * scaleY;

            juce::Rectangle<float> indRect(finalX, finalY, 180.0f * scaleX, 24.0f * scaleY);

            g.setColour(juce::Colour(0xcc111111));
            g.fillRoundedRectangle(indRect, 4.0f);

            float ledSize = 8.0f * scaleY;
            g.setColour(juce::Colours::limegreen);
            g.fillEllipse(indRect.getX() + (6.0f * scaleX), indRect.getCentreY() - (ledSize / 2.0f), ledSize, ledSize);

            juce::Rectangle<float> iconBounds(indRect.getX() + (18.0f * scaleX), indRect.getY() + (4.0f * scaleY), 16.0f * scaleY, 16.0f * scaleY);

                    juce::String text = audioProcessor.loadedMediaName;

                    if (hasFloppy && floppyIcon != nullptr) {
                        floppyIcon->drawWithin(g, iconBounds, juce::RectanglePlacement::centred, 1.0f);
                    }
                    else if (hasCart && cartIcon != nullptr) {
                        cartIcon->drawWithin(g, iconBounds, juce::RectanglePlacement::centred, 1.0f);
                    }
                    else {
                        // Fallback
                        text = (hasFloppy ? "FLOPPY: " : "CART: ") + text;
                    }

                    // Filename
                    g.setColour(juce::Colours::white);
                    g.setFont(12.0f * scaleY);

            g.drawText(text, indRect.withTrimmedLeft(38.0f * scaleX).withTrimmedRight(5.0f * scaleX), juce::Justification::centredLeft, true);
        }
    
}

void EnsoniqSD1AudioProcessorEditor::paintOverChildren(juce::Graphics& g)
{
    // ==============================================================================
    // --- SYSEX TRANSMISSION OVERLAY ---
    // ==============================================================================
    if (audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xaa000000));
        
        juce::Rectangle<float> panelRect(getWidth() / 2.0f - 200.0f, getHeight() / 2.0f - 75.0f, 400.0f, 150.0f);
        
        g.setColour(juce::Colour(0xee111111));
        g.fillRoundedRectangle(panelRect, 8.0f);
        
        g.setColour(juce::Colours::orange);
        g.drawRoundedRectangle(panelRect, 8.0f, 2.0f);
        
        g.setColour(juce::Colours::white);
        g.setFont(22.0f);
        g.drawText("Transmitting Sys-Ex Data...", panelRect.withTrimmedTop(20.0f).withHeight(30.0f), juce::Justification::centred);
        
        g.setColour(juce::Colours::lightgrey);
        g.setFont(15.0f);
        g.drawText("Please wait. Do not play notes or press buttons.", panelRect.withTrimmedTop(60.0f).withHeight(20.0f), juce::Justification::centred);
        
        // --- SPINNER ---
        auto time = juce::Time::getMillisecondCounterHiRes();
        float speed = 0.005f; // Speed
        float angle = (float)std::fmod(time * speed, juce::MathConstants<double>::twoPi);
        
        juce::Path spinner;
        spinner.addCentredArc(panelRect.getCentreX(), panelRect.getY() + 115.0f,
                              14.0f, 14.0f, 0.0f, angle, angle + juce::MathConstants<float>::pi * 1.5f, true);
        
        g.setColour(juce::Colours::orange);
        g.strokePath(spinner, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void EnsoniqSD1AudioProcessorEditor::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0) return;

    // --- PIXEL-PERFECT RESOLUTION NOTIFICATION ---
    // Tell MAME exactly how large it should render the next frame!
    audioProcessor.windowWidth.store(getWidth(), std::memory_order_release);
    audioProcessor.windowHeight.store(getHeight(), std::memory_order_release);
    audioProcessor.requestRenderResize.store(true, std::memory_order_release);

    int viewIdx = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
    int baseW = 2048;
    int baseH = 925; // Compact
    if (viewIdx == 1) baseH = 671;
    else if (viewIdx == 2) baseH = 379;
    else if (viewIdx == 3) baseH = 1476;
    
    float targetAspect = (float)baseW / (float)baseH;
    float currentAspect = (float)getWidth() / (float)getHeight();

    // SAFETY NET: Only save dimensions if the DAW hasn't corrupted the aspect ratio
    if (std::abs(currentAspect - targetAspect) < 0.05f) {
        audioProcessor.savedWindowWidth = getWidth();
        audioProcessor.savedWindowHeight = getHeight();
        audioProcessor.requestGlobalSave.store(true, std::memory_order_release);
    }

    float scaleX = getWidth() / (float)baseW;
    float scaleY = getHeight() / (float)baseH;

    // --- DYNAMIC BUTTON POSITIONING ---
    int btnX = 55;
    int btnY = 403; // Default position for Compact layout
    if (viewIdx == 1) { btnX = 82; btnY = 171; }
    else if (viewIdx == 2) { btnX = 85; btnY = 265; }
    else if (viewIdx == 3) { btnX = 95; btnY = 1250; }

    int btnWidth = 192;
    int btnHeight = 30;
    
    loadMediaButton.setBounds(juce::roundToInt(btnX * scaleX), juce::roundToInt(btnY * scaleY),
                              juce::roundToInt(btnWidth * scaleX), juce::roundToInt(btnHeight * scaleY));
                              
    settingsButton.setBounds(juce::roundToInt(btnX * scaleX), juce::roundToInt((btnY + 40) * scaleY),
                             juce::roundToInt(btnWidth * scaleX), juce::roundToInt(btnHeight * scaleY));

    // --- FLEXIBLE SETTINGS PANEL ---
    auto bounds = getLocalBounds();
    auto panelRect = bounds.withSizeKeepingCentre(juce::jmin(550, getWidth() - 20), juce::jmin(320, getHeight() - 20));
    settingsGroup.setBounds(panelRect);

    int y = panelRect.getY() + 35;
    int startX = panelRect.getCentreX() - 170;
    int rowHeight = 25;
    int spacing = (panelRect.getHeight() > 310) ? 15 : 5;

    bufferLabel.setBounds(startX, y, 180, rowHeight);
    bufferCombo.setBounds(startX + 190, y, 150, rowHeight);
    y += rowHeight + spacing;

    viewLabel.setBounds(startX, y, 180, rowHeight);
    viewCombo.setBounds(startX + 190, y, 150, rowHeight);
    y += rowHeight + spacing;

    if (panelRect.getHeight() > 310) {
        if (isSettingsVisible) aboutLabel.setVisible(true);
        aboutLabel.setBounds(panelRect.getX() + 10, y, panelRect.getWidth() - 20, 85);
        y += 85 + spacing;
    } else {
        aboutLabel.setVisible(false);
    }

    webLink.setBounds(panelRect.getX() + 10, y, panelRect.getWidth() - 20, rowHeight);
    y += rowHeight + spacing;

    closeSettingsButton.setBounds(panelRect.getCentreX() - 50, y, 100, rowHeight);
}

// ==============================================================================
// MOUSE EVENT INJECTION (Pixel-Perfect 1:1)
// Since MAME renders at the exact window resolution, no complex scaling math is required!
// ==============================================================================
void EnsoniqSD1AudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseDown(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::mouseUp(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseUp(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::mouseDrag(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseMove(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::mouseMove(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseMove(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::loadMediaButtonClicked()
{
    fileChooser = std::make_unique<juce::FileChooser>("Select Floppy Image, Cartridge or SYS-EX file",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.img;*.hfe;*.dsk;*.eda;*.syx;*.eeprom;*.rom;*.cart");

    auto folderChooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync(folderChooserFlags, [this](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file.existsAsFile()) {
            
            // --- SYSEX FILE  ---
            if (file.getFileExtension().toLowerCase() == ".syx") {

                            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                .withIconType (juce::MessageBoxIconType::InfoIcon)
                                .withTitle ("Prepare SD-1 for SYS-EX")
                                .withMessage ("IMPORTANT: The SD-1 will ignore this file unless SYS-EX reception is enabled!\n\n"
                                              "1. Press the SYSTEM/MIDI CONTROL button TWICE on the panel.\n"
                                              "2. Set SYS-EX to ON.\n"
                                              "3. Press SOUNDS or PRESETS to return to the main screen.\n\n"
                                              "Is the synthesizer ready?")
                                .withButton ("Transmit")
                                .withButton ("Cancel"),
                                [this, file] (int result)
                                {

                                    if (result == 1) {
                                        audioProcessor.loadSysExFile(file);
                                    }
                                });
                        }
            // --- FLOPPY / CARTRIDGE  ---
                        else {
                                        std::lock_guard<std::mutex> lock(audioProcessor.mediaMutex);
                        #ifdef _WIN32
                                        audioProcessor.pendingFloppyPath = file.getFullPathName().toUTF8().getAddress();
                        #else
                                        audioProcessor.pendingFloppyPath = file.getFullPathName().toStdString();
                        #endif
                                        audioProcessor.loadedMediaName = file.getFileName();

                            juce::String ext = file.getFileExtension().toLowerCase();

                            if (ext == ".eeprom" || ext == ".rom" || ext == ".cart") {
                                audioProcessor.pendingCartPath = audioProcessor.pendingFloppyPath;
                                audioProcessor.requestCartLoad.store(true, std::memory_order_release);
                                audioProcessor.isCartLoaded.store(true, std::memory_order_release);
                                audioProcessor.isFloppyLoaded.store(false, std::memory_order_release);
                            } else {
                                audioProcessor.requestFloppyLoad.store(true, std::memory_order_release);
                                audioProcessor.isFloppyLoaded.store(true, std::memory_order_release);
                                audioProcessor.isCartLoaded.store(false, std::memory_order_release);
                            }
                                        
                                        repaint();
                                    }
                    }
                });
            }

void EnsoniqSD1AudioProcessorEditor::toggleSettings()
{
    isSettingsVisible = !isSettingsVisible;
    settingsGroup.setVisible(isSettingsVisible);
    bufferLabel.setVisible(isSettingsVisible);
    bufferCombo.setVisible(isSettingsVisible);
    viewLabel.setVisible(isSettingsVisible);
    viewCombo.setVisible(isSettingsVisible);
    aboutLabel.setVisible(isSettingsVisible);
    webLink.setVisible(isSettingsVisible);
    closeSettingsButton.setVisible(isSettingsVisible);
    repaint();
}

void EnsoniqSD1AudioProcessorEditor::updateWindowSize()
{
    int viewIdx = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
    int baseW = 2048;
    int baseH = 925;
    if (viewIdx == 1) baseH = 671;
    else if (viewIdx == 2) baseH = 379;
    else if (viewIdx == 3) baseH = 1476;
    
    float aspect = (float)baseW / (float)baseH;
    
    int currentW = getWidth();
    if (currentW < 900) {
        currentW = 1200;
    }
    int winH = juce::roundToInt(currentW / aspect);
    
    if (getWidth() == currentW && getHeight() == winH) {
        return;
    }
    
    // Apply constraint logic and resize
    getConstrainer()->setFixedAspectRatio(aspect);
    getConstrainer()->setSizeLimits(900, juce::roundToInt(900.0f / aspect), 2400, juce::roundToInt(2400.0f / aspect));

    juce::MessageManager::callAsync([this, currentW, winH]() {
        setSize(currentW, winH);
    });
}

// Global settings
void EnsoniqSD1AudioProcessorEditor::saveGlobalSettings()
{
    juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::File settingsDir = docsDir.getChildFile("EnsoniqSD1");
    if (!settingsDir.exists()) settingsDir.createDirectory();

    juce::File settingsFile = settingsDir.getChildFile("settings.xml");
    juce::XmlElement xml("EnsoniqSD1Settings");

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(audioProcessor.apvts.getParameter("buffer_size")))
        xml.setAttribute("buffer_size", p->getIndex());

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(audioProcessor.apvts.getParameter("layout_view")))
        xml.setAttribute("layout_view", p->getIndex());

    xml.setAttribute("window_width", audioProcessor.savedWindowWidth);
    xml.setAttribute("window_height", audioProcessor.savedWindowHeight);

    xml.writeTo(settingsFile);
}
