#include "boards/sensecap-watcher/sscma_camera_api.h"

#include "board.h"
#include "board_defs.h"
#include "camera.h"
#include "system_info.h"

#include <esp_log.h>

#include <stdexcept>
#include <string>

#define TAG "SscmaCamBridge"

namespace {

class SscmaCameraBridge : public Camera {
public:
    explicit SscmaCameraBridge(sscma_camera_t *cam) : cam_(cam) {}

    void SetExplainUrl(const std::string &url, const std::string &token) override {
        explain_url_ = url;
        explain_token_ = token;
    }

    bool Capture() override {
        if (!cam_) {
            return false;
        }
        esp_err_t err = sscma_camera_capture_still(cam_, 8000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "capture_still failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool SetHMirror(bool enabled) override {
        (void)enabled;
        return false;
    }

    bool SetVFlip(bool enabled) override {
        (void)enabled;
        return false;
    }

    bool SetSwapBytes(bool enabled) override {
        (void)enabled;
        return false;
    }

    std::string Explain(const std::string &question) override {
        if (explain_url_.empty()) {
            throw std::runtime_error("Image explain URL or token is not set");
        }
        size_t jpeg_len = 0;
        const uint8_t *jpeg = sscma_camera_last_jpeg(cam_, &jpeg_len);
        if (!jpeg || jpeg_len == 0) {
            throw std::runtime_error("No camera frame captured");
        }

        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(3);
        const std::string boundary = "----ESP32_CAMERA_BOUNDARY";

        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        if (!explain_token_.empty()) {
            http->SetHeader("Authorization", "Bearer " + explain_token_);
        }
        http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http->SetHeader("Transfer-Encoding", "chunked");
        if (!http->Open("POST", explain_url_)) {
            ESP_LOGE(TAG, "Failed to connect to explain URL");
            throw std::runtime_error("Failed to connect to explain URL");
        }

        {
            std::string question_field;
            question_field += "--" + boundary + "\r\n";
            question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
            question_field += "\r\n";
            question_field += question + "\r\n";
            http->Write(question_field.c_str(), question_field.size());
        }
        {
            std::string file_header;
            file_header += "--" + boundary + "\r\n";
            file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
            file_header += "Content-Type: image/jpeg\r\n";
            file_header += "\r\n";
            http->Write(file_header.c_str(), file_header.size());
        }

        http->Write(reinterpret_cast<const char *>(jpeg), jpeg_len);

        {
            std::string multipart_footer;
            multipart_footer += "\r\n--" + boundary + "--\r\n";
            http->Write(multipart_footer.c_str(), multipart_footer.size());
        }
        http->Write("", 0);

        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "Explain upload failed, status=%d", http->GetStatusCode());
            http->Close();
            throw std::runtime_error("Failed to upload photo");
        }

        std::string result = http->ReadAll();
        http->Close();

        ESP_LOGI(TAG, "Explain jpeg_len=%u remain_stack=%u question=%s",
                 (unsigned)jpeg_len,
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                 question.c_str());
        return result;
    }

private:
    sscma_camera_t *cam_;
    std::string explain_url_;
    std::string explain_token_;
};

}  // namespace

extern "C" Camera *board_bridge_wrap_camera_handle(board_camera_kind_t kind, void *raw) {
    if (kind != BOARD_CAMERA_KIND_SSCMA || raw == nullptr) {
        return nullptr;
    }
    return new SscmaCameraBridge(static_cast<sscma_camera_t *>(raw));
}
