/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#include "PresetBrowserComponent.h"
#include "PluginEditor.h"
#include "sd1disk.h"
#include "vfxcart.h"
#include "machine/eeprompar.h"

static juce::String decodeEnsoniqNameFromBuf(const char* nameBuf) {
    juce::String progName = "";
    for (int c = 0; c < 11; ++c) {
        unsigned char ch = static_cast<unsigned char>(nameBuf[c]);

        if (ch < 0x20 || ch >= 0x60 || ch == 0x26 || ch == 0x2C || ch == 0x3A || ch == 0x3F) progName += " ";
        else if (ch == 0x21) progName += "0.";
        else if (ch == 0x23) progName += "1.";
        else if (ch == 0x25) progName += "2.";
        else if (ch == 0x28) progName += "3.";
        else if (ch == 0x29) progName += "4.";
        else if (ch == 0x3B) progName += "6.";
        else if (ch == 0x5C) progName += "8.";
        else progName += juce::String::charToString((juce::juce_wchar)ch);
    }
    if (progName.trim().isEmpty()) progName = "[EMPTY]";
    return progName.trim();
}

juce::StringArray extractNamesFromSysEx(const juce::MemoryBlock& sysExData)
{
    juce::StringArray names;
    const uint8_t* data = static_cast<const uint8_t*>(sysExData.getData());
    size_t size = sysExData.getSize();

    // Minimum size check for single preset (103 bytes)
    if (size < 103) return names;
    
    // Header check: F0 0F 05 [00 or 01] 00 [MsgType]
    if (data[0] != 0xF0 || data[1] != 0x0F || data[2] != 0x05) return names;

    uint8_t messageType = data[5];

    if (messageType == 0x02 && size >= 1067) {
            // --- SINGLE PROGRAM SYX ---
            size_t nameMidiOffset = 6 + (498 * 2);
            char nameBuf[12] = { 0 };
            for (int i = 0; i < 11; ++i) {
                uint8_t hi = data[nameMidiOffset + (i * 2)];
                uint8_t lo = data[nameMidiOffset + (i * 2) + 1];
                nameBuf[i] = static_cast<char>((hi << 4) | lo);
            }

            names.add(decodeEnsoniqNameFromBuf(nameBuf));
        }
            else if (messageType == 0x03 && size >= 6367) {
            // --- BANK SYX (Dynamic: 6, 30, or 60 Programs) ---
            int numProgs = (size - 7) / 1060;
            for (int p = 0; p < numProgs; ++p) {
                size_t programStart = 6 + (p * 1060);
                size_t nameMidiOffset = programStart + (498 * 2);
                char nameBuf[12] = { 0 };
                for (int i = 0; i < 11; ++i) {
                    uint8_t hi = data[nameMidiOffset + (i * 2)];
                    uint8_t lo = data[nameMidiOffset + (i * 2) + 1];
                    nameBuf[i] = static_cast<char>((hi << 4) | lo);
                }

                names.add(juce::String::formatted("%02d: ", p) + decodeEnsoniqNameFromBuf(nameBuf));
            }
        }
            else if (messageType == 0x04 && size >= 103) {
                    // --- SINGLE PRESET SYX ---
                    names.add("Preset");
                }
                else if (messageType == 0x05 && size >= 967) {
                    // --- PRESET BANK SYX (Dynamic: 10 or 20 Presets) ---
                    int numPresets = (size - 7) / 96;
                    for (int p = 0; p < numPresets; ++p) {
                        names.add(juce::String::formatted("%02d: Preset", p));
                    }
                }
                return names;
}

static bool isSystemBooting(EnsoniqSD1AudioProcessor& p) {
    if (p.mameMachine == nullptr) return true;

    double now       = p.mameMachine->time().as_double();

    // Cold boot: first 3.5s after MAME starts
    if (now < 3.5) return true;

    // Warm boot (song load / load state): schedTime set by PluginEditor timer.
    // ONLY show boot indicator if the firmware is actually rebooting (mameIsFullyBooted = false).
    // On editor hide/restore, schedTime is also set but mameIsFullyBooted stays true → no false positive.
    double schedTime = p.scheduledCompareResetTime.load(std::memory_order_acquire);
    if (schedTime > 0.0 && now < schedTime
        && !p.mameIsFullyBooted.load(std::memory_order_acquire)) {
        return true;
    }

    return false;
}

static double getSafeBootTime(EnsoniqSD1AudioProcessor& p) {
    if (p.mameMachine == nullptr) return 3.5;
    double now = p.mameMachine->time().as_double();
    double schedTime = p.scheduledCompareResetTime.load(std::memory_order_acquire);
    double bootTime = (schedTime > 3.5) ? schedTime : 3.5;
    return (now > bootTime) ? now : bootTime;
}

