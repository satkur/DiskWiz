#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <exception>
#include <iomanip>
#include <cwctype>
#include <future>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// 除外パスは変更なし
static const std::vector<std::wstring> EXCLUDED_PATHS = {
    L"C:\\Windows",
    //L"C:\\Program Files",
    //L"C:\\Program Files (x86)",
    L"C:\\ProgramData",
    L"C:\\$Recycle.Bin",
    L"C:\\System Volume Information",
    L"C:\\Recovery",
    L"C:\\pagefile.sys",
    L"C:\\hiberfil.sys",
};

// 結果格納用構造体
struct PathSizeInfo {
    fs::path path;
    std::uintmax_t size;
    bool calculated;
    bool isPartial;
    std::chrono::milliseconds elapsed;

    PathSizeInfo()
        : path(), size(0), calculated(false), isPartial(false), elapsed(0) {}

    PathSizeInfo(const fs::path& p, std::uintmax_t s, bool c)
        : path(p), size(s), calculated(c), isPartial(false), elapsed(0) {}
};

// ResultManagerクラス
class ResultManager {
private:
    std::vector<PathSizeInfo> results;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<size_t> completedCount{ 0 };  // 完了数のカウント用

public:
    void update(const fs::path& path, std::uintmax_t size, bool partial,
                std::chrono::milliseconds elapsedTime) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = std::find_if(results.begin(), results.end(),
                               [&path](const PathSizeInfo& info) { return info.path == path; });
        if (it != results.end() && !it->calculated) {
            it->size = size;
            it->calculated = true;
            it->isPartial = partial;
            it->elapsed = elapsedTime;
            completedCount++;
        }
        cv.notify_all();
    }

    void addTarget(const fs::path& path) {
        std::lock_guard<std::mutex> lock(mutex);
        results.emplace_back(path, 0, false);
    }

    std::vector<PathSizeInfo> getTopN(size_t n) const {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<PathSizeInfo> sorted = results;
        std::sort(sorted.begin(), sorted.end(),
                  [](const PathSizeInfo& a, const PathSizeInfo& b) {
                      return a.size > b.size;
                  });
        if (sorted.size() > n) {
            sorted.resize(n);
        }
        return sorted;
    }

    bool isComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return std::all_of(results.begin(), results.end(),
                           [](const PathSizeInfo& info) { return info.calculated; });
    }

    size_t totalTargets() const {
        std::lock_guard<std::mutex> lock(mutex);
        return results.size();
    }

    size_t completedTargets() const {
        return completedCount;
    }
};

// ユーティリティ関数
double toGB(std::uintmax_t bytes) {
    static const double GB = 1024.0 * 1024.0 * 1024.0;
    return static_cast<double>(bytes) / GB;
}

// 集計単位の判定用関数
bool isTargetUnit(const fs::path& path, int depth) {
    try {
        // シンボリックリンクは集計対象外
        if (fs::is_symlink(path)) {
            return false;
        }

        if (depth == 0) {
            // ルート直下のファイルは集計単位
            return fs::is_regular_file(path);
        }

        // パスの階層数をカウント
        int pathDepth = 0;
        for (const auto& _ : path.relative_path()) {
            pathDepth++;
        }

        // 指定された深さと一致する場合、または
        // ファイルが存在する最深の階層の場合に集計単位とする
        return pathDepth == depth ||
            (pathDepth < depth && fs::is_regular_file(path));
    } catch (...) {
        return false;
    }
}

bool isExcludedPath(const fs::path& p) {
    try {
        std::wstring pathW = p.lexically_normal().wstring();
        std::transform(pathW.begin(), pathW.end(), pathW.begin(), ::towlower);
        for (const auto& ex : EXCLUDED_PATHS) {
            std::wstring exW = ex;
            std::transform(exW.begin(), exW.end(), exW.begin(), ::towlower);
            if (pathW.rfind(exW, 0) == 0) {
                return true;
            }
        }
    } catch (...) {
        return true;
    }
    return false;
}

// ディレクトリサイズ計算関数（再帰）
std::pair<std::uintmax_t, bool> calculateDirectorySizeWithTimeout(
    const fs::path& dir,
    const std::chrono::steady_clock::time_point& startTime,
    const ResultManager& manager
) {
    std::uintmax_t total = 0;
    const auto timeLimit = std::chrono::minutes(1);
    bool isPartial = false;

    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            // シンボリックリンクをスキップ
            if (fs::is_symlink(entry)) {
                continue;
            }

            // 時間制限チェック
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed >= timeLimit) {
                auto currentTop = manager.getTopN(1);
                if (manager.completedTargets() == manager.totalTargets() - 1 &&
                    (currentTop.empty() || total > currentTop[0].size)) {
                    isPartial = true;
                    break;
                }
            }

            try {
                if (fs::is_directory(entry)) {
                    auto [size, partial] = calculateDirectorySizeWithTimeout(entry, startTime, manager);
                    total += size;
                    isPartial |= partial;
                } else if (fs::is_regular_file(entry)) {
                    total += fs::file_size(entry);
                }
            } catch (...) {}
        }
    } catch (...) {}

    return { total, isPartial };
}

