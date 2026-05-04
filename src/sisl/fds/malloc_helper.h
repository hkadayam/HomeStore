/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Author/Developer(s): Harihara Kadayam, Bryan Zimmerman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>

#ifdef __linux__
#include <malloc.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include <nlohmann/json.hpp>

#include <sisl/logging/logging.h>
#include <sisl/metrics/metrics.h>

#if defined(USING_TCMALLOC)
#include <gperftools/malloc_extension.h>
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
#include <jemalloc/jemalloc.h>
#endif

namespace sisl {
#ifdef USING_TCMALLOC
class MallocMetrics;
static void get_parse_tcmalloc_stats(nlohmann::json* const j, MallocMetrics* const metrics);
static uint64_t tcmalloc_page_size{8192};
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
class MallocMetrics;
static void get_parse_jemalloc_stats(nlohmann::json* const j, MallocMetrics* const metrics, const bool refresh);
#endif

class MallocMetrics : public MetricsGroup {
public:
    explicit MallocMetrics() : sisl::MetricsGroup{"MallocMetrics", "Singelton"} {
        REGISTER_COUNTER(num_times_exceed_soft_threshold, "Number of times mem usage exceeded soft threshold");
        REGISTER_COUNTER(num_times_exceed_aggressive_threshold,
                         "Number of times mem usage exceeded aggressive threshold");
#ifdef USING_TCMALLOC
        REGISTER_GAUGE(appln_used_bytes, "Bytes used by the application");
        REGISTER_GAUGE(page_heap_freelist_size, "Bytes in page heap freelist");

        REGISTER_GAUGE(central_cache_freelist_size, "Bytes in central cache freelist");
        REGISTER_GAUGE(transfer_cache_freelist_size, "Bytes in transfer cache freelist");
        REGISTER_GAUGE(thread_cache_freelist_size, "Bytes in thread cache freelist");
        REGISTER_GAUGE(os_released_bytes, "Bytes released to OS");

        REGISTER_HISTOGRAM(free_page_span_distribution, "Continuous pages in heap freelist(higher the better)");
        REGISTER_HISTOGRAM(unmapped_page_span_distribution, "Continuous pages returned back to system");
        REGISTER_HISTOGRAM(inuse_page_span_distribution, "Continuous pages which are being used by app");
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
        REGISTER_GAUGE(active_memory, "Bytes in active pages allocated by the application");
        REGISTER_GAUGE(allocated_memory, "Bytes allocated by the application");
        REGISTER_GAUGE(metadata_memory, "Bytes dedicated to metadata");
        REGISTER_GAUGE(metadata_thp, "Number of transparent huge pages (THP) used for metadata");
        REGISTER_GAUGE(mapped_memory, "Bytes in active extents mapped by the allocator");
        REGISTER_GAUGE(resident_memory,
                       "Maximum number of bytes in physically resident data pages mapped by the allocator");
        REGISTER_GAUGE(retained_memory,
                       "Bytes in virtual memory mappings that were retained rather than returned to OS");
        REGISTER_GAUGE(dirty_memory, "Total dirty page bytes in the arenas");
        REGISTER_GAUGE(muzzy_memory, "Total muzzy page bytes in the arenas");
#endif
        register_me_to_farm();
        attach_gather_cb(std::bind(&MallocMetrics::on_gather, this));
    }
    MallocMetrics(const MallocMetrics&) = delete;
    MallocMetrics(MallocMetrics&&) noexcept = delete;
    MallocMetrics& operator=(const MallocMetrics&) = delete;
    MallocMetrics& operator=(MallocMetrics&&) noexcept = delete;

    ~MallocMetrics() {
        deregister_me_from_farm();
    }

    void on_gather() {
#ifdef USING_TCMALLOC
        get_parse_tcmalloc_stats(nullptr, this);
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
        get_parse_jemalloc_stats(nullptr, this, true /* refresh */);
#endif
    }

