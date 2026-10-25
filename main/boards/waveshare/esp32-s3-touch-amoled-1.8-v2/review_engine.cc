#include "review_engine.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <nvs_flash.h>
#include <cJSON.h>

#define TAG "ReviewEngine"

// cards.json 已转为 cards_data.cc 中的字节数组（board 目录源码随构建自动编译）。
extern const unsigned char cards_json_bin[];
extern const size_t cards_json_bin_len;

namespace {

// 设备时间与服务器同步后才有意义；低于该值视为“未同步时间”。
// （ota.cc 通过 settimeofday 写入服务器时间）
constexpr uint32_t kMinValidUnixTime = 1600000000u;  // 2020-09-13

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

inline std::vector<std::string> JsonStringArray(const cJSON* obj, const char* key) {
    std::vector<std::string> result;
    if (obj == nullptr) {
        return result;
    }
    const cJSON* arr = cJSON_GetObjectItem(obj, key);
    if (arr == nullptr || !cJSON_IsArray(arr)) {
        return result;
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        cJSON* p = cJSON_GetArrayItem(arr, i);
        if (p != nullptr && cJSON_IsString(p) && p->valuestring != nullptr) {
            result.push_back(std::string(p->valuestring));
        }
    }
    return result;
}

// 子串匹配：中文按字节比较即可；ASCII 部分做大小写不敏感处理。
bool ContainsFold(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.empty()) {
        return false;
    }
    const size_t hn = haystack.size();
    const size_t nn = needle.size();
    if (nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; ++i) {
        bool match = true;
        for (size_t j = 0; j < nn; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            // ASCII 大小写折叠；UTF-8 多字节（中文）首位为 1，不会被改写。
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
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
    LoadProgress();
    loaded_ = true;
    ESP_LOGI(TAG, "Loaded %d cards, %d docs", (int)cards_.size(), (int)docs_.size());
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
            continue;
        }
        cards_.push_back(std::move(card));
    }

    ParseDocs(root);
    cJSON_Delete(root);

    if (cards_.empty()) {
        return false;
    }
    BuildDocIndex();
    return true;
}

// 解析 docs[]：法规的中文全名、分类与标签。
void ReviewEngine::ParseDocs(const cJSON* root) {
    docs_.clear();
    const cJSON* docs_array = cJSON_GetObjectItem(root, "docs");
    if (docs_array == nullptr || !cJSON_IsArray(docs_array)) {
        ESP_LOGW(TAG, "cards.json has no 'docs' array, category/tag filter disabled");
        return;
    }
    int n = cJSON_GetArraySize(docs_array);
    for (int i = 0; i < n; ++i) {
        const cJSON* item = cJSON_GetArrayItem(docs_array, i);
        if (item == nullptr) {
            continue;
        }
        DocInfo doc;
        doc.id = JsonString(item, "id");
        doc.name = JsonString(item, "name");
        doc.category = JsonString(item, "category");
        doc.tags = JsonStringArray(item, "tags");
        if (doc.id.empty()) {
            continue;
        }
        docs_.push_back(std::move(doc));
    }
}

// 统计每部法规的卡片数，并按卡片出现顺序收集章节。
// 若卡片引用了 docs[] 里没有的 doc，则动态补一条，避免漏卡。
void ReviewEngine::BuildDocIndex() {
    for (size_t i = 0; i < cards_.size(); ++i) {
        const ReviewCard& card = cards_[i];
        int di = IndexOfDoc(card.doc);
        if (di < 0) {
            DocInfo doc;
            doc.id = card.doc;
            doc.name = card.doc;  // 无元数据时退化为缩写
            docs_.push_back(std::move(doc));
            di = (int)docs_.size() - 1;
        }
        DocInfo& doc = docs_[di];
        doc.card_count++;
        if (!card.chapter.empty()) {
            bool exists = false;
            for (const auto& c : doc.chapters) {
                if (c == card.chapter) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                doc.chapters.push_back(card.chapter);
            }
        }
    }
    // 卡片多的法规排在前面，便于语音播报时优先推荐。
    for (size_t i = 1; i < docs_.size(); ++i) {
        for (size_t j = i; j > 0; --j) {
            if (docs_[j].card_count > docs_[j - 1].card_count) {
                DocInfo tmp = std::move(docs_[j]);
                docs_[j] = std::move(docs_[j - 1]);
                docs_[j - 1] = std::move(tmp);
            }
        }
    }
}

