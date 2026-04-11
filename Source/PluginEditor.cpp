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
    
    // Hide UI elements if the plugin is in a disabled state
        if (audioProcessor.isRomMissing.load(std::memory_order_acquire) ||
            audioProcessor.isRomInvalid.load(std::memory_order_acquire) ||
            audioProcessor.isSelfCheckFailed.load(std::memory_order_acquire) ||
            audioProcessor.isUnsupportedAUHost.load(std::memory_order_acquire)) {
            
            loadMediaButton.setVisible(false);
            settingsButton.setVisible(false);
        }
    
    // --- ROM BUTTONS ---
        addAndMakeVisible(locateRomButton);
        locateRomButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff0055aa));
        locateRomButton.onClick = [this] { locateRomButtonClicked(); };

        addAndMakeVisible(rescanRomButton);
        rescanRomButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffaa5500));
        rescanRomButton.onClick = [this] { audioProcessor.checkRomAndBootMame(); repaint(); };
    
    // SAVE MACRO
        saveBadge.setButtonText(""); // Text and icon will be drawn natively in paint()
        
        // Make the button completely transparent
        saveBadge.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        saveBadge.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        saveBadge.setWantsKeyboardFocus(false);
        saveBadge.setMouseClickGrabsKeyboardFocus(false);

        saveBadge.onClick = [this]() {
            // 1. Electronically hold down the PRESETS button via APVTS
            if (auto* p = audioProcessor.apvts.getParameter("btn_presets")) {
                p->setValueNotifyingHost(1.0f);
            }
            
            // 2. Arm the macro in the processor
            audioProcessor.isSaveMacroActive.store(true, std::memory_order_release);
            
            // 3. Show the instructional text
            savePromptLabel.setText("Press any BANK button to save...", juce::dontSendNotification);
            savePromptLabel.setVisible(true);
            
        };
        addAndMakeVisible(saveBadge);

        // Set up the prompt label
        savePromptLabel.setJustificationType(juce::Justification::centred);
        savePromptLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
        savePromptLabel.setVisible(false); // Hidden by default
        addAndMakeVisible(savePromptLabel);
    
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
    bufferLabel.setJustificationType(juce::Justification::centredRight);

    addChildComponent(bufferCombo);
    bufferCombo.addItemList({ "128", "256", "512", "1024", "2048", "4096", "8192" }, 1);
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
            audioProcessor.isSelfCheckFailed.load(std::memory_order_acquire) ||
            audioProcessor.isUnsupportedAUHost.load(std::memory_order_acquire)) {
            
            loadMediaButton.setVisible(false);
            settingsButton.setVisible(false);
            saveBadge.setVisible(false);
            savePromptLabel.setVisible(false);
        }
}

EnsoniqSD1AudioProcessorEditor::~EnsoniqSD1AudioProcessorEditor()
{
    stopTimer();
}

void EnsoniqSD1AudioProcessorEditor::timerCallback()
{
    
    // Standard offline protection
    if (audioProcessor.isNonRealtime()) {
        audioProcessor.getFrameFlag().store(false, std::memory_order_relaxed);
        return;
    }
            
    // SAVE MACRO - Flash label
        if (audioProcessor.isSaveMacroActive.load(std::memory_order_acquire)) {
            // Flash 1 sec on, 1 sec off (1000 ms)
            bool flashState = (juce::Time::getMillisecondCounter() % 1000) < 500;
            savePromptLabel.setVisible(flashState);
        } else {
            savePromptLabel.setVisible(false);
        }
    
    // SYSEX TRANSMITING START
    bool currentSysEx = audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire);
        
        if (currentSysEx != lastSysExState) {
            lastSysExState = currentSysEx;

            loadMediaButton.setEnabled(!currentSysEx);
            settingsButton.setEnabled(!currentSysEx);
            saveBadge.setEnabled(!currentSysEx);
            
            repaint();
        }

        else if (currentSysEx) {
            repaint();
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
    
        bool missing = audioProcessor.isRomMissing.load(std::memory_order_acquire);
        bool invalid = audioProcessor.isRomInvalid.load(std::memory_order_acquire);
        bool checkFailed = audioProcessor.isSelfCheckFailed.load(std::memory_order_acquire);
        bool unsupportedHost = audioProcessor.isUnsupportedAUHost.load(std::memory_order_acquire);
            
            // ROM error buttons visibility
            locateRomButton.setVisible(missing);
            rescanRomButton.setVisible(invalid);

            // Main UI buttons should only be visible if there are no errors and MAME can run
            bool showMainUi = !(missing || invalid || checkFailed || unsupportedHost);
            loadMediaButton.setVisible(showMainUi);
            settingsButton.setVisible(showMainUi);
            saveBadge.setVisible(showMainUi);
            
            if (!showMainUi) {
                savePromptLabel.setVisible(false);
            }
}