PresetBrowserComponent::PresetBrowserComponent(EnsoniqSD1AudioProcessor& p)
    : audioProcessor(p)
{
    headerLabel.setFont(juce::Font(juce::FontOptions(20.0f)).boldened());
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible(headerLabel);
    
    // Progress bar (hidden by default)
    progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colours::orange);
    addAndMakeVisible(progressBar);
    progressBar.setVisible(false);
    
    progressLabel.setFont(juce::FontOptions(12.0f));
    progressLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    progressLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(progressLabel);
    progressLabel.setVisible(false);
    
    closeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffaa3333));
    closeButton.onClick = [this] {
                if (isSYXProcessing || progressValue >= 0.0) return;
                
                if (onClose) onClose();
            };
    addAndMakeVisible(closeButton);
    
    addAndMakeVisible(writeSingleButton);
    addAndMakeVisible(importBankButton);
    
    // UX: Back button, which ONLY takes you back to the root directory of the images
    backToFilesButton.setColour(juce::TextButton::buttonColourId, juce::Colours::dodgerblue);
    backToFilesButton.onClick = [this] {
        if (isViewingDiskBank && currentlyLoadedArchive.existsAsFile()) {
            int32_t err = 0;
            DiskImage* img = nullptr;
            juce::String path = currentlyLoadedArchive.getFullPathName();
            
            std::string stdPath = path.toStdString();
            const char* cPath = stdPath.c_str();

            if (currentlyLoadedArchive.getFileExtension().toLowerCase() == ".hfe") img = sd1_read_hfe(cPath, &err);
            else img = sd1_disk_open(cPath, &err);
            
            if (img) {
                clearBankList();
                
                // List all 4 subdirectories (stock disks may use subdir 3, not 0)
                for (uint8_t subdir = 0; subdir < 4; ++subdir) {
                    uintptr_t fileCount = 0;
                    Sd1DirectoryEntry* entries = sd1_disk_list(img, subdir, &fileCount);
                    if (entries && fileCount > 0) {
                        for (uintptr_t i = 0; i < fileCount; ++i) {
                            char nameBuf[12] = {0};
                            memcpy(nameBuf, entries[i].name, 11);
                            nameBuf[11] = '\0';
                            
                            juce::String fatName(nameBuf);
                            bankRawNames.add(fatName);
                            bankEntryTypes.push_back(entries[i].file_type);
                            
                            // Store the exact 11-byte name for the FAT engine
                            std::array<char, 11> exact;
                            std::memcpy(exact.data(), entries[i].name, 11);
                            bankExactNames.push_back(exact);
                            
                            juce::String itemName = fatName + " (Type: " + juce::String((int)entries[i].file_type) + ")";
                            bankModel.items.add(itemName);
                        }
                        sd1_entries_free(entries, fileCount);
                    }
                }
                sd1_disk_free(img);
                
                isViewingDiskBank = false;
                backToFilesButton.setEnabled(false); // Back to root
                bankContentList.updateContent();
                bankContentList.repaint();
                
                selectionNameLabel.setText("Disk Image: " + currentlyLoadedArchive.getFileName(), juce::dontSendNotification);
                writeSingleButton.setEnabled(false);
                importBankButton.setEnabled(false);
                importBankButton.setButtonText("Import Full Bank");  // Reset from "Import Sequencer Data"
                exportSysExButton.setEnabled(false);
                playHintLabel.setVisible(false);
            }
        }
    };
    addAndMakeVisible(backToFilesButton);
    backToFilesButton.setEnabled(false);
    
    addAndMakeVisible(exportSysExButton);
        exportSysExButton.onClick = [this] {
                if (audioProcessor.mameMachine == nullptr) return;
                
                // --- DIRECT SEQUENCE EXPORT INTERCEPT ---
                // Bypass block logic and export the perfect SysEx buffer directly
                if (importBankButton.getButtonText() == "Import Sequencer Data" && currentlyLoadedBankData.getSize() > 0) {
                    juce::String defName = "SD1-Sequence.syx";
                    juce::String selText = selectionNameLabel.getText();
                    if (selText.contains(": ")) {
                        defName = selText.substring(selText.indexOf(": ") + 2).trim().replaceCharacters(" /\\:", "____") + ".syx";
                    }

                    // If combined dump (programs + sequences), export them together
                    auto dataToSave = std::make_shared<juce::MemoryBlock>();
                    if (combinedDumpProgramSyx.getSize() > 0) {
                        dataToSave->append(combinedDumpProgramSyx.getData(), combinedDumpProgramSyx.getSize());
                    }
                    dataToSave->append(currentlyLoadedBankData.getData(), currentlyLoadedBankData.getSize());

                    auto chooser = std::make_shared<juce::FileChooser>("Export Sequence SysEx",
                        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getChildFile(defName), "*.syx");

                    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                        [this, chooser, dataToSave](const juce::FileChooser& fc) {
                            juce::File file = fc.getResult();
                            if (file != juce::File()) {
                                if (!file.hasFileExtension(".syx")) file = file.withFileExtension(".syx");
                                if (file.replaceWithData(dataToSave->getData(), dataToSave->getSize()))
                                    selectionNameLabel.setText("Exported: " + file.getFileName(), juce::dontSendNotification);
                            }
                        });
                    return;
                }
                
            bool isIntRam = (currentCategory == "INT (RAM)");
            bool isCart = (currentCategory == "CART");
            bool isExternalFolder = (currentCategory == "Documents/EnsoniqSD1" || currentCategory == "Downloads" || currentCategory.startsWith("BOOKMARK:"));
            bool isArchiveDrillIn = (isViewingDiskBank && !isIntRam && !isCart);
                        
                        juce::SparseSet<int> selectedRows;
                        
                        if (bankContentList.getNumSelectedRows() > 0) {
                            selectedRows = bankContentList.getSelectedRows();
                        } else {
                            selectedRows = contentList.getSelectedRows();
                        }
                
                int selectionCount = selectedRows.size();
                if (selectionCount == 0) return;

                // --- 1. COLLECT DATA BLOCKS ---
                std::vector<juce::MemoryBlock> blocks;
                bool isPresetType = false;
                
                // Set source
                if (isIntRam) {

                    auto* osram_share = audioProcessor.mameMachine->root_device().memshare("osram");
                    if (osram_share) {
                        uint8_t interleaved[31800];
                        uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
                        for (int i = 0; i < 31800; i++) interleaved[i] = osram[(0x0003C8 + i) ^ 1];
                        uint8_t* linear = nullptr; uintptr_t linLen = 0;
                        if (sd1_deinterleave_sixty_programs(interleaved, 31800, &linear, &linLen) == SD1_OK) {
                            for (int i = 0; i < selectionCount; ++i) {
                                juce::MemoryBlock b(530, true);
                                b.copyFrom(linear + (selectedRows[i] * 530), 0, 530);
                                blocks.push_back(b);
                            }
                            sd1_bytes_free(linear, linLen);
                        }
                    }
                }
                else if (isCart) {
                    auto* eeprom = audioProcessor.mameMachine->root_device().subdevice<eeprom_parallel_base_device>("cart:eeprom");
                    if (eeprom) {
                        std::vector<uint8_t> storage(0x8000);
                        for (int i = 0; i < 0x8000; i++) {
                            storage[i] = eeprom->read(i);
                        }
                        for (int i = 0; i < selectionCount; ++i) {
                            juce::MemoryBlock b(530, true);
                            b.copyFrom(&storage[selectedRows[i] * 530], 0, 530);
                            blocks.push_back(b);
                        }
                    }
                }
                else if (currentlyLoadedArchive.existsAsFile()) {
                                    juce::String ext = currentlyLoadedArchive.getFileExtension().toLowerCase();
                                    bool isDisk = (ext == ".img" || ext == ".hfe" || ext == ".dsk" || ext == ".eda");

                                    if (isDisk && !isViewingDiskBank) {
                                        // Root of Disk Image: Extract files directly from FAT
                                        
                                        // --- 1. DIRECT SEQUENCE EXPORT INTERCEPT ---
                                        // (Sequences cannot be batched, they must be exported one by one)
                                        if (selectionCount == 1) {
                                            int r = selectedRows[0];
                                            // Bounds check — bankEntryTypes may have been cleared (Type 0x0E case)
                                            if (r < 0 || r >= (int)bankEntryTypes.size()) {
                                                selectionCount = 0; // Fall through to Fix 4
                                            } else {
                                            uint8_t fileType = bankEntryTypes[r];
                                            
                                            if (fileType == 0x11 || fileType == 0x12 || fileType == 0x13) {
                                                int32_t err = 0;
                                                DiskImage* img = nullptr;
                                                std::string stdPath = currentlyLoadedArchive.getFullPathName().toStdString();
                                                if (ext == ".hfe") img = sd1_read_hfe(stdPath.c_str(), &err);
                                                else               img = sd1_disk_open(stdPath.c_str(), &err);
                                                
                                                if (img) {
                                                    char exactCStr[12] = { 0 };
                                                    std::memcpy(exactCStr, bankExactNames[r].data(), 11);
                                                    for (int k = 10; k >= 0 && exactCStr[k] == ' '; --k) exactCStr[k] = '\0';
                                                    
                                                    uint8_t* rawData = nullptr;
                                                    uintptr_t rawLen = 0;
                                                    if (sd1_disk_extract(img, exactCStr, &rawData, &rawLen) == SD1_OK) {
                                                        uint8_t* seqPayload = nullptr;
                                                        uintptr_t seqLen = 0;
                                                        int32_t convErr = SD1_ERR_INVALID_SYSEX;
                                                        
                                                        if (fileType == 0x12) convErr = sd1_disk_to_thirty_sequences(rawData, rawLen, &seqPayload, &seqLen);
                                                        else if (fileType == 0x13) {
                                                            convErr = sd1_disk_to_allsequences(rawData, rawLen, true, &seqPayload, &seqLen);
                                                            if (convErr != SD1_OK) {
                                                                if (seqPayload) { sd1_bytes_free(seqPayload, seqLen); seqPayload = nullptr; }
                                                                convErr = sd1_disk_to_allsequences(rawData, rawLen, false, &seqPayload, &seqLen);
                                                            }
                                                        } else {
                                                            convErr = sd1_disk_to_thirty_sequences(rawData, rawLen, &seqPayload, &seqLen);
                                                            if (convErr != SD1_OK) {
                                                                if (seqPayload) { sd1_bytes_free(seqPayload, seqLen); seqPayload = nullptr; }
                                                                convErr = sd1_disk_to_allsequences(rawData, rawLen, false, &seqPayload, &seqLen);
                                                            }
                                                        }
                                                        
                                                        if (convErr == SD1_OK && seqPayload != nullptr) {
                                                            juce::MemoryBlock transferData;
                                                            uint8_t p1[17];
                                                            p1[0]=0xF0; p1[1]=0x0F; p1[2]=0x05; p1[3]=0x00; p1[4]=0x00; p1[5]=0x00;
                                                            p1[6]=0x00; p1[7]=0x0C;
                                                            uint32_t sz = static_cast<uint32_t>(seqLen);
                                                            p1[8]=(sz>>28)&0x0F; p1[9]=(sz>>24)&0x0F;
                                                            p1[10]=(sz>>20)&0x0F; p1[11]=(sz>>16)&0x0F;
                                                            p1[12]=(sz>>12)&0x0F; p1[13]=(sz>>8)&0x0F;
                                                            p1[14]=(sz>>4)&0x0F;  p1[15]=sz&0x0F;
                                                            p1[16]=0xF7;
                                                            transferData.append(p1, 17);
                                                            uint8_t p2hdr[6] = { 0xF0, 0x0F, 0x05, 0x00, 0x00, 0x0A };
                                                            transferData.append(p2hdr, 6);
                                                            for (uintptr_t i = 0; i < seqLen; ++i) {
                                                                uint8_t hi = (seqPayload[i] >> 4) & 0x0F;
                                                                uint8_t lo = seqPayload[i] & 0x0F;
                                                                transferData.append(&hi, 1); transferData.append(&lo, 1);
                                                            }
                                                            uint8_t eox = 0xF7; transferData.append(&eox, 1);
                                                            
                                                            juce::String defName = (fileType == 0x11) ? "SD1-SingleSeq.syx" :
                                                                                   (fileType == 0x12) ? "SD1-30Seq.syx" : "SD1-60Seq.syx";
                                                                                   
                                                            auto chooser = std::make_shared<juce::FileChooser>("Export Sequence SysEx",
                                                                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getChildFile(defName), "*.syx");
                                                            
                                                            auto dataToSave = std::make_shared<juce::MemoryBlock>(transferData);
                                                            
                                                            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                                                [this, chooser, dataToSave](const juce::FileChooser& fc) {
                                                                    juce::File file = fc.getResult();
                                                                    if (file != juce::File()) {
                                                                        if (!file.hasFileExtension(".syx")) file = file.withFileExtension(".syx");
                                                                        if (file.replaceWithData(dataToSave->getData(), dataToSave->getSize()))
                                                                            selectionNameLabel.setText("Exported: " + file.getFileName(), juce::dontSendNotification);
                                                                    }
                                                                });
                                                            
                                                            sd1_bytes_free(seqPayload, seqLen);
                                                        }
                                                        sd1_bytes_free(rawData, rawLen);
                                                    }
                                                    sd1_disk_free(img);
                                                }
                                                return; // Sequence export handled completely!
                                            }
                                        }

                                        // --- 2. STANDARD PROGRAM / PRESET EXPORT ---
                                        int32_t err = 0;
                                        DiskImage* img = nullptr;
                                        std::string stdPath = currentlyLoadedArchive.getFullPathName().toStdString();
                                        if (ext == ".hfe") img = sd1_read_hfe(stdPath.c_str(), &err);
                                        else               img = sd1_disk_open(stdPath.c_str(), &err);

                                        if (img) {
                                            for (int i = 0; i < selectionCount; ++i) {
                                                int r = selectedRows[i];
                                                if (r >= 0 && r < bankExactNames.size()) {
                                                    char exactCStr[12] = { 0 };
                                                    std::memcpy(exactCStr, bankExactNames[r].data(), 11);
                                                    for (int k = 10; k >= 0 && exactCStr[k] == ' '; --k) exactCStr[k] = '\0';

                                                    uint8_t* rawData = nullptr;
                                                    uintptr_t rawLen = 0;
                                                    if (sd1_disk_extract(img, exactCStr, &rawData, &rawLen) == SD1_OK) {
                                                        
                                                        uint8_t fileType = bankEntryTypes[r];
                                                        if (fileType == 0x0E || fileType == 0x0F || fileType == 0x10) isPresetType = true;
                                                                                                
                                                        size_t stride = isPresetType ? 48 : 530;
                                                        int itemsInFile = 1;
                                                        if (fileType == 0x0B) itemsInFile = 6;
                                                            else if (fileType == 0x0C) itemsInFile = 30;
                                                            else if (fileType == 0x0D) itemsInFile = 60;
                                                            else if (fileType == 0x0F) itemsInFile = 10;
                                                            else if (fileType == 0x10) itemsInFile = 20;

                                                        uint8_t* actualData = rawData;
                                                        uint8_t* deinterleaved = nullptr;
                                                        uintptr_t deLen = 0;
                                                                                                
                                                        if (fileType == 0x0D) {
                                                            if (sd1_deinterleave_sixty_programs(rawData, rawLen, &deinterleaved, &deLen) == SD1_OK) {
                                                               actualData = deinterleaved;
                                                            }
                                                        }
                                                                                                
                                                        // We only copy exactly the items expected (ignoring any FAT sector padding!)
                                                        for (int k = 0; k < itemsInFile; ++k) {
                                                            juce::MemoryBlock b(stride, true);
                                                            b.copyFrom(actualData + (k * stride), 0, stride);
                                                            blocks.push_back(b);
                                                        }
                                                                                                
                                                        if (deinterleaved) sd1_bytes_free(deinterleaved, deLen);
                                                                                                
                                                        sd1_bytes_free(rawData, rawLen);
                                                    }
                                                }
                                            }
                                            sd1_disk_free(img);
                                        }
                                    } // end else (bounds check)
                                    } else {
                                        // Drill-in, or .syx bank, or .cart
                                        size_t dataSize = currentlyLoadedBankData.getSize();
                                        int totalItems = (int)bankModel.items.size();
                                        size_t stride = (totalItems > 0) ? dataSize / totalItems : 530;
                                        if (stride == 48) isPresetType = true;
                                        
                                        for (int i = 0; i < selectionCount; ++i) {
                                            juce::MemoryBlock b(stride, true);
                                            b.copyFrom(static_cast<const uint8_t*>(currentlyLoadedBankData.getData()) + (selectedRows[i] * stride), 0, stride);
                                            blocks.push_back(b);
                                        }
                                    }
                                }
                                else if (isExternalFolder && !isViewingDiskBank) {
                                    // Reading syx files in folder
                                    for (int i = 0; i < selectionCount; ++i) {
                                        juce::String fileName = currentListItems[selectedRows[i]];
                                        juce::File f = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getChildFile(fileName);
                                        if (!f.existsAsFile()) f = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Downloads").getChildFile(fileName);
                                        if (!f.existsAsFile() && customBrowseFolder.exists()) f = customBrowseFolder.getChildFile(fileName);
                                        
                                        juce::MemoryBlock syx;
                                        f.loadFileAsData(syx);
                                        if (syx.getSize() >= 7) {
                                            size_t payloadSize = (syx.getSize() - 7) / 2;
                                            if (payloadSize == 48 || payloadSize == 480 || payloadSize == 960) isPresetType = true;
                                            juce::MemoryBlock raw(payloadSize, true);
                                            const uint8_t* d = static_cast<const uint8_t*>(syx.getData());
                                            for (size_t k = 0; k < payloadSize; ++k)
                                                static_cast<uint8_t*>(raw.getData())[k] = (d[6 + (k * 2)] << 4) | d[6 + (k * 2) + 1];
                                            blocks.push_back(raw);
                                        }
                                    }
                                }

                                if (blocks.empty()) return;

                            // --- 2. BUILD EXPORT BUFFER ---
                            juce::MemoryBlock exportRawData;
                            bool isBank = (selectionCount > 1);
                            
                            if (isBank) {
                                            size_t bankSize = 0;
                                            size_t stride = isPresetType ? 48 : 530;
                                            int maxSlots = 0;
                                            
                                            if (isPresetType) {
                                                // FORCE 20 SLOTS: The SD-1 SysEx Spec ONLY defines a 20-Preset Bank (0x05)!
                                                // Sending 10 presets via SysEx is invalid and results in Receive Error.
                                                bankSize = 20 * 48; maxSlots = 20;
                                            } else {
                                                if (selectionCount <= 6) { bankSize = 6 * 530; maxSlots = 6; }
                                                else if (selectionCount <= 30) { bankSize = 30 * 530; maxSlots = 30; }
                                                else { bankSize = 60 * 530; maxSlots = 60; }
                                            }
                                    
                                    exportRawData.setSize(bankSize, true);
                                    for (int i = 0; i < (int)blocks.size() && i < maxSlots; ++i) {
                                        exportRawData.copyFrom(blocks[i].getData(), i * stride, stride);
                                    }
                                } else {
                                    exportRawData = blocks[0];
                                }

                                // --- 3. SAVE DIALOG ---
                                juce::String defName;
                                        size_t finalSize = exportRawData.getSize();
                                        
                                        if (isPresetType) {
                                            if (finalSize == 48) defName = "SD1-Preset.syx";
                                            else defName = "SD1-20Presets.syx"; // 10 is naturally padded to 20
                                        } else {
                                            if (finalSize == 530) defName = "SD1-Program.syx";
                                            else if (finalSize <= 6 * 530) defName = "SD1-6Programs.syx";
                                            else if (finalSize <= 30 * 530) defName = "SD1-30Programs.syx";
                                            else defName = "SD1-60Programs.syx";
                                        }
                            
                                auto chooser = std::make_shared<juce::FileChooser>("Export SysEx",
                                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getChildFile(defName), "*.syx");

                                auto dataToSave = std::make_shared<juce::MemoryBlock>(exportRawData);

                                chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                        [this, isPresetType, chooser, dataToSave](const juce::FileChooser& fc) {
                                            juce::File file = fc.getResult();
                                            if (file == juce::File()) return;
                                            if (!file.hasFileExtension(".syx")) file = file.withFileExtension(".syx");

                                size_t rawSize = dataToSave->getSize();
                                uint8_t msgType = 0x02; // Default to Program
                                if (rawSize == 48) msgType = 0x04;      // Single Preset
                                   else if (rawSize == 960) msgType = 0x05; // All Presets Bank
                                   else if (rawSize > 530) msgType = 0x03;  // Program Bank

                                juce::MemoryBlock syx;
                                // Header: F0 0F 05 00 00 [Type] - Always use 00 for compatibility
                                uint8_t head[6] = { 0xF0, 0x0F, 0x05, 0x00, 0x00, msgType };
                                syx.append(head, 6);
                                    
                        const uint8_t* raw = static_cast<const uint8_t*>(dataToSave->getData());
                        for (size_t i = 0; i < dataToSave->getSize(); ++i) {
                            uint8_t hi = (raw[i] >> 4) & 0x0F;
                            uint8_t lo = raw[i] & 0x0F;
                            syx.append(&hi, 1); syx.append(&lo, 1);
                        }
                        uint8_t eox = 0xF7; syx.append(&eox, 1);
                        
                        if (file.replaceWithData(syx.getData(), syx.getSize()))
                            selectionNameLabel.setText("Exported: " + file.getFileName(), juce::dontSendNotification);
                    });
            };
    
    bankContentList.setModel(&bankModel);
    bankModel.parentComponent = this;
    bankContentList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff151515));
    addAndMakeVisible(bankContentList);
    bankContentList.setMultipleSelectionEnabled(true);
    
    refreshButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    refreshButton.onClick = [this] {
        if (currentCategory.isNotEmpty()) {
            updateContentList(currentCategory);
        }
    };
    addAndMakeVisible(refreshButton);
    
    // --- IMPORT FULL BANK BUTTON (also handles sequence import) ---
            importBankButton.onClick = [this] {
                if (audioProcessor.mameMachine == nullptr) return;
                setCompareState(false);
                
                juce::String btnText = importBankButton.getButtonText();
                
                if (btnText == "Import Preset") {
                    // Just sends the single preset to the Edit Buffer again
                    wasSYXPreview = true;
                    injectAndPreview();
                    selectionNameLabel.setText("Preset imported to buffer", juce::dontSendNotification);
                    return;
                }
            
            // --- SEQUENCE & ALL PRESETS IMPORT ---

            if (btnText == "Import Sequencer Data" || btnText == "Import All Presets") {
                if (currentlyLoadedBankData.getSize() == 0) return;

                bool hasCombinedPrograms = (combinedDumpProgramSyx.getSize() > 0);
                        
                juce::String title = (btnText == "Import All Presets") ? "Import All Presets" : "Import Sequencer Data";
                juce::String msg = (btnText == "Import All Presets")
                                           ? "This will overwrite all 20 internal Presets.\nProceed?"
                                           : hasCombinedPrograms
                                               ? "This combined dump will:\n  1. Overwrite all 60 programs in Internal RAM\n  2. Send All Sequences data via MIDI SysEx\n\nProceed?"
                                               : "This will send sequencer data to the SD-1 via MIDI SysEx.\nProceed?";

                auto options = juce::MessageBoxOptions()
                            .withTitle(title)
                            .withMessage(msg)
                            .withButton("Yes").withButton("No")
                            .withIconType(juce::MessageBoxIconType::WarningIcon)
                            .withAssociatedComponent(this);
                        
                juce::AlertWindow::showAsync(options, [this, msg, btnText, hasCombinedPrograms](int result) {
                    if (result != 1) return;
                                    
                        if (btnText == "Import All Presets") {
                            wasPresetPreviewed = true;
                                        juce::MemoryBlock transferData;
                                        uint8_t p2hdr[6] = { 0xF0, 0x0F, 0x05, 0x00, 0x00, 0x05 };
                                        transferData.append(p2hdr, 6);
                                        uint8_t paddedRaw[960] = {0};
                                        size_t copyLen = juce::jmin((size_t)960, currentlyLoadedBankData.getSize());
                                        std::memcpy(paddedRaw, currentlyLoadedBankData.getData(), copyLen);
                                        for (uintptr_t i = 0; i < 960; ++i) {
                                            uint8_t hi = (paddedRaw[i] >> 4) & 0x0F;
                                            uint8_t lo = paddedRaw[i] & 0x0F;
                                            transferData.append(&hi, 1);
                                            transferData.append(&lo, 1);
                                        }
                                        uint8_t eox = 0xF7;
                                        transferData.append(&eox, 1);
                                        startSequenceTransfer(transferData, msg);
                        } else if (hasCombinedPrograms) {
                            // Combined dump: import programs via RAM writer, then sequence
                            // Step 1: denibblize 0x03 AllPrograms SysEx → linear 31800 bytes
                            const uint8_t* pd = static_cast<const uint8_t*>(combinedDumpProgramSyx.getData());
                            size_t ps = combinedDumpProgramSyx.getSize();
                            const uint8_t* nibs = pd + 6; // skip F0 0F 05 00 00 03
                            size_t nibCount = ps - 7;     // minus header(6) + F7(1)
                            juce::MemoryBlock linearProg(nibCount / 2, true);
                            uint8_t* dst = static_cast<uint8_t*>(linearProg.getData());
                            for (size_t i = 0; i + 1 < nibCount; i += 2)
                                dst[i / 2] = (uint8_t)((nibs[i] << 4) | nibs[i + 1]);

                            // Step 2: interleave and inject into RAM (same as Import Full Bank)
                            juce::MemoryBlock fullBank(31800, true);
                            fullBank.copyFrom(linearProg.getData(), 0, juce::jmin(linearProg.getSize(), (size_t)31800));
                            const uint8_t* linearPtr = static_cast<const uint8_t*>(fullBank.getData());
                            uint8_t* interleavedData = nullptr;
                            uintptr_t intLen = 0;
                            if (sd1_interleave_sixty_programs(linearPtr, 31800, &interleavedData, &intLen) == SD1_OK) {
                                audioProcessor.pendingBankData.setSize(0);
                                audioProcessor.pendingBankData.append(interleavedData, intLen);
                                audioProcessor.pendingBankInjection.store(true, std::memory_order_release);
                                sd1_bytes_free(interleavedData, intLen);
                            }

                            // Step 3: Bank Select refresh (same as Import Full Bank)
                            juce::Component::SafePointer<PresetBrowserComponent> safeThis(this);
                            isSYXProcessing = true;
                            juce::Timer::callAfterDelay(1000, [safeThis]() {
                                if (safeThis == nullptr || safeThis->audioProcessor.mameMachine == nullptr) return;
                                if (auto* p = safeThis->audioProcessor.apvts.getParameter("btn_sounds")) {
                                    p->setValueNotifyingHost(1.0f);
                                    juce::Timer::callAfterDelay(100, [p]{ p->setValueNotifyingHost(0.0f); });
                                }
                                double t = safeThis->audioProcessor.mameMachine->time().as_double() + 0.1;
                                safeThis->audioProcessor.pushMidiByte(0xB0, t);
                                safeThis->audioProcessor.pushMidiByte(32,   t);
                                safeThis->audioProcessor.pushMidiByte(0,    t);
                                safeThis->audioProcessor.pushMidiByte(0xC0, t + 0.05);
                                safeThis->audioProcessor.pushMidiByte(1,    t + 0.05);
                                safeThis->audioProcessor.pushMidiByte(0xC0, t + 0.10);
                                safeThis->audioProcessor.pushMidiByte(0,    t + 0.10);
                            });

                            // Step 4: sequence import after bank refresh settles
                            juce::MemoryBlock seqSyx = currentlyLoadedBankData;
                            juce::Timer::callAfterDelay(2000, [safeThis, seqSyx]() mutable {
                                if (safeThis == nullptr) return;
                                safeThis->startSequenceTransfer(seqSyx, "Importing sequences...");
                            });
                        } else {
                            startSequenceTransfer(currentlyLoadedBankData, msg);
                        }
                    });
                return;
            }
            
            // --- NORMAL BANK IMPORT ---
            size_t bankSize = currentlyLoadedBankData.getSize();
            if (bankSize == 3180 || bankSize == 15900 || bankSize == 31800) {

            // Confirmation dialog — this will overwrite all 60 INT RAM programs
            auto options = juce::MessageBoxOptions()
                .withTitle("Import Full Bank")
                .withMessage("Warning!\n\nThis will overwrite all 60 programs in Internal RAM.\nThis action cannot be undone.\n\nProceed?")
                .withButton("Yes")
                .withButton("No")
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withAssociatedComponent(this);
            
                juce::AlertWindow::showAsync(options, [this, bankSize](int result) {
                    if (result != 1) return;  // 1 = "Yes" (first button)
                                        
                    // MAKE AN EMPTY BANK
                    juce::MemoryBlock fullBank(31800, true);
                    // COPY PRESETS TO EMPTY BANK
                    fullBank.copyFrom(currentlyLoadedBankData.getData(), 0, bankSize);
                    
                    const uint8_t* linearData = static_cast<const uint8_t*>(fullBank.getData());
                    uint8_t* interleavedData = nullptr;
                    uintptr_t intLen = 0;
                    
                    if (sd1_interleave_sixty_programs(linearData, 31800, &interleavedData, &intLen) == SD1_OK) {
                        audioProcessor.pendingBankData.setSize(0);
                        audioProcessor.pendingBankData.append(interleavedData, intLen);
                        audioProcessor.pendingBankInjection.store(true, std::memory_order_release);
                        sd1_bytes_free(interleavedData, intLen);
                    } else {
                        selectionNameLabel.setText("Failed to interleave bank!", juce::dontSendNotification);
                        return;
                    }

                });
                wasSYXImport = true;
                
                // Refresh the firmware by selecting Bank 0 / Program 0 via MIDI
                juce::Component::SafePointer<PresetBrowserComponent> safeThis(this);
                isSYXProcessing = true;
                juce::Timer::callAfterDelay(1000, [safeThis]() {
                if (safeThis == nullptr || safeThis->audioProcessor.mameMachine == nullptr) return;
                    if (auto* p = safeThis->audioProcessor.apvts.getParameter("btn_sounds")) {
                                        p->setValueNotifyingHost(1.0f);
                                        juce::Timer::callAfterDelay(100, [p]{ p->setValueNotifyingHost(0.0f); });
                                    }
                    double t = safeThis->audioProcessor.mameMachine->time().as_double() + 0.1;
                    safeThis->audioProcessor.pushMidiByte(0xB0, t);  // CC on ch 1
                    safeThis->audioProcessor.pushMidiByte(32, t);    // Bank Select LSB
                    safeThis->audioProcessor.pushMidiByte(0, t);     // Bank 0 = INT
                    safeThis->audioProcessor.pushMidiByte(0xC0, t + 0.05);
                    safeThis->audioProcessor.pushMidiByte(1, t + 0.05);
                    safeThis->audioProcessor.pushMidiByte(0xC0, t + 0.10);
                    safeThis->audioProcessor.pushMidiByte(0, t + 0.10);
                });
                
                selectionNameLabel.setText("Bank successfully imported to INT RAM!", juce::dontSendNotification);
                isSYXProcessing = false;
            }
        };

        // --- WRITE SINGLE PROGRAM BUTTON ---
        writeSingleButton.onClick = [this] {
            if (currentlySelectedSingleRawData.getSize() < 530 || audioProcessor.mameMachine == nullptr) return;
            if (wpState > 0) return; // write already in progress

            juce::PopupMenu m;
            m.addSectionHeader("Write Program to INT RAM Slot:");
            juce::StringArray realNames = getRealIntRamNames();
            for (int i = 0; i < 60; ++i) {
                juce::String patchName = (i < realNames.size()) ? realNames[i] : "[EMPTY]";
                m.addItem(i + 1, juce::String::formatted("%02d: ", i) + patchName);
            }

            m.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
                if (result > 0 && wpState == 0) {
                    wpTargetSlot = result - 1;
                    wpState = 1;
                    wpCmdSent = false;
                    wpStateStartMs = juce::Time::getMillisecondCounterHiRes();
                    isSYXProcessing = true;
                    audioProcessor.suppressMidiInput.store(true, std::memory_order_release);

                    transferDurationMs = 6000.0;
                    transferStartTimeMs = juce::Time::getMillisecondCounterHiRes();
                    progressValue = 0.0;
                    progressBar.setVisible(true);
                    progressLabel.setText("Writing to INT Slot " + juce::String(wpTargetSlot) + "...",
                                          juce::dontSendNotification);
                    progressLabel.setVisible(true);
                    startTimerHz(30);
                }
            });
        };
    
    // --- SELECT ITEM FROM INSPECTOR ---
    bankModel.onSelect = [this](int bankRow) {
        
        // LOCK FOR SEQ INSPECTOR VIEW
                    if (importBankButton.getButtonText() == "Import Sequencer Data") {
                            return;
                        }
                            
                    // Multi-selection: disable Write Single, keep Import enabled if bank is loaded
                    int numSelected = bankContentList.getNumSelectedRows();
                    if (numSelected > 1) {
                        bool canExport = false;
                        juce::String statusText = "Multiple selection (" + juce::String(numSelected) + " items)";
                        
                        if (currentlyLoadedArchive.existsAsFile()) {
                            juce::String ext = currentlyLoadedArchive.getFileExtension().toLowerCase();
                            
                            if (ext == ".syx" || ext == ".eeprom" || ext == ".rom" || ext == ".cart" || ext == ".sc32") {
                                
                                canExport = true;
                                statusText = juce::String(numSelected) + " programs selected";
                            } else if (ext == ".img" || ext == ".hfe" || ext == ".dsk" || ext == ".eda") {
                                if (isViewingDiskBank) {
                                   
                                    canExport = true;
                                    
                                    size_t dataSize = currentlyLoadedBankData.getSize();
                                    int itemsCount = bankModel.items.size();
                                    if (itemsCount > 0 && dataSize / itemsCount == 48) {
                                        statusText = juce::String(numSelected) + " presets selected";
                                    } else {
                                        statusText = juce::String(numSelected) + " programs selected";
                                    }
                                } else {
                                    
                                    auto selectedRows = bankContentList.getSelectedRows();
                                    bool allSame = true;
                                    bool hasValid = false;
                                    uint8_t refType = 0;
                                    
                                    for (int i = 0; i < selectedRows.size(); ++i) {
                                        int r = selectedRows[i];
                                        if (r < 0 || r >= bankEntryTypes.size()) continue;
                                        uint8_t t = bankEntryTypes[r];
                                        
                                        if (!hasValid) {
                                            refType = t;
                                            hasValid = true;
                                        } else if (t != refType) {
                                            allSame = false;
                                            break;
                                        }
                                    }
                                    
                                    if (hasValid && allSame && (refType == 0x0A || refType == 0x0E)) {
                                        canExport = true;
                                        statusText = juce::String(numSelected) + (refType == 0x0A ? " programs selected" : " presets selected");
                                    } else {
                                        statusText = "Mixed or invalid types selected - Export disabled";
                                    }
                                }
                            }
                        }
                        
                        writeSingleButton.setEnabled(false);
                        selectionNameLabel.setText(statusText, juce::dontSendNotification);
                        importBankButton.setEnabled(currentlyLoadedBankData.getSize() == 31800);
                        exportSysExButton.setEnabled(canExport);
                        return;
                    }
        
        if (!currentlyLoadedArchive.existsAsFile()) return;
        juce::String ext = currentlyLoadedArchive.getFileExtension().toLowerCase();
        
        if (ext == ".syx" || ext == ".eeprom" || ext == ".rom" || ext == ".cart" || ext == ".sc32") {
            // Both SYX banks and cartridge files: currentlyLoadedBankData is linear 60 × 530 bytes
            if (currentlyLoadedBankData.getSize() >= 31800 && bankRow >= 0 && bankRow < 60) {
                if (progressValue >= 0.0) return;
                const uint8_t* data = static_cast<const uint8_t*>(currentlyLoadedBankData.getData());
                
                currentlySelectedSingleRawData.setSize(530);
                std::memcpy(currentlySelectedSingleRawData.getData(), data + bankRow * 530, 530);
                
                selectionNameLabel.setText(bankModel.items[bankRow].substring(4).trim(), juce::dontSendNotification);
                writeSingleButton.setEnabled(true);
                importBankButton.setEnabled(true);
                setCompareState(true);
                injectAndPreview();
            }
        }
        else if (ext == ".img" || ext == ".hfe" || ext == ".dsk" || ext == ".eda") {
            int32_t err = 0;
            DiskImage* img = nullptr;
            juce::String path = currentlyLoadedArchive.getFullPathName();
            std::string stdPath = path.toStdString();
            const char* cPath = stdPath.c_str();
            if (ext == ".hfe") img = sd1_read_hfe(cPath, &err);
            else               img = sd1_disk_open(cPath, &err);
            
            if (img) {
                if (!isViewingDiskBank) {
                    // Use the parallel arrays for reliable name/type lookup
                    // instead of fragile display-string parsing
                    juce::String lookupName = (bankRow < bankRawNames.size())
                        ? bankRawNames[bankRow] : juce::String();
                    uint8_t fileType = (bankRow < (int)bankEntryTypes.size())
                        ? bankEntryTypes[bankRow] : 0;
                    
                    // Convert JUCE string to std::string, trimming trailing spaces but keeping internal ones
                    std::string searchName = bankRawNames[bankRow].trimEnd().toStdString();
                    
                    // Use the exact 11-byte FAT name, trimming only trailing spaces.
                    // Leading spaces are significant (e.g. " 30-GROOVES") and must be preserved.
                    char exactCStr[12] = { 0 };
                    if (bankRow < (int)bankExactNames.size()) {
                         std::memcpy(exactCStr, bankExactNames[bankRow].data(), 11);
                         // Trim trailing spaces only (not leading)
                         for (int k = 10; k >= 0 && exactCStr[k] == ' '; --k)
                             exactCStr[k] = '\0';
                     }
                    
                    // SD1_FILE_SIXTY_PROGRAMS (0x0D=13) — drill into the 60-program bank
                    if (fileType == 0x0D) {
                        uint8_t* rawData = nullptr;
                        uintptr_t rawLen = 0;
                        int32_t extErr = sd1_disk_extract(img, exactCStr, &rawData, &rawLen);
                        if (extErr == SD1_OK) {
                            if (rawLen >= 31800) {
                                                            uint8_t* deinterleaved = nullptr;
                                                            uintptr_t deLen = 0;
                                                            if (sd1_deinterleave_sixty_programs(rawData, rawLen, &deinterleaved, &deLen) == SD1_OK) {
                                                                // TRUNCATE padding to exact Ensoniq block sizes!
                                                                currentlyLoadedBankData.setSize(0);
                                                                currentlyLoadedBankData.append(deinterleaved, 31800);
                                                                sd1_bytes_free(deinterleaved, deLen);
                                                            } else {
                                                                currentlyLoadedBankData.setSize(0);
                                                                currentlyLoadedBankData.append(rawData, 31800);
                                                            }
                                
                                isViewingDiskBank = true;
                                currentDiskBankName = lookupName.trim();
                                clearBankList();
                                
                                for (int i = 0; i < 60; ++i) {
                                    char nameBuf[13] = {0};
                                    const uint8_t* linearData = static_cast<const uint8_t*>(currentlyLoadedBankData.getData());
                                    sd1_program_name_from_slot(linearData + (i * 530), 530, nameBuf, sizeof(nameBuf));
                                    bankModel.items.add(juce::String::formatted("%02d: ", i) + decodeEnsoniqName(nameBuf));
                                }
                                backToFilesButton.setEnabled(true);
                                importBankButton.setEnabled(true);
                                bankContentList.updateContent();
                            } else {
                                selectionNameLabel.setText("Error: Bank file too small", juce::dontSendNotification);
                                isErrorState = true;
                            }
                            sd1_bytes_free(rawData, rawLen);
                        } else {
                            selectionNameLabel.setText("Extract Error: " + juce::String(sd1_error_message(extErr)), juce::dontSendNotification);
                            isErrorState = true;
                        }
                    }
                    // SD1_FILE_SIX_PROGRAMS (0x0B) / SD1_FILE_THIRTY_PROGRAMS (0x0C) — multi-program banks
                                        else if (fileType == 0x0B || fileType == 0x0C) {
                                            uint8_t* rawData = nullptr;
                                            uintptr_t rawLen = 0;
                                            int32_t extErr = sd1_disk_extract(img, exactCStr, &rawData, &rawLen);
                                            if (extErr == SD1_OK) {
                                                int programCount = (fileType == 0x0B) ? 6 : 30;
                                                int expectedSize = programCount * 530;
                                                
                                                if ((int)rawLen >= expectedSize) {
                                                    isViewingDiskBank = true;
                                                    currentDiskBankName = lookupName.trim();
                                                    clearBankList();
                                                    
                                                    // TRUNCATE padding to exact Ensoniq block sizes!
                                                    currentlyLoadedBankData.setSize(0);
                                                    currentlyLoadedBankData.append(rawData, expectedSize);
                                
                                for (int i = 0; i < programCount; ++i) {
                                    char nameBuf[13] = {0};
                                    sd1_program_name_from_slot(rawData + (i * 530), 530, nameBuf, sizeof(nameBuf));
                                    bankModel.items.add(juce::String::formatted("%02d: ", i) + decodeEnsoniqName(nameBuf));
                                }
                                backToFilesButton.setEnabled(true);
                                importBankButton.setEnabled(true);
                                bankContentList.updateContent();
                            } else {
                                selectionNameLabel.setText("Error: Multi-program file too small", juce::dontSendNotification);
                                isErrorState = true;
                            }
                            sd1_bytes_free(rawData, rawLen);
                        } else {
                            selectionNameLabel.setText("Extract Error: " + juce::String(sd1_error_message(extErr)), juce::dontSendNotification);
                            isErrorState = true;
                        }
                    }
                    // SD1_FILE_ONE_PROGRAM (0x0A) — single program, preview via SysEx
                    else if (fileType == 0x0A) {
                        if (progressValue >= 0.0) return;
                        uint8_t* rawData = nullptr;
                        uintptr_t rawLen = 0;
                        int32_t extErr = sd1_disk_extract(img, exactCStr, &rawData, &rawLen);
                        if (extErr == SD1_OK && rawLen >= 530) {
                            currentlySelectedSingleRawData.setSize(0);
                            currentlySelectedSingleRawData.append(rawData, 530);
                            selectionNameLabel.setText("Program: " + lookupName.trim(), juce::dontSendNotification);
                            writeSingleButton.setEnabled(true);
                            setCompareState(true);
                            injectAndPreview();
                            sd1_bytes_free(rawData, rawLen);
                        } else if (extErr != SD1_OK) {
                            selectionNameLabel.setText("Extract Error: " + juce::String(sd1_error_message(extErr)), juce::dontSendNotification);
                            isErrorState = true;
                        }
                    }
                    // SD1_FILE_ONE_PRESET (0x0E) / SD1_FILE_TEN_PRESETS (0x0F) / SD1_FILE_TWENTY_PRESETS (0x10)
                    else if (fileType == 0x0E || fileType == 0x0F || fileType == 0x10) {
                             uint8_t* rawData = nullptr;
                             uintptr_t rawLen = 0;
                             int32_t extErr = sd1_disk_extract(img, exactCStr, &rawData, &rawLen);
                             if (extErr == SD1_OK) {

                                if (fileType == 0x10 || fileType == 0x0F) {
                                   int numPresets = (fileType == 0x0F) ? 10 : 20;
                                   int expectedSize = numPresets * 48;
                                                                                                                                
                                    if (rawLen >= expectedSize) {
                                       // --- MULTI PRESETS (BANK) ---
                                       selectionNameLabel.setText("Preset Bank: " + lookupName.trim(), juce::dontSendNotification);
                                       writeSingleButton.setEnabled(false);
                                       exportSysExButton.setEnabled(true);
                                                                                                            
                                      // TRUNCATE padding to exact Ensoniq block sizes
                                      currentlyLoadedBankData.setSize(0);
                                      currentlyLoadedBankData.append(rawData, expectedSize);
                                                                                                
                                      importBankButton.setEnabled(true);
                                      importBankButton.setButtonText("Import All Presets");
                                                                                                                                                
                                      isErrorState = false;
                                                                                                                                    
                                      // Allow drill-in!
                                      isViewingDiskBank = true;
                                      currentDiskBankName = lookupName.trim();
                                      clearBankList();
                                      for (int i = 0; i < numPresets; ++i) {
                                           bankModel.items.add(juce::String::formatted("%02d: Preset", i));
                                        }
                                           backToFilesButton.setEnabled(true);
                                           bankContentList.updateContent();
                                        }
                                                                                                                                
                                      } else if (fileType == 0x0E && rawLen >= 48) {
                                      // --- ONE PRESET (PREVIEW) ---
                                      currentlySelectedSingleRawData.setSize(0);
                                      // TRUNCATE padding to exact Ensoniq block sizes
                                      currentlySelectedSingleRawData.append(rawData, 48);
                                                                                                                
                                      selectionNameLabel.setText("Preset: " + lookupName.trim(), juce::dontSendNotification);
                                      writeSingleButton.setEnabled(false);
                                      importBankButton.setEnabled(true);
                                      importBankButton.setButtonText("Import Preset");
                                      exportSysExButton.setEnabled(true);
                                                                                                                            
                                      isErrorState = false;
                                      wasSYXPreview = true;
                                      setCompareState(false);
                                                                                                                            
                                      injectAndPreview();
                                    }
                                    sd1_bytes_free(rawData, rawLen);
                            } else {
                                selectionNameLabel.setText("Extract Error: " + juce::String(sd1_error_message(extErr)), juce::dontSendNotification);
                                isErrorState = true;
                            }
                        }
                    // SD1_FILE_ONE_SEQUENCE (0x11) / SD1_FILE_THIRTY_SEQUENCES (0x12) / SD1_FILE_SIXTY_SEQUENCES (0x13)
                    else if (fileType == 0x11 || fileType == 0x12 || fileType == 0x13) {
                        uint8_t* rawData = nullptr;
                        uintptr_t rawLen = 0;
                        int32_t extErr = sd1_disk_extract(img, exactCStr, &rawData, &rawLen);
                        if (extErr == SD1_OK) {
                            // Show "Converting..." immediately — hw_sysex conversion can take ~1s on large files
                            selectionNameLabel.setText("Converting sequence data...", juce::dontSendNotification);
                            importBankButton.setEnabled(false);
                            exportSysExButton.setEnabled(false);
                            
                            // Move expensive conversion off the message thread
                            auto rawCopyForConv = std::make_shared<juce::MemoryBlock>(rawData, rawLen);
                            sd1_bytes_free(rawData, rawLen);
                            uint8_t ft = fileType;
                            juce::String lookupNameCopy = lookupName;

                            juce::Thread::launch([this, rawCopyForConv, ft, lookupNameCopy]() {
                                uint8_t* seqPayload = nullptr;
                                uintptr_t seqLen = 0;
                                int32_t convErr = SD1_ERR_INVALID_SYSEX;
                                const uint8_t* rd = static_cast<const uint8_t*>(rawCopyForConv->getData());
                                uintptr_t rl = static_cast<uintptr_t>(rawCopyForConv->getSize());

                                bool hasProgs = (ft == 0x13);
                                auto tryHwSeq = [&](bool hp, bool al) -> int32_t {
                                    if (seqPayload) { sd1_bytes_free(seqPayload, seqLen); seqPayload = nullptr; seqLen = 0; }
                                    return (ft == 0x12 || ft == 0x11)
                                        ? sd1_disk_to_thirty_sequences_hw_sysex(rd, rl, al, &seqPayload, &seqLen)
                                        : sd1_disk_to_allsequences_hw_sysex(rd, rl, hp, al, &seqPayload, &seqLen);
                                };

                                convErr = tryHwSeq(hasProgs, false);
                                if (convErr != SD1_OK && convErr != SD1_ERR_SLOT59_HAS_DATA && ft == 0x13) {
                                    hasProgs = false;
                                    convErr = tryHwSeq(false, false);
                                }

                                if (convErr == SD1_ERR_SLOT59_HAS_DATA) {
                                    auto rawCopy2 = rawCopyForConv;
                                    bool hasPr = hasProgs;
                                    if (seqPayload) { sd1_bytes_free(seqPayload, seqLen); seqPayload = nullptr; }
                                    juce::MessageManager::callAsync([this, rawCopy2, hasPr, ft]() {
                                        juce::AlertWindow::showOkCancelBox(
                                            juce::MessageBoxIconType::WarningIcon,
                                            "Sequence slot 59 will be lost",
                                            "This file has data in sequence slot 59, which cannot be sent to the SD-1 hardware.\n\nSlot 59 will be discarded. Continue?",
                                            "Import anyway", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create([this, rawCopy2, hasPr, ft](int result) {
                                                if (result != 1) { selectionNameLabel.setText("Import cancelled.", juce::dontSendNotification); return; }
                                                const uint8_t* r = static_cast<const uint8_t*>(rawCopy2->getData());
                                                uintptr_t l = static_cast<uintptr_t>(rawCopy2->getSize());
                                                uint8_t* sp = nullptr; uintptr_t sl = 0;
                                                int32_t ce = (ft == 0x12 || ft == 0x11)
                                                    ? sd1_disk_to_thirty_sequences_hw_sysex(r, l, true, &sp, &sl)
                                                    : sd1_disk_to_allsequences_hw_sysex(r, l, hasPr, true, &sp, &sl);
                                                if (ce == SD1_OK && sp != nullptr) {
                                                    juce::MemoryBlock syx(sp, sl); sd1_bytes_free(sp, sl);
                                                    juce::String typeName = (ft==0x11)?"Single Sequence":(ft==0x12)?"30 Sequences":"60 Sequences";
                                                    currentlyLoadedBankData = syx;
                                                    importBankButton.setEnabled(true); importBankButton.setButtonText("Import Sequencer Data");
                                                    selectionNameLabel.setText(typeName + " (slot 59 dropped)", juce::dontSendNotification);
                                                } else {
                                                    selectionNameLabel.setText("Slot59 retry error: " + juce::String(sd1_error_message(ce)), juce::dontSendNotification);
                                                }
                                            })
                                        );
                                    });
                                    return;
                                }

                                // Back to message thread for UI update
                                juce::MemoryBlock result;
                                juce::MemoryBlock progSyx;  // non-empty if disk file has embedded programs
                                juce::String errMsg;
                                if (convErr == SD1_OK && seqPayload != nullptr) {
                                    // Library may report wrong seqLen (e.g. 1.2GB for a 91k SysEx).
                                    // Scan for the actual F7 terminator to find the real end.
                                    const uintptr_t MAX_SCAN = 5u * 1024u * 1024u;
                                    uintptr_t actualLen = seqLen;
                                    if (seqLen > MAX_SCAN) {
                                        actualLen = MAX_SCAN;
                                        for (uintptr_t i = 6; i < MAX_SCAN && i < seqLen; ++i) {
                                            if (seqPayload[i] == 0xF7) { actualLen = i + 1; break; }
                                        }
                                    }
                                    result = juce::MemoryBlock(seqPayload, actualLen);
                                    sd1_bytes_free(seqPayload, seqLen);

                                    // If disk file had embedded programs (has_programs=true succeeded),
                                    // extract them from raw[11776:43576] = 31800 bytes interleaved.
                                    // Deinterleave → linear → nibblize as AllPrograms SysEx (0x03).
                                    if (hasProgs && rl >= 43576) {
                                        const uint8_t* interleaved = rd + 11776;
                                        uint8_t* linear = nullptr; uintptr_t linLen = 0;
                                        if (sd1_deinterleave_sixty_programs(interleaved, 31800, &linear, &linLen) == SD1_OK && linLen == 31800) {
                                            // Build AllPrograms SysEx: F0 0F 05 00 00 03 [nibblize 31800] F7
                                            juce::MemoryBlock pb(6 + 31800*2 + 1, false);
                                            auto* p = static_cast<uint8_t*>(pb.getData());
                                            p[0]=0xF0; p[1]=0x0F; p[2]=0x05; p[3]=0x00; p[4]=0x00; p[5]=0x03;
                                            for (uintptr_t i = 0; i < 31800; ++i) {
                                                p[6 + i*2]     = (linear[i] >> 4) & 0x0F;
                                                p[6 + i*2 + 1] =  linear[i]       & 0x0F;
                                            }
                                            p[6 + 31800*2] = 0xF7;
                                            progSyx = pb;
                                            sd1_bytes_free(linear, linLen);
                                        }
                                    }
                                } else {
                                    errMsg = "Sequence convert error: " + juce::String(sd1_error_message(convErr));
                                }

                                juce::MessageManager::callAsync([this, result, progSyx, errMsg, ft, lookupNameCopy]() mutable {
                                    if (errMsg.isNotEmpty()) {
                                        selectionNameLabel.setText(errMsg, juce::dontSendNotification);
                                        isErrorState = true;
                                        return;
                                    }
                                    juce::String seqName = (ft==0x11)?"Single Sequence":(ft==0x12)?"30 Sequences":"60 Sequences";
                                    bool isCombined = (progSyx.getSize() > 0);
                                    juce::String typeName = isCombined ? ("Combined: 60 Programs + " + seqName) : seqName;
                                    currentlyLoadedBankData = result;
                                    combinedDumpProgramSyx = progSyx;  // empty unless has_programs
                                    backToFilesButton.setEnabled(true);
                                    isViewingDiskBank = true;
                                    importBankButton.setEnabled(true);
                                    importBankButton.setButtonText("Import Sequencer Data");
                                    exportSysExButton.setEnabled(true);
                                    selectionNameLabel.setText(typeName + ": " + lookupNameCopy.trim(), juce::dontSendNotification);
                                    clearBankList();
                                    double transferSec = result.getSize() / 3125.0;
                                    if (isCombined) {
                                        bankModel.items.add("Combined dump: 60 programs + " + seqName);
                                        bankModel.items.add("Clicking Import will:");
                                        bankModel.items.add("  1. Import 60 programs into RAM (with confirmation)");
                                        bankModel.items.add("  2. Import " + seqName + " data (~" + juce::String((int)std::ceil(transferSec)) + "s)");
                                    } else {
                                        bankModel.items.add(seqName + ": " + lookupNameCopy.trim());
                                        bankModel.items.add("Size: " + juce::String((int)result.getSize()) + " bytes");
                                        bankModel.items.add("Transfer: ~" + juce::String((int)std::ceil(transferSec)) + "s");
                                        bankModel.items.add("Click 'Import Sequencer Data' to send.");
                                    }
                                    bankContentList.updateContent();
                                });
                            }); // end Thread::launch (rawData already freed above)
                        } else {
                            selectionNameLabel.setText("Extract Error: " + juce::String(sd1_error_message(extErr)), juce::dontSendNotification);
                            isErrorState = true;
                        }
                    }
                    // Type 0x14 = System Exclusive
                    else if (fileType == 0x14) {
                        selectionNameLabel.setText("System exclusive file: not importable", juce::dontSendNotification);
                    }
                    // Type 0x15 = System Setup
                    else if (fileType == 0x15) {
                        selectionNameLabel.setText("System setup file: not importable", juce::dontSendNotification);
                    }
                    // Type 0x16 (22) = Sequencer OS — skip
                    else if (fileType == 0x16) {
                        selectionNameLabel.setText("Sequencer OS firmware: not importable", juce::dontSendNotification);
                    }
                    else {
                        selectionNameLabel.setText("File type 0x" + juce::String::toHexString(fileType) + " not supported", juce::dontSendNotification);
                    }
                } else {
                    // Already viewing inside a bank — select individual program
                    size_t bankSize = currentlyLoadedBankData.getSize();
                    int programIndex = bankRow;
                                        
                    // --- CHECK BLOCK SIZE (Program vs Preset) ---
                                         if (bankSize > 0 && (bankSize % 530 == 0) && bankSize >= (programIndex + 1) * 530) {
                                            if (progressValue >= 0.0) return;
                                                                
                                            const uint8_t* data = static_cast<const uint8_t*>(currentlyLoadedBankData.getData());
                                            const uint8_t* slotData = data + (programIndex * 530);
                                                                
                                            currentlySelectedSingleRawData.setSize(0);
                                            currentlySelectedSingleRawData.append(slotData, 530);
                                            selectionNameLabel.setText(bankModel.items[bankRow].substring(4).trim(), juce::dontSendNotification);
                                            writeSingleButton.setEnabled(true);
                                            importBankButton.setEnabled(true);
                                            setCompareState(true);
                                            injectAndPreview();
                                        }
                                        else if (bankSize > 0 && (bankSize % 48 == 0) && bankSize >= (programIndex + 1) * 48) {
                                            if (progressValue >= 0.0) return;
                                            
                                            const uint8_t* data = static_cast<const uint8_t*>(currentlyLoadedBankData.getData());
                                            const uint8_t* slotData = data + (programIndex * 48);
                                            
                                            currentlySelectedSingleRawData.setSize(0);
                                            currentlySelectedSingleRawData.append(slotData, 48);
                                            selectionNameLabel.setText(bankModel.items[bankRow].substring(4).trim(), juce::dontSendNotification);
                                            writeSingleButton.setEnabled(false);
                                            importBankButton.setEnabled(true);
                                            importBankButton.setButtonText("Import Preset");
                                            exportSysExButton.setEnabled(true);
                                            
                                            wasSYXPreview = true;
                                            setCompareState(false);
                                            injectAndPreview();
                                        }
                                    }
                            sd1_disk_free(img);
            }
        }
    };
      
    addAndMakeVisible(sourcesTree);
    addAndMakeVisible(contentList);
    addAndMakeVisible(inspectorGroup);

    selectionNameLabel.setFont(juce::FontOptions(18.0f));
    selectionNameLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(selectionNameLabel);
    
    playHintLabel.setFont(juce::Font(juce::FontOptions(15.0f)).italicised());
    playHintLabel.setColour(juce::Label::textColourId, juce::Colours::limegreen);
    playHintLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(playHintLabel);
    playHintLabel.setVisible(false);
    
    contentList.setModel(this); 
    contentList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff2a2a2a)); 
    contentList.setRowHeight(24);
    contentList.setMultipleSelectionEnabled(true);
    
    rebuildTree();
    
    sourcesTree.setRootItemVisible(false);
    sourcesTree.setDefaultOpenness(true);
    sourcesTree.setColour(juce::TreeView::backgroundColourId, juce::Colour(0xff151515));
    sourcesTree.setIndentSize(1);
    
    // Timer runs from construction — needed for boot indicator repaint.
    // Without this, paintOverChildren draws the boot bar but no repaint()
    // is called when booting ends, so the bar stays until user interaction.
    startTimerHz(30);
}