    static void enable() {
        get();
    }
    static MallocMetrics& get() {
        static MallocMetrics malloc_metrics;
        return malloc_metrics;
    }
};

#ifndef USING_TCMALLOC
#if defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
class JEMallocStatics {
public:
    JEMallocStatics(const JEMallocStatics&) = delete;
    JEMallocStatics(JEMallocStatics&&) noexcept = delete;
    JEMallocStatics& operator=(const JEMallocStatics&) = delete;
    JEMallocStatics& operator=(JEMallocStatics&&) noexcept = delete;

    ~JEMallocStatics() = default;

    static JEMallocStatics& get() {
        static JEMallocStatics jemalloc_statics{};
        return jemalloc_statics;
    }

    const auto& get_arenas_narenas_mib() const { return arenas_narenas_; }
    const auto& get_arenas_pdirty_mib() const { return arenas_pdirty_; }
    const auto& get_arenas_pmuzzy_mib() const { return arenas_pmuzzy_; }
    const auto& get_epoch_mib() const { return epoch_; }
    const auto& get_stats_allocated_mib() const { return stats_allocated_; }
    const auto& get_stats_active_mib() const { return stats_active_; }
    const auto& get_stats_mapped_mib() const { return stats_mapped_; }
    const auto& get_stats_resident_mib() const { return stats_resident_; }
    const auto& get_stats_retained_mib() const { return stats_retained_; }
    const auto& get_stats_metadata_mib() const { return stats_metadata_; }
    const auto& get_stats_metadata_thp_mib() const { return stats_metadata_thp_; }
    const auto& get_arenas_dirty_decay_mib() const { return arenas_dirty_decay_; }
    const auto& get_arenas_muzzy_decay_mib() const { return arenas_muzzy_decay_; }
    const auto& get_background_thread_mib() const { return background_thread_; }
    const auto& get_arena_decay_mib() const { return arena_decay_; }
    const auto& get_arena_purge_mib() const { return arena_purge_; }
    size_t page_size() const { return page_size_; }

private:
    std::pair< std::array< size_t, 2 >, size_t > arenas_narenas_{{0, 0}, 2};
    std::pair< std::array< size_t, 4 >, size_t > arenas_pdirty_{{0, 0, 0, 0}, 4};
    std::pair< std::array< size_t, 4 >, size_t > arenas_pmuzzy_{{0, 0, 0, 0}, 4};
    std::pair< std::array< size_t, 1 >, size_t > epoch_{{0}, 1};
    std::pair< std::array< size_t, 2 >, size_t > stats_allocated_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > stats_active_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > stats_mapped_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > stats_resident_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > stats_retained_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > stats_metadata_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > stats_metadata_thp_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > arenas_dirty_decay_{{0, 0}, 2};
    std::pair< std::array< size_t, 2 >, size_t > arenas_muzzy_decay_{{0, 0}, 2};
    std::pair< std::array< size_t, 1 >, size_t > background_thread_{{0}, 1};
    std::pair< std::array< size_t, 3 >, size_t > arena_decay_{{0, 0, 0}, 3};
    std::pair< std::array< size_t, 3 >, size_t > arena_purge_{{0, 0, 0}, 3};
    size_t page_size_{4096};

