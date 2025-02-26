#include "cache.h"
#include "pangloss.h"

// 初始化
void CACHE::l2c_prefetcher_initialize() 
{
	printf("Ultra pref. initializing...\n"); fflush(stdout);

	// 初始化 Delta Cache
	for (int i = 0; i < L2C_DELTA_CACHE_SETS; i++){ 
		for (int j = 0; j < L2C_DELTA_CACHE_WAYS; j++){ 
            L2C_Delta_Cache[i][j].next_delta = 1 + L2C_DELTA_CACHE_SETS / 2;
            L2C_Delta_Cache[i][j].LFU_count = 0;
		}
	}
		
	// Note: the Page Cache initialisation is less important, since we are looking for page tag hits of 10-bits
	printf("Ultra pref. initialized\n"); fflush(stdout);
}

uint32_t CACHE::l2c_prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type, uint32_t metadata_in)
{
	uint64_t page = addr >> PAGE_SIZE_OFFSET;
	int page_index = page % L2C_PAGE_CACHE_SETS;
	// 判断当前的 page 是否在 Page Cache 中 hit
	int page_way;
	for(page_way = 0; page_way < L2C_PAGE_CACHE_WAYS; page_way++) {
		if(L2C_Page_Cache[page_index][page_way].page_tag == get_l2c_page_tag(page)){
			break;
		}
	}

	int new_delta = 1 + L2C_DELTA_CACHE_SETS / 2;
	int page_offset = (addr >> LOG2_BLOCK_SIZE)  % (L2C_DELTA_CACHE_SETS / 2);
	
	// 如果 page_way!= L2C_PAGE_CACHE_WAYS 则证明 page 在 Page Cache 中 hit
    // 并且这个获取并不是由于预取的 miss 导致的
	if(page_way != L2C_PAGE_CACHE_WAYS && !((type == PREFETCH) && (cache_hit == 0))) {
		int last_delta = L2C_Page_Cache[page_index][page_way].last_delta;
		int last_offset = L2C_Page_Cache[page_index][page_way].last_offset;
		// 计算新的 delta
		new_delta = page_offset - last_offset + L2C_DELTA_CACHE_SETS / 2;
		// 更新 l2c Delta Cache
		update_l2c_delta_cache(last_delta, new_delta);
	}

	// 进行预取
	int next_delta = new_delta;
	uint64_t next_addr = addr;
    int l2c_prefetch_degree = (MSHR.SIZE - MSHR.occupancy) * 2 / 3;
    if((type == PREFETCH) && (cache_hit == 0)) {
        l2c_prefetch_degree /= 2;
    }
	for(int i = 0, prefetch_count = 0; i < l2c_prefetch_degree && prefetch_count < l2c_prefetch_degree; i++) {
		// 获取当前 next_delta 对应的最好的预取策略
		int best_delta = get_l2c_next_best_transition(next_delta);
		// 如果找不到合适的 delta 则退出
		if(best_delta == -1) {
			break;
		}
		// 预取在 delta cache 构成的 Markov 图中概率超过 1/3 的节点
		else {
			// 由于概率超过 1/3 的节点不可能超过2个
			int candidate_way[2];
			int max_LFU[2] = {0};
			// 计算在当前 set 中 LFU_count 的总和 进而计算概率
			int set_LFU_sum = 0;
			for(int j = 0; j < L2C_DELTA_CACHE_WAYS; j++) {
				int lfu_count = L2C_Delta_Cache[next_delta][j].LFU_count;
				set_LFU_sum += lfu_count;
				if(lfu_count > max_LFU[0]) {
					max_LFU[0] = lfu_count;
					candidate_way[0] = j;
				}
				else if(lfu_count > max_LFU[1]) {
					max_LFU[1] = lfu_count;
					candidate_way[1] = j;
				}
				else ;
			}
			// 接下来判断前两个候选是否满足要求
			for(int j = 0; j < 2; j++) {
				// 如果满足预取条件 则进行预取
				if(max_LFU[j] * 3 > set_LFU_sum && prefetch_count < l2c_prefetch_degree) {
					// 计算预取地址
					uint64_t pref_addr = ((next_addr >> LOG2_BLOCK_SIZE) 
							+ (L2C_Delta_Cache[next_delta][candidate_way[j]].next_delta - L2C_DELTA_CACHE_SETS / 2)) 
							<< LOG2_BLOCK_SIZE;
					uint64_t pref_page = pref_addr >> PAGE_SIZE_OFFSET;
					// 判断预取的 block 是否在当前 page 中 并且确实需要进行预取
					if((page == pref_page)) {
						prefetch_line(ip, addr, pref_addr, FILL_L2, 0);
						prefetch_count++;
					}
				}
			}
		}

		// 向前走一步
		next_delta = best_delta;
		uint64_t pref_addr = ((next_addr >> LOG2_BLOCK_SIZE)
				+ (best_delta - L2C_DELTA_CACHE_SETS / 2))
				<< LOG2_BLOCK_SIZE;
		next_addr = pref_addr;
	}

	// 如果在 Page Cache 中 miss
	// 则需要根据 NRU 逐出一项
	int evict_page_way = -1;
	if(page_way == L2C_PAGE_CACHE_WAYS) {
		// 选择 NRU_bit 为0的项
		for(int i = 0; i < L2C_PAGE_CACHE_WAYS; i++) {
			if(L2C_Page_Cache[page_index][i].NRU_bit == 0) {
				evict_page_way = i;
				break;
			}
		}
		// 如果所有都为1 则反转 NRU_bit
		if(evict_page_way == -1) {
			evict_page_way = 0;
			for(int i = 0; i < L2C_PAGE_CACHE_WAYS; i++) {
				L2C_Page_Cache[page_index][i].NRU_bit = 0;
			}
		}
	}

	// 如果 page 在 Page Cache 中 hit 则需要刷新 Page Cache 中的 last_delta
	// 如果 page 在 Page Cache 中 miss 则需要将逐出项对应的 last_delta 清空
	// 对这两种情况 都需要更新 last_offset page_tag 和 NRU_bit
	if(page_way != L2C_PAGE_CACHE_WAYS) {
		L2C_Page_Cache[page_index][page_way].last_delta = new_delta;
		L2C_Page_Cache[page_index][page_way].last_offset = page_offset;
		L2C_Page_Cache[page_index][page_way].page_tag = get_l2c_page_tag(page);
		L2C_Page_Cache[page_index][page_way].NRU_bit = 1;
	}
	else {
		L2C_Page_Cache[page_index][evict_page_way].last_delta = 0;
		L2C_Page_Cache[page_index][evict_page_way].last_offset = page_offset;
		L2C_Page_Cache[page_index][evict_page_way].page_tag = get_l2c_page_tag(page);
		L2C_Page_Cache[page_index][evict_page_way].NRU_bit = 1;
	}
    return metadata_in;
}


// 没用到
uint32_t CACHE::l2c_prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in)
{
    return metadata_in;
}


void CACHE::l2c_prefetcher_final_stats()
{

}