int ReviewEngine::IndexOfDoc(const std::string& doc_id) const {
    for (size_t i = 0; i < docs_.size(); ++i) {
        if (docs_[i].id == doc_id) {
            return (int)i;
        }
    }
    return -1;
}

// ===== 法规选择入口 =====

std::vector<std::string> ReviewEngine::Categories() const {
    std::vector<std::string> result;
    for (const auto& d : docs_) {
        if (d.category.empty()) {
            continue;
        }
        bool exists = false;
        for (const auto& c : result) {
            if (c == d.category) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            result.push_back(d.category);
        }
    }
    return result;
}

std::vector<std::string> ReviewEngine::Tags() const {
    std::vector<std::string> result;
    for (const auto& d : docs_) {
        for (const auto& t : d.tags) {
            bool exists = false;
            for (const auto& e : result) {
                if (e == t) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                result.push_back(t);
            }
        }
    }
    return result;
}

std::vector<DocInfo> ReviewEngine::SearchDocs(const std::string& keyword,
                                              const std::string& category,
                                              const std::string& tag) const {
    std::vector<DocInfo> result;
    for (const auto& d : docs_) {
        // 名称关键字：命中 id 或中文全名。
        if (!keyword.empty() &&
            !ContainsFold(d.name, keyword) &&
            !ContainsFold(d.id, keyword)) {
            continue;
        }
        // 分类：精确匹配（分类数量少，精确更好用）。
        if (!category.empty() && d.category != category) {
            continue;
        }
        // 标签：命中任一即可。
        if (!tag.empty()) {
            bool hit = false;
            for (const auto& t : d.tags) {
                if (t == tag || ContainsFold(t, tag)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                continue;
            }
        }
        result.push_back(d);
    }
    return result;
}

std::vector<std::pair<std::string, int>> ReviewEngine::ChaptersOf(const std::string& doc_id) const {
    std::vector<std::pair<std::string, int>> result;
    int di = IndexOfDoc(doc_id);
    if (di < 0) {
        return result;
    }
    for (const auto& chapter : docs_[di].chapters) {
        int n = 0;
        for (const auto& card : cards_) {
            if (card.doc == doc_id && card.chapter == chapter) {
                n++;
            }
        }
        result.push_back(std::make_pair(chapter, n));
    }
    return result;
}

std::vector<std::string> ReviewEngine::ResolveDocIds(const std::string& input) const {
    std::vector<std::string> result;
    if (input.empty()) {
        return result;
    }
    // 1) 精确匹配 id（忽略大小写）。
    std::string folded = input;
    for (char& c : folded) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
    }
    for (const auto& d : docs_) {
        std::string id = d.id;
        for (char& c : id) {
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
        }
        if (id == folded) {
            result.push_back(d.id);
            return result;
        }
    }
    // 2) 名称子串匹配（可能命中多部）。
    for (const auto& d : docs_) {
        if (ContainsFold(d.name, input)) {
            result.push_back(d.id);
        }
    }
    return result;
}

// ===== 范围选择 =====

