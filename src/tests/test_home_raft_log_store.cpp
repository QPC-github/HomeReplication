#include "log_store/home_raft_log_store.h"
#include <string>
#include <vector>
#include <filesystem>
#include <gtest/gtest.h>
#include <iomgr/io_environment.hpp>
#include <homestore/homestore.hpp>
#include <home_replication/repl_decls.h>

using namespace home_replication;

SISL_LOGGING_INIT(HOMEREPL_LOG_MODS)

static constexpr uint32_t g_max_logsize{512};
static std::random_device g_rd{};
static std::default_random_engine g_re{g_rd()};
static std::uniform_int_distribution< uint32_t > g_randlogsize_generator{2, g_max_logsize};

static constexpr std::array< const char, 62 > alphanum{
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
    'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};

static std::string gen_random_string(size_t len, uint64_t preamble = std::numeric_limits< uint32_t >::max()) {
    std::string str;
    if (preamble != std::numeric_limits< uint64_t >::max()) {
        std::stringstream ss;
        ss << std::setw(8) << std::setfill('0') << std::hex << preamble;
        str += ss.str();
    }

    std::uniform_int_distribution< size_t > rand_char{0, alphanum.size() - 1};
    for (size_t i{0}; i < len; ++i) {
        str += alphanum[rand_char(g_re)];
    }
    str += '\0';
    return str;
}

static const std::string s_fpath_root{"/tmp/home_raft_log_store"};

static void remove_files(uint32_t ndevices) {
    for (uint32_t i{0}; i < ndevices; ++i) {
        const std::string fpath{s_fpath_root + std::to_string(i + 1)};
        if (std::filesystem::exists(fpath)) { std::filesystem::remove(fpath); }
    }
}

static void init_files(uint32_t ndevices, uint64_t dev_size) {
    remove_files(ndevices);
    for (uint32_t i{0}; i < ndevices; ++i) {
        const std::string fpath{s_fpath_root + std::to_string(i + 1)};
        std::ofstream ofs{fpath, std::ios::binary | std::ios::out | std::ios::trunc};
        std::filesystem::resize_file(fpath, dev_size);
    }
}

struct pack_result_t {
    raft_buf_ptr_t actual_data;
    std::vector< std::string > exp_data;
};

class RaftLogStoreClient {
public:
    friend class TestRaftLogStore;

    void append_read_test(uint32_t num_entries) {
        ASSERT_EQ(m_rls->next_slot(), m_next_lsn);
        ASSERT_EQ(m_rls->start_index(), m_start_lsn);

        auto max_lsn_this_iter = uint64_cast(m_next_lsn) + num_entries;
        for (uint64_t lsn = m_next_lsn; lsn <= max_lsn_this_iter; ++lsn) {
            auto le = make_log(m_cur_term, lsn);
            int64_t const store_sn = m_rls->append(le);

            ASSERT_EQ(lsn, store_sn);
            ASSERT_EQ(m_rls->next_slot(), lsn + 1);
            validate_log(m_rls->last_entry(), lsn);

            ++m_next_lsn;
        }
        m_rls->flush();
        ASSERT_EQ(m_rls->start_index(), m_start_lsn) << "Start Index not expected to be updated after insertion";
    }

    void rollback_test() {
        m_next_lsn = (m_next_lsn - m_start_lsn) / 2; // Rollback half of the current logs
        ++m_cur_term;
        auto le = make_log(m_cur_term, m_next_lsn);
        m_rls->write_at(m_next_lsn, le); // Rollback and write with next term
        m_shadow_log.erase(m_shadow_log.begin() + m_next_lsn, m_shadow_log.end());
        ++m_next_lsn;

        ASSERT_EQ(m_rls->next_slot(), m_next_lsn) << "Post rollback, next slot doesn't have expected value";
        validate_log(m_rls->last_entry(), m_next_lsn - 1);
        validate_all_logs();
    }

    void compact_test(uint32_t num_records) {
        uint64_t compact_upto = m_start_lsn + num_records - 1;

        // Reflect expected behavior from logstore, if we are compacting beyond next insertion index, then we should
        // reset the next insertion slot and we expect logstores to create holes and fill it with dummy.
        if (compact_upto >= uint64_cast(m_next_lsn)) { m_next_lsn = compact_upto + 1; }

        m_start_lsn = compact_upto + 1;
        m_rls->compact(compact_upto);
        ASSERT_EQ(m_rls->start_index(), m_start_lsn) << "Post compaction, start_index is invalid";
        validate_all_logs();
    }

    void pack_test(uint64_t from, int32_t cnt, pack_result_t& out_pack) {
        out_pack.actual_data = m_rls->pack(from, cnt);
        ASSERT_NE(out_pack.actual_data.get(), nullptr);
        out_pack.exp_data.assign(m_shadow_log.begin() + from - 1, m_shadow_log.begin() + from + cnt - 1);
    }

    pack_result_t pack_test() {
        pack_result_t p;
        pack_test(m_start_lsn, m_next_lsn - m_start_lsn, p);
        return p;
    }

    void unpack_test(const pack_result_t& p) {
        m_rls->apply_pack(m_next_lsn, *p.actual_data);
        m_shadow_log.insert(std::end(m_shadow_log), p.exp_data.begin(), p.exp_data.end());
        m_next_lsn += p.exp_data.size();
        validate_all_logs();
    }

    size_t total_records() const { return m_shadow_log.size() - m_start_lsn + 1; }

    void validate_all_logs() {
        // Do Basic read validation
        ASSERT_EQ(m_rls->next_slot(), m_next_lsn);
        ASSERT_EQ(m_rls->start_index(), m_start_lsn);

        if (m_next_lsn > m_start_lsn) { validate_log(m_rls->last_entry(), m_next_lsn - 1); }

        // Do invidivual get validation
        for (uint64_t lsn = m_start_lsn; lsn < uint64_cast(m_next_lsn); ++lsn) {
            validate_log(m_rls->entry_at(lsn), lsn);
        }

        // Do bulk get validation as well.
        auto lsn = m_start_lsn;
        auto const entries = m_rls->log_entries(m_start_lsn, m_next_lsn);
        ASSERT_EQ(entries->size(), uint64_cast(m_next_lsn - m_start_lsn));
        for (const auto& le : *entries) {
            validate_log(le, lsn++);
        }
    }

private:
    nuraft::ptr< nuraft::log_entry > make_log(uint64_t term, uint64_t lsn) {
        auto val = gen_random_string(g_randlogsize_generator(g_re), term);
        raft_buf_ptr_t buf = nuraft::buffer::alloc(val.size() + 1);
        buf->put(val);
        m_shadow_log[lsn - 1] = std::move(val);
        return nuraft::cs_new< nuraft::log_entry >(term, buf);
    }

    void validate_log(const nuraft::ptr< nuraft::log_entry >& le, int64_t lsn) {
        uint64_t expected_term;
        std::stringstream ss;
        ss << std::hex << m_shadow_log[lsn - 1].substr(0, 8);
        ss >> expected_term;
        ASSERT_EQ(le->get_term(), expected_term) << "Term mismatch at lsn=" << lsn;

        nuraft::buffer& buf = le->get_buf();
        buf.pos(0);
        auto bytes = buf.get_raw(buf.size());

        ASSERT_EQ(buf.size() - 1, m_shadow_log[lsn - 1].size()) << "Size from log and shadow mismatch for lsn=" << lsn;
        ASSERT_EQ(std::string(r_cast< const char* >(bytes), buf.size() - 1), m_shadow_log[lsn - 1])
            << "Log entry mismatch for lsn=" << lsn;
        buf.pos(0);
    }

private:
    homestore::logstore_id_t m_store_id{UINT32_MAX};
    std::unique_ptr< HomeRaftLogStore > m_rls;
    sisl::sparse_vector< std::string > m_shadow_log;
    uint64_t m_cur_term{1};
    int64_t m_next_lsn{1};
    int64_t m_start_lsn{1};
};

class TestRaftLogStore : public ::testing::Test {
public:
    void start_homestore(bool restart = false) {
        auto const ndevices = SISL_OPTIONS["num_devs"].as< uint32_t >();
        auto const dev_size = SISL_OPTIONS["dev_size_mb"].as< uint64_t >() * 1024 * 1024;
        auto nthreads = SISL_OPTIONS["num_threads"].as< uint32_t >();

        if (restart) {
            shutdown(false);
            std::this_thread::sleep_for(std::chrono::seconds{5});
        }

        std::vector< homestore::dev_info > device_info;
        if (SISL_OPTIONS.count("device_list")) {
            auto dev_names = SISL_OPTIONS["device_list"].as< std::vector< std::string > >();
            std::string dev_list_str;
            for (const auto& d : dev_names) {
                dev_list_str += d;
            }
            LOGINFO("Taking input dev_list: {}", dev_list_str);

            /* if user customized file/disk names */
            for (uint32_t i{0}; i < dev_names.size(); ++i) {
                // const std::filesystem::path fpath{m_dev_names[i]};
                device_info.emplace_back(dev_names[i], homestore::HSDevType::Data);
            }
        } else {
            /* create files */
            LOGINFO("creating {} device files with each of size {} ", ndevices, homestore::in_bytes(dev_size));
            if (!restart) { init_files(ndevices, dev_size); }
            for (uint32_t i{0}; i < ndevices; ++i) {
                const std::filesystem::path fpath{s_fpath_root + std::to_string(i + 1)};
                device_info.emplace_back(std::filesystem::canonical(fpath).string(), homestore::HSDevType::Data);
            }
        }

        LOGINFO("Starting iomgr with {} threads, spdk: {}", nthreads, false);
        ioenvironment.with_iomgr(nthreads, false);

        const uint64_t app_mem_size = ((ndevices * dev_size) * 15) / 100;
        LOGINFO("Initialize and start HomeStore with app_mem_size = {}", homestore::in_bytes(app_mem_size));

        homestore::hs_input_params params;
        params.app_mem_size = app_mem_size;
        params.data_devices = device_info;
        homestore::HomeStore::instance()
            ->with_params(params)
            .with_meta_service(5.0)
            .with_log_service(80.0, 5.0)
            .before_init_devices([this]() {
                m_leader_store.m_rls = std::make_unique< HomeRaftLogStore >(m_leader_store.m_store_id);
                m_follower_store.m_rls = std::make_unique< HomeRaftLogStore >(m_follower_store.m_store_id);
            })
            .init(true /* wait_for_init */);

        if (!restart) {
            m_leader_store.m_rls->create_store();
            m_follower_store.m_rls->create_store();
        }
        m_leader_store.m_store_id = m_leader_store.m_rls->logstore_id();
        m_follower_store.m_store_id = m_follower_store.m_rls->logstore_id();
    }

    void shutdown(bool cleanup = true) {
        homestore::HomeStore::instance()->shutdown();
        homestore::HomeStore::reset_instance();
        iomanager.stop();

        if (cleanup) { remove_files(SISL_OPTIONS["num_devs"].as< uint32_t >()); }
    }

protected:
    RaftLogStoreClient m_leader_store;
    RaftLogStoreClient m_follower_store;
};

TEST_F(TestRaftLogStore, lifecycle_test) {
    auto nrecords = SISL_OPTIONS["num_records"].as< uint32_t >();
    LOGINFO("Step 1: Start HomeStore");
    this->start_homestore();

    LOGINFO("Step 2: Append and test {} records", nrecords);
    this->m_leader_store.append_read_test(nrecords); // assuming nrecords = 1000, total_records = 1000

    LOGINFO("Step 3: Rollback half of the records");
    this->m_leader_store.rollback_test(); // total_records = 500

    LOGINFO("Step 4: Post rollback add {} records", nrecords);
    this->m_leader_store.append_read_test(nrecords); // total_records = 1500

    auto shrink_records = (this->m_leader_store.total_records() * 10) / 100;
    LOGINFO("Step 5: Compact first 10% records = {}", shrink_records);
    this->m_leader_store.compact_test(shrink_records); // total_records = 1350

    LOGINFO("Step 6: Post compaction add {} records", nrecords);
    this->m_leader_store.append_read_test(nrecords); // total_records = 2350

    shrink_records = this->m_leader_store.total_records() + (this->m_leader_store.total_records() * 10) / 100;
    LOGINFO("Step 7: Compaction 10% records={} beyond max appended entries test", shrink_records);
    this->m_leader_store.compact_test(shrink_records); // total_records = 0

    LOGINFO("Step 8: Post compaction add {} records", nrecords);
    this->m_leader_store.append_read_test(nrecords); // total_records = 1000

    LOGINFO("Step 9: Pack all records");
    auto pack_data = this->m_leader_store.pack_test(); // total_records = 1000

    LOGINFO("Step 10: Unpack all records on an empty logstore");
    this->m_follower_store.unpack_test(pack_data); // total_records in follower = 1000

    LOGINFO("Step 11: Append more {} records to follower logstore", nrecords);
    this->m_follower_store.append_read_test(nrecords); // total_records in follower = 2000

    LOGINFO("Step 12: Unpack same leader records again after append inserted records");
    this->m_follower_store.unpack_test(pack_data); // total_records in follower = 3000

    LOGINFO("Step 13: Restart homestore and validate recovery");
    this->start_homestore(true /* restart */);
    this->m_leader_store.validate_all_logs();
    this->m_follower_store.validate_all_logs();

    LOGINFO("Step 14: Post recovery do append test");
    this->m_leader_store.append_read_test(nrecords);   // total_records in leader = 2000
    this->m_follower_store.append_read_test(nrecords); // total_records in follower = 4000

    this->shutdown();
}

SISL_OPTIONS_ENABLE(logging, test_raft_log_store)
SISL_OPTION_GROUP(test_raft_log_store,
                  (num_threads, "", "num_threads", "number of threads",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
                  (num_devs, "", "num_devs", "number of devices to create",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
                  (dev_size_mb, "", "dev_size_mb", "size of each device in MB",
                   ::cxxopts::value< uint64_t >()->default_value("1024"), "number"),
                  (device_list, "", "device_list", "Device List instead of default created",
                   ::cxxopts::value< std::vector< std::string > >(), "path [...]"),
                  (num_records, "", "num_records", "number of record to test",
                   ::cxxopts::value< uint32_t >()->default_value("1000"), "number"),
                  (iterations, "", "iterations", "Iterations", ::cxxopts::value< uint32_t >()->default_value("1"),
                   "the number of iterations to run each test"));

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging, test_raft_log_store);
    sisl::logging::SetLogger("test_raft_log_store");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    return RUN_ALL_TESTS();
}
