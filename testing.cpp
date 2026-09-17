// testing.cpp — стенд для массового прогона cache_sim.
//
// Что делает программа:
//   1) генерирует наборы входных данных трёх групп (цикл / "горячие и
//      редкие" элементы / сбалансированная смесь);
//   2) перебирает конфигурации кеша: 1, 2 и 3 уровня со всеми возможными
//      сочетаниями алгоритмов вытеснения на уровнях; ёмкости уровней
//      зафиксированы — L1 = 10, L2 = 20, L3 = 40;
//   3) на каждый прогон переписывает config.txt по формату из README.md,
//      запускает cache_sim, подавая данные на stdin, и разбирает вывод;
//   4) складывает все результаты в CSV, после чего восстанавливает
//      исходный config.txt.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra -o testing testing.cpp
// Запуск: ./testing [каталог_результатов]   (по умолчанию test_results)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>   // mkdir
#include <unistd.h>     // unlink, rmdir

#include "MyCppLibs/sassert.h"

// ===================== общие утилиты =====================

// Универсальное приведение любого значения к тексту через поток вывода.
template <typename ValueType>
std::string ToText(const ValueType& Value) {
    std::ostringstream Stream;
    Stream << Value;
    return Stream.str();
}

// Дробные числа в текстовых отчётах — всегда с двумя знаками после точки.
inline std::string ToText(double Value) {
    std::ostringstream Stream;
    Stream << std::fixed << std::setprecision(2) << Value;
    return Stream.str();
}

template <typename ValueType>
std::string JoinWith(const std::vector<ValueType>& Values, std::string_view Separator) {
    std::string Result;
    for (std::size_t Index = 0; Index < Values.size(); ++Index) {
        if (Index != 0) {
            Result += Separator;
        }
        Result += ToText(Values[Index]);
    }
    return Result;
}

// std::setw считает байты, а символ кириллицы в UTF-8 занимает их два,
// поэтому колонки с русским текстом выравниваем по числу символов сами.
std::size_t DisplayWidth(const std::string& Text) {
    std::size_t Width = 0;
    for (unsigned char Character : Text) {
        if ((Character & 0xC0u) != 0x80u) {   // все байты, кроме продолжающих
            ++Width;
        }
    }
    return Width;
}

std::string PadRight(const std::string& Text, std::size_t Width) {
    const std::size_t Current = DisplayWidth(Text);
    return (Current >= Width) ? Text : Text + std::string(Width - Current, ' ');
}

// Доля в процентах с защитой от деления на ноль.
inline double PercentOf(std::size_t Part, std::size_t Whole) {
    return (Whole == 0) ? 0.0 : (100.0 * static_cast<double>(Part) / static_cast<double>(Whole));
}

// Выбор значения по весам — используется генераторами данных, чтобы
// смешивать обращения разных сортов в заданной пропорции.
template <typename ValueType>
class TWeightedPicker {
public:
    void Add(const ValueType& Value, double Weight) {
        CHECK_EX(Weight > 0.0, std::invalid_argument,
                 "TWeightedPicker: вес варианта должен быть положительным.");
        TotalWeight_ += Weight;
        Values_.push_back(Value);
        CumulativeWeights_.push_back(TotalWeight_);
    }

    const ValueType& Pick(std::mt19937& Engine) const {
        CHECK(!Values_.empty(), "TWeightedPicker: нет ни одного варианта для выбора.");

        std::uniform_real_distribution<double> Distribution(0.0, TotalWeight_);
        const double Point = Distribution(Engine);

        const auto Found = std::lower_bound(CumulativeWeights_.begin(), CumulativeWeights_.end(), Point);
        const std::size_t Index = static_cast<std::size_t>(Found - CumulativeWeights_.begin());

        return Values_[std::min(Index, Values_.size() - 1)];
    }

private:
    std::vector<ValueType> Values_;
    std::vector<double>    CumulativeWeights_;
    double                 TotalWeight_ = 0.0;
};

inline void MakeDirectory(const std::string& Path) {
    const int Status = ::mkdir(Path.c_str(), 0755);
    CHECK(Status == 0 || errno == EEXIST, "Не удалось создать каталог '" + Path + "'.");
}

// ===================== генерация входных данных =====================

// Один сгенерированный поток обращений плюс его "паспорт" для отчёта.
struct SDataStream {
    std::string      GroupName;
    std::string      StreamName;
    std::string      Description;
    std::vector<int> Values;

    std::size_t Length() const { return Values.size(); }

