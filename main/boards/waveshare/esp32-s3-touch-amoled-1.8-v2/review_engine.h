#ifndef REVIEW_ENGINE_H
#define REVIEW_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 单张复习卡片的结构，对应 cards.json 中 cards[] 数组的一项。
struct ReviewCard {
    std::string id;          // 如 "AQF-01"
    std::string doc;         // 文件缩写，如 "AQF"
    std::string chapter;     // 章节
    std::string title;       // 卡片标题
    std::vector<std::string> points;    // 要点（屏幕显示，考试采分点）
    std::string mnemonic;    // 记忆口诀
    std::string exam_point;  // 考点提示
    std::string recite;      // 口播稿（交给在线 TTS 朗读）
};

// 本地复习引擎：解析编译进固件的 cards.json，提供抽卡 / 翻卡 / 随机 /
// 错题标记与回顾，错题状态用 NVS 持久化，断电可恢复。
//
// 说明：本引擎不直接操作屏幕与 TTS。它只负责“数据与状态”，
// 由 board 里的 MCP 工具把结果写屏、把 recite 文本交给云端 TTS 朗读。
class ReviewEngine {
public:
    static ReviewEngine& GetInstance();

    // 用嵌入的 JSON 文本初始化（必须在应用启动早期调用一次）。
    // 返回 true 表示解析成功。
    bool Initialize(const char* json_text, size_t json_length);

    bool IsLoaded() const { return loaded_; }
    size_t CardCount() const { return cards_.size(); }
    // 返回本次“错题回顾”队列中待复习的数量（-1 表示未初始化）。
    int WrongQueueSize() const { return loaded_ ? (int)wrong_queue_.size() : -1; }

    // 抽卡 / 翻卡。返回 true 表示成功，*card 指向当前卡片。
    // 顺序模式：current_index_ 递增；随机模式：随机抽取不重复。
    bool NextCard(bool random, const ReviewCard** card, int* index, int* total);

    // 读取当前卡片（不推进）。
    const ReviewCard* CurrentCard() const;

    // 错题标记：mark=true 标记为错题，false 表示“我会了”清除错题标记。
    void MarkWrong(const std::string& card_id, bool mark);

    // 进入“错题回顾”：只遍历被标记为错题的卡片。
    void StartWrongReview();
    void ExitWrongReview() { wrong_mode_ = false; wrong_queue_.clear(); }
    bool InWrongMode() const { return wrong_mode_; }

    // 清空全部进度（可选，供调试）。
    void ResetAllProgress();

private:
    ReviewEngine() = default;
    ~ReviewEngine() = default;
    ReviewEngine(const ReviewEngine&) = delete;
    ReviewEngine& operator=(const ReviewEngine&) = delete;

    bool ParseJson(const char* text, size_t length);
    void LoadWrongFlags();
    void SaveWrongFlag(const std::string& card_id, bool mark);
    void RebuildWrongQueue();

    bool loaded_ = false;
    std::vector<ReviewCard> cards_;
    int current_index_ = -1;
    bool random_mode_ = false;
    bool wrong_mode_ = false;
    std::vector<int> wrong_queue_;       // 错题卡片在 cards_ 中的下标
    size_t wrong_queue_pos_ = 0;
    std::vector<bool> wrong_flags_;      // 与 cards_ 对齐的错题标记
};

#endif  // REVIEW_ENGINE_H
