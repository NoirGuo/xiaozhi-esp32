#include "review_engine.h"

#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <nvs_flash.h>
#include <cJSON.h>

#define TAG "ReviewEngine"

// cards.json 已转为 cards_data.cc 中的字节数组（board 目录源码随构建自动编译）。
extern const unsigned char cards_json_bin[];
extern const size_t cards_json_bin_len;

namespace {

// 判断 cJSON 字符串字段是否非空。
inline std::string JsonString(const cJSON* obj, const char* key) {
    if (obj == nullptr) {
        return "";
    }
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr) {
        return "";
    }
    return std::string(item->valuestring);
}

}  // namespace

ReviewEngine& ReviewEngine::GetInstance() {
    static ReviewEngine instance;
    return instance;
}

bool ReviewEngine::Initialize(const char* json_text, size_t json_length) {
    if (loaded_) {
        return true;
    }
    if (json_text == nullptr || json_length == 0) {
        // 未传入文本时，回退到嵌入的 cards_data.cc 字节数组。
        json_text = reinterpret_cast<const char*>(cards_json_bin);
        json_length = cards_json_bin_len;
    }
    if (json_text == nullptr || json_length == 0) {
        ESP_LOGE(TAG, "cards.json not embedded or empty");
        return false;
    }
    if (!ParseJson(json_text, json_length)) {
        return false;
    }
    LoadWrongFlags();
    loaded_ = true;
    ESP_LOGI(TAG, "Loaded %d cards", (int)cards_.size());
    return true;
}

bool ReviewEngine::ParseJson(const char* text, size_t length) {
    cJSON* root = cJSON_ParseWithLength(text, length);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse cards.json");
        return false;
    }

    cJSON* cards_array = cJSON_GetObjectItem(root, "cards");
    if (cards_array == nullptr || !cJSON_IsArray(cards_array)) {
        ESP_LOGE(TAG, "cards.json missing 'cards' array");
        cJSON_Delete(root);
        return false;
    }

    cards_.clear();
    int count = cJSON_GetArraySize(cards_array);
    cards_.reserve(count);

    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(cards_array, i);
        if (item == nullptr) {
            continue;
        }
        ReviewCard card;
        card.id = JsonString(item, "id");
        card.doc = JsonString(item, "doc");
        card.chapter = JsonString(item, "chapter");
        card.title = JsonString(item, "title");
        card.mnemonic = JsonString(item, "mnemonic");
        card.exam_point = JsonString(item, "exam_point");
        card.recite = JsonString(item, "recite");

        cJSON* points = cJSON_GetObjectItem(item, "points");
        if (points != nullptr && cJSON_IsArray(points)) {
            int n = cJSON_GetArraySize(points);
            for (int j = 0; j < n; ++j) {
                cJSON* p = cJSON_GetArrayItem(points, j);
                if (p != nullptr && cJSON_IsString(p) && p->valuestring != nullptr) {
                    card.points.push_back(std::string(p->valuestring));
                }
            }
        }
        if (card.id.empty() && card.title.empty()) {
            // 跳过明显无效的条目。
            continue;
        }
        cards_.push_back(std::move(card));
    }

    cJSON_Delete(root);
    return !cards_.empty();
}

bool ReviewEngine::NextCard(bool random, const ReviewCard** card, int* index, int* total) {
    if (!loaded_ || cards_.empty()) {
        return false;
    }

    random_mode_ = random;
    int total_count = wrong_mode_ ? (int)wrong_queue_.size() : (int)cards_.size();
    if (total_count <= 0) {
        return false;
    }

    if (wrong_mode_) {
        // 错题回顾：顺序遍历错题队列。
        if (wrong_queue_pos_ >= wrong_queue_.size()) {
            wrong_queue_pos_ = 0;
        }
        current_index_ = wrong_queue_[wrong_queue_pos_];
        if (card != nullptr) {
            *card = &cards_[current_index_];
        }
        if (index != nullptr) {
            *index = (int)wrong_queue_pos_;
        }
        if (total != nullptr) {
            *total = total_count;
        }
        wrong_queue_pos_++;
    } else if (random) {
        int next = current_index_;
        if (cards_.size() == 1) {
            next = 0;
        } else {
            // 随机且避免与上一张重复。
            int n = (int)cards_.size();
            do {
                next = rand() % n;
            } while (next == current_index_);
        }
        current_index_ = next;
        if (card != nullptr) {
            *card = &cards_[current_index_];
        }
        if (index != nullptr) {
            *index = current_index_;
        }
        if (total != nullptr) {
            *total = total_count;
        }
    } else {
        // 顺序模式：递增，到头回到 0。
        current_index_ = (current_index_ + 1) % cards_.size();
        if (card != nullptr) {
            *card = &cards_[current_index_];
        }
        if (index != nullptr) {
            *index = current_index_;
        }
        if (total != nullptr) {
            *total = total_count;
        }
    }
    return true;
}

const ReviewCard* ReviewEngine::CurrentCard() const {
    if (!loaded_ || current_index_ < 0 || current_index_ >= (int)cards_.size()) {
        return nullptr;
    }
    return &cards_[current_index_];
}

void ReviewEngine::MarkWrong(const std::string& card_id, bool mark) {
    // 找到对应下标。
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (cards_[i].id == card_id) {
            wrong_flags_[i] = mark;
            SaveWrongFlag(card_id, mark);
            break;
        }
    }
    RebuildWrongQueue();
}

void ReviewEngine::StartWrongReview() {
    RebuildWrongQueue();
    wrong_mode_ = true;
    wrong_queue_pos_ = 0;
}

void ReviewEngine::ResetAllProgress() {
    for (size_t i = 0; i < cards_.size(); ++i) {
        wrong_flags_[i] = false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open("review", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    RebuildWrongQueue();
}

void ReviewEngine::LoadWrongFlags() {
    wrong_flags_.assign(cards_.size(), false);

    nvs_handle_t handle = 0;
    if (nvs_open("review", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < cards_.size(); ++i) {
        uint8_t flag = 0;
        if (nvs_get_u8(handle, cards_[i].id.c_str(), &flag) == ESP_OK && flag != 0) {
            wrong_flags_[i] = true;
        }
    }
    nvs_close(handle);
    RebuildWrongQueue();
}

void ReviewEngine::SaveWrongFlag(const std::string& card_id, bool mark) {
    nvs_handle_t handle = 0;
    if (nvs_open("review", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for review progress");
        return;
    }
    nvs_set_u8(handle, card_id.c_str(), mark ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
}

void ReviewEngine::RebuildWrongQueue() {
    wrong_queue_.clear();
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (wrong_flags_[i]) {
            wrong_queue_.push_back((int)i);
        }
    }
}