    std::size_t UniqueKeyCount() const {
        std::vector<int> Sorted = Values;
        std::sort(Sorted.begin(), Sorted.end());
        Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());
        return Sorted.size();
    }

    // Формат stdin для cache_sim (README.md): M, затем M обращений.
    std::string ToSimulatorInput() const {
        std::ostringstream Stream;
        Stream << Values.size() << '\n';
        for (std::size_t Index = 0; Index < Values.size(); ++Index) {
            Stream << Values[Index] << (Index + 1 == Values.size() ? '\n' : ' ');
        }
        return Stream.str();
    }
};

// Диапазоны ключей разнесены, чтобы "горячие", "редкие" и фоновые
// обращения нельзя было перепутать при разборе результатов.
constexpr int HotKeyBase    = 1;
constexpr int RareKeyBase   = 1001;
constexpr int MixHotKeyBase = 1001;
constexpr int OtherKeyBase  = 100001;
constexpr int CycleKeyBase  = 1;

// Группа 1: строгий проход по циклу 1..CycleLength, повторённый много раз.
SDataStream MakeCyclicStream(const std::string& StreamName, std::size_t CycleLength,
                             std::size_t TotalAccesses) {
    CHECK_EX(CycleLength != 0 && TotalAccesses != 0, std::invalid_argument,
             "MakeCyclicStream: длина цикла и число обращений должны быть ненулевыми.");

    SDataStream Stream;
    Stream.GroupName   = "Cycle";
    Stream.StreamName  = StreamName;
    Stream.Description = "цикл 1.." + ToText(CycleLength) + ", повторён " +
                         ToText(TotalAccesses / CycleLength) + " раз";

    Stream.Values.reserve(TotalAccesses);
    for (std::size_t Index = 0; Index < TotalAccesses; ++Index) {
        Stream.Values.push_back(CycleKeyBase + static_cast<int>(Index % CycleLength));
    }
    return Stream;
}

struct SHotColdParams {
    std::size_t HotCount    = 4;      // часто повторяющиеся элементы
    double      HotShare    = 0.60;
    std::size_t RareCount   = 12;     // редко повторяющиеся элементы
    double      RareShare   = 0.08;
    std::size_t OtherRange  = 500;    // фоновые "прочие" обращения
    std::uint32_t Seed      = 20260915u;
};

// Группа 2: несколько часто повторяющихся и несколько редко повторяющихся
// элементов вперемешку с прочими обращениями.
SDataStream MakeHotColdStream(const std::string& StreamName, const SHotColdParams& Params,
                              std::size_t TotalAccesses) {
    CHECK_EX(Params.HotCount != 0 && Params.RareCount != 0 && Params.OtherRange != 0,
             std::invalid_argument, "MakeHotColdStream: все размеры множеств ключей должны быть ненулевыми.");
    CHECK_EX(Params.HotShare + Params.RareShare < 1.0, std::invalid_argument,
             "MakeHotColdStream: на прочие обращения не осталось доли.");

    enum class EAccessKind { Hot, Rare, Other };

    TWeightedPicker<EAccessKind> KindPicker;
    KindPicker.Add(EAccessKind::Hot,   Params.HotShare);
    KindPicker.Add(EAccessKind::Rare,  Params.RareShare);
    KindPicker.Add(EAccessKind::Other, 1.0 - Params.HotShare - Params.RareShare);

    std::mt19937 Engine(Params.Seed);
    std::uniform_int_distribution<int> HotDistribution(0, static_cast<int>(Params.HotCount) - 1);
    std::uniform_int_distribution<int> RareDistribution(0, static_cast<int>(Params.RareCount) - 1);
    std::uniform_int_distribution<int> OtherDistribution(0, static_cast<int>(Params.OtherRange) - 1);

    SDataStream Stream;
    Stream.GroupName   = "HotCold";
    Stream.StreamName  = StreamName;
    Stream.Description = ToText(Params.HotCount) + " частых (" + ToText(100.0 * Params.HotShare) +
                         "%), " + ToText(Params.RareCount) + " редких (" +
                         ToText(100.0 * Params.RareShare) + "%), фон из " +
                         ToText(Params.OtherRange) + " ключей";

    Stream.Values.reserve(TotalAccesses);
    for (std::size_t Index = 0; Index < TotalAccesses; ++Index) {
        switch (KindPicker.Pick(Engine)) {
            case EAccessKind::Hot:
                Stream.Values.push_back(HotKeyBase + HotDistribution(Engine));
                break;
            case EAccessKind::Rare:
                Stream.Values.push_back(RareKeyBase + RareDistribution(Engine));
                break;
            case EAccessKind::Other:
                Stream.Values.push_back(OtherKeyBase + OtherDistribution(Engine));
                break;
        }
    }
    return Stream;
}

