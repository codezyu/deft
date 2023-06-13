#pragma once

#include <atomic>
#include <queue>
#include <vector>

#include "CacheEntry.h"
#include "HugePageAlloc.h"
#include "Timer.h"
#include "WRLock.h"
#include "third_party/inlineskiplist.h"

extern bool enter_debug;

using CacheSkipList = InlineSkipList<CacheEntryComparator>;

class IndexCache {
 public:
  IndexCache(int cache_size);

  bool add_to_cache(InternalPage *page);
  bool add_sub_node(GlobalAddress addr, InternalEntry *guard, int guard_offset,
                    int group_id, int granularity, Key min, Key max);
  const CacheEntry *search_from_cache(const Key &k, GlobalAddress *addr,
                                      GlobalAddress *parent_addr,
                                      bool is_leader = false);

  void search_range_from_cache(const Key &from, const Key &to,
                               std::vector<InternalPage *> &result);

  bool add_entry(const Key &from, InternalPage *ptr);
  const CacheEntry *find_entry(const Key &k);
  // const CacheEntry *find_entry(const Key &from, const Key &to);

  bool invalidate(const CacheEntry *entry);

  const CacheEntry *get_a_random_entry(uint64_t &freq);

  void statistics();

  void bench();

 private:
  uint64_t cache_size;  // MB;
  std::atomic<int64_t> free_page_cnt;
  std::atomic<int64_t> skiplist_node_cnt;
  int64_t all_page_cnt;

  std::queue<std::pair<void *, uint64_t>> delay_free_list;
  WRLock free_lock;

  // SkipList
  CacheSkipList *skiplist;
  CacheEntryComparator cmp;
  Allocator alloc;

  void evict_one();
};

inline IndexCache::IndexCache(int cache_size) : cache_size(cache_size) {
  skiplist = new CacheSkipList(cmp, &alloc, 21);
  uint64_t memory_size = define::MB * cache_size;

  all_page_cnt = memory_size / kInternalPageSize;
  free_page_cnt.store(all_page_cnt);
  skiplist_node_cnt.store(0);
}

inline bool IndexCache::add_entry(const Key &from, InternalPage *ptr) {
  // TODO memory leak
  auto buf = skiplist->AllocateKey(sizeof(CacheEntry));
  auto &e = *(CacheEntry *)buf;
  e.from = from;
  // e.to = to - 1; // !IMPORTANT;
  e.ptr = ptr;

  return skiplist->InsertConcurrently(buf);
}

inline const CacheEntry *IndexCache::find_entry(const Key &from) {
  CacheSkipList::Iterator iter(skiplist);

  CacheEntry e;
  e.from = from;
  // e.to = to - 1;
  iter.SeekForPrev((char *)&e);
  if (iter.Valid()) {
    auto val = (const CacheEntry *)iter.key();
    return val;
  } else {
    return nullptr;
  }
}

inline bool IndexCache::add_to_cache(InternalPage *page) {
  auto new_page =
      (InternalPage *)aligned_alloc(kInternalPageSize, kInternalPageSize);
  memcpy(reinterpret_cast<void *>(new_page), page, sizeof(InternalPage));
  new_page->hdr.index_cache_freq = 0;
  assert(new_page->hdr.myself != GlobalAddress::Null());

  if (this->add_entry(page->hdr.lowest, new_page)) {
    skiplist_node_cnt.fetch_add(1);
    auto v = free_page_cnt.fetch_add(-1);
    if (v <= 0) {
      evict_one();
    }

    return true;
  } else {  // conflicted
    auto e = this->find_entry(page->hdr.lowest);
    if (e && e->from == page->hdr.lowest) {
      auto ptr = e->ptr;

      if (__sync_bool_compare_and_swap(&(e->ptr), ptr, new_page)) {
        if (ptr == nullptr) {
          auto v = free_page_cnt.fetch_add(-1);
          if (v <= 0) {
            evict_one();
          }
        } else {
          free_lock.wLock();
          delay_free_list.push(std::make_pair(ptr, asm_rdtsc()));
          free_lock.wUnlock();
        }
        return true;
      }
    }

    free(new_page);
    return false;
  }
}