std::vector<int> ReviewEngine::CollectIndices(const std::vector<std::string>& doc_ids,
                                              const std::string& chapter,
                                              const std::string& keyword) const {
    std::vector<int> result;
    for (size_t i = 0; i < cards_.size(); ++i) {
        const ReviewCard& card = cards_[i];
        // 法规多选：并集；doc_ids 为空表示不限。
        if (!doc_ids.empty()) {
            bool hit = false;
            for (const auto& d : doc_ids) {
                if (card.doc == d) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                continue;
            }
        }
        // 章节：双向子串匹配，容忍“总则”与“第一章 总则”这类差异。
        if (!chapter.empty() &&
            !ContainsFold(card.chapter, chapter) &&
            !ContainsFold(chapter, card.chapter)) {
            continue;
        }
        // 内容关键字：标题 / 要点 / 口诀 / 考点 / 口播稿。
        if (!keyword.empty()) {
            bool hit = ContainsFold(card.title, keyword) ||
                       ContainsFold(card.mnemonic, keyword) ||
                       ContainsFold(card.exam_point, keyword) ||
                       ContainsFold(card.recite, keyword);
            if (!hit) {
                for (const auto& p : card.points) {
                    if (ContainsFold(p, keyword)) {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit) {
                continue;
            }
        }
        result.push_back((int)i);
    }
    return result;
}

bool ReviewEngine::SelectScope(const std::vector<std::string>& doc_ids,
                               const std::string& chapter,
                               const std::string& keyword,
                               std::string* error) {
    if (!loaded_) {
        if (error != nullptr) {
            *error = "复习卡片尚未加载完成，请稍后再试";
        }
        return false;
    }

    std::vector<int> indices = CollectIndices(doc_ids, chapter, keyword);
    if (indices.empty()) {
        // 空状态：给出可用的替代建议，方便语音交互继续下去。
        std::string hint = "没有找到匹配的复习卡片";
        if (!doc_ids.empty()) {
            std::string names;
            for (size_t i = 0; i < doc_ids.size(); ++i) {
                int di = IndexOfDoc(doc_ids[i]);
                if (di >= 0) {
                    if (!names.empty()) {
                        names += "、";
                    }
                    names += docs_[di].name;
                }
            }
            if (!names.empty()) {
                hint = "《" + names + "》" + (chapter.empty() ? "" : " 的“" + chapter + "”") +
                       " 暂没有可复习的卡片";
            }
        }
        if (!chapter.empty() && doc_ids.size() == 1) {
            auto chapters = ChaptersOf(doc_ids[0]);
            if (!chapters.empty()) {
                hint += "。该法规可用章节有：";
                for (size_t i = 0; i < chapters.size() && i < 8; ++i) {
                    if (i > 0) {
                        hint += "、";
                    }
                    hint += chapters[i].first;
                }
            }
        } else if (!doc_ids.empty()) {
            hint += "。可以说“列出全部法规”查看可选范围";
        }
        if (error != nullptr) {
            *error = hint;
        }
        return false;
    }

    scope_ = std::move(indices);
    scope_pos_ = 0;
    mode_ = Mode::kScope;
    current_index_ = -1;
    return true;
}

void ReviewEngine::ClearScope() {
    scope_.clear();
    scope_pos_ = 0;
    if (mode_ == Mode::kScope) {
        mode_ = Mode::kAll;
    }
}

std::string ReviewEngine::ScopeDescription() const {
    if (mode_ != Mode::kScope) {
        return "";
    }
    // 汇总范围内涉及的法规名称。
    std::vector<std::string> doc_ids;
    for (int idx : scope_) {
        const std::string& d = cards_[idx].doc;
        bool exists = false;
        for (const auto& e : doc_ids) {
            if (e == d) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            doc_ids.push_back(d);
        }
    }
    std::string result;
    for (size_t i = 0; i < doc_ids.size() && i < 3; ++i) {
        int di = IndexOfDoc(doc_ids[i]);
        if (i > 0) {
            result += "、";
        }
        result += (di >= 0 ? docs_[di].name : doc_ids[i]);
    }
    if (doc_ids.size() > 3) {
        result += " 等 " + std::to_string(doc_ids.size()) + " 部法规";
    }
    result += "（" + std::to_string(scope_.size()) + " 张）";
    return result;
}

// ===== 抽卡 / 翻卡 =====

bool ReviewEngine::NextCard(bool random, const ReviewCard** card, int* index, int* total) {
    if (!loaded_ || cards_.empty()) {
        return false;
    }

    if (mode_ == Mode::kWrong) {
        // 错题回顾：顺序遍历错题队列。
        if (wrong_queue_.empty()) {
            return false;
        }
        if (wrong_queue_pos_ >= wrong_queue_.size()) {
            wrong_queue_pos_ = 0;
        }
        current_index_ = wrong_queue_[wrong_queue_pos_];
        if (index != nullptr) {
            *index = (int)wrong_queue_pos_;
        }
        if (total != nullptr) {
            *total = (int)wrong_queue_.size();
        }
        wrong_queue_pos_++;
    } else if (mode_ == Mode::kScope) {
        // 定向复习：在选定范围内顺序或随机取卡。
        if (scope_.empty()) {
            return false;
        }
        if (random) {
            int next = current_index_;
            if (scope_.size() == 1) {
                next = scope_[0];
            } else {
                do {
                    next = scope_[rand() % scope_.size()];
                } while (next == current_index_);
            }
            current_index_ = next;
            if (index != nullptr) {
                // 报位置时用范围内的序号，便于用户感知进度。
                for (size_t i = 0; i < scope_.size(); ++i) {
                    if (scope_[i] == current_index_) {
                        *index = (int)i;
                        break;
                    }
                }
            }
        } else {
            if (scope_pos_ >= scope_.size()) {
                scope_pos_ = 0;  // 一轮结束，回到开头
            }
            current_index_ = scope_[scope_pos_];
            if (index != nullptr) {
                *index = (int)scope_pos_;
            }
            scope_pos_++;
        }
        if (total != nullptr) {
            *total = (int)scope_.size();
        }
    } else {
        // 全量。
        int n = (int)cards_.size();
        if (random) {
            int next = current_index_;
            if (n == 1) {
                next = 0;
            } else {
                do {
                    next = rand() % n;
                } while (next == current_index_);
            }
            current_index_ = next;
        } else {
            current_index_ = (current_index_ + 1) % n;
        }
        if (index != nullptr) {
            *index = current_index_;
        }
        if (total != nullptr) {
            *total = n;
        }
    }

    if (current_index_ < 0 || current_index_ >= (int)cards_.size()) {
        return false;
    }
    if (card != nullptr) {
        *card = &cards_[current_index_];
    }
    // 抽到即视为已复习，并记录时间。
    TouchCard((size_t)current_index_);
    return true;
}

const ReviewCard* ReviewEngine::CurrentCard() const {
    if (!loaded_ || current_index_ < 0 || current_index_ >= (int)cards_.size()) {
        return nullptr;
    }
    return &cards_[current_index_];
}

void ReviewEngine::MarkWrong(const std::string& card_id, bool mark) {
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (cards_[i].id == card_id) {
            wrong_flags_[i] = mark;
            if (mark) {
                // 标记错题时同时认为这张卡被复习过。
                TouchCard(i);
            } else {
                SaveCardState(i);
            }
            break;
        }
    }
    RebuildWrongQueue();
}

bool ReviewEngine::StartWrongReview() {
    RebuildWrongQueue();
    if (wrong_queue_.empty()) {
        return false;
    }
    mode_ = Mode::kWrong;
    wrong_queue_pos_ = 0;
    return true;
}

void ReviewEngine::ExitWrongReview() {
    if (mode_ == Mode::kWrong) {
        mode_ = Mode::kAll;
    }
    wrong_queue_.clear();
    wrong_queue_pos_ = 0;
}

// ===== 进度与掌握情况 =====

ProgressStats ReviewEngine::StatsForIndices(const std::vector<int>& indices) const {
    ProgressStats s;
    s.total = (int)indices.size();
    if (s.total == 0) {
        return s;
    }
    s.valid = true;
    uint32_t newest = 0;
    for (int idx : indices) {
        if (idx < 0 || idx >= (int)cards_.size()) {
            continue;
        }
        bool seen = seen_flags_[idx];
        bool wrong = wrong_flags_[idx];
        if (seen) {
            s.reviewed++;
            if (!wrong) {
                s.mastered++;
            }
        } else {
            s.unreviewed++;
        }
        if (wrong) {
            s.wrong++;
        }
        if (last_time_[idx] > newest) {
            newest = last_time_[idx];
        }
    }
    s.last_review_time = newest;
    if (s.reviewed > 0) {
        int correct = s.reviewed - (s.wrong > s.reviewed ? s.reviewed : s.wrong);
        if (correct < 0) {
            correct = 0;
        }
        s.accuracy_percent = (int)((correct * 100) / s.reviewed);
    }
    return s;
}

ProgressStats ReviewEngine::StatsForDoc(const std::string& doc_id) const {
    std::vector<int> indices;
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (cards_[i].doc == doc_id) {
            indices.push_back((int)i);
        }
    }
    return StatsForIndices(indices);
}