void PresetBrowserComponent::rebuildTree()
{
    sourcesTree.setRootItem(nullptr);
    rootItem = std::make_unique<SourceTreeItem>("Root", true);
    
    auto onTreeSelect = [this](juce::String name) { this->updateContentList(name); };

    auto* internalNode = new SourceTreeItem("SD-1 INTERNAL BANKS", true);
    auto* intNode = new SourceTreeItem("INT (RAM)");
    intNode->onSelection = onTreeSelect;
    internalNode->addSubItem(intNode);
    auto* rom0Node = new SourceTreeItem("ROM0");
    rom0Node->onSelection = onTreeSelect;
    internalNode->addSubItem(rom0Node);
    auto* rom1Node = new SourceTreeItem("ROM1");
    rom1Node->onSelection = onTreeSelect;
    internalNode->addSubItem(rom1Node);
    auto* cartNode = new SourceTreeItem("CART");
    cartNode->onSelection = onTreeSelect;
    internalNode->addSubItem(cartNode);
    rootItem->addSubItem(internalNode);

    auto* localNode = new SourceTreeItem("LOCAL COMPUTER", true);
    auto* docsNode = new SourceTreeItem("Documents/EnsoniqSD1");
    docsNode->onSelection = onTreeSelect;
    localNode->addSubItem(docsNode);
    auto* downNode = new SourceTreeItem("Downloads");
    downNode->onSelection = onTreeSelect;
    localNode->addSubItem(downNode);

    // Saved folder bookmarks with red X to remove
    for (int i = 0; i < audioProcessor.bookmarkFolders.size(); ++i) {
        juce::File folder(audioProcessor.bookmarkFolders[i]);
        auto* bmNode = new SourceTreeItem(folder.getFileName());
        bmNode->itemName = "BOOKMARK:" + audioProcessor.bookmarkFolders[i];
        bmNode->onSelection = onTreeSelect;
        int bookmarkIndex = i;
        bmNode->onRemove = [this, bookmarkIndex]() {
            if (bookmarkIndex < audioProcessor.bookmarkFolders.size()) {
                audioProcessor.bookmarkFolders.remove(bookmarkIndex);
                if (auto* editor = dynamic_cast<EnsoniqSD1AudioProcessorEditor*>(getParentComponent()))
                    editor->saveGlobalSettings();
                rebuildTree();
            }
        };
        localNode->addSubItem(bmNode);
    }

    // --- Only show the "Add Folder..." button if we haven't reached the 10 bookmark limit ---
        if (audioProcessor.bookmarkFolders.size() < 10) {
            auto* addNode = new SourceTreeItem("Add Folder...");
            addNode->onSelection = onTreeSelect;
            localNode->addSubItem(addNode);
        }
    rootItem->addSubItem(localNode);
    sourcesTree.setRootItem(rootItem.get());
}

