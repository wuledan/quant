// data_initializer_test.cc — Tests for DataInitializer CSV bulk loader
#include <gtest/gtest.h>

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

#include "cpp/quant/storage/data_initializer.h"
#include "cpp/quant/storage/storage_engine.h"

namespace quant::storage {
namespace {

// Helper: create a temporary CSV file with given content
class TempCsvFile {
public:
    explicit TempCsvFile(const std::string& name, const std::string& content) {
        path_ = "/tmp/" + name + ".csv";
        std::ofstream f(path_);
        f << content;
    }
    ~TempCsvFile() { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// Helper: create a temporary directory
class TempDir {
public:
    explicit TempDir(const std::string& name) {
        path_ = "/tmp/" + name;
        ::mkdir(path_.c_str(), 0755);
    }
    ~TempDir() {
        // Remove files then directory
        DIR* d = ::opendir(path_.c_str());
        if (d) {
            struct dirent* e;
            while ((e = ::readdir(d)) != nullptr) {
                std::string n(e->d_name);
                if (n == "." || n == "..") continue;
                std::remove((path_ + "/" + n).c_str());
            }
            ::closedir(d);
        }
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }
    void write_file(const std::string& filename, const std::string& content) {
        std::ofstream f(path_ + "/" + filename);
        f << content;
    }
private:
    std::string path_;
};

const std::string kSampleCsv =
    "symbol,date,open,high,low,close,volume,amount\n"
    "600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000\n"
    "600519.SH,2020-01-03,1142.00,1150.00,1135.00,1148.00,2800000,3210000000\n"
    "600519.SH,2020-01-06,1145.00,1160.00,1140.00,1155.00,3200000,3690000000\n";

TEST(DataInitializerTest, LoadSingleCsvFile) {
    TempCsvFile tmp("test_init_single", kSampleCsv);
    StorageEngine storage;
    DataInitializer init(storage);

    ASSERT_TRUE(init.load_csv(tmp.path()));

    auto stats = init.stats();
    EXPECT_EQ(stats.files_loaded, 1);
    EXPECT_EQ(stats.rows_loaded, 3);
    EXPECT_EQ(stats.rows_failed, 0);

    // Verify data is queryable
    auto result = storage.query_kline("600519.SH", event::DataType::kKlineDay,
                                       DataField::kClose,
                                       TimeRange{0, INT64_MAX});
    EXPECT_EQ(result.values.size(), 3u);
}

TEST(DataInitializerTest, LoadCsvDirectory) {
    TempDir tmpdir("test_init_dir");

    std::string csv1 =
        "symbol,date,open,high,low,close,volume,amount\n"
        "600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000\n";

    std::string csv2 =
        "symbol,date,open,high,low,close,volume,amount\n"
        "000001.SZ,2020-01-02,16.50,16.80,16.40,16.70,50000000,835000000\n";

    tmpdir.write_file("600519_SH.csv", csv1);
    tmpdir.write_file("000001_SZ.csv", csv2);

    StorageEngine storage;
    DataInitializer init(storage);

    int loaded = init.load_csv_dir(tmpdir.path());
    EXPECT_EQ(loaded, 2);

    auto stats = init.stats();
    EXPECT_EQ(stats.files_loaded, 2);
    EXPECT_EQ(stats.rows_loaded, 2);

    // Both symbols should be queryable
    auto r1 = storage.query_kline("600519.SH", event::DataType::kKlineDay,
                                    DataField::kClose, TimeRange{0, INT64_MAX});
    EXPECT_EQ(r1.values.size(), 1u);

    auto r2 = storage.query_kline("000001.SZ", event::DataType::kKlineDay,
                                    DataField::kClose, TimeRange{0, INT64_MAX});
    EXPECT_EQ(r2.values.size(), 1u);
}

TEST(DataInitializerTest, LoadInvalidCsvSkipsBadRows) {
    std::string csv =
        "symbol,date,open,high,low,close,volume,amount\n"
        "600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000\n"
        "BADROW,not-a-date,x,y,z,w,bad,bad\n"
        "600519.SH,2020-01-03,1142.00,1150.00,1135.00,1148.00,2800000,3210000000\n";

    TempCsvFile tmp("test_init_bad", csv);
    StorageEngine storage;
    DataInitializer init(storage);

    ASSERT_TRUE(init.load_csv(tmp.path()));

    auto stats = init.stats();
    EXPECT_EQ(stats.rows_loaded, 2);
    EXPECT_EQ(stats.rows_failed, 1);
}

TEST(DataInitializerTest, LoadEmptyCsv) {
    std::string csv = "symbol,date,open,high,low,close,volume,amount\n";
    TempCsvFile tmp("test_init_empty", csv);

    StorageEngine storage;
    DataInitializer init(storage);

    ASSERT_TRUE(init.load_csv(tmp.path()));

    auto stats = init.stats();
    EXPECT_EQ(stats.rows_loaded, 0);
    EXPECT_EQ(stats.rows_failed, 0);
}

TEST(DataInitializerTest, PriceConversion) {
    std::string csv =
        "symbol,date,open,high,low,close,volume,amount\n"
        "600519.SH,2020-01-02,1130.50,1145.75,1128.25,1140.00,3500000,3980000000\n";

    TempCsvFile tmp("test_init_price", csv);
    StorageEngine storage;
    DataInitializer init(storage);

    ASSERT_TRUE(init.load_csv(tmp.path()));

    // Query close price and verify ×10000 conversion
    auto result = storage.query_kline("600519.SH", event::DataType::kKlineDay,
                                       DataField::kClose, TimeRange{0, INT64_MAX});
    ASSERT_EQ(result.values.size(), 1u);
    // Query close price: stored as int32 11400000, returned as double 1140.0
    EXPECT_DOUBLE_EQ(result.values[0], 1140.0);

    // Query open price: 1130.50 → int32 11305000 → double 1130.5
    auto open_result = storage.query_kline("600519.SH", event::DataType::kKlineDay,
                                            DataField::kOpen, TimeRange{0, INT64_MAX});
    ASSERT_EQ(open_result.values.size(), 1u);
    EXPECT_DOUBLE_EQ(open_result.values[0], 1130.5);
}

}  // namespace
}  // namespace quant::storage
