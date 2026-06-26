#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/TextInput.hpp>
#include <fmod.hpp>
#include <Geode/utils/string.hpp>

#include <thread>
#include <mutex>
#include <vector>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#undef min
#undef max

#include <stdio.h>
#include <fmt/format.h>

#pragma comment(lib, "Ole32.lib")

using namespace geode::prelude;

bool g_isRecording = false;
bool g_levelIsActive = true; 

std::filesystem::path g_saveDirectory = ""; 
int g_targetFPS = 60; 
int g_crf = 18;
static int g_recordingSessionId = 0; 

// windows named pipes for sending raw data safely to FFmpeg
HANDLE g_ffmpegPipeWrite = NULL; 
HANDLE g_ffmpegAudioPipeWrite = NULL;
FMOD::DSP* g_captureDSP = nullptr;

// local memory buffers for storing frame data before encoding
std::mutex g_audioMutex;
std::vector<float> g_audioBuffer;

std::mutex g_videoMutex;
std::vector<std::vector<unsigned char>> g_videoQueue;

FMOD_RESULT F_CALLBACK DSP_Create(FMOD_DSP_STATE *dsp_state) { return FMOD_OK; }
FMOD_RESULT F_CALLBACK DSP_Release(FMOD_DSP_STATE *dsp_state) { return FMOD_OK; }
FMOD_RESULT F_CALLBACK DSP_Reset(FMOD_DSP_STATE *dsp_state) { return FMOD_OK; }

// flushes memory and closes local pipe handles to prevent leaks
void stopLosslessRecording() {
    if (!g_isRecording) return;
    g_isRecording = false;

    if (g_captureDSP) {
        auto system = FMODAudioEngine::sharedEngine()->m_system;
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
            system->lockDSP();
            masterGroup->removeDSP(g_captureDSP);
            system->unlockDSP();
        }
        g_captureDSP->release();
        g_captureDSP = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_audioBuffer.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_videoMutex);
        g_videoQueue.clear();
    }

    // Gives background writer threads 100ms to cleanly exit their loops 
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (g_ffmpegPipeWrite) {
        CloseHandle(g_ffmpegPipeWrite);
        g_ffmpegPipeWrite = NULL;
    }
    if (g_ffmpegAudioPipeWrite) {
        CloseHandle(g_ffmpegAudioPipeWrite);
        g_ffmpegAudioPipeWrite = NULL;
    }
}

// Reads raw game audio and pipes it strictly to local video encoding
FMOD_RESULT F_CALLBACK AudioCaptureCallback(FMOD_DSP_STATE* dsp_state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int* outchannels) {
    if (inbuffer && outbuffer && outchannels) {
        int copyChans = (inchannels < *outchannels) ? inchannels : *outchannels;
        if (inbuffer != outbuffer) {
            memcpy(outbuffer, inbuffer, length * copyChans * sizeof(float));
        }
    }

    // Doesnt record when paused
    if (g_isRecording && g_levelIsActive && g_ffmpegAudioPipeWrite && inbuffer) {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_audioBuffer.insert(g_audioBuffer.end(), inbuffer, inbuffer + (length * inchannels));
    }
    return FMOD_OK;
}

// Opens a native dialogue allowing the user to select their save location
std::filesystem::path PickFolderNative() {
    std::filesystem::path result = "";
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        IFileOpenDialog* pFileOpen;
        hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
        if (SUCCEEDED(hr)) {
            DWORD dwOptions;
            pFileOpen->GetOptions(&dwOptions);
            pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            
            if (SUCCEEDED(pFileOpen->Show(NULL))) {
                IShellItem* pItem;
                if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                    PWSTR pszFilePath;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                        std::wstring ws(pszFilePath);
                        result = std::filesystem::path(ws);
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
            pFileOpen->Release();
        }
        CoUninitialize();
    }
    return result;
}

// Handles UI rendering and parameter configuration within the game menu
class RecorderSettingsPopup : public FLAlertLayer {
protected:
    CCLabelBMFont* m_dirLabel;
    TextInput* m_crfInput;
    TextInput* m_fpsInput;