inline bool IndexCache::add_sub_node(GlobalAddress addr, InternalEntry *guard,
                                     int guard_offset, int group_id,
                                     int granularity, Key min, Key max) {
  auto new_page =
      (InternalPage *)aligned_alloc(kInternalPageSize, kInternalPageSize);
  // memset(new_page, 0, kInternalPageSize);
  size_t sz;
  if (granularity == gran_quarter) {
    sz = sizeof(InternalEntry) * (kGroupCardinality + 1);
    assert((guard + kGroupCardinality)->ptr.group_gran == granularity);
  } else {
    assert(granularity == gran_half);
    sz = sizeof(InternalEntry) * (kInternalCardinality / 2 + 1);
    assert((guard + kInternalCardinality / 2)->ptr.group_gran == granularity);
  }

  auto e = this->find_entry(min);
  if (e && e->from == min) {  // update sub-node
    auto ptr = e->ptr;
    if (ptr) {
      // update
      memcpy(reinterpret_cast<char *>(new_page), ptr, kInternalPageSize);
      memcpy(reinterpret_cast<char *>(new_page) + guard_offset, guard, sz);
      if (ptr->hdr.leftmost_ptr.group_gran != granularity) {
        InternalEntry *origin_guard = reinterpret_cast<InternalEntry *>(
            reinterpret_cast<char *>(ptr) + guard_offset);
        InternalEntry *new_guard = reinterpret_cast<InternalEntry *>(
            reinterpret_cast<char *>(new_page) + guard_offset);
        new_guard->ptr.group_gran = origin_guard->ptr.group_gran;
      }
    } else {
      // add
      memset(reinterpret_cast<char *>(new_page), 0, kInternalPageSize);
      memcpy(reinterpret_cast<char *>(new_page) + guard_offset, guard, sz);
      new_page->hdr.lowest = min;
      new_page->hdr.highest = max;
      new_page->hdr.sibling_ptr = GlobalAddress::Null();
      new_page->hdr.level = 1;
      new_page->hdr.index_cache_freq = 0;
      new_page->hdr.myself = addr;
      new_page->hdr.leftmost_ptr.group_gran = granularity;
    }
    if (__sync_bool_compare_and_swap(&(e->ptr), ptr, new_page)) {
      if (ptr == nullptr) {
        auto v = free_page_cnt.fetch_add(-1);
        if (v <= 0) {
          evict_one();
        }
      } else {
        free_lock.wLock();
        delay_free_list.push(std::make_pair(ptr, asm_rdtsc()));
        free_lock.wUnlock();
      }
      return true;
    }
    free(new_page);
    return false;
  } else {
    // add
    memset(reinterpret_cast<char *>(new_page), 0, kInternalPageSize);
    memcpy(reinterpret_cast<char *>(new_page) + guard_offset, guard, sz);
    new_page->hdr.lowest = min;
    new_page->hdr.highest = max;
    new_page->hdr.sibling_ptr = GlobalAddress::Null();
    new_page->hdr.level = 1;
    new_page->hdr.index_cache_freq = 0;
    new_page->hdr.myself = addr;
    new_page->hdr.leftmost_ptr.group_gran = granularity;
    if (this->add_entry(min, new_page)) {
      skiplist_node_cnt.fetch_add(1);
      auto v = free_page_cnt.fetch_add(-1);
      if (v <= 0) {
        evict_one();
      }
      return true;
    }
    free(new_page);
    return false;
  }
}

