#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_c_api.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "c_api/main_event_bits.h"

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_is_voice_detected(audio_svc_); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    audio_service_t *GetAudioService() { return audio_svc_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

    /** Shared with C via `application_main_event_group()` for incremental C main-loop work. */
    EventGroupHandle_t GetMainEventGroupHandle() const { return event_group_; }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    audio_service_t *audio_svc_ = nullptr;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