// =========================================================================================
// STATE SAVE / RESTORE
// =========================================================================================

void PresetBrowserComponent::saveStateToProcessor()
{
    auto& state = audioProcessor.fileManagerState;
    state.visible = (audioProcessor.requestedViewIndex.load() == 4);
    state.category = currentCategory;
    state.viewingDiskBank = isViewingDiskBank;
    state.openedDiskBankName = isViewingDiskBank ? currentDiskBankName : "";
    
    state.selectedRow = contentList.getLastRowSelected();
    state.selectedName = (state.selectedRow >= 0 && state.selectedRow < currentListItems.size())
                         ? currentListItems[state.selectedRow] : "";
    
    state.bankSelectedRow = bankContentList.getLastRowSelected();
    state.bankSelectedName = (state.bankSelectedRow >= 0 && state.bankSelectedRow < bankModel.items.size())
                              ? bankModel.items[state.bankSelectedRow] : "";
    
    // Save the first visible row as scroll position
    auto* viewport = contentList.getViewport();
    state.scrollPosition = viewport ? viewport->getViewPositionY() : 0;
    auto* bankViewport = bankContentList.getViewport();
    state.bankScrollPosition = bankViewport ? bankViewport->getViewPositionY() : 0;
    
    if (currentlyLoadedArchive.existsAsFile())
        state.openedFilePath = currentlyLoadedArchive.getFullPathName();
    else
        state.openedFilePath = "";
    
    if (currentCategory.startsWith("BOOKMARK:"))
        state.activeBookmark = currentCategory.substring(9);
    else
        state.activeBookmark = "";
}