struct SBalancedParams {
    std::size_t CycleLength = 30;
    double      CycleShare  = 0.50;
    std::size_t HotCount    = 6;
    double      HotShare    = 0.30;
    std::size_t OtherRange  = 800;
    std::uint32_t Seed      = 987654321u;
};

// Группа 3: сбалансированная смесь — часть обращений продолжает цикл,
// часть бьёт по горячим ключам, остальное приходится на фон.
SDataStream MakeBalancedStream(const std::string& StreamName, const SBalancedParams& Params,
                               std::size_t TotalAccesses) {
    CHECK_EX(Params.CycleLength != 0 && Params.HotCount != 0 && Params.OtherRange != 0,
             std::invalid_argument, "MakeBalancedStream: все размеры множеств ключей должны быть ненулевыми.");
    CHECK_EX(Params.CycleShare + Params.HotShare < 1.0, std::invalid_argument,
             "MakeBalancedStream: на прочие обращения не осталось доли.");

    enum class EAccessKind { CycleStep, Hot, Other };

    TWeightedPicker<EAccessKind> KindPicker;
    KindPicker.Add(EAccessKind::CycleStep, Params.CycleShare);
    KindPicker.Add(EAccessKind::Hot,       Params.HotShare);
    KindPicker.Add(EAccessKind::Other,     1.0 - Params.CycleShare - Params.HotShare);

    std::mt19937 Engine(Params.Seed);
    std::uniform_int_distribution<int> HotDistribution(0, static_cast<int>(Params.HotCount) - 1);
    std::uniform_int_distribution<int> OtherDistribution(0, static_cast<int>(Params.OtherRange) - 1);

    SDataStream Stream;
    Stream.GroupName   = "Mixed";
    Stream.StreamName  = StreamName;
    Stream.Description = "цикл 1.." + ToText(Params.CycleLength) + " (" +
                         ToText(100.0 * Params.CycleShare) + "%) + " + ToText(Params.HotCount) +
                         " горячих (" + ToText(100.0 * Params.HotShare) + "%) + фон";

    std::size_t CycleCursor = 0;
    Stream.Values.reserve(TotalAccesses);
    for (std::size_t Index = 0; Index < TotalAccesses; ++Index) {
        switch (KindPicker.Pick(Engine)) {
            case EAccessKind::CycleStep:
                Stream.Values.push_back(CycleKeyBase + static_cast<int>(CycleCursor % Params.CycleLength));
                ++CycleCursor;
                break;
            case EAccessKind::Hot:
                Stream.Values.push_back(MixHotKeyBase + HotDistribution(Engine));
                break;
            case EAccessKind::Other:
                Stream.Values.push_back(OtherKeyBase + OtherDistribution(Engine));
                break;
        }
    }
    return Stream;
}

constexpr std::size_t AccessesPerStream = 1000;

// Все девять потоков: по три на каждую группу наполнения.
std::vector<SDataStream> BuildAllDataStreams() {
    std::vector<SDataStream> Streams;

    Streams.push_back(MakeCyclicStream("Cycle_K8",   8,   AccessesPerStream));
    Streams.push_back(MakeCyclicStream("Cycle_K25",  25,  AccessesPerStream));
    Streams.push_back(MakeCyclicStream("Cycle_K100", 100, AccessesPerStream));

    SHotColdParams SharpHotCold;
    SharpHotCold.HotCount = 4;   SharpHotCold.HotShare = 0.60;
    SharpHotCold.RareCount = 12; SharpHotCold.RareShare = 0.08;
    SharpHotCold.OtherRange = 500; SharpHotCold.Seed = 11111u;
    Streams.push_back(MakeHotColdStream("Hot4_60", SharpHotCold, AccessesPerStream));

    SHotColdParams MediumHotCold;
    MediumHotCold.HotCount = 10;  MediumHotCold.HotShare = 0.45;
    MediumHotCold.RareCount = 20; MediumHotCold.RareShare = 0.05;
    MediumHotCold.OtherRange = 800; MediumHotCold.Seed = 22222u;
    Streams.push_back(MakeHotColdStream("Hot10_45", MediumHotCold, AccessesPerStream));

    SHotColdParams WideHotCold;
    WideHotCold.HotCount = 25;  WideHotCold.HotShare = 0.35;
    WideHotCold.RareCount = 30; WideHotCold.RareShare = 0.05;
    WideHotCold.OtherRange = 1500; WideHotCold.Seed = 33333u;
    Streams.push_back(MakeHotColdStream("Hot25_35", WideHotCold, AccessesPerStream));

    SBalancedParams CycleHeavy;
    CycleHeavy.CycleLength = 30; CycleHeavy.CycleShare = 0.60;
    CycleHeavy.HotCount = 6;     CycleHeavy.HotShare = 0.25;
    CycleHeavy.OtherRange = 800; CycleHeavy.Seed = 44444u;
    Streams.push_back(MakeBalancedStream("Mix_CycleHeavy", CycleHeavy, AccessesPerStream));

    SBalancedParams EvenMix;
    EvenMix.CycleLength = 50; EvenMix.CycleShare = 0.40;
    EvenMix.HotCount = 8;     EvenMix.HotShare = 0.40;
    EvenMix.OtherRange = 1000; EvenMix.Seed = 55555u;
    Streams.push_back(MakeBalancedStream("Mix_Even", EvenMix, AccessesPerStream));

    SBalancedParams HotHeavy;
    HotHeavy.CycleLength = 80; HotHeavy.CycleShare = 0.25;
    HotHeavy.HotCount = 5;     HotHeavy.HotShare = 0.55;
    HotHeavy.OtherRange = 1200; HotHeavy.Seed = 66666u;
    Streams.push_back(MakeBalancedStream("Mix_HotHeavy", HotHeavy, AccessesPerStream));

    return Streams;
}

