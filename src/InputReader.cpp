#include <iostream>
#include <string>

#include "../include/InputReader.h"
#include "../MyCppLibs/sassert.h"

std::vector<int> ReadDataStream(std::istream& InputStream) {
    std::size_t ElementCount = 0;
    CHECK(InputStream >> ElementCount,
          "Не удалось прочитать количество элементов входного потока данных (stdin).");

    std::vector<int> DataStream;
    DataStream.reserve(ElementCount);

    for (std::size_t Index = 0; Index < ElementCount; ++Index) {
        int Value;
        CHECK(InputStream >> Value,
              "Во входном потоке (stdin) заявлено " + std::to_string(ElementCount) +
              " элементов, но данных оказалось меньше: не хватает элемента номер " +
              std::to_string(Index + 1) + ".");
        DataStream.push_back(Value);
    }

    return DataStream;
}