ProgressStats ReviewEngine::StatsForScope() const {
    if (!loaded_) {
        return ProgressStats();
    }
    if (mode_ == Mode::kScope) {
        return StatsForIndices(scope_);
    }
    if (mode_ == Mode::kWrong) {
        return StatsForIndices(wrong_queue_);
    }
    std::vector<int> all;
    all.reserve(cards_.size());
    for (size_t i = 0; i < cards_.size(); ++i) {
        all.push_back((int)i);
    }
    return StatsForIndices(all);
}

std::string ReviewEngine::DescribeLastReview(uint32_t unix_time) const {
    if (unix_time == 0) {
        return "还没复习过";
    }
    if (unix_time < kMinValidUnixTime) {
        return "复习过（时间未同步）";
    }
    time_t now = time(nullptr);
    if (now <= 0 || (uint32_t)now < kMinValidUnixTime) {
        return "复习过（时间未同步）";
    }
    int64_t diff = (int64_t)now - (int64_t)unix_time;
    if (diff < 0) {
        diff = 0;
    }
    int64_t days = diff / 86400;
    if (days == 0) {
        int64_t hours = diff / 3600;
        if (hours == 0) {
            return "刚刚复习过";
        }
        return std::to_string((int)hours) + " 小时前复习过";
    }
    if (days == 1) {
        return "昨天复习过";
    }
    return std::to_string((int)days) + " 天前复习过";
}