void EnsoniqSD1AudioProcessorEditor::paint (juce::Graphics& g)
{
    // ==============================================================================
    // --- SAFEGUARDS & ERROR SCREENS ---
    // ==============================================================================
    
    // --- UNSUPPORTED AU HOST WARNING MESSAGE ---
        if (audioProcessor.isUnsupportedAUHost.load(std::memory_order_acquire)) {
            g.fillAll(juce::Colour(0xff222222));
            g.setColour(juce::Colours::orange);
            g.setFont(22.0f);
            
            juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nUnsupported AU Host Detected!\n\nFor DAWs other than Logic, GarageBand, MainStage, Reaper, Ableton Live and Fender Studio Pro (Studio One),\nplease use the VST3 version of this plugin.\n\nThis ensures sample-accurate offline rendering and UI stability.";
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
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nError! Roms not found!\n\n";
        errorMsg += juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getFullPathName();
        errorMsg += "\n\nPlease locate 'sd132.zip' file:";
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20).withTrimmedBottom(120), juce::Justification::centred, 10);
        return;
    }
    
    // --- MISSING FILES WARNING MESSAGE ---
    if (audioProcessor.isRomInvalid.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::red);
        g.setFont(20.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nError! Missing ROM files in zip!\n\nThe 'sd132.zip' file was found, but the following required files are missing from it:\n\n";
        errorMsg += audioProcessor.missingFilesList;
        errorMsg += "\nPlease add them to the zip (folders inside the zip are allowed)\nand push the Rescan button!";
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20).withTrimmedBottom(120), juce::Justification::centred, 10);
        return;
    }
    
    // --- SELF CHECK FAILED WARNING MESSAGE ---
        if (audioProcessor.isSelfCheckFailed.load(std::memory_order_acquire)) {
            g.fillAll(juce::Colour(0xff222222));
            g.setColour(juce::Colours::red);
            g.setFont(22.0f);
            
            juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nSystem Integrity Check Failed!\n\nThe plugin cannot start due to the following issues:\n\n";
            errorMsg += audioProcessor.selfCheckErrorMsg;
            errorMsg += "\n\nPlease fix these permission/installation issues and reload the plugin.";
            
            // Draw the text without trimming for buttons, as there is no retry button for this
            g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 15);
            return;
        }
    
    // --- ENGINE STANDBY / OFFLINE MESSAGE ---
        // If ROMs are fine, NO self-check errors, but MAME is simply not running.
        if (!audioProcessor.isMameRunningFlag() &&
            !audioProcessor.isRomMissing.load(std::memory_order_acquire) &&
            !audioProcessor.isRomInvalid.load(std::memory_order_acquire) &&
            !audioProcessor.isSelfCheckFailed.load(std::memory_order_acquire))
        {
            g.fillAll(juce::Colour(0xff222222));
            g.setColour(juce::Colours::red);
            g.setFont(22.0f);

            juce::String msg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\nMAME(R) Engine Offline\n\nThe emulator is halted or waiting for the host's audio engine.\n\nIf your DAW is currently scanning plugins, this is normal.\nIf the emulator crashed or failed to start, please reload the plugin!";
            g.drawFittedText(msg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
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
        // --- 3. PANELS & BADGES (Floppy / Cartridge / Save) ---
        // (Moved ABOVE Settings so they are naturally dimmed when Settings open!)
        // ==============================================================================
        int viewIdxBadge = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);
        int baseWBadge = 2048;
        int baseHBadge = 925; // Default Compact
        if (viewIdxBadge == 1) baseHBadge = 671;
        else if (viewIdxBadge == 2) baseHBadge = 379;
        else if (viewIdxBadge == 3) baseHBadge = 1476;

        float scaleXBadge = getWidth() / (float)baseWBadge;
        float scaleYBadge = getHeight() / (float)baseHBadge;

        // --- Variables for Media Badges ---
        float badgeW = 260.0f;
        float badgeH = 30.0f;
        float flopX = 0.0f, flopY = 0.0f;
        float cartX = 0.0f, cartY = 0.0f;

        // --- Variables for Save Badge (SYNCED EXACTLY WITH RESIZED) ---
        float saveW = 130.0f;
        float saveH = 30.0f;
        float saveX = 0.0f, saveY = 0.0f;

        if (viewIdxBadge == 0) { // Compact
            flopX = 304.0f; flopY = 402.0f;
            cartX = 574.0f; cartY = 402.0f;
            
            saveW = 130.0f;  saveH = 30.0f;
            saveX = 471.0f;  saveY = 214.0f;
        } else if (viewIdxBadge == 1) { // Full
            badgeW = 234.0f; badgeH = 30.0f;
            flopX = 59.0f;   flopY = 288.0f;
            cartX = 1718.0f; cartY = 176.0f;
            
            saveW = 130.0f;  saveH = 30.0f;
            saveX = 954.0f;  saveY = 325.0f;
        } else if (viewIdxBadge == 2) { // Rack
            badgeW = 192.0f; badgeH = 30.0f;
            flopX = 20.0f;   flopY = 306.0f;
            cartX = 220.0f;  cartY = 306.0f;
            
            saveW = 130.0f;  saveH = 30.0f;
            saveX = 471.0f;  saveY = 214.0f;
        } else if (viewIdxBadge == 3) { // Tablet
            badgeW = 300.0f; badgeH = 40.0f;
            flopX = 36.0f;   flopY = 1303.0f;
            cartX = 356.0f;  cartY = 1303.0f;
            
            saveW = 180.0f;  saveH = 40.0f;
            saveX = 343.0f;  saveY = 448.0f;
        }

        auto drawBadge = [&](float x, float y, float w, float h, const juce::String& text, juce::Drawable* icon, juce::Colour ledColor, float alpha, bool hasLed) {
            float finalX = x * scaleXBadge;
            float finalY = y * scaleYBadge;
            float finalW = w * scaleXBadge;
            float finalH = h * scaleYBadge;

            juce::Rectangle<float> indRect(finalX, finalY, finalW, finalH);

            g.setColour(juce::Colour(0xcc111111).withMultipliedAlpha(alpha));
            g.fillRoundedRectangle(indRect, 4.0f);

            float padding = finalH * 0.2f;
            float iconStartX = indRect.getX() + padding;

            if (hasLed) {
                float ledSize = finalH * 0.35f;
                g.setColour(ledColor.withMultipliedAlpha(alpha));
                g.fillEllipse(iconStartX, indRect.getCentreY() - (ledSize / 2.0f), ledSize, ledSize);
                iconStartX += ledSize + padding;
            } else {
                iconStartX += padding;
            }

            float iconSize = finalH * 0.65f;
            juce::Rectangle<float> iconBounds(iconStartX, indRect.getCentreY() - (iconSize / 2.0f), iconSize, iconSize);

            if (icon != nullptr) {
                icon->drawWithin(g, iconBounds, juce::RectanglePlacement::centred, alpha);
            }

            g.setColour(juce::Colours::white.withMultipliedAlpha(alpha));
            g.setFont(finalH * 0.55f);
            
            float textStartX = iconBounds.getRight() + padding;
            juce::Rectangle<float> textRect(textStartX, indRect.getY(), indRect.getRight() - textStartX, finalH);
            g.drawText(text, textRect, juce::Justification::centredLeft, true);
        };

        bool hasFloppy = audioProcessor.isFloppyLoaded.load(std::memory_order_acquire);
        bool hasCart = audioProcessor.isCartLoaded.load(std::memory_order_acquire);

        if (hasFloppy) drawBadge(flopX, flopY, badgeW, badgeH, audioProcessor.loadedFloppyName, floppyIcon.get(), juce::Colours::limegreen, 1.0f, true);
        if (hasCart)   drawBadge(cartX, cartY, badgeW, badgeH, audioProcessor.loadedCartName, cartIcon.get(), juce::Colours::limegreen, 1.0f, true);
        
        // Dim the save badge dynamically if disabled (e.g. during SysEx)
        float saveAlpha = saveBadge.isEnabled() ? 1.0f : 0.4f;
        drawBadge(saveX, saveY, saveW, saveH, "Save preset", floppyIcon.get(), juce::Colours::transparentBlack, saveAlpha, false);

        // ==============================================================================
        // --- 4. SETTINGS OVERLAY (Dimmed background & Settings Panel)
        // (Drawn last, so it successfully darkens the buttons beneath it)
        // ==============================================================================
        if (isSettingsVisible) {
            g.fillAll(juce::Colour(0x4d000000)); // Dim the entire UI (including the badges!)
            
            auto panelRect = settingsGroup.getBounds().toFloat();
            panelRect.removeFromTop(8.0f);
            
            g.setColour(juce::Colour(0xcc000000)); // Solid dark background for the settings panel
            g.fillRoundedRectangle(panelRect, 5.0f);
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
        g.drawText("Transmitting SYS-EX Data...", panelRect.withTrimmedTop(20.0f).withHeight(30.0f), juce::Justification::centred);
        
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

    // --- DYNAMIC BUTTON POSITIONING & SAVE BADGE BOUNDS ---

        int loadBtnX = 55; int loadBtnY = 403;
        int setBtnX  = 55; int setBtnY  = 443;
        
        float btnWidth = 192.0f;
        float btnHeight = 30.0f;
        
        // Save badge defaults (Compact View)
        float saveW = 130.0f; float saveH = 30.0f;
        float saveX = 471.0f; float saveY = 214.0f;
        
        // Prompt Label center defaults (Compact View)
        // 1024.0f is exactly the horizontal center of your 2048.0f base width
        float promptCenterX = 1013.0f;
        float promptCenterY = 273.0f;

        if (viewIdx == 1) { // Full Keyboard
            loadBtnX = 81 ; loadBtnY = 170;
            setBtnX  = 81;  setBtnY  = 210;
            
            saveW = 130.0f;  saveH = 30.0f;
            saveX = 954.0f; saveY = 325.0f;

            promptCenterX = 1115.0f;
            promptCenterY = 216.0f;
        }
        else if (viewIdx == 2) { // Rack Panel
            loadBtnX = 20;  loadBtnY = 270;
            setBtnX  = 220; setBtnY  = 270;
            
            saveW = 130.0f;  saveH = 30.0f;
            saveX = 471.0f;  saveY = 214.0f;

            promptCenterX = 1013.0f;
            promptCenterY = 273.0f;
        }
        else if (viewIdx == 3) { // Tablet
            btnWidth = 300.0f;
            btnHeight = 40.0f;
            
            loadBtnX = 36;  loadBtnY = 1253;
            setBtnX  = 356; setBtnY  = 1253;
            
            saveW = 180.0f;  saveH = 40.0f;
            saveX = 343.0f;  saveY = 448.0f;

            promptCenterX = 1443.0f;
            promptCenterY = 544.0f;
        }

        loadMediaButton.setBounds(juce::roundToInt(loadBtnX * scaleX), juce::roundToInt(loadBtnY * scaleY),
                                  juce::roundToInt(btnWidth * scaleX), juce::roundToInt(btnHeight * scaleY));
                                  
        settingsButton.setBounds(juce::roundToInt(setBtnX * scaleX), juce::roundToInt(setBtnY * scaleY),
                                 juce::roundToInt(btnWidth * scaleX), juce::roundToInt(btnHeight * scaleY));

        // Place the invisible, clickable hotspot exactly over the drawn save badge
        saveBadge.setBounds(juce::roundToInt(saveX * scaleX), juce::roundToInt(saveY * scaleY),
                            juce::roundToInt(saveW * scaleX), juce::roundToInt(saveH * scaleY));

        // --- POSITION THE PROMPT LABEL (Auto-Centered) ---
        // Create a bounding box large enough to hold the text, scale it, and pin its exact center to your coordinates
        int promptW = juce::roundToInt(400.0f * scaleX);
        int promptH = juce::roundToInt(30.0f * scaleY);
        int finalCenterX = juce::roundToInt(promptCenterX * scaleX);
        int finalCenterY = juce::roundToInt(promptCenterY * scaleY);

        savePromptLabel.setSize(promptW, promptH);
        savePromptLabel.setCentrePosition(finalCenterX, finalCenterY);
    
        // --- FLEXIBLE SETTINGS PANEL ---
        auto bounds = getLocalBounds();
        auto panelRect = bounds.withSizeKeepingCentre(juce::jmin(600, getWidth() - 20), juce::jmin(320, getHeight() - 20));
        settingsGroup.setBounds(panelRect);

        int y = panelRect.getY() + 35;
        
        int totalWidth = 520;
        int startX = panelRect.getCentreX() - (totalWidth / 2);
        int rowHeight = 25;
        int spacing = (panelRect.getHeight() > 270) ? 15 : 5;

        bufferLabel.setBounds(startX, y, 160, rowHeight);
        bufferCombo.setBounds(bufferLabel.getRight() + 5, y, 80, rowHeight);

        viewLabel.setBounds(bufferCombo.getRight() + 15, y, 100, rowHeight);
        viewCombo.setBounds(viewLabel.getRight() + 5, y, 155, rowHeight);

        y += rowHeight + spacing;

    if (panelRect.getHeight() > 280) {
        if (isSettingsVisible) aboutLabel.setVisible(true);
        aboutLabel.setBounds(panelRect.getX() + 10, y, panelRect.getWidth() - 20, 85);
        y += 85 + spacing;
    } else {
        aboutLabel.setVisible(false);
    }

    webLink.setBounds(panelRect.getX() + 10, y, panelRect.getWidth() - 20, rowHeight);
    y += rowHeight + spacing;

    closeSettingsButton.setBounds(panelRect.getCentreX() - 50, y, 100, rowHeight);
    
    // ROM Buttons positions (anchored to the bottom to avoid text overlap)
        locateRomButton.setBounds(getWidth() / 2 - 100, getHeight() - 100, 200, 40);
        rescanRomButton.setBounds(getWidth() / 2 - 100, getHeight() - 100, 200, 40);
}

// ==============================================================================
// MOUSE EVENT INJECTION (Pixel-Perfect 1:1)
// Since MAME renders at the exact window resolution, no complex scaling math is required!
// ==============================================================================
void EnsoniqSD1AudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
    if (isSettingsVisible || audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire) || getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseDown(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::mouseUp(const juce::MouseEvent& e) {
    if (isSettingsVisible || audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire) || getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseUp(e.x, e.y);
    
    // --- CLEAR SAVE PRESET MACRO ON MOUSE LIFT ---
    if (audioProcessor.isSaveMacroActive.load(std::memory_order_acquire)) {
        if (auto* p = audioProcessor.apvts.getParameter("btn_presets")) {
            p->setValueNotifyingHost(0.0f);
        }
        audioProcessor.isSaveMacroActive.store(false, std::memory_order_release);
    }
}

void EnsoniqSD1AudioProcessorEditor::mouseDrag(const juce::MouseEvent& e) {
    if (isSettingsVisible || audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire) || getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseMove(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::mouseMove(const juce::MouseEvent& e) {
    if (isSettingsVisible || audioProcessor.isTransmittingSysEx.load(std::memory_order_acquire) || getWidth() <= 0 || getHeight() <= 0) return;
    audioProcessor.injectMouseMove(e.x, e.y);
}

void EnsoniqSD1AudioProcessorEditor::loadMediaButtonClicked()
{
    juce::PopupMenu m;
    m.addItem(1, "Load Media File (Floppy/Cart/SYX)...");
    m.addSeparator();
    
    // Add Eject options, but disable them if no media is currently loaded!
    m.addItem(2, "Eject Floppy", audioProcessor.isFloppyLoaded.load(std::memory_order_acquire));
    m.addItem(3, "Remove Cartridge", audioProcessor.isCartLoaded.load(std::memory_order_acquire));

    // Show the drop-down menu attached to the button
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&loadMediaButton),
        [this](int result) {
            
            if (result == 1) {
                // --- EXISTING FILE CHOOSER LOGIC ---
                fileChooser = std::make_unique<juce::FileChooser>("Select Floppy Image, Cartridge or SYS-EX file",
                    juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                    "*.img;*.hfe;*.dsk;*.eda;*.syx;*.eeprom;*.rom;*.cart;*.sc32");

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
                            juce::String ext = file.getFileExtension().toLowerCase();

                            if (ext == ".eeprom" || ext == ".rom" || ext == ".sc32" || ext == ".cart") {
                            #ifdef _WIN32
                                audioProcessor.pendingCartPath = file.getFullPathName().toUTF8().getAddress();
                            #else
                                audioProcessor.pendingCartPath = file.getFullPathName().toStdString();
                            #endif
                                audioProcessor.loadedCartName = file.getFileName();
                                audioProcessor.requestCartLoad.store(true, std::memory_order_release);
                                audioProcessor.isCartLoaded.store(true, std::memory_order_release);
                            } else {
                            #ifdef _WIN32
                                audioProcessor.pendingFloppyPath = file.getFullPathName().toUTF8().getAddress();
                            #else
                                audioProcessor.pendingFloppyPath = file.getFullPathName().toStdString();
                            #endif
                                audioProcessor.loadedFloppyName = file.getFileName();
                                audioProcessor.requestFloppyLoad.store(true, std::memory_order_release);
                                audioProcessor.isFloppyLoaded.store(true, std::memory_order_release);
                            }
                            repaint();
                        }
                    }
                });
            }
            else if (result == 2) {
                // --- EJECT FLOPPY ---
                std::lock_guard<std::mutex> lock(audioProcessor.mediaMutex);
                audioProcessor.pendingFloppyPath = "";
                audioProcessor.loadedFloppyName = "";
                audioProcessor.isFloppyLoaded.store(false, std::memory_order_release);
                audioProcessor.requestFloppyLoad.store(true, std::memory_order_release); // Trigger MAME Unload
                repaint();
            }
            else if (result == 3) {
                // --- REMOVE CARTRIDGE ---
                std::lock_guard<std::mutex> lock(audioProcessor.mediaMutex);
                audioProcessor.pendingCartPath = "";
                audioProcessor.loadedCartName = "";
                audioProcessor.isCartLoaded.store(false, std::memory_order_release);
                audioProcessor.requestCartLoad.store(true, std::memory_order_release); // Trigger MAME Unload
                repaint();
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
    loadMediaButton.setEnabled(!isSettingsVisible);
    saveBadge.setEnabled(!isSettingsVisible);
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

    // Save the custom ROM path
    xml.setAttribute("rom_path", audioProcessor.customRomPath);
    
    xml.writeTo(settingsFile);
}

// Check ROMs
void EnsoniqSD1AudioProcessorEditor::locateRomButtonClicked()
{
    romChooser = std::make_unique<juce::FileChooser>("Locate sd132.zip",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.zip");

    auto folderChooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    romChooser->launchAsync(folderChooserFlags, [this](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file.existsAsFile()) {
            
            // Set the custom path directly to the selected zip file
            audioProcessor.customRomPath = file.getFullPathName();
            
            // Immediately save global settings so the path is remembered for the next session
            saveGlobalSettings();
            
            // Re-check ROMs and attempt to boot the engine
            audioProcessor.checkRomAndBootMame();
            repaint();
        }
    });
}