// 集計対象パス収集関数
void collectTargetPaths(const fs::path& root, int currentDepth, int maxDepth,
                        ResultManager& manager) {
    try {
        // 除外パスと深さの制限のみをチェック
        if (isExcludedPath(root) || currentDepth > maxDepth) {
            return;
        }

        // 集計単位の判定（シンボリックリンクのチェックを含む）
        if (isTargetUnit(root, maxDepth)) {
            manager.addTarget(root);
        }

        // ディレクトリの場合は再帰
        if (fs::is_directory(root) && currentDepth < maxDepth) {
            for (const auto& entry : fs::directory_iterator(root)) {
                collectTargetPaths(entry.path(), currentDepth + 1, maxDepth, manager);
            }
        }
    } catch (...) {}
}

// カーソル制御用の関数を追加
void moveCursorToTop() {
    std::cout << "\033[H"; // カーソルを画面の先頭に移動
}

void clearToEndOfLine() {
    std::cout << "\033[K"; // カーソルから行末までクリア
}

// 結果表示関数を修正
void displayResults(const ResultManager& manager, size_t limit) {
    moveCursorToTop();

    // 進捗表示
    size_t completed = manager.completedTargets();
    size_t total = manager.totalTargets();
    std::cout << "Progress: " << completed << "/" << total
        << " (" << (total > 0 ? (completed * 100 / total) : 0) << "%)\n\n";
    clearToEndOfLine();

    // ランキング表示
    std::cout << "=== Top " << limit << " Largest Files/Folders ===\n";
    clearToEndOfLine();

    for (size_t i = 0; i < limit; ++i) {
        auto results = manager.getTopN(limit);
        if (i < results.size()) {
            const auto& info = results[i];
            if (info.calculated) {
                std::cout << (i + 1) << ". " << info.path.string()
                    << " : " << std::fixed << std::setprecision(2)
                    << toGB(info.size) << " GB"
                    << (info.isPartial ? "+" : "")
                    << " (" << info.elapsed.count() / 1000.0 << " sec)";
            } else {
                std::cout << (i + 1) << ". " << info.path.string()
                    << " : calculating...";
            }
        }
        std::cout << "\n";
        clearToEndOfLine();
    }
}

int main() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
#endif

    std::cout.setf(std::ios::unitbuf);
    const int MAX_DEPTH = 3;
    const size_t DISPLAY_LIMIT = 16;
    const int DISPLAY_FPS = 2;
    const auto DISPLAY_INTERVAL = std::chrono::milliseconds(1000 / DISPLAY_FPS);

    ResultManager manager;

    // Phase 1: 集計対象の収集
    std::cout << "Collecting target paths...\n";
    collectTargetPaths(L"C:\\", 0, MAX_DEPTH, manager);

    // Phase 2: 並列サイズ計算
    std::vector<std::future<void>> calculationTasks;
    auto results = manager.getTopN(manager.totalTargets());  // 全ターゲットを取得

    for (const auto& target : results) {
        calculationTasks.push_back(std::async(std::launch::async,
            [&manager](const fs::path& path) {
                auto startTime = std::chrono::steady_clock::now();
                std::uintmax_t size;
                bool isPartial = false;
                try {
                    if (fs::is_directory(path)) {
                        auto [dirSize, partial] = calculateDirectorySizeWithTimeout(path, startTime, manager);
                        size = dirSize;
                        isPartial = partial;
                    } else {
                        size = fs::file_size(path);
                    }
                } catch (...) {
                    size = 0;
                }
                auto endTime = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTime);
                manager.update(path, size, isPartial, elapsed);
            }, target.path
        ));
    }

    // Phase 3: 結果表示ループ
    auto lastUpdate = std::chrono::steady_clock::now();
    while (!manager.isComplete()) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastUpdate >= DISPLAY_INTERVAL) {
            displayResults(manager, DISPLAY_LIMIT);
            lastUpdate = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 最終結果表示
    displayResults(manager, DISPLAY_LIMIT);
    std::cout << "\nAnalysis complete!\n";

    // 全タスクの完了を待機
    for (auto& task : calculationTasks) {
        task.wait();
    }

    return 0;
}