    bool init() {
        if (!FLAlertLayer::init(75)) return false;

        if (g_saveDirectory.empty()) {
            g_saveDirectory = Mod::get()->getSaveDir();
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        auto bg = CCScale9Sprite::create("GJ_square01.png"); 
        bg->setContentSize({420.f, 280.f});
        bg->setPosition({winSize.width / 2, winSize.height / 2});
        m_mainLayer->addChild(bg, -1);

        auto title = CCLabelBMFont::create("Level Recorder", "goldFont.fnt");
        title->setPosition({winSize.width / 2, winSize.height / 2 + 115.f});
        title->setScale(0.8f);
        m_mainLayer->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        closeSpr->setScale(0.8f);
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(RecorderSettingsPopup::onClose));
        closeBtn->setPosition({winSize.width / 2 - 200.f, winSize.height / 2 + 130.f});
        menu->addChild(closeBtn);

        m_dirLabel = CCLabelBMFont::create(geode::utils::string::pathToString(g_saveDirectory).c_str(), "chatFont.fnt");
        m_dirLabel->setPosition({winSize.width / 2, winSize.height / 2 + 50});
        m_dirLabel->limitLabelWidth(240, 0.7f, 0.1f);
        m_mainLayer->addChild(m_dirLabel);

        auto browseSpr = ButtonSprite::create("Browse", "goldFont.fnt", "GJ_button_04.png", .8f);
        auto browseBtn = CCMenuItemSpriteExtra::create(browseSpr, this, menu_selector(RecorderSettingsPopup::onBrowse));
        browseBtn->setPosition({winSize.width / 2 + 150, winSize.height / 2 + 50});
        menu->addChild(browseBtn);

        auto fpsLabel = CCLabelBMFont::create("Target FPS:", "bigFont.fnt");
        fpsLabel->setScale(0.5f);
        fpsLabel->setPosition({winSize.width / 2 - 60, winSize.height / 2 - 10});
        m_mainLayer->addChild(fpsLabel);

        m_fpsInput = TextInput::create(60.f, "60", "chatFont.fnt");
        m_fpsInput->setCommonFilter(CommonFilter::Uint);
        m_fpsInput->setMaxCharCount(4);
        m_fpsInput->setString(std::to_string(g_targetFPS));
        m_fpsInput->setPosition({winSize.width / 2 + 70, winSize.height / 2 - 10});
        m_mainLayer->addChild(m_fpsInput);

        auto crfLabel = CCLabelBMFont::create("CRF:", "bigFont.fnt");
        crfLabel->setScale(0.5f);
        crfLabel->setPosition({winSize.width / 2 - 60, winSize.height / 2 - 50});
        m_mainLayer->addChild(crfLabel);

        m_crfInput = TextInput::create(60.f, "0", "chatFont.fnt");
        m_crfInput->setCommonFilter(CommonFilter::Uint);
        m_crfInput->setMaxCharCount(2);
        m_crfInput->setString(std::to_string(g_crf));
        m_crfInput->setPosition({winSize.width / 2 + 70, winSize.height / 2 - 50});
        m_mainLayer->addChild(m_crfInput);

        auto startSpr = ButtonSprite::create("Start", "bigFont.fnt", "GJ_button_01.png", .8f);
        auto startBtn = CCMenuItemSpriteExtra::create(startSpr, this, menu_selector(RecorderSettingsPopup::onStart));
        startBtn->setPosition({winSize.width / 2 - 70, winSize.height / 2 - 110});
        menu->addChild(startBtn);

        auto stopSpr = ButtonSprite::create("Stop", "bigFont.fnt", "GJ_button_06.png", .8f);
        auto stopBtn = CCMenuItemSpriteExtra::create(stopSpr, this, menu_selector(RecorderSettingsPopup::onStop));
        stopBtn->setPosition({winSize.width / 2 + 70, winSize.height / 2 - 110});
        menu->addChild(stopBtn);

        this->setKeypadEnabled(true);
        this->setTouchEnabled(true);

        return true;
    }

    void onClose(CCObject*) {
        this->setKeypadEnabled(false);
        this->removeFromParentAndCleanup(true);
    }
    