void PresetBrowserComponent::restoreStateFromProcessor()
{
    auto& state = audioProcessor.fileManagerState;
    
    if (state.category.isEmpty()) return;
    
    isRestoringUI = true;
    
    // Restore category (this populates the content list)
    updateContentList(state.category);
    contentList.updateContent();
    
    auto* root = sourcesTree.getRootItem();
    if (root != nullptr) {
        for (int i = 0; i < root->getNumSubItems(); ++i) {
            auto* mainCat = root->getSubItem(i);
            if (auto* stMain = dynamic_cast<SourceTreeItem*>(mainCat)) {
                if (stMain->itemName == state.category) stMain->setSelected(true, false);
            }
            for (int j = 0; j < mainCat->getNumSubItems(); ++j) {
                auto* subCat = mainCat->getSubItem(j);
                if (auto* stSub = dynamic_cast<SourceTreeItem*>(subCat)) {
                    if (stSub->itemName == state.category) stSub->setSelected(true, false);
                }
            }
        }
    }

    // Restore opened file (for external categories)
    if (state.openedFilePath.isNotEmpty()) {
        juce::File file(state.openedFilePath);
        if (file.existsAsFile()) {
            int fileRow = currentListItems.indexOf(file.getFileName());
            if (fileRow >= 0) {
                contentList.selectRow(fileRow);
                
                auto now = juce::Time::getCurrentTime();
                auto source = juce::Desktop::getInstance().getMainMouseSource();
                juce::MouseEvent e(source, {}, {}, 0, 0, 0, 0, 0,
                                   &contentList, &contentList, now, {}, now, 1, false);
                listBoxItemClicked(fileRow, e);
                
                if (state.viewingDiskBank && state.openedDiskBankName.isNotEmpty()) {
                    int diskRow = -1;
                    for (int i = 0; i < bankRawNames.size(); ++i) {
                        if (bankRawNames[i].trim() == state.openedDiskBankName) {
                            diskRow = i;
                            break;
                        }
                    }
                    if (diskRow >= 0) {
                        bankModel.onSelect(diskRow);
                    }
                }
            }
        }
    }
    
    // Restore scroll positions
    if (auto* viewport = contentList.getViewport())
        viewport->setViewPosition(0, state.scrollPosition);
    if (auto* bankViewport = bankContentList.getViewport())
        bankViewport->setViewPosition(0, state.bankScrollPosition);
    
    // Restore selections — primary: by name, fallback: by row index, final: no selection
    // Content list
    {
        int rowToSelect = -1;
        
        // 1. Try name-based lookup (immune to folder content changes)
        if (state.selectedName.isNotEmpty())
            rowToSelect = currentListItems.indexOf(state.selectedName);
        
        // 2. Fall back to saved row index (only if in range and no name or name not found)
        if (rowToSelect < 0 && state.selectedRow >= 0 && state.selectedRow < currentListItems.size())
            rowToSelect = state.selectedRow;
        
        // 3. Default: no selection (rowToSelect = -1 → nothing selected)
        if (rowToSelect >= 0) {
                    contentList.selectRow(rowToSelect, false, true);
                    
                    // --- Show play hint label for internal programs on restore ---
                    bool isInternal = (state.category == "INT (RAM)" || state.category.startsWith("ROM") || state.category == "CART");
                    if (isInternal) {
                        playHintLabel.setVisible(true);
                        
                        // --- Force synth to sync with restored internal state ---
                        bool wasRestoring = isRestoringUI;
                        isRestoringUI = false;
                        auto now = juce::Time::getCurrentTime();
                        auto source = juce::Desktop::getInstance().getMainMouseSource();
                        juce::MouseEvent e(source, {}, {}, 0, 0, 0, 0, 0, &contentList, &contentList, now, {}, now, 1, false);
                        listBoxItemClicked(rowToSelect, e);
                        isRestoringUI = wasRestoring;
                    }
        }
    }
    
    // Bank list
    if (state.bankSelectedRow >= 0 || state.bankSelectedName.isNotEmpty()) {
        int bankRowToSelect = -1;
        
        // 1. Try name-based lookup
        if (state.bankSelectedName.isNotEmpty()) {
            for (int i = 0; i < bankModel.items.size(); ++i) {
                if (bankModel.items[i] == state.bankSelectedName) {
                    bankRowToSelect = i;
                    break;
                }
            }
        }
        
        // 2. Fall back to saved bank row index
        if (bankRowToSelect < 0 && state.bankSelectedRow >= 0 && state.bankSelectedRow < bankModel.items.size())
            bankRowToSelect = state.bankSelectedRow;
        
        if (bankRowToSelect >= 0) {
            bankContentList.selectRow(bankRowToSelect, false, true);
            
            if (currentlyLoadedArchive.existsAsFile()) {
                            juce::String ext = currentlyLoadedArchive.getFileExtension().toLowerCase();
                            bool isDisk = (ext == ".img" || ext == ".hfe" || ext == ".dsk" || ext == ".eda");

                            if (isDisk && state.viewingDiskBank) {
                                bankModel.onSelect(bankRowToSelect);
                            }
                            
                            // --- CHECK PROGRAM BLOCK SIZE ---
                            size_t bankSize = currentlyLoadedBankData.getSize();
                                            if (bankSize > 0 && (bankSize % 530 == 0) && bankSize >= (size_t)(bankRowToSelect + 1) * 530) {
                                                const uint8_t* data = static_cast<const uint8_t*>(currentlyLoadedBankData.getData());
                                                currentlySelectedSingleRawData.setSize(0);
                                                currentlySelectedSingleRawData.append(data + bankRowToSelect * 530, 530);
                                                selectionNameLabel.setText(bankModel.items[bankRowToSelect].substring(4).trim(), juce::dontSendNotification);
                                                writeSingleButton.setEnabled(true);
                                                importBankButton.setEnabled(true);
                                                
                                                // --- Force synth to sync with restored external program ---
                                                // Fresh boot: no previous preview to exit, just inject directly.
                                                // wasSYXPreview set AFTER so injectAndPreview doesn't send EXIT.
                                                bool wasRestoring = isRestoringUI;
                                                isRestoringUI = false;
                                                injectAndPreview();
                                                wasSYXPreview = true;
                                                isRestoringUI = wasRestoring;
                                                
                                                // --- Show play hint label for external programs on restore ---
                                                playHintLabel.setVisible(true);
                                            }
                                            // --- Add restore branch for Presets ---
                                            else if (bankSize > 0 && (bankSize % 48 == 0) && bankSize >= (size_t)(bankRowToSelect + 1) * 48) {
                                                const uint8_t* data = static_cast<const uint8_t*>(currentlyLoadedBankData.getData());
                                                currentlySelectedSingleRawData.setSize(0);
                                                currentlySelectedSingleRawData.append(data + bankRowToSelect * 48, 48);
                                                selectionNameLabel.setText(bankModel.items[bankRowToSelect].substring(4).trim(), juce::dontSendNotification);
                                                writeSingleButton.setEnabled(false);
                                                importBankButton.setEnabled(true);
                                                importBankButton.setButtonText("Import Preset");
                                                
                                                // --- Force synth to sync with restored external preset ---
                                                bool wasRestoring = isRestoringUI;
                                                isRestoringUI = false;
                                                setCompareState(false);
                                                injectAndPreview();
                                                wasSYXPreview = true;
                                                isRestoringUI = wasRestoring;
                                                
                                                playHintLabel.setVisible(true);
                                            }
                        }
        }
    }
        
        isRestoringUI = false;
    }

PresetBrowserComponent::~PresetBrowserComponent() { stopTimer(); }

// HELPER: Reading Actual INT RAM Names
juce::String PresetBrowserComponent::decodeEnsoniqName(const char* nameBuf) {
    juce::String progName = "";
    for (int c = 0; c < 11; ++c) {
        unsigned char ch = static_cast<unsigned char>(nameBuf[c]);
        if (ch < 0x20 || ch >= 0x60 || ch == 0x26 || ch == 0x2C || ch == 0x3A || ch == 0x3F) progName += " ";
        else if (ch == 0x21) progName += "0.";
        else if (ch == 0x23) progName += "1.";
        else if (ch == 0x25) progName += "2.";
        else if (ch == 0x28) progName += "3.";
        else if (ch == 0x29) progName += "4.";
        else if (ch == 0x3B) progName += "6.";
        else if (ch == 0x5C) progName += "8.";
        else progName += juce::String::charToString((juce::juce_wchar)ch);
    }
    if (progName.trim().isEmpty()) progName = "[EMPTY]";
    return progName.trim();
}

juce::StringArray PresetBrowserComponent::getRealIntRamNames() {
    juce::StringArray names;
    if (audioProcessor.mameMachine != nullptr) {
        // Direct memshare read — fast and safe (no CPU bus contention).
        // The Motorola 68000 is big-endian; on a little-endian host, MAME's
        // 16-bit memshare swaps each byte pair. To read 68000 address A,
        // we access host offset (A ^ 1).
        auto* osram_share = audioProcessor.mameMachine->root_device().memshare("osram");
        if (osram_share != nullptr && osram_share->bytes() >= (0x0003C8 + 31800)) {
            uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
            
            uint8_t interleaved[31800];
            for (int i = 0; i < 31800; i++) {
                interleaved[i] = osram[(0x0003C8 + i) ^ 1];
            }
            
            uint8_t* linearData = nullptr;
            uintptr_t linearLen = 0;
            if (sd1_deinterleave_sixty_programs(interleaved, 31800, &linearData, &linearLen) == SD1_OK) {
                for (int p = 0; p < 60; ++p) {
                    char nameBuf[13] = {0};
                    sd1_program_name_from_slot(linearData + (p * 530), 530, nameBuf, sizeof(nameBuf));
                    names.add(decodeEnsoniqName(nameBuf));
                }
                sd1_bytes_free(linearData, linearLen);
                return names;
            }
        }
    }
    // Fallback if MAME is not ready
    for (int i=0; i<60; ++i) names.add("[EMPTY]");
    return names;
}

void PresetBrowserComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));
    g.setColour(juce::Colours::black);
    
    auto bounds = getLocalBounds().withTrimmedTop(30);
    int colWidth = bounds.getWidth() / 3;
    g.drawVerticalLine(colWidth, bounds.getY(), bounds.getBottom());
    g.drawVerticalLine(colWidth * 2, bounds.getY(), bounds.getBottom());

    // --- VFD DISPLAY & PROGRESS BACKGROUND ---
    juce::Rectangle<float> vfdBounds(colWidth + 10, 8, colWidth - 20, 34);
    
    g.setColour(juce::Colour(0xff0a0a0a));
    g.fillRoundedRectangle(vfdBounds, 4.0f);
    
    if (audioProcessor.mameMachine != nullptr && !progressBar.isVisible()) {
        juce::String vfd = audioProcessor.getHardwareVfdText();

        g.setFont(juce::Font(juce::FontOptions(13.0f)).boldened());
        g.setColour(juce::Colours::orange);

        auto topRow = vfdBounds.removeFromTop(17);
        auto bottomRow = vfdBounds;

#ifdef _WIN32
        // Windows (DirectWrite) 
        topRow = topRow.withSizeKeepingCentre(topRow.getWidth(), 24.0f);
        bottomRow = bottomRow.withSizeKeepingCentre(bottomRow.getWidth(), 24.0f);
#endif

        // Mac/Linux
        g.drawText(vfd.substring(0, 40), topRow, juce::Justification::centred, false);
        g.drawText(vfd.substring(40, 80), bottomRow, juce::Justification::centred, false);
    }
}

void PresetBrowserComponent::paintOverChildren(juce::Graphics& g)
{
    paintOverlay(g);

    // --- SD-1 BOOT INDICATOR ---
    if (isSystemBooting(audioProcessor))
    {
        double mameTime = audioProcessor.mameMachine ? audioProcessor.mameMachine->time().as_double() : 0.0;
        double bootEnd  = audioProcessor.scheduledCompareResetTime.load(std::memory_order_acquire);
        if (bootEnd <= 0.0) bootEnd = 3.5;

        float fraction = (bootEnd > 0.0) ? juce::jlimit(0.0f, 1.0f, (float)(mameTime / bootEnd)) : 0.0f;

        auto strip = getLocalBounds().removeFromTop(37).toFloat().reduced(4.0f, 3.0f).withY(getLocalBounds().getY() + 10.0f);
        g.setColour(juce::Colour(0xee1a0a00));
        g.fillRoundedRectangle(strip, 5.0f);
        g.setColour(juce::Colours::orange);
        g.drawRoundedRectangle(strip, 5.0f, 1.0f);
        
        int dots = (int)(mameTime * 2.0) % 4;
        juce::String dots_str;
        for (int i = 0; i < dots; ++i) dots_str += ".";
        juce::String msg = juce::String::fromUTF8("\xe2\x96\xba")
                           + "  SD-1 BOOTING" + dots_str
                           + "  Please wait...";

        g.setFont(juce::Font(juce::FontOptions(13.0f)).boldened());
        g.setColour(juce::Colours::orange);
        g.drawText(msg, strip.reduced(10.0f, 0.0f), juce::Justification::centredLeft, false);

        // Progress bar on the right side of the strip
        auto bar = strip.reduced(10.0f, 7.0f).withTrimmedLeft(260.0f);
        g.setColour(juce::Colour(0x33ffffff));
        g.fillRoundedRectangle(bar, 3.0f);
        g.setColour(juce::Colours::orange.withAlpha(0.9f));
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * fraction), 3.0f);
    }

}

void PresetBrowserComponent::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(10);
    
    auto headerArea = bounds.removeFromTop(35);
    closeButton.setBounds(headerArea.removeFromRight(160).reduced(0, 4).withTrimmedRight(15));
    headerLabel.setBounds(headerArea.removeFromLeft(280).reduced(10, 0));

    int colWidth = bounds.getWidth() / 3;

    // Progress bar aligned with the middle column
        {
            auto progressArea = juce::Rectangle<int>(colWidth + 10, 8, colWidth - 20, 34);
            progressArea.reduce(4, 2); 
            
            progressBar.setBounds(progressArea.removeFromTop(14));
            progressLabel.setBounds(progressArea);
        }

    auto leftColArea = bounds.removeFromLeft(colWidth).reduced(10);
    refreshButton.setBounds(leftColArea.removeFromTop(30).withTrimmedBottom(5));
    sourcesTree.setBounds(leftColArea);

    auto inspectorArea = bounds.removeFromRight(colWidth).reduced(10);
    inspectorGroup.setBounds(inspectorArea);
    
    auto inner = inspectorArea.reduced(15).withTrimmedTop(10);
    selectionNameLabel.setBounds(inner.removeFromTop(24));
    playHintLabel.setBounds(inner.removeFromTop(20));
    
    inner.removeFromTop(10); 
    
    auto btnRow1 = inner.removeFromTop(30);
    writeSingleButton.setBounds(btnRow1.removeFromLeft(btnRow1.getWidth() / 2).withTrimmedRight(2));
    importBankButton.setBounds(btnRow1.withTrimmedLeft(2));
    
    inner.removeFromTop(5);
    
    auto btnRow2 = inner.removeFromTop(30);
    backToFilesButton.setBounds(btnRow2.removeFromLeft(btnRow2.getWidth() / 2).withTrimmedRight(2));
    exportSysExButton.setBounds(btnRow2.withTrimmedLeft(2));
    
    inner.removeFromTop(10);
    bankContentList.setBounds(inner);
    
    contentList.setBounds(bounds.reduced(10));
}

void PresetBrowserComponent::showClosingProgress(const juce::String& message, double durationMs)
{
    closeButton.setEnabled(false);
    backToFilesButton.setEnabled(false);
    
    transferDurationMs = durationMs;
    transferStartTimeMs = juce::Time::getMillisecondCounterHiRes();
    progressValue = 0.0;
    progressBar.setVisible(true);
    progressLabel.setText(message, juce::dontSendNotification);
    progressLabel.setVisible(true);
    startTimerHz(30);
}

void PresetBrowserComponent::timerCallback()
{
    
        bool booting = isSystemBooting(audioProcessor);
        
        if (booting) {
            if (!wasBootingOverlay) {
                setInterceptsMouseClicks(true, false);
                wasBootingOverlay = true;
            }
            repaint();
            return; // Halt normal timer processing while booting
        } else if (wasBootingOverlay) {
            clickBlocker.reset();
            wasBootingOverlay = false;
            // MIDI ON FOR PREVIEW
            //audioProcessor.suppressMidiInput.store(false, std::memory_order_release);
            
            // Sync progress bars if an injection was queued during boot
            if (isSYXProcessing) {
                transferStartTimeMs = juce::Time::getMillisecondCounterHiRes();
            }
            repaint();
        }
    
    // --- Write Single Program state machine (VFD/LED-driven) ---
    if (wpState > 0) {
        runWriteProgramSM();
        repaint();
        return; // SM controls its own progress — skip normal progress bar logic
    }

    // --- Sequence transfer overlay ---
    if (isSequenceTransferActive) {
        double elapsed = juce::Time::getMillisecondCounterHiRes() - seqTransferStartTimeMs;
        if (elapsed >= seqTransferDurationMs) {
            isSequenceTransferActive = false;
            isSYXProcessing = false;
            wasSEQImport = true;
            clickBlocker.reset();  // Remove overlay — re-enables all child clicks
            audioProcessor.suppressMidiInput.store(false, std::memory_order_release);
            stopTimer();
            selectionNameLabel.setText("Sys-Ex data imported successfully!", juce::dontSendNotification);
        }
        repaint(); // Redraw overlay progress
        return;    // Don't process normal progress bar during sequence transfer
    }
    
    // --- Normal progress bar (SysEx preview, write preset, etc.) ---
    double elapsed = juce::Time::getMillisecondCounterHiRes() - transferStartTimeMs;
    double ratio = elapsed / transferDurationMs;
    
    if (ratio >= 1.0) {
            progressValue = -1.0;
            isSYXProcessing = false;
            progressBar.setVisible(false);
            progressLabel.setVisible(false);
            closeButton.setEnabled(true);
            stopTimer();
            
            // MIDI ON FOR PREVIEW
            audioProcessor.suppressMidiInput.store(false, std::memory_order_release);
        } else {
            progressValue = ratio;
        }
}

// =========================================================================================
// SYSEX VIRTUAL BUTTON PRESS
// SD-1 SysEx Spec Section 3.1.1: Message Type 0x00, Command Type 0x00
// Button 63 = Compare. Down = buttonNum, Up = buttonNum + 96.
// Delay of 200-300ms between down/up is recommended by the spec.
// =========================================================================================

void PresetBrowserComponent::pushSysExButtonPress(int buttonNum, double downTime, double upTime)
{
    if (audioProcessor.mameMachine == nullptr) return;
    
    // Button DOWN: F0 0F 05 00 00 00 00 00 [hi lo] F7
    uint8_t downMsg[11] = {
        0xF0, 0x0F, 0x05, 0x00, 0x00, 0x00,  // header (ch=0, msgType=0x00)
        0x00, 0x00,                          // command type 0x00 nibblized
        static_cast<uint8_t>((buttonNum >> 4) & 0x0F),
        static_cast<uint8_t>(buttonNum & 0x0F),
        0xF7
    };
    for (int i = 0; i < 11; ++i)
        audioProcessor.pushMidiByte(downMsg[i], downTime + i * 0.00035);
    
    // Button UP: buttonNum + 96
    int upNum = buttonNum + 96;
    uint8_t upMsg[11] = {
        0xF0, 0x0F, 0x05, 0x00, 0x00, 0x00,
        0x00, 0x00,
        static_cast<uint8_t>((upNum >> 4) & 0x0F),
        static_cast<uint8_t>(upNum & 0x0F),
        0xF7
    };
    for (int i = 0; i < 11; ++i)
        audioProcessor.pushMidiByte(upMsg[i], upTime + i * 0.00035);
}