// ===================== конфигурации кеша =====================

struct SLevelSpec {
    std::size_t Capacity  = 0;
    std::string Algorithm;
};

struct SCacheConfig {
    std::vector<SLevelSpec> Levels;

    std::size_t LevelCount() const { return Levels.size(); }

    std::size_t TotalCapacity() const {
        std::size_t Total = 0;
        for (const SLevelSpec& Level : Levels) {
            Total += Level.Capacity;
        }
        return Total;
    }

    // Человекочитаемое имя конфигурации: "40 LRU | 40 LFU".
    std::string Describe() const {
        std::vector<std::string> Parts;
        Parts.reserve(Levels.size());
        for (const SLevelSpec& Level : Levels) {
            Parts.push_back(ToText(Level.Capacity) + " " + Level.Algorithm);
        }
        return JoinWith(Parts, " | ");
    }

    // Текст config.txt ровно в формате из README.md.
    std::string ToConfigFileText() const {
        std::ostringstream Stream;
        Stream << Levels.size() << '\n';
        for (const SLevelSpec& Level : Levels) {
            Stream << Level.Capacity << ' ' << Level.Algorithm << '\n';
        }
        return Stream.str();
    }
};

// ARC из README в текущей сборке не распознаётся (ParseCacheAlgorithm знает
// только эти четыре), поэтому перебираем именно их.
const std::vector<std::string>& SupportedAlgorithms() {
    static const std::vector<std::string> Algorithms = {"LRU", "LFU", "2Q", "LIRS"};
    return Algorithms;
}

// У части алгоритмов есть собственный нижний предел ёмкости (2Q делит
// ёмкость на три очереди, LIRS — на LIR/HIR-части), и при её нарушении
// симулятор падает с исключением. Такие конфигурации в перебор не идут.
std::size_t MinimumCapacityFor(const std::string& Algorithm) {
    if (Algorithm == "2Q")   { return 4; }
    if (Algorithm == "LIRS") { return 3; }
    return 1;
}

bool IsConfigSupported(const SCacheConfig& Config) {
    for (const SLevelSpec& Level : Config.Levels) {
        if (Level.Capacity < MinimumCapacityFor(Level.Algorithm)) {
            return false;
        }
    }
    return true;
}

// Декартова степень множества алгоритмов: все размещения с повторениями
// по LevelCount уровням.
std::vector<std::vector<std::string>> MakeAlgorithmCombinations(std::size_t LevelCount) {
    CHECK_EX(LevelCount != 0, std::invalid_argument,
             "MakeAlgorithmCombinations: число уровней должно быть ненулевым.");

    std::vector<std::vector<std::string>> Combinations = {{}};
    for (std::size_t Level = 0; Level < LevelCount; ++Level) {
        std::vector<std::vector<std::string>> Extended;
        Extended.reserve(Combinations.size() * SupportedAlgorithms().size());
        for (const std::vector<std::string>& Prefix : Combinations) {
            for (const std::string& Algorithm : SupportedAlgorithms()) {
                std::vector<std::string> Next = Prefix;
                Next.push_back(Algorithm);
                Extended.push_back(std::move(Next));
            }
        }
        Combinations = std::move(Extended);
    }
    return Combinations;
}

// Ёмкость каждого уровня задана жёстко: чем глубже уровень, тем он
// больше. Индекс в векторе — номер уровня, считая от нулевого.
const std::vector<std::size_t>& LevelCapacities() {
    static const std::vector<std::size_t> Capacities = {10, 20, 40};
    return Capacities;
}

constexpr std::size_t MaxLevelCount = 3;