    void keyBackClicked() override {
        onClose(nullptr);
    }

    void onBrowse(CCObject*) {
        static bool isPickerThreadRunning = false;
        if (isPickerThreadRunning) return;
        isPickerThreadRunning = true;

        this->retain(); 

        std::thread([this]() {
            std::filesystem::path picked = PickFolderNative();
            
            geode::Loader::get()->queueInMainThread([this, picked]() {
                if (!picked.empty()) {
                    g_saveDirectory = picked;
                    if (m_dirLabel) {
                        m_dirLabel->setString(geode::utils::string::pathToString(g_saveDirectory).c_str());
                    }
                }
                isPickerThreadRunning = false;
                this->release();
            });
        }).detach();
    }

    void onStart(CCObject*) {
        if (g_isRecording) return;
        
        // Safety check using Geode's numFromString to prevent parser exception crashes
        if (m_crfInput && !m_crfInput->getString().empty()) {
            auto crfRes = geode::utils::numFromString<int>(m_crfInput->getString());
            if (crfRes.isOk()) {
                g_crf = crfRes.unwrap();
            } else {
                FLAlertLayer::create("Error", "Invalid CRF format.", "OK")->show();
                return;
            }
        }

        if (m_fpsInput && !m_fpsInput->getString().empty()) {
            auto fpsRes = geode::utils::numFromString<int>(m_fpsInput->getString());
            if (fpsRes.isOk()) {
                g_targetFPS = fpsRes.unwrap();
            } else {
                FLAlertLayer::create("Error", "Invalid FPS format.", "OK")->show();
                return;
            }
        }

        std::string finalPath = "";
        int counter = 1;
        while (true) {
            std::string filename = fmt::format("Recording_{}.mp4", counter);
            std::filesystem::path fullPath = g_saveDirectory / filename;
            if (!std::filesystem::exists(fullPath)) {
                finalPath = geode::utils::string::pathToString(fullPath);
                break;
            }
            counter++;
        }

        auto size = CCDirector::sharedDirector()->getOpenGLView()->getFrameSize();
        int w = static_cast<int>(size.width);
        int h = static_cast<int>(size.height);

        // Forces even dimensions to prevent libx264 crashing
        if (w % 2 != 0) w--;
        if (h % 2 != 0) h--;
        
        // Look for ffmpeg.exe in the Geode config folder. 
        std::filesystem::path configDir = Mod::get()->getConfigDir();
        std::filesystem::create_directories(configDir);
        std::filesystem::path ffmpegPath = configDir / "ffmpeg.exe";
        
        if (!std::filesystem::exists(ffmpegPath)) {
            std::string msg = fmt::format("Please download ffmpeg.exe and place it in:\n{}", geode::utils::string::pathToString(configDir));
            FLAlertLayer::create("FFmpeg Missing", msg.c_str(), "OK")->show();
            return;
        }

        g_recordingSessionId++;
        
        // local named pipes for secure process-to-process communication
        std::string vidPipeName = fmt::format("\\\\.\\pipe\\gd_vid_{}_{}", GetCurrentProcessId(), g_recordingSessionId);
        std::string audPipeName = fmt::format("\\\\.\\pipe\\gd_aud_{}_{}", GetCurrentProcessId(), g_recordingSessionId);

        HANDLE hVidPipe = CreateNamedPipeA(vidPipeName.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1024 * 1024 * 8, 1024 * 1024 * 8, 0, NULL);
        HANDLE hAudPipe = CreateNamedPipeA(audPipeName.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1024 * 1024 * 8, 1024 * 1024 * 8, 0, NULL);

        if (hVidPipe == INVALID_HANDLE_VALUE || hAudPipe == INVALID_HANDLE_VALUE) {
            FLAlertLayer::create("Error", "Failed to setup audio/video pipes.", "OK")->show();
            return;
        }

        auto system = FMODAudioEngine::sharedEngine()->m_system;
        int sampleRate = 44100;
        FMOD_SPEAKERMODE speakerMode;
        int numSpeakers = 2;
        system->getSoftwareFormat(&sampleRate, &speakerMode, &numSpeakers);

        FMOD_DSP_DESCRIPTION dspDesc;
        memset(&dspDesc, 0, sizeof(FMOD_DSP_DESCRIPTION));
        strncpy(dspDesc.name, "LosslessAudioCapture", sizeof(dspDesc.name));
        dspDesc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION; 
        dspDesc.numinputbuffers = 1;
        dspDesc.numoutputbuffers = 1;
        dspDesc.read = AudioCaptureCallback;
        dspDesc.create = DSP_Create;   
        dspDesc.release = DSP_Release; 
        dspDesc.reset = DSP_Reset;     

        if (system->createDSP(&dspDesc, &g_captureDSP) == FMOD_OK && g_captureDSP) {
            FMOD::ChannelGroup* masterGroup = nullptr;
            if (system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
                system->lockDSP();
                masterGroup->addDSP(0, g_captureDSP);
                system->unlockDSP();
            } else {
                FLAlertLayer::create("Error", "Failed to hook into FMOD Master Group.", "OK")->show();
                g_captureDSP->release();
                g_captureDSP = nullptr;
                CloseHandle(hVidPipe);
                CloseHandle(hAudPipe);
                return;
            }
        } else {
            FLAlertLayer::create("Error", "Failed to construct FMOD DSP capture hook.", "OK")->show();
            CloseHandle(hVidPipe);
            CloseHandle(hAudPipe);
            return;
        }

        // ffmpeg things
        std::string cmd = fmt::format(
            "\"{}\" -y "
            "-thread_queue_size 1024 -f rawvideo -pix_fmt rgb24 -s {}x{} -r {} -i {} "
            "-thread_queue_size 1024 -f f32le -ar {} -ac {} -i {} "
            "-vf \"vflip,scale=trunc(iw/2)*2:trunc(ih/2)*2\" -c:v libx264 -preset ultrafast -pix_fmt yuv420p -crf {} "
            "-c:a aac -ac 2 -b:a 320k -movflags +faststart \"{}\"",
            geode::utils::string::pathToString(ffmpegPath), w, h, g_targetFPS, vidPipeName, sampleRate, numSpeakers, audPipeName, g_crf, finalPath
        );

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(STARTUPINFOA));
        si.cb = sizeof(STARTUPINFOA);
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE; 

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

