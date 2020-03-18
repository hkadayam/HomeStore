//
// Created by Kadayam, Hari on 28/10/17.
//

//
// Created by Kadayam, Hari on 24/10/17.
//

#include <iostream>
#include <sds_logging/logging.h>
#include <sds_options/options.h>
#include <benchmark/benchmark.h>
#include <boost/range/irange.hpp>
#include <boost/intrusive_ptr.hpp>
#include "cache.cpp"

SDS_LOGGING_INIT(HOMESTORE_LOG_MODS)
THREAD_BUFFER_INIT;

using namespace std;

#define TEST_COUNT 100000U
#define ITERATIONS 100U
//#define THREADS            8U
#define THREADS 4U
//#define THREADS            1U

//#define MAX_CACHE_SIZE     2 * 1024 * 1024 * 1024
#define MAX_CACHE_SIZE 2 * 1024 * 1024

struct blk_id {
    static homeds::blob get_blob(const blk_id& id) {
        homeds::blob b;
        b.bytes = (uint8_t*)&id.m_id;
        b.size = sizeof(uint64_t);

        return b;
    }

    static int compare(const blk_id& one, const blk_id& two) {
        if (one.m_id == two.m_id) {
            return 0;
        } else if (one.m_id > two.m_id) {
            return -1;
        } else {
            return 1;
        }
    }

    blk_id(uint64_t id) : m_id(id) {}
    blk_id() : blk_id(-1) {}
    blk_id(const blk_id& other) { m_id = other.m_id; }

    blk_id& operator=(const blk_id& other) {
        m_id = other.m_id;
        return *this;
    }

    std::string to_string() const {
        std::stringstream ss;
        ss << m_id;
        return ss.str();
    }
    uint64_t m_id;
};

homestore::Cache< blk_id >* glob_cache;
char** glob_bufs;
blk_id** glob_ids;

#if 0
void temp() {
    homestore::CacheBuffer< BlkId > *buf;
    homestore::intrusive_ptr_release(buf);
}
#endif

void setup(int count) {
    glob_cache = new homestore::Cache< blk_id >(MAX_CACHE_SIZE, 8192);
    glob_ids = new blk_id*[count];
    glob_bufs = new char*[count];

    for (auto i : boost::irange(0, count)) {
        glob_ids[i] = new blk_id(i);
        glob_bufs[i] = new char[64];
        sprintf(glob_bufs[i], "Content for blk id = %d\n", i);
    }
}

// template <class ...Args>
// void benchmarked_insert(benchmark::State& state, Args&&... args) {
void test_insert(benchmark::State& state) {
    // Actual test
    for (auto _ : state) { // Loops upto iteration count
        auto index = state.thread_index;
        // LOG(INFO) << "Will insert " << index << " - " << state.range(0) << " entries in this thread";
        for (auto i = index; i < state.range(0); i += state.threads) { // Loops for provided ranges
            boost::intrusive_ptr< homestore::CacheBuffer< blk_id > > cbuf;
            glob_cache->insert(*glob_ids[i], {(uint8_t*)glob_bufs[i], 64}, 0, &cbuf);
            // LOG(INFO) << "Completed insert of index i = " << i;
        }
    }
}

void test_reads(benchmark::State& state) {
    // Actual test
    for (auto _ : state) { // Loops upto iteration count
        auto index = state.thread_index;
        for (auto i = index; i < state.range(0); i += state.threads) { // Loops for provided ranges
            boost::intrusive_ptr< homestore::CacheBuffer< blk_id > > cbuf;
            bool found = glob_cache->get(*glob_ids[i], &cbuf);
#if 0
#ifndef NDEBUG
            assert(found);
            int id;
            homeds::blob b;
            cbuf->get(&b);
            sscanf((const char *)b.bytes, "Content for blk id = %d\n", &id);
            assert(id == glob_ids[i]->m_internal_id);
#endif
#endif
        }
    }
}

void test_updates(benchmark::State& state) {
    // Actual test
    for (auto _ : state) { // Loops upto iteration count
        auto index = state.thread_index;
        for (auto i = index; i < state.range(0); i += state.threads) { // Loops for provided ranges
            boost::intrusive_ptr< homestore::CacheBuffer< blk_id > > cbuf;
            glob_cache->update(*glob_ids[i], {(uint8_t*)glob_bufs[i], 64}, 16384, &cbuf);
        }
    }
}

void test_erase(benchmark::State& state) {
    // Actual test
    for (auto _ : state) { // Loops upto iteration count
        auto index = state.thread_index;
        for (auto i = index; i < state.range(0); i += state.threads) { // Loops for provided ranges
            boost::intrusive_ptr< homestore::CacheBuffer< blk_id > > cbuf;
            glob_cache->erase(*glob_ids[i], &cbuf);
        }
    }
}

BENCHMARK(test_insert)->Range(TEST_COUNT, TEST_COUNT)->Iterations(ITERATIONS)->Threads(THREADS);
BENCHMARK(test_reads)->Range(TEST_COUNT, TEST_COUNT)->Iterations(ITERATIONS)->Threads(THREADS);
BENCHMARK(test_updates)->Range(TEST_COUNT, TEST_COUNT)->Iterations(ITERATIONS)->Threads(THREADS);
BENCHMARK(test_erase)->Range(TEST_COUNT, TEST_COUNT)->Iterations(ITERATIONS)->Threads(THREADS);

SDS_OPTIONS_ENABLE(logging)

int main(int argc, char** argv) {
    SDS_OPTIONS_LOAD(argc, argv, logging)
    sds_logging::SetLogger("perf_cache");
    sds_logging::install_crash_handler();
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%n] [%t] %v");
    setup(TEST_COUNT * THREADS);
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
}