std::vector<SCacheConfig> BuildAllCacheConfigs() {
    CHECK(LevelCapacities().size() >= MaxLevelCount,
          "Задано меньше ёмкостей уровней, чем уровней в переборе.");

    std::vector<SCacheConfig> Configs;

    for (std::size_t LevelCount = 1; LevelCount <= MaxLevelCount; ++LevelCount) {
        for (const std::vector<std::string>& Algorithms : MakeAlgorithmCombinations(LevelCount)) {
            SCacheConfig Config;
            Config.Levels.reserve(LevelCount);
            for (std::size_t Index = 0; Index < LevelCount; ++Index) {
                Config.Levels.push_back({LevelCapacities()[Index], Algorithms[Index]});
            }

            CHECK(IsConfigSupported(Config),
                  "Конфигурация '" + Config.Describe() + "' нарушает нижний предел ёмкости "
                  "одного из алгоритмов.");

            Configs.push_back(std::move(Config));
        }
    }
    return Configs;
}

// ===================== запуск симулятора =====================

struct SRunResult {
    std::string              GroupName;
    std::string              StreamName;
    std::string              StreamDescription;
    std::size_t              StreamLength = 0;
    std::size_t              UniqueKeys   = 0;

    std::string              ConfigDescription;
    std::vector<SLevelSpec>  Levels;
    std::vector<std::size_t> LevelHits;

    std::size_t              TotalHits     = 0;
    std::size_t              IdealCapacity = 0;
    std::size_t              IdealHits     = 0;

    double HitRatePercent() const      { return PercentOf(TotalHits, StreamLength); }
    double IdealHitRatePercent() const { return PercentOf(IdealHits, StreamLength); }
    double EfficiencyPercent() const   { return PercentOf(TotalHits, IdealHits); }
};

// Одна строка вывода cache_sim: "Level 1 (LRU, capacity=10): 42 hits".
struct SOutputLine {
    std::size_t LevelIndex = 0;
    std::string Algorithm;
    std::size_t Capacity = 0;
    std::size_t Hits     = 0;
};

std::vector<SOutputLine> ParseSimulatorOutput(const std::string& Output) {
    static const std::regex LinePattern(
        R"(^Level\s+(\d+)\s+\(([^,]+),\s*capacity=(\d+)\):\s*(\d+)\s+hits\s*$)");

    std::vector<SOutputLine> Lines;
    std::istringstream Stream(Output);
    std::string Text;

    while (std::getline(Stream, Text)) {
        if (Text.empty()) {
            continue;
        }
        std::smatch Match;
        CHECK(std::regex_match(Text, Match, LinePattern),
              "Не удалось разобрать строку вывода cache_sim: '" + Text + "'.");

        SOutputLine Line;
        Line.LevelIndex = std::stoull(Match[1].str());
        Line.Algorithm  = Match[2].str();
        Line.Capacity   = std::stoull(Match[3].str());
        Line.Hits       = std::stoull(Match[4].str());
        Lines.push_back(std::move(Line));
    }

    CHECK(!Lines.empty(), "cache_sim не вывел ни одной строки результата.");
    return Lines;
}

// Обёртка над запуском внешнего симулятора: прячет временные файлы,
// перезапись config.txt и разбор вывода. Исходный config.txt сохраняется
// при создании и возвращается на место в деструкторе.
class TSimulatorRunner {
public:
    TSimulatorRunner(std::string BinaryPath, std::string ConfigPath)
        : BinaryPath_(std::move(BinaryPath)), ConfigPath_(std::move(ConfigPath)) {

        std::ifstream Binary(BinaryPath_);
        CHECK(Binary.is_open(), "Не найден исполняемый файл симулятора: '" + BinaryPath_ +
                                "'. Соберите его командой make.");

        std::ifstream OriginalConfig(ConfigPath_);
        if (OriginalConfig.is_open()) {
            std::ostringstream Buffer;
            Buffer << OriginalConfig.rdbuf();
            OriginalConfigText_  = Buffer.str();
            bHadOriginalConfig_ = true;
        }

        char TemplatePath[] = "/tmp/cache_sim_testing_XXXXXX";
        const char* CreatedPath = ::mkdtemp(TemplatePath);
        CHECK(CreatedPath != nullptr, "Не удалось создать временный каталог в /tmp.");
        TempDirectory_ = CreatedPath;
    }

    TSimulatorRunner(const TSimulatorRunner&)            = delete;
    TSimulatorRunner& operator=(const TSimulatorRunner&) = delete;