        std::vector<char> cmdBuffer(cmd.begin(), cmd.end());
        cmdBuffer.push_back('\0');

        BOOL success = CreateProcessA(
            NULL, cmdBuffer.data(), NULL, NULL, FALSE, 
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi
        );

        if (!success) {
            if (g_captureDSP) {
                FMOD::ChannelGroup* masterGroup = nullptr;
                if (system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
                    system->lockDSP();
                    masterGroup->removeDSP(g_captureDSP);
                    system->unlockDSP();
                }
                g_captureDSP->release();
                g_captureDSP = nullptr;
            }
            CloseHandle(hVidPipe);
            CloseHandle(hAudPipe);
            FLAlertLayer::create("Error", "Could not start FFmpeg process.", "OK")->show();
            return;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        g_isRecording = true;

        std::thread([hVidPipe]() {
            ConnectNamedPipe(hVidPipe, NULL);
            if (g_isRecording) g_ffmpegPipeWrite = hVidPipe;
            else CloseHandle(hVidPipe);
        }).detach();

        std::thread([hAudPipe]() {
            ConnectNamedPipe(hAudPipe, NULL);
            if (g_isRecording) g_ffmpegAudioPipeWrite = hAudPipe;
            else CloseHandle(hAudPipe);
        }).detach();

        // Background Writer Threads
        std::thread([]() {
            while (g_isRecording) {
                if (!g_ffmpegAudioPipeWrite) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                std::vector<float> localBuffer;
                {
                    std::lock_guard<std::mutex> lock(g_audioMutex);
                    localBuffer.swap(g_audioBuffer);
                }
                
                if (!localBuffer.empty()) {
                    DWORD written = 0;
                    WriteFile(g_ffmpegAudioPipeWrite, localBuffer.data(), localBuffer.size() * sizeof(float), &written, NULL);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        }).detach();

        std::thread([]() {
            while (g_isRecording) {
                if (!g_ffmpegPipeWrite) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                std::vector<unsigned char> frame;
                {
                    std::lock_guard<std::mutex> lock(g_videoMutex);
                    if (!g_videoQueue.empty()) {
                        frame = std::move(g_videoQueue.front());
                        g_videoQueue.erase(g_videoQueue.begin());
                    }
                }
                
                if (!frame.empty()) {
                    DWORD written = 0;
                    WriteFile(g_ffmpegPipeWrite, frame.data(), frame.size(), &written, NULL);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }).detach();

        FLAlertLayer::create("Recorder", fmt::format("Started capturing to Recording_{}.mp4! Unpause to record.", counter).c_str(), "OK")->show();
    }

    void onStop(CCObject*) {
        stopLosslessRecording();
        FLAlertLayer::create("Recorder", "Recording Saved.", "OK")->show();
    }

public:
    static RecorderSettingsPopup* create() {
        auto ret = new RecorderSettingsPopup();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void show() {
        FLAlertLayer::show(); 
        geode::cocos::handleTouchPriority(this); 
    }
};

class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        // when pause menu opens, all capturing pauses
        g_levelIsActive = false;

        auto menu = this->getChildByID("right-button-menu");
        if (menu) {
            auto recSpr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
            recSpr->setColor({255, 105, 180}); 
            auto recBtn = CCMenuItemSpriteExtra::create(
                recSpr, 
                this, 
                menu_selector(MyPauseLayer::onRecorderMenu)
            );
            recBtn->setID("lossless-recorder-btn");
            menu->addChild(recBtn);
            menu->updateLayout();
        }
    }

    void onResume(CCObject* sender) {
        // resync recording on unpause
        g_levelIsActive = true;
        PauseLayer::onResume(sender);
    }

    void onQuit(CCObject* sender) {
        stopLosslessRecording(); // force saves the video if you quit
        PauseLayer::onQuit(sender);
    }

    void onRecorderMenu(CCObject*) {
        RecorderSettingsPopup::create()->show();
    }
};

// Intercepts raw OpenGL frame buffers natively inside the render loop
class $modify(CCDirector) {
    void drawScene() {
        CCDirector::drawScene();

        static std::chrono::time_point<std::chrono::steady_clock> s_startTime;
        static int s_framesPushed = 0;
        static bool s_recordingStarted = false; 

        if (g_isRecording) {
            // If in the menu, dont record frames
            if (!g_levelIsActive) {
                s_recordingStarted = false; 
                return;
            }

            if (!s_recordingStarted) {
                s_startTime = std::chrono::steady_clock::now();
                s_recordingStarted = true;
                s_framesPushed = -1; // forces the frame immediately after unpausing
            }

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - s_startTime).count();
            int expectedFrames = static_cast<int>(elapsed * g_targetFPS);

            if (expectedFrames > s_framesPushed) {
                auto size = this->getOpenGLView()->getFrameSize();
                int width = static_cast<int>(size.width);
                int height = static_cast<int>(size.height);

                if (width % 2 != 0) width--;
                if (height % 2 != 0) height--;

                glPixelStorei(GL_PACK_ALIGNMENT, 1);

                static std::vector<unsigned char> s_pixelBuffer;
                size_t reqSize = static_cast<size_t>(width) * height * 3;
                if (s_pixelBuffer.size() != reqSize) {
                    s_pixelBuffer.resize(reqSize);
                }

                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, s_pixelBuffer.data());
                
                int framesToPush = expectedFrames - s_framesPushed;

                std::lock_guard<std::mutex> lock(g_videoMutex);
                for (int i = 0; i < framesToPush; i++) {
                    g_videoQueue.push_back(s_pixelBuffer);
                }

                s_framesPushed = expectedFrames;

                if (g_videoQueue.size() > 600) { 
                    g_videoQueue.erase(g_videoQueue.begin());
                }
            }
        } else {
            s_recordingStarted = false;
        }
    }
};