void PresetBrowserComponent::setCompareState(bool shouldBeOn)
{
    if (isRestoringUI || audioProcessor.mameMachine == nullptr) return;
    
    // --- 1. GET TRUE COMPARE STATE FROM OSRAM ---
    auto* osram_share = audioProcessor.mameMachine->root_device().memshare("osram");
    if (osram_share == nullptr || osram_share->bytes() <= 0x92DC) return;
    
    uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
    bool currentlyOn = (osram[0x92DC] != 0x00);
    double now = getSafeBootTime(audioProcessor);

    // --- 2. GET TRUE UI STATE FROM MAME OUTPUTS ---
    juce::String vfdText = audioProcessor.getHardwareVfdText();
    bool hasError = vfdText.containsIgnoreCase("ERROR");
    bool isWriting = vfdText.containsIgnoreCase("WRITE EDIT PROGRAM");

    if (shouldBeOn) {
        if (!currentlyOn) {
                    if (hasError) {
                        pushSysExButtonPress(19, now + 0.05, now + 0.20); // EXIT (150ms hold)
                        now += 0.50; // 0.05 base + 0.15 hold + 0.30 gap
                    }
                    pushSysExButtonPress(63, now + 0.05, now + 0.20); // COMPARE ON
                }
    } else {
        if (currentlyOn) {
                    if (isWriting || hasError) {
                        pushSysExButtonPress(19, now + 0.05, now + 0.20); // EXIT
                        now += 0.50;
                    }
                    pushSysExButtonPress(63, now + 0.05, now + 0.20); // COMPARE OFF
                } else if (hasError) {
                    pushSysExButtonPress(19, now + 0.05, now + 0.20); // EXIT
                }
    }
}

void PresetBrowserComponent::returnToInternalMode(int bank, int program)
{
    if (audioProcessor.mameMachine == nullptr) return;
    if (!wasSYXPreview) return;
    
    audioProcessor.clearMidiBuffer();
    
    double now = getSafeBootTime(audioProcessor);

    double safeTime = now + 0.3;
    
    // 2. MIDI panic
    audioProcessor.pushMidiByte(0xB0, safeTime + 0.05);
    audioProcessor.pushMidiByte(120, safeTime + 0.05);  // All Sound Off
    audioProcessor.pushMidiByte(0, safeTime + 0.05);
    audioProcessor.pushMidiByte(0xB0, safeTime + 0.06);
    audioProcessor.pushMidiByte(123, safeTime + 0.06);  // All Notes Off
    audioProcessor.pushMidiByte(0, safeTime + 0.06);
    
    // 3. CHECK COMPARE STATE
    bool isCompareOn = false;
        auto* osram_share = audioProcessor.mameMachine->root_device().memshare("osram");
        if (osram_share != nullptr && osram_share->bytes() > 0x92DC) {
            uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
            isCompareOn = (osram[0x92DC] != 0x00);
        }

        // CHECK FLAGS
        if (isCompareOn || isErrorState) {
            pushSysExButtonPress(19, safeTime + 0.05, safeTime + 0.20); // EXIT (150ms hold)
            safeTime += 0.50; // 300ms gap after UP
            
            if (isCompareOn) {
                pushSysExButtonPress(63, safeTime, safeTime + 0.15); // COMPARE OFF
                safeTime += 0.45; // 150ms hold + 300ms gap
            }
            
            wasSYXPreview = false;
            isErrorState = false;
        }

    // 4. Bank Select + Program Change (after write mode exit completes)
            if (!isRestoringUI) {
                double time = safeTime;
                
                juce::String vfd = audioProcessor.getHardwareVfdText();
                juce::String vfdStart = vfd.substring(0, 3);
                int currentBankSet = 0;
                if (vfdStart == "RM0") currentBankSet = 1;
                else if (vfdStart == "RM1") currentBankSet = 2;
                else if (vfdStart == "CRT") currentBankSet = 3;

                int targetBankSet = 0;
                if (bank == 126) targetBankSet = 1;
                else if (bank == 123) targetBankSet = 2;
                else if (bank == 125 && program >= 60) targetBankSet = 3; // CART

                bool hasCart = false;
                if (auto* cart = audioProcessor.mameMachine->root_device().subdevice<ensoniq_vfx_cartridge>("cart")) {
                    hasCart = cart->exists();
                }
                int numBanks = hasCart ? 4 : 3;
                if (targetBankSet >= numBanks) targetBankSet = 0;

                pushSysExButtonPress(11, time, time + 0.10); // SOUNDS
                time += 0.40;

                if (currentBankSet != targetBankSet) {
                    int presses = (targetBankSet - currentBankSet + numBanks) % numBanks;
                    for (int i = 0; i < presses; ++i) {
                        pushSysExButtonPress(10, time, time + 0.10); // BANKSET
                        time += 0.40;
                    }
                }
                
                // Actual Program Change (0-59 or 60-119)
                audioProcessor.pushMidiByte(0xC0, time);
                audioProcessor.pushMidiByte(program, time + 0.00035);
            }
    
    // 5. Progress bar
    transferDurationMs = (isCompareOn ? 1000.0 : 400.0);
        transferStartTimeMs = juce::Time::getMillisecondCounterHiRes();
        progressValue = 0.0;
        progressBar.setVisible(true);
        progressLabel.setText("Returning to internal mode...", juce::dontSendNotification);
        progressLabel.setVisible(true);
        startTimerHz(30);
}

void PresetBrowserComponent::updateContentList(const juce::String& categoryName)
{
    currentCategory = categoryName;
    currentListItems.clear();
    
    // UX: For INT/ROM/CART, the Write button is inactive, and the Back button is inactive
    if (categoryName == "INT (RAM)" || categoryName.startsWith("ROM") || categoryName == "CART") {
        setCompareState(false);
        writeSingleButton.setEnabled(false);
        exportSysExButton.setEnabled(!categoryName.startsWith("ROM"));
    } else {
        writeSingleButton.setEnabled(false);
        exportSysExButton.setEnabled(true);
    }
        
    selectionNameLabel.setText("No selection", juce::dontSendNotification);
    playHintLabel.setVisible(false);
    importBankButton.setButtonText("Import Full Bank");  // Reset from "Import Sequencer Data"

    if (categoryName == "INT (RAM)") {
        juce::StringArray names = getRealIntRamNames();
        for (int i = 0; i < 60; ++i) {
            currentListItems.add(juce::String::formatted("%02d: ", i) + names[i]);
        }
    }
    else if (categoryName == "ROM0") {
        const char* const* romNames = sd1_rom_all_programs();
        for (int i = 0; i < 60; ++i) {
            juce::String n = (romNames && romNames[i] && juce::String(romNames[i]).isNotEmpty()) ? juce::String(romNames[i]) : "<UNKNOWN>";
            currentListItems.add(juce::String::formatted("%02d: ", i) + n);
        }
    }
    else if (categoryName == "ROM1") {
        const char* const* romNames = sd1_rom_all_programs();
        for (int i = 60; i < 120; ++i) {
            juce::String n = (romNames && romNames[i] && juce::String(romNames[i]).isNotEmpty()) ? juce::String(romNames[i]) : "<UNKNOWN>";
            currentListItems.add(juce::String::formatted("%02d: ", i - 60) + n);
        }
    }
    else if (categoryName == "CART") {
        if (audioProcessor.mameMachine == nullptr) {
            currentListItems.add("(MAME engine not running)");
            // Disable export and import if MAME is not running
                        exportSysExButton.setEnabled(false);
                        importBankButton.setEnabled(false);
        } else {
            auto* cart = audioProcessor.mameMachine->root_device().subdevice<ensoniq_vfx_cartridge>("cart");
            if (cart == nullptr || !cart->exists()) {
                currentListItems.add("No cartridge loaded.");
                currentListItems.add("");
                currentListItems.add("To load a cartridge:");
                currentListItems.add("- attach via the PRESET/FILE MANAGER menu");
                currentListItems.add("- or browse your folders for cartridge files.");
                // Disable export and import if no cartridge is loaded
                                exportSysExButton.setEnabled(false);
                                importBankButton.setEnabled(false);
            } else {
                auto* eeprom = audioProcessor.mameMachine->root_device().subdevice<eeprom_parallel_base_device>("cart:eeprom");
                if (eeprom != nullptr) {
                    std::vector<uint8_t> storage(0x8000);
                    for (int i = 0; i < 0x8000; i++) {
                        storage[i] = eeprom->read(i);
                    }
                    
                    // Validate cartridge signature at 0x7FFE-0x7FFF
                    if (storage.size() >= 0x8000 && storage[0x7FFE] == 0x05 && storage[0x7FFF] == 0x01) {
                        // Cartridge stores programs SEQUENTIALLY (not interleaved like INT RAM!)
                        // Each program is 530 bytes: program 0 at [0..529], program 1 at [530..1059], etc.
                        // Program name is at byte offset 498 within each 530-byte block (11 chars).
                        for (int p = 0; p < 60; ++p) {
                            size_t nameOffset = (p * 530) + 498;
                            if (nameOffset + 11 <= storage.size()) {
                                char nameBuf[12] = {0};
                                std::memcpy(nameBuf, &storage[nameOffset], 11);
                                currentListItems.add(juce::String::formatted("%02d: ", p) + decodeEnsoniqName(nameBuf));
                            } else {
                                currentListItems.add(juce::String::formatted("%02d: [EMPTY]", p));
                            }
                        }
                    } else {
                        currentListItems.add("(Cartridge is empty or unformatted)");
                        // Disable export and import if cartridge is invalid
                                                exportSysExButton.setEnabled(false);
                                                importBankButton.setEnabled(false);
                    }
                } else {
                    currentListItems.add("(EEPROM device not found)");
                    // Disable export and import if EEPROM is missing
                                        exportSysExButton.setEnabled(false);
                                        importBankButton.setEnabled(false);
                }
            }
        }
    }
    else if (categoryName == "Documents/EnsoniqSD1") {
        juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1");
        if (docsDir.exists() && docsDir.isDirectory()) {
            juce::Array<juce::File> files = docsDir.findChildFiles(juce::File::findFiles, false, "*.syx;*.img;*.hfe;*.dsk;*.eda;*.eeprom;*.rom;*.cart;*.sc32");
            files.sort();
            for (const auto& file : files) currentListItems.add(file.getFileName());
        }
    }
    else if (categoryName == "Downloads") {
        juce::File downDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Downloads");
        if (downDir.exists() && downDir.isDirectory()) {
            juce::Array<juce::File> files = downDir.findChildFiles(juce::File::findFiles, false, "*.syx;*.img;*.hfe;*.dsk;*.eda;*.eeprom;*.rom;*.cart;*.sc32");
            files.sort();
            for (const auto& file : files) currentListItems.add(file.getFileName());
        }
    }
    else if (categoryName == "Add Folder...") {
        sourcesTree.clearSelectedItems();
        folderChooser = std::make_unique<juce::FileChooser>("Select Folder with SD-1 files",
                                                            juce::File::getSpecialLocation(juce::File::userHomeDirectory), "");
        auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        folderChooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
            auto folder = fc.getResult();
            if (folder.exists() && folder.isDirectory()) {
                juce::String path = folder.getFullPathName();
                if (!audioProcessor.bookmarkFolders.contains(path)) {
                    if (audioProcessor.bookmarkFolders.size() >= 10)
                        audioProcessor.bookmarkFolders.remove(0);
                    audioProcessor.bookmarkFolders.add(path);
                    if (auto* editor = dynamic_cast<EnsoniqSD1AudioProcessorEditor*>(getParentComponent()))
                        editor->saveGlobalSettings();
                    rebuildTree();
                }
                updateContentList("BOOKMARK:" + path);
            }
        });
        return;
    }
    else if (categoryName.startsWith("BOOKMARK:")) {
        juce::String folderPath = categoryName.substring(9);
        juce::File folder(folderPath);
        customBrowseFolder = folder;
        if (folder.exists() && folder.isDirectory()) {
            juce::Array<juce::File> files = folder.findChildFiles(juce::File::findFiles, false, "*.syx;*.img;*.hfe;*.dsk;*.eda;*.eeprom;*.rom;*.cart;*.sc32");
            files.sort();
            for (const auto& f : files) currentListItems.add(f.getFileName());
        }
    }
    
    contentList.deselectAllRows();
    contentList.updateContent();
    contentList.repaint();
    selectionNameLabel.setText("No selection", juce::dontSendNotification);
    clearBankList();
    bankContentList.updateContent();
    bankContentList.repaint();
    
    // The blue "Back" button is disabled when the list is being refreshed
    isViewingDiskBank = false;
    backToFilesButton.setEnabled(false);
}

void PresetBrowserComponent::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (rowNumber >= currentListItems.size()) return;

    if (rowIsSelected) {
        g.fillAll(juce::Colour(0xff446688)); 
    }

    g.setColour(rowIsSelected ? juce::Colours::white : juce::Colours::lightgrey);
    g.setFont(juce::FontOptions(18.0f));
    g.drawText(currentListItems[rowNumber], 10, 0, width - 15, height, juce::Justification::centredLeft, true);
}