    ~TSimulatorRunner() {
        // Деструктор обязан быть "тихим": ошибки уборки не должны
        // превращаться в исключение из-под стека раскрутки.
        try {
            if (bHadOriginalConfig_) {
                std::ofstream ConfigFile(ConfigPath_, std::ios::trunc);
                if (ConfigFile.is_open()) {
                    ConfigFile << OriginalConfigText_;
                }
            }
            for (const std::string& Path : TempFiles_) {
                ::unlink(Path.c_str());
            }
            if (!TempDirectory_.empty()) {
                ::rmdir(TempDirectory_.c_str());
            }
        }
        catch (...) {
        }
    }

    // Поток данных пишется на диск один раз и переиспользуется всеми
    // прогонами — это заметно быстрее, чем гонять 20000 чисел через пайп
    // на каждую из сотен конфигураций.
    std::string RegisterDataStream(const SDataStream& Stream) {
        const std::string Path = TempDirectory_ + "/" + Stream.StreamName + ".txt";

        std::ofstream DataFile(Path, std::ios::trunc);
        CHECK(DataFile.is_open(), "Не удалось создать файл данных '" + Path + "'.");
        DataFile << Stream.ToSimulatorInput();
        CHECK(DataFile.good(), "Ошибка записи файла данных '" + Path + "'.");

        TempFiles_.push_back(Path);
        return Path;
    }

    SRunResult Run(const SCacheConfig& Config, const SDataStream& Stream,
                   const std::string& DataFilePath) {
        WriteConfigFile(Config);

        const std::string Command = "'" + BinaryPath_ + "' '" + ConfigPath_ + "' < '" + DataFilePath + "'";
        const std::string Output  = ExecuteCommand(Command);

        return CollectResult(Config, Stream, ParseSimulatorOutput(Output));
    }

private:
    void WriteConfigFile(const SCacheConfig& Config) {
        std::ofstream ConfigFile(ConfigPath_, std::ios::trunc);
        CHECK(ConfigFile.is_open(), "Не удалось открыть на запись конфиг-файл '" + ConfigPath_ + "'.");
        ConfigFile << Config.ToConfigFileText();
        CHECK(ConfigFile.good(), "Ошибка записи конфиг-файла '" + ConfigPath_ + "'.");
    }

    // popen с перенаправлением stdin из файла — самый короткий путь
    // "запустить и прочитать вывод" в Linux.
    static std::string ExecuteCommand(const std::string& Command) {
        std::FILE* Pipe = ::popen(Command.c_str(), "r");
        CHECK(Pipe != nullptr, "Не удалось запустить симулятор: '" + Command + "'.");

        std::string Output;
        std::array<char, 4096> Buffer{};
        std::size_t ReadCount = 0;
        while ((ReadCount = std::fread(Buffer.data(), 1, Buffer.size(), Pipe)) > 0) {
            Output.append(Buffer.data(), ReadCount);
        }

        const int ExitStatus = ::pclose(Pipe);
        CHECK(ExitStatus == 0, "Симулятор завершился с ошибкой (код " + ToText(ExitStatus) +
                               ") на команде: " + Command + "\nВывод: " + Output);
        return Output;
    }

    // Вывод симулятора — это сначала уровни реальной иерархии, затем один
    // уровень эталонного идеального кеша суммарной ёмкости.
    static SRunResult CollectResult(const SCacheConfig& Config, const SDataStream& Stream,
                                    const std::vector<SOutputLine>& Lines) {
        CHECK(Lines.size() == Config.LevelCount() + 1,
              "Ожидалось " + ToText(Config.LevelCount() + 1) + " строк вывода, получено " +
              ToText(Lines.size()) + ".");

        SRunResult Result;
        Result.GroupName         = Stream.GroupName;
        Result.StreamName        = Stream.StreamName;
        Result.StreamDescription = Stream.Description;
        Result.StreamLength      = Stream.Length();
        Result.ConfigDescription = Config.Describe();
        Result.Levels            = Config.Levels;

        for (std::size_t Index = 0; Index < Config.LevelCount(); ++Index) {
            const SOutputLine& Line = Lines[Index];
            CHECK(Line.Capacity == Config.Levels[Index].Capacity,
                  "Симулятор сообщил ёмкость " + ToText(Line.Capacity) + " на уровне " +
                  ToText(Index + 1) + ", а в конфиге " + ToText(Config.Levels[Index].Capacity) + ".");

            Result.LevelHits.push_back(Line.Hits);
            Result.TotalHits += Line.Hits;
        }

        const SOutputLine& IdealLine = Lines.back();
        Result.IdealCapacity = IdealLine.Capacity;
        Result.IdealHits     = IdealLine.Hits;

        return Result;
    }

    std::string              BinaryPath_;
    std::string              ConfigPath_;
    std::string              OriginalConfigText_;
    bool                     bHadOriginalConfig_ = false;
    std::string              TempDirectory_;
    std::vector<std::string> TempFiles_;
};

// ===================== таблица результатов =====================