    JEMallocStatics() {
        if (::mallctlnametomib("arenas.narenas", arenas_narenas_.first.data(), &arenas_narenas_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc arenas.narenas mib mib");
        }

        if (::mallctlnametomib("stats.arenas.0.pdirty", arenas_pdirty_.first.data(), &arenas_pdirty_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.arenas.0.pdirty mib");
        }

        if (::mallctlnametomib("stats.arenas.0.pmuzzy", arenas_pmuzzy_.first.data(), &arenas_pmuzzy_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.arenas.0.pmuzzy mib");
        }

        if (::mallctlnametomib("epoch", epoch_.first.data(), &epoch_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc epoch mib");
        }

        if (::mallctlnametomib("stats.allocated", stats_allocated_.first.data(), &stats_allocated_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.allocated mib");
        }

        if (::mallctlnametomib("stats.active", stats_active_.first.data(), &stats_active_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.active mib");
        }

        if (::mallctlnametomib("stats.mapped", stats_mapped_.first.data(), &stats_mapped_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.mapped mib");
        }

        if (::mallctlnametomib("stats.resident", stats_resident_.first.data(), &stats_resident_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.resident mib");
        }

        if (::mallctlnametomib("stats.retained", stats_retained_.first.data(), &stats_retained_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.retained mib");
        }

        if (::mallctlnametomib("stats.metadata", stats_metadata_.first.data(), &stats_metadata_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc stats.metadata mib");
        }

        if (::mallctlnametomib("stats.metadata_thp", stats_metadata_thp_.first.data(), &stats_metadata_thp_.second) !=
            0) {
            LOGWARN("Failed to resolve jemalloc stats.metadata_thp mib");
        }

        if (::mallctlnametomib("arenas.dirty_decay_ms", arenas_dirty_decay_.first.data(),
                               &arenas_dirty_decay_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc arenas.dirty_decay_ms mib");
        }

        if (::mallctlnametomib("arenas.muzzy_decay_ms", arenas_muzzy_decay_.first.data(),
                               &arenas_muzzy_decay_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc arenas.muzzy_decay_ms mib");
        }

        if (::mallctlnametomib("background_thread", background_thread_.first.data(), &background_thread_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc background_thread mib");
        }

        if (::mallctlnametomib("arena.0.decay", arena_decay_.first.data(), &arena_decay_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc arena decay mib");
        }

        if (::mallctlnametomib("arena.0.purge", arena_purge_.first.data(), &arena_purge_.second) != 0) {
            LOGWARN("Failed to resolve jemalloc arena purge mib");
        }

        size_t page_size_len{sizeof(page_size_)};
        if (::mallctl("arenas.page", &page_size_, &page_size_len, nullptr, 0)) {
            LOGWARN("Failed to obtain jemalloc arenas.page size");
        }
    }
};

static size_t get_jemalloc_dirty_page_count() {
    static const auto& jemalloc_statics{JEMallocStatics::get()};

    size_t npages{0};
    unsigned int num_arenas{0};
    size_t sz_num_arenas{sizeof(num_arenas)};
    static const auto& num_arenas_mib{jemalloc_statics.get_arenas_narenas_mib()};
    if (::mallctlbymib(num_arenas_mib.first.data(), num_arenas_mib.second, &num_arenas, &sz_num_arenas, nullptr, 0) ==
        0) {
        for (unsigned int i{0}; i < num_arenas; ++i) {
            static thread_local auto arenas_pdirty_mib{jemalloc_statics.get_arenas_pdirty_mib()};
            arenas_pdirty_mib.first[2] = static_cast< size_t >(i);
            size_t dirty_pages{0};
            size_t sz_dirty_pages{sizeof(dirty_pages)};
            if (::mallctlbymib(arenas_pdirty_mib.first.data(), arenas_pdirty_mib.second, &dirty_pages, &sz_dirty_pages,
                               nullptr, 0) == 0) {
                npages += dirty_pages;
            }
        }
    }

    return npages;
}

static size_t get_jemalloc_muzzy_page_count() {
    static const auto& jemalloc_statics{JEMallocStatics::get()};

    size_t npages{0};
    unsigned int num_arenas{0};
    size_t sz_num_arenas{sizeof(num_arenas)};
    static const auto& num_arenas_mib{jemalloc_statics.get_arenas_narenas_mib()};
    if (::mallctlbymib(num_arenas_mib.first.data(), num_arenas_mib.second, &num_arenas, &sz_num_arenas, nullptr, 0) ==
        0) {
        for (unsigned int i{0}; i < num_arenas; ++i) {
            static thread_local auto arenas_muzzy_mib{jemalloc_statics.get_arenas_pmuzzy_mib()};
            arenas_muzzy_mib.first[2] = static_cast< size_t >(i);
            size_t muzzy_pages{0};
            size_t sz_muzzy_pages{sizeof(muzzy_pages)};
            if (::mallctlbymib(arenas_muzzy_mib.first.data(), arenas_muzzy_mib.second, &muzzy_pages, &sz_muzzy_pages,
                               nullptr, 0) == 0) {
                npages += muzzy_pages;
            }
        }
    }

    return npages;
}
#endif
#endif

[[maybe_unused]] static size_t get_total_memory([[maybe_unused]] const bool refresh = true) {
    size_t allocated{0};

#ifndef USING_TCMALLOC
#if defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
    static const auto& jemalloc_statics{JEMallocStatics::get()};
    size_t sz_allocated{sizeof(allocated)};
    static const auto& stats_allocated_mib{jemalloc_statics.get_stats_allocated_mib()};
    if (refresh) {
        static const auto& epoch_mib{jemalloc_statics.get_epoch_mib()};
        uint64_t out_epoch{0}, in_epoch{1};
        size_t sz_epoch{sizeof(out_epoch)};
        if (::mallctlbymib(epoch_mib.first.data(), epoch_mib.second, &out_epoch, &sz_epoch, &in_epoch, sz_epoch) != 0) {
            LOGWARN("failed to refresh jemalloc epoch");
        }
    }
    if (::mallctlbymib(stats_allocated_mib.first.data(), stats_allocated_mib.second, &allocated, &sz_allocated, nullptr,
                       0) != 0) {}
#endif
#endif
    return allocated;
}

#if defined(USING_TCMALLOC)
static void update_tcmalloc_range_stats([[maybe_unused]] void* const arg, const base::MallocRange* const range) {
    auto& m{MallocMetrics::get()};
    if (range->type == base::MallocRange::Type::FREE) {
        HISTOGRAM_OBSERVE(m, free_page_span_distribution, (range->length / tcmalloc_page_size));
    } else if (range->type == base::MallocRange::Type::UNMAPPED) {
        HISTOGRAM_OBSERVE(m, unmapped_page_span_distribution, (range->length / tcmalloc_page_size));
    } else if (range->type == base::MallocRange::Type::INUSE) {
        HISTOGRAM_OBSERVE(m, inuse_page_span_distribution, (range->length * range->fraction / tcmalloc_page_size));
    }
}

static void get_parse_tcmalloc_stats(nlohmann::json* const j, MallocMetrics* const metrics) {
    const size_t buf_len{static_cast< size_t >(j ? 1024 * 20 : 9999)};
    char* stats_buf{new char[buf_len]};

    MallocExtension::instance()->GetStats(stats_buf, buf_len);
    LOGDEBUG("TCMalloc Detailed stats: {}", stats_buf);

    std::istringstream ss(stats_buf);
    std::string line;

    const std::regex re1{"MALLOC:[\\s=\\+]+(\\d+) (\\(.* MiB\\)) (.*)"};
    const std::regex re2{"MALLOC:[\\s=\\+]+(\\d+)\\s+(.*)"};
    const std::regex re3{"class\\s+\\d+\\s+\\[\\s*(\\d+) bytes \\] :\\s+(\\d+) objs;\\s+(.*) MiB;\\s+(.*) cum MiB"};
    const std::regex re4{"PageHeap:\\s+(\\d+) sizes;\\s+(.*) MiB free;\\s+(.*) MiB unmapped"};
    const std::regex re5{"\\s+(\\d+) pages \\*\\s+(\\d+) spans ~\\s+(.*) MiB;\\s+.* MiB cum; unmapped:\\s+(.*) MiB;"};
    const std::regex re6{"(>\\d+)\\s+large \\*\\s+(\\d+) spans ~\\s+(.*) MiB;\\s+.* MiB cum; unmapped:\\s+(.*) MiB;"};

    while (std::getline(ss, line, '\n')) {
        std::smatch match;
        if (std::regex_search(line, match, re1) && match.size() > 1) {
            if (j) {
                (*j)["Stats"]["Malloc"][match.str(3)] = match.str(1) + match.str(2);
            } else if (match.str(3) == "Bytes in use by application") {
                GAUGE_UPDATE(*metrics, appln_used_bytes, std::stol(match.str(1)));
            } else if (match.str(3) == "Bytes in page heap freelist") {
                GAUGE_UPDATE(*metrics, page_heap_freelist_size, std::stol(match.str(1)));
            } else if (match.str(3) == "Bytes in thread cache freelists") {
                GAUGE_UPDATE(*metrics, thread_cache_freelist_size, std::stol(match.str(1)));
            } else if (match.str(3) == "Bytes in central cache freelist") {
                GAUGE_UPDATE(*metrics, central_cache_freelist_size, std::stol(match.str(1)));
            } else if (match.str(3) == "Bytes in transfer cache freelist") {
                GAUGE_UPDATE(*metrics, transfer_cache_freelist_size, std::stol(match.str(1)));
            } else if (match.str(3) == "Bytes released to OS (aka unmapped)") {
                GAUGE_UPDATE(*metrics, os_released_bytes, std::stol(match.str(1)));
            }
        } else if (std::regex_search(line, match, re2) && match.size() > 1) {
            if (j)
                (*j)["Stats"]["Malloc"][match.str(2)] = match.str(1);
            if (match.str(2) == "Tcmalloc page size") {
                const auto sz{std::stol(match.str(1))};
                if (sz != 0) {
                    tcmalloc_page_size = sz;
                }
            }
        } else if (j) {
            if (std::regex_search(line, match, re3) && match.size() > 1) {
                nlohmann::json j1;
                j1["total_objs"] = match.str(2);
                j1["bytes (MiB)"] = match.str(3);
                (*j)["Stats"]["FreeListClasses"][match.str(1)] = j1;
            } else if (std::regex_search(line, match, re4) && match.size() > 1) {
                (*j)["Stats"]["PageHeap"]["total sizes"] = match.str(1);
                (*j)["Stats"]["PageHeap"]["free bytes (MiB)"] = match.str(2);
                (*j)["Stats"]["PageHeap"]["unmapped bytes (MiB)"] = match.str(3);
            } else if ((std::regex_search(line, match, re5) && match.size() > 1) ||
                       (std::regex_search(line, match, re6) && match.size() > 1)) {
                (*j)["Stats"]["PageHeap"]["Page span of"][match.str(1)]["count"] = match.str(2);
                (*j)["Stats"]["PageHeap"]["Page span of"][match.str(1)]["unmapped"] =
                    (std::stol(match.str(3)) * 1048576.0) / tcmalloc_page_size;
            }
        }
    }

    if (metrics) {
        MallocExtension::instance()->Ranges(nullptr, update_tcmalloc_range_stats);
    }
    delete[] stats_buf;
}
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
static void get_parse_jemalloc_stats(nlohmann::json* const j, MallocMetrics* const metrics, const bool refresh) {
    static const auto& jemalloc_statics{JEMallocStatics::get()};

    if (refresh) {
        static const auto& epoch_mib{jemalloc_statics.get_epoch_mib()};
        uint64_t out_epoch{0}, in_epoch{1};
        size_t sz_epoch{sizeof(out_epoch)};
        if (::mallctlbymib(epoch_mib.first.data(), epoch_mib.second, &out_epoch, &sz_epoch, &in_epoch, sz_epoch) != 0) {
            LOGWARN("failed to refresh jemalloc epoch");
        }
    }

    size_t allocated{0};
    size_t sz_allocated{sizeof(allocated)};
    static const auto& stats_allocated_mib{jemalloc_statics.get_stats_allocated_mib()};
    if (::mallctlbymib(stats_allocated_mib.first.data(), stats_allocated_mib.second, &allocated, &sz_allocated, nullptr,
                       0) == 0) {
        GAUGE_UPDATE(*metrics, allocated_memory, allocated);
        if (j) {
            (*j)["Stats"]["Malloc"]["Allocated"] = allocated;
        }
    }

    size_t active{0};
    size_t sz_active{sizeof(active)};
    static const auto& stats_active_mib{jemalloc_statics.get_stats_active_mib()};
    if (::mallctlbymib(stats_active_mib.first.data(), stats_active_mib.second, &active, &sz_active, nullptr, 0) == 0) {
        GAUGE_UPDATE(*metrics, active_memory, active);
        if (j) {
            (*j)["Stats"]["Malloc"]["Active"] = active;
        }
    }

    size_t mapped{0};
    size_t sz_mapped{sizeof(mapped)};
    static const auto& stats_mapped_mib{jemalloc_statics.get_stats_mapped_mib()};
    if (::mallctlbymib(stats_mapped_mib.first.data(), stats_mapped_mib.second, &mapped, &sz_mapped, nullptr, 0) == 0) {
        GAUGE_UPDATE(*metrics, mapped_memory, mapped);
        if (j) {
            (*j)["Stats"]["Malloc"]["Mapped"] = mapped;
        }
    }

    size_t resident{0};
    size_t sz_resident{sizeof(resident)};
    static const auto& stats_resident_mib{jemalloc_statics.get_stats_resident_mib()};
    if (::mallctlbymib(stats_resident_mib.first.data(), stats_resident_mib.second, &resident, &sz_resident, nullptr,
                       0) == 0) {
        GAUGE_UPDATE(*metrics, resident_memory, resident);
        if (j) {
            (*j)["Stats"]["Malloc"]["Resident"] = resident;
        }
    }

    size_t retained{0};
    size_t sz_retained{sizeof(retained)};
    static const auto& stats_retained_mib{jemalloc_statics.get_stats_retained_mib()};
    if (::mallctlbymib(stats_retained_mib.first.data(), stats_retained_mib.second, &retained, &sz_retained, nullptr,
                       0) == 0) {
        GAUGE_UPDATE(*metrics, retained_memory, retained);
        if (j) {
            (*j)["Stats"]["Malloc"]["Retained"] = retained;
        }
    }

    size_t metadata_memory{0};
    size_t sz_metadata_memory{sizeof(metadata_memory)};
    static const auto& stats_metadata_mib{jemalloc_statics.get_stats_metadata_mib()};
    if (::mallctlbymib(stats_metadata_mib.first.data(), stats_metadata_mib.second, &metadata_memory,
                       &sz_metadata_memory, nullptr, 0) == 0) {
        GAUGE_UPDATE(*metrics, metadata_memory, metadata_memory);
        if (j) {
            (*j)["Stats"]["Malloc"]["Metadata"]["Memory"] = metadata_memory;
        }
    }

    size_t metadata_thp{0};
    size_t sz_metadata_thp{sizeof(metadata_thp)};
    static const auto& stats_metadata_thp_mib{jemalloc_statics.get_stats_metadata_thp_mib()};
    if (::mallctlbymib(stats_metadata_thp_mib.first.data(), stats_metadata_thp_mib.second, &metadata_thp,
                       &sz_metadata_thp, nullptr, 0) == 0) {
        GAUGE_UPDATE(*metrics, metadata_thp, metadata_thp);
        if (j) {
            (*j)["Stats"]["Malloc"]["Metadata"]["THP"] = metadata_thp;
        }
    }

    const size_t dirty_pages{get_jemalloc_dirty_page_count()};
    GAUGE_UPDATE(*metrics, dirty_memory, dirty_pages * jemalloc_statics.page_size());
    if (j) {
        (*j)["Stats"]["Malloc"]["Arenas"]["DirtyPages"] = dirty_pages;
    }

    const size_t muzzy_pages{get_jemalloc_muzzy_page_count()};
    GAUGE_UPDATE(*metrics, muzzy_memory, muzzy_pages * jemalloc_statics.page_size());
    if (j) {
        (*j)["Stats"]["Malloc"]["Arenas"]["MuzzyPages"] = muzzy_pages;
    }
}

static void print_my_jemalloc_data(void* const opaque, const char* const buf) {
    if (opaque && buf) {
        std::string* const json_buf{static_cast< std::string* >(opaque)};
        json_buf->append(buf);
    }
}
#endif

[[maybe_unused]] static nlohmann::json get_malloc_stats_detailed() {
    nlohmann::json j;

#if defined(USING_TCMALLOC)
    j["Implementation"] = "TCMalloc (possibly)";
    get_parse_tcmalloc_stats(&j, nullptr);
    j["Stats"]["Malloc"]["MemoryReleaseRate"] = MallocExtension::instance()->GetMemoryReleaseRate();
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
    static std::mutex stats_mutex;
    std::string detailed;
    {
        std::lock_guard lock{stats_mutex};
        ::malloc_stats_print(print_my_jemalloc_data, static_cast< void* >(&detailed), "J");
    }

    j["Implementation"] = "JEMalloc";
    if (!detailed.empty()) {
        j["Stats"] = nlohmann::json::parse(detailed);
    }
#endif

    char* common_stats;
    size_t common_stats_len;
    FILE* const stream{::open_memstream(&common_stats, &common_stats_len)};
    if (stream != nullptr) {
        if (::malloc_info(0, stream) == 0) {
            std::fflush(stream);
            j["StatsMallocInfo"] = std::string{common_stats, common_stats_len};
        }
        std::fclose(stream);
        std::free(common_stats);
    }

    return j;
}

#ifndef USING_TCMALLOC
#if defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
[[maybe_unused]] static bool set_jemalloc_decay_times(const ssize_t dirty_decay_ms_in = 0,
                                                      const ssize_t muzzy_decay_ms_in = 0) {
    static const auto& jemalloc_statics{JEMallocStatics::get()};

    ssize_t dirty_decay_ms{dirty_decay_ms_in};
    size_t sz_dirty_decay{sizeof(dirty_decay_ms)};
    static const auto& arenas_dirty_decay_mib{jemalloc_statics.get_arenas_dirty_decay_mib()};
    if (::mallctlbymib(arenas_dirty_decay_mib.first.data(), arenas_dirty_decay_mib.second, nullptr, nullptr,
                       &dirty_decay_ms, sz_dirty_decay) != 0) {
        LOGWARN("failed to set jemalloc dirty page decay time in ms {}", dirty_decay_ms);
        return false;
    }

    ssize_t muzzy_decay_ms{muzzy_decay_ms_in};
    size_t sz_muzzy_decay{sizeof(muzzy_decay_ms)};
    static const auto& arenas_muzzy_decay_mib{jemalloc_statics.get_arenas_muzzy_decay_mib()};
    if (::mallctlbymib(arenas_muzzy_decay_mib.first.data(), arenas_muzzy_decay_mib.second, nullptr, nullptr,
                       &muzzy_decay_ms, sz_muzzy_decay) != 0) {
        LOGWARN("failed to set jemalloc muzzy page decay time in ms {}", muzzy_decay_ms);
        return false;
    }

    return true;
}

[[maybe_unused]] static bool set_jemalloc_background_threads(const bool enable_in) {
    static const auto& jemalloc_statics{JEMallocStatics::get()};

    bool enable{enable_in};
    size_t sz_enable{sizeof(enable)};
    static const auto& background_thread_mib{jemalloc_statics.get_background_thread_mib()};
    if (::mallctlbymib(background_thread_mib.first.data(), background_thread_mib.second, nullptr, nullptr, &enable,
                       sz_enable) != 0) {
        LOGWARN("failed to set jemalloc background threads {}", enable);
        return false;
    }
    return true;
}

#endif
#endif

[[maybe_unused]] static bool set_memory_release_rate([[maybe_unused]] const double level) {
#if defined(USING_TCMALLOC)
    MallocExtension::instance()->SetMemoryReleaseRate(level);
    return true;
#endif
    return false;
}

#if defined(USING_TCMALLOC)
namespace tcmalloc_helper {
static std::atomic< bool > s_is_aggressive_decommit{false};
} // namespace tcmalloc_helper
#endif

[[maybe_unused]] static bool set_aggressive_decommit_mem() {
#if defined(USING_TCMALLOC)
    MallocExtension::instance()->SetNumericProperty("tcmalloc.aggressive_memory_decommit", 1);
    MallocExtension::instance()->ReleaseFreeMemory();
    tcmalloc_helper::s_is_aggressive_decommit.store(true, std::memory_order_release);
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
    static thread_local auto arena_purge_mib{JEMallocStatics::get().get_arena_purge_mib()};
    arena_purge_mib.first[1] = static_cast< size_t >(MALLCTL_ARENAS_ALL);
    if (::mallctlbymib(arena_purge_mib.first.data(), arena_purge_mib.second, nullptr, nullptr, nullptr, 0) != 0) {
        LOGWARN("failed to set jemalloc arena purge");
        return false;
    }
#endif
    return true;
}

[[maybe_unused]] static bool
reset_aggressive_decommit_mem_if_needed([[maybe_unused]] const size_t mem_usage,
                                        [[maybe_unused]] const size_t aggressive_threshold) {
#if defined(USING_TCMALLOC)
    if (tcmalloc_helper::s_is_aggressive_decommit.load(std::memory_order_acquire)) {
        LOGINFO("Total memory alloced={} is restored back to less than aggressive threshold limit {}, "
                "set malloc lib to relax from aggressively decommitting",
                mem_usage, aggressive_threshold);
        MallocExtension::instance()->SetNumericProperty("tcmalloc.aggressive_memory_decommit", 0);
        tcmalloc_helper::s_is_aggressive_decommit.store(false);
        return true;
    }
#endif
    return false;
}

[[maybe_unused]] static bool soft_decommit_mem() {
#if defined(USING_TCMALLOC)
    MallocExtension::instance()->ReleaseFreeMemory();
#elif defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
    static thread_local auto arena_decay_mib{JEMallocStatics::get().get_arena_decay_mib()};
    arena_decay_mib.first[1] = static_cast< size_t >(MALLCTL_ARENAS_ALL);
    if (::mallctlbymib(arena_decay_mib.first.data(), arena_decay_mib.second, nullptr, nullptr, nullptr, 0) != 0) {
        LOGWARN("failed to set jemalloc arena decay");
        return false;
    }
#endif
    return true;
}

[[maybe_unused]] static bool release_mem_if_needed([[maybe_unused]] const size_t soft_threshold,
                                                   [[maybe_unused]] const size_t aggressive_threshold_in) {
    bool ret{false};
#if defined(USING_TCMALLOC) || defined(USING_JEMALLOC) || defined(USE_JEMALLOC)
    size_t mem_usage{0};
    const size_t aggressive_threshold{std::max(aggressive_threshold_in, soft_threshold)};

    struct rusage usage;
    if ((::getrusage(RUSAGE_SELF, &usage) == 0) &&
        ((static_cast< size_t >(usage.ru_maxrss) * 1024) <= soft_threshold)) {
        mem_usage = static_cast< size_t >(usage.ru_maxrss) * 1024;
    } else {
        if (auto usage_file{std::ifstream{"/sys/fs/cgroup/memory/memory.usage_in_bytes"}}; usage_file.is_open()) {
            usage_file >> mem_usage;
        }
    }

    if (mem_usage > aggressive_threshold) {
        LOGINFO(
            "Total memory alloced={} exceeds aggressive threshold limit={}, set malloc lib to decommit aggressively",
            mem_usage, aggressive_threshold);
        COUNTER_INCREMENT(MallocMetrics::get(), num_times_exceed_aggressive_threshold, 1);
        set_aggressive_decommit_mem();
        ret = true;
        goto done;
    }

    if (mem_usage > soft_threshold) {
        LOGINFO("Total memory alloced {} exceed soft threshold limit {}, ask tcmalloc to release memory", mem_usage,
                soft_threshold);
        COUNTER_INCREMENT(MallocMetrics::get(), num_times_exceed_soft_threshold, 1);
        soft_decommit_mem();
        ret = true;
    }

    reset_aggressive_decommit_mem_if_needed(mem_usage, aggressive_threshold);
done:
#endif
    return ret;
}
} // namespace sisl