std::string ReviewEngine::FormatStats(const ProgressStats& s, const std::string& label) const {
    if (!s.valid || s.total == 0) {
        return (label.empty() ? "当前范围" : label) + "没有可复习的卡片";
    }
    std::string out = label;
    if (!out.empty()) {
        out += "：";
    }
    out += "共 " + std::to_string(s.total) + " 张，";
    out += "已复习 " + std::to_string(s.reviewed) + " 张，";
    out += "未复习 " + std::to_string(s.unreviewed) + " 张，";
    out += "错题 " + std::to_string(s.wrong) + " 张";
    if (s.accuracy_percent >= 0) {
        out += "，正确率 " + std::to_string(s.accuracy_percent) + "%";
    } else {
        out += "，暂无正确率（还没有复习记录）";
    }
    out += "，" + DescribeLastReview(s.last_review_time);
    return out;
}

std::string ReviewEngine::FormatDocBrief(const DocInfo& d) const {
    std::string out = d.id + " " + d.name;
    if (!d.category.empty()) {
        out += "（" + d.category + "）";
    }
    ProgressStats s = StatsForDoc(d.id);
    out += "，共 " + std::to_string(s.total) + " 张";
    if (s.reviewed > 0) {
        out += "，已复习 " + std::to_string(s.reviewed) + " 张";
        if (s.accuracy_percent >= 0) {
            out += "，掌握率 " + std::to_string(s.accuracy_percent) + "%";
        }
    } else {
        out += "，未开始";
    }
    return out;
}

// ===== 持久化 =====

void ReviewEngine::TouchCard(size_t idx) {
    if (idx >= cards_.size()) {
        return;
    }
    bool changed = !seen_flags_[idx];
    seen_flags_[idx] = true;
    time_t now = time(nullptr);
    if (now > 0 && (uint32_t)now >= kMinValidUnixTime) {
        last_time_[idx] = (uint32_t)now;
        changed = true;
    }
    if (changed) {
        SaveCardState(idx);
    }
}

void ReviewEngine::SaveCardState(size_t index) {
    if (index >= cards_.size()) {
        return;
    }
    nvs_handle_t handle = 0;
    if (nvs_open("review", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for review progress");
        return;
    }
    const std::string& id = cards_[index].id;
    // 键长上限 15 字节，卡片 id 最长 7，加前缀后仍安全。
    nvs_set_u8(handle, id.c_str(), wrong_flags_[index] ? 1 : 0);          // 错题（兼容旧键）
    nvs_set_u8(handle, ("s:" + id).c_str(), seen_flags_[index] ? 1 : 0);  // 已复习
    nvs_set_u32(handle, ("t:" + id).c_str(), last_time_[index]);          // 最近复习时间
    nvs_commit(handle);
    nvs_close(handle);
}

void ReviewEngine::LoadProgress() {
    const size_t n = cards_.size();
    wrong_flags_.assign(n, false);
    seen_flags_.assign(n, false);
    last_time_.assign(n, 0);

    nvs_handle_t handle = 0;
    if (nvs_open("review", NVS_READONLY, &handle) != ESP_OK) {
        RebuildWrongQueue();
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        uint8_t flag = 0;
        if (nvs_get_u8(handle, cards_[i].id.c_str(), &flag) == ESP_OK && flag != 0) {
            wrong_flags_[i] = true;
        }
        flag = 0;
        if (nvs_get_u8(handle, ("s:" + cards_[i].id).c_str(), &flag) == ESP_OK && flag != 0) {
            seen_flags_[i] = true;
        }
        uint32_t t = 0;
        if (nvs_get_u32(handle, ("t:" + cards_[i].id).c_str(), &t) == ESP_OK) {
            last_time_[i] = t;
        }
    }
    nvs_close(handle);
    RebuildWrongQueue();
}

void ReviewEngine::ResetAllProgress() {
    for (size_t i = 0; i < cards_.size(); ++i) {
        wrong_flags_[i] = false;
        seen_flags_[i] = false;
        last_time_[i] = 0;
    }
    nvs_handle_t handle = 0;
    if (nvs_open("review", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    RebuildWrongQueue();
}

void ReviewEngine::RebuildWrongQueue() {
    wrong_queue_.clear();
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (wrong_flags_[i]) {
            wrong_queue_.push_back((int)i);
        }
    }
}