// Строка отчёта — набор уже готовых текстовых полей.
using TRow = std::vector<std::string>;

// ---------- CSV ----------

std::string EscapeCsvField(const std::string& Field) {
    const bool bNeedsQuotes = Field.find_first_of(",\";\n\r") != std::string::npos;
    if (!bNeedsQuotes) {
        return Field;
    }

    std::string Escaped = "\"";
    for (char Character : Field) {
        if (Character == '"') {
            Escaped += '"';
        }
        Escaped += Character;
    }
    Escaped += '"';
    return Escaped;
}

void WriteCsvFile(const std::string& Path, const std::vector<std::string>& Headers,
                  const std::vector<TRow>& Rows) {
    std::ofstream File(Path, std::ios::trunc);
    CHECK(File.is_open(), "Не удалось открыть на запись '" + Path + "'.");

    // BOM — чтобы Excel/LibreOffice не ломали кириллицу в описаниях.
    File << "\xEF\xBB\xBF";

    for (std::size_t Index = 0; Index < Headers.size(); ++Index) {
        File << (Index == 0 ? "" : ",") << EscapeCsvField(Headers[Index]);
    }
    File << '\n';

    for (const TRow& Row : Rows) {
        for (std::size_t Index = 0; Index < Row.size(); ++Index) {
            File << (Index == 0 ? "" : ",") << EscapeCsvField(Row[Index]);
        }
        File << '\n';
    }

    CHECK(File.good(), "Ошибка записи '" + Path + "'.");
}

// ===================== сборка отчёта =====================
// Две величины в одном столбце: "1000/8", "812/81.20".
template <typename LeftType, typename RightType>
std::string MakePair(const LeftType& Left, const RightType& Right) {
    return ToText(Left) + "/" + ToText(Right);
}

std::vector<std::string> MakeReportHeaders() {
    return {
        "Группа/Поток",
        "Конфигурация",
        "Обращений/Уникальных",
        "Попаданий/Hit rate, %",
        "Идеальных попаданий/Hit rate, %"
    };
}

TRow MakeReportRow(const SRunResult& Result) {
    return {
        MakePair(Result.GroupName, Result.StreamName),
        Result.ConfigDescription,
        MakePair(Result.StreamLength, Result.UniqueKeys),
        MakePair(Result.TotalHits, Result.HitRatePercent()),
        MakePair(Result.IdealHits, Result.IdealHitRatePercent())
    };
}

// Сводка: одна конфигурация-победитель на раздел данных плюс победитель
// в целом. Попадания суммируются по всем потокам, попавшим в раздел, то
// есть конфигурации сравниваются на одинаковом объёме обращений.
struct SConfigScore {
    std::string ConfigDescription;
    std::size_t TotalHits = 0;
    std::size_t IdealHits = 0;
    std::size_t Accesses  = 0;

    double HitRatePercent() const    { return PercentOf(TotalHits, Accesses); }
    double EfficiencyPercent() const { return PercentOf(TotalHits, IdealHits); }
};

// Лучшая конфигурация среди прогонов, прошедших отбор предикатом. Предикат
// шаблонный, поэтому один и тот же поиск работает и для отдельного раздела
// данных, и для всех прогонов сразу.
template <typename PredicateType>
SConfigScore FindBestConfiguration(const std::vector<SRunResult>& Results, PredicateType Accept) {
    std::map<std::string, SConfigScore> Aggregated;

    for (const SRunResult& Result : Results) {
        if (!Accept(Result)) {
            continue;
        }
        SConfigScore& Score = Aggregated[Result.ConfigDescription];
        Score.ConfigDescription = Result.ConfigDescription;
        Score.TotalHits += Result.TotalHits;
        Score.IdealHits += Result.IdealHits;
        Score.Accesses  += Result.StreamLength;
    }

    CHECK(!Aggregated.empty(), "Нечего сравнивать: под условие не попал ни один прогон.");

    // Строго больше — значит при равенстве побеждает конфигурация, которая
    // раньше по алфавиту, и результат не зависит от порядка прогонов.
    const SConfigScore* Best = &Aggregated.begin()->second;
    for (const auto& Entry : Aggregated) {
        if (Entry.second.TotalHits > Best->TotalHits) {
            Best = &Entry.second;
        }
    }
    return *Best;
}

// Человекочитаемое имя раздела данных.
std::string GroupTitle(const std::string& GroupName) {
    if (GroupName == "Cycle")   { return "циклы"; }
    if (GroupName == "HotCold") { return "горячие/холодные"; }
    if (GroupName == "Mixed")   { return "смешанные"; }
    return GroupName;
}