void PresetBrowserComponent::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (isSequenceTransferActive) return;
        if (row >= currentListItems.size()) return;
        
        if (contentList.getNumSelectedRows() > 1) {
            bool isInternalBank = (currentCategory == "INT (RAM)" || currentCategory == "CART");
            int numSel = contentList.getNumSelectedRows();
            
            bool canExport = false;
            juce::String statusText = juce::String(numSel) + " items selected";
            
            if (isInternalBank) {
                canExport = true;
                statusText = juce::String(numSel) + " programs selected";
            } else {
                // EXTERNAL FOLDER MULTI-SELECT VALIDATION
                auto selectedRows = contentList.getSelectedRows();
                bool allSameType = true;
                bool hasValidSyx = false;
                int64_t referenceSize = 0;
                
                for (int i = 0; i < selectedRows.size(); ++i) {
                    int r = selectedRows[i];
                    if (r < 0 || r >= currentListItems.size()) continue;
                    juce::String fileName = currentListItems[r];
                                
                    if (!fileName.endsWithIgnoreCase(".syx")) {
                        allSameType = false;
                        break;
                    }
                                
                    juce::File fileToRead = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getChildFile(fileName);
                    if (!fileToRead.existsAsFile()) fileToRead = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Downloads").getChildFile(fileName);
                    if (!fileToRead.existsAsFile() && customBrowseFolder.exists()) fileToRead = customBrowseFolder.getChildFile(fileName);
                    
                    if (fileToRead.existsAsFile()) {
                        int64_t fSize = fileToRead.getSize();
                        // Program (1067-63607) or Preset (103-1927)
                        int64_t categorySize = 0;
                        if (fSize >= 1067 && (fSize - 7) % 1060 == 0) categorySize = 1067; // Program or ProgBank
                        else if (fSize >= 103 && (fSize - 7) % 96 == 0) categorySize = 103; // Preset or PresetBank
                        
                        if (categorySize == 0) {
                            allSameType = false;
                            break;
                        }
                        
                        if (!hasValidSyx) {
                            referenceSize = categorySize;
                            hasValidSyx = true;
                        } else if (categorySize != referenceSize) {
                            allSameType = false;
                            break;
                        }
                    }
                }
                
                if (hasValidSyx && allSameType) {
                    canExport = true;
                    statusText = juce::String(numSel) + (referenceSize == 1067 ? " programs selected" : " presets selected");
                } else {
                    statusText = "Mixed or invalid types selected - Export disabled";
                }
            }
            
            selectionNameLabel.setText(statusText, juce::dontSendNotification);
            writeSingleButton.setEnabled(false);
            importBankButton.setEnabled(false);
            exportSysExButton.setEnabled(canExport);
            playHintLabel.setVisible(false);
            clearBankList();
            bankContentList.updateContent();
            return;
        }
    
    juce::String selectedName = currentListItems[row];
    juce::String ext = selectedName.substring(selectedName.lastIndexOfChar('.')).toLowerCase();
    
    bool isExternal = (ext == ".syx" || ext == ".img" || ext == ".hfe" || ext == ".dsk" || ext == ".eda"
                           || ext == ".eeprom" || ext == ".rom" || ext == ".cart" || ext == ".sc32");
        
        if (isExternal) {
        // SPAM Protection
        if (ext == ".syx" && progressValue >= 0.0) return;
        importBankButton.setButtonText("Import Full Bank");
        
        juce::File fileToRead = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                          .getChildFile("EnsoniqSD1").getChildFile(selectedName);
                
        if (!fileToRead.existsAsFile()) {
            fileToRead = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                           .getChildFile("Downloads").getChildFile(selectedName);
        }
        if (!fileToRead.existsAsFile() && customBrowseFolder.exists()) {
            fileToRead = customBrowseFolder.getChildFile(selectedName);
        }

        if (fileToRead.existsAsFile()) {
            currentlyLoadedArchive = fileToRead;
            clearBankList();
            exportSysExButton.setEnabled(true);
            
            isViewingDiskBank = false; 
            backToFilesButton.setEnabled(false);
            
            if (ext == ".syx") {
                juce::MemoryBlock syxData;
                fileToRead.loadFileAsData(syxData);
                const uint8_t* data = static_cast<const uint8_t*>(syxData.getData());
                size_t size = syxData.getSize();
                
                // --- DETECT SEQUENCE SYX ---
                // Files may contain multiple SysEx messages (combined dumps: programs + presets + sequences).
                // Scan ALL messages to find sequence Command+Data or bare Data.
                // msgTypes: 0x0A=AllSeqDump, 0x0B=OneSeqDump, 0x09=OneSeqDump-alt, 0x03=AllPrograms
                bool isSequenceSyx = false;
                bool isCommandOnly = false;
                juce::String seqLabel;
                size_t seqDataOffset = 0;
                size_t seqDataSize = 0;
                combinedDumpProgramSyx.setSize(0); // clear any previous combined data

                {
                    size_t pos = 0;
                    size_t cmdOffset = 0, cmdSize = 0;
                    while (pos + 6 < size) {
                        if (data[pos] != 0xF0) { ++pos; continue; }
                        size_t end = pos + 1;
                        while (end < size && data[end] != 0xF7) ++end;
                        if (end >= size) break;
                        size_t msgLen = end - pos + 1;
                        uint8_t mt = (pos + 5 < size) ? data[pos + 5] : 0;
                        uint8_t cmd7 = (pos + 7 < size) ? data[pos + 7] : 0;

                        if (mt == 0x03) {
                            // AllPrograms — save for combined import
                            combinedDumpProgramSyx.setSize(0);
                            combinedDumpProgramSyx.append(data + pos, msgLen);
                        } else if (mt == 0x00 && (cmd7 == 0x0C || cmd7 == 0x0B) && msgLen == 17) {
                            cmdOffset = pos; cmdSize = msgLen;
                        } else if (mt == 0x0A || mt == 0x0B || mt == 0x09) {
                            isSequenceSyx = true;
                            seqLabel = (mt == 0x0A) ? "All Sequences" : "Single Sequence";
                            if (cmdSize > 0 && cmdOffset < pos) {
                                seqDataOffset = cmdOffset;
                                seqDataSize = (pos + msgLen) - cmdOffset;
                            } else {
                                seqDataOffset = pos;
                                seqDataSize = msgLen;
                            }
                            break;
                        }
                        pos = end + 1;
                    }
                    if (!isSequenceSyx && cmdSize > 0 && cmdSize == size)
                        isCommandOnly = true;
                }
                
                bool isPresetSyx = false;
                    if (!isSequenceSyx && size == 199 && data[0] == 0xF0 && data[5] == 0x04) {
                        isPresetSyx = true;
                    }
                
                if (isSequenceSyx) {
                    
                    setCompareState(false);
                    bool isCombined = (combinedDumpProgramSyx.getSize() > 0);
                    juce::String label = isCombined ? ("Combined: 60 Programs + " + seqLabel) : seqLabel;
                    selectionNameLabel.setText(label + ": " + fileToRead.getFileName(), juce::dontSendNotification);
                    writeSingleButton.setEnabled(false);
                    importBankButton.setEnabled(true);
                    importBankButton.setButtonText("Import Sequencer Data");
                    exportSysExButton.setEnabled(false);
                    playHintLabel.setVisible(false);
                    
                    juce::MemoryBlock seqData(data + seqDataOffset, seqDataSize);
                    currentlyLoadedBankData = seqData;
                    
                    double transferSec = seqData.getSize() / 3125.0;
                    if (isCombined) {
                        bankModel.items.add("Combined dump: 60 programs + " + seqLabel);
                        bankModel.items.add("Clicking Import will:");
                        bankModel.items.add("  1. Import 60 programs into RAM (with confirmation)");
                        bankModel.items.add("  2. Import " + seqLabel + " data (~" + juce::String((int)std::ceil(transferSec)) + "s)");
                    } else {
                        bankModel.items.add(seqLabel + " SYX: " + juce::String((int)seqData.getSize()) + " bytes");
                        bankModel.items.add("Estimated transfer time: ~" + juce::String((int)std::ceil(transferSec)) + " seconds");
                    }
                    bankModel.items.add("");
                    bankModel.items.add("Click 'Import Sequencer Data' to send to SD-1.");
                    bankContentList.updateContent();
                    return;

                } else if (isCommandOnly) {
                    selectionNameLabel.setText("Command-only SYX — load the Data file instead", juce::dontSendNotification);
                    bankModel.items.add("This is a Command-only packet (17 bytes).");
                    bankModel.items.add("Load the accompanying Data file (allseq-2.syx etc.)");
                    bankModel.items.add("which contains the actual sequence data.");
                    bankContentList.updateContent();
                    return;
                    
                } else if (isPresetSyx) {
                    
                    // --- PRESET SYX ---
                    selectionNameLabel.setText("Preset: " + fileToRead.getFileNameWithoutExtension(), juce::dontSendNotification);
                    writeSingleButton.setEnabled(false);
                    importBankButton.setEnabled(true);
                    importBankButton.setButtonText("Import Preset");
                    exportSysExButton.setEnabled(true);
                    playHintLabel.setVisible(false);
                                        
                    currentlySelectedSingleRawData.setSize(48);
                    uint8_t* rawDst = static_cast<uint8_t*>(currentlySelectedSingleRawData.getData());
                                        
                    for (int i = 0; i < 48; ++i) {
                         uint8_t hi = data[6 + (i * 2)];
                         uint8_t lo = data[6 + (i * 2) + 1];
                         rawDst[i] = static_cast<uint8_t>((hi << 4) | lo);
                    }
                                        
                    setCompareState(false);
                    wasSYXPreview = true;
                    injectAndPreview();
                    
                } else {
                    
                    // --- PROGRAM OR BANK SYX ---
                    juce::StringArray extracted = extractNamesFromSysEx(syxData);
                    if (!extracted.isEmpty()) {
                        if (extracted.size() == 1) {
                            // SINGLE SYX
                                            selectionNameLabel.setText("Single: " + extracted[0], juce::dontSendNotification);
                                            writeSingleButton.setEnabled(true);
                                            importBankButton.setEnabled(false);
                                            
                                            size_t payloadSize = (syxData.getSize() - 7) / 2;
                                            currentlySelectedSingleRawData.setSize(payloadSize);
                                            uint8_t* rawDst = static_cast<uint8_t*>(currentlySelectedSingleRawData.getData());
                                            const uint8_t* data = static_cast<const uint8_t*>(syxData.getData());
                                            
                                            for (size_t i = 0; i < payloadSize; ++i) {
                                                uint8_t hi = data[6 + (i * 2)];
                                                uint8_t lo = data[6 + (i * 2) + 1];
                                                rawDst[i] = static_cast<uint8_t>((hi << 4) | lo);
                                            }
                                            setCompareState(true);
                                            injectAndPreview();

                                        } else {
                                            
                                            // BANK SYX (Dynamic size)
                                            selectionNameLabel.setText("Bank (" + juce::String(extracted.size()) + " items)", juce::dontSendNotification);
                                            writeSingleButton.setEnabled(false);
                                            importBankButton.setEnabled(true);
                                            setCompareState(false);
                                            
                                            size_t payloadSize = (syxData.getSize() - 7) / 2;
                                            size_t itemSize = payloadSize / extracted.size();
                                            if (itemSize == 48) {
                                                    importBankButton.setButtonText("Import All Presets");
                                                } else {
                                                    importBankButton.setButtonText("Import Full Bank");
                                            }
                                            
                                            currentlyLoadedBankData.setSize(payloadSize);
                                            uint8_t* linearData = static_cast<uint8_t*>(currentlyLoadedBankData.getData());
                                            const uint8_t* data = static_cast<const uint8_t*>(syxData.getData());

                                            for (int p = 0; p < extracted.size(); ++p) {
                                                size_t srcOffset = 6 + (p * itemSize * 2);
                                                size_t dstOffset = p * itemSize;
                                                for (size_t i = 0; i < itemSize; ++i) {
                                                    uint8_t hi = data[srcOffset + (i * 2)];
                                                    uint8_t lo = data[srcOffset + (i * 2) + 1];
                                                    linearData[dstOffset + i] = static_cast<uint8_t>((hi << 4) | lo);
                                                }
                                                bankModel.items.add(extracted[p]);
                                            }
                                            playHintLabel.setVisible(false);
                                        }
                    } else {
                    selectionNameLabel.setText("Invalid or empty .SYX", juce::dontSendNotification);
                    playHintLabel.setVisible(false);
                }
            }
            }
            else if (ext == ".eeprom" || ext == ".rom" || ext == ".cart" || ext == ".sc32") {
                // CARTRIDGE FILE (32KB EEPROM: 60 sequential programs × 530 bytes)
                setCompareState(false);
                
                juce::MemoryBlock eepromData;
                fileToRead.loadFileAsData(eepromData);
                
                if (eepromData.getSize() == 32768) {
                    const uint8_t* storage = static_cast<const uint8_t*>(eepromData.getData());
                    
                    // Validate cartridge signature at 0x7FFE-0x7FFF
                    if (storage[0x7FFE] == 0x05 && storage[0x7FFF] == 0x01) {
                        selectionNameLabel.setText("Cartridge: " + fileToRead.getFileName(), juce::dontSendNotification);
                        
                        // Store as linear bank data (sequential, no deinterleave needed)
                        currentlyLoadedBankData.setSize(0);
                        currentlyLoadedBankData.append(storage, 31800);
                        
                        // List 60 programs (sequential 530-byte blocks, name at offset 498)
                        for (int p = 0; p < 60; ++p) {
                            size_t nameOffset = (p * 530) + 498;
                            char nameBuf[12] = {0};
                            std::memcpy(nameBuf, storage + nameOffset, 11);
                            bankModel.items.add(juce::String::formatted("%02d: ", p) + decodeEnsoniqName(nameBuf));
                        }
                        
                        writeSingleButton.setEnabled(false);
                        importBankButton.setEnabled(true);
                        playHintLabel.setVisible(false);
                    } else {
                        selectionNameLabel.setText("Invalid cartridge file (bad signature)", juce::dontSendNotification);
                    }
                } else {
                    selectionNameLabel.setText("Invalid cartridge file (must be 32KB)", juce::dontSendNotification);
                }
            }
            else {
                // DISK IMAGE (IMG / HFE)
                setCompareState(false);
                int32_t err = 0;
                DiskImage* img = nullptr;
                juce::String path = fileToRead.getFullPathName();
                std::string stdPath = path.toStdString();
                const char* cPath = stdPath.c_str();
                if (ext == ".hfe") img = sd1_read_hfe(cPath, &err);
                else               img = sd1_disk_open(cPath, &err);

                if (img) {
                    selectionNameLabel.setText("Disk Image: " + fileToRead.getFileName(), juce::dontSendNotification);
                    
                    // List all 4 subdirectories (stock disks may use subdir 3, not 0)
                    for (uint8_t subdir = 0; subdir < 4; ++subdir) {
                        uintptr_t fileCount = 0;
                        Sd1DirectoryEntry* entries = sd1_disk_list(img, subdir, &fileCount);
                        if (entries && fileCount > 0) {
                            for (uintptr_t i = 0; i < fileCount; ++i) {
                                char nameBuf[12] = {0};
                                memcpy(nameBuf, entries[i].name, 11);
                                nameBuf[11] = '\0';
                                
                                juce::String fatName(nameBuf);
                                bankRawNames.add(fatName);
                                bankEntryTypes.push_back(entries[i].file_type);
                                
                                // Store the exact 11-byte name for the FAT engine
                                std::array<char, 11> exact;
                                std::memcpy(exact.data(), entries[i].name, 11);
                                bankExactNames.push_back(exact);
                                
                                juce::String itemName = fatName + " (Type: " + juce::String((int)entries[i].file_type) + ")";
                                bankModel.items.add(itemName);
                            }
                            sd1_entries_free(entries, fileCount);
                        }
                    }
                    
                    if (bankModel.items.isEmpty()) {
                        bankModel.items.add("(Disk is empty or unsupported directory)");
                    }
                    sd1_disk_free(img);
                    
                    writeSingleButton.setEnabled(false);
                    importBankButton.setEnabled(false);
                    playHintLabel.setVisible(false);
                }
            }
            bankContentList.updateContent();
            bankContentList.repaint();
        }
    }
    else {
        
        // --- INTERNAL PRESET PREVIEW (INT / ROM0 / ROM1 / CART) ---
        
        // Skip placeholder messages like "(No cartridge loaded)"
        if (selectedName.startsWith("(")) return;
        if (progressValue >= 0.0) return;
        
        juce::String cleanName = selectedName.substring(4).trim();
        selectionNameLabel.setText(cleanName, juce::dontSendNotification);
                
        writeSingleButton.setEnabled(false); // UX: INT/ROM/CART
        importBankButton.setEnabled(false);
        playHintLabel.setVisible(true);
        
        // --- ENSONIQ BANK SELECT — Pure MIDI PC Implementation ---
        // Manual numbers programs 1-128, MIDI transmits 0-127 → subtract 1 from all manual values.
        //
        // Bank select PCs (must be sent first, set context for subsequent PCs):
        //   Manual PC 126 → MIDI 125 : INT bank  → programs 0-59=INT, 60-119=CART
        //   Manual PC 127 → MIDI 126 : ROM0 bank → programs 0-59=ROM0, 60-119=CART
        //   Manual PC 124 → MIDI 123 : ROM1 bank → programs 0-59=ROM1, 60-119=CART
        //
        // CART: no separate bank select needed — always 60-119 in any bank context.

        int bankSelectPC = 125;   // default: INT
        int programPC    = row;   // 0-based

        if      (currentCategory == "ROM0") { bankSelectPC = 126; }
        else if (currentCategory == "ROM1") { bankSelectPC = 123; }
        else if (currentCategory == "CART") { bankSelectPC = 125; programPC = row + 60; }
        // INT (RAM): bankSelectPC = 125, programPC = row  (already set)

        if (!isRestoringUI && audioProcessor.mameMachine != nullptr) {
            audioProcessor.clearMidiBuffer();
            double t = getSafeBootTime(audioProcessor) + 0.05;

            // --- Read current panel state ---
            bool compareOn = false;
            if (auto* sh = audioProcessor.mameMachine->root_device().memshare("osram")) {
                auto* osram = static_cast<uint8_t*>(sh->ptr());
                if (sh->bytes() > 0x92DC) compareOn = (osram[0x92DC] != 0x00);
            }
            juce::String vfd   = audioProcessor.getHardwareVfdText();
            bool inWriteMode   = vfd.containsIgnoreCase("WRITE EDIT");
            bool inErrorMode   = vfd.containsIgnoreCase("ERROR");
            bool inPresetsMode = audioProcessor.isHardwareLedOn(7); // PRESETS LED bit 7

            // --- Step 1: EXIT from write/error mode (SysEx button required — PC ignored in write mode) ---
            // 300 ms between each SysEx command per spec
            if (wasSYXPreview || inWriteMode || inErrorMode || isErrorState) {
                pushSysExButtonPress(19, t, t + 0.025); // EXIT
                t += 0.300;
            }
            if (compareOn) {
                pushSysExButtonPress(63, t, t + 0.025); // COMPARE OFF
                t += 0.300;
            }

            // --- Step 2: Switch to SOUNDS mode if in PRESETS (PC bank select works in SOUNDS only) ---
            if (inPresetsMode || wasPresetPreviewed) {
                pushSysExButtonPress(11, t, t + 0.025); // SOUNDS
                t += 0.300;
            }

            // --- Step 3: Bank select PC (sets INT/ROM0/ROM1 context, ALSO switches to SOUNDS mode) ---
            // 300 ms delay before program PC per spec
            audioProcessor.pushMidiByte(0xC0, t);
            audioProcessor.pushMidiByte(bankSelectPC, t + 0.00035);
            t += 0.300;

            // --- Step 4: Program select PC ---
            audioProcessor.pushMidiByte(0xC0, t);
            audioProcessor.pushMidiByte(programPC, t + 0.00035);

            wasPresetPreviewed = false;
            wasSYXPreview      = false;
            isErrorState       = false;
            lastBankContext    = currentCategory;
        }
                        
                clearBankList();
        bankContentList.updateContent();
    }
}

// =========================================================================================
// CLEAR BANK LIST HELPER — keeps items, rawNames, entryTypes in sync
// =========================================================================================

void PresetBrowserComponent::clearBankList()
{
    bankModel.items.clear();
    bankRawNames.clear();
    bankEntryTypes.clear();
    bankExactNames.clear();
    bankContentList.deselectAllRows();
    bankContentList.updateContent();  // Force JUCE ListBox to see 0 rows — prevents stale selection restore
}

// =========================================================================================
// SYX INJECT
// =========================================================================================

// =========================================================================================
// SEQUENCE TRANSFER (sends raw SYX bytes via MIDI at 31250 baud)
// =========================================================================================

