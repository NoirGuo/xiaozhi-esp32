#ifndef REVIEW_ENGINE_H
#define REVIEW_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// 前向声明：避免在头文件里展开整个 cJSON。
struct cJSON;

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

// 法规（文档）元信息，对应 cards.json 的 docs[] 一项。
// category / tags 用于“按分类或标签筛选”的法规选择入口。
struct DocInfo {
    std::string id;                     // "AQF"
    std::string name;                   // 中文全名
    std::string category;               // 法律 / 行政法规 / 部门规范性文件 / 行业标准
    std::vector<std::string> tags;      // 主题标签
    std::vector<std::string> chapters;  // 该法规下的章节（按卡片出现顺序去重）
    int card_count = 0;                 // 该法规的卡片数
};

// 一组卡片的复习进度与掌握情况。
struct ProgressStats {
    bool valid = false;         // 范围是否有效（有卡片）
    int total = 0;              // 范围内卡片总数
    int reviewed = 0;           // 已复习（至少抽到过一次）
    int unreviewed = 0;         // 未复习
    int wrong = 0;              // 标记为错题（未掌握）
    int mastered = 0;           // 已复习且未标错
    int accuracy_percent = -1;  // 正确率，-1 表示样本不足
    uint32_t last_review_time = 0;  // 最近复习时间（unix 秒），0 表示无记录
};

// 本地复习引擎：解析编译进固件的 cards.json，提供
//   1) 全量 / 随机 / 错题回顾
//   2) 定向复习：按法规（可细化到章节、关键字）选定范围
//   3) 进度与掌握情况统计
// 错题与复习状态用 NVS 持久化，断电可恢复。
//
// 说明：本引擎不直接操作屏幕与 TTS。它只负责“数据与状态”，
// 由 board 里的 MCP 工具把结果写屏、把 recite 文本交给云端 TTS 朗读。
class ReviewEngine {
public:
    static ReviewEngine& GetInstance();

    // 用嵌入的 JSON 文本初始化（必须在应用启动早期调用一次）。
    bool Initialize(const char* json_text, size_t json_length);

    bool IsLoaded() const { return loaded_; }
    size_t CardCount() const { return cards_.size(); }
    // 返回本次“错题回顾”队列中待复习的数量（-1 表示未初始化）。
    int WrongQueueSize() const { return loaded_ ? (int)wrong_queue_.size() : -1; }

    // 抽卡 / 翻卡。返回 true 表示成功，*card 指向当前卡片。
    // 自动跟随当前范围：定向范围 > 错题回顾 > 全量。
    bool NextCard(bool random, const ReviewCard** card, int* index, int* total);

    // 读取当前卡片（不推进）。
    const ReviewCard* CurrentCard() const;

    // 错题标记：mark=true 标记为错题，false 表示“我会了”清除错题标记。
    void MarkWrong(const std::string& card_id, bool mark);

    // 进入“错题回顾”：只遍历被标记为错题的卡片。返回 false 表示当前没有错题
    // （此时不会改变范围，避免静默退回全量抽卡）。
    bool StartWrongReview();
    void ExitWrongReview();
    bool InWrongMode() const { return mode_ == Mode::kWrong; }

    // ===== 定向复习：法规选择入口 =====

    // 全部法规（按卡片数从多到少）。
    const std::vector<DocInfo>& Docs() const { return docs_; }

    // 全部分类 / 全部标签（去重），供语音引导用户筛选。
    std::vector<std::string> Categories() const;
    std::vector<std::string> Tags() const;

    // 按名称关键字、分类、标签搜索筛选法规。三个条件取交集，空条件表示不限制。
    std::vector<DocInfo> SearchDocs(const std::string& keyword,
                                    const std::string& category,
                                    const std::string& tag) const;

    // 某法规的章节列表（章节名 -> 卡片数）。
    std::vector<std::pair<std::string, int>> ChaptersOf(const std::string& doc_id) const;

    // 把用户的说法（法规 id 如 "AQF"，或名称关键字如 "安全生产法"）解析为 doc id 列表。
    // 优先精确匹配 id，其次按名称子串匹配（可能命中多部）。
    std::vector<std::string> ResolveDocIds(const std::string& input) const;

    // 选定复习范围。
    //   doc_ids 为空表示不限法规；多个元素表示多选（并集）。
    //   chapter 为章节关键字（子串匹配），空表示不限章节。
    //   keyword 为内容关键字（匹配标题/要点/口诀/考点/口播稿），空表示不限。
    // 命中卡片会进入定向范围。返回 false 时 *error 给出空状态提示（含可用章节建议）。
    bool SelectScope(const std::vector<std::string>& doc_ids,
                     const std::string& chapter,
                     const std::string& keyword,
                     std::string* error);

    // 退出定向复习，回到全量（不改变错题模式）。
    void ClearScope();

    bool HasScope() const { return mode_ == Mode::kScope; }
    int ScopeSize() const { return (int)scope_.size(); }
    // 当前范围的文字描述，如“《安全生产法》· 总则（5 张）”。
    std::string ScopeDescription() const;

    // ===== 进度与掌握情况 =====

    ProgressStats StatsForDoc(const std::string& doc_id) const;
    // 当前生效范围（定向 > 错题 > 全量）的统计。
    ProgressStats StatsForScope() const;
    // 把统计格式化为适合 TTS 朗读的一行中文。
    std::string FormatStats(const ProgressStats& s, const std::string& label) const;
    // 把法规格式化为一行摘要（含卡片数与掌握率），供选择列表朗读。
    std::string FormatDocBrief(const DocInfo& d) const;
    // “最近复习”的人类可读描述。
    std::string DescribeLastReview(uint32_t unix_time) const;

    // 清空全部进度（可选，供调试）。
    void ResetAllProgress();

private:
    // 当前生效的复习范围模式，优先级：kScope > kWrong > kAll。
    enum class Mode { kAll, kWrong, kScope };

    ReviewEngine() = default;
    ~ReviewEngine() = default;
    ReviewEngine(const ReviewEngine&) = delete;
    ReviewEngine& operator=(const ReviewEngine&) = delete;

    bool ParseJson(const char* text, size_t length);
    void ParseDocs(const cJSON* root);
    void BuildDocIndex();
    void LoadProgress();
    void SaveCardState(size_t index);
    void RebuildWrongQueue();

    // 供 NextCard 使用：把 idx 记为已复习并刷新时间。
    void TouchCard(size_t idx);

    ProgressStats StatsForIndices(const std::vector<int>& indices) const;
    // 按条件收集卡片下标。
    std::vector<int> CollectIndices(const std::vector<std::string>& doc_ids,
                                    const std::string& chapter,
                                    const std::string& keyword) const;
    int IndexOfDoc(const std::string& doc_id) const;

    bool loaded_ = false;
    std::vector<ReviewCard> cards_;
    std::vector<DocInfo> docs_;
    int current_index_ = -1;

    Mode mode_ = Mode::kAll;
    std::vector<int> scope_;              // 定向范围的卡片下标
    size_t scope_pos_ = 0;
    std::vector<int> wrong_queue_;        // 错题卡片在 cards_ 中的下标
    size_t wrong_queue_pos_ = 0;

    std::vector<bool> wrong_flags_;       // 与 cards_ 对齐：错题（未掌握）标记
    std::vector<bool> seen_flags_;        // 与 cards_ 对齐：是否已复习过
    std::vector<uint32_t> last_time_;     // 与 cards_ 对齐：最近复习时间（unix 秒）
};

#endif  // REVIEW_ENGINE_H