// Разделы в том порядке, в каком они встретились в прогонах.
std::vector<std::string> CollectGroupNames(const std::vector<SRunResult>& Results) {
    std::vector<std::string> GroupNames;
    for (const SRunResult& Result : Results) {
        if (std::find(GroupNames.begin(), GroupNames.end(), Result.GroupName) == GroupNames.end()) {
            GroupNames.push_back(Result.GroupName);
        }
    }
    return GroupNames;
}

void PrintBestConfiguration(const std::string& Title, const SConfigScore& Score) {
    std::cout << "  " << PadRight(Title, 20) << PadRight(Score.ConfigDescription, 32)
              << "hit rate " << std::setw(6) << ToText(Score.HitRatePercent())
              << "%   от идеального " << std::setw(6) << ToText(Score.EfficiencyPercent()) << "%\n";
}

void PrintBestConfigurations(const std::vector<SRunResult>& Results) {
    CHECK(!Results.empty(), "Нет ни одного прогона, сводку строить не из чего.");

    std::cout << "\nЛучшие конфигурации:\n";

    for (const std::string& GroupName : CollectGroupNames(Results)) {
        const SConfigScore Best = FindBestConfiguration(
            Results, [&GroupName](const SRunResult& Result) { return Result.GroupName == GroupName; });
        PrintBestConfiguration(GroupTitle(GroupName), Best);
    }

    const SConfigScore BestOverall = FindBestConfiguration(
        Results, [](const SRunResult&) { return true; });

    std::cout << '\n';
    PrintBestConfiguration("в целом", BestOverall);
}

// ===================== точка входа =====================

struct STestingOptions {
    std::string BinaryPath      = "./cache_sim";
    std::string ConfigPath      = "config.txt";
    std::string OutputDirectory = "test_results";
};

STestingOptions ParseCommandLine(int ArgumentCount, char* Arguments[]) {
    CHECK(ArgumentCount <= 4, std::string("usage: ") + Arguments[0] +
                              " [каталог_результатов] [путь_к_cache_sim] [путь_к_config.txt]");

    STestingOptions Options;
    if (ArgumentCount >= 2) { Options.OutputDirectory = Arguments[1]; }
    if (ArgumentCount >= 3) { Options.BinaryPath      = Arguments[2]; }
    if (ArgumentCount >= 4) { Options.ConfigPath      = Arguments[3]; }
    return Options;
}

int main(int ArgumentCount, char* Arguments[]) {
    try {
        const STestingOptions Options = ParseCommandLine(ArgumentCount, Arguments);
        MakeDirectory(Options.OutputDirectory);

        const std::vector<SDataStream>  DataStreams  = BuildAllDataStreams();
        const std::vector<SCacheConfig> CacheConfigs = BuildAllCacheConfigs();
        const std::size_t TotalRuns = DataStreams.size() * CacheConfigs.size();

        std::cout << "Потоков данных:   " << DataStreams.size() << '\n'
                  << "Ёмкости уровней:  " << JoinWith(LevelCapacities(), " / ") << '\n'
                  << "Конфигураций:     " << CacheConfigs.size() << '\n'
                  << "Всего прогонов:   " << TotalRuns << "\n\n";

        TSimulatorRunner Runner(Options.BinaryPath, Options.ConfigPath);

        std::vector<SRunResult> Results;
        Results.reserve(TotalRuns);

        std::size_t CompletedRuns = 0;
        for (const SDataStream& Stream : DataStreams) {
            const std::string DataFilePath = Runner.RegisterDataStream(Stream);
            const std::size_t UniqueKeys   = Stream.UniqueKeyCount();

            for (const SCacheConfig& Config : CacheConfigs) {
                SRunResult Result = Runner.Run(Config, Stream, DataFilePath);
                Result.UniqueKeys = UniqueKeys;
                Results.push_back(std::move(Result));

                if (++CompletedRuns % 100 == 0 || CompletedRuns == TotalRuns) {
                    std::cout << "\r  прогонов выполнено: " << CompletedRuns << " / " << TotalRuns
                              << std::flush;
                }
            }
        }
        std::cout << "\n";

        const std::vector<std::string> Headers = MakeReportHeaders();

        std::vector<TRow> AllRows;
        AllRows.reserve(Results.size());
        for (const SRunResult& Result : Results) {
            AllRows.push_back(MakeReportRow(Result));
        }

        const std::string CsvPath = Options.OutputDirectory + "/cache_sim_results.csv";
        WriteCsvFile(CsvPath, Headers, AllRows);

        std::cout << "\nРезультаты сохранены: " << CsvPath << '\n';

        PrintBestConfigurations(Results);
    }
    catch (const std::exception& Error) {
        std::cerr << "Ошибка: " << Error.what() << '\n';
        return 1;
    }

    return 0;
}