inline const CacheEntry *IndexCache::search_from_cache(
    const Key &k, GlobalAddress *addr, GlobalAddress *parent_addr,
    bool is_leader) {
  // notice: please ensure the thread 0 can make progress
  if (is_leader &&
      !delay_free_list.empty()) {  // try to free a page in the delay-free-list
    free_lock.wLock();
    auto p = delay_free_list.front();
    if (asm_rdtsc() - p.second > 5000ul * 10) {
      free(p.first);
      delay_free_list.pop();
    }
    free_lock.wUnlock();
  }

  auto entry = find_entry(k);

  InternalPage *page = entry ? entry->ptr : nullptr;

  if (page && k >= page->hdr.lowest && k < page->hdr.highest) {
    page->hdr.index_cache_freq++;

    int group_id = get_key_group(k, page->hdr.lowest, page->hdr.highest);
    uint8_t cur_group_gran = std::max(
        page->hdr.leftmost_ptr.group_gran,
        page->records[kGroupCardinality * (group_id + 1) - 1].ptr.group_gran);
    int end, group_cnt;
    if (cur_group_gran == gran_quarter) {
      end = kGroupCardinality * (group_id + 1);
      group_cnt = kGroupCardinality;
    } else if (cur_group_gran == gran_half) {
      end = group_id < 2 ? kGroupCardinality * 2 : kInternalCardinality;
      group_cnt = kGroupCardinality * 2;
    } else {
      assert(cur_group_gran == gran_full);
      end = kInternalCardinality;
      group_cnt = kInternalCardinality;
    }
    InternalEntry *p = page->records + (end - 1);
    InternalEntry *head = page->records + (end - group_cnt - 1);
    *addr = GlobalAddress::Null();
    while (p >= head) {
      if (p->ptr == GlobalAddress::Null()) {
        break;
      } else if (k >= p->key) {
        *addr = p->ptr;
        break;
      }
      --p;
    }
    if (*addr == GlobalAddress::Null() && p > head) {
      if (k >= head->key) {
        *addr = head->ptr;
      }
    }
    // assert(*addr != GlobalAddress::Null());
    // compiler_barrier();
    if (entry->ptr &&
        *addr != GlobalAddress::Null()) {  // check if it is freed.
      *parent_addr = page->hdr.myself;
      return entry;
    }
  }

  return nullptr;
}

inline void IndexCache::search_range_from_cache(
    const Key &from, const Key &to, std::vector<InternalPage *> &result) {
  CacheSkipList::Iterator iter(skiplist);

  result.clear();
  CacheEntry e;
  e.from = from;
  iter.SeekForPrev((char *)&e);

  while (iter.Valid()) {
    auto val = (const CacheEntry *)iter.key();
    if (val->ptr) {
      if (val->from > to) {
        return;
      }
      result.push_back(val->ptr);
    }
    iter.Next();
  }
}

inline bool IndexCache::invalidate(const CacheEntry *entry) {
  auto ptr = entry->ptr;

  if (ptr == nullptr) {
    return false;
  }

  if (__sync_bool_compare_and_swap(&(entry->ptr), ptr, 0)) {
    free_lock.wLock();
    delay_free_list.push(std::make_pair(ptr, asm_rdtsc()));
    free_lock.wUnlock();
    free_page_cnt.fetch_add(1);
    return true;
  }

  return false;
}

inline const CacheEntry *IndexCache::get_a_random_entry(uint64_t &freq) {
  uint32_t seed = asm_rdtsc();
  GlobalAddress tmp_addr, tmp_parent_addr;
retry:
  auto k = rand_r(&seed) % (1000ull * define::MB);
  auto e = this->search_from_cache(k, &tmp_addr, &tmp_parent_addr);
  if (!e) {
    goto retry;
  }
  auto ptr = e->ptr;
  if (!ptr) {
    goto retry;
  }

  freq = ptr->hdr.index_cache_freq;
  if (e->ptr != ptr) {
    goto retry;
  }
  return e;
}

inline void IndexCache::evict_one() {
  uint64_t freq1, freq2;
  auto e1 = get_a_random_entry(freq1);
  auto e2 = get_a_random_entry(freq2);

  if (freq1 < freq2) {
    invalidate(e1);
  } else {
    invalidate(e2);
  }
}

inline void IndexCache::statistics() {
  printf("[skiplist node: %ld]  [page cache: %ld]\n", skiplist_node_cnt.load(),
         all_page_cnt - free_page_cnt.load());
}

inline void IndexCache::bench() {
  Timer t;
  t.begin();
  const int loop = 100000;

  for (int i = 0; i < loop; ++i) {
    uint64_t r = rand() % (5 * define::MB);
    this->find_entry(r);
  }

  t.end_print(loop);
}