void PresetBrowserComponent::startSequenceTransfer(const juce::MemoryBlock& rawSyxData, const juce::String& message)
{
    if (audioProcessor.mameMachine == nullptr) return;
    if (rawSyxData.getSize() == 0) return;

    // Exit any active SYX preview state before starting sequence transfer.
    // Only needed if we previously injected a program (wasSYXPreview=true),
    // which may have left the panel in Compare or Write mode.
    if (wasSYXPreview) {
        double exitNow = audioProcessor.mameMachine->time().as_double();
        pushSysExButtonPress(19, exitNow + 0.05, exitNow + 0.25);
    }
    setCompareState(false);

    // -----------------------------------------------------------------------
    // Build transfer buffer — auto-prepend Command (0x0C) if the file is
    // AllSeqDump-only (starts with msgType 0x0A, no 0x0C command before it).
    //
    // SD-1 AllSeqDump structure:
    //   [0:240)            60×4-byte pointer table (fixed)
    //   [240:240+declared) sequence event data (variable)
    //   [240+declared:end) headers + globals (always 11189 bytes)
    //
    // Correct declared_size = total_payload - 11429
    // Library bug: sets declared = total_payload. We patch it here.
    // -----------------------------------------------------------------------
    juce::MemoryBlock transferBuf;
    const uint8_t* srcData = static_cast<const uint8_t*>(rawSyxData.getData());
    size_t srcSize = rawSyxData.getSize();

    // Detect sequence-only files (no preceding Command packet) by msgType:
    //   0x0A = AllSeqDump     → Command type 0x0C, declared = totalPayload - 11429
    //   0x0B = OneSeqDump     → Command type 0x0B, declared = denibblized payload size
    //   0x09 = OneSeqDump alt → Command type 0x0B, declared = denibblized payload size
    bool needsCommandPrepend = (srcSize >= 6 && srcData[0] == 0xF0 &&
        (srcData[5] == 0x0A || srcData[5] == 0x0B || srcData[5] == 0x09));

    if (needsCommandPrepend) {
        size_t nibLen = srcSize - 7; // nybbles between SysEx header and F7
        uint32_t totalPayload = (uint32_t)(nibLen / 2);
        uint8_t cmdType = srcData[5]; // data msgType matches command type
        uint8_t cmd[17] = {};
        cmd[0]=0xF0; cmd[1]=0x0F; cmd[2]=0x05;
        cmd[3]=0x00; cmd[4]=0x00; cmd[5]=0x00; cmd[16]=0xF7;

        if (cmdType == 0x0A) {
            // AllSeqDump: declared = pool_size = totalPayload - PTR(240) - HEADERS(11160) - GLOBAL(29)
            uint32_t declared = (totalPayload > 11429) ? (totalPayload - 11429) : 0;
            cmd[6]=0x00; cmd[7]=0x0C;
            cmd[8]=(declared>>28)&0xF; cmd[9]=(declared>>24)&0xF;
            cmd[10]=(declared>>20)&0xF; cmd[11]=(declared>>16)&0xF;
            cmd[12]=(declared>>12)&0xF; cmd[13]=(declared>>8)&0xF;
            cmd[14]=(declared>>4)&0xF;  cmd[15]=declared&0xF;
            DBG("SeqTransfer: AllSeqDump Command, declared=" << declared);
        } else {
            // OneSequence (0x0B or 0x09): Command type 0x0B
            // Bytes [8:12] = slot number (4 nibbles, default 0)
            // Bytes [12:16] = declared size (4 nibbles)
            //
            // For 0x09: declared is embedded in payload bytes [0:4] denibblized.
            // (Total payload = sequence_header(188B) + event_data(declared B))
            // For 0x0B: declared = full denibblized payload size.
            uint32_t declared;
            if (cmdType == 0x09 && srcSize >= 14) {
                // Read declared from first 4 denibblized payload bytes (srcData[6:14])
                declared = 0;
                for (int i = 0; i < 8; ++i)
                    declared = (declared << 4) | (srcData[6 + i] & 0x0F);
            } else {
                declared = totalPayload;
            }
            cmd[6]=0x00; cmd[7]=0x0B;
            cmd[8]=0x00; cmd[9]=0x00; cmd[10]=0x00; cmd[11]=0x00; // slot 0
            cmd[12]=(declared>>12)&0xF; cmd[13]=(declared>>8)&0xF;
            cmd[14]=(declared>>4)&0xF;  cmd[15]=declared&0xF;
            DBG("SeqTransfer: OneSequence Command type=" << (int)cmdType << " declared=" << declared);
        }

        transferBuf.append(cmd, 17);
        transferBuf.append(srcData, srcSize);
        // Patch byte[3] to 0x01 (hardware Model ID) if library outputs 0x00
        {
            uint8_t* tbuf = static_cast<uint8_t*>(transferBuf.getData());
            if (transferBuf.getSize() > 20 && tbuf[20] == 0x00)
                tbuf[20] = 0x01;
        }
    } else {
        // Library may set declared = total_payload (wrong). Patch Command bytes [8:16].
        transferBuf = rawSyxData;
        uint8_t* buf = static_cast<uint8_t*>(transferBuf.getData());
        size_t bufSize = transferBuf.getSize();

        // Find Command packet end (first F7)
        size_t cmd_end = 0;
        for (size_t i = 0; i < bufSize && i < 32; ++i)
            if (buf[i] == 0xF7) { cmd_end = i + 1; break; }

        if (cmd_end > 0 && cmd_end + 6 < bufSize && buf[cmd_end + 5] == 0x0A) {
            // Patch byte[3] of Data packet to 0x01 (SD-1 Model ID).
            // Library generates 0x00 but firmware only accepts AllSeqDump with 0x01.
            if (buf[cmd_end + 3] == 0x00)
                buf[cmd_end + 3] = 0x01;
            
            uint32_t declared = ((uint32_t)buf[8]<<28)|((uint32_t)buf[9]<<24)|
                                ((uint32_t)buf[10]<<20)|((uint32_t)buf[11]<<16)|
                                ((uint32_t)buf[12]<<12)|((uint32_t)buf[13]<<8)|
                                ((uint32_t)buf[14]<<4)|(uint32_t)buf[15];
            size_t nibLen = bufSize - cmd_end - 7;
            uint32_t totalPayload = (uint32_t)(nibLen / 2);

            if (declared == totalPayload && totalPayload > 11429) {
                uint32_t correct = totalPayload - 11429;
                buf[8]=(correct>>28)&0xF; buf[9]=(correct>>24)&0xF;
                buf[10]=(correct>>20)&0xF; buf[11]=(correct>>16)&0xF;
                buf[12]=(correct>>12)&0xF; buf[13]=(correct>>8)&0xF;
                buf[14]=(correct>>4)&0xF;  buf[15]=correct&0xF;
                DBG("SeqTransfer: patched declared_size " << declared << " -> " << correct);
            }
        }
    }

    audioProcessor.suppressMidiInput.store(true, std::memory_order_release);

    // --- BACKGROUND THREAD: schedule all bytes without blocking the message thread ---
    // 91k+ bytes at 0.35ms/byte = ~32s of scheduled MAME time.
    // Running this on the message thread would freeze the DAW for several seconds.
    auto sharedBuf = std::make_shared<juce::MemoryBlock>(transferBuf);
    double now = audioProcessor.mameMachine->time().as_double();

    // Pre-calculate total scheduled duration for the progress overlay.
    // currentDelay builds as: 1.0s initial + size * 0.00035 + 0.3s Command gap
    double estimatedDelay = 1.0 + sharedBuf->getSize() * 0.00035 + 0.3;

    juce::Thread::launch([this, sharedBuf, now]() {
        const uint8_t* data = static_cast<const uint8_t*>(sharedBuf->getData());
        size_t size = sharedBuf->getSize();
        double timePerByte = 0.00035;
        double currentDelay = 1.0;

        for (size_t i = 0; i < size; ++i) {
            audioProcessor.pushMidiByte(data[i], now + currentDelay);
            currentDelay += timePerByte;

            // Small gap between Command (17 bytes ending at F7) and Data packet.
            if (i == 16 && data[i] == 0xF7)
                currentDelay += 0.3;
        }
    });

    isSequenceTransferActive = true;
    isSYXProcessing = true;
    wasSEQImport = true;
    // Transparent overlay component that reliably blocks ALL child clicks during transfer
    clickBlocker = std::make_unique<juce::Component>();
    clickBlocker->setInterceptsMouseClicks(true, true);
    addAndMakeVisible(*clickBlocker);
    clickBlocker->setBounds(getLocalBounds());
    clickBlocker->toFront(false);
    seqTransferStartTimeMs = juce::Time::getMillisecondCounterHiRes();
    double extraCpuTime = (rawSyxData.getSize() < 2500) ? 4.0 : 0.5;
    seqTransferDurationMs = (estimatedDelay + extraCpuTime) * 1000.0;
    int estimatedSec = juce::jmax(1, (int)std::ceil(estimatedDelay));
    seqTransferMessage = message + "\n(~" + juce::String(estimatedSec) + "s — please wait)";
    startTimerHz(30);
    repaint();
}

void PresetBrowserComponent::paintOverlay(juce::Graphics& g)
{
    
    bool booting = isSystemBooting(audioProcessor);
    if (!isSequenceTransferActive) return;
    
    auto bounds = getLocalBounds();
    
    // Dark 70% overlay
    g.setColour(juce::Colour(0xB3000000));  // black at 70% opacity
    g.fillRect(bounds);
    
    // Center message
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(22.0f)).boldened());

    // --- Show Animated Booting Text ---
    if (booting) {

        uint32_t timeMs = juce::Time::getMillisecondCounter();
        int dotCount = (int)((timeMs / 400) % 4);
        juce::String dotStr;
        for (int i = 0; i < dotCount; ++i) dotStr += ".";

        g.setColour(juce::Colours::orange);
        g.setFont(juce::Font(juce::FontOptions(22.0f)).boldened());
        g.drawText("SD-1 is booting" + dotStr, bounds.withTrimmedBottom(40),
                   juce::Justification::centred);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText("Please wait", bounds.withTrimmedTop(40),
                   juce::Justification::centred);
        return; // No progress bar during boot
    }
    
    g.drawText(seqTransferMessage, bounds, juce::Justification::centred);
    
    // Progress bar below the message
    double elapsed = juce::Time::getMillisecondCounterHiRes() - seqTransferStartTimeMs;
    double progress = juce::jlimit(0.0, 1.0, elapsed / seqTransferDurationMs);
    
    auto barArea = bounds.withSizeKeepingCentre(400, 20).translated(0, 40);
    
    // Background
    g.setColour(juce::Colour(0xff333333));
    g.fillRoundedRectangle(barArea.toFloat(), 4.0f);
    
    // Fill
    g.setColour(juce::Colours::orange);
    auto fillArea = barArea.withWidth(static_cast<int>(barArea.getWidth() * progress));
    g.fillRoundedRectangle(fillArea.toFloat(), 4.0f);
    
    // Percentage text
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(14.0f));
    juce::String pctText = juce::String(static_cast<int>(progress * 100)) + "%";
    g.drawText(pctText, barArea.translated(0, 24), juce::Justification::centred);
}

// =========================================================================================
// SYX INJECT
// =========================================================================================

// =============================================================================
// Write Single Program — DIRECT MEMORY INJECTION
// Bypasses the slow and error-prone SD-1 SysEx Write mechanism completely!
// =============================================================================
void PresetBrowserComponent::runWriteProgramSM()
{
    auto& ap = audioProcessor;
    if (ap.mameMachine == nullptr) { wpState = 0; isSYXProcessing = false; return; }

    double now = ap.mameMachine->time().as_double();

    auto fail = [&](const juce::String& msg) {
        selectionNameLabel.setText(msg, juce::dontSendNotification);
        wpState = 0; isSYXProcessing = false;
        progressValue = -1.0; progressBar.setVisible(false); progressLabel.setVisible(false);
        ap.suppressMidiInput.store(false, std::memory_order_release);
    };

    // We only need one execution cycle!
    if (wpState == 1) {
        
        auto* osram_share = ap.mameMachine->root_device().memshare("osram");
        size_t rawSize = currentlySelectedSingleRawData.getSize();
        
        if (osram_share != nullptr && osram_share->bytes() >= (0x0003C8 + 31800) && rawSize == 530) {
            
            // 1. EXTRACT FULL 60-PROGRAM BANK FROM RAM
            uint8_t* osram = static_cast<uint8_t*>(osram_share->ptr());
            uint8_t interleaved[31800];
            for (int i = 0; i < 31800; i++) interleaved[i] = osram[(0x0003C8 + i) ^ 1];
            
            uint8_t* linearData = nullptr;
            uintptr_t linearLen = 0;
            
            if (sd1_deinterleave_sixty_programs(interleaved, 31800, &linearData, &linearLen) == SD1_OK) {
                
                // 2. OVERWRITE THE TARGET SLOT WITH OUR SINGLE PRESET
                const uint8_t* newPreset = static_cast<const uint8_t*>(currentlySelectedSingleRawData.getData());
                std::memcpy(linearData + (wpTargetSlot * 530), newPreset, 530);
                
                // 3. RE-INTERLEAVE AND INJECT BACK TO RAM
                uint8_t* newInterleaved = nullptr;
                uintptr_t newIntLen = 0;
                
                if (sd1_interleave_sixty_programs(linearData, 31800, &newInterleaved, &newIntLen) == SD1_OK) {
                    
                    // We use the AudioProcessor's safe pendingBankInjection system so it writes on the MAME thread
                    ap.pendingBankData.setSize(0);
                    ap.pendingBankData.append(newInterleaved, newIntLen);
                    ap.pendingBankInjection.store(true, std::memory_order_release);
                    
                    sd1_bytes_free(newInterleaved, newIntLen);
                    
                    // 4. CLEANUP & UI SYNC
                    double t = now + 0.05;
                    
                    // Force the SD-1 to refresh the VFD and load the new RAM slot
                    pushSysExButtonPress(11, t, t + 0.10); // SOUNDS
                    t += 0.30;
                    
                    // Switch MIDI context to INT and select the newly written program
                    ap.pushMidiByte(0xC0, t);
                    ap.pushMidiByte(125,  t + 0.00035); // INT Bank
                    t += 0.10;
                    ap.pushMidiByte(0xC0, t);
                    ap.pushMidiByte(wpTargetSlot, t + 0.00035);
                    
                    selectionNameLabel.setText("Saved to INT Slot " + juce::String(wpTargetSlot), juce::dontSendNotification);
                    if (currentCategory == "INT (RAM)") updateContentList("INT (RAM)");
                    
                    wasWriteSinglePreset = true;
                    wasSYXPreview = false; // We didn't use Preview SysEx!
                    isErrorState = false;
                    contentList.deselectAllRows();
                    
                } else {
                    fail("Write failed: Interleave error!");
                }
                sd1_bytes_free(linearData, linearLen);
            } else {
                fail("Write failed: De-interleave error!");
            }
        } else {
            fail("Write failed: Invalid RAM or Data size.");
        }
        
        // Hide progress bar and exit state machine
        progressValue = -1.0;
        progressBar.setVisible(false);
        progressLabel.setVisible(false);
        ap.suppressMidiInput.store(false, std::memory_order_release);
        wpState = 0;
        isSYXProcessing = false;
    }
}


void PresetBrowserComponent::injectAndPreview(double startTimeOffset)
{
    if (isRestoringUI) return;
    size_t rawSize = currentlySelectedSingleRawData.getSize();
    if (rawSize == 0) return;
    if (audioProcessor.mameMachine == nullptr) return;

    audioProcessor.suppressMidiInput.store(true, std::memory_order_release);
    
    const uint8_t* raw = static_cast<const uint8_t*>(currentlySelectedSingleRawData.getData());
        
    uint8_t msgType = 0x02;                     // Default: Program
    if (rawSize == 530) msgType = 0x02;         // One Program
    else if (rawSize == 48) msgType = 0x04;     // One Preset
    else msgType = 0x05;                        // All Presets (Bank)
    
    // --- PRESET MODE OR PRESET BANK ---
    double now = (startTimeOffset >= 0.0) ? startTimeOffset : getSafeBootTime(audioProcessor);
    double t = now + 0.05;
    bool buttonsPushed = false;

    // Exit previous SYX preview state if needed.
    if (wasSYXPreview) {
        pushSysExButtonPress(19, t, t + 0.10); // EXIT
        t += 0.30;
        buttonsPushed = true;
    }

    if (msgType == 0x04 || msgType == 0x05) {
        wasPresetPreviewed = true;
        pushSysExButtonPress(12, t, t + 0.10); // PRESETS
        t += 0.30;
        buttonsPushed = true;
    } else if (msgType == 0x02) {
        if (wasPresetPreviewed) {
            pushSysExButtonPress(11, t, t + 0.10); // SOUNDS
            wasPresetPreviewed = false;
            t += 0.30;
            buttonsPushed = true;
        }
    }
            
    if (buttonsPushed) {
        t += 0.30;
    }

    double startTime = t;

    double timePerByte = 0.00040;

    // SD-1 SysEx Header
    uint8_t header[6] = { 0xF0, 0x0F, 0x05, 0x00, 0x00, msgType };
    for (int i = 0; i < 6; ++i) {
        audioProcessor.pushMidiByte(header[i], startTime);
        startTime += timePerByte;
    }

    // "Nybbling"
    for (size_t i = 0; i < rawSize; ++i) {
        audioProcessor.pushMidiByte((raw[i] >> 4) & 0x0F, startTime);
        startTime += timePerByte;
        
        audioProcessor.pushMidiByte(raw[i] & 0x0F, startTime);
        startTime += timePerByte;
    }

    // EOX
    audioProcessor.pushMidiByte(0xF7, startTime);
    
    lastPreviewEndTime = startTime + 0.50;
    
    // Start progress bar animation
    double totalMidiBytes = 7.0 + (rawSize * 2.0);
    transferDurationMs = 500.0 + (totalMidiBytes * 0.40) + 250.0;
    transferStartTimeMs = juce::Time::getMillisecondCounterHiRes();
    progressValue = 0.0;
    progressBar.setVisible(true);
    progressLabel.setText(msgType == 0x02 ? "Sending Program..." : "Sending Preset...", juce::dontSendNotification);
    progressLabel.setVisible(true);
    startTimerHz(30);
    
    playHintLabel.setVisible(true);
    isSYXProcessing = true;
    wasSYXPreview = true;